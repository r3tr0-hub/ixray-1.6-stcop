#pragma once

#include "AiText.h"
#include "MaterialCommands.h"
#include "ActorEditor/UIActorEditorForm.h"
#include "ActorEditor/UI_ActorTools.h"
#include "ParticleEditor/UIParticleEditorForm.h"
#include "ParticleEditor/UI_ParticleTools.h"
#include "ShaderEditor/UIShaderEditorForm.h"
#include "ShaderEditor/UI_shadertools.h"
#include "ShaderEditor/SHCompilerTools.h"
#include "../../../../xrEngine/GameMtlLib.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>

namespace AiContentEditors
{
using Json = nlohmann::json;

inline bool Handles(const xr_string& Method)
{
	return Method == "scene.particle_read" || Method == "scene.particle_edit" ||
		Method == "scene.actor_read" || Method == "scene.actor_edit" ||
		Method == "scene.shader_read" || Method == "scene.shader_edit";
}

inline bool IsMutation(const xr_string& Method)
{
	return Method == "scene.particle_edit" || Method == "scene.actor_edit" || Method == "scene.shader_edit";
}

inline Json Error(const char* Code, const char* Message)
{
	return {{"ok", false}, {"error", {{"code", Code}, {"message", Message}}}};
}

inline Json Text(const char* Value)
{
	return AiText::Utf8(Value ? Value : "").c_str();
}

inline xr_string String(const Json& Params, const char* Key)
{
	auto It = Params.find(Key);
	return It != Params.end() && It->is_string() ? It->get_ref<const Json::string_t&>().c_str() : "";
}

inline bool ResourceName(const Json& Params, const char* Key, xr_string& Name, size_t MaxLength)
{
	auto It = Params.find(Key);
	if (It == Params.end() || !It->is_string())
	{
		return false;
	}
	const auto& TextValue = It->get_ref<const Json::string_t&>();
	if (TextValue.empty() || TextValue.size() > MaxLength)
	{
		return false;
	}
	Name.clear();
	for (char Character : TextValue)
	{
		if (u8(Character) < 32 || Character == ':' || Character == '*' || Character == '?' || Character == '"' || Character == '<' || Character == '>' || Character == '|')
		{
			return false;
		}
		if (Character == '/')
		{
			Character = '\\';
		}
		Name += Character;
	}
	if (Name.front() == '\\' || Name.back() == '\\')
	{
		return false;
	}
	for (size_t Start = 0; Start < Name.size();)
	{
		const size_t End = Name.find('\\', Start);
		const xr_string Part = Name.substr(Start, End - Start);
		if (Part.empty() || Part.back() == '.' || Part.back() == ' ')
		{
			return false;
		}
		if (End == xr_string::npos)
		{
			break;
		}
		Start = End + 1;
	}
	return true;
}

inline bool Integer(const Json& Value, s64 Min, s64 Max)
{
	return Value.is_number_integer() && Value.get<double>() >= double(Min) && Value.get<double>() <= double(Max);
}

inline bool Number(const Json& Value, double Min, double Max)
{
	return Value.is_number() && std::isfinite(Value.get<double>()) && Value.get<double>() >= Min && Value.get<double>() <= Max;
}

inline bool Page(const Json& Params, size_t& Offset, size_t& Limit)
{
	const Json Start = Params.value("offset", Json(0));
	const Json Count = Params.value("limit", Json(30));
	if (!Integer(Start, 0, 100000000) || !Integer(Count, 1, 500))
	{
		return false;
	}
	Offset = Start.get<size_t>();
	Limit = Count.get<size_t>();
	return true;
}

inline void SetPage(Json& Response, size_t Offset, size_t Count, size_t Total)
{
	const size_t Next = std::min(Offset, Total) + Count;
	Response["total"] = Total;
	Response["next_offset"] = Next;
	Response["has_more"] = Next < Total;
}

inline Json Vector(const Fvector& Value)
{
	return Json::array({Value.x, Value.y, Value.z});
}

inline Json ParticleAction(const EParticleAction& Action, size_t Index)
{
	Json Fields = Json::object();
	for (const auto& [Key, Field] : Action.floats)
	{
		Fields[Key.c_str()] = {{"value", Field.val}, {"min", Field.mn}, {"max", Field.mx}};
	}
	for (const auto& [Key, Field] : Action.ints)
	{
		Fields[Key.c_str()] = {{"value", Field.val}, {"min", Field.mn}, {"max", Field.mx}};
	}
	for (const auto& [Key, Field] : Action.bools)
	{
		Fields[Key.c_str()] = {{"value", Field.val}};
	}
	for (const auto& [Key, Field] : Action.vectors)
	{
		Fields[Key.c_str()] = {{"value", Vector(Field.val)}, {"min", Field.mn}, {"max", Field.mx}, {"native_angle_units", Field.type == PVector::vAngle}};
	}
	return {{"action_index", Index}, {"name", Text(Action.actionName.c_str())}, {"type", Text(Action.actionType.c_str())}, {"enabled", Action.flags.is(EParticleAction::flEnabled)}, {"fields", std::move(Fields)}};
}

inline Json ParticleEffect(const PS::CPEDef& Effect, size_t Offset, size_t Limit)
{
	Json Items = Json::array();
	for (size_t Index = Offset; Index < Effect.m_EActionList.size() && Items.size() < Limit; ++Index)
	{
		if (const EParticleAction* Action = Effect.m_EActionList[Index])
		{
			Items.push_back(ParticleAction(*Action, Index));
		}
		else
		{
			Items.push_back({{"action_index", Index}, {"invalid", true}});
		}
	}
	Json Result = {{"ok", true}, {"name", Text(Effect.Name())}, {"max_particles", Effect.m_MaxParticles}, {"time_limit_seconds", Effect.m_Flags.is(PS::CPEDef::dfTimeLimit) ? Effect.m_fTimeLimit : 0.f}, {"texture", Text(Effect.m_TextureName.c_str())}, {"shader", Text(Effect.m_ShaderName.c_str())}, {"actions", Items}};
	SetPage(Result, Offset, Items.size(), Effect.m_EActionList.size());
	return Result;
}

inline Json ReadParticles(CParticleTool& Tool, const Json& Params)
{
	size_t Offset;
	size_t Limit;
	if (!Page(Params, Offset, Limit))
	{
		return Error("INVALID_PAGE", "Use an offset of zero or more and a limit in 1..500");
	}
	if (Params.contains("name"))
	{
		xr_string Name;
		if (!ResourceName(Params, "name", Name, 200))
		{
			return Error("INVALID_PARTICLE_NAME", "Specify the effect name from Particle Editor");
		}
		PS::CPEDef* Effect = Tool.FindPE(Name.c_str());
		return Effect ? ParticleEffect(*Effect, Offset, Limit) : Error("PARTICLE_NOT_FOUND", "The particle effect does not exist");
	}
	const auto& Effects = RImplementation.PSLibrary.VecPEDs();
	Json Items = Json::array();
	for (size_t Index = Offset; Index < Effects.size() && Items.size() < Limit; ++Index)
	{
		Items.push_back(Text(Effects[Index]->Name()));
	}
	Json Types = Json::array();
	for (const auto* Token = actions_token; Token && Token->name; ++Token)
	{
		Types.push_back(Text(Token->name));
	}
	Json Result = {{"ok", true}, {"items", Items}, {"modified", Tool.IsModified()}, {"playing", Tool.IsPlaying()}, {"action_types", Types}};
	SetPage(Result, Offset, Items.size(), Effects.size());
	return Result;
}

inline bool CheckParticleField(EParticleAction& Action, const char* Key, const Json& Value)
{
	if (auto It = Action.floats.find(Key); It != Action.floats.end())
	{
		return Number(Value, It->second.mn, It->second.mx) &&
			(Action.type != PAPI::PASourceID || xr_strcmp(Key, "Rate") != 0 || Number(Value, 0, 1000000));
	}
	if (auto It = Action.ints.find(Key); It != Action.ints.end())
	{
		return Integer(Value, It->second.mn, It->second.mx);
	}
	if (Action.bools.find(Key) != Action.bools.end())
	{
		return Value.is_boolean();
	}
	if (auto It = Action.vectors.find(Key); It != Action.vectors.end())
	{
		return Value.is_array() && Value.size() == 3 && std::all_of(Value.begin(), Value.end(), [&](const Json& Part)
		{
			return Number(Part, It->second.mn, It->second.mx);
		});
	}
	return false;
}

inline void SetParticleField(EParticleAction& Action, const char* Key, const Json& Value)
{
	if (auto It = Action.floats.find(Key); It != Action.floats.end())
	{
		It->second.val = Value.get<float>();
	}
	else if (auto It = Action.ints.find(Key); It != Action.ints.end())
	{
		It->second.val = Value.get<int>();
	}
	else if (auto It = Action.bools.find(Key); It != Action.bools.end())
	{
		It->second.val = Value.get<bool>();
	}
	else
	{
		Action.vectors.find(Key)->second.val.set(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>());
	}
}

inline Json PatchParticle(CParticleTool& Tool, PS::CPEDef& Effect, const Json& Params)
{
	const Json IndexValue = Params.value("action_index", Json());
	if (!Integer(IndexValue, 0, 100000000) || IndexValue.get<size_t>() >= Effect.m_EActionList.size())
	{
		return Error("INVALID_ACTION_INDEX", "Use an action_index from scene_particle_read");
	}
	const size_t Index = IndexValue.get<size_t>();
	EParticleAction* Action = Effect.m_EActionList[Index];
	const Json Fields = Params.value("fields", Json::object());
	if (!Action || !Fields.is_object() || Fields.size() > 32 || (Fields.empty() && !Params.contains("enabled")) ||
		(Params.contains("enabled") && !Params["enabled"].is_boolean()))
	{
		return Error("INVALID_PARTICLE_PATCH", "Specify editable action fields or a boolean enabled state");
	}
	for (auto It = Fields.begin(); It != Fields.end(); ++It)
	{
		if (It.key().size() > 100 || It.key().find('\0') != std::string::npos || !CheckParticleField(*Action, It.key().c_str(), *It))
		{
			Json Result = Error("INVALID_PARTICLE_FIELD", "The action does not support the field, value type, or range");
			Result["field"] = It.key();
			return Result;
		}
	}
	if (Action->type == PAPI::PAKillOldID && Params.contains("enabled") && !Params["enabled"].get<bool>())
	{
		return Error("REQUIRED_PARTICLE_ACTION", "The native effect requires a KillOld action");
	}
	Tool.SelectEffectForEditing(Effect, Action);
	for (auto It = Fields.begin(); It != Fields.end(); ++It)
	{
		SetParticleField(*Action, It.key().c_str(), *It);
	}
	if (Params.contains("enabled"))
	{
		Action->flags.set(EParticleAction::flEnabled, Params["enabled"].get<bool>());
	}
	Tool.CommitEffectEdit();
	return {{"ok", true}, {"name", Text(Effect.Name())}, {"saved", false}, {"effect_action", ParticleAction(*Action, Index)}};
}

inline Json SaveParticle(PS::CPEDef& Effect)
{
	const bool Valid = std::any_of(Effect.m_EActionList.begin(), Effect.m_EActionList.end(), [](const EParticleAction* Action)
	{
		return Action && Action->type == PAPI::PAKillOldID && Action->flags.is(EParticleAction::flEnabled);
	});
	if (!Valid || !FS.path_exist("$game_particles$") || std::any_of(Effect.m_EActionList.begin(), Effect.m_EActionList.end(), [](const EParticleAction* Action) { return Action == nullptr; }))
	{
		return Error("PARTICLE_SAVE_UNAVAILABLE", "The effect needs an enabled KillOld action and the SDK particles path");
	}
	string_path Path;
	xr_string Leaf = Effect.Name();
	Leaf += ".pe";
	FS.update_path(Path, "$game_particles$", Leaf.c_str());

	// Stage next to the destination and parse the native serialization before replacement.
	const auto Destination = std::filesystem::u8path(Path);
	auto Temporary = Destination;
	Temporary += ".ixray-ai-stage";
	std::error_code Code;
	std::filesystem::create_directories(Destination.parent_path(), Code);
	if (Code || !std::filesystem::create_directory(Temporary, Code) || Code)
	{
		return Error("PARTICLE_STAGE_FAILED", "The effect folder or private staging directory is unavailable");
	}
	struct RemoveStage
	{
		const std::filesystem::path& Path;
		~RemoveStage()
		{
			std::error_code Code;
			std::filesystem::remove_all(Path, Code);
		}
	} Remove{Temporary};
	const auto Stage = Temporary / Destination.filename();
	const auto StageUtf8 = Stage.u8string();
	if (StageUtf8.size() >= sizeof(string_path))
	{
		return Error("PARTICLE_STAGE_FAILED", "The effect path is too long to stage");
	}
	const char* StageName = reinterpret_cast<const char*>(StageUtf8.c_str());
	CInifile Ini(StageName, false, false, false);
	Effect.Save2(Ini);
	if (!Ini.save_as(StageName))
	{
		return Error("PARTICLE_SAVE_FAILED", "The SDK could not write the effect file");
	}
	xr_vector<char> SavedBytes;
	{
		std::ifstream Stream(Stage, std::ios::binary);
		SavedBytes.assign(std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>());
	}
	if (SavedBytes.empty())
	{
		return Error("PARTICLE_VERIFY_FAILED", "The staged effect file is empty");
	}
	IReader SavedData(SavedBytes.data(), SavedBytes.size());
	PS::CPEDef Reopened;
	CInifile Saved(&SavedData);
	if (!Saved.line_exist("_effect", "version") || !Reopened.Load2(Saved) || Reopened.m_EActionList.size() != Effect.m_EActionList.size())
	{
		return Error("PARTICLE_VERIFY_FAILED", "The staged effect could not be reopened with its complete action list");
	}
	Reopened.SetName(Effect.Name());
	std::filesystem::rename(Stage, Destination, Code);
	if (Code)
	{
		return Error("PARTICLE_SAVE_FAILED", "The staged effect could not replace the destination file");
	}
	FS.TryLoad(xr_string(Path));
	Json Result = ParticleEffect(Reopened, 0, 30);
	Result["saved"] = true;
	Result["verified"] = "reopened_native_serialization";
	Result["path"] = Text(Path);
	return Result;
}

inline Json CreateParticle(CParticleTool& Tool, const Json& Params, const xr_string& Name, bool Clone)
{
	if (std::filesystem::u8path(Name.c_str()).extension() == ".pe")
	{
		return Error("INVALID_PARTICLE_NAME", "Use a library name without the .pe extension");
	}
	string_path FileName;
	if (Tool.FindPE(Name.c_str()) || Tool.FindPG(Name.c_str()) || Tool.FindPAC(Name.c_str()) ||
		(FS.path_exist("$game_particles$") && FS.exist(FileName, "$game_particles$", Name.c_str(), ".pe")))
	{
		return Error("PARTICLE_ALREADY_EXISTS", "Choose a new library name for the effect");
	}
	PS::CPEDef* Source = nullptr;
	if (Clone)
	{
		xr_string SourceName;
		if (!ResourceName(Params, "source_name", SourceName, 200) || !(Source = Tool.FindPE(SourceName.c_str())))
		{
			return Error("PARTICLE_NOT_FOUND", "Provide source_name from scene_particle_read");
		}
		if (std::any_of(Source->m_EActionList.begin(), Source->m_EActionList.end(), [](const EParticleAction* Action)
		{
			return Action == nullptr;
		}))
		{
			return Error("INVALID_PARTICLE_SOURCE", "The source contains an invalid action");
		}
	}
	PS::CPEDef* Effect = Tool.CreateEffectForEditing(Name.c_str(), Source);
	Json Result = ParticleEffect(*Effect, 0, 30);
	Result["saved"] = false;
	return Result;
}

inline Json EditParticleActions(CParticleTool& Tool, PS::CPEDef& Effect, const Json& Params, bool Add)
{
	if (Add)
	{
		const xr_string Type = String(Params, "action_type");
		const auto* Token = actions_token;
		while (Token && Token->name && Type != Token->name)
		{
			++Token;
		}
		xr_unique_ptr<EParticleAction> Action(Token && Token->name && pCreateEAction ? pCreateEAction(PAPI::PActionEnum(Token->id)) : nullptr);
		if (!Action)
		{
			return Error("INVALID_PARTICLE_ACTION_TYPE", "Use an action type from scene_particle_read");
		}
		if (Effect.m_EActionList.size() >= 256)
		{
			return Error("PARTICLE_ACTION_LIMIT", "This effect already has 256 actions");
		}
		xr_string Prefix = Action->actionName.c_str();
		xr_strlwr(Prefix);
		xr_string Unique = Prefix;
		for (u32 Suffix = 1;; ++Suffix)
		{
			bool Exists = false;
			Effect.FindActionByName(Unique.c_str(), Exists);
			if (!Exists)
			{
				break;
			}
			Unique = Prefix + "_" + xr_string::ToString(Suffix);
		}
		Action->actionName = Unique.c_str();
		Action->parent = &Effect;
		Tool.SelectEffectForEditing(Effect);
		Effect.m_EActionList.push_back(Action.get());
		EParticleAction* Added = Action.release();
		Tool.CommitEffectEdit();
		return {{"ok", true}, {"name", Text(Effect.Name())}, {"saved", false}, {"effect_action", ParticleAction(*Added, Effect.m_EActionList.size() - 1)}};
	}
	const Json IndexValue = Params.value("action_index", Json());
	if (!Integer(IndexValue, 0, 100000000) || IndexValue.get<size_t>() >= Effect.m_EActionList.size())
	{
		return Error("INVALID_ACTION_INDEX", "Use an action_index from scene_particle_read");
	}
	const size_t Index = IndexValue.get<size_t>();
	const EParticleAction* Action = Effect.m_EActionList[Index];
	if (Action && Action->type == PAPI::PAKillOldID && Action->flags.is(EParticleAction::flEnabled))
	{
		const bool HasOtherKill = std::any_of(Effect.m_EActionList.begin(), Effect.m_EActionList.end(), [&](const EParticleAction* Other)
		{
			return Other && Other != Action && Other->type == PAPI::PAKillOldID && Other->flags.is(EParticleAction::flEnabled);
		});
		if (!HasOtherKill)
		{
			return Error("REQUIRED_PARTICLE_ACTION", "Keep at least one enabled KillOld action");
		}
	}
	Tool.SelectEffectForEditing(Effect);
	Tool.RemoveAction(u32(Index), true);
	Tool.CommitEffectEdit();
	return {{"ok", true}, {"name", Text(Effect.Name())}, {"saved", false}, {"removed_action_index", Index}, {"total", Effect.m_EActionList.size()}};
}

inline Json PatchParticleEffect(CParticleTool& Tool, PS::CPEDef& Effect, const Json& Params)
{
	const bool ChangeLimit = Params.contains("max_particles");
	const bool ChangeTime = Params.contains("time_limit_seconds");
	const bool ChangeSprite = Params.contains("texture") || Params.contains("render_shader");
	xr_string Texture = Effect.m_TextureName.c_str() ? Effect.m_TextureName.c_str() : "";
	xr_string Shader = Effect.m_ShaderName.c_str() ? Effect.m_ShaderName.c_str() : "";
	if ((!ChangeLimit && !ChangeTime && !ChangeSprite) ||
		(ChangeLimit && !Integer(Params["max_particles"], 1, 100000)) ||
		(ChangeTime && !Number(Params["time_limit_seconds"], 0, 3600)))
	{
		return Error("INVALID_PARTICLE_EFFECT_PATCH", "Specify a valid particle count, time limit, or sprite resources");
	}
	if (ChangeSprite)
	{
		if ((Params.contains("texture") && !ResourceName(Params, "texture", Texture, 255)) ||
			(Params.contains("render_shader") && !ResourceName(Params, "render_shader", Shader, 127)) ||
			(Texture != "$null" && Texture.find('$') != xr_string::npos) ||
			!AiMaterials::TextureExists(Texture.c_str()) || Shader.empty() || !DEV->_FindBlender(Shader.c_str()))
		{
			return Error("INVALID_PARTICLE_SPRITE", "Provide a valid SDK sprite texture and render shader");
		}
	}
	Tool.SelectEffectForEditing(Effect);
	if (ChangeLimit)
	{
		Effect.m_MaxParticles = Params["max_particles"].get<int>();
	}
	if (ChangeTime)
	{
		Effect.m_fTimeLimit = Params["time_limit_seconds"].get<float>();
		Effect.m_Flags.set(PS::CPEDef::dfTimeLimit, Effect.m_fTimeLimit > 0.f);
	}
	if (ChangeSprite)
	{
		Effect.m_Flags.set(PS::CPEDef::dfSprite, true);
		Effect.m_TextureName = Texture.c_str();
		Effect.m_ShaderName = Shader.c_str();
		Effect.OnShaderChange(nullptr);
	}
	Tool.CommitEffectEdit();
	Json Result = ParticleEffect(Effect, 0, 30);
	Result["saved"] = false;
	return Result;
}

inline Json EditParticles(CParticleTool& Tool, const Json& Params)
{
	const xr_string Operation = String(Params, "action");
	if (Operation == "undo" || Operation == "redo")
	{
		const bool Changed = Operation == "undo" ? Tool.Undo() : Tool.Redo();
		return {{"ok", true}, {"changed", Changed}};
	}
	if (Operation == "stop")
	{
		Tool.StopCurrent(false);
		return {{"ok", true}, {"playing", Tool.IsPlaying()}};
	}
	xr_string Name;
	if (!ResourceName(Params, "name", Name, 200))
	{
		return Error("INVALID_PARTICLE_NAME", "Specify the effect name from Particle Editor");
	}
	if (Operation == "create" || Operation == "clone")
	{
		return CreateParticle(Tool, Params, Name, Operation == "clone");
	}
	PS::CPEDef* Effect = Tool.FindPE(Name.c_str());
	if (!Effect)
	{
		return Error("PARTICLE_NOT_FOUND", "The particle effect does not exist");
	}
	if (Operation == "add_action" || Operation == "remove_action")
	{
		return EditParticleActions(Tool, *Effect, Params, Operation == "add_action");
	}
	if (Operation == "set_effect")
	{
		return PatchParticleEffect(Tool, *Effect, Params);
	}
	if (Operation == "set")
	{
		return PatchParticle(Tool, *Effect, Params);
	}
	if (Operation == "save")
	{
		return SaveParticle(*Effect);
	}
	if (Operation != "play")
	{
		return Error("UNKNOWN_PARTICLE_ACTION", "Use a registered Particle Editor action");
	}
	Tool.SelectEffectForEditing(*Effect);
	Tool.PlayCurrent();
	Tool.UpdateProperties();
	return {{"ok", true}, {"name", Text(Effect->Name())}, {"playing", Tool.IsPlaying()}};
}

inline Json Surface(const CSurface& Value, size_t Index)
{
	return {{"surface_index", Index}, {"name", Text(Value._Name())}, {"texture", Text(Value._Texture())},
		{"render_shader", Text(Value._ShaderName())}, {"compiler_shader", Text(Value._ShaderXRLCName())},
		{"game_material", Text(Value._GameMtlName())}, {"shared", Value.UseShared}};
}

inline Json ActorBone(CBone& Bone, size_t Index)
{
	const Fvector Rotation = {rad2deg(Bone.rest_rotate.x), rad2deg(Bone.rest_rotate.y), rad2deg(Bone.rest_rotate.z)};
	return {{"bone_index", Index}, {"name", Text(Bone.name.c_str())}, {"parent", Text(Bone.parent_name.c_str())},
		{"weight_map", Text(Bone.wmap.c_str())}, {"mass", Bone.mass}, {"center_of_mass_m", Vector(Bone.center_of_mass)},
		{"bind_position_m", Vector(Bone.rest_offset)}, {"bind_rotation_degrees", Vector(Rotation)},
		{"game_material", Text(Bone.game_mtl.c_str())}, {"shape_type", Bone.shape.type}};
}

inline Json ActorMotion(CSMotion& Motion, size_t Index)
{
	return {{"motion_index", Index}, {"name", Text(Motion.Name())}, {"frame_start", Motion.FrameStart()},
		{"frame_end", Motion.FrameEnd()}, {"fps", Motion.FPS()}, {"speed", Motion.fSpeed}, {"accrue", Motion.fAccrue},
		{"falloff", Motion.fFalloff}, {"power", Motion.fPower}, {"is_fx", Motion.m_Flags.is(esmFX)},
		{"stop_at_end", Motion.m_Flags.is(esmStopAtEnd)}, {"no_mix", Motion.m_Flags.is(esmNoMix)}, {"sync_part", Motion.m_Flags.is(esmSyncPart)}};
}

inline Json ReadActor(CActorTools& Tool, const Json& Params)
{
	size_t Offset;
	size_t Limit;
	if (!Page(Params, Offset, Limit))
	{
		return Error("INVALID_PAGE", "Use an offset of zero or more and a limit in 1..500");
	}
	CEditableObject* Object = Tool.CurrentObject();
	Json Result = {{"ok", true}, {"loaded", Object != nullptr}, {"path", Text(Tool.GetEditFileName().c_str())}, {"modified", Tool.IsModified()}, {"read_only", Tool.ReadOnly()}};
	if (Object == nullptr)
	{
		return Result;
	}
	const xr_string Section = Params.contains("section") ? String(Params, "section") : "surfaces";
	Json Items = Json::array();
	size_t Total;
	if (Section == "surfaces")
	{
		Total = Object->m_Surfaces.size();
		for (size_t Index = Offset; Index < Total && Items.size() < Limit; ++Index)
		{
			if (CSurface* Entry = Object->m_Surfaces[Index]; Entry && Entry->m_pData.second)
			{
				Items.push_back(Surface(*Entry, Index));
			}
			else
			{
				Items.push_back({{"surface_index", Index}, {"invalid", true}});
			}
		}
	}
	else if (Section == "bones")
	{
		Total = Object->m_Bones.size();
		for (size_t Index = Offset; Index < Total && Items.size() < Limit; ++Index)
		{
			Items.push_back(ActorBone(*Object->m_Bones[Index], Index));
		}
	}
	else if (Section == "motions")
	{
		Total = Object->m_SMotions.size();
		for (size_t Index = Offset; Index < Total && Items.size() < Limit; ++Index)
		{
			Items.push_back(ActorMotion(*Object->m_SMotions[Index], Index));
		}
	}
	else
	{
		return Error("UNKNOWN_ACTOR_SECTION", "Use surfaces, bones or motions");
	}
	Result["section"] = Section.c_str();
	Result["items"] = Items;
	Result["bones"] = Object->m_Bones.size();
	Result["motions"] = Object->m_SMotions.size();
	Result["surfaces"] = Object->m_Surfaces.size();
	SetPage(Result, Offset, Items.size(), Total);
	return Result;
}

inline Json PatchActorSurface(CActorTools& Tool, CEditableObject& Object, const Json& Params)
{
	const Json IndexValue = Params.value("surface_index", Json());
	if (Tool.ReadOnly() || !Integer(IndexValue, 0, 100000000) || IndexValue.get<size_t>() >= Object.m_Surfaces.size())
	{
		return Error("INVALID_ACTOR_SURFACE", "Use a surface index from scene_actor_read for a writable model");
	}
	const size_t Index = IndexValue.get<size_t>();
	CSurface* Entry = Object.m_Surfaces[Index];
	if (!Entry || !Entry->m_pData.second || Entry->IsSharedMode())
	{
		return Error("SHARED_ACTOR_SURFACE", "Use a local surface to avoid changing a shared material");
	}
	xr_string Texture;
	xr_string RenderShader;
	xr_string CompilerShader;
	xr_string Material;
	if (Params.contains("texture") &&
		(!ResourceName(Params, "texture", Texture, 255) || (Texture != "$null" && Texture.find('$') != xr_string::npos) ||
			!AiMaterials::TextureExists(Texture.c_str())))
	{
		return Error("INVALID_TEXTURE", "The texture must exist in the SDK runtime library");
	}
	if (Params.contains("render_shader") &&
		(!ResourceName(Params, "render_shader", RenderShader, 127) || !DEV->_FindBlender(RenderShader.c_str())))
	{
		return Error("INVALID_RENDER_SHADER", "The render shader must exist in the SDK library");
	}
	if (Params.contains("compiler_shader") &&
		(!ResourceName(Params, "compiler_shader", CompilerShader, 127) || !EDevice->ShaderXRLC.Get(CompilerShader.c_str())))
	{
		return Error("INVALID_COMPILER_SHADER", "The compiler shader must exist in the SDK library");
	}
	if (Params.contains("game_material") &&
		(!ResourceName(Params, "game_material", Material, 127) || !GameMaterialLibraryEditors ||
			!GameMaterialLibraryEditors->GetMaterial(Material.c_str())))
	{
		return Error("INVALID_GAME_MATERIAL", "The game material must exist in the SDK library");
	}
	if (Texture.empty() && RenderShader.empty() && CompilerShader.empty() && Material.empty())
	{
		return Error("EMPTY_PATCH", "Specify texture, render_shader, compiler_shader or game_material");
	}
	if (!Texture.empty())
	{
		Entry->SetTexture(Texture.c_str());
		Entry->OnDeviceDestroy();
	}
	if (!RenderShader.empty())
	{
		Entry->SetShader(RenderShader.c_str());
	}
	if (!CompilerShader.empty())
	{
		Entry->SetShaderXRLC(CompilerShader.c_str());
	}
	if (!Material.empty())
	{
		Entry->SetGameMtl(Material.c_str());
	}
	Tool.OnObjectModified();
	Tool.UpdateProperties();
	return {{"ok", true}, {"saved", false}, {"surface", Surface(*Entry, Index)}};
}

inline bool ReadVector(const Json& Data, Fvector& Value, float Min, float Max)
{
	if (!Data.is_array() || Data.size() != 3 || !std::all_of(Data.begin(), Data.end(), [&](const Json& Component)
	{
		return Number(Component, Min, Max);
	}))
	{
		return false;
	}
	Value.set(Data[0].get<float>(), Data[1].get<float>(), Data[2].get<float>());
	return true;
}

inline Json PatchActorBone(CActorTools& Tool, CEditableObject& Object, const Json& Params)
{
	const Json IndexValue = Params.value("bone_index", Json());
	const Json Fields = Params.value("fields", Json::object());
	if (Tool.ReadOnly() || !Integer(IndexValue, 0, 100000000) || IndexValue.get<size_t>() >= Object.m_Bones.size() ||
		!Fields.is_object() || (Fields.empty() && !Params.contains("game_material")))
	{
		return Error("INVALID_ACTOR_BONE", "Specify a writable bone and a field from scene_actor_read");
	}
	const size_t Index = IndexValue.get<size_t>();
	CBone& Bone = *Object.m_Bones[Index];
	float Mass = Bone.mass;
	Fvector Center = Bone.center_of_mass;
	Fvector Position = Bone.rest_offset;
	Fvector Rotation = Bone.rest_rotate;
	xr_string Material;
	for (auto It = Fields.begin(); It != Fields.end(); ++It)
	{
		const auto& Key = It.key();
		if (Key == "mass" && Number(*It, 0, 10000))
		{
			Mass = It->get<float>();
			continue;
		}
		if ((Key == "center_of_mass_m" && ReadVector(*It, Center, -10000, 10000)) ||
			(Key == "bind_position_m" && ReadVector(*It, Position, -10000, 10000)))
		{
			continue;
		}
		if (Key == "bind_rotation_degrees" && ReadVector(*It, Rotation, -360, 360))
		{
			Rotation.set(deg2rad(Rotation.x), deg2rad(Rotation.y), deg2rad(Rotation.z));
			continue;
		}
		Json Result = Error("INVALID_ACTOR_BONE_FIELD", "The bone does not support this field or value");
		Result["field"] = Key;
		return Result;
	}
	if (Params.contains("game_material") &&
		(!ResourceName(Params, "game_material", Material, 127) || !GameMaterialLibraryEditors ||
			!GameMaterialLibraryEditors->GetMaterial(Material.c_str())))
	{
		return Error("INVALID_GAME_MATERIAL", "The game material must exist in the SDK library");
	}
	Tool.PhysicsStopSimulate();
	Bone.mass = Mass;
	Bone.center_of_mass = Center;
	if (!Material.empty())
	{
		Bone.game_mtl = Material.c_str();
	}
	if (Fields.contains("bind_position_m") || Fields.contains("bind_rotation_degrees"))
	{
		Bone.rest_offset = Position;
		Bone.rest_rotate = Rotation;
		Object.OnBindTransformChange();
	}
	Tool.OnBoneModified();
	Tool.UpdateProperties();
	return {{"ok", true}, {"saved", false}, {"bone", ActorBone(Bone, Index)}};
}

struct MotionFloatField
{
	const char* Key;
	float CSMotion::*Value;
};

inline constexpr MotionFloatField MotionFloats[] = {
	{"speed", &CSMotion::fSpeed},
	{"accrue", &CSMotion::fAccrue},
	{"falloff", &CSMotion::fFalloff},
	{"power", &CSMotion::fPower},
};

struct MotionFlagField
{
	const char* Key;
	u8 Mask;
};

inline constexpr MotionFlagField MotionFlags[] = {
	{"stop_at_end", esmStopAtEnd},
	{"no_mix", esmNoMix},
	{"sync_part", esmSyncPart},
};

inline Json PatchActorMotion(CActorTools& Tool, CEditableObject& Object, const Json& Params)
{
	const Json IndexValue = Params.value("motion_index", Json());
	const Json Fields = Params.value("fields", Json::object());
	if (Tool.ReadOnly() || !Object.m_SMotionRefs.empty() || !Integer(IndexValue, 0, 100000000) ||
		IndexValue.get<size_t>() >= Object.m_SMotions.size() || !Fields.is_object() || Fields.empty())
	{
		return Error("INVALID_ACTOR_MOTION", "Specify a writable local motion and fields from scene_actor_read");
	}
	const size_t Index = IndexValue.get<size_t>();
	CSMotion& Motion = *Object.m_SMotions[Index];
	for (auto It = Fields.begin(); It != Fields.end(); ++It)
	{
		const bool ValidFloat = std::any_of(std::begin(MotionFloats), std::end(MotionFloats), [&](const MotionFloatField& Field)
		{
			return It.key() == Field.Key && Number(*It, 0, 10) && (It.key() != "power" || Motion.m_Flags.is(esmFX));
		});
		const bool ValidFlag = !Motion.m_Flags.is(esmFX) && std::any_of(std::begin(MotionFlags), std::end(MotionFlags), [&](const MotionFlagField& Field)
		{
			return It.key() == Field.Key && It->is_boolean();
		});
		if (!ValidFloat && !ValidFlag)
		{
			Json Result = Error("INVALID_ACTOR_MOTION_FIELD", "The motion does not support this field or value");
			Result["field"] = It.key();
			return Result;
		}
	}
	for (const auto& Field : MotionFloats)
	{
		if (Fields.contains(Field.Key))
		{
			Motion.*Field.Value = Fields[Field.Key].get<float>();
		}
	}
	for (const auto& Field : MotionFlags)
	{
		if (Fields.contains(Field.Key))
		{
			Motion.m_Flags.set(Field.Mask, Fields[Field.Key].get<bool>());
		}
	}
	Tool.OnMotionDefsModified();
	Tool.UpdateProperties();
	return {{"ok", true}, {"saved", false}, {"motion", ActorMotion(Motion, Index)}};
}

inline Json SaveActorCopy(CEditableObject& Object, const char* FileName)
{
	const auto Destination = std::filesystem::u8path(FileName);
	std::error_code Code;
	if (!std::filesystem::is_directory(Destination.parent_path(), Code) || Code)
	{
		return Error("ACTOR_DESTINATION_UNAVAILABLE", "The parent folder must exist in the object library");
	}
	if (FS.exist(FileName) || std::filesystem::exists(Destination, Code) || Code)
	{
		return Error("ACTOR_DESTINATION_EXISTS", "Specify a new object reference");
	}
	auto Temporary = Destination;
	Temporary += ".ixray-ai-stage";
	if (!std::filesystem::create_directory(Temporary, Code) || Code)
	{
		return Error("ACTOR_STAGE_FAILED", "The private staging directory is unavailable");
	}
	struct RemoveStage
	{
		const std::filesystem::path& Path;
		~RemoveStage()
		{
			std::error_code Code;
			std::filesystem::remove_all(Path, Code);
		}
	} Remove{Temporary};

	CMemoryWriter Data;
	Data.open_chunk(EEditableObjectChunks::OBJECT_BODY);
	Object.Save(Data);
	Data.close_chunk();
	const auto Stage = Temporary / Destination.filename();
	const auto StageUtf8 = Stage.u8string();
	const char* StageName = reinterpret_cast<const char*>(StageUtf8.c_str());
	if (StageUtf8.size() >= sizeof(string_path) || !Data.save_to(StageName) ||
		std::filesystem::file_size(Stage, Code) != Data.size() || Code || !FS.TryLoad(xr_string(StageName)))
	{
		return Error("ACTOR_SAVE_FAILED", "The SDK could not stage the new object file");
	}
	{
		EditableObjectReadOnlyScope ReadOnlyLoad;
		CEditableObject Reopened(StageName);
		if (!Reopened.Load(StageName) || Reopened.m_Bones.size() != Object.m_Bones.size() ||
			Reopened.m_SMotions.size() != Object.m_SMotions.size() || Reopened.m_Surfaces.size() != Object.m_Surfaces.size())
		{
			return Error("ACTOR_VERIFY_FAILED", "The staged object could not be reopened with all parts");
		}
	}
	if (!std::filesystem::copy_file(Stage, Destination, Code) || Code)
	{
		return Error("ACTOR_SAVE_FAILED", "The SDK could not publish the new object file");
	}
	FS.TryLoad(xr_string(FileName));
	return {{"ok", true}, {"saved", true}, {"verified", "reopened_native_serialization"}, {"path", Text(FileName)}};
}

inline Json EditActor(CActorTools& Tool, const Json& Params)
{
	const xr_string Operation = String(Params, "action");
	if (Operation == "load" || Operation == "save_copy")
	{
		xr_string Reference;
		if (!ResourceName(Params, "reference", Reference, 200) || !FS.path_exist(_objects_))
		{
			return Error("INVALID_ACTOR_REFERENCE", "Specify a relative library reference with .object or .ogf extension");
		}
		const auto Extension = std::filesystem::u8path(Reference.c_str()).extension();
		if (Extension != ".object" && (Operation == "save_copy" || Extension != ".ogf"))
		{
			return Error("INVALID_ACTOR_EXTENSION", "Use .object for save_copy and .object or .ogf for load");
		}
		string_path FileName;
		FS.update_path(FileName, _objects_, Reference.c_str());
		if (Operation == "load")
		{
			if (Tool.IsModified())
			{
				return Error("ACTOR_HAS_UNSAVED_CHANGES", "Save or discard current edits in Actor Editor before loading a different model");
			}
			if (!FS.exist(FileName) || !u32(Tool.CommandLoad(xr_string(FileName))) || Tool.CurrentObject() == nullptr)
			{
				return Error("ACTOR_LOAD_FAILED", "The model could not be loaded from the object library");
			}
			return ReadActor(Tool, Json::object());
		}
		CEditableObject* Object = Tool.CurrentObject();
		return Object ? SaveActorCopy(*Object, FileName) : Error("ACTOR_NO_MODEL", "Load a model in Actor Editor");
	}
	CEditableObject* Object = Tool.CurrentObject();
	if (Object == nullptr)
	{
		return Error("ACTOR_NO_MODEL", "Load a model in Actor Editor");
	}
	if (Operation == "set_surface")
	{
		return PatchActorSurface(Tool, *Object, Params);
	}
	if (Operation == "set_bone")
	{
		return PatchActorBone(Tool, *Object, Params);
	}
	if (Operation == "set_motion")
	{
		return PatchActorMotion(Tool, *Object, Params);
	}
	if (Operation == "undo" || Operation == "redo")
	{
		const bool Changed = Operation == "undo" ? Tool.Undo() : Tool.Redo();
		return {{"ok", true}, {"changed", Changed}};
	}
	if (Operation == "play")
	{
		xr_string Motion;
		if (!ResourceName(Params, "reference", Motion, 200) || !Object->FindSMotionByName(Motion.c_str()))
		{
			return Error("ACTOR_MOTION_NOT_FOUND", "Use reference from the local motions listed by scene_actor_read");
		}
		Tool.SetCurrentMotion(Motion.c_str(), 0);
		Tool.PlayMotion();
		return {{"ok", true}, {"motion", Text(Motion.c_str())}, {"status", "accepted"}};
	}
	if (Operation == "stop")
	{
		Tool.StopMotion();
		return {{"ok", true}, {"status", "accepted"}};
	}
	return Error("UNKNOWN_ACTOR_ACTION", "Use a registered Actor Editor action");
}

struct CompilerFloatField
{
	const char* Key;
	float Shader_xrLC::*Value;
	float Min;
	float Max;
};

inline constexpr CompilerFloatField CompilerFloats[] = {
	{"translucency", &Shader_xrLC::vert_translucency, 0, 1},
	{"ambient", &Shader_xrLC::vert_ambient, 0, 1},
	{"lightmap_density", &Shader_xrLC::lm_density, 0.01f, 20},
};

struct CompilerFlagField
{
	const char* Key;
	u32 Mask;
};

inline constexpr CompilerFlagField CompilerFlags[] = {
	{"collision", Shader_xrLC::flCollision},
	{"rendering", Shader_xrLC::flRendering},
	{"optimize_uv", Shader_xrLC::flOptimizeUV},
	{"vertex_light", Shader_xrLC::flLIGHT_Vertex},
	{"cast_shadow", Shader_xrLC::flLIGHT_CastShadow},
};

inline Json CompilerShader(const Shader_xrLC& Value)
{
	Json Fields = Json::object();
	for (const auto& Field : CompilerFloats)
	{
		Fields[Field.Key] = Value.*Field.Value;
	}
	for (const auto& Field : CompilerFlags)
	{
		Fields[Field.Key] = Value.m_Flags.is(Field.Mask);
	}
	return {{"name", Text(Value.Name)}, {"fields", Fields}};
}

inline Json ReadShaders(CSHCompilerTools& Tool, const Json& Params)
{
	size_t Offset;
	size_t Limit;
	if (!Page(Params, Offset, Limit))
	{
		return Error("INVALID_PAGE", "Use an offset of zero or more and a limit in 1..500");
	}
	const auto& Library = Tool.GetLibrary();
	if (Params.contains("name"))
	{
		xr_string Name;
		if (!ResourceName(Params, "name", Name, 127))
		{
			return Error("INVALID_COMPILER_SHADER", "Use a shader name from the compiler shader list");
		}
		const auto Entry = std::find_if(Library.begin(), Library.end(), [&](const Shader_xrLC& Value)
		{
			return Name == Value.Name;
		});
		if (Entry == Library.end())
		{
			return Error("COMPILER_SHADER_NOT_FOUND", "The compiler shader does not exist");
		}
		return {{"ok", true}, {"shader", CompilerShader(*Entry)}, {"modified", Tool.IsModified()}};
	}
	Json Items = Json::array();
	for (size_t Index = Offset; Index < Library.size() && Items.size() < Limit; ++Index)
	{
		Items.push_back(Text(Library[Index].Name));
	}
	Json Result = {{"ok", true}, {"items", Items}, {"modified", Tool.IsModified()}};
	SetPage(Result, Offset, Items.size(), Library.size());
	return Result;
}

inline Json EditShaders(CShaderTool& Editor, CSHCompilerTools& Tool, const Json& Params)
{
	const xr_string Operation = String(Params, "action");
	if (Operation == "undo" || Operation == "redo")
	{
		if (Editor.Current() != &Tool)
		{
			Editor.OnChangeEditor(&Tool);
		}
		const bool Changed = Operation == "undo" ? Tool.Undo() : Tool.Redo();
		return {{"ok", true}, {"changed", Changed}};
	}
	if (Operation == "save")
	{
		if (!Tool.Save())
		{
			return Error("SHADER_SAVE_FAILED", "The SDK could not write the compiler shader library");
		}
		string_path Path;
		FS.update_path(Path, _game_data_, "shaders_xrlc.xr");
		const auto& Library = Tool.GetLibrary();
		std::ifstream Saved(std::filesystem::u8path(Path), std::ios::binary);
		Shader_xrLC Record;
		for (const Shader_xrLC& Entry : Library)
		{
			if (!Saved.read(reinterpret_cast<char*>(&Record), sizeof(Record)) ||
				std::memcmp(&Entry, &Record, sizeof(Record)) != 0)
			{
				return Error("SHADER_VERIFY_FAILED", "The saved compiler shader library could not be verified");
			}
		}
		if (!Saved || Saved.peek() != std::char_traits<char>::eof())
		{
			return Error("SHADER_VERIFY_FAILED", "The saved compiler shader library length did not match");
		}
		return {{"ok", true}, {"saved", true}, {"verified", true}, {"path", Text(Path)}, {"total", Library.size()}};
	}
	if (Operation != "set")
	{
		return Error("UNKNOWN_SHADER_ACTION", "Use set, save, undo or redo");
	}
	xr_string Name;
	const Json Fields = Params.value("fields", Json::object());
	if (!ResourceName(Params, "name", Name, 127) || !Fields.is_object() || Fields.empty() || Fields.size() > 8)
	{
		return Error("INVALID_SHADER_PATCH", "Provide an existing compiler shader name and editable fields");
	}
	for (auto It = Fields.begin(); It != Fields.end(); ++It)
	{
		const auto Float = std::find_if(std::begin(CompilerFloats), std::end(CompilerFloats), [&](const CompilerFloatField& Field)
		{
			return It.key() == Field.Key;
		});
		const auto Flag = std::find_if(std::begin(CompilerFlags), std::end(CompilerFlags), [&](const CompilerFlagField& Field)
		{
			return It.key() == Field.Key;
		});
		if ((Float != std::end(CompilerFloats) && Number(*It, Float->Min, Float->Max)) ||
			(Flag != std::end(CompilerFlags) && It->is_boolean()))
		{
			continue;
		}
		Json Result = Error("INVALID_COMPILER_SHADER_FIELD", "The compiler shader does not support the field, value type, or range");
		Result["field"] = It.key();
		return Result;
	}
	if (Editor.Current() != &Tool)
	{
		Editor.OnChangeEditor(&Tool);
	}
	Tool.SetCurrentItem(Name.c_str(), true);
	Shader_xrLC* Entry = Tool.m_Shader;
	if (!Entry || Name != Entry->Name)
	{
		return Error("COMPILER_SHADER_NOT_FOUND", "The compiler shader does not exist in the loaded editor library");
	}
	for (const auto& Field : CompilerFloats)
	{
		if (Fields.contains(Field.Key))
		{
			Entry->*Field.Value = Fields[Field.Key].get<float>();
		}
	}
	for (const auto& Field : CompilerFlags)
	{
		if (Fields.contains(Field.Key))
		{
			Entry->m_Flags.set(Field.Mask, Fields[Field.Key].get<bool>());
		}
	}
	Tool.Modified();
	Editor.UpdateProperties();
	return {{"ok", true}, {"saved", false}, {"shader", CompilerShader(*Entry)}};
}

inline Json Execute(const xr_string& Method, const Json& Params)
{
	if (Method == "scene.particle_read" || Method == "scene.particle_edit")
	{
		CParticleTool* Tool = UIParticleEditorForm::GetActiveTool();
		if (!Tool)
		{
			return Error("PARTICLE_TAB_REQUIRED", "Open Particle Editor with scene_sdk_tab first");
		}
		return Method == "scene.particle_read" ? ReadParticles(*Tool, Params) : EditParticles(*Tool, Params);
	}
	if (Method == "scene.actor_read" || Method == "scene.actor_edit")
	{
		CActorTools* Tool = UIActorEditorForm::GetActiveTool();
		if (!Tool)
		{
			return Error("ACTOR_TAB_REQUIRED", "Open Actor Editor with scene_sdk_tab first");
		}
		return Method == "scene.actor_read" ? ReadActor(*Tool, Params) : EditActor(*Tool, Params);
	}
	CShaderTool* Editor = UIShaderEditorForm::GetActiveTool();
	if (!Editor)
	{
		return Error("SHADER_TAB_REQUIRED", "Open Shader Editor with scene_sdk_tab first");
	}
	auto* Tool = static_cast<CSHCompilerTools*>(Editor->FindTools(aeCompiler));
	if (!Tool)
	{
		return Error("COMPILER_SHADER_LIBRARY_UNAVAILABLE", "The compiler shader library is unavailable");
	}
	return Method == "scene.shader_read" ? ReadShaders(*Tool, Params) : EditShaders(*Editor, *Tool, Params);
}
} // namespace AiContentEditors
