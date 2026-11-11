import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, rm, mkdir } from 'node:fs/promises';
import { tmpdir, homedir } from 'node:os';
import path from 'node:path';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { AppServerClient, redactSecrets } from '../src/app-server.mjs';

const fixtureSource = `#!/usr/bin/env node
import { createInterface } from 'node:readline';
const mcp = process.argv.includes('--mcp');
let initialized = false;
function send(value) { process.stdout.write(JSON.stringify(value) + '\\n'); }
createInterface({input:process.stdin}).on('line', line => {
 const msg=JSON.parse(line);
 if (mcp) {
  if (!Object.hasOwn(msg,'id')) return;
  let result={};
  if(msg.method==='initialize') result={protocolVersion:msg.params.protocolVersion,capabilities:{tools:{}},serverInfo:{name:'ixray-test',version:'1'}};
  else if(msg.method==='tools/list') result={tools:[{name:'scene_status',description:'Test scene',inputSchema:{type:'object',properties:{},additionalProperties:false}}]};
  else if(msg.method==='resources/list') result={resources:[]};
  else if(msg.method==='resources/templates/list') result={resourceTemplates:[]};
  else if(msg.method==='tools/call') result={content:[{type:'text',text:'{"scene":"test"}'}]};
  send({jsonrpc:'2.0',id:msg.id,result}); return;
 }
 if(msg.method==='initialize') { initialized=true;send({id:msg.id,result:{userAgent:'ixray-test',platformFamily:'unix',platformOs:'linux'}});return; }
 if(msg.method==='initialized') return;
 if(!Object.hasOwn(msg,'method')) { send({method:'test/approval',params:msg});return; }
 if(!initialized) {send({id:msg.id,error:{code:-32600,message:'Not initialized'}});return;}
 if(msg.method==='test/timeout') return;
 if(msg.method==='test/exit') {setTimeout(()=>process.exit(17),10);return;}
 if(msg.method==='test/approval') {
   send({id:500,method:'item/commandExecution/requestApproval',params:{command:'forbidden'}});
   send({id:501,method:'item/fileChange/requestApproval',params:{}});
   send({id:502,method:'item/permissions/requestApproval',params:{}});
 }
 if(msg.method==='test/stderr') {
   process.stderr.write('Authorization: Bear');
   setTimeout(()=>process.stderr.write('er sensitive-bearer-value\\nsecret='+process.env.IXRAY_TEST_CAPABILITY+'\\n'),10);
 }
 if(msg.method==='test/error') {send({id:msg.id,error:{code:-42,message:'access_token=very-secret-token',data:{refresh_token:'value',normal:'ok'}}});return;}
 if(msg.method==='test/unicode') {
  const bytes=Buffer.from(JSON.stringify({id:msg.id,result:{text:'Привет, сцена 🟩'}})+'\\n');
  for(let i=0;i<bytes.length;i++) process.stdout.write(bytes.subarray(i,i+1));return;
 }
 send({id:msg.id,result:msg.params});
});
`;

async function context(t) {
  const root = await mkdtemp(path.join(tmpdir(), 'ixray app-server Кириллица '));
  const fixture = path.join(root, 'codex-app-server-fixture.mjs');
  await writeFile(fixture, fixtureSource, { mode: 0o700 });
  const client = new AppServerClient({
    requestTimeoutMs: 1000, startupTimeoutMs: 3000,
    // Windows cannot execute a shebang .mjs file. Exercise the same child-process
    // transport through an explicit Node executable on every platform.
    spawnImpl: (command, args, options) => command === fixture
      ? spawn(process.execPath, [fixture, ...args], options)
      : spawn(command, args, options),
  });
  t.after(async () => { await client.stop(); await rm(root, { recursive: true, force: true }); });
  const options = {
    codexPath: fixture, home: path.join(root, 'private codex'), cwd: root,
    mcpCommand: process.execPath, mcpArgs: [fixture, '--mcp'],
    mcpEnv: { IXRAY_TEST_CAPABILITY: 'fixture-only-secret-capability' },
  };
  return { root, client, options, fixture };
}

test('initializes once, handles Unicode streams and concurrent correlation', async t => {
  const { client, options } = await context(t);
  const info = await client.start(options);
  assert.equal(info.userAgent, 'ixray-test');
  assert.equal(client.ready, true);
  assert.equal((await client.request('test/unicode')).text, 'Привет, сцена 🟩');
  const results = await Promise.all([client.request('echo', { n: 1 }), client.request('echo', { n: 2 })]);
  assert.deepEqual(results, [{ n: 1 }, { n: 2 }]);
  await assert.rejects(client.start(options), { code: 'ALREADY_RUNNING' });
});

