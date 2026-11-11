#pragma once

#include "Editor/Entry/Light/ELight.h"
#include "Editor/Entry/Sound/ESound_Source.h"
#include "Editor/Entry/Spawn/SpawnPoint.h"
#include "Editor/Entry/Terrain/Terrain.h"
#include "Editor/Tools/Details/ESceneDOTools.h"
#include "Editor/Tools/Spawn/ESceneSpawnTools.h"
#include "../../../../xrServerEntities/xrServer_Objects.h"

namespace AiLevelEntity
{
struct Field
{
	const char* Key;
	float* Value;
	float Min;
	float Max;
};

inline bool ApplyNumbers(const AiJson& Changes, std::initializer_list<Field> Fields, bool Write)
{
	for (const Field& Entry : Fields)
	{
		auto Item = Changes.find(Entry.Key);
		if (Item == Changes.end())
		{
			continue;
		}
		if (!Item->is_number())
		{
			return false;
		}
		const double Number = Item->get<double>();
		if (!std::isfinite(Number) || Number < Entry.Min || Number > Entry.Max)
		{
			return false;
		}
		if (Write)
		{
			*Entry.Value = float(Number);
		}
	}
	return true;
}

inline bool OnlyKeys(const AiJson& Changes, std::initializer_list<const char*> Keys)
{
	if (!Changes.is_object() || Changes.empty())
	{
		return false;
	}
	for (const auto& Item : Changes.items())
	{
		if (std::none_of(Keys.begin(), Keys.end(), [&](const char* Key) { return Item.key() == Key; }))
		{
			return false;
		}
	}
	return true;
}

inline bool Unsigned(const AiJson& Params, const char* Key, u32 Min, u32 Max, u32& Number)
{
	auto Item = Params.find(Key);
	if (Item == Params.end() || !Item->is_number_integer())
	{
		return false;
	}
	const double Value = Item->get<double>();
	if (Value < Min || Value > Max)
	{
		return false;
	}
	Number = u32(Value);
	return true;
}

inline bool SoundResource(const AiJson& Data, const char* Key, xr_string& Name)
{
	if (!AiHasString(Data, Key, 200))
	{
		return false;
	}
	Name = AiString(Data, Key);
	if (Name.empty() || Name.find("..") != xr_string::npos || Name.front() == '/' || Name.front() == '\\')
	{
		return false;
	}
	for (char& Character : Name)
	{
		if (Character == '/')
		{
			Character = '\\';
		}
		if ((u8)Character < 32 || Character == ':' || Character == '?' || Character == '*' || Character == '"' || Character == '<' || Character == '>' || Character == '|')
		{
			return false;
		}
	}
	if (Name.ends_with(".ogg"))
	{
		Name.resize(Name.size() - 4);
	}
	string_path Path;
	return !Name.empty() && FS.path_exist(_game_sounds_) && FS.exist(Path, _game_sounds_, Name.c_str(), ".ogg");
}

inline bool SpawnSection(const xr_string& Section)
{
	auto* Tool = smart_cast<ESceneSpawnTool*>(Scene->GetTool(OBJCLASS_SPAWNPOINT));
	if (Section.empty() || Tool == nullptr)
	{
		return false;
	}
	if (Section == RPOINT_CHOOSE_NAME || Section == ENVMOD_CHOOSE_NAME)
	{
		return true;
	}
	for (const auto& [Id, Entries] : Tool->m_Classes)
	{
		for (const SChooseItem& Entry : Entries)
		{
			if (Entry.hint.c_str() && Section == Entry.hint.c_str())
			{
				return true;
			}
		}
	}
	return false;
}

inline AiJson LightDetails(const CLight& Light)
{
	return {{"kind", "light"}, {"type_id", Light.m_Type}, {"brightness", Light.m_Brightness}, {"range_m", Light.m_Range},
		{"color_rgba", {Light.m_Color.r, Light.m_Color.g, Light.m_Color.b, Light.m_Color.a}},
		{"attenuation", {Light.m_Attenuation0, Light.m_Attenuation1, Light.m_Attenuation2}},
		{"affect_static", Light.m_Flags.is(ELight::flAffectStatic)}, {"affect_dynamic", Light.m_Flags.is(ELight::flAffectDynamic)},
		{"use_in_d3d", Light.m_UseInD3D}};
}

inline bool SetLight(CLight& Light, const AiJson& Changes, bool Write)
{
	if (!OnlyKeys(Changes, {"brightness", "range_m", "color_rgba", "attenuation", "affect_static", "affect_dynamic", "use_in_d3d"}) ||
		!ApplyNumbers(Changes, {{"brightness", &Light.m_Brightness, -3, 3}, {"range_m", &Light.m_Range, 0.1f, 1000}}, Write))
	{
		return false;
	}
	for (const char* Key : {"affect_static", "affect_dynamic", "use_in_d3d"})
	{
		if (Changes.contains(Key) && !Changes[Key].is_boolean())
		{
			return false;
		}
	}
	auto Color = Changes.find("color_rgba");
	if (Color != Changes.end())
	{
		if (!Color->is_array() || Color->size() != 4 || !std::all_of(Color->begin(), Color->end(), [](const auto& Item)
			{
				return Item.is_number() && Item.template get<double>() >= 0 && Item.template get<double>() <= 1;
			}))
		{
			return false;
		}
		if (Write)
		{
			Light.m_Color.set((*Color)[0].get<float>(), (*Color)[1].get<float>(), (*Color)[2].get<float>(), (*Color)[3].get<float>());
		}
	}
	Fvector Attenuation;
	if (!AiVector(Changes, "attenuation", Attenuation, 0, 1) || Attenuation.x < 0 || Attenuation.y < 0 || Attenuation.z < 0)
	{
		return false;
	}
	if (Write)
	{
		if (Changes.contains("attenuation"))
		{
			Light.m_Attenuation0 = Attenuation.x;
			Light.m_Attenuation1 = Attenuation.y;
			Light.m_Attenuation2 = Attenuation.z;
		}
		Light.m_Flags.set(ELight::flAffectStatic, Changes.value("affect_static", Light.m_Flags.is(ELight::flAffectStatic) != 0));
		Light.m_Flags.set(ELight::flAffectDynamic, Changes.value("affect_dynamic", Light.m_Flags.is(ELight::flAffectDynamic) != 0));
		Light.m_UseInD3D = Changes.value("use_in_d3d", Light.m_UseInD3D);
		Light.Update();
	}
	return true;
}

inline AiJson SpawnDetails(CSpawnPoint& Spawn)
{
	if (CSE_Abstract* Entity = Spawn.m_SpawnData.GetEntity())
	{
		return {{"kind", "spawn"}, {"section", AiText::Utf8(Entity->name()).c_str()}, {"respawn_seconds", Entity->RespawnTime},
			{"custom_data", AiText::Utf8(Entity->m_ini_string.c_str()).c_str()}, {"has_shape", Spawn.m_AttachedObject != nullptr}};
	}
	if (Spawn.m_Type == ptRPoint)
	{
		return {{"kind", "spawn"}, {"section", RPOINT_CHOOSE_NAME}, {"team", Spawn.m_RP_TeamID}, {"spawn_type_id", Spawn.m_RP_Type}};
	}
	if (Spawn.m_Type == ptEnvMod)
	{
		return {{"kind", "spawn"}, {"section", ENVMOD_CHOOSE_NAME}, {"radius_m", Spawn.m_EM_Radius},
			{"power", Spawn.m_EM_Power}, {"view_distance_m", Spawn.m_EM_ViewDist}, {"fog_density", Spawn.m_EM_FogDensity}};
	}
	return {{"kind", "spawn"}, {"valid", false}};
}

inline bool SetSpawn(CSpawnPoint& Spawn, const AiJson& Changes, bool Write)
{
	if (CSE_Abstract* Entity = Spawn.m_SpawnData.GetEntity())
	{
		const bool HasCustomData = Changes.contains("custom_data");
		const auto Custom = Changes.find("custom_data");
		u32 Seconds = 0;
		if (!OnlyKeys(Changes, {"respawn_seconds", "custom_data"}) ||
			(Changes.contains("respawn_seconds") && !Unsigned(Changes, "respawn_seconds", 0, 65535, Seconds)) ||
			(HasCustomData && (!AiHasString(Changes, "custom_data", 1024) ||
				Custom->get_ref<const AiJson::string_t&>().find('\0') != AiJson::string_t::npos || !Entity->cast_alife_object())))
		{
			return false;
		}
		if (Write && Changes.contains("respawn_seconds"))
		{
			Entity->RespawnTime = u16(Seconds);
		}
		if (Write && HasCustomData)
		{
			Entity->m_ini_string = AiString(Changes, "custom_data").c_str();
			xr_delete(Entity->m_ini_file);
		}
		return true;
	}
	if (Spawn.m_Type == ptRPoint)
	{
		u32 Team = 0;
		if (!OnlyKeys(Changes, {"team"}) || !Unsigned(Changes, "team", 0, 32, Team))
		{
			return false;
		}
		if (Write)
		{
			Spawn.m_RP_TeamID = u8(Team);
		}
		return true;
	}
	return Spawn.m_Type == ptEnvMod && OnlyKeys(Changes, {"radius_m", "power", "view_distance_m", "fog_density"}) &&
		ApplyNumbers(Changes, {{"radius_m", &Spawn.m_EM_Radius, EPS_L, 10000}, {"power", &Spawn.m_EM_Power, EPS, 1000},
			{"view_distance_m", &Spawn.m_EM_ViewDist, EPS_L, 10000}, {"fog_density", &Spawn.m_EM_FogDensity, 0, 10000}}, Write);
}
} // namespace AiLevelEntity

