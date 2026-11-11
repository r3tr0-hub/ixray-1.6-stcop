import { z } from 'zod/v4';

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

const Text = z.string().max(8192);
const Path = z.string().min(1).max(240);
const Resource = z.string().min(1).max(200);
const Name = z.string().regex(/^[a-z0-9_-]{1,128}$/);
const Index = z.number().int().min(0).max(0xffffffff);
const Number = z.number().finite();
const Position2D = { x: Number.optional(), y: Number.optional() };
const Document = { ...Mutation, ...Page };
const Sample = { x: Index, z: Index };
const Scalar = z.union([Number, z.boolean()]);
const ChangeFields = z.record(z.string().max(128), z.union([Scalar, z.array(Number).max(32)]));
const GitPath = z.string().min(1).max(512);
const ThmChanges = z.object({
  format: Index.optional(), type: Index.optional(), mip_filter: Index.optional(),
  generate_mipmaps: z.boolean().optional(), material: Index.optional(), material_weight: Number.optional(),
  bump_mode: Index.optional(), bump_name: Resource.optional(), bump_virtual_height: Number.optional(),
  detail_name: Resource.optional(), detail_scale: Number.optional()
}).strict();

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
  ['scene.level_open', 'Открыть существующий уровень .level по абсолютному пути. Дождитесь scene_operation_status и получите новую сессию через scene_status.', { ...Mutation, path: Path }],
  ['scene.level_new', 'Создать пустую сцену, если текущий уровень сохранён.', Mutation],
  ['scene.level_save', 'Сохранить текущий уровень на диск. Для новой сцены укажите новый абсолютный путь .level; при сохранении открытого уровня опустите path.', { ...Mutation, path: Path.optional() }],
  ['scene.level_options', 'Прочитать имя сборки, версию карты и параметры компиляции уровня.', Session],
  ['scene.level_options_set', 'Настроить сборку уровня одним шагом Undo. Изменение числовых полей переводит качество в custom.', { ...Mutation, changes: z.object({
    build_name: Name.max(96).optional(), name_prefix: Name.max(96).optional(), map_version: z.string().regex(/^[0-9.]{1,96}$/).optional(),
    quality: z.enum(['draft','high','custom']).optional(), hemisphere_quality: Index.max(3).optional(), sun_quality: Index.max(3).optional(),
    normal_smooth_degrees: Number.min(0).max(180).optional(), weld_distance_m: Number.min(0).max(1).optional(), lightmap_pixels_per_m: Number.min(0).max(20).optional(),
    lightmap_error: Index.max(255).optional(), lightmap_error_zero: Index.max(255).optional(), jitter_samples: z.union([z.literal(1),z.literal(4),z.literal(9)]).optional()
  }).strict() }],
  ['scene.object_delete', 'Удалить явный список объектов одним шагом Undo. После операции запросите новые ID.', { ...Mutation, ids: z.array(Id).min(1).max(128) }],
  ['scene.partition_read', 'Прочитать размер, состояние и ячейки World Partition.', { ...Session, ...Page }],
  ['scene.partition_edit', 'Управлять World Partition, состоянием ячейки и фокусом камеры.', { ...Mutation, action: z.enum(['enable','disable','enable_all','set_size','set_cell','focus','set_streaming','load_all']), cell: z.tuple([z.number().int(), z.number().int()]).optional(), disabled: z.boolean().optional(), size_m: Number.min(8).max(100000).optional(), stream_around_camera: z.boolean().optional(), unload_disabled: z.boolean().optional(), stream_radius_m: Number.min(25).max(5000).optional() }],
  ['scene.group_read', 'Прочитать состав группы по ID и опорную точку.', { ...Session, ...Page, object_id: Id }],
  ['scene.group_edit', 'Создать группу, присоединить или отсоединить объекты, разгруппировать либо изменить опорную точку.', { ...Mutation, action: z.enum(['create','attach','detach','ungroup','pivot']), object_id: Id.optional(), ids: z.array(Id).min(1).max(128).optional(), name: Name.optional(), pivot_id: Id.optional() }],
  ['scene.way_read', 'Прочитать точки, флаги и связи одного AI-пути.', { ...Session, ...Page, object_id: Id }],
  ['scene.way_edit', 'Создать AI-путь, изменить его точки или связи. point и target — точные имена существующих точек.', { ...Mutation, action: z.enum(['create','add_point','move_point','remove_point','rename_point','set_flags','link','unlink']), object_id: Id.optional(), name: Name.optional(), point: Id.optional(), target: Id.optional(), position_m: Vec.optional(), bidirectional: z.boolean().optional(), probability: Number.min(0).max(1).optional(), flags: Index.optional() }],
  ['scene.sector_read', 'Прочитать состав сектора или меши статического объекта с их принадлежностью секторам.', { ...Session, ...Page, object_id: Id }],
  ['scene.sector_edit', 'Создать сектор или изменить его состав. Для меша укажите ID статического объекта и имя меша.', { ...Mutation, action: z.enum(['create','add_mesh','remove_mesh','remove']), object_id: Id.optional(), mesh_object_id: Id.optional(), mesh: Resource.optional(), name: Name.optional() }],
  ['scene.portal_read', 'Прочитать геометрию портала и связанные секторы.', { ...Session, object_id: Id }],
  ['scene.portal_edit', 'Создать портал, заменить его геометрию или перевернуть направление.', { ...Mutation, action: z.enum(['create','set_geometry','invert','calculate']), object_id: Id.optional(), front_id: Id.optional(), back_id: Id.optional(), name: Name.optional(), vertices_m: z.array(Vec).min(3).max(6).optional(), replace_existing: z.boolean().optional() }],
  ['scene.entity_get', 'Прочитать параметры объекта сцены или деталей. Для террейна укажите samples. Без ID параметр resource_kind перечисляет частицы или звуковые среды.', { ...Session, ...Page, resource_kind: z.enum(['particles','sound_environments']).optional(), query: z.string().max(128).optional(), object_id: Id.optional(), detail_tool: z.boolean().optional(), detail_model: Resource.optional(), samples: z.array(z.object(Sample).strict()).max(128).optional() }],
  ['scene.entity_set', 'Изменить свойства объекта или деталей. Для объёмов задайте size_m, для сферы radius_m. Присоединение формы перемещает спавн на её позицию. Сначала прочитайте scene_entity_get.', { ...Mutation, object_id: Id.optional(), detail_tool: z.boolean().optional(), detail_model: Resource.optional(), changes: z.object({
    brightness: Number.optional(), range_m: Number.optional(), color_rgba: z.tuple([Number,Number,Number,Number]).optional(), attenuation: Vec.optional(), affect_static: z.boolean().optional(), affect_dynamic: z.boolean().optional(), use_in_d3d: z.boolean().optional(),
    source: Resource.optional(), volume: Number.optional(), frequency: Number.optional(), active_time_s: z.tuple([Number,Number]).optional(), play_time_s: z.tuple([Number,Number]).optional(), pause_time_s: z.tuple([Number,Number]).optional(),
    attach_shape_id: Id.optional(), detach_shape: z.literal(true).optional(), custom_data: Text.optional(), team: Index.optional(), respawn_seconds: Number.optional(), radius_m: Number.optional(), power: Number.optional(), view_distance_m: Number.optional(), fog_density: Number.optional(),
    size_m: Vec.optional(), stiffness: Number.optional(), inner_environment: z.string().max(200).optional(), outer_environment: z.string().max(200).optional(), fog_kind: z.enum(['emitter','occlusion']).optional(), fog_profile: Resource.optional(), particle: Resource.optional(), game_type_flags: Index.optional(),
    heights: z.array(z.object({ ...Sample, height: Number }).strict()).min(1).max(128).optional(), density: Number.optional(), density_factor: Number.optional(), min_scale: Number.optional(), max_scale: Number.optional(), no_waving: z.boolean().optional()
  }).strict() }],
  ['scene.entity_create', 'Разместить объект SDK. Укажите section для спавна, sound для звука, particle для частицы. Для shape можно выбрать box или sphere.', { ...Mutation, kind: z.enum(['spawn','light','sound','terrain','particle','shape','puddle','planar','sound_environment','fog_volume']), section: Id.optional(), sound: Resource.optional(), particle: Resource.optional(), position_m: Vec.optional(), rotation_degrees: Vec.optional(), size_m: Vec.optional(), shape_type: z.enum(['box','sphere']).optional(), radius_m: Number.optional(), name: Name.optional(), width: Index.optional(), height: Index.optional(), fill: Number.optional() }],
  ['scene.spawn_sections', 'Найти реальные классы спавна из конфигурации SDK.', { ...Session, ...Page, query: z.string().max(128).optional() }],
  ['scene.particle_read', 'Прочитать список эффектов и редактируемые поля действий указанного эффекта на вкладке Particles.', { ...Session, ...Page, name: Resource.optional() }],
  ['scene.particle_edit', 'Создать или клонировать эффект, управлять его параметрами и действиями, сохранить, воспроизвести или отменить правку.', { ...Mutation, action: z.enum(['set','save','undo','redo','play','stop','create','clone','add_action','remove_action','set_effect']), name: Resource.optional(), source_name: Resource.optional(), action_index: Index.optional(), action_type: z.string().min(1).max(64).optional(), fields: ChangeFields.optional(), enabled: z.boolean().optional(), max_particles: Index.min(1).max(100000).optional(), time_limit_seconds: Number.min(0).max(3600).optional(), texture: z.string().min(1).max(255).optional(), render_shader: Id.optional() }],
  ['scene.actor_read', 'Прочитать объект на вкладке Actor и его поверхности, кости или анимации с редактируемыми полями.', { ...Session, ...Page, section: z.enum(['surfaces','bones','motions']).optional() }],
  ['scene.actor_edit', 'Загрузить библиотечный объект, поменять поверхность, сохранить отдельную модель или управлять предпросмотром и Undo на вкладке Actor.', { ...Mutation, action: z.enum(['load','set_surface','set_bone','set_motion','save_copy','undo','redo','play','stop']), reference: Resource.optional(), surface_index: Index.optional(), bone_index: Index.optional(), motion_index: Index.optional(), fields: ChangeFields.optional(), texture: z.string().min(1).max(255).optional(), render_shader: Id.optional(), compiler_shader: Id.optional(), game_material: Id.optional() }],
  ['scene.shader_read', 'Прочитать библиотеку шейдеров компилятора и поля указанного шейдера на вкладке Shader.', { ...Session, ...Page, name: z.string().min(1).max(127).optional() }],
  ['scene.shader_edit', 'Изменить и сохранить поля шейдера компилятора или выполнить Undo/Redo.', { ...Mutation, action: z.enum(['set','save','undo','redo']), name: z.string().min(1).max(127).optional(), fields: z.record(z.string().max(128), Scalar).optional() }],
  ['scene.logic_document', 'Открыть LTX логики, прочитать секции, редактировать ключи и сохранить документ в Nodes Logic.', { ...Document, action: z.enum(['read','open','set_key','add_section','remove_key','rename_section','save','reload','undo','redo']), path: Path.optional(), section: Id.optional(), key: Id.optional(), value: Text.optional(), new_name: Id.optional() }],
  ['scene.dialog_document', 'Открыть XML диалога, прочитать реплики, изменить ID текста и сохранить документ в Nodes Dialogs.', { ...Document, action: z.enum(['read','open','set_phrase','create_dialog','add_phrase','remove_phrase','link','unlink','move_phrase','save','reload','undo','redo']), path: Path.optional(), dialog: Id.optional(), phrase: Id.optional(), target: Id.optional(), branch: z.boolean().optional(), value: Text.optional() }],
  ['scene.macro_document', 'Прочитать и редактировать граф макроса SDK и сохранить его в Nodes Macros.', { ...Document, ...Position2D, action: z.enum(['read','add_node','set_args','remove_node','link','unlink','validate','run','save','reload','undo','redo']), command: Id.optional(), first: Text.optional(), second: Text.optional(), node_id: Index.optional(), from: Index.optional(), to: Index.optional() }],
  ['scene.layout_document', 'Создать, прочитать или изменить XML-макет и его виджеты в UI Layout.', { ...Document, ...Position2D, action: z.enum(['read','open','create','add_widget','set_widget','remove_widget','reparent','save','reload','undo','redo']), name: Resource.optional(), type: Id.optional(), widget_id: Index.optional(), parent_id: Index.optional(), width: Number.optional(), height: Number.optional(), text: Text.optional(), texture: Resource.optional(), hidden: z.boolean().optional(), locked: z.boolean().optional() }],
  ['scene.code_document', 'Открыть файл из gamedata/scripts или gamedata/configs в Coder, прочитать, заменить точный фрагмент, проверить и сохранить.', { ...Document, action: z.enum(['read','open','replace','validate','save','reload','undo','redo']), path: Path.optional(), old_text: Text.optional(), new_text: Text.optional() }],
  ['scene.game_material_read', 'Прочитать игровые или общие материалы SDK и параметры указанного материала.', { ...Session, ...Page, kind: z.enum(['game','shared']).optional(), name: Resource.optional() }],
  ['scene.game_material_edit', 'Править и сохранять игровой материал на вкладке Shader. Для правки сначала прочитайте исходные значения.', { ...Mutation, action: z.enum(['set','save']), name: Resource.optional(), values: z.object({
    description: Text.optional(), friction: Number.optional(), damping: Number.optional(), spring: Number.optional(), bounce_start_velocity: Number.optional(),
    bouncing: Number.optional(), bounce_damage: Number.optional(), injurious_speed: Number.optional(), shoot_factor: Number.optional(), shoot_factor_mp: Number.optional(),
    transparency: Number.optional(), sound_occlusion: Number.optional(), flotation: Number.optional(), density: Number.optional(),
    passable: z.boolean().optional(), climbable: z.boolean().optional(), liquid: z.boolean().optional(), actor_obstacle: z.boolean().optional(), no_ricochet: z.boolean().optional()
  }).strict().optional() }],
  ['scene.texture_thm', 'Прочитать штатные параметры THM текстуры по имени без расширения.', { ...Session, texture: Resource, source: z.enum(['game','source']).optional() }],
  ['scene.texture_thm_set', 'Изменить и перечитать штатные параметры THM. Для DDS-конверсии используйте отдельно scene_texture_import.', { ...Mutation, texture: Resource, source: z.enum(['game','source']).optional(), changes: ThmChanges }],
  ['scene.texture_import', 'Импортировать исходную текстуру из rawdata штатным Image Manager SDK.', { ...Mutation, texture: Resource, format: Index.optional() }],
  ['scene.git_read', 'Прочитать статус, историю или ветки Git-проекта SDK.', { ...Session, ...Page, view: z.enum(['status','history','branches']).optional() }],
  ['scene.git_write', 'Обновить состояние Git, добавить или убрать файл из индекса либо создать локальный коммит.', { ...Mutation, action: z.enum(['refresh','stage','unstage','commit']), path: GitPath.optional(), message: Text.optional() }],
  ['scene.dcc_export_obj', 'Экспортировать библиотечный объект SDK в OBJ/MTL в новую папку SDK import для 3ds Max или Blender.', { ...Mutation, model: Resource, destination: Name }],
  ['scene.ai_map_read', 'Прочитать узлы и связи AI Map вместе с map_revision. Номера узлов действительны только для этой ревизии.', { ...Session, ...Page }],
  ['scene.ai_map_edit', 'Изменить узлы и связи AI Map. Сначала прочитайте map_revision и номера узлов через scene_ai_map_read.', { ...Mutation, map_revision: Index, action: z.enum(['set_snap','add_node','remove_nodes','set_height','make_links','remove_links','generate','cleanup']), ids: z.array(Id).min(1).max(128).optional(), nodes: z.array(Index).min(1).max(128).optional(), position_m: Vec.optional(), height_m: Number.optional(), auto_link: z.boolean().optional(), selected_only: z.boolean().optional() }],
  ['scene.plugin_list', 'Прочитать перечень установленных Lua/Python-плагинов SDK и имена их аргументов.', { ...Session, ...Page }],
  ['scene.plugin_run', 'Запустить указанный установленный Lua/Python-плагин SDK с его объявленными аргументами. Для Python проверьте итог через scene_operation_status.', { ...Mutation, plugin_id: Path, arguments: z.record(Id, z.string().max(255)).optional() }],
  ['scene.pack_start', 'Упаковать gamedata или её подпапку через штатный xrCompress. Результат проверяется через scene_pack_status.', { ...Mutation, folder: Resource.optional(), mode: z.enum(['normal','fast','store']).optional() }],
  ['scene.pack_status', 'Прочитать статус xrCompress и перечень созданных пакетов.', {}],
  ['scene.operation_status', 'Проверить уже отправленную операцию по op_id после обрыва связи или таймаута. Не создаёт и не повторяет изменения.', { op_id: z.string().min(8).max(128) }]
];

const MutatingTools = new Set(ToolDefinitions.filter(([method, , shape]) => Object.hasOwn(shape, 'op_id') && method !== 'scene.operation_status').map(([method]) => method));

export function isMutationTool(method) {
  return MutatingTools.has(method);
}
