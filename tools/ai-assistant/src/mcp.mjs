import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod/v4';
import { pathToFileURL } from 'node:url';
import { LocalEditorClient } from './editor-channel.mjs';

import { ToolDefinitions, isMutationTool } from './tool-definitions.mjs';
export { ToolDefinitions } from './tool-definitions.mjs';

export function createMcpServer(editor) {
  const server = new McpServer({ name: 'ixray-level-editor', version: '0.3.0' });
  for (const [method, description, shape] of ToolDefinitions) {
    const mutation = isMutationTool(method);
    server.registerTool(method.replaceAll('.', '_'), {
      title: `IX-Ray: ${method.slice(6)}`,
      description,
      inputSchema: z.object(shape).strict(),
      annotations: { readOnlyHint: !mutation, destructiveHint: method === 'scene.object_delete' || method === 'scene.level_save' || method === 'scene.git_write', idempotentHint: true, openWorldHint: false }
    }, async (params, extra) => {
      if (extra.signal.aborted) return { content: [{ type: 'text', text: 'Команда отменена до отправки в редактор.' }], isError: true };
      try {
        const result = await editor.request(method, params, { signal: extra.signal });
        if (method === 'scene.viewport' && result?.ok !== false) {
          const data = result.data ?? result.image_base64 ?? result.png_base64;
          const mime = result.mime_type ?? result.mimeType ?? 'image/png';
          if (typeof data !== 'string' || data.length > 24 * 1024 * 1024 || mime !== 'image/png') {
            throw Object.assign(new Error('Редактор вернул неподдерживаемый снимок viewport.'), { code: 'INVALID_VIEWPORT' });
          }
          const metadata = { ...result }; delete metadata.data; delete metadata.image_base64; delete metadata.png_base64;
          return { content: [{ type: 'text', text: JSON.stringify(metadata) }, { type: 'image', data, mimeType: mime }] };
        }
        return { content: [{ type: 'text', text: JSON.stringify(result) }], structuredContent: result && typeof result === 'object' ? result : { result }, isError: result?.ok === false };
      } catch (error) {
        return { content: [{ type: 'text', text: JSON.stringify({ ok: false, error: { code: error.code ?? 'EDITOR_ERROR', message: error.message, outcome: error.outcome } }) }], isError: true };
      }
    });
  }
  return server;
}

async function main() {
  const editor = await new LocalEditorClient({ path: process.env.IXRAY_EDITOR_PIPE, token: process.env.IXRAY_EDITOR_TOKEN }).start();
  const server = createMcpServer(editor);
  await server.connect(new StdioServerTransport());
  process.stdin.once('end', () => { editor.close(); server.close().catch(() => {}); });
  process.once('SIGTERM', () => { editor.close(); server.close().catch(() => {}); });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch(error => { process.stderr.write(`IX-Ray MCP: ${error.code ?? 'START_FAILED'}\n`); process.exitCode = 1; });
}
