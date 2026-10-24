import { fail } from './errors.js';
import { basicSizes, MAX_SCHEMA_DEPTH, type Field, type Schema } from './schema.js';

// Cached names are bounded separately from plans; basic-vector runs reuse a plan
// across counts, with a larger names cache to avoid cycling through 9+ lengths.
export const CACHE_LIMITS = Object.freeze({masks:16, layouts:8, vectorLayouts:32, names:65536});
/** A "string" field is one JS string; every other basic type is a number. */
export type Value = number | string;
export interface FlatSnapshot { names: readonly string[]; values: Value[] }
export type MessageValue = Value | Message | MessageValue[];
export interface Message { [key: string]: MessageValue }
type Key = string | number;
const typeCodes: Readonly<Record<string, number>> = Object.assign(Object.create(null),
  Object.fromEntries(Object.keys(basicSizes).map((name,i)=>[name,i])));
// A "string" leaf: its uint32 byte count is the 4 bytes before its offset.
const TEXT=Object.keys(basicSizes).length;
// UTF-8 with U+FFFD for invalid bytes, as Python's "replace"; a leading BOM is text too.
const utf8=new TextDecoder('utf-8',{ignoreBOM:true});
// "string" is a basic type, even where the schema also defines a custom type of that name.
const isText=(f: Field): boolean=>f.type==='string';
const isCustom=(f: Field): boolean=>!basicSizes[f.type] && !isText(f);
function numberAt(view: DataView, p: number, code: number): number {
  switch(code) {
    case 0:return +(view.getUint8(p)!==0);
    case 1:case 3:return view.getUint8(p);
    case 2:return view.getInt8(p);
    case 4:return view.getInt16(p,true);
    case 5:return view.getUint16(p,true);
    case 6:return view.getInt32(p,true);
    case 7:return view.getUint32(p,true);
    case 8:return view.getInt32(p+4,true)*4294967296+view.getUint32(p,true);
    case 9:return view.getUint32(p+4,true)*4294967296+view.getUint32(p,true);
    case 10:return view.getFloat32(p,true);
    default:return view.getFloat64(p,true);
  }
}
function valueAt(view: DataView, p: number, code: number): Value {
  if(code!==TEXT) return numberAt(view,p,code);
  return utf8.decode(new Uint8Array(view.buffer,view.byteOffset+p,view.getUint32(p-4,true)));
}
type Leaf = {name: string; code: number; offset: number; keys: Key[]};
type Run = {field: Field; keys: Key[]; code: number; size: number};
type ValueRun = {code: number; size: number; offset: number; count: number};
type Layout = {
  names: readonly string[]; paths: Key[][]; offsets: number[]; codes: Uint8Array; doubles: boolean;
  counts: number[]; countOffsets: number[]; size: number; retained: number;
  runs?: ValueRun[]; rows?: ObjectRow[];
};
type Plan = {
  fields: readonly Field[]; short: boolean; layouts: Layout[]; retained: number;
  runs?: Run[]; vectors: Map<string, Layout>;
};
type State = {
  plans: Map<string, Plan>; sizes: Map<string, readonly [number, number]>;
  lastMask?: Uint8Array; lastPlan?: Plan; paths: WeakMap<Field, Key[]>;
};
const states = new WeakMap<Schema, State>();
function stateFor(schema: Schema): State {
  let state=states.get(schema);
  if(!state) {state={plans:new Map(),sizes:new Map(),paths:new WeakMap()};states.set(schema,state);}
  return state;
}
function planFor(schema: Schema, mask: Uint8Array, state: State): Plan {
  const previous=state.lastMask;
  if(previous && previous.length===mask.length) {
    let same=true;
    for(let i=0;i<mask.length;++i) if(previous[i]!==mask[i]) {same=false;break;}
    if(same) return state.lastPlan!;
  }
  const n=schema.fields.length, bytes=Math.min(mask.length,Math.ceil(n/8));
  let key='';
  for(let i=0;i<bytes;++i)
    key+=String.fromCharCode(mask[i] & (i===Math.ceil(n/8)-1 && n%8 ? (1<<(n%8))-1 : 255));
  let plan=state.plans.get(key);
  if(!plan) {
    const fields=schema.fields.slice(0,bytes*8).filter((_,i)=>mask[i>>3]&(1<<(i&7)));
    plan={fields,short:mask.length*8<n,layouts:[],retained:0,vectors:new Map()};
    // Runs cover the common basic-vector messages. Nested/custom vectors and strings use
    // the general walker; names that might collide keep its duplicate-name handling.
    if(fields.some(f=>f.count===0) && fields.every(f=>basicSizes[f.type] && !f.name.includes('['))
        && new Set(fields.map(f=>f.name)).size===fields.length)
      plan.runs=fields.map(field=>({field,keys:fieldKeys(field,state),code:typeCodes[field.type],size:basicSizes[field.type]}));
    if(state.plans.size===CACHE_LIMITS.masks) state.plans.delete(state.plans.keys().next().value!);
    state.plans.set(key,plan);
  }
  // Copy even Buffer inputs (Buffer.slice is a view); retain only schema-sized masks.
  state.lastMask=mask.length<=Math.ceil(n/8)?new Uint8Array(mask):undefined;
  state.lastPlan=plan;
  return plan;
}
function undecodable(schema: Schema, type: string): never {
  return schema.opaqueTypes[type]?fail('opaque_type',`type ${type} has an opaque encoding`):fail('unknown_type',`unknown type ${type}`);
}
function minElement(f: Field, schema: Schema, state: State, depth: number): number {
  if(basicSizes[f.type]) return basicSizes[f.type];
  if(isText(f)) return 4; // the byte count of an empty string
  // Height keeps a size learned shallow subject to the same limit when reused deeper.
  let known=state.sizes.get(f.type);
  if(!known) {
    const subs=schema.customTypes[f.type];
    if(!subs) return undecodable(schema,f.type);
    if(depth>MAX_SCHEMA_DEPTH) fail('too_deep','custom types nested too deeply');
    let size=0,height=0;
    for(const sub of subs) {
      if(sub.count===0) {size+=4;continue;}
      size+=minElement(sub,schema,state,depth+1)*(sub.count??1);
      if(isCustom(sub)) height=Math.max(height,state.sizes.get(sub.type)![1]+1);
    }
    state.sizes.set(f.type,known=[size,height]);
  } else if(depth+known[1]>MAX_SCHEMA_DEPTH) fail('too_deep','custom types nested too deeply');
  return known[0];
}
function fieldKeys(f: Field, state: State): Key[] {
  let keys=state.paths.get(f);
  if(keys) return keys;
  keys=[];
  for(const segment of f.name.split('/')) {
    let end=segment.length;
    const indices: number[]=[];
    // Parse only original field names, from the right: a[b][0] keeps a[b].
    while(segment[end-1]===']') {
      const open=segment.lastIndexOf('[',end-1), digits=segment.slice(open+1,end-1);
      if(open<0 || !digits || /[^0-9]/.test(digits)) break;
      const index=Number(digits);
      if(!Number.isSafeInteger(index) || index>=0xffffffff) break;
      indices.push(index);end=open;
    }
    keys.push(segment.slice(0,end),...indices.reverse());
  }
  state.paths.set(f,keys);
  return keys;
}
function retain(plan: Plan, layout: Layout, key?: string): Layout {
  const limit=key===undefined?CACHE_LIMITS.layouts:CACHE_LIMITS.vectorLayouts;
  if(layout.retained<=CACHE_LIMITS.names) {
    while(plan.layouts.length && (plan.layouts.length>=limit || plan.retained+layout.retained>CACHE_LIMITS.names)) {
      const old=plan.layouts.shift()!;plan.retained-=old.retained;
      for(const [k,v] of plan.vectors) if(v===old) plan.vectors.delete(k);
    }
    plan.layouts.push(layout);plan.retained+=layout.retained;
    if(key!==undefined) plan.vectors.set(key,layout);
  }
  return layout;
}
function vectorLayout(plan: Plan, view: DataView): Layout {
  let pos=0,key='';
  // Validate every run before allocating element names, paths, or output values.
  for(const run of plan.runs!) {
    let count=run.field.count??1;
    if(count===0) {
      if(pos+4>view.byteLength) fail('truncated','payload truncated');
      count=view.getUint32(pos,true);pos+=4;key+=count+',';
    }
    if(count*run.size>view.byteLength-pos) fail('truncated','payload truncated');
    pos+=count*run.size;
  }
  if(plan.short) fail('short_mask','active mask shorter than the schema');
  if(pos!==view.byteLength) fail('trailing_bytes','trailing bytes in payload');
  const known=plan.vectors.get(key);
  if(known) return known;
  const names: string[]=[],paths: Key[][]=[],runs: ValueRun[]=[];
  pos=0;
  for(const run of plan.runs!) {
    const f=run.field;let count=f.count??1;
    if(count===0) {count=view.getUint32(pos,true);pos+=4;}
    runs.push({code:run.code,size:run.size,offset:pos,count});
    for(let i=0;i<count;++i) {
      names.push(f.count===null?f.name:`${f.name}[${i}]`);
      paths.push(f.count===null?run.keys:[...run.keys,i]);
    }
    pos+=count*run.size;
  }
  return retain(plan,{names:Object.freeze(names),paths,runs,offsets:[],codes:new Uint8Array(),
    doubles:runs.every(r=>r.code===11),counts:[],countOffsets:[],size:pos,retained:names.length},key);
}
function compileLayout(schema: Schema, plan: Plan, view: DataView, state: State): Layout {
  const leaves: Leaf[]=[],counts: number[]=[],countOffsets: number[]=[];let pos=0;
  const check=(bytes: number): void=>{if(bytes>view.byteLength-pos) fail('truncated','payload truncated');};
  // A vector count or a text's byte count, which getLayout() checks to reuse the layout.
  const readCount=(): number=>{check(4);countOffsets.push(pos);const n=view.getUint32(pos,true);counts.push(n);pos+=4;return n;};
  function walk(f: Field,prefix: string,parentKeys: Key[],depth: number): void {
    if(depth>MAX_SCHEMA_DEPTH) fail('too_deep','custom types nested too deeply');
    const name=prefix?prefix+'/'+f.name:f.name,keys=[...parentKeys,...fieldKeys(f,state)];
    let count=f.count??1;
    if(f.count===0) {
      count=readCount();
      if(!count) {
        if(isCustom(f) && !schema.customTypes[f.type] && !schema.opaqueTypes[f.type]) undecodable(schema,f.type);
        return;
      }
    }
    if(f.count!==null) {const least=minElement(f,schema,state,depth);if(!least)return;check(count*least);}
    const size=basicSizes[f.type];
    if(size) for(let i=0;i<count;++i) {
      check(size);leaves.push({name:f.count===null?name:`${name}[${i}]`,code:typeCodes[f.type],offset:pos,keys:f.count===null?keys:[...keys,i]});pos+=size;
    } else if(isText(f)) for(let i=0;i<count;++i) {
      const length=readCount();check(length);leaves.push({name:f.count===null?name:`${name}[${i}]`,code:TEXT,offset:pos,keys:f.count===null?keys:[...keys,i]});pos+=length;
    } else {
      const subs=schema.customTypes[f.type];if(!subs) undecodable(schema,f.type);
      // Empty scalar DAGs can expand exponentially without consuming a byte.
      if(f.count===null) {
        try {if(minElement(f,schema,state,depth)===0)return;}
        catch { /* Preserve the first error encountered in payload order. */ }
      }
      for(let i=0;i<count;++i) for(const sub of subs)
        walk(sub,f.count===null?name:`${name}[${i}]`,f.count===null?keys:[...keys,i],depth+1);
    }
  }
  for(const f of plan.fields) walk(f,'',[],0);
  if(plan.short) fail('short_mask','active mask shorter than the schema');
  if(pos!==view.byteLength) fail('trailing_bytes',`${view.byteLength-pos} trailing bytes in payload`);
  const unique=new Map<string,Leaf>();for(const leaf of leaves)unique.set(leaf.name,leaf);
  const names: string[]=[],paths: Key[][]=[],offsets: number[]=[],codes: number[]=[];
  for(const leaf of unique.values()) {names.push(leaf.name);paths.push(leaf.keys);offsets.push(leaf.offset);codes.push(leaf.code);}
  return retain(plan,{names:Object.freeze(names),paths,offsets,codes:Uint8Array.from(codes),doubles:codes.every(c=>c===11),counts,countOffsets,size:pos,retained:leaves.length+counts.length});
}
function getLayout(schema: Schema, mask: Uint8Array, view: DataView): Layout {
  const state=stateFor(schema),plan=planFor(schema,mask,state);
  if(plan.runs) return vectorLayout(plan,view);
  for(let i=plan.layouts.length-1;i>=0;--i) {
    const layout=plan.layouts[i];if(layout.size!==view.byteLength)continue;
    let match=true;
    for(let j=0;j<layout.counts.length;++j) if(view.getUint32(layout.countOffsets[j],true)!==layout.counts[j]) {match=false;break;}
    if(match)return layout;
  }
  return compileLayout(schema,plan,view,state);
}
function viewOf(payload: Uint8Array): DataView {return new DataView(payload.buffer,payload.byteOffset,payload.byteLength);}
function readValues(layout: Layout,view: DataView): Value[] {
  const out=new Array<Value>(layout.names.length);
  if(layout.runs) {
    let index=0;
    for(const run of layout.runs) {
      const end=index+run.count;
      let p=run.offset;
      if(run.code===11) while(index<end) {out[index++]=view.getFloat64(p,true);p+=8;}
      else while(index<end) {out[index++]=numberAt(view,p,run.code);p+=run.size;}
    }
  } else if(layout.doubles) for(let i=0;i<out.length;++i)out[i]=view.getFloat64(layout.offsets[i],true);
  else for(let i=0;i<out.length;++i)out[i]=valueAt(view,layout.offsets[i],layout.codes[i]);
  return out;
}
export function decodeFlat(schema: Schema,mask: Uint8Array,payload: Uint8Array): FlatSnapshot {
  const view=viewOf(payload),layout=getLayout(schema,mask,view);return {names:layout.names,values:readValues(layout,view)};
}
type ObjectNode={array:boolean;children:Map<Key,ObjectNode|number>};
type ObjectRow={parent:number;key:Key;size:number;keys:Key[];indices:number[]};
function objectPlan(layout: Layout): ObjectRow[] {
  const root: ObjectNode={array:false,children:new Map()};
  for(let i=0;i<layout.paths.length;++i) {
    const keys=layout.paths[i];let node=root;
    for(let j=0;j<keys.length;++j) {
      const key=keys[j],old=node.children.get(key);
      if(node.array!==(typeof key==='number'))fail('name_conflict','conflicting object/array paths');
      if(j===keys.length-1) {
        if(old!==undefined && typeof old!=='number')fail('name_conflict','value is also an object');
        if(old===undefined || (layout.offsets[old as number]??old as number)<=(layout.offsets[i]??i))node.children.set(key,i);
      } else {
        const array=typeof keys[j+1]==='number';
        if(typeof old==='number' || (old && old.array!==array))fail('name_conflict','value is also an object/array');
        const child=old??{array,children:new Map()};node.children.set(key,child);node=child;
      }
    }
  }
  const rows: ObjectRow[]=[],pending=[{node:root,parent:-1,key:'' as Key}];
  for(let i=0;i<pending.length;++i) {
    const {node,parent,key}=pending[i],keys:Key[]=[],indices:number[]=[];
    let size=node.array?0:-1;
    for(const [key,child] of node.children) {
      if(node.array)size=Math.max(size,(key as number)+1);
      if(typeof child==='number') {keys.push(key);indices.push(child);} else pending.push({node:child,parent:i,key});
    }
    rows.push({parent,key,size,keys,indices});
  }
  return rows;
}
function setOwn(out: Message,key: Key,value: MessageValue): void {
  if(key==='__proto__')Object.defineProperty(out,key,{value,enumerable:true,writable:true,configurable:true});else out[key]=value;
}
function materialize(rows: ObjectRow[],view: DataView,layout: Layout): Message {
  const containers: Message[]=new Array(rows.length),values=layout.runs?readValues(layout,view):undefined;
  for(let i=0;i<rows.length;++i) {
    const row=rows[i];
    // Literal pairs avoid growing a tiny array one element at a time.
    const pair=!values && layout.doubles && row.size===2 && row.keys.length===2 && row.keys[0]===0 && row.keys[1]===1;
    const out=(pair ? [view.getFloat64(layout.offsets[row.indices[0]],true),view.getFloat64(layout.offsets[row.indices[1]],true)]
      : row.size<0 ? {} : new Array<MessageValue>(row.size)) as Message;
    containers[i]=out;if(row.parent>=0)setOwn(containers[row.parent],row.key,out);
    if(pair)continue;
    if(values) for(let j=0;j<row.keys.length;++j)
      setOwn(out,row.keys[j],values[row.indices[j]]);
    else if(layout.doubles) for(let j=0;j<row.keys.length;++j)
      setOwn(out,row.keys[j],view.getFloat64(layout.offsets[row.indices[j]],true));
    else for(let j=0;j<row.keys.length;++j) {
      const index=row.indices[j];
      setOwn(out,row.keys[j],valueAt(view,layout.offsets[index],layout.codes[index]));
    }
  }
  return containers[0];
}
export function decodeMessage(schema: Schema,mask: Uint8Array,payload: Uint8Array): Message {
  const view=viewOf(payload),layout=getLayout(schema,mask,view);
  return materialize(layout.rows??=objectPlan(layout),view,layout);
}
/** Returns zero-copy views into body. */
export function splitMcapMessage(body: Uint8Array): {activeMask:Uint8Array;payload:Uint8Array} {
  const view=viewOf(body),length=(pos:number):number=>{if(pos+4>body.length)fail('truncated','MCAP message body truncated');return view.getUint32(pos,true);};
  const maskEnd=4+length(0),start=maskEnd+4,end=start+length(maskEnd);
  if(end>body.length)fail('truncated','MCAP message body truncated');
  if(end<body.length)fail('trailing_bytes','MCAP message body has trailing bytes');
  return {activeMask:body.subarray(4,maskEnd),payload:body.subarray(start,end)};
}
/** Test-only cache counters; deliberately absent from the package exports. */
export function cacheStats(schema: Schema): Readonly<{masks:number;layouts:number;names:number}> {
  let layouts=0,names=0;const state=states.get(schema);
  for(const plan of state?.plans.values()??[]) {layouts+=plan.layouts.length;names+=plan.retained;}
  return {masks:state?.plans.size??0,layouts,names};
}
