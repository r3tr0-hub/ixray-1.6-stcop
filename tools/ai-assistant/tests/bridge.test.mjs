import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtemp, rm, readFile, writeFile, mkdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { unzipSync, strFromU8 } from 'fflate';
import { Bridge, whitelistedBuildInfo } from '../src/bridge.mjs';

class FakeClient extends EventEmitter {
  constructor() { super(); this.calls = []; this.signedIn = true; this.turn = 0; }
  async start(options) { this.options = options; return {}; }
  async request(method, params) {
    this.calls.push({ method, params });
    if (method === 'account/read') return { account: this.signedIn ? { type: 'chatgpt', email: 'test@example.invalid', planType: 'test' } : null };
    if (method === 'model/list') return { data: [{ id: 'model-id', model: 'catalog-default', displayName: 'Official catalog model', isDefault: true }], nextCursor: null };
    if (method === 'thread/start') return { thread: { id: 'thread-1' } };
    if (method === 'thread/resume') return { thread: { id: params.threadId } };
    if (method === 'thread/items/list') return { data: [{ turnId: 'old-turn', item: { type: 'agentMessage', id: 'a1', text: 'Раньше был выделен куб.' } }, { turnId: 'old-turn', item: { type: 'userMessage', id: 'u1', content: [{ type: 'text', text: 'Что было выделено?' }] } }], nextCursor: null };
    if (method === 'turn/start') { this.turn++; this.emit('notification', 'turn/started', { threadId: params.threadId, turn: { id: `turn-${this.turn}` } }); return { turn: { id: `turn-${this.turn}` } }; }
    if (method === 'turn/interrupt') return {};
    if (method === 'account/login/start') return params.type === 'chatgpt' ? { type: 'chatgpt', loginId: 'login-1', authUrl: 'https://auth.openai.com/test' } : { type: 'chatgptDeviceCode', verificationUrl: 'https://auth.openai.com/codex/device', userCode: 'ABC-TEST' };
    if (method === 'account/logout') { this.signedIn = false; return {}; }
    throw Object.assign(new Error(`Unimplemented test method ${method}`), { code: 'TEST_METHOD' });
  }
  async stop() { this.stopped = true; }
}

