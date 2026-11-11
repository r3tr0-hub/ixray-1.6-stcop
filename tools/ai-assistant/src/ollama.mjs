import { randomUUID } from 'node:crypto';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { codedError } from './editor-channel.mjs';
import { createMcpServer } from './mcp.mjs';
import { isMutationTool } from './tool-definitions.mjs';

const Endpoint = 'http://127.0.0.1:11434';
const DefaultModel = 'deepseek-r1:8b';
const CoreTools = new Set([
  'scene_status', 'scene_search', 'scene_selection', 'scene_select',
  'scene_transform', 'scene_undo', 'scene_operation_status'
]);
const FindTools = {
  type: 'function',
  function: {
    name: 'ixray_find_tools',
    description: 'Найти дополнительные команды SDK по имени или описанию. Подходящие команды появятся в следующем запросе. Пустой query показывает список по страницам.',
    parameters: {
      type: 'object',
      properties: { query: { type: 'string' }, offset: { type: 'integer', minimum: 0 } },
      required: ['query'],
      additionalProperties: false
    }
  }
};

function assertActive(signal) {
  if (signal?.aborted) throw codedError('CANCELED', 'Запрос к Ollama остановлен.', 'canceled');
}

async function readJson(response) {
  let bytes = 0;
  const chunks = [];
  for await (const chunk of response.body ?? []) {
    bytes += chunk.byteLength;
    if (bytes > 2 * 1024 * 1024) throw codedError('OLLAMA_RESPONSE_LIMIT', 'Ответ Ollama превышает допустимый размер.');
    chunks.push(chunk);
  }
  try { return JSON.parse(Buffer.concat(chunks).toString('utf8')); }
  catch { throw codedError('OLLAMA_PROTOCOL', 'Ollama вернул некорректный JSON.'); }
}

function limitedText(text, length) {
  return text.length <= length ? text : text.slice(0, length) + '\n[Результат сокращён. Уточните фильтр или запросите меньшую страницу.]';
}

function parseToolError(text) {
  try { return JSON.parse(text); } catch { return null; }
}

function toolResult(result) {
  const content = (result.content ?? []).filter(item => item.type === 'text').map(item => item.text).join('\n');
  return limitedText(content || (result.isError ? 'Ошибка MCP.' : 'Инструмент не вернул текст.'), 3500);
}

export class Ollama {
  constructor(fetchImpl = globalThis.fetch) {
    this.fetch = fetchImpl;
  }

  async request(path, payload, signal, timeoutMs = 5000) {
    assertActive(signal);
    const timeout = AbortSignal.timeout(timeoutMs);
    const requestSignal = signal ? AbortSignal.any([signal, timeout]) : timeout;
    try {
      const response = await this.fetch(Endpoint + path, {
        ...(payload === undefined ? {} : {
          method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
        }),
        redirect: 'error', signal: requestSignal
      });
      if (!response.ok) {
        await response.body?.cancel();
        if (response.status === 404) throw codedError('OLLAMA_MODEL_MISSING', 'Модель не найдена. Установите её через ollama pull и обновите список.');
        throw codedError('OLLAMA_HTTP', `Ollama отклонил запрос (HTTP ${response.status}). Проверьте модель и журнал Ollama.`);
      }
      return await readJson(response);
    } catch (error) {
      assertActive(signal);
      if (timeout.aborted) throw codedError('OLLAMA_TIMEOUT', 'Ollama не ответил вовремя. Проверьте загрузку модели и доступную память.');
      if (error.code) throw error;
      throw codedError('OLLAMA_UNAVAILABLE', 'Запустите Ollama на этом компьютере. Помощник подключается к 127.0.0.1:11434.');
    }
  }

  async listModels(signal) {
    const result = await this.request('/api/tags', undefined, signal);
    if (!Array.isArray(result?.models)) throw codedError('OLLAMA_PROTOCOL', 'В ответе Ollama отсутствует список моделей.');
    const names = [...new Set(result.models.filter(item =>
      typeof item?.name === 'string' && /^[\w./:-]{1,200}$/.test(item.name) &&
      Number.isSafeInteger(item.size) && item.size > 0 &&
      !item.remote_host && !item.remote_model && !/[:\-]cloud(?:$|:)/i.test(item.name)
    ).map(item => item.name))];
    const preferred = [DefaultModel, 'deepseek-r1:latest', 'deepseek-r1:14b'].find(name => names.includes(name)) ?? names[0];
    return names.map(model => ({ id: model, model, displayName: model, isDefault: model === preferred }));
  }