class AiLevelEntityAccess
{
public:
	static AiJson SoundDetails(const ESoundSource& Sound)
	{
		return {{"kind", "sound"}, {"source", AiText::Utf8(Sound.m_WAVName.c_str()).c_str()}, {"volume", Sound.m_Params.volume},
			{"frequency", Sound.m_Params.freq}, {"min_distance_m", Sound.m_Params.min_distance}, {"max_distance_m", Sound.m_Params.max_distance},
			{"max_ai_distance_m", Sound.m_Params.max_ai_distance}, {"active_time_s", {Sound.m_ActiveTime.x, Sound.m_ActiveTime.y}},
			{"play_time_s", {Sound.m_PlayTime.x, Sound.m_PlayTime.y}}, {"pause_time_s", {Sound.m_RandomPause.x, Sound.m_RandomPause.y}}};
	}

	static bool SetSound(ESoundSource& Sound, const AiJson& Changes, bool Write)
	{
		using namespace AiLevelEntity;
		xr_string Source;
		if (!OnlyKeys(Changes, {"source", "volume", "frequency", "active_time_s", "play_time_s", "pause_time_s"}) ||
			(Changes.contains("source") && !SoundResource(Changes, "source", Source)) ||
			!ApplyNumbers(Changes, {{"volume", &Sound.m_Params.volume, 0, 1}, {"frequency", &Sound.m_Params.freq, 0.001f, 2}}, Write))
		{
			return false;
		}
		if (Write && !Source.empty() && (!Sound.m_WAVName.c_str() || Source != Sound.m_WAVName.c_str()))
		{
			Sound.m_WAVName = Source.c_str();
			Sound.OnChangeWAV(nullptr);
		}
		for (const auto& [Key, Times] : {std::pair{"active_time_s", &Sound.m_ActiveTime}, {"play_time_s", &Sound.m_PlayTime}, {"pause_time_s", &Sound.m_RandomPause}})
		{
			auto Range = Changes.find(Key);
			if (Range == Changes.end())
			{
				continue;
			}
			if (!Range->is_array() || Range->size() != 2 || !(*Range)[0].is_number() || !(*Range)[1].is_number())
			{
				return false;
			}
			const double Start = (*Range)[0].get<double>();
			const double End = (*Range)[1].get<double>();
			if (!std::isfinite(Start) || !std::isfinite(End) || Start < 0 || End < Start || End > 86400)
			{
				return false;
			}
			if (Write)
			{
				Times->set(float(Start), float(End));
			}
		}
		if (Write)
		{
			Sound.OnChangeSource(nullptr);
		}
		return true;
	}