async function setup(t, client = new FakeClient(), { savedThread } = {}) {
  const root = await mkdtemp(join(tmpdir(), 'IXRay тест с пробелами-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const messages = [], native = [];
  const bridge = new Bridge({ sdkRoot: join(root, 'SDK с пробелами'), runtimeRoot: join(root, 'runtime'), dataRoot: root, clientFactory: () => client, openUrl: url => { messages.push({ opened: url }); }, write: message => {
    messages.push(message);
    if (message.type === 'editor.request') {
      native.push(message);
      const result = message.method === 'scene.selection' ? { ok: true, objects: [] } : { ok: true, scene_session: 'scene-1', scene_name: 'Тестовая сцена', selected_count: 1 };
      queueMicrotask(() => bridge.editor.push(JSON.stringify({ type: 'editor.response', id: message.id, result }) + '\n'));
    }
  } });
  // The host kernel denies socket listeners. Socket authentication is tested
  // separately on Windows; these tests exercise the real panel/editor channel.
  bridge.local.start = async () => {};
  bridge.local.close = async () => {};
  if (savedThread) { await mkdir(bridge.profileRoot, { recursive: true }); await writeFile(bridge.threadPath, JSON.stringify({ version: 1, threadId: savedThread })); }
  await bridge.start(); t.after(() => bridge.stop());
  return { bridge, client, root, messages, native };
}

test('UI request reaches official client API contract; picks dynamic catalog model and streams assistant events', async t => {
  const { bridge, client, messages } = await setup(t);
  await bridge.handleUi({ type: 'ui.request', id: 'ui-1', method: 'chat.send', params: { text: 'Что выделено?' } });
  const response = messages.find(item => item.type === 'ui.response' && item.id === 'ui-1');
  assert.equal(response.result.state, 'running');
  assert.equal(client.calls.find(item => item.method === 'turn/start').params.model, 'catalog-default');
  assert.equal(client.calls.find(item => item.method === 'thread/start').params.sandbox, 'read-only');
  assert.match(client.options.home, /IXRay.+AI.+codex/);
  assert.equal(client.options.mcpEnv.IXRAY_EDITOR_TOKEN, bridge.local.token);
  client.emit('notification', 'item/agentMessage/delta', { threadId: 'thread-1', itemId: 'item-1', delta: 'Выделен куб.' });
  client.emit('notification', 'turn/completed', { threadId: 'thread-1', turn: { status: 'completed' } });
  assert.equal(messages.find(item => item.event === 'agent.delta').params.delta, 'Выделен куб.');
  assert.equal(bridge.state, 'idle');
  await bridge.dispatch('chat.send', { text: 'Покажи материалы' });
  assert.equal(client.calls.filter(item => item.method === 'thread/start').length, 1);
  assert.equal(client.calls.filter(item => item.method === 'thread/resume').length, 0);
});

test('login has official managed browser/device flows; no API key or token-copy step', async t => {
  const { bridge, client, messages } = await setup(t);
  const browser = await bridge.dispatch('auth.login');
  const device = await bridge.dispatch('auth.login_device');
  assert.equal(browser.authUrl, 'https://auth.openai.com/test');
  assert.equal(device.userCode, 'ABC-TEST');
  assert.equal(messages.filter(item => item.opened).length, 2);
  assert.deepEqual(client.calls.filter(item => item.method === 'account/login/start').map(item => item.params.type), ['chatgpt', 'chatgptDeviceCode']);
  await bridge.dispatch('auth.logout');
  await assert.rejects(bridge.dispatch('chat.send', { text: 'Команда' }), error => error.code === 'LOGIN_REQUIRED');
});

test('cancel interrupts active turn, forbids parallel send, and waits for confirmation event', async t => {
  const { bridge, client } = await setup(t);
  await bridge.dispatch('chat.send', { text: 'Сдвинь объект' });
  await assert.rejects(bridge.dispatch('chat.send', { text: 'Ещё команда' }), error => error.code === 'TURN_ACTIVE');
  assert.equal((await bridge.dispatch('chat.cancel')).state, 'canceling');
  assert.equal(client.calls.find(item => item.method === 'turn/interrupt').params.turnId, 'turn-1');
  client.emit('notification', 'turn/completed', { threadId: 'thread-1', turn: { status: 'interrupted' } });
  assert.equal(bridge.state, 'interrupted'); assert.equal(bridge.local.stopped, true);
  await bridge.dispatch('chat.new'); assert.equal(bridge.threadId, null);
});

test('cancel while model catalog is pending never starts a turn', async t => {
  const client = new FakeClient(); let release;
  const original = client.request.bind(client);
  client.request = async (method, params) => method === 'model/list' ? new Promise(resolve => { release = () => resolve({ data: [{ model: 'catalog-default', isDefault: true }] }); }) : original(method, params);
  const { bridge } = await setup(t, client);
  const sending = bridge.dispatch('chat.send', { text: 'Сдвинь объект' });
  while (!release) await new Promise(resolve => setImmediate(resolve));
  await assert.rejects(bridge.dispatch('chat.cancel'), error => error.code === 'CANCEL_PENDING');
  release(); await assert.rejects(sending, error => error.code === 'CANCELED');
  assert.equal(client.calls.filter(item => item.method === 'turn/start').length, 0);
});

test('large native batch waits for one panel decision and cannot be approved by model params', async t => {
  const { bridge, messages } = await setup(t);
  const params = { op_id: 'operation-big', ids: Array.from({ length: 101 }, (_, i) => `id-${i}`), confirmed: true };
  const pending = bridge.authorizeBatch('scene.transform', params);
  const requested = messages.find(item => item.event === 'confirmation.requested');
  assert.ok(requested); assert.equal(bridge.approvals.size, 1);
  await bridge.dispatch('approval.respond', { id: requested.params.id, approved: false });
  await assert.rejects(pending, error => error.code === 'CONFIRMATION_DECLINED');
  await assert.rejects(bridge.dispatch('approval.respond', { id: requested.params.id, approved: true }), error => error.code === 'APPROVAL_EXPIRED');
});

test('aborted tool dismisses pending confirmation so a late approval cannot apply a batch', async t => {
  const { bridge, messages } = await setup(t);
  const controller = new AbortController();
  const pending = bridge.authorizeBatch('scene.transform', { op_id: 'operation-big', ids: Array.from({ length: 101 }, (_, i) => `id-${i}`) }, { signal: controller.signal });
  const id = messages.find(message => message.event === 'confirmation.requested').params.id;
  controller.abort();
  await assert.rejects(pending, error => error.code === 'CANCELED' && error.outcome === 'not_sent');
  assert.ok(messages.find(message => message.event === 'confirmation.dismissed' && message.params.id === id));
  await assert.rejects(bridge.dispatch('approval.respond', { id, approved: true }), error => error.code === 'APPROVAL_EXPIRED');
});

test('diagnostic zip is a whitelist excluding credentials, scene names, asset files, transcripts and raw error text', async t => {
  const { bridge, root } = await setup(t);
  await mkdir(bridge.codexHome, { recursive: true });
  await writeFile(join(bridge.codexHome, 'auth.json'), 'SECRET_TOKEN_CREDENTIAL');
  await writeFile(join(bridge.codexHome, 'history.json'), 'SECRET_TRANSCRIPT');
  await writeFile(join(root, 'scene.level'), 'SECRET_SCENE');
  await mkdir(bridge.sdkRoot, { recursive: true });
  await writeFile(join(bridge.sdkRoot, 'build-info.json'), JSON.stringify({ product: 'IXRay-AI-Assistant', version: '0.1.0', upstream: 'b'.repeat(40), commit: 'c'.repeat(40), configuration: 'Release', platform: 'Windows x64', node: '24.22.1', codex: '0.167.1', secret: 'SECRET_BUILD_FILE', credentials: { token: 'SECRET_CREDENTIAL' }, builtAt: 'UNREQUESTED_BUILD_TIME' }));
  bridge.account = { email: 'SECRET_EMAIL' }; bridge.editorStatus = { scene_name: 'SECRET_SCENE_NAME' };
  bridge.report(Object.assign(new Error('SECRET_RAW_ERROR'), { code: 'TEST_FAILURE' }));
  bridge.operations.push({ at: 'now', method: 'scene.transform', op_id: 'operation-safe', state: 'completed', secret: 'SECRET_TOOL_ARG' });
  const result = await bridge.dispatch('diagnostics.collect');
  const files = unzipSync(await readFile(result.path));
  assert.deepEqual(Object.keys(files).sort(), ['README.txt', 'diagnostics.json']);
  const text = strFromU8(files['diagnostics.json']);
  assert.doesNotMatch(text, /SECRET_/); assert.match(text, /TEST_FAILURE/); assert.match(text, /operation-safe/);
  const diagnostic = JSON.parse(text);
  assert.deepEqual(diagnostic.editorBuild, { product: 'IXRay-AI-Assistant', version: '0.1.0', upstream: 'b'.repeat(40), commit: 'c'.repeat(40), configuration: 'Release', platform: 'Windows x64', node: '24.22.1', codex: '0.167.1' });
  assert.doesNotMatch(text, /UNREQUESTED_BUILD_TIME/);
});

test('build provenance accepts only validated bounded scalar fields', () => {
  assert.deepEqual(whitelistedBuildInfo({ product: { token: 'secret' }, version: 'secret', commit: 'not-a-commit', upstream: 'https://example.invalid', configuration: 'secret', platform: 'unknown', node: 'SECRET_TOKEN', codex: [] }), {});
  assert.deepEqual(whitelistedBuildInfo(null), {});
  assert.deepEqual(whitelistedBuildInfo({ product: 'IXRay-AI-Assistant', commit: 'a'.repeat(40), arbitrary: 'secret' }), { product: 'IXRay-AI-Assistant', commit: 'a'.repeat(40) });
});

test('connection check reports actual editor context and current model catalog', async t => {
  const { bridge, native } = await setup(t);
  const result = await bridge.dispatch('connection.check');
  assert.equal(result.editor.scene_name, 'Тестовая сцена');
  assert.equal(native[0].method, 'scene.status');
  assert.equal(result.models[0].model, 'catalog-default');
});

test('persisted project thread restores official paginated history in chronological order without extra transcript file', async t => {
  const { bridge, client, messages } = await setup(t, new FakeClient(), { savedThread: 'thread-old' });
  await bridge.dispatch('connection.check');
  const history = messages.find(message => message.event === 'chat.history');
  assert.equal(history.params.threadId, 'thread-old');
  assert.deepEqual(history.params.messages.map(message => [message.role, message.text]), [['user', 'Что было выделено?'], ['assistant', 'Раньше был выделен куб.']]);
  assert.equal(client.calls.filter(call => call.method === 'thread/items/list').length, 1);
  await bridge.dispatch('chat.send', { text: 'Продолжай' });
  assert.equal(client.calls.filter(call => call.method === 'thread/start').length, 0);
  assert.equal(client.calls.find(call => call.method === 'thread/resume').params.threadId, 'thread-old');
  assert.deepEqual(JSON.parse(await readFile(bridge.threadPath, 'utf8')), { version: 1, threadId: 'thread-old' });
});

test('simultaneous editors from the same SDK keep distinct conversations while preserving primary project history', async t => {
  const { bridge: primary, root } = await setup(t);
  await primary.dispatch('chat.send', { text: 'Первый редактор' });
  const secondaryClient = new FakeClient();
  const original = secondaryClient.request.bind(secondaryClient);
  secondaryClient.request = async (method, params) => {
    const result = await original(method, params);
    return method === 'thread/start' ? { thread: { id: 'thread-secondary' } } : result;
  };
  const secondary = new Bridge({ sdkRoot: primary.sdkRoot, runtimeRoot: primary.runtimeRoot, dataRoot: root, clientFactory: () => secondaryClient, write: () => {} });
  secondary.local.start = async () => {}; secondary.local.close = async () => {};
  await secondary.start(); t.after(() => secondary.stop());
  assert.equal(primary.ownsProfileLock, true); assert.equal(secondary.ownsProfileLock, false);
  assert.notEqual(secondary.threadPath, primary.threadPath); assert.equal(secondary.threadId, null);
  await secondary.dispatch('chat.send', { text: 'Второй редактор' });
  assert.equal(secondary.threadId, 'thread-secondary');
  assert.equal(secondaryClient.calls.filter(call => call.method === 'thread/resume').length, 0);
  assert.equal(JSON.parse(await readFile(primary.threadPath, 'utf8')).threadId, 'thread-1');
  assert.equal(JSON.parse(await readFile(secondary.threadPath, 'utf8')).threadId, 'thread-secondary');
  await primary.stop();
  const reopened = new Bridge({ sdkRoot: primary.sdkRoot, runtimeRoot: primary.runtimeRoot, dataRoot: root, clientFactory: () => new FakeClient(), write: () => {} });
  reopened.local.start = async () => {}; reopened.local.close = async () => {};
  await reopened.start(); t.after(() => reopened.stop());
  assert.equal(reopened.ownsProfileLock, true); assert.equal(reopened.threadId, 'thread-1');
});
