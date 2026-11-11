import { EventEmitter, once } from 'node:events';
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile, rename } from 'node:fs/promises';
import { homedir } from 'node:os';
import path from 'node:path';
import { StringDecoder } from 'node:string_decoder';

const CONFIG_MARKER = '# Managed by IX-Ray AI Assistant; isolated integration configuration.';
const MAX_LINE_BYTES = 32 * 1024 * 1024;

function errorWith(message, code, extra = {}) {
  return Object.assign(new Error(message), { code, ...extra });
}

// Diagnostics must never contain OAuth parameters, JWTs, API keys, or MCP capabilities.
export function redactSecrets(value, secrets = []) {
  let text = String(value);
  for (const secret of secrets) {
    if (typeof secret === 'string' && secret.length >= 8) text = text.split(secret).join('[REDACTED]');
  }
  return text
    .replace(/\bBearer\s+[^\s"',}]+/gi, 'Bearer [REDACTED]')
    .replace(/\bsk-[A-Za-z0-9_-]{8,}/g, '[REDACTED]')
    .replace(/\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+/g, '[REDACTED]')
    .replace(/((?:access_token|refresh_token|id_token|client_secret|api_key|authorization|userCode|user_code)\s*["']?\s*[:=]\s*["']?)[^\s"',}&]+/gi, '$1[REDACTED]')
    .replace(/https?:\/\/[^\s"<>]+/g, url => url.replace(/[?#].*$/, '?[REDACTED]'));
}

function redactObject(value, secrets) {
  if (typeof value === 'string') return redactSecrets(value, secrets);
  if (Array.isArray(value)) return value.map(item => redactObject(item, secrets));
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key,
      /token|secret|authorization|cookie|password|user.?code/i.test(key)
        ? '[REDACTED]' : redactObject(item, secrets)]));
  }
  return value;
}

function absolute(value, name) {
  if (typeof value !== 'string' || !path.isAbsolute(value)) {
    throw errorWith(`${name} must be an absolute path`, 'INVALID_CONFIGURATION');
  }
  return path.resolve(value);
}

function tomlString(value) {
  // JSON basic-string escaping is compatible with TOML for the paths and arguments used here.
  return JSON.stringify(value).replace(/\\u0000/g, '\\u0000');
}

/** A private, asynchronous stdio client for the official Codex app-server. */
export class AppServerClient extends EventEmitter {
  constructor({ requestTimeoutMs = 120000, startupTimeoutMs = 30000 } = {}) {
    super();
    this.requestTimeoutMs = requestTimeoutMs;
    this.startupTimeoutMs = startupTimeoutMs;
    this.pending = new Map();
    this.nextId = 1;
    this.child = null;
    this.ready = false;
    this.secrets = [];
    this.buffer = '';
  }

