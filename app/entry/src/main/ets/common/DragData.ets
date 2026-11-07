import { unifiedDataChannel, uniformDataStruct, uniformTypeDescriptor } from '@kit.ArkData';

// What a HarmonyOS drag carries, as the MIME types a Wayland app reads:
// files as a URI list, and text, or the files' paths, as plain text.
export const DROP_TYPES: string[] = [
  uniformTypeDescriptor.UniformDataType.FILE_URI,
  uniformTypeDescriptor.UniformDataType.PLAIN_TEXT,
  uniformTypeDescriptor.UniformDataType.TEXT,
];

const URI_LIST = 'text/uri-list';
const TEXT_TYPES = ['text/plain;charset=utf-8', 'text/plain', 'UTF8_STRING'];
const TEXT_KINDS: string[] = [
  uniformTypeDescriptor.UniformDataType.PLAIN_TEXT,
  uniformTypeDescriptor.UniformDataType.TEXT,
  uniformTypeDescriptor.UniformDataType.HYPERLINK,
];

// Known before the drop, from the drag's summary.
export function dragMimeTypes(summary: unifiedDataChannel.Summary): string[] {
  const kinds = Object.keys(summary.summary);
  const files = kinds.some((kind: string) => !TEXT_KINDS.includes(kind));
  return files ? [URI_LIST].concat(TEXT_TYPES) : TEXT_TYPES;
}

export interface DropPayload {
  mimeTypes: string[];
  payloads: string[];
}

// The dropped data; toPathUri turns a HarmonyOS URI into the file:// URI of
// the path the app's processes open.
export function dropPayload(data: unifiedDataChannel.UnifiedData, toPathUri: (uri: string) => string): DropPayload {
  const uris: string[] = [];
  const texts: string[] = [];
  for (const record of data.getRecords()) {
    const types = record.getTypes();
    if (types.includes(uniformTypeDescriptor.UniformDataType.FILE_URI)) {
      const file = record.getEntry(uniformTypeDescriptor.UniformDataType.FILE_URI) as uniformDataStruct.FileUri;
      uris.push(toPathUri(file.oriUri));
    } else if (types.includes(uniformTypeDescriptor.UniformDataType.PLAIN_TEXT)) {
      const text = record.getEntry(uniformTypeDescriptor.UniformDataType.PLAIN_TEXT) as uniformDataStruct.PlainText;
      texts.push(text.textContent);
    }
  }
  if (uris.length > 0) {
    const paths = uris.map((uri: string) => decodeURI(uri.substring('file://'.length))).join('\n');
    return {
      mimeTypes: [URI_LIST].concat(TEXT_TYPES),
      payloads: [uris.join('\r\n') + '\r\n'].concat(TEXT_TYPES.map(() => paths)),
    };
  }
  const text = texts.join('\n');
  return { mimeTypes: TEXT_TYPES, payloads: TEXT_TYPES.map(() => text) };
}
