import assert from 'node:assert/strict';
import { mkdtemp, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { AppServerClient } from '../src/app-server.mjs';

// No login, inference, inherited credentials, or existing Codex home is used.
const executable = process.argv[2];
if (!executable) throw new Error('Usage: node smoke-app-server.mjs <runtime/codex/codex.exe>');
const workspace = await mkdtemp(path.join(tmpdir(), 'IX-Ray AI Кириллица '));
const home = path.join(workspace, 'private codex');
const client = new AppServerClient({ requestTimeoutMs: 30000, startupTimeoutMs: 30000 });
const diagnostics = [];
client.on('stderr', line => diagnostics.push(line));
try {
  const initialized = await client.start({
    codexPath: path.resolve(executable), home, cwd: workspace,
    mcpCommand: process.execPath, mcpArgs: ['-e', 'process.stdin.resume()'],
  });
  const account = await client.request('account/read', { refreshToken: false });
  assert.equal(account.account, null, 'Fresh isolated CODEX_HOME must not inherit an account');
  const models = await client.request('model/list', { limit: 100, includeHidden: false });
  assert.ok(Array.isArray(models.data), 'model/list must return a catalog');
  assert.ok(!(await readdir(home)).includes('auth.json'), 'Smoke must not create credentials');
  assert.ok(!diagnostics.some(line => /unknown feature|unrecognized/i.test(line)), diagnostics.join(''));
  console.log(JSON.stringify({
    passed: true, initialized: Boolean(initialized), platformFamily: initialized.platformFamily,
    platformOs: initialized.platformOs, account: null, modelCount: models.data.length,
    defaultModel: models.data.find(model => model.isDefault)?.model ?? null,
    loginPerformed: false, inferencePerformed: false,
  }, null, 2));
} finally {
  await client.stop();
  await rm(workspace, { recursive: true, force: true });
}
