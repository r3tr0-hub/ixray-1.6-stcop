import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { createMcpServer, ToolDefinitions } from '../src/mcp.mjs';

async function connected(t, handler) {
  const server = createMcpServer({ request: handler });
  const client = new Client({ name: 'ixray-tests', version: '1' });
  const [serverTransport, clientTransport] = InMemoryTransport.createLinkedPair();
  await Promise.all([server.connect(serverTransport), client.connect(clientTransport)]);
  t.after(async () => { await client.close(); await server.close(); });
  return client;
}

test('official MCP initializes and exposes all typed editor tools', async t => {
  const client = await connected(t, async () => ({ ok: true }));
  const { tools } = await client.listTools();
  assert.equal(tools.length, ToolDefinitions.length);
  assert.equal(tools.find(item => item.name === 'scene_transform').inputSchema.additionalProperties, false);
  assert.equal(tools.find(item => item.name === 'scene_operation_status').annotations.readOnlyHint, true);
});

test('MCP rejects invalid/missing operation identity and finite vector violations before editor access', async t => {
  let calls = 0;
  const client = await connected(t, async () => { calls++; return { ok: true }; });
  const invalid = await client.callTool({ name: 'scene_transform', arguments: { translation_m: [1, 0, 0] } });
  assert.equal(invalid.isError, true); assert.equal(calls, 0);
  const invalidVector = await client.callTool({ name: 'scene_transform', arguments: { op_id: 'operation-1', scene_session: 'test', translation_m: [1, 0] } });
  assert.equal(invalidVector.isError, true); assert.equal(calls, 0);
  const valid = await client.callTool({ name: 'scene_transform', arguments: { op_id: 'operation-1', scene_session: 'test', translation_m: [1, 0, 0] } });
  assert.equal(valid.isError, false); assert.equal(calls, 1);
});

test('viewport returns supported image content plus scene provenance', async t => {
  const client = await connected(t, async () => ({ ok: true, scene_session: 'scene-1', revision: 4, mime_type: 'image/png', data: 'iVBORw0KGgo=' }));
  const result = await client.callTool({ name: 'scene_viewport', arguments: {} });
  assert.equal(result.content[1].type, 'image'); assert.equal(result.content[1].mimeType, 'image/png');
  assert.equal(JSON.parse(result.content[0].text).revision, 4);
  assert.equal(JSON.parse(result.content[0].text).data, undefined);
});

test('native failures and unknown timeout outcomes reach model unchanged', async t => {
  const client = await connected(t, async () => { throw Object.assign(new Error('Результат неизвестен'), { code: 'EDITOR_TIMEOUT', outcome: 'unknown' }); });
  const result = await client.callTool({ name: 'scene_operation_status', arguments: { op_id: 'operation-1' } });
  assert.equal(result.isError, true); assert.equal(JSON.parse(result.content[0].text).error.outcome, 'unknown');
});
