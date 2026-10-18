import type { ExtensionContext } from '@foxglove/extension';
import {
  convertSchemas, convertSnapshot, convertSnapshotBatch,
} from './converters';

export function activate(context: ExtensionContext): void {
  context.registerMessageConverter({
    type: 'schema', fromSchemaName: 'data_tamer_msgs/msg/Schemas',
    toSchemaName: 'data_tamer.Schemas', converter: convertSchemas,
  });
  context.registerMessageConverter({
    type: 'schema', fromSchemaName: 'data_tamer_msgs/msg/Snapshot',
    toSchemaName: 'data_tamer.Snapshot', converter: convertSnapshot,
  });
  context.registerMessageConverter({
    type: 'schema', fromSchemaName: 'data_tamer_msgs/msg/SnapshotBatch',
    toSchemaName: 'data_tamer.SnapshotBatch', converter: convertSnapshotBatch,
  });
}
