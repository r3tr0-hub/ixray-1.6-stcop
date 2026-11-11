import { EventEmitter } from 'node:events';
import { randomUUID, randomBytes, timingSafeEqual } from 'node:crypto';
import { createServer, connect } from 'node:net';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { unlink } from 'node:fs/promises';

export const MaxMessageBytes = 32 * 1024 * 1024;

export function codedError(code, message, outcome) {
  return Object.assign(new Error(message), { code, ...(outcome ? { outcome } : {}) });
}

// One newline-framed channel is shared by panel actions and MCP calls. No scene
// structures are accessed by Node: the editor drains these requests on its frame.
export class EditorChannel extends EventEmitter {
  constructor({ write, timeoutMs = 30000 }) {
    super();
    this.write = write;
    this.timeoutMs = timeoutMs;
    this.pending = new Map();
    this.deadlines = new Map();
    this.buffer = '';
    this.closed = false;
  }

  push(chunk) {
    this.buffer += chunk;
    if (Buffer.byteLength(this.buffer) > MaxMessageBytes) {
      this.close(codedError('FRAME_TOO_LARGE', 'Превышен допустимый размер сообщения редактора.'));
      return;
    }
    for (;;) {
      const end = this.buffer.indexOf('\n');
      if (end < 0) break;
      const line = this.buffer.slice(0, end);
      this.buffer = this.buffer.slice(end + 1);
      if (!line.trim()) continue;
      let message;
      try { message = JSON.parse(line); }
      catch { this.emit('protocolError', codedError('INVALID_JSON', 'Редактор прислал некорректный JSON.')); continue; }
      if (message.type === 'editor.response') {
        const pending = this.pending.get(message.id);
        if (!pending) { this.emit('lateResponse', message.id); continue; }
        this.pending.delete(message.id);
        pending.cleanup();
        if (message.error) pending.reject(codedError(message.error.code ?? 'EDITOR_ERROR', message.error.message ?? 'Ошибка редактора.', message.error.outcome));
        else pending.resolve(message.result);
      } else if (message.type === 'ui.request') {
        this.emit('uiRequest', message);
      }
    }
  }

  request(method, params = {}, { timeoutMs = this.timeoutMs, signal } = {}) {
    if (this.closed) return Promise.reject(codedError('EDITOR_CLOSED', 'Соединение с редактором закрыто.', 'not_sent'));
    if (signal?.aborted) return Promise.reject(codedError('CANCELED', 'Запрос отменён до отправки.', 'not_sent'));
    const id = randomUUID();
    if (params.op_id && method !== 'scene.operation_status') {
      if (!this.deadlines.has(params.op_id) && this.deadlines.size >= 10000) return Promise.reject(codedError('OPERATION_LIMIT', 'Достигнут предел журнала операций. Сохраните сцену и перезапустите редактор.', 'not_sent'));
      if (!this.deadlines.has(params.op_id)) this.deadlines.set(params.op_id, Date.now() + timeoutMs);
      params = { ...params, deadline_ms: this.deadlines.get(params.op_id) };
    }
    return new Promise((resolve, reject) => {
      const abort = () => {
        if (!this.pending.delete(id)) return;
        cleanup();
        try { this.write({ type: 'editor.cancel', ids: [id] }); } catch {}
        reject(codedError('REQUEST_INTERRUPTED', 'Остановка запрошена; результат отправленной операции неизвестен.', 'unknown'));
      };
      const cleanup = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); };
      const timer = setTimeout(() => {
        this.pending.delete(id);
        cleanup();
        try { this.write({ type: 'editor.cancel', ids: [id] }); } catch {}
        reject(codedError('EDITOR_TIMEOUT', 'Редактор не ответил вовремя. Результат операции неизвестен; повторяйте только с тем же op_id.', 'unknown'));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer, cleanup });
      signal?.addEventListener('abort', abort, { once: true });
      try { this.write({ type: 'editor.request', id, method, params }); }
      catch (error) {
        cleanup(); this.pending.delete(id);
        reject(codedError('EDITOR_WRITE_FAILED', error.message, 'unknown'));
      }
    });
  }

  cancelPending(reason = 'Отменено до подтверждения результата.') {
    if (this.pending.size) {
      try { this.write({ type: 'editor.cancel', ids: [...this.pending.keys()] }); } catch {}
    }
    for (const item of this.pending.values()) {
      item.cleanup();
      item.reject(codedError('REQUEST_INTERRUPTED', reason, 'unknown'));
    }
    this.pending.clear();
  }

  close(error = codedError('EDITOR_CLOSED', 'Редактор закрыл соединение.', 'unknown')) {
    this.closed = true;
    for (const item of this.pending.values()) { item.cleanup(); item.reject(error); }
    this.pending.clear();
    this.emit('closed', error);
  }
}