	static AiJson DetailModelDetails(EDetail& Model)
	{
		return {{"name", AiText::Utf8(Model.GetName()).c_str()}, {"density_factor", Model.m_fDensityFactor},
			{"min_scale", Model.m_fMinScale}, {"max_scale", Model.m_fMaxScale}, {"no_waving", Model.m_Flags.is(DO_NO_WAVING)}};
	}

	static bool SetDetailModel(EDetail& Model, const AiJson& Changes, bool Write)
	{
		using namespace AiLevelEntity;
		float Density = Model.m_fDensityFactor;
		float MinScale = Model.m_fMinScale;
		float MaxScale = Model.m_fMaxScale;
		if (!OnlyKeys(Changes, {"density_factor", "min_scale", "max_scale", "no_waving"}) ||
			!ApplyNumbers(Changes, {{"density_factor", &Density, 0.1f, 1}, {"min_scale", &MinScale, 0.1f, 100},
				{"max_scale", &MaxScale, 0.1f, 100}}, true) || MinScale > MaxScale ||
			(Changes.contains("no_waving") && !Changes["no_waving"].is_boolean()))
		{
			return false;
		}
		if (Write)
		{
			Model.m_fDensityFactor = Density;
			Model.m_fMinScale = MinScale;
			Model.m_fMaxScale = MaxScale;
			Model.m_Flags.set(DO_NO_WAVING, Changes.value("no_waving", Model.m_Flags.is(DO_NO_WAVING) != 0));
		}
		return true;
	}

