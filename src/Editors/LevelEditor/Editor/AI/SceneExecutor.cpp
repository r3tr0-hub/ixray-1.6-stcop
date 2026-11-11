#include "stdafx.h"
#include "SceneExecutor.h"
#include "MaterialCommands.h"
#include "AiText.h"
#include "UI/UIAiAssistant.h"
#include "ActorEditor/UIActorEditorForm.h"
#include "ShaderEditor/UIShaderEditorForm.h"
#include "ParticleEditor/UIParticleEditorForm.h"
#include "Nodes/UINodesEditorForm.h"
#include "UI/LayoutEditor/UILayoutEditorForm.h"
#include "UI/WorldPartition/UIWorldPartition.h"

#include <chrono>
#include <cmath>
#include <filesystem>

using AiJson = nlohmann::json;

class AiSceneSnapshot final
{
public:
	AiSceneSnapshot() : Flags(Scene->m_RTFlags)
	{
		Scene->CaptureUndoState(Parts);
	}

	void Restore() const
	{
		Scene->RestoreAssistantState(Parts);
		Scene->m_RTFlags = Flags;
		EContext.UI->UpdateScene();
		EContext.UI->RedrawScene();
	}

	CUndoJournal::State Parts;
	Flags32 Flags;
};

class AiOwnedDirectory final
{
public:
	AiOwnedDirectory() = default;
	~AiOwnedDirectory()
	{
		if (IsOwned)
		{
			std::error_code Code;
			std::filesystem::remove_all(Path, Code);
		}
	}

	bool Create(const std::filesystem::path& Value)
	{
		std::error_code Code;
		Path = Value;
		IsOwned = std::filesystem::create_directory(Path, Code) && !Code;
		return IsOwned;
	}

	void Keep() { IsOwned = false; }
	std::filesystem::path Path;

private:
	bool IsOwned = false;
};

static xr_string AiPathText(const std::filesystem::path& Path)
{
	const auto Text = Path.u8string();
	return reinterpret_cast<const char*>(Text.c_str());
}

static bool AiFileWritten(const std::filesystem::path& Path)
{
	std::error_code Code;
	return std::filesystem::is_regular_file(Path, Code) && !Code && std::filesystem::file_size(Path, Code) > 0 && !Code;
}

static bool AiHasString(const AiJson& Data, const char* Key, size_t Max = 1024)
{
	auto It = Data.find(Key);
	return It != Data.end() && It->is_string() && It->get_ref<const AiJson::string_t&>().size() <= Max;
}

static xr_string AiString(const AiJson& Data, const char* Key, const char* Default = "")
{
	auto It = Data.find(Key);
	return It != Data.end() && It->is_string() ? xr_string(It->get_ref<const AiJson::string_t&>().c_str()) : xr_string(Default);
}

static bool AiContains(const char* Text, const xr_string& Query)
{
	if (Text == nullptr)
	{
		return Query.empty();
	}
	xr_string Haystack = AiText::Utf8(Text);
	xr_string Needle = Query;
	for (xr_string* Value : {&Haystack, &Needle})
	{
		for (char& Character : *Value)
		{
			if (Character >= 'A' && Character <= 'Z')
			{
				Character = char(Character - 'A' + 'a');
			}
			if (Character == '/')
			{
				Character = '\\';
			}
		}
	}
	return Haystack.find(Needle) != xr_string::npos;
}

static bool AiVector(const AiJson& Data, const char* Key, Fvector& Value, float Default, float Bound)
{
	Value.set(Default, Default, Default);
	auto It = Data.find(Key);
	if (It == Data.end())
	{
		return true;
	}
	if (!It->is_array() || It->size() != 3)
	{
		return false;
	}
	for (size_t Index = 0; Index < 3; Index++)
	{
		if (!(*It)[Index].is_number())
		{
			return false;
		}
		double Number = (*It)[Index].get<double>();
		if (!std::isfinite(Number) || std::abs(Number) > Bound)
		{
			return false;
		}
		Value[(int)Index] = (float)Number;
	}
	return true;
}

static AiJson AiVectorJson(const Fvector& Value)
{
	return AiJson::array({Value.x, Value.y, Value.z});
}

static bool AiPage(const AiJson& Params, size_t& Offset, size_t& Limit)
{
	Offset = 0;
	Limit = 100;
	for (const char* Key : {"offset", "limit"})
	{
		auto It = Params.find(Key);
		if (It == Params.end())
		{
			continue;
		}
		if (!It->is_number_integer() || It->get<double>() < 0 || It->get<double>() > 100000000)
		{
			return false;
		}
		if (xr_strcmp(Key, "offset") == 0)
		{
			Offset = It->get<size_t>();
		}
		else
		{
			Limit = It->get<size_t>();
		}
	}
	return Limit > 0 && Limit <= 500;
}

struct AiSdkAction
{
	const char* Name;
	const char* Description;
	u32 Command;
	u32 Argument;
};

static constexpr AiSdkAction AiSdkActions[] = {
	{"library.open", "Open the object library in Actor Editor", COMMAND_LIBRARY_EDITOR, 0},
	{"materials.open", "Open shared materials", COMMAND_SHARED_MATERIALS_EDITOR, 0},
	{"textures.open", "Open Image Editor", COMMAND_IMAGE_EDITOR, 0},
	{"textures.reload", "Reload preview textures", COMMAND_RELOAD_TEXTURES, 0},
	{"sounds.open", "Open Sound Editor", COMMAND_SOUND_EDITOR, 0},
	{"light_animation.open", "Open light animations", COMMAND_LANIM_EDITOR, 0},
	{"minimap.open", "Open Minimap Editor", COMMAND_MINIMAP_EDITOR, 0},
	{"weather.open", "Open lighting and weather properties", COMMAND_WEATHER_PROPERTIES, 0},
	{"scene.options", "Open scene options", COMMAND_OPTIONS, 0},
	{"scene.summary", "Collect scene summary and open its window", COMMAND_SHOW_SCENE_SUMMARY, 0},
	{"scene.select_all", "Select all objects of the active class", COMMAND_SELECT_ALL, 0},
	{"scene.invert_selection", "Invert selection of the active class", COMMAND_INVERT_SELECTION_ALL, 0},
	{"scene.snap_set", "Set the snap list from the current selection", COMMAND_SET_SNAP_OBJECTS, 0},
	{"scene.snap_add", "Append the current selection to the snap list", COMMAND_ADD_SEL_SNAP_OBJECTS, 0},
	{"scene.snap_remove", "Remove the current selection from the snap list", COMMAND_DEL_SEL_SNAP_OBJECTS, 0},
	{"scene.snap_clear", "Clear the snap list", COMMAND_CLEAR_SNAP_OBJECTS, 0},
	{"scene.snap_select", "Select objects from the snap list", COMMAND_SELECT_SNAP_OBJECTS, 0},
	{"build.make_all", "Prepare level outputs. SDK confirmation required", COMMAND_BUILD, 0},
	{"build.make_game", "Export game data. SDK confirmation required", COMMAND_MAKE_GAME, 0},
	{"build.make_details", "Export detail objects. SDK confirmation required", COMMAND_MAKE_DETAILS, 0},
	{"build.make_puddles", "Export puddles. SDK confirmation required", COMMAND_MAKE_PUDDLES, 0},
	{"build.make_planars", "Export planar reflections. SDK confirmation required", COMMAND_MAKE_PLANARS, 0},
	{"build.make_hom", "Export HOM. SDK confirmation required", COMMAND_MAKE_HOM, 0},
	{"build.make_som", "Export SOM. SDK confirmation required", COMMAND_MAKE_SOM, 0},
	{"build.make_ai_map", "Export AI Map. SDK confirmation required", COMMAND_MAKE_AIMAP, 0},
	{"build.make_ai_map_legacy", "Export legacy AI Map. SDK confirmation required", COMMAND_MAKE_AIMAP_LEGACY, 0},
};

