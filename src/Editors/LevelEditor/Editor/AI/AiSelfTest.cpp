#include "stdafx.h"
#include "AiSelfTest.h"
#include "SceneExecutor.h"
#include "AiProjectTools.h"
#include "DocumentCommandsSelfTest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>
#include <algorithm>

namespace AiAssistantSelfTest
{
using Json = nlohmann::json;

static u64 NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static xr_string String(const Json& Value, const char* Name)
{
	auto Iter = Value.find(Name);
	return Iter != Value.end() && Iter->is_string() ? xr_string(Iter->get_ref<const Json::string_t&>().c_str()) : xr_string();
}

static bool IsOk(const Json& Result)
{
	return Result.is_object() && Result.contains("ok") && Result["ok"] == true;
}

static std::filesystem::path FilePath(const char* Name)
{
	const xr_string Utf8 = IsUTF8(Name) ? xr_string(Name) : Platform::ANSI_TO_UTF8(Name);
	return std::filesystem::u8path(Utf8.c_str());
}

static xr_vector<u8> ReadBytes(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	return xr_vector<u8>(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
}

static xr_vector<u8> DecodeBase64(const xr_string& Text)
{
	static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	xr_vector<u8> Bytes;
	u32 Buffer = 0;
	u32 Bits = 0;
	for (char Character : Text)
	{
		if (Character == '=')
		{
			break;
		}
		const char* Found = strchr(Alphabet, Character);
		if (Found == nullptr)
		{
			return {};
		}
		Buffer = (Buffer << 6) | u32(Found - Alphabet);
		Bits += 6;
		if (Bits >= 8)
		{
			Bits -= 8;
			Bytes.push_back(u8(Buffer >> Bits));
		}
	}
	return Bytes;
}

class NativeSuite final
{
public:
	NativeSuite() = default;
	~NativeSuite() = default;

	void Tick()
	{
		if (IsDone || EContext.UI == nullptr || Scene == nullptr || !EContext.UI->m_bReady)
		{
			return;
		}
		if (StartedAt == 0)
		{
			StartedAt = NowMs();
			string_path Path;
			FS.update_path(Path, "$app_data_root$", "");
			OutputDirectory = FilePath(Path);
			ReportPath = OutputDirectory / "ai-selftest-report.json";
			RunDirectory = OutputDirectory / ("AI self test " + std::to_string(StartedAt)) / std::filesystem::u8path("Проверка ИИ");
			std::error_code Code;
			std::filesystem::create_directories(RunDirectory, Code);
			Report = {{"version", 1}, {"ok", false}, {"status", "running"}, {"upstream", "bcf4dd51d6433fdee29ece01f7ce40af16666dec"}, {"live_model_test", "requires_user_login"}, {"checks", Json::array()}};
			Check("test_output_directory", !Code, {{"error", Code.message()}});
			SaveReport();
		}
		if (NowMs() - StartedAt > 120000)
		{
			Check("native_suite_timeout", false, {{"stage", Stage}});
			Complete();
			return;
		}
		if (HasFailed)
		{
			Complete();
			return;
		}
		if (LUI->LoaderEvent.IsBusy() || Scene->locked() || EContext.UI->GetEState() != esEditScene || EContext.UI->ActiveTabIndex != 0)
		{
			return;
		}
		if (ReadyFrames++ < 10 || !EContext.UI->RT)
		{
			return;
		}
		Step();
		Stage++;
		Report["last_stage"] = Stage;
		SaveReport();
	}

private:
	void Check(const char* Name, bool Passed, Json Details = Json::object())
	{
		HasFailed |= !Passed;
		Report["checks"].push_back({{"name", Name}, {"passed", Passed}, {"details", std::move(Details)}});
	}

	void SaveReport()
	{
		std::ofstream File(ReportPath, std::ios::binary | std::ios::trunc);
		if (File)
		{
			File << Report.dump(2, ' ', false, Json::error_handler_t::replace);
		}
	}

	void Complete()
	{
		IsDone = true;
		Report["ok"] = !HasFailed;
		Report["status"] = HasFailed ? "failed" : "passed";
		Report["elapsed_ms"] = NowMs() - StartedAt;
		SaveReport();
		Msg("AI_SELFTEST %s", HasFailed ? "FAILED" : "PASSED");
		Scene->m_RTFlags.set(EScene::flRT_Unsaved | EScene::flRT_Modified, false);
		EContext.UI->Quit();
	}

	Json Call(const char* Method, Json Params = Json::object(), bool IsMutation = false)
	{
		if (IsMutation)
		{
			const Json Status = Executor.Execute("scene.status", Json::object());
			Params["scene_session"] = Status.value("scene_session", Json());
			Params["op_id"] = "selftest-" + std::to_string(++OperationId);
		}
		return Executor.Execute(Method, Params);
	}

	Json Cube()
	{
		const Json Found = Call("scene.search", {{"name", "ai_cube"}, {"limit", 100}});
		if (!IsOk(Found) || !Found.contains("objects") || !Found["objects"].is_array())
		{
			return Json::object();
		}
		for (const Json& Object : Found["objects"])
		{
			if (String(Object, "name") == "ai_cube")
			{
				return Object;
			}
		}
		return Json::object();
	}

	bool HasPosition(double X)
	{
		const Json Object = Cube();
		return Object.contains("position_m") && Object["position_m"].is_array() && Object["position_m"].size() == 3 && std::abs(Object["position_m"][0].get<double>() - X) < 0.0001;
	}

	Json CubeMaterials()
	{
		const Json Object = Cube();
		const Json Result = Call("scene.materials", {{"ids", Json::array({Object.value("id", Json())})}});
		return IsOk(Result) && Result.contains("objects") && !Result["objects"].empty() ? Result["objects"][0] : Json::object();
	}

	bool HasExpectedTextures()
	{
		const Json Materials = CubeMaterials();
		return Materials.contains("surfaces") && Materials["surfaces"].size() == 2 && String(Materials["surfaces"][0], "texture") == "ai_test\\alternate" && String(Materials["surfaces"][1], "texture") == "ai_test\\checker";
	}

	void Step()
	{
		switch (Stage)
		{
			case 0:
			{
				EPrefs->scene_undo_level = std::max(EPrefs->scene_undo_level, 32u);
				string_path Path;
				FS.update_path(Path, _objects_, "ai_test\\cube.object");
				LibraryPath = FilePath(Path);
				OriginalLibrary = ReadBytes(LibraryPath);
				Check("fixture_library_exists", !OriginalLibrary.empty());
				FS.update_path(Path, _maps_, "ai_test.level");
				Scene->m_RTFlags.set(EScene::flRT_Unsaved | EScene::flRT_Modified, false);
				Check("fixture_load_started", !!ExecCommand(COMMAND_LOAD, xr_string(Path)));
				break;
			}
			case 1:
			{
				const Json Status = Call("scene.status");
				InitialSession = String(Status, "scene_session");
				Check("fixture_loaded", IsOk(Status) && Status.value("object_count", 0) == 1 && HasPosition(0), Status);
				const Json Object = Cube();
				InitialId = String(Object, "id");
				Check("find_and_select", IsOk(Call("scene.select", {{"ids", Json::array({InitialId.c_str()})}, {"clear", true}}, true)));
				const Json Selection = Call("scene.selection");
				Check("read_selection", IsOk(Selection) && Selection.contains("objects") && Selection["objects"].size() == 1);
				break;
			}
			case 2:
			{
				const size_t Before = Scene->GetUndoCount();
				const Json Result = Call("scene.transform", {{"ids", Json::array({InitialId.c_str()})}, {"translation_m", {1, 0, 0}}}, true);
				Check("move_one_metre_one_undo_step", IsOk(Result) && HasPosition(1) && Scene->GetUndoCount() == Before + 1, Result);
				break;
			}
			case 3:
			{
				const Json Result = Call("scene.undo", Json::object(), true);
				Check("undo_position", IsOk(Result) && HasPosition(0), Result);
				Check("undo_invalidates_ids", String(Call("scene.status"), "scene_session") != InitialSession && String(Cube(), "id") != InitialId);
				break;
			}
			case 4:
			{
				const Json Result = Call("scene.redo", Json::object(), true);
				Check("redo_position", IsOk(Result) && HasPosition(1), Result);
				break;
			}
			case 5:
			{
				const Json Status = Call("scene.status");
				Json Request = {{"ids", Json::array({Cube().value("id", Json())})}, {"translation_m", {2, 0, 0}}, {"op_id", "selftest-duplicate-once"}, {"scene_session", Status.value("scene_session", Json())}};
				const Json First = Call("scene.duplicate", Request);
				const Json Second = Call("scene.duplicate", Request);
				Check("duplicate_idempotent_replay", IsOk(First) && IsOk(Second) && Second.value("replayed", false) && Call("scene.status").value("object_count", 0) == 2, {{"first", First}, {"replay", Second}});
				break;
			}
			case 6:
			{
				const Json Result = Call("scene.material_set", {{"changes", Json::array({{{"object_id", Cube().value("id", Json())}, {"surface_index", 0}, {"texture", "ai_test\\alternate"}}})}}, true);
				Check("material_instance_surface_only", IsOk(Result) && HasExpectedTextures(), Result);
				Check("library_object_unchanged_after_material", ReadBytes(LibraryPath) == OriginalLibrary);
				Check("focus_object", IsOk(Call("scene.focus", {{"ids", Json::array({Cube().value("id", Json())})}}, true)));
				break;
			}
			case 7:
			{
				Json Result = Call("scene.viewport", {{"width", 640}});
				const xr_vector<u8> Bytes = DecodeBase64(String(Result, "data"));
				const bool HasPng = Bytes.size() > 8 && Bytes[0] == 137 && Bytes[1] == 80 && Bytes[2] == 78 && Bytes[3] == 71;
				if (HasPng)
				{
					std::ofstream File(RunDirectory / "viewport.png", std::ios::binary);
					File.write(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
				}
				Result.erase("data");
				Check("viewport_actual_renderer_png", IsOk(Result) && HasPng, Result);
				break;
			}
			case 8:
			{
				CopyPath = RunDirectory / "scene copy.level";
				const auto Utf8 = CopyPath.u8string();
				const Json Result = Call("scene.save_copy", {{"path", reinterpret_cast<const char*>(Utf8.c_str())}}, true);
				Check("save_scene_copy_unicode_space_path", IsOk(Result), Result);
				break;
			}
			case 9:
			{
				const auto Utf8 = CopyPath.u8string();
				Scene->m_RTFlags.set(EScene::flRT_Unsaved | EScene::flRT_Modified, false);
				Check("reload_copy_started", !!ExecCommand(COMMAND_LOAD, xr_string(reinterpret_cast<const char*>(Utf8.c_str()))));
				break;
			}
			case 10:
			{
				Check("reload_copy_keeps_transforms_materials", Call("scene.status").value("object_count", 0) == 2 && HasPosition(1) && HasExpectedTextures());
				Check("library_object_byte_exact_after_reload", ReadBytes(LibraryPath) == OriginalLibrary);
				CCustomObject* Object = Scene->FindObjectByName("ai_cube", OBJCLASS_SCENEOBJECT);
				if (Object != nullptr)
				{
					const xr_string PreviousName = Object->GetName();
					const bool WasSelected = Object->Selected();
					Object->Select(true);
					const xr_string LegacyName = Platform::UTF8_to_CP1251("тест_объект");
					Object->SetName(LegacyName.c_str());
					const Json Found = Call("scene.search", {{"name", "тест_объект"}});
					Check("legacy_cp1251_name_search_utf8", IsOk(Found) && Found.contains("objects") && Found["objects"].size() == 1 && String(Found["objects"][0], "name") == "тест_объект", Found);
					const Json Selected = Call("scene.selection");
					bool HasLegacyName = false;
					if (IsOk(Selected) && Selected.contains("objects"))
					{
						for (const Json& Item : Selected["objects"])
						{
							HasLegacyName |= String(Item, "name") == "тест_объект";
						}
					}
					Check("legacy_cp1251_selection_name_utf8", HasLegacyName, Selected);
					Object->SetName(PreviousName.c_str());
					Object->Select(WasSelected);
				}
				else
				{
					Check("legacy_cp1251_test_target", false);
				}
				break;
			}
			case 11:
			{
				Check("invalid_object_rejected", !IsOk(Call("scene.transform", {{"ids", {"unknown-object"}}, {"translation_m", {1, 0, 0}}}, true)));
				Check("missing_resource_rejected", !IsOk(Call("scene.place", {{"reference", "ai_test\\missing_model"}}, true)));
				const Json Status = Call("scene.status");
				Json Request = {{"ids", Json::array({Cube().value("id", Json())})}, {"translation_m", {5, 0, 0}}, {"op_id", "selftest-cancelled-operation"}, {"scene_session", Status.value("scene_session", Json())}};
				Executor.CancelOperation("scene.transform", Request);
				Check("cancelled_operation_replay_never_moves", !IsOk(Call("scene.transform", Request)) && HasPosition(1));
				Check("expired_request_never_moves", !IsOk(Call("scene.transform", {{"ids", Json::array({Cube().value("id", Json())})}, {"translation_m", {5, 0, 0}}, {"deadline_ms", 0}}, true)) && HasPosition(1));
				Request["op_id"] = "selftest-stale-scene";
				Request["scene_session"] = InitialSession.c_str();
				Check("stale_scene_rejected", !IsOk(Call("scene.transform", Request)) && HasPosition(1));
				break;
			}
			case 12:
			{
				const Json Catalog = Call("scene.sdk_catalog", {{"query", "COMMAND_VALIDATE_SCENE"}});
				Check("sdk_catalog_reads_native_registry", IsOk(Catalog) && Catalog.value("total", 0) > 0, Catalog);
				const Json Assets = Call("scene.sdk_assets", {{"kind", "objects"}, {"query", "cube.object"}});
				Check("sdk_catalog_finds_library_fixture", IsOk(Assets) && Assets.value("total", 0) > 0, Assets);
				const Json Model = Call("scene.sdk_model", {{"reference", "ai_test\\cube.object"}});
				Check("sdk_model_reads_surfaces", IsOk(Model) && Model.value("total", 0) > 0, Model);
				const Json Id = Cube().value("id", Json());
				const Json Properties = Call("scene.object_properties", {{"object_id", Id}});
				Check("sdk_object_properties_reads_schema", IsOk(Properties) && Properties.value("total", 0) > 0, Properties);
				const Json Rename = Call("scene.object_edit", {{"object_id", Id}, {"name", "ai_sdk_renamed"}}, true);
				const Json Search = Call("scene.search", {{"name", "ai_sdk_renamed"}});
				Check("sdk_object_rename_visible_in_search", IsOk(Rename) && IsOk(Search) && Search.value("objects", Json::array()).size() == 1, Rename);
				Check("sdk_object_rename_undo", IsOk(Call("scene.undo", Json::object(), true)) && !String(Cube(), "id").empty());
				const Json Unknown = Call("scene.sdk_run", {{"action", "COMMAND_EXIT"}}, true);
				Check("sdk_action_rejects_unmapped_commands", !IsOk(Unknown), Unknown);
				break;
			}
			case 13:
			{
				const Json Light = Call("scene.entity_create", {{"kind", "light"}, {"name", "ai_test_light"}, {"position_m", {2, 3, 4}}}, true);
				const Json Id = Light.value("created", Json::object()).value("id", Json());
				Check("sdk_create_light", IsOk(Light) && Id.is_string(), Light);
				if (IsOk(Light) && Id.is_string())
				{
					const Json Edit = Call("scene.entity_set", {{"object_id", Id}, {"changes", {{"brightness", 1.5}, {"range_m", 12.0}}}}, true);
					Check("sdk_edit_light", IsOk(Edit), Edit);
					const Json Read = Call("scene.entity_get", {{"object_id", Id}});
					Check("sdk_read_edited_light", IsOk(Read) && Read.value("details", Json::object()).value("brightness", 0.0) == 1.5, Read);
				}
				const Json Partition = Call("scene.partition_read");
				Check("sdk_world_partition_status", IsOk(Partition) && Partition.value("cell_size_m", 0.0) > 0, Partition);
				const Json Way = Call("scene.way_edit", {{"action", "create"}, {"name", "ai_test_way"}, {"position_m", {0, 0, 0}}}, true);
				Check("sdk_create_waypoint", IsOk(Way), Way);
				break;
			}
			case 14:
			{
				const Json Metadata = AiProjectTools::SelfTestMetadata(RunDirectory);
				Check("sdk_texture_metadata_round_trip", IsOk(Metadata), Metadata);
				const Json Pack = Call("scene.pack_start", {{"folder", "../escape"}}, true);
				Check("sdk_pack_rejects_path_traversal", !IsOk(Pack), Pack);
				const Json PackStatus = Call("scene.pack_status");
				Check("sdk_pack_reads_status", IsOk(PackStatus), PackStatus);
				break;
			}
			case 15:
			{
				AiDocuments::SmokeTest(StartedAt,
					[this](const char* Method, Json Params, bool Mutation) { return Call(Method, std::move(Params), Mutation); },
					[this](const char* Name, bool Passed, Json Details) { Check(Name, Passed, std::move(Details)); });
				break;
			}
			default:
			{
				Complete();
				break;
			}
		}
	}

	AiSceneExecutor Executor;
	Json Report = Json::object();
	std::filesystem::path OutputDirectory;
	std::filesystem::path ReportPath;
	std::filesystem::path RunDirectory;
	std::filesystem::path LibraryPath;
	std::filesystem::path CopyPath;
	xr_vector<u8> OriginalLibrary;
	xr_string InitialSession;
	xr_string InitialId;
	u64 StartedAt = 0;
	u64 OperationId = 0;
	u32 Stage = 0;
	u32 ReadyFrames = 0;
	bool HasFailed = false;
	bool IsDone = false;
};

static NativeSuite* Suite = nullptr;

void Tick()
{
	if (Core.Params == nullptr)
	{
		return;
	}
	bool IsEnabled = false;
	for (const xr_string& Argument : xr_string(Core.Params).Split(' '))
	{
		IsEnabled |= Argument == "-ai-selftest";
	}
	if (!IsEnabled)
	{
		return;
	}
	if (Suite == nullptr)
	{
		Suite = new NativeSuite();
	}
	Suite->Tick();
}

void Shutdown()
{
	xr_delete(Suite);
}
} // namespace AiAssistantSelfTest