test('isolates config and never serializes MCP capabilities or API keys', async t => {
  const { client, options } = await context(t);
  await client.start(options);
  const config = await readFile(path.join(options.home, 'config.toml'), 'utf8');
  assert.match(config, /forced_login_method = "chatgpt"/);
  assert.match(config, /shell_tool = false/);
  assert.match(config, /env_vars = \["IXRAY_TEST_CAPABILITY"\]/);
  assert.ok(!config.includes(options.mcpEnv.IXRAY_TEST_CAPABILITY));
  assert.ok(!config.includes('api_key'));
  await assert.rejects(client.request('account/login/start', { type: 'apiKey', apiKey: 'unused' }), { code: 'AUTH_MODE_DISABLED' });
  await assert.rejects(client.request('command/exec', { command: ['bad'] }), { code: 'METHOD_DISABLED' });
});

test('preserves unrelated config and rejects the default Codex home', async t => {
  const { client, options } = await context(t);
  await assert.rejects(client.start({ ...options, home: path.join(homedir(), '.codex') }), { code: 'UNSAFE_CODEX_HOME' });
  await mkdir(options.home);
  const existing = 'model = "unrelated"\n';
  await writeFile(path.join(options.home, 'config.toml'), existing);
  await assert.rejects(client.start(options), { code: 'UNSAFE_CODEX_CONFIG' });
  assert.equal(await readFile(path.join(options.home, 'config.toml'), 'utf8'), existing);
});

test('timeout reports unknown result, does not retry and leaves connection usable', async t => {
  const { client, options } = await context(t);
  await client.start(options);
  await assert.rejects(client.request('test/timeout', {}, { timeoutMs: 30 }), { code: 'RPC_TIMEOUT', outcome: 'unknown' });
  assert.equal(client.pending.size, 0);
  assert.deepEqual(await client.request('echo', { alive: true }), { alive: true });
});

test('rejects every pending request when process exits', async t => {
  const { client, options } = await context(t);
  await client.start(options);
  const results = await Promise.allSettled([client.request('test/timeout'), client.request('test/exit')]);
  assert.ok(results.every(result => result.status === 'rejected' && result.reason.code === 'PROCESS_EXITED'));
  assert.equal(client.pending.size, 0);
  assert.equal(client.ready, false);
});

test('declines command, file and permission server approvals', async t => {
  const { client, options } = await context(t);
  await client.start(options);
  const replies = [];
  client.on('notification', (method, params) => { if (method === 'test/approval') replies.push(params); });
  const completed = new Promise(resolve => {
    client.on('notification', () => { if (replies.length === 3) resolve(); });
  });
  await client.request('test/approval');
  await completed;
  assert.deepEqual(replies.map(x => x.result), [{ decision: 'decline' }, { decision: 'decline' }, { permissions: {}, scope: 'turn' }]);
});

test('redacts errors and stderr even when secrets cross stream chunks', async t => {
  const { client, options } = await context(t);
  const lines = [];
  client.on('stderr', line => lines.push(line));
  await client.start(options);
  const stderr = once(client, 'stderr');
  await client.request('test/stderr');
  await stderr;
  await client.stop();
  const output = lines.join('');
  assert.ok(!output.includes('sensitive-bearer-value'));
  assert.ok(!output.includes(options.mcpEnv.IXRAY_TEST_CAPABILITY));
  assert.match(output, /\[REDACTED\]/);
  assert.equal(redactSecrets('https://auth.openai.com/login?code=private'), 'https://auth.openai.com/login?[REDACTED]');
});

test('redacts RPC error metadata while preserving machine-readable codes', async t => {
  const { client, options } = await context(t);
  await client.start(options);
  await assert.rejects(client.request('test/error'), error => {
    assert.equal(error.code, -42);
    assert.ok(!error.message.includes('very-secret-token'));
    assert.equal(error.data.refresh_token, '[REDACTED]');
    assert.equal(error.data.normal, 'ok');
    return true;
  });
});

test('official app-server starts in fresh home without reading account credentials', { skip: !process.env.IXRAY_CODEX_SMOKE_PATH, timeout: 60000 }, async t => {
  const { client, options } = await context(t);
  const stderr = [];
  client.on('stderr', line => stderr.push(line));
  await client.start({ ...options, codexPath: process.env.IXRAY_CODEX_SMOKE_PATH });
  const account = await client.request('account/read', { refreshToken: false });
  assert.equal(account.account, null);
  const models = await client.request('model/list', { limit: 100, includeHidden: false });
  assert.ok(Array.isArray(models.data));
  assert.ok(!stderr.some(line => /unknown feature|unrecognized/i.test(line)), stderr.join(''));
});