	static AiJson TerrainDetails(const CTerrain& Terrain, const AiJson& Params)
	{
		using namespace AiLevelEntity;
		const auto& Map = Terrain.HMap;
		AiJson Samples = AiJson::array();
		if (Params.contains("samples"))
		{
			const AiJson& Requested = Params["samples"];
			if (!Requested.is_array() || Requested.size() > 4096 || !Map.Data || Map.Width == 0 || Map.Height == 0)
			{
				return nullptr;
			}
			for (const AiJson& Point : Requested)
			{
				u32 X, Z;
				if (!Point.is_object() || !Unsigned(Point, "x", 0, Map.Width - 1, X) || !Unsigned(Point, "z", 0, Map.Height - 1, Z))
				{
					return nullptr;
				}
				Samples.push_back({{"x", X}, {"z", Z}, {"height", Map.GetHeight(X, Z)}});
			}
		}
		const auto Surface = Terrain.SurfaceTemplate();
		return {{"kind", "terrain"}, {"width", Map.Width}, {"height", Map.Height}, {"height_multiplier", Terrain.ScaleY},
			{"texture", AiText::Utf8(Surface.Texture).c_str()}, {"render_shader", AiText::Utf8(Surface.Shader).c_str()},
			{"compiler_shader", AiText::Utf8(Surface.ShaderXRLC).c_str()}, {"game_material", AiText::Utf8(Surface.GameMtl).c_str()},
			{"samples", Samples}};
	}

