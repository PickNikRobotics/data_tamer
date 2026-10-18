import {performance} from 'node:perf_hooks';
const implementations =
    [['current', await import(process.env.DECODER_MODULE ?? '../dist/index.js')]];
if(process.env.COMPARE_MODULE)
  implementations.unshift(['before', await import(process.env.COMPARE_MODULE)]);
const runs = Number(process.env.BENCH_RUNS ?? 7),
      duration = Number(process.env.BENCH_MS ?? 150),
      warm = Number(process.env.BENCH_WARM_MS ?? 200);
const sep = '='.repeat(59);
const cases = [
  ['6 values', 'float64[2] positions\nfloat64[2] velocities\nfloat64[2] efforts', 6],
  [
    '1000 scalars', Array.from({ length: 1000 }, (_, i) => `float64 v_${i}`).join('\n'),
    1000
  ],
  ['1000 doubles vector', 'float64[] samples', 1000, true],
  [
    '100 poses',
    `Pose[100] poses\n${sep}\nMSG: Pose\nPoint pos\nQuaternion rot\n${
        sep}\nMSG: Point\nfloat64 x\nfloat64 y\nfloat64 z\n${
        sep}\nMSG: Quaternion\nfloat64 w\nfloat64 x\nfloat64 y\nfloat64 z`,
    700
  ],
  [
    'mixed 1000',
    Array
        .from(
            { length: 1000 },
            (_, i) =>
                `${['float64', 'float32', 'int16', 'uint32', 'bool'][i % 5]} v_${i}`)
        .join('\n'),
    1000
  ],
  ['int64 1000', 'int64[500] signed\nuint64[500] unsigned', 1000],
  ['vector 16 lengths', 'float64[] samples', 1000, true, true],
];
let sink;
function sample(fn, frames, ms)
{
  let count = 0;
  const start = performance.now();
  let end = start;
  do
  {
    for(const frame of frames)
      sink = fn(frame);
    count += frames.length;
    end = performance.now();
  } while(end - start < ms);
  globalThis.__dataTamerBenchmarkSink = sink;
  return (end - start) * 1e6 / count;
}
console.log(`Node ${process.version}; ${runs} runs × ${duration} ms, warmup ${
    warm} ms; ns/snapshot (spread = max - min)`);
console.log(
    'implementation | shape | flat median | min | spread | message median | min | spread');
for(const [name, fields, count, vector, vary] of cases)
{
  const entries = implementations.map(([label, api]) => {
    const schema = api.parseSchema(`### version: 5\n### hash: 0\n${fields}\n`);
    const mask = new Uint8Array(Math.ceil(schema.fields.length / 8)).fill(255);
    return {
      label,
      fns: [p => api.decodeFlat(schema, mask, p), p => api.decodeMessage(schema, mask, p)]
    };
  });
  const frames = Array.from({ length: vary ? 1024 : 1000 }, (_, n) => {
    if(name === 'mixed 1000')
    {
      const p = Buffer.alloc(200 * (8 + 4 + 2 + 4 + 1));
      let pos = 0;
      for(let i = 0; i < 1000; ++i)
      {
        switch(i % 5)
        {
          case 0:
            p.writeDoubleLE(n * .125 + i, pos);
            pos += 8;
            break;
          case 1:
            p.writeFloatLE(n * .125 + i, pos);
            pos += 4;
            break;
          case 2:
            p.writeInt16LE(i, pos);
            pos += 2;
            break;
          case 3:
            p.writeUInt32LE(n + i, pos);
            pos += 4;
            break;
          case 4:
            p[pos++] = n % 2;
        }
      }
      return p;
    }
    const length = count + (vary ? n % 16 : 0),
          p = Buffer.alloc(length * 8 + (vector ? 4 : 0));
    if(vector)
      p.writeUInt32LE(length);
    for(let i = 0; i < length; ++i)
    {
      if(name === 'int64 1000')
        p.writeBigInt64LE(BigInt(n * 1000 + i), i * 8);
      else
        p.writeDoubleLE(n * .125 + i * .5, i * 8 + (vector ? 4 : 0));
    }
    return p;
  });
  for(const { fns } of entries)
    for(const fn of fns)
      sample(fn, frames, warm);
  const results = entries.map(() => [[], []]);
  for(let r = 0; r < runs; ++r)
    for(let i = 0; i < 2; ++i)
      for(let j = 0; j < entries.length; ++j)
        results[j][i].push(sample(entries[j].fns[i], frames, duration));
  for(let j = 0; j < entries.length; ++j)
  {
    const stats = results[j].flatMap(a => {
      a.sort((x, y) => x - y);
      return [a[Math.floor(runs / 2)], a[0], a.at(-1) - a[0]].map(n => n.toFixed(0));
    });
    console.log(`${entries[j].label} | ${name} | ${stats.join(' | ')}`);
  }
}