static constexpr const char* AiSdkTabs[] = {"actor", "shader", "particles", "logic", "dialogs", "macros", "layout", "level"};
static constexpr const char* AiCompilerModes[] = {"geometry", "details", "ai_high", "ai_low", "ai_verify", "spawn_current", "spawn_all"};

static bool AiIsMutation(const xr_string& Method)
{
	return Method == "scene.select" || Method == "scene.focus" || Method == "scene.transform" || Method == "scene.duplicate" || Method == "scene.place" || Method == "scene.material_set" || Method == "scene.undo" || Method == "scene.redo" || Method == "scene.save_copy" || Method == "scene.sdk_run" || Method == "scene.sdk_tab" || Method == "scene.sdk_compile" || Method == "scene.object_edit";
}

static bool AiIsSceneChange(const xr_string& Method)
{
	return Method == "scene.transform" || Method == "scene.duplicate" || Method == "scene.place" || Method == "scene.material_set" || Method == "scene.object_edit";
}

AiSceneExecutor::AiSceneExecutor()
{
	Generation = (u64)std::chrono::steady_clock::now().time_since_epoch().count();
	Invalidate();
}

void AiSceneExecutor::Invalidate()
{
	Generation++;
	Session = "scene-";
	Session += std::to_string(Generation).c_str();
	ById.clear();
	ByObject.clear();
	KnownEpoch = Scene != nullptr ? Scene->GetAssistantEpoch() : 0;
	KnownRevision = Scene != nullptr ? Scene->GetModifyCount() : 0;
}

void AiSceneExecutor::Synchronize()
{
	if (Scene != nullptr && (KnownEpoch != Scene->GetAssistantEpoch() || KnownRevision != Scene->GetModifyCount()))
	{
		Invalidate();
	}
}

AiSceneExecutor::Json AiSceneExecutor::Error(const char* Code, const char* Message) const
{
	return {{"ok", false}, {"scene_session", Session.c_str()}, {"revision", KnownRevision}, {"error", {{"code", Code}, {"message", Message}}}};
}

xr_vector<CCustomObject*> AiSceneExecutor::Objects() const
{
	xr_vector<CCustomObject*> Result;
	for (auto It = Scene->FirstTool(); It != Scene->LastTool(); ++It)
	{
		if (It->first == OBJCLASS_DUMMY)
		{
			continue;
		}
		ESceneCustomOTool* Tool = smart_cast<ESceneCustomOTool*>(It->second);
		if (Tool == nullptr)
		{
			continue;
		}
		for (CCustomObject* Object : Tool->GetObjects())
		{
			if (Object != nullptr && !Object->IsDeleted())
			{
				Result.push_back(Object);
			}
		}
	}
	return Result;
}

xr_string AiSceneExecutor::Identify(CCustomObject* Object)
{
	auto It = ByObject.find(Object);
	if (It != ByObject.end())
	{
		return It->second;
	}
	xr_string Id = "object-";
	Id += std::to_string(++NextObjectId).c_str();
	ById.emplace(Id, Object);
	ByObject.emplace(Object, Id);
	return Id;
}

CCustomObject* AiSceneExecutor::Find(const xr_string& Id)
{
	auto It = ById.find(Id);
	if (It == ById.end())
	{
		return nullptr;
	}
	const xr_vector<CCustomObject*> Current = Objects();
	return std::find(Current.begin(), Current.end(), It->second) != Current.end() ? It->second : nullptr;
}

AiSceneExecutor::Json AiSceneExecutor::Describe(CCustomObject* Object)
{
	Fvector Rotation = Object->GetRotation();
	Rotation.mul(180.f / PI);
	const char* Reference = Object->RefName();
	return {{"id", Identify(Object).c_str()}, {"name", AiText::Utf8(Object->GetName()).c_str()}, {"class_id", Object->FClassID}, {"class_name", AiText::Utf8(Object->FParentTools->ClassName()).c_str()}, {"reference", AiText::Utf8(Reference).c_str()}, {"position_m", AiVectorJson(Object->GetPosition())}, {"rotation_degrees", AiVectorJson(Rotation)}, {"scale", AiVectorJson(Object->GetScale())}, {"selected", Object->Selected()}, {"visible", Object->Visible()}, {"locked", Object->Locked()}, {"editable", Object->Editable() && !Object->Locked() && !Object->RegionDisabled()}};
}

AiSceneExecutor::Json AiSceneExecutor::Status()
{
	Json Counts = Json::array();
	size_t Selected = 0;
	size_t Count = 0;
	for (auto It = Scene->FirstTool(); It != Scene->LastTool(); ++It)
	{
		if (It->first == OBJCLASS_DUMMY)
		{
			continue;
		}
		ESceneCustomOTool* Tool = smart_cast<ESceneCustomOTool*>(It->second);
		if (Tool == nullptr)
		{
			continue;
		}
		size_t ClassCount = 0;
		for (CCustomObject* Object : Tool->GetObjects())
		{
			if (!Object->IsDeleted())
			{
				ClassCount++;
				Selected += Object->Selected() ? 1 : 0;
			}
		}
		Count += ClassCount;
		Counts.push_back({{"class_id", It->first}, {"class_name", AiText::Utf8(Tool->ClassName()).c_str()}, {"count", ClassCount}});
	}
	return {{"ok", true}, {"path", AiText::Utf8(LTools->m_LastFileName.c_str()).c_str()}, {"scene_name", LTools->m_LastFileName.empty() ? "Новая сцена" : AiText::Utf8(LTools->m_LastFileName.c_str()).c_str()}, {"editor_state", (int)EContext.UI->GetEState()}, {"editable", EContext.UI->GetEState() == esEditScene && !Scene->locked() && !Scene->IsPlayInEditor()}, {"unsaved", Scene->IsUnsaved()}, {"modified", Scene->IsModified()}, {"object_count", Count}, {"selected_count", Selected}, {"classes", Counts}, {"undo_enabled", EPrefs->scene_undo_level != 0}, {"undo_count", Scene->GetUndoCount()}, {"redo_count", Scene->GetRedoCount()}, {"coordinate_system", "left-handed; +Y up; world metres; Euler XYZ degrees; scale dimensionless"}};
}