function hasValidToken(actual, expected) {
  if (typeof actual !== 'string') return false;
  const a = Buffer.from(actual), b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

export class LocalEditorServer extends EventEmitter {
  constructor(channel, { beforeRequest } = {}) {
    super();
    this.channel = channel;
    this.token = randomBytes(32).toString('hex');
    const name = `ixray-ai-${process.pid}-${randomBytes(16).toString('hex')}`;
    this.path = process.platform === 'win32' ? `\\\\.\\pipe\\${name}` : join(tmpdir(), `${name}.sock`);
    this.sockets = new Set();
    this.authenticatedSockets = new Set();
    this.stopped = false;
    this.cancelGeneration = 0;
    this.beforeRequest = beforeRequest;
  }

  async start() {
    this.server = createServer(socket => {
      this.sockets.add(socket);
      socket.setEncoding('utf8');
      let buffer = '', authenticated = false, busy = false;
      const controllers = new Map();
      const authenticationTimer = setTimeout(() => socket.destroy(), 5000);
      socket.on('error', () => {});
      socket.on('close', () => { clearTimeout(authenticationTimer); for (const controller of controllers.values()) controller.abort(); this.sockets.delete(socket); this.authenticatedSockets.delete(socket); this.emit('clients', this.authenticatedSockets.size); });
      socket.on('data', chunk => {
        buffer += chunk;
        if (Buffer.byteLength(buffer) > MaxMessageBytes) { socket.destroy(); return; }
        for (;;) {
          const index = buffer.indexOf('\n');
          if (index < 0) break;
          const line = buffer.slice(0, index); buffer = buffer.slice(index + 1);
          let request;
          try { request = JSON.parse(line); } catch { socket.destroy(); return; }
          if (!authenticated) {
            if (request.type !== 'hello' || !hasValidToken(request.token, this.token)) { socket.destroy(); return; }
            authenticated = true; clearTimeout(authenticationTimer);
            this.authenticatedSockets.add(socket);
            socket.write(JSON.stringify({ type: 'ready', protocol: 1 }) + '\n');
            this.emit('clients', this.authenticatedSockets.size); continue;
          }
          if (request.type === 'cancel' && typeof request.id === 'string') { controllers.get(request.id)?.abort(); continue; }
          if (request.type !== 'request' || typeof request.id !== 'string' || typeof request.method !== 'string' || !request.method.startsWith('scene.')) { socket.destroy(); return; }
          if (busy) { socket.write(JSON.stringify({ id: request.id, error: { code: 'BUSY', message: 'Предыдущая команда ещё выполняется.', outcome: 'not_sent' } }) + '\n'); continue; }
          busy = true;
          const controller = new AbortController(); controllers.set(request.id, controller);
          const generation = this.cancelGeneration;
          const changesScene = ['scene.select', 'scene.focus', 'scene.transform', 'scene.duplicate', 'scene.place', 'scene.undo', 'scene.redo', 'scene.material_set', 'scene.save_copy'].includes(request.method);
          if (this.stopped && changesScene) {
            busy = false; controllers.delete(request.id);
            socket.write(JSON.stringify({ id: request.id, error: { code: 'CANCELED', message: 'Запрос отменён до выполнения.', outcome: 'canceled' } }) + '\n'); continue;
          }
          Promise.resolve().then(async () => {
            if (controller.signal.aborted) throw codedError('CANCELED', 'Запрос отменён до отправки.', 'canceled');
            if (this.beforeRequest) await this.beforeRequest(request.method, request.params, { signal: controller.signal });
            if (changesScene && (this.stopped || generation !== this.cancelGeneration)) throw codedError('CANCELED', 'Запрос отменён до отправки в редактор.', 'canceled');
            this.emit('operation', { method: request.method, op_id: request.params?.op_id, state: 'sent' });
            return this.channel.request(request.method, request.params, { signal: controller.signal });
          }).then(result => {
            this.emit('operation', { method: request.method, op_id: request.params?.op_id, state: result?.ok === false ? 'failed' : 'completed' });
            if (!socket.destroyed) socket.write(JSON.stringify({ id: request.id, result }) + '\n');
          }, error => {
            this.emit('operation', { method: request.method, op_id: request.params?.op_id, state: error.outcome ?? 'unknown', code: error.code });
            if (!socket.destroyed) socket.write(JSON.stringify({ id: request.id, error: { code: error.code ?? 'EDITOR_ERROR', message: error.message, outcome: error.outcome } }) + '\n');
          }).finally(() => { busy = false; controllers.delete(request.id); });
        }
      });
    });
    await new Promise((resolve, reject) => { this.server.once('error', reject); this.server.listen(this.path, resolve); });
    if (process.platform !== 'win32') { const { chmod } = await import('node:fs/promises'); await chmod(this.path, 0o600); }
    return { path: this.path, token: this.token };
  }

  cancel() { this.cancelGeneration++; this.stopped = true; }
  resume() { this.stopped = false; }

  async close() {
    this.stopped = true;
    for (const socket of this.sockets) socket.destroy();
    if (this.server) await new Promise(resolve => this.server.close(resolve));
    if (process.platform !== 'win32') await unlink(this.path).catch(() => {});
  }
}

export class LocalEditorClient {
  constructor({ path, token, timeoutMs = 335000 }) {
    this.path = path; this.token = token; this.timeoutMs = timeoutMs;
    this.pending = new Map(); this.buffer = '';
  }

  async start() {
    if (!this.path || !this.token) throw codedError('NO_EDITOR_SESSION', 'Редактор не указал локальный сеанс MCP.');
    this.socket = connect(this.path); this.socket.setEncoding('utf8');
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.socket.destroy(); reject(codedError('CONNECT_TIMEOUT', 'Истекло время подключения к редактору.')); }, 5000);
      this.socket.on('error', error => { clearTimeout(timer); reject(error); this.failAll(error); });
      this.socket.on('close', () => this.failAll(codedError('EDITOR_DISCONNECTED', 'Соединение с редактором разорвано.', 'unknown')));
      this.socket.on('data', chunk => {
        this.buffer += chunk;
        if (Buffer.byteLength(this.buffer) > MaxMessageBytes) { this.socket.destroy(); return; }
        for (;;) {
          const index = this.buffer.indexOf('\n'); if (index < 0) break;
          const line = this.buffer.slice(0, index); this.buffer = this.buffer.slice(index + 1);
          let response; try { response = JSON.parse(line); } catch { this.socket.destroy(); return; }
          if (response.type === 'ready') { clearTimeout(timer); resolve(); continue; }
          const item = this.pending.get(response.id); if (!item) continue;
          item.cleanup(); this.pending.delete(response.id);
          if (response.error) item.reject(codedError(response.error.code, response.error.message, response.error.outcome));
          else item.resolve(response.result);
        }
      });
      this.socket.on('connect', () => this.socket.write(JSON.stringify({ type: 'hello', token: this.token }) + '\n'));
    });
    return this;
  }

  request(method, params, { signal } = {}) {
    if (!this.socket || this.socket.destroyed) return Promise.reject(codedError('EDITOR_CLOSED', 'Редактор недоступен.', 'not_sent'));
    if (signal?.aborted) return Promise.reject(codedError('CANCELED', 'Запрос отменён до отправки.', 'not_sent'));
    const id = randomUUID();
    return new Promise((resolve, reject) => {
      const abort = () => {
        if (!this.pending.delete(id)) return;
        cleanup();
        if (!this.socket.destroyed) this.socket.write(JSON.stringify({ type: 'cancel', id }) + '\n');
        reject(codedError('REQUEST_INTERRUPTED', 'Запрошена остановка инструмента; проверяйте результат по op_id.', 'unknown'));
      };
      const cleanup = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); };
      const timer = setTimeout(() => { this.pending.delete(id); cleanup(); if (!this.socket.destroyed) this.socket.write(JSON.stringify({ type: 'cancel', id }) + '\n'); reject(codedError('EDITOR_TIMEOUT', 'Результат команды неизвестен. Используйте исходный op_id.', 'unknown')); }, this.timeoutMs);
      this.pending.set(id, { resolve, reject, timer, cleanup });
      signal?.addEventListener('abort', abort, { once: true });
      this.socket.write(JSON.stringify({ type: 'request', id, method, params }) + '\n');
    });
  }

  failAll(error) { for (const item of this.pending.values()) { item.cleanup(); item.reject(error); } this.pending.clear(); }
  close() { this.socket?.destroy(); this.failAll(codedError('EDITOR_CLOSED', 'Соединение закрыто.', 'unknown')); }
}
