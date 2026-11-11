import { test } from 'node:test';
import assert from 'node:assert/strict';
import { connect } from 'node:net';
import { once } from 'node:events';
import { EditorChannel, LocalEditorServer, LocalEditorClient } from '../src/editor-channel.mjs';

test('editor response framing, IDs, Cyrillic and independent requests', async () => {
  const messages = [];
  const channel = new EditorChannel({ write: item => messages.push(item) });
  const a = channel.request('scene.status', {}), b = channel.request('scene.selection', { scene_session: 'session' });
  assert.equal(messages.length, 2);
  const response = JSON.stringify({ type: 'editor.response', id: messages[1].id, result: { name: 'Сцена с пробелами' } }) + '\n';
  channel.push(response.slice(0, 17)); channel.push(response.slice(17));
  channel.push(JSON.stringify({ type: 'editor.response', id: messages[0].id, result: { ok: true } }) + '\n');
  assert.deepEqual(await b, { name: 'Сцена с пробелами' }); assert.deepEqual(await a, { ok: true });
  channel.close();
});

test('timeout outcome stays unknown, sends no automatic retry, late result does not mutate pending', async () => {
  const messages = [];
  const channel = new EditorChannel({ write: item => messages.push(item), timeoutMs: 15 });
  let late;
  channel.on('lateResponse', id => { late = id; });
  await assert.rejects(channel.request('scene.transform', { op_id: 'same-operation' }), error => error.code === 'EDITOR_TIMEOUT' && error.outcome === 'unknown');
  assert.equal(messages.filter(item => item.type === 'editor.request').length, 1);
  assert.equal(messages[1].type, 'editor.cancel');
  channel.push(JSON.stringify({ type: 'editor.response', id: messages[0].id, result: { ok: true } }) + '\n');
  assert.equal(late, messages[0].id);
  channel.close();
});

test('local pipe authenticates token and forwards exact operation ID; cancel blocks future actions but permits recovery reads', async t => {
  const requests = [];
  const channel = new EditorChannel({ write: message => {
    requests.push(message);
    queueMicrotask(() => channel.push(JSON.stringify({ type: 'editor.response', id: message.id, result: { ok: true, op_id: message.params.op_id, scene_session: 'session' } }) + '\n'));
  } });
  const server = new LocalEditorServer(channel);
  try { await server.start(); } catch (error) { if (error.code === 'EPERM' && process.platform !== 'win32') { t.skip('Execution environment disallows local socket listeners; Windows CI must run this integration.'); return; } throw error; }
  t.after(async () => { channel.close(); await server.close(); });
  const client = await new LocalEditorClient({ path: server.path, token: server.token }).start(); t.after(() => client.close());
  const params = { op_id: 'operation-1', scene_session: 'session', translation_m: [1, 0, 0] };
  assert.deepEqual(await client.request('scene.transform', params), { ok: true, op_id: 'operation-1', scene_session: 'session' });
  const sent = { ...requests[0].params }; delete sent.deadline_ms;
  assert.deepEqual(sent, params);
  assert.equal(typeof requests[0].params.deadline_ms, 'number');
  server.cancel();
  await assert.rejects(client.request('scene.duplicate', { op_id: 'operation-2' }), error => error.code === 'CANCELED' && error.outcome === 'canceled');
  assert.equal(requests.length, 1);
  await client.request('scene.operation_status', { op_id: 'operation-1' });
  assert.equal(requests.length, 2);
  server.resume(); await client.request('scene.select', { ids: [] });
  assert.equal(requests.length, 3);
  const wrong = connect(server.path); wrong.on('error', () => {}); wrong.write(JSON.stringify({ type: 'hello', token: 'wrong' }) + '\n');
  await once(wrong, 'close');
  assert.equal(requests.length, 3);
});

test('cancel during asynchronous confirmation does not forward native change', async t => {
  let approve;
  const messages = [];
  const channel = new EditorChannel({ write: item => messages.push(item) });
  const server = new LocalEditorServer(channel, { beforeRequest: () => new Promise(resolve => { approve = resolve; }) });
  try { await server.start(); } catch (error) { if (error.code === 'EPERM' && process.platform !== 'win32') { t.skip('Execution environment disallows local socket listeners; Windows CI must run this integration.'); return; } throw error; }
  t.after(async () => { channel.close(); await server.close(); });
  const client = await new LocalEditorClient({ path: server.path, token: server.token }).start(); t.after(() => client.close());
  const pending = client.request('scene.transform', { op_id: 'operation-1' });
  while (!approve) await new Promise(resolve => setImmediate(resolve));
  server.cancel(); approve();
  await assert.rejects(pending, error => error.code === 'CANCELED');
  assert.equal(messages.length, 0);
});

test('closed editor rejects every pending request', async () => {
  const channel = new EditorChannel({ write: () => {} });
  const a = channel.request('scene.status'), b = channel.request('scene.selection');
  channel.close();
  await assert.rejects(a, error => error.code === 'EDITOR_CLOSED');
  await assert.rejects(b, error => error.code === 'EDITOR_CLOSED');
});

test('transport deadline remains identical when the same operation is retried and abort sends cancellation identity', async () => {
  const messages = [];
  const channel = new EditorChannel({ write: message => messages.push(message) });
  const controller = new AbortController();
  const params = { scene_session: 'scene', op_id: 'operation-stable', translation_m: [1, 0, 0] };
  const first = channel.request('scene.transform', params, { signal: controller.signal });
  controller.abort();
  await assert.rejects(first, error => error.code === 'REQUEST_INTERRUPTED' && error.outcome === 'unknown');
  assert.deepEqual(messages[1], { type: 'editor.cancel', ids: [messages[0].id] });
  const retried = channel.request('scene.transform', params);
  assert.equal(messages[2].params.deadline_ms, messages[0].params.deadline_ms);
  assert.notEqual(messages[2].id, messages[0].id);
  channel.push(JSON.stringify({ type: 'editor.response', id: messages[2].id, result: { ok: true, replayed: true } }) + '\n');
  assert.equal((await retried).replayed, true);
  channel.close();
});