bool AiSceneExecutor::Resolve(const Json& Params, xr_vector<CCustomObject*>& Result, Json& Failure, bool IsEditable)
{
	if (Params.contains("object_id") && (!AiHasString(Params, "object_id", 128) || AiString(Params, "object_id").empty() || Params.contains("ids")))
	{
		Failure = Error("INVALID_ID", "Supply one nonempty object_id or an ids array, not both");
		return false;
	}
	auto Ids = Params.find("ids");
	if (Ids != Params.end())
	{
		if (!Ids->is_array() || Ids->size() > 128)
		{
			Failure = Error("INVALID_IDS", "ids must be an array of up to 128 object IDs");
			return false;
		}
		for (const Json& Id : *Ids)
		{
			if (!Id.is_string() || Id.get_ref<const Json::string_t&>().size() > 128)
			{
				Failure = Error("INVALID_ID", "Object ID must be a short string");
				return false;
			}
			CCustomObject* Object = Find(Id.get_ref<const Json::string_t&>().c_str());
			if (Object == nullptr)
			{
				Failure = Error("UNKNOWN_OBJECT", "Object ID is unknown or expired; query the current scene again");
				return false;
			}
			if (std::find(Result.begin(), Result.end(), Object) == Result.end())
			{
				Result.push_back(Object);
			}
		}
	}
	else if (AiHasString(Params, "object_id", 128))
	{
		CCustomObject* Object = Find(AiString(Params, "object_id"));
		if (Object == nullptr)
		{
			Failure = Error("UNKNOWN_OBJECT", "Object ID is unknown or expired");
			return false;
		}
		Result.push_back(Object);
	}
	else
	{
		for (CCustomObject* Object : Objects())
		{
			if (Object->Selected() && !Object->m_CO_Flags.test(CCustomObject::flObjectInGroup))
			{
				Result.push_back(Object);
			}
		}
	}
	if (Result.size() > 128)
	{
		Failure = Error("BATCH_TOO_LARGE", "Select at most 128 objects per operation");
		return false;
	}
	if (IsEditable)
	{
		for (CCustomObject* Object : Result)
		{
			if (Object->Locked() || Object->RegionDisabled() || !Object->Editable() || !Object->FParentTools->IsEditable() || !Object->FParentTools->IsEnabled() || Object->m_CO_Flags.test(CCustomObject::flObjectInGroup))
			{
				Failure = Error("OBJECT_NOT_EDITABLE", "A target is locked, disabled, or belongs to a group; target the group itself");
				return false;
			}
		}
	}
	return true;
}

bool AiSceneExecutor::FinishChange(const AiSceneSnapshot& Before, Json& Result)
{
	if (!Scene->CommitAssistantUndo())
	{
		Before.Restore();
		Synchronize();
		Result = Error("NO_CHANGE", "The operation produced no verified Undo step; the snapshot was restored");
		return false;
	}
	KnownRevision = Scene->GetModifyCount();
	KnownEpoch = Scene->GetAssistantEpoch();
	EContext.UI->UpdateScene();
	EContext.UI->RedrawScene();
	Result["ok"] = true;
	Result["undoable"] = true;
	Result["undo_steps"] = 1;
	return true;
}


AiSceneExecutor::Json AiSceneExecutor::CancelOperation(const xr_string& Method, const Json& Params)
{
	if (!Params.is_object() || !AiIsMutation(Method) || !AiHasString(Params, "op_id", 128) || AiString(Params, "op_id").empty())
	{
		return Error("OPERATION_CANCELLED", "The queued read was cancelled before execution");
	}
	const xr_string OpId = AiString(Params, "op_id");
	Json RequestParams = Params;
	RequestParams.erase("deadline_ms");
	const xr_string Request = Json{{"method", Method.c_str()}, {"params", RequestParams}}.dump(-1, ' ', false, Json::error_handler_t::replace).c_str();
	auto Previous = Operations.find(OpId);
	if (Previous != Operations.end())
	{
		if (Previous->second.Request != Request)
		{
			return Error("OP_ID_COLLISION", "This operation ID was used with different parameters");
		}
		return Previous->second.Result;
	}
	Json Result = Error("OPERATION_CANCELLED", "The command was cancelled before execution; no change was made");
	Result["status"] = "cancelled";
	Result["op_id"] = OpId.c_str();
	Operations.emplace(OpId, Operation{Request, Result});
	return Result;
}