	static bool SetTerrain(CTerrain& Terrain, const AiJson& Changes, bool Write)
	{
		using namespace AiLevelEntity;
		auto& Map = Terrain.HMap;
		if (!OnlyKeys(Changes, {"heights"}) || !Changes["heights"].is_array() || Changes["heights"].empty() ||
			Changes["heights"].size() > 4096 || !Map.Data || Map.Width < 2 || Map.Height < 2)
		{
			return false;
		}
		xr_hash_map<size_t, float> Updates;
		for (const AiJson& Point : Changes["heights"])
		{
			u32 X, Z;
			if (!OnlyKeys(Point, {"x", "z", "height"}) || !Unsigned(Point, "x", 0, Map.Width - 1, X) ||
				!Unsigned(Point, "z", 0, Map.Height - 1, Z) || !Point.contains("height") || !Point["height"].is_number())
			{
				return false;
			}
			const double Height = Point["height"].get<double>();
			const size_t Index = size_t(Z) * Map.Width + X;
			if (!std::isfinite(Height) || Height < 0 || Height > 1 || !Updates.emplace(Index, float(Height)).second)
			{
				return false;
			}
			if (Write)
			{
				Map.Data[Index] = float(Height);
			}
		}
		if (Write)
		{
			const auto Range = std::minmax_element(Map.Data, Map.Data + size_t(Map.Width) * Map.Height);
			Map.MinH = *Range.first;
			Map.MaxH = *Range.second;
			Terrain.RebuildMesh();
		}
		return true;
	}
};