  async chat({ model, text, history, instructions, editor, signal }) {
    const info = await this.request('/api/show', { model }, signal, 30000);
    if (info?.remote_host || info?.remote_model) {
      throw codedError('OLLAMA_REMOTE_MODEL', 'Выберите модель, загруженную для локального выполнения.');
    }
    if (!Array.isArray(info?.capabilities) || !info.capabilities.includes('tools')) {
      throw codedError('OLLAMA_TOOLS_UNSUPPORTED', 'Ollama не подтвердил поддержку инструментов у модели. Попробуйте актуальный deepseek-r1:8b и обновите Ollama.');
    }

    const client = new Client({ name: 'ixray-ollama', version: '1' });
    const server = createMcpServer(editor);
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    try {
      await server.connect(serverTransport);
      await client.connect(clientTransport);
      const { tools } = await client.listTools();
      const available = tools.filter(tool => tool.name !== 'scene_viewport');
      const visible = new Set(CoreTools);
      const messages = [
        { role: 'system', content: instructions },
        ...history.slice(-6),
        { role: 'user', content: text }
      ];

      let executed = 0;
      for (let round = 0; round < 16; round++) {
        const advertised = available.filter(tool => visible.has(tool.name));
        const response = await this.request('/api/chat', {
          model, messages, stream: false, think: false, keep_alive: '2m',
          options: { num_ctx: 8192, num_predict: 1536, temperature: 0.1 },
          tools: [FindTools, ...advertised.map(tool => ({
            type: 'function',
            function: { name: tool.name, description: tool.description, parameters: tool.inputSchema }
          }))]
        }, signal, 180000);
        assertActive(signal);
        const message = response?.message;
        if (message?.role !== 'assistant' || typeof message.content !== 'string' ||
            (message.tool_calls !== undefined && !Array.isArray(message.tool_calls))) {
          throw codedError('OLLAMA_PROTOCOL', 'Ollama вернул некорректное сообщение модели.');
        }
        const calls = message.tool_calls ?? [];
        if (!calls.length) {
          if (!message.content.trim()) throw codedError('OLLAMA_EMPTY', 'Модель вернула пустой ответ.');
          return message.content;
        }
        if (executed + calls.length > 24 || calls.length > 8) break;
        executed += calls.length;
        messages.push(message);
        for (const call of calls) {
          assertActive(signal);
          const name = call?.function?.name;
          const args = call?.function?.arguments;
          if (typeof name !== 'string' || !args || typeof args !== 'object' || Array.isArray(args)) {
            throw codedError('OLLAMA_PROTOCOL', 'Модель вернула некорректные параметры инструмента.');
          }
          let content;
          if (name === FindTools.function.name) {
            const query = typeof args.query === 'string' ? args.query.toLowerCase().trim().slice(0, 120) : '';
            const offset = Number.isSafeInteger(args.offset) && args.offset > 0 ? args.offset : 0;
            const matches = available.filter(tool => `${tool.name} ${tool.description}`.toLowerCase().includes(query));
            const page = matches.slice(offset, offset + 4);
            // Keep the core schema and the most recent discovery page small enough for a local context.
            visible.clear();
            for (const core of CoreTools) visible.add(core);
            for (const tool of page) visible.add(tool.name);
            content = JSON.stringify({
              enabled: page.map(tool => tool.name),
              next_offset: offset + page.length < matches.length ? offset + page.length : null
            });
          } else if (!advertised.some(tool => tool.name === name)) {
            content = JSON.stringify({ ok: false, error: 'Вызовите ixray_find_tools с точным именем команды.' });
          } else {
            const method = name.replace(/^scene_/, 'scene.');
            const validatedArgs = { ...args };
            if (isMutationTool(method) && validatedArgs.op_id === undefined) validatedArgs.op_id = randomUUID();
            const schema = advertised.find(tool => tool.name === name).inputSchema;
            if (schema.properties?.limit && validatedArgs.limit === undefined) validatedArgs.limit = 12;
            const result = await client.callTool({ name, arguments: validatedArgs }, undefined, { signal, timeout: 340000 });
            content = toolResult(result);
            if (isMutationTool(method) && result.isError) {
              const failure = result.structuredContent ?? parseToolError(result.content?.[0]?.text);
              if (failure?.error?.outcome === 'unknown' || failure?.outcome === 'unknown' || failure?.status === 'unknown') {
                throw codedError('OLLAMA_OPERATION_UNKNOWN', `Результат операции ${validatedArgs.op_id} неизвестен. Проверьте сцену или запросите её статус по этому op_id.`, 'unknown');
              }
            }
          }
          messages.push({ role: 'tool', tool_name: name, content });
        }
      }
      throw codedError('OLLAMA_TOOL_LIMIT', 'Модель исчерпала лимит вызовов. Проверьте журнал команд перед повторным запросом.');
    } finally {
      await client.close();
      await server.close();
    }
  }
}
