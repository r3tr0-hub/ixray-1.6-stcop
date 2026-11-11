import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod/v4';
import { pathToFileURL } from 'node:url';
import { LocalEditorClient } from './editor-channel.mjs';

const Id = z.string().min(1).max(128);
const Vec = z.tuple([z.number().finite(), z.number().finite(), z.number().finite()]);
const Session = { scene_session: Id.optional() };
const Mutation = { scene_session: Id, op_id: z.string().min(8).max(128).describe('Уникальный ID операции. При повторе после сбоя ОБЯЗАТЕЛЬНО используйте тот же ID и те же параметры.') };
const Objects = { ids: z.array(Id).max(128).optional().describe('Непрозрачные идентификаторы из текущей сцены; по умолчанию выделение. Один пакет — до 128 объектов.') };
const Page = { offset: z.number().int().nonnegative().optional(), limit: z.number().int().min(1).max(500).optional() };
const Material = {
  object_id: Id,
  surface_index: z.number().int().nonnegative(),
  texture: z.string().min(1).max(255).optional(),
  render_shader: z.string().min(1).max(256).optional(),
  compiler_shader: z.string().min(1).max(256).optional(),
  game_material: z.string().min(1).max(256).optional()
};

export const ToolDefinitions = [
  ['scene.sdk_catalog', 'Поиск возможностей SDK. Выполняются записи с непустым tool. Записи COMMAND_* показывают внутренние команды без адаптера.', { ...Page, query: z.string().max(128).optional(), include_native: z.boolean().optional() }],
  ['scene.sdk_status', 'Состояние вкладки, экспорта, компилятора и игры. Остановка процесса сама по себе не означает успех.', {}],
  ['scene.sdk_assets', 'Поиск по библиотекам файлов SDK без изменения ресурсов. Продолжайте с next_offset.', { ...Session, ...Page, kind: z.enum(['objects', 'textures', 'texture_sources', 'texture_metadata', 'sounds', 'levels']), query: z.string().max(128).optional() }],
  ['scene.sdk_model', 'Читает геометрию и поверхности библиотечной модели .object по её пути относительно библиотеки SDK.', { ...Session, ...Page, reference: z.string().min(1).max(200) }],
  ['scene.sdk_run', 'Запустить штатную операцию из каталога scene_sdk_catalog. Для экспорта пользователь подтверждает штатный диалог SDK. accepted не означает, что результат проверен.', { ...Mutation, action: Id }],
  ['scene.sdk_tab', 'Перейти в штатный редактор SDK. Возвращает активную вкладку; свойства редакторов доступны через отдельные команды.', { ...Mutation, tab: z.enum(['actor', 'shader', 'particles', 'logic', 'dialogs', 'macros', 'layout', 'level']) }],
  ['scene.sdk_compile', 'Запустить компилятор, заданный в настройках SDK. Сцена должна быть сохранена. Состояние процесса проверяйте через scene_sdk_status, результат — по журналу компилятора.', { ...Mutation, mode: z.enum(['geometry', 'details', 'ai_high', 'ai_low', 'ai_verify', 'spawn_current', 'spawn_all']) }],
  ['scene.object_properties', 'Читает перечень полей штатной панели свойств объекта, их типы и доступность. Основные значения находятся в object.', { ...Session, ...Page, object_id: Id }],
  ['scene.object_edit', 'Переименовать один объект или изменить его видимость одним шагом Undo. Имя: строчные латинские буквы, цифры, дефис и подчёркивание.', { ...Mutation, object_id: Id, name: z.string().regex(/^[a-z0-9_-]{1,128}$/).optional(), visible: z.boolean().optional() }],
  ['scene.status', 'Текущая сцена, доступность редактирования, несохранённые изменения, количество объектов и выделение. Начните с этого инструмента.', Session],
  ['scene.search', 'Поиск объектов с постраничным результатом. Совместные фильтры пересекаются. texture учитывает реальные поверхности экземпляра. offset — курсор сканирования сцены: продолжайте next_offset, пока has_more=true, даже при пустой странице.', { ...Session, ...Page, name: z.string().max(255).optional(), class_id: z.number().int().nonnegative().optional(), reference: z.string().max(255).optional(), texture: z.string().max(255).optional() }],
  ['scene.selection', 'Читает выделение: непрозрачный ID, имя, класс, модель, позиция (метры), поворот (градусы), масштаб.', { ...Session, ...Page }],
  ['scene.select', 'Выделить указанные объекты; clear=true сначала снимает выделение. ids=[] и clear=true снимает всё выделение. Для большего набора используйте последовательные пакеты по 128 с clear=false после первого.', { ...Mutation, ids: z.array(Id).max(128), clear: z.boolean().optional() }],
  ['scene.focus', 'Навести камеру на объекты или текущее выделение.', { ...Mutation, ...Objects }],
  ['scene.transform', 'Атомарно изменить объекты одним шагом Undo. translation_m и rotation_degrees — приращения; scale_factor — множители. Система левая, +Y вверх; оси мировые.', { ...Mutation, ...Objects, translation_m: Vec.optional(), rotation_degrees: Vec.optional(), scale_factor: Vec.optional() }],
  ['scene.duplicate', 'Дублировать указанные объекты или выделение одним шагом Undo. Возвращает ID созданных объектов.', { ...Mutation, ...Objects, translation_m: Vec.optional() }],
  ['scene.place', 'Разместить существующую модель .object из библиотеки SDK одним шагом Undo.', { ...Mutation, reference: z.string().min(1).max(200), position_m: Vec.optional(), rotation_degrees: Vec.optional(), scale: Vec.optional() }],
  ['scene.undo', 'Отменить последний штатный шаг Undo. Все старые object ID становятся недействительными; перечитайте сцену.', Mutation],
  ['scene.redo', 'Повторить последний штатный шаг Redo. Все старые object ID становятся недействительными.', Mutation],
  ['scene.materials', 'Прочитать отдельные поверхности выбранных объектов: текстура, render shader, compiler shader, game material.', { ...Session, ...Objects, object_id: Id.optional() }],
  ['scene.material_set', 'Изменить точно указанные поверхности ЭКЗЕМПЛЯРОВ сцены одним шагом Undo. Библиотечные .object не изменяются. Сначала прочитайте материалы; используйте только существующие ресурсы. Один пакет — до 128 поверхностей.', { ...Mutation, changes: z.array(z.object(Material).strict()).min(1).max(128) }],
  ['scene.missing_textures', 'Проверить текстуры поверхностей с учётом реальных путей файловой системы SDK.', { ...Session, ...Objects, ...Page, object_id: Id.optional() }],
  ['scene.viewport', 'Получить актуальное изображение viewport с привязкой к сцене и ревизии. Выполняйте после изменения для визуальной проверки.', { ...Session, width: z.number().int().min(64).max(1280).optional() }],
  ['scene.save_copy', 'Сохранить открытую сцену в отдельную НОВУЮ копию .level по абсолютному пути (не более 240 UTF-8 байт). Существующие файлы не перезаписываются.', { ...Mutation, path: z.string().min(1).max(240) }],
  ['scene.operation_status', 'Проверить уже отправленную операцию по op_id после обрыва связи или таймаута. Не создаёт и не повторяет изменения.', { op_id: z.string().min(8).max(128) }]
];

export function createMcpServer(editor) {
  const server = new McpServer({ name: 'ixray-level-editor', version: '0.1.0' });
  for (const [method, description, shape] of ToolDefinitions) {
    const mutation = Object.hasOwn(shape, 'op_id') && method !== 'scene.operation_status';
    server.registerTool(method.replaceAll('.', '_'), {
      title: `IX-Ray: ${method.slice(6)}`,
      description,
      inputSchema: z.object(shape).strict(),
      annotations: { readOnlyHint: !mutation, destructiveHint: false, idempotentHint: true, openWorldHint: false }
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