AiSceneExecutor::Json AiSceneExecutor::Execute(const xr_string& Method, const Json& Params)
{
	if (!Params.is_object())
	{
		return Error("INVALID_PARAMS", "Parameters must be a JSON object");
	}
	const bool IsMutation = AiIsMutation(Method);
	const xr_string OpId = AiString(Params, "op_id");
	Json RequestParams = Params;
	RequestParams.erase("deadline_ms");
	const xr_string Request = (Json{{"method", Method.c_str()}, {"params", RequestParams}}.dump(-1, ' ', false, Json::error_handler_t::replace)).c_str();
	if (IsMutation)
	{
		if (!AiHasString(Params, "op_id", 128) || OpId.empty())
		{
			return Error("OP_ID_REQUIRED", "A unique op_id is required for every operation");
		}
		auto Previous = Operations.find(OpId);
		if (Previous != Operations.end())
		{
			if (Previous->second.Request != Request)
			{
				return Error("OP_ID_COLLISION", "This operation ID was already used with different parameters");
			}
			Json Result = Previous->second.Result;
			Result["replayed"] = true;
			return Result;
		}
		if (Operations.size() >= 10000)
		{
			return Error("OPERATION_LIMIT", "Restart the assistant before starting more operations; previous results are retained until restart");
		}
	}
	if (Method == "scene.operation_status")
	{
		auto It = Operations.find(OpId);
		return It == Operations.end() ? Json{{"ok", true}, {"status", "unknown"}} : Json{{"ok", true}, {"status", It->second.Result.value("status", "completed")}, {"result", It->second.Result}};
	}
	if (Scene == nullptr || EContext.UI == nullptr || !Scene->valid())
	{
		return Error("EDITOR_UNAVAILABLE", "The scene is not initialized");
	}
	if (Method == "scene.sdk_status")
	{
		return {{"ok", true}, {"active_tab", EContext.UI->ActiveTabIndex}, {"loader_busy", LUI->LoaderEvent.IsBusy()}, {"compiler_running", LTools->IsCompilerRunning()}, {"game_running", LTools->IsGameRunning()}, {"play_in_editor", Scene->IsPlayInEditor()}};
	}
	if (LUI->LoaderEvent.IsBusy() || Scene->locked())
	{
		return Error("EDITOR_BUSY", "The editor is loading, saving, or holding a scene lock; retry after it becomes idle");
	}
	Synchronize();
	if (Params.contains("scene_session") && (!AiHasString(Params, "scene_session", 128) || AiString(Params, "scene_session") != Session))
	{
		return Error("STALE_SCENE", "The scene has changed; query status and object IDs again");
	}
	if (IsMutation && !AiHasString(Params, "scene_session", 128))
	{
		return Error("SCENE_SESSION_REQUIRED", "Query scene.status and include scene_session before changing the editor");
	}
	const bool StopPlay = Method == "scene.sdk_run" && AiString(Params, "action") == "play.stop";
	if (IsMutation && !StopPlay && (EContext.UI->GetEState() != esEditScene || (EContext.UI->ActiveTabIndex != 0 && Method != "scene.sdk_tab") || EContext.UI->IsMouseCaptured() || !EContext.UI->IsEnableInput || Scene->IsPlayInEditor()))
	{
		return Error("EDITOR_NOT_EDITABLE", "Open the scene tab and finish the current editing operation");
	}
	if (AiIsSceneChange(Method) && !Scene->EnsureAssistantUndoBaseline())
	{
		return Error("UNDO_DISABLED", "Enable scene Undo before requesting changes");
	}
	if (Params.contains("deadline_ms"))
	{
		if (!Params["deadline_ms"].is_number() || !std::isfinite(Params["deadline_ms"].get<double>()) || Params["deadline_ms"].get<double>() < 0 || Params["deadline_ms"].get<double>() > 9000000000000000.)
		{
			return Error("INVALID_DEADLINE", "deadline_ms must contain a finite Unix timestamp in milliseconds");
		}
		const double Now = (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		if (Now >= Params["deadline_ms"].get<double>())
		{
			Json Expired = Error("OPERATION_CANCELLED", "The command expired before execution; no change was made");
			Expired["status"] = "cancelled";
			Expired["op_id"] = OpId.c_str();
			if (IsMutation)
			{
				Operations.emplace(OpId, Operation{Request, Expired});
			}
			return Expired;
		}
	}
	EditableObjectReadOnlyScope ReadOnlyLoad;
	Json Result = Dispatch(Method, Params);
	Result["scene_session"] = Session.c_str();
	Result["revision"] = Scene->GetModifyCount();
	if (IsMutation)
	{
		Result["op_id"] = OpId.c_str();
		Operations.emplace(OpId, Operation{Request, Result});
	}
	return Result;
}

AiSceneExecutor::Json AiSceneExecutor::Dispatch(const xr_string& Method, const Json& Params)
{
	if (Method == "scene.sdk_catalog")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit) || (Params.contains("query") && !AiHasString(Params, "query", 128)))
		{
			return Error("INVALID_FILTER", "Provide a short query and valid pagination");
		}
		const xr_string Query = AiString(Params, "query");
		Json Entries = Json::array();
		size_t Matches = 0;
		auto Add = [&](const char* Name, const char* Description, const char* Tool, const char* Action)
		{
			if (!AiContains(Name, Query) && !AiContains(Description, Query))
			{
				return;
			}
			if (Matches++ >= Offset && Entries.size() < Limit)
			{
				Entries.push_back({{"name", Name}, {"description", Description}, {"tool", Tool}, {"action", Action}});
			}
		};
		for (const AiSdkAction& Action : AiSdkActions)
		{
			Add(Action.Name, Action.Description, "scene_sdk_run", Action.Name);
		}
		for (const char* Tab : AiSdkTabs)
		{
			Add(Tab, "Switch editor tab", "scene_sdk_tab", Tab);
		}
		for (const char* Mode : AiCompilerModes)
		{
			Add(Mode, "Run the SDK-configured compiler", "scene_sdk_compile", Mode);
		}
		Add("scene.validate", "Validate the level using the SDK and report its result", "scene_sdk_run", "scene.validate");
		Add("partition.open", "Open World Partition", "scene_sdk_run", "partition.open");
		Add("play.start", "Start Play in Editor", "scene_sdk_run", "play.start");
		Add("play.stop", "Stop Play in Editor", "scene_sdk_run", "play.stop");
		if (!Params.contains("include_native") || Params["include_native"] == true)
		{
			for (SECommand* Command : GetEditorCommands())
			{
				if (Command != nullptr)
				{
					Add(Command->Name(), AiText::Utf8(Command->Desc()).c_str(), "", "");
				}
			}
		}
		const size_t Next = std::min(Offset, Matches) + Entries.size();
		return {{"ok", true}, {"entries", Entries}, {"total", Matches}, {"next_offset", Next}, {"has_more", Next < Matches}};
	}
	if (Method == "scene.sdk_assets")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit) || (Params.contains("query") && !AiHasString(Params, "query", 128)))
		{
			return Error("INVALID_FILTER", "Provide a short query and valid pagination");
		}
		const xr_string Kind = AiString(Params, "kind");
		const char* Alias = nullptr;
		const char* Mask = nullptr;
		if (Kind == "objects")
		{
			Alias = _objects_; Mask = "*.object";
		}
		else if (Kind == "textures")
		{
			Alias = _game_textures_; Mask = "*.dds";
		}
		else if (Kind == "texture_sources")
		{
			Alias = _textures_; Mask = "*.tga";
		}
		else if (Kind == "texture_metadata")
		{
			Alias = _textures_; Mask = "*.thm";
		}
		else if (Kind == "sounds")
		{
			Alias = _game_sounds_; Mask = "*.ogg";
		}
		else if (Kind == "levels")
		{
			Alias = _maps_; Mask = "*.level";
		}
		if (Alias == nullptr || !FS.path_exist(Alias))
		{
			return Error("ASSET_ROOT_UNAVAILABLE", "The requested SDK asset root is unavailable");
		}
		const xr_string Query = AiString(Params, "query");
		FS_FileSet Files;
		FS.file_list(Files, Alias, FS_ListFiles, Mask);
		Json Items = Json::array();
		size_t Matches = 0;
		for (const FS_File& File : Files)
		{
			if (AiContains(File.name.c_str(), Query) && Matches++ >= Offset && Items.size() < Limit)
			{
				Items.push_back(AiText::Utf8(File.name.c_str()).c_str());
			}
		}
		const size_t Next = std::min(Offset, Matches) + Items.size();
		return {{"ok", true}, {"kind", Kind.c_str()}, {"items", Items}, {"total", Matches}, {"next_offset", Next}, {"has_more", Next < Matches}};
	}
	if (Method == "scene.sdk_model")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit) || !AiHasString(Params, "reference", 200))
		{
			return Error("INVALID_PARAMS", "Provide a library .object reference and valid pagination");
		}
		const xr_string Reference = AiString(Params, "reference");
		const auto RelativePath = std::filesystem::u8path(Reference.c_str());
		if (Reference.empty() || Reference.find("..") != xr_string::npos || Reference.find(':') != xr_string::npos || RelativePath.has_root_path())
		{
			return Error("INVALID_REFERENCE", "Use a reference relative to the SDK object library");
		}
		string_path ModelPath;
		FS.update_path(ModelPath, _objects_, EFS.ChangeFileExt(Reference, ".object").c_str());
		if (!FS.exist(ModelPath))
		{
			return Error("MODEL_MISSING", "The library .object does not exist");
		}
		auto Release = [](CEditableObject* Object) { Lib.RemoveEditObject(Object); };
		std::unique_ptr<CEditableObject, decltype(Release)> Model(Lib.CreateEditObject(ModelPath), Release);
		if (!Model)
		{
			return Error("MODEL_LOAD_FAILED", "The SDK could not read this model");
		}
		auto ReadName = [](const char* Name)
		{
			return AiText::Utf8(Name != nullptr ? Name : "");
		};
		const size_t Total = Model->m_Surfaces.size();
		size_t Cursor = std::min(Offset, Total);
		Json Items = Json::array();
		while (Cursor < Total && Items.size() < Limit)
		{
			const CSurface* Surface = Model->m_Surfaces[Cursor++];
			Items.push_back({{"index", Cursor - 1}, {"name", ReadName(Surface->_Name()).c_str()}, {"texture", ReadName(Surface->_Texture()).c_str()}, {"render_shader", ReadName(Surface->_ShaderName()).c_str()}, {"compiler_shader", ReadName(Surface->_ShaderXRLCName()).c_str()}, {"game_material", ReadName(Surface->_GameMtlName()).c_str()}});
		}
		return {{"ok", true}, {"reference", Reference.c_str()}, {"faces", Model->GetFaceCount(false, false)}, {"vertices", Model->GetVertexCount()}, {"surfaces", Items}, {"total", Total}, {"next_offset", Cursor}, {"has_more", Cursor < Total}};
	}
	if (Method == "scene.object_properties")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit) || !AiHasString(Params, "object_id", 128) || AiString(Params, "object_id").empty())
		{
			return Error("INVALID_PARAMS", "Provide one object ID and valid pagination");
		}
		xr_vector<CCustomObject*> Targets;
		Json Failure;
		if (!Resolve(Json{{"object_id", Params["object_id"]}}, Targets, Failure, false))
		{
			return Failure;
		}
		struct PropertyItems
		{
			PropItemVec Items;
			~PropertyItems()
			{
				for (PropItem*& Item : Items)
				{
					xr_delete(Item);
				}
			}
		} Properties;
		CCustomObject* Object = Targets.front();
		Object->FillProp("", Properties.Items);
		Json Items = Json::array();
		size_t Cursor = std::min(Offset, Properties.Items.size());
		while (Cursor < Properties.Items.size() && Items.size() < Limit)
		{
			PropItem* Item = Properties.Items[Cursor++];
			Items.push_back({{"key", AiText::Utf8(Item->Key()).c_str()}, {"type_id", Item->Type()}, {"enabled", Item->Enabled()}});
		}
		return {{"ok", true}, {"object", Describe(Object)}, {"properties", Items}, {"total", Properties.Items.size()}, {"next_offset", Cursor}, {"has_more", Cursor < Properties.Items.size()}};
	}
	if (Method == "scene.sdk_tab")
	{
		const xr_string Tab = AiString(Params, "tab");
		if (Tab == "level")
		{
			EContext.UI->ActiveTabIndex = 0;
		}
		else if (Tab == "actor")
		{
			UIActorEditorForm::Open();
		}
		else if (Tab == "shader")
		{
			UIShaderEditorForm::Open();
		}
		else if (Tab == "particles")
		{
			UIParticleEditorForm::Open();
		}
		else if (Tab == "logic")
		{
			UINodesEditorForm::Activate(ENodesMode::Logic);
		}
		else if (Tab == "dialogs")
		{
			UINodesEditorForm::Activate(ENodesMode::Dialogs);
		}
		else if (Tab == "macros")
		{
			UINodesEditorForm::Activate(ENodesMode::Macros);
		}
		else if (Tab == "layout")
		{
			if (!UILayoutEditorForm::Open())
			{
				return Error("TAB_UNAVAILABLE", "The SDK could not open UI Layout Editor");
			}
		}
		else
		{
			return Error("UNKNOWN_TAB", "Use a tab from scene_sdk_catalog");
		}
		return {{"ok", true}, {"requested_tab", Tab.c_str()}, {"active_tab", EContext.UI->ActiveTabIndex}};
	}
	if (Method == "scene.sdk_run")
	{
		const xr_string Name = AiString(Params, "action");
		if (Name == "scene.validate")
		{
			const bool Valid = Scene->Validate(false, true, true, true, true, true);
			return {{"ok", true}, {"valid", Valid}};
		}
		if (Name == "partition.open")
		{
			UIWorldPartition::Show();
			return {{"ok", UIWorldPartition::IsOpen()}};
		}
		if (Name == "play.start")
		{
			if (LTools->IsCompilerRunning() || LTools->IsGameRunning())
			{
				return Error("SDK_BUSY", "Wait for the running compiler or game");
			}
			Scene->Play();
			if (!Scene->IsPlayInEditor())
			{
				return Error("PLAY_START_FAILED", "Play in Editor could not start. Check the SDK log");
			}
			return {{"ok", true}, {"play_in_editor", true}};
		}
		if (Name == "play.stop")
		{
			if (!Scene->IsPlayInEditor())
			{
				return {{"ok", true}, {"play_in_editor", false}};
			}
			Scene->Stop();
			return {{"ok", true}, {"status", "accepted"}, {"completion_verified", false}};
		}
		for (const AiSdkAction& Action : AiSdkActions)
		{
			if (Name != Action.Name)
			{
				continue;
			}
			if (Name.find("build.") == 0 && (LTools->IsCompilerRunning() || LTools->IsGameRunning()))
			{
				return Error("SDK_BUSY", "Wait for the running compiler or game");
			}
			if (Name == "scene.summary" && !u32(ExecCommand(COMMAND_COLLECT_SCENE_SUMMARY)))
			{
				return Error("SDK_COMMAND_FAILED", "The SDK could not collect the scene summary");
			}
			CCommandVar Result = ExecCommand(Action.Command, Action.Argument);
			if (!Result.IsInteger() || !u32(Result))
			{
				return Error("SDK_COMMAND_FAILED", "The SDK declined or failed the command. Check the editor log");
			}
			return {{"ok", true}, {"action", Action.Name}, {"status", LUI->LoaderEvent.IsBusy() ? "running" : "accepted"}, {"completion_verified", false}};
		}
		return Error("UNKNOWN_SDK_ACTION", "This command has no MCP adapter");
	}
	if (Method == "scene.sdk_compile")
	{
		if (LTools->IsCompilerRunning() || LTools->IsGameRunning())
		{
			return Error("SDK_BUSY", "Wait for the running compiler or game");
		}
		if (LTools->m_LastFileName.empty() || Scene->IsUnsaved() || Scene->IsModified())
		{
			return Error("SAVE_REQUIRED", "Save the level before running its configured compiler");
		}
		const xr_string Mode = AiString(Params, "mode");
		if (Mode == "geometry")
		{
			LTools->RunXrLC();
		}
		else if (Mode == "details")
		{
			LTools->RunXrDO();
		}
		else if (Mode == "ai_high")
		{
			LTools->RunXrAI_AIMap(false);
		}
		else if (Mode == "ai_low")
		{
			LTools->RunXrAI_AIMap(true);
		}
		else if (Mode == "ai_verify")
		{
			LTools->RunXrAI_Verify();
		}
		else if (Mode == "spawn_current")
		{
			LTools->RunXrAI_Spawn(true);
		}
		else if (Mode == "spawn_all")
		{
			LTools->RunXrAI_Spawn(false);
		}
		else
		{
			return Error("UNKNOWN_COMPILER_MODE", "Use a compiler mode from scene_sdk_catalog");
		}
		if (!LTools->IsCompilerRunning())
		{
			return Error("COMPILER_START_FAILED", "The configured compiler did not start. Check the SDK compiler paths and log");
		}
		return {{"ok", true}, {"mode", Mode.c_str()}, {"status", "running"}, {"completion_verified", false}};
	}
	if (Method == "scene.status")
	{
		return Status();
	}
	if (Method == "scene.viewport")
	{
		return AiCaptureViewport(Params);
	}
	if (Method == "scene.search" || Method == "scene.selection")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit))
		{
			return Error("INVALID_PAGE", "offset must be non-negative and limit must be in 1..500");
		}
		for (const char* Key : {"name", "reference", "texture"})
		{
			if (Params.contains(Key) && !AiHasString(Params, Key, 255))
			{
				return Error("INVALID_FILTER", "Search filters must be strings of at most 255 characters");
			}
		}
		if (Params.contains("class_id") && (!Params["class_id"].is_number_integer() || Params["class_id"].get<double>() < 0 || Params["class_id"].get<double>() >= OBJCLASS_COUNT))
		{
			return Error("INVALID_CLASS", "class_id is outside the editor class range");
		}
		const xr_string Name = AiString(Params, "name");
		const xr_string Reference = AiString(Params, "reference");
		const xr_string Texture = AiString(Params, "texture");
		const xr_vector<CCustomObject*> Current = Objects();
		Json Items = Json::array();
		size_t Cursor = std::min(Offset, Current.size());
		size_t Scanned = 0;
		while (Cursor < Current.size() && Items.size() < Limit && Scanned < 2000 && (Texture.empty() || Scanned < 32))
		{
			CCustomObject* Object = Current[Cursor++];
			Scanned++;
			if ((Method == "scene.selection" && !Object->Selected()) || (!Name.empty() && !AiContains(Object->GetName(), Name)))
			{
				continue;
			}
			if (Params.contains("class_id") && Object->FClassID != Params["class_id"].get<u32>())
			{
				continue;
			}
			const char* Ref = Object->RefName();
			if (!Reference.empty() && !AiContains(Ref, Reference))
			{
				continue;
			}
			if (!Texture.empty())
			{
				CSceneObject* StaticObject = smart_cast<CSceneObject*>(Object);
				if (StaticObject == nullptr)
				{
					continue;
				}
				Json Materials = AiMaterials::Read(*StaticObject);
				bool HasTexture = false;
				if (Materials.contains("surfaces"))
				{
					for (const Json& Surface : Materials["surfaces"])
					{
						if (AiContains(AiString(Surface, "texture").c_str(), Texture))
						{
							HasTexture = true;
						}
					}
				}
				if (!HasTexture)
				{
					continue;
				}
			}
			Items.push_back(Describe(Object));
		}
		return {{"ok", true}, {"objects", Items}, {"offset", Offset}, {"next_offset", Cursor}, {"has_more", Cursor < Current.size()}, {"scanned", Scanned}, {"scene_object_count", Current.size()}, {"pagination", "offset is the scene scan cursor; continue next_offset until has_more=false"}};
	}

	if (Method == "scene.undo" || Method == "scene.redo")
	{
		if (EPrefs->scene_undo_level == 0)
		{
			return Error("UNDO_DISABLED", "Scene Undo is disabled");
		}
		const bool IsDone = Method == "scene.undo" ? Scene->Undo() : Scene->Redo();
		Synchronize();
		if (!IsDone)
		{
			return Error("HISTORY_EMPTY", "There is no completed change to undo or redo");
		}
		return {{"ok", true}, {"undo_count", Scene->GetUndoCount()}, {"redo_count", Scene->GetRedoCount()}, {"object_ids_invalidated", true}};
	}

	if (Method == "scene.save_copy")
	{
		if (!AiHasString(Params, "path", 240) || AiString(Params, "path").empty())
		{
			return Error("INVALID_PATH", "Specify an absolute path for a new .level copy (up to 240 UTF-8 bytes)");
		}
		for (auto It = Scene->FirstTool(); It != Scene->LastTool(); ++It)
		{
			ESceneToolBase* Tool = It->second;
			if (It->first != OBJCLASS_DUMMY && Tool != nullptr && Tool->IsNeedSave() && (!Tool->IsEnabled() || !Tool->IsEditable()))
			{
				return Error("COPY_HAS_UNAVAILABLE_TOOLS", "Enable and unlock all scene tools before saving a complete copy");
			}
		}
		const xr_string Name = AiString(Params, "path");
		const std::filesystem::path Target = std::filesystem::u8path(Name.c_str());
		std::error_code Code;
		if (!Target.is_absolute() || Target.extension() != ".level" || !std::filesystem::is_directory(Target.parent_path(), Code) || Code)
		{
			return Error("INVALID_PATH", "The copy needs an absolute .level path in an existing directory");
		}
		const std::filesystem::path Parts = Target.parent_path() / Target.stem();
		if (std::filesystem::exists(Target, Code) || std::filesystem::exists(Parts, Code) || Code)
		{
			return Error("COPY_EXISTS", "The destination scene or its part directory already exists; choose a new name");
		}
		AiOwnedDirectory Staging;
		xr_string StageLeaf = ".ixray-ai-copy-";
		StageLeaf += std::to_string(Generation).c_str();
		StageLeaf += "-";
		StageLeaf += std::to_string(Operations.size()).c_str();
		if (!Staging.Create(Target.parent_path() / StageLeaf.c_str()))
		{
			return Error("WRITE_DENIED", "A private staging directory could not be created beside the destination");
		}
		const std::filesystem::path StageMain = Staging.Path / Target.filename();
		const std::filesystem::path StageParts = Staging.Path / Target.stem();
		const xr_string StageName = AiPathText(StageMain);
		if (StageName.size() >= 255)
		{
			return Error("PATH_TOO_LONG", "Choose a shorter destination path so the SDK can stage the scene copy");
		}
		const Flags32 BeforeFlags = Scene->m_RTFlags;
		Scene->SaveLTX(StageName.c_str(), false, true, false);
		Scene->m_RTFlags = BeforeFlags;
		if (!AiFileWritten(StageMain) || !std::filesystem::is_directory(StageParts, Code) || Code)
		{
			return Error("SAVE_FAILED", "The staged scene was not created; inspect the editor log");
		}
		Json PartFiles = Json::array();
		for (auto It = Scene->FirstTool(); It != Scene->LastTool(); ++It)
		{
			ESceneToolBase* Tool = It->second;
			if (!EScene::IsSavedTool(It->first, Tool))
			{
				continue;
			}
			const int FileCount = Tool->can_use_inifile() ? Tool->SaveFileCount() : 1;
			for (int Index = 0; Index < FileCount; Index++)
			{
				xr_string PartName = Tool->ClassName();
				if (PartName == "ai_map")
				{
					PartName = "ai_map_ixray";
				}
				PartName += ".part";
				if (Index != 0)
				{
					PartName += std::to_string(Index).c_str();
				}
				if (!AiFileWritten(StageParts / PartName.c_str()))
				{
					return Error("SAVE_PART_FAILED", "A scene part was not written; the destination was left unchanged");
				}
				PartFiles.push_back(PartName.c_str());
			}
		}
		AiOwnedDirectory FinalParts;
		if (!FinalParts.Create(Parts))
		{
			return Error("COPY_EXISTS", "The destination part directory appeared during staging; no existing files were changed");
		}
		std::filesystem::copy(StageParts, Parts, std::filesystem::copy_options::recursive, Code);
		if (Code || !std::filesystem::copy_file(StageMain, Target, std::filesystem::copy_options::none, Code) || Code)
		{
			return Error("COPY_COMMIT_FAILED", "The copy could not be committed without overwriting; no existing scene was replaced");
		}
		FinalParts.Keep();
		return {{"ok", true}, {"path", Name.c_str()}, {"parts_path", AiPathText(Parts).c_str()}, {"verified_file", true}, {"verified_parts", PartFiles}, {"original_scene_kept", true}};
	}

	if (Method == "scene.place")
	{
		if (!AiHasString(Params, "reference", 200))
		{
			return Error("INVALID_REFERENCE", "Specify a library model reference");
		}
		xr_string Reference = AiString(Params, "reference");
		if (Reference.empty() || Reference.find("..") != xr_string::npos || Reference.find(':') != xr_string::npos || Reference.front() == '/' || Reference.front() == '\\')
		{
			return Error("INVALID_REFERENCE", "The model reference must be relative to the SDK object library");
		}
		Fvector Position, Rotation, Scale;
		if (!AiVector(Params, "position_m", Position, 0, 1000000) || !AiVector(Params, "rotation_degrees", Rotation, 0, 360000) || !AiVector(Params, "scale", Scale, 1, 10000) || Scale.x < 0.0001f || Scale.y < 0.0001f || Scale.z < 0.0001f)
		{
			return Error("INVALID_TRANSFORM", "Model transform must contain finite three-component vectors and positive scale");
		}
		ESceneCustomOTool* Tool = Scene->GetOTool(OBJCLASS_SCENEOBJECT);
		if (Tool == nullptr || !Tool->IsEnabled() || !Tool->IsEditable())
		{
			return Error("TOOL_NOT_EDITABLE", "The scene object tool is disabled or locked");
		}
		string_path ModelPath;
		FS.update_path(ModelPath, _objects_, EFS.ChangeFileExt(Reference, ".object").c_str());
		if (!FS.exist(ModelPath))
		{
			return Error("MODEL_MISSING", "The library .object does not exist");
		}
		AiSceneSnapshot Before;
		string256 Name;
		Scene->GenObjectName(OBJCLASS_SCENEOBJECT, Name, "ai_object");
		CSceneObject* Object = new CSceneObject(nullptr, Name);
		if (Object->SetReference(Reference.c_str()) == nullptr)
		{
			xr_delete(Object);
			return Error("MODEL_LOAD_FAILED", "The library model could not be loaded");
		}
		if (Object->IsDynamic() && (Scale.x != 1 || Scale.y != 1 || Scale.z != 1))
		{
			xr_delete(Object);
			return Error("DYNAMIC_SCALE", "Dynamic models cannot be scaled in this SDK");
		}
		Rotation.mul(PI / 180.f);
		Object->SetPosition(Position);
		Object->SetRotation(Rotation);
		Object->SetScale(Scale);
		Scene->AppendObject(Object, false);
		Object->Select(true);
		Json Result = {{"created", Json::array({Describe(Object)})}};
		FinishChange(Before, Result);
		return Result;
	}

	if (Method == "scene.material_set")
	{
		auto Changes = Params.find("changes");
		if (Changes == Params.end() || !Changes->is_array() || Changes->empty() || Changes->size() > 128)
		{
			return Error("INVALID_CHANGES", "changes must contain 1..128 explicit object/surface changes");
		}
		xr_vector<CSceneObject*> Targets;
		xr_set<xr_string> Seen;
		for (const Json& Change : *Changes)
		{
			if (!Change.is_object() || !AiHasString(Change, "object_id", 128) || AiString(Change, "object_id").empty() || Change.contains("ids"))
			{
				return Error("INVALID_CHANGE", "Each surface change needs an object_id");
			}
			xr_vector<CCustomObject*> ObjectsToChange;
			Json Failure;
			if (!Resolve(Change, ObjectsToChange, Failure, true))
			{
				return Failure;
			}
			if (ObjectsToChange.size() != 1)
			{
				return Error("INVALID_CHANGE", "Each surface change must resolve to exactly one instance");
			}
			CSceneObject* Object = smart_cast<CSceneObject*>(ObjectsToChange.front());
			if (Object == nullptr)
			{
				return Error("NOT_SCENE_OBJECT", "Material changes require a scene object instance");
			}
			Json Validation = AiMaterials::ValidatePatch(*Object, Change);
			if (!Validation.value("ok", false))
			{
				return Validation;
			}
			xr_string Key = AiString(Change, "object_id") + ":" + Change["surface_index"].dump().c_str();
			if (!Seen.insert(Key).second)
			{
				return Error("DUPLICATE_SURFACE", "A batch may specify each instance surface only once");
			}
			Targets.push_back(Object);
		}
		AiSceneSnapshot Before;
		Json Applied = Json::array();
		for (size_t Index = 0; Index < Targets.size(); Index++)
		{
			Scene->BeforeObjectChange(Targets[Index]);
			Json Change = AiMaterials::ApplyPatch(*Targets[Index], (*Changes)[Index]);
			if (!Change.value("ok", false))
			{
				Before.Restore();
				Synchronize();
				Change["rolled_back"] = true;
				return Change;
			}
			Change["object_id"] = Identify(Targets[Index]).c_str();
			Applied.push_back(Change);
		}
		Json Result = {{"changes", Applied}, {"library_objects_modified", false}};
		bool HasChange = false;
		for (const Json& Change : Applied)
		{
			HasChange |= !Change["changed_surfaces"].empty();
		}
		if (!HasChange)
		{
			Result["ok"] = true;
			Result["unchanged"] = true;
			Result["undo_steps"] = 0;
			return Result;
		}
		FinishChange(Before, Result);
		return Result;
	}

	if (Method == "scene.object_edit")
	{
		if (!AiHasString(Params, "object_id", 128) || AiString(Params, "object_id").empty() || Params.contains("ids"))
		{
			return Error("INVALID_ID", "Provide one object ID");
		}
		const bool Rename = Params.contains("name");
		const bool Visibility = Params.contains("visible");
		if ((!Rename && !Visibility) || (Visibility && !Params["visible"].is_boolean()))
		{
			return Error("INVALID_PARAMS", "Provide a new name or a boolean visible flag");
		}
		const xr_string Name = AiString(Params, "name");
		if (Rename && (Name.empty() || Name.size() > 128 || Name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != xr_string::npos))
		{
			return Error("INVALID_NAME", "Use 1 to 128 lowercase ASCII letters, digits, hyphens, or underscores");
		}
		xr_vector<CCustomObject*> EditTargets;
		Json EditFailure;
		if (!Resolve(Params, EditTargets, EditFailure, true))
		{
			return EditFailure;
		}
		CCustomObject* Object = EditTargets.front();
		if (Rename)
		{
			for (CCustomObject* Other : Objects())
			{
				if (Other != Object && Name == Other->GetName())
				{
					return Error("DUPLICATE_NAME", "An object already uses this name");
				}
			}
		}
		const bool NewVisibility = Visibility ? Params["visible"].get<bool>() : Object->Visible();
		if ((!Rename || Name == Object->GetName()) && NewVisibility == Object->Visible())
		{
			return {{"ok", true}, {"unchanged", true}, {"object", Describe(Object)}};
		}
		AiSceneSnapshot Before;
		Scene->BeforeObjectChange(Object);
		if (Rename)
		{
			Object->SetName(Name.c_str());
		}
		if (Visibility)
		{
			Object->Show(NewVisibility);
		}
		Json Result = {{"object", Describe(Object)}};
		FinishChange(Before, Result);
		return Result;
	}

	xr_vector<CCustomObject*> Targets;
	Json Failure;
	if (Method != "scene.select" && Method != "scene.focus" && Method != "scene.transform" && Method != "scene.duplicate" && Method != "scene.materials" && Method != "scene.missing_textures")
	{
		return Error("UNKNOWN_METHOD", "This editor command is not supported");
	}
	if (!Resolve(Params, Targets, Failure, AiIsSceneChange(Method)))
	{
		return Failure;
	}
	if (Targets.empty() && Method != "scene.select")
	{
		return Error("EMPTY_SELECTION", "No target objects were supplied or selected");
	}
	if (Method == "scene.select")
	{
		if (Params.contains("clear") && !Params["clear"].is_boolean())
		{
			return Error("INVALID_CLEAR", "clear must be a boolean");
		}
		if (!Params.contains("clear") || Params["clear"].get<bool>())
		{
			Scene->SelectObjects(false);
		}
		for (CCustomObject* Object : Targets)
		{
			Object->Select(true);
		}
		EContext.UI->UpdateScene();
		EContext.UI->RedrawScene();
		Json Result = Status();
		Result["selected"] = Json::array();
		for (CCustomObject* Object : Targets)
		{
			if (Object->Selected())
			{
				Result["selected"].push_back(Describe(Object));
			}
		}
		return Result;
	}
	if (Method == "scene.focus")
	{
		Fbox Box;
		Box.invalidate();
		for (CCustomObject* Object : Targets)
		{
			Fbox ObjectBox;
			if (Object->GetBox(ObjectBox) && ObjectBox.is_valid())
			{
				Box.merge(ObjectBox);
			}
		}
		if (!Box.is_valid())
		{
			return Error("NO_BOUNDS", "The target objects have no valid bounding box");
		}
		EContext.UI->CurrentView().m_Camera.ZoomExtents(Box);
		EContext.UI->RedrawScene();
		return {{"ok", true}, {"focused_count", Targets.size()}};
	}
	if (Method == "scene.materials" || Method == "scene.missing_textures")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit))
		{
			return Error("INVALID_PAGE", "Invalid material diagnostic page");
		}
		Json Items = Json::array();
		for (CCustomObject* Object : Targets)
		{
			CSceneObject* StaticObject = smart_cast<CSceneObject*>(Object);
			if (StaticObject == nullptr)
			{
				return Error("NOT_SCENE_OBJECT", "Surface diagnostics require scene object instances");
			}
			Json Item = Method == "scene.materials" ? AiMaterials::Read(*StaticObject) : AiMaterials::MissingTextures(*StaticObject, Offset, Limit);
			Item["object_id"] = Identify(Object).c_str();
			Item["name"] = AiText::Utf8(Object->GetName()).c_str();
			Items.push_back(Item);
		}
		return {{"ok", true}, {"objects", Items}};
	}
	if (Method == "scene.transform")
	{
		Fvector Translation, RotationDelta, ScaleFactor;
		if (!AiVector(Params, "translation_m", Translation, 0, 1000000) || !AiVector(Params, "rotation_degrees", RotationDelta, 0, 360000) || !AiVector(Params, "scale_factor", ScaleFactor, 1, 10000) || ScaleFactor.x <= 0 || ScaleFactor.y <= 0 || ScaleFactor.z <= 0)
		{
			return Error("INVALID_TRANSFORM", "Transform values must be finite vectors with positive scale factors");
		}
		if (Translation.x == 0 && Translation.y == 0 && Translation.z == 0 && RotationDelta.x == 0 && RotationDelta.y == 0 && RotationDelta.z == 0 && ScaleFactor.x == 1 && ScaleFactor.y == 1 && ScaleFactor.z == 1)
		{
			return {{"ok", true}, {"unchanged", true}, {"undo_steps", 0}};
		}
		RotationDelta.mul(PI / 180.f);
		for (CCustomObject* Object : Targets)
		{
			CSceneObject* StaticObject = smart_cast<CSceneObject*>(Object);
			if (StaticObject != nullptr && StaticObject->IsDynamic() && (ScaleFactor.x != 1 || ScaleFactor.y != 1 || ScaleFactor.z != 1))
			{
				return Error("DYNAMIC_SCALE", "Dynamic objects cannot be scaled in this SDK");
			}
			Fvector Position = Object->GetPosition();
			Fvector Rotation = Object->GetRotation();
			Fvector Scale = Object->GetScale();
			Position.add(Translation);
			Rotation.add(RotationDelta);
			Scale.mul(ScaleFactor);
			if (!_valid(Position) || !_valid(Rotation) || !_valid(Scale) || Position.magnitude() > 10000000 || Scale.x < 0.0001f || Scale.y < 0.0001f || Scale.z < 0.0001f || Scale.x > 100000 || Scale.y > 100000 || Scale.z > 100000)
			{
				return Error("TRANSFORM_RANGE", "The resulting transform is outside the supported range");
			}
		}
		AiSceneSnapshot Before;
		Json Changed = Json::array();
		for (CCustomObject* Object : Targets)
		{
			Json Previous = Describe(Object);
			Scene->BeforeObjectChange(Object);
			Object->UpdateTransform(true);
			Fvector Position = Object->GetPosition();
			Fvector Rotation = Object->GetRotation();
			Fvector Scale = Object->GetScale();
			Position.add(Translation);
			Rotation.add(RotationDelta);
			Scale.mul(ScaleFactor);
			Object->NumSetPosition(Position);
			Object->NumSetRotation(Rotation);
			Object->NumSetScale(Scale);
			Object->UpdateTransform(true);
			if (!Object->GetPosition().similar(Position) || !Object->GetRotation().similar(Rotation) || !Object->GetScale().similar(Scale))
			{
				Before.Restore();
				Synchronize();
				return Error("TRANSFORM_FAILED", "A target rejected the transform; the scene snapshot was restored");
			}
			Changed.push_back({{"before", Previous}, {"after", Describe(Object)}});
		}
		Json Result = {{"changed", Changed}, {"transform_space", "world translation; Euler XYZ rotation delta; scaling about each object's pivot"}};
		FinishChange(Before, Result);
		return Result;
	}
	if (Method == "scene.duplicate")
	{
		Fvector Translation;
		if (!AiVector(Params, "translation_m", Translation, 0, 1000000))
		{
			return Error("INVALID_TRANSLATION", "translation_m must contain three finite coordinates");
		}
		for (CCustomObject* Object : Targets)
		{
			if (Object->FClassID != OBJCLASS_SCENEOBJECT)
			{
				return Error("DUPLICATE_CLASS", "Version 1 duplicates scene model instances only");
			}
			const char* Reference = Object->RefName();
			string_path ModelPath;
			if (Reference == nullptr || xr_strlen(Object->GetName()) > 180)
			{
				return Error("INVALID_REFERENCE", "The target needs a valid model reference and a name shorter than 181 characters");
			}
			FS.update_path(ModelPath, _objects_, EFS.ChangeFileExt(xr_string(Reference), ".object").c_str());
			if (!FS.exist(ModelPath))
			{
				return Error("MODEL_MISSING", "A target library model is missing; duplication was cancelled before appending any objects");
			}
			Fvector Position = Object->GetPosition();
			Position.add(Translation);
			if (!_valid(Position) || Position.magnitude() > 10000000)
			{
				return Error("TRANSFORM_RANGE", "A duplicate position would exceed the supported coordinate range");
			}
		}
		AiSceneSnapshot Before;
		xr_vector<CCustomObject*> Created;
		for (CCustomObject* Object : Targets)
		{
			CMemoryWriter Writer;
			Scene->SaveObjectStream(Object, Writer);
			IReader Reader(Writer.pointer(), Writer.size());
			CCustomObject* Copy = nullptr;
			if (!Scene->ReadObjectStream(Reader, Copy) || Copy == nullptr)
			{
				for (CCustomObject* Partial : Created)
				{
					xr_delete(Partial);
				}
				return Error("DUPLICATE_FAILED", "The instance could not be cloned; no objects were appended");
			}
			string256 Name;
			Scene->GenObjectName(Copy->FClassID, Name, Object->GetName());
			xr_string UniqueName = Name;
			UniqueName += "_";
			UniqueName += std::to_string(Created.size()).c_str();
			if (UniqueName.size() >= 255 || Scene->FindObjectByName(UniqueName.c_str(), OBJCLASS_DUMMY) != nullptr)
			{
				xr_delete(Copy);
				for (CCustomObject* Partial : Created)
				{
					xr_delete(Partial);
				}
				return Error("NAME_CONFLICT", "A unique duplicate name could not be reserved");
			}
			Copy->SetName(UniqueName.c_str());
			Fvector Position = Copy->GetPosition();
			Position.add(Translation);
			Copy->SetPosition(Position);
			Created.push_back(Copy);
		}
		Json Items = Json::array();
		for (CCustomObject* Object : Created)
		{
			Scene->AppendObject(Object, false);
			Object->Select(true);
			Items.push_back(Describe(Object));
		}
		Json Result = {{"created", Items}};
		FinishChange(Before, Result);
		return Result;
	}
	return Error("UNKNOWN_METHOD", "This editor command is not supported");
}