AiSceneExecutor::Json AiSceneExecutor::DispatchLevelEntity(const xr_string& Method, const Json& Params)
{
	using namespace AiLevelEntity;
	if (Method == "scene.spawn_sections")
	{
		size_t Offset, Limit;
		auto* Tool = smart_cast<ESceneSpawnTool*>(Scene->GetTool(OBJCLASS_SPAWNPOINT));
		if (!Tool || !AiPage(Params, Offset, Limit) || (Params.contains("query") && !AiHasString(Params, "query", 128)))
		{
			return Error("INVALID_PARAMS", "Spawn tool or query unavailable");
		}
		const xr_string Query = AiString(Params, "query");
		Json Sections = Json::array();
		size_t Count = 0;
		auto Add = [&](const char* Name, const char* Caption)
		{
			if (Name && (AiContains(Name, Query) || AiContains(Caption, Query)) && Count++ >= Offset && Sections.size() < Limit)
			{
				Sections.push_back({{"section", AiText::Utf8(Name).c_str()}, {"caption", AiText::Utf8(Caption).c_str()}});
			}
		};
		Add(RPOINT_CHOOSE_NAME, "Respawn point");
		Add(ENVMOD_CHOOSE_NAME, "Environment modifier");
		for (const auto& [Id, Entries] : Tool->m_Classes)
		{
			for (const SChooseItem& Entry : Entries)
			{
				Add(Entry.hint.c_str(), Entry.name.c_str());
			}
		}
		const size_t Next = std::min(Offset, Count) + Sections.size();
		return {{"ok", true}, {"sections", Sections}, {"next_offset", Next}, {"has_more", Next < Count}, {"total", Count}};
	}
	if (Method == "scene.entity_create")
	{
		const xr_string Kind = AiString(Params, "kind");
		ObjClassID Class = OBJCLASS_DUMMY;
		xr_string Section, Source;
		u32 Width = 129, Height = 129;
		float Fill = 0.5f;
		if (Kind == "light")
		{
			Class = OBJCLASS_LIGHT;
		}
		else if (Kind == "spawn" && AiHasString(Params, "section", 200) && SpawnSection(Section = AiString(Params, "section")))
		{
			Class = OBJCLASS_SPAWNPOINT;
		}
		else if (Kind == "sound" && SoundResource(Params, "sound", Source))
		{
			Class = OBJCLASS_SOUND_SRC;
		}
		else if (Kind == "terrain" && (!Params.contains("width") || Unsigned(Params, "width", 2, 513, Width)) &&
			(!Params.contains("height") || Unsigned(Params, "height", 2, 513, Height)) &&
			ApplyNumbers(Params, {{"fill", &Fill, 0.001f, 1}}, true))
		{
			Class = OBJCLASS_TERRAIN;
		}
		if (Class == OBJCLASS_DUMMY)
		{
			return Error("INVALID_ENTITY", "Choose light, an installed spawn section or sound, or a valid terrain grid");
		}
		Fvector Position;
		if (!AiVector(Params, "position_m", Position, 0, 1000000))
		{
			return Error("INVALID_TRANSFORM", "Supply a finite position in metres");
		}
		string256 Name;
		if (Params.contains("name"))
		{
			const xr_string Requested = AiString(Params, "name");
			if (!AiHasString(Params, "name", 128) || Requested.empty() ||
				Requested.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != xr_string::npos ||
				Scene->FindObjectByName(Requested.c_str(), static_cast<CCustomObject*>(nullptr)))
			{
				return Error("INVALID_NAME", "Use a unique lowercase ASCII object name of at most 128 characters");
			}
			xr_strcpy(Name, sizeof(Name), Requested.c_str());
		}
		else
		{
			Scene->GenObjectName(Class, Name, Kind.c_str());
		}
		ESceneCustomOTool* Tool = Scene->GetOTool(Class);
		if (!Tool || !Tool->IsEnabled() || !Tool->IsEditable())
		{
			return Error("TOOL_NOT_EDITABLE", "The entity tool is disabled or locked");
		}
		AiSceneSnapshot Before;
		CCustomObject* Object = Tool->CreateObject(Section.empty() ? nullptr : Section.data(), Name);
		if (!Object || !Object->Valid())
		{
			xr_delete(Object);
			return Error("ENTITY_CREATE_FAILED", "SDK could not create this entity");
		}
		Object->SetPosition(Position);
		if (Class == OBJCLASS_SOUND_SRC)
		{
			static_cast<ESoundSource*>(Object)->SetSourceWAV(Source.c_str());
		}
		if (Class == OBJCLASS_TERRAIN)
		{
			static_cast<CTerrain*>(Object)->InitializeHeightmap(Width, Height, Fill);
		}
		Scene->AppendObject(Object, false);
		Object->Select(true);
		Json Result = {{"created", Describe(Object)}};
		FinishChange(Before, Result);
		return Result;
	}

	const bool Write = Method == "scene.entity_set";
	const Json Empty = Json::object();
	const Json& Changes = Write && Params.contains("changes") ? Params["changes"] : Empty;
	if (Params.contains("detail_tool") && !Params["detail_tool"].is_boolean())
	{
		return Error("INVALID_PARAMS", "detail_tool must be a boolean");
	}
	if (Params.value("detail_tool", false))
	{
		if (Params.contains("object_id") || Params.contains("samples"))
		{
			return Error("INVALID_PARAMS", "The detail tool does not use an object ID or terrain samples");
		}
		auto* Tool = smart_cast<EDetailManager*>(Scene->GetTool(OBJCLASS_DO));
		if (!Tool || !Tool->IsEnabled())
		{
			return Error("TOOL_UNAVAILABLE", "Enable the detail objects tool");
		}
		if (Params.contains("detail_model"))
		{
			const xr_string Name = AiString(Params, "detail_model");
			EDetail* Model = AiHasString(Params, "detail_model", 200) && !Name.empty() ? Tool->FindDOByName(Name.c_str()) : nullptr;
			if (!Model)
			{
				return Error("UNKNOWN_DETAIL_MODEL", "Use a registered detail model name from entity_get");
			}
			if (!Write)
			{
				return {{"ok", true}, {"kind", "detail_tool"}, {"model", AiLevelEntityAccess::DetailModelDetails(*Model)}};
			}
			if (!Tool->IsEditable() || !AiLevelEntityAccess::SetDetailModel(*Model, Changes, false))
			{
				return Error("INVALID_DETAIL_CHANGE", "Use an editable detail tool and valid model settings");
			}
			Device.DetailsTask.wait();
			AiSceneSnapshot Before;
			AiLevelEntityAccess::SetDetailModel(*Model, Changes, true);
			Tool->InvalidateCache();
			Json Result = {{"model", AiLevelEntityAccess::DetailModelDetails(*Model)}};
			FinishChange(Before, Result);
			return Result;
		}
		if (!Write)
		{
			Json Models = Json::array();
			for (CDetail* Model : Tool->objects)
			{
				if (Models.size() >= 128)
				{
					break;
				}
				Models.push_back(AiLevelEntityAccess::DetailModelDetails(*static_cast<EDetail*>(Model)));
			}
			return {{"ok", true}, {"kind", "detail_tool"}, {"density", ps_current_detail_density}, {"has_data", Tool->Valid()},
				{"snap_object_count", Tool->m_SnapObjects.size()}, {"color_group_count", Tool->m_ColorIndices.size()},
				{"models", Models}, {"model_count", Tool->objects.size()}};
		}
		float Density = ps_current_detail_density;
		if (!Tool->IsEditable() || !Tool->Valid() || !OnlyKeys(Changes, {"density"}) ||
			!ApplyNumbers(Changes, {{"density", &Density, 0.15f, 1}}, true))
		{
			return Error("INVALID_DETAIL_CHANGE", "Use an editable detail tool with data and a density from 0.15 to 1");
		}
		if (Density == ps_current_detail_density)
		{
			return {{"ok", true}, {"changed", false}, {"density", Density}};
		}
		Device.DetailsTask.wait();
		AiSceneSnapshot Before;
		ps_current_detail_density = Density;
		Tool->OnDensityChange(nullptr);
		Json Result = {{"density", ps_current_detail_density}};
		FinishChange(Before, Result);
		return Result;
	}

	if (Params.contains("detail_model") || !AiHasString(Params, "object_id", 128) || AiString(Params, "object_id").empty() || Params.contains("ids"))
	{
		return Error("INVALID_PARAMS", "Provide one supported object ID");
	}
	xr_vector<CCustomObject*> Targets;
	Json Failure;
	if (!Resolve(Params, Targets, Failure, Write))
	{
		return Failure;
	}
	CCustomObject* Object = Targets.front();
	auto DescribeDetails = [&](const Json& Query) -> Json
	{
		switch (Object->FClassID)
		{
		case OBJCLASS_LIGHT: return LightDetails(*static_cast<CLight*>(Object));
		case OBJCLASS_SOUND_SRC: return AiLevelEntityAccess::SoundDetails(*static_cast<ESoundSource*>(Object));
		case OBJCLASS_SPAWNPOINT: return SpawnDetails(*static_cast<CSpawnPoint*>(Object));
		case OBJCLASS_TERRAIN: return AiLevelEntityAccess::TerrainDetails(*static_cast<CTerrain*>(Object), Query);
		default: return nullptr;
		}
	};
	if (!Write)
	{
		const Json Details = DescribeDetails(Params);
		if (Details.is_null())
		{
			return Error("INVALID_ENTITY_QUERY", "Unsupported entity class or invalid terrain sample coordinates");
		}
		return {{"ok", true}, {"object", Describe(Object)}, {"details", Details}};
	}
	auto Apply = [&](bool WriteFields)
	{
		switch (Object->FClassID)
		{
		case OBJCLASS_LIGHT: return SetLight(*static_cast<CLight*>(Object), Changes, WriteFields);
		case OBJCLASS_SOUND_SRC: return AiLevelEntityAccess::SetSound(*static_cast<ESoundSource*>(Object), Changes, WriteFields);
		case OBJCLASS_SPAWNPOINT: return SetSpawn(*static_cast<CSpawnPoint*>(Object), Changes, WriteFields);
		case OBJCLASS_TERRAIN: return AiLevelEntityAccess::SetTerrain(*static_cast<CTerrain*>(Object), Changes, WriteFields);
		default: return false;
		}
	};
	if (!Apply(false))
	{
		return Error("INVALID_ENTITY_CHANGE", "Unsupported entity fields, value bounds, coordinates, or resource");
	}
	AiSceneSnapshot Before;
	Scene->BeforeObjectChange(Object);
	if (!Apply(true))
	{
		Before.Restore();
		Synchronize();
		return Error("ENTITY_CHANGE_FAILED", "Could not apply the SDK change");
	}
	Json Query = Json::object();
	if (Changes.contains("heights"))
	{
		Query["samples"] = Changes["heights"];
	}
	Json Result = {{"object", Describe(Object)}, {"details", DescribeDetails(Query)}};
	FinishChange(Before, Result);
	return Result;
}