  async start({ codexPath, home, cwd, mcpCommand, mcpArgs = [], mcpEnv = {}, standalone } = {}) {
    if (this.child) throw errorWith('App-server is already running', 'ALREADY_RUNNING');
    codexPath = absolute(codexPath, 'codexPath');
    home = absolute(home, 'home');
    cwd = absolute(cwd, 'cwd');
    mcpCommand = absolute(mcpCommand, 'mcpCommand');
    const defaultHome = path.resolve(homedir(), '.codex');
    if (home.toLowerCase() === defaultHome.toLowerCase()) {
      throw errorWith('IX-Ray requires its own Codex home', 'UNSAFE_CODEX_HOME');
    }
    if (!Array.isArray(mcpArgs) || mcpArgs.some(value => typeof value !== 'string' || value.includes('\0'))) {
      throw errorWith('mcpArgs must contain strings', 'INVALID_CONFIGURATION');
    }
    if (!mcpEnv || typeof mcpEnv !== 'object' || Array.isArray(mcpEnv)
        || Object.entries(mcpEnv).some(([key, value]) => !/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)
          || typeof value !== 'string' || value.includes('\0') || /^(CODEX_HOME|OPENAI_API_KEY|CODEX_API_KEY)$/i.test(key))) {
      throw errorWith('Invalid MCP environment', 'INVALID_CONFIGURATION');
    }
    this.secrets = Object.values(mcpEnv);
    await mkdir(home, { recursive: true, mode: 0o700 });
    const configPath = path.join(home, 'config.toml');
    let previous;
    try { previous = await readFile(configPath, 'utf8'); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (previous !== undefined && !previous.startsWith(CONFIG_MARKER)) {
      throw errorWith('Refusing to replace an unrelated Codex configuration', 'UNSAFE_CODEX_CONFIG');
    }
    const config = [
      CONFIG_MARKER,
      'forced_login_method = "chatgpt"',
      'cli_auth_credentials_store = "auto"',
      'approval_policy = "never"',
      'sandbox_mode = "read-only"',
      'web_search = "disabled"',
      '',
      '[features]',
      'shell_tool = false',
      'unified_exec = false',
      'apply_patch_freeform = false',
      'apps = false',
      'plugins = false',
      'multi_agent = false',
      'multi_agent_mode = false',
      'code_mode = false',
      'js_repl = false',
      'computer_use = false',
      'browser_use = false',
      'image_generation = false',
      '',
      '[mcp_servers.ixray]',
      `command = ${tomlString(mcpCommand)}`,
      `args = [${mcpArgs.map(tomlString).join(', ')}]`,
      `env_vars = [${Object.keys(mcpEnv).map(tomlString).join(', ')}]`,
      `cwd = ${tomlString(cwd)}`,
      'enabled = true',
      'required = true',
      'startup_timeout_sec = 20',
      'tool_timeout_sec = 600',
      '',
    ].join('\n');
    const temporary = `${configPath}.${process.pid}.tmp`;
    await writeFile(temporary, config, { mode: 0o600 });
    await rename(temporary, configPath);

    const env = { ...process.env };
    for (const key of Object.keys(env)) {
      if (/OPENAI|CHATGPT|CODEX/i.test(key)) delete env[key];
    }
    Object.assign(env, mcpEnv, { CODEX_HOME: home });
    const isStandalone = standalone ?? /codex-app-server/i.test(path.basename(codexPath));
    const args = isStandalone ? ['--listen', 'stdio://'] : ['app-server', '--listen', 'stdio://'];
    this.buffer = '';
    const child = spawn(codexPath, args, {
      cwd, env, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true,
      detached: process.platform !== 'win32', shell: false,
    });
    this.child = child;
    const decoder = new StringDecoder('utf8');
    const stderrDecoder = new StringDecoder('utf8');
    let stderrBuffer = '';
    let discardStderrLine = false;
    const readStderr = (chunk, final = false) => {
      stderrBuffer += chunk;
      let newline;
      while ((newline = stderrBuffer.indexOf('\n')) >= 0) {
        const line = stderrBuffer.slice(0, newline + 1);
        stderrBuffer = stderrBuffer.slice(newline + 1);
        if (!discardStderrLine) this.emit('stderr', redactSecrets(line, this.secrets));
        discardStderrLine = false;
      }
      if (stderrBuffer.length > 65536) {
        stderrBuffer = '';
        discardStderrLine = true;
      }
      if (final && stderrBuffer && !discardStderrLine) this.emit('stderr', redactSecrets(stderrBuffer, this.secrets));
    };
    child.stdout.on('data', bytes => this.#read(decoder.write(bytes)));
    child.stdout.on('end', () => this.#read(decoder.end()));
    child.stderr.on('data', bytes => readStderr(stderrDecoder.write(bytes)));
    child.stderr.on('end', () => readStderr(stderrDecoder.end(), true));
    child.stdin.on('error', error => this.#failed(errorWith(redactSecrets(error.message, this.secrets), 'TRANSPORT_ERROR')));
    child.on('error', error => {
      this.#failed(errorWith(redactSecrets(error.message, this.secrets), 'START_FAILED'));
      if (this.child === child) this.child = null;
    });
    child.on('exit', (code, signal) => {
      this.#failed(errorWith(`App-server exited (${code ?? signal ?? 'unknown'})`, 'PROCESS_EXITED'));
      if (this.child === child) this.child = null;
      this.emit('exit', code, signal);
    });
    try {
      const info = await this.request('initialize', {
        clientInfo: { name: 'ixray_level_editor', title: 'IX-Ray Level Editor', version: '0.1.0' },
      }, { timeoutMs: this.startupTimeoutMs });
      this.notify('initialized', {});
      this.ready = true;
      return info;
    } catch (error) {
      await this.stop();
      throw error;
    }
  }

  request(method, params = {}, { timeoutMs = this.requestTimeoutMs } = {}) {
    if (typeof method !== 'string' || !method) return Promise.reject(errorWith('Invalid RPC method', 'INVALID_REQUEST'));
    if (/^(command\/|process\/|thread\/shellCommand$|config\/(?:value\/write|batchWrite)$)/.test(method)) {
      return Promise.reject(errorWith('This integration exposes editor tools only', 'METHOD_DISABLED'));
    }
    if (method === 'account/login/start' && !['chatgpt', 'chatgptDeviceCode'].includes(params?.type)) {
      return Promise.reject(errorWith('Use official managed ChatGPT login', 'AUTH_MODE_DISABLED'));
    }
    if (!this.child || this.child.killed || !this.child.stdin.writable) {
      return Promise.reject(errorWith('App-server is disconnected', 'DISCONNECTED'));
    }
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(errorWith(`Timed out waiting for ${method}; result is unknown`, 'RPC_TIMEOUT', { method, outcome: 'unknown' }));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer, method });
      try { this.#write({ method, id, params }); }
      catch (error) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(error);
      }
    });
  }

  notify(method, params = {}) {
    this.#write({ method, params });
  }

  #write(message) {
    if (!this.child?.stdin?.writable || this.child.killed) throw errorWith('App-server is disconnected', 'DISCONNECTED');
    this.child.stdin.write(`${JSON.stringify(message)}\n`);
  }

  #read(chunk) {
    this.buffer += chunk;
    if (Buffer.byteLength(this.buffer) > MAX_LINE_BYTES) {
      this.#failed(errorWith('App-server message exceeds limit', 'MESSAGE_TOO_LARGE'));
      void this.stop();
      return;
    }
    let newline;
    while ((newline = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (!line) continue;
      let message;
      try { message = JSON.parse(line); }
      catch {
        // Never copy malformed stdout to diagnostics: it may contain credentials.
        this.emit('stderr', 'App-server emitted malformed JSON; message omitted.');
        continue;
      }
      if (message.method && Object.hasOwn(message, 'id')) this.#serverRequest(message);
      else if (message.method) this.emit('notification', message.method, message.params ?? {});
      else if (Object.hasOwn(message, 'id')) {
        const pending = this.pending.get(message.id);
        if (!pending) continue;
        clearTimeout(pending.timer);
        this.pending.delete(message.id);
        if (message.error) {
          pending.reject(errorWith(redactSecrets(message.error.message ?? 'App-server request failed', this.secrets), message.error.code,
            { method: pending.method, data: redactObject(message.error.data, this.secrets) }));
        } else pending.resolve(message.result);
      }
    }
  }

  #serverRequest({ id, method, params }) {
    // No server-initiated command, file-write, or privilege approval may escape the editor API.
    let result;
    if (['item/commandExecution/requestApproval', 'item/fileChange/requestApproval'].includes(method)) result = { decision: 'decline' };
    else if (['execCommandApproval', 'applyPatchApproval'].includes(method)) result = { decision: { denied: { rejection: 'Use IX-Ray editor tools.' } } };
    else if (method === 'item/permissions/requestApproval') result = { permissions: {}, scope: 'turn' };
    else if (method === 'mcpServer/elicitation/request') result = { action: 'decline', content: null };
    else if (['item/tool/requestUserInput', 'tool/requestUserInput'].includes(method)) result = { answers: {} };
    else if (method === 'item/tool/call') result = { success: false, contentItems: [{ type: 'inputText', text: 'Dynamic tools are disabled. Use the ixray MCP server.' }] };
    this.emit('serverRequest', method, redactObject(params ?? {}, this.secrets));
    try {
      if (result !== undefined) this.#write({ id, result });
      else this.#write({ id, error: { code: -32601, message: 'Unsupported server request' } });
    } catch { /* A closing process cannot receive a reply. */ }
  }

  #failed(error) {
    this.ready = false;
    for (const { reject, timer } of this.pending.values()) {
      clearTimeout(timer);
      reject(error);
    }
    this.pending.clear();
  }

  async stop() {
    const child = this.child;
    this.ready = false;
    if (!child) return;
    this.#failed(errorWith('App-server stopped', 'DISCONNECTED'));
    const exited = once(child, 'exit').catch(() => {});
    child.stdin.end();
    let timer;
    const graceful = await Promise.race([exited.then(() => true), new Promise(resolve => { timer = setTimeout(() => resolve(false), 1500); })]);
    clearTimeout(timer);
    if (!graceful && child.pid) {
      if (process.platform === 'win32') {
        const taskkill = path.join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'taskkill.exe');
        await new Promise(resolve => {
          const killer = spawn(taskkill, ['/PID', String(child.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' });
          killer.once('error', resolve);
          killer.once('exit', resolve);
        });
      } else {
        try { process.kill(-child.pid, 'SIGTERM'); } catch { child.kill('SIGTERM'); }
      }
      let killTimer;
      await Promise.race([exited, new Promise(resolve => { killTimer = setTimeout(() => {
        try { if (process.platform !== 'win32') process.kill(-child.pid, 'SIGKILL'); else child.kill(); } catch { /* Already exited. */ }
        resolve();
      }, 1500); })]);
      clearTimeout(killTimer);
    }
    if (this.child === child) this.child = null;
  }
}
