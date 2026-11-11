#pragma once

#include "Editor/Entry/Light/ELight.h"
#include "Editor/Entry/Sound/ESound_Source.h"
#include "Editor/Entry/Spawn/SpawnPoint.h"
#include "Editor/Entry/Planars/planar.h"
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

inline bool ResourceName(const AiJson& Data, const char* Key, xr_string& Name)
{
	if (!AiHasString(Data, Key, 200))
	{
		return false;
	}
	const auto& Raw = Data[Key].get_ref<const AiJson::string_t&>();
	Name = AiString(Data, Key);
	if (Name.empty() || Name.size() != Raw.size() || Name.find("..") != xr_string::npos ||
		Name.front() == '/' || Name.front() == '\\' || Name.back() == '/' || Name.back() == '\\')
	{
		return false;
	}
	for (char& Character : Name)
	{
		if ((u8)Character < 32 || Character == ':' || Character == '?' || Character == '*' || Character == '"' || Character == '<' || Character == '>' || Character == '|')
		{
			return false;
		}
		if (Character == '/')
		{
			Character = '\\';
		}
	}
	return true;
}

inline bool SoundResource(const AiJson& Data, const char* Key, xr_string& Name)
{
	if (!ResourceName(Data, Key, Name))
	{
		return false;
	}
	if (Name.ends_with(".ogg"))
	{
		Name.resize(Name.size() - 4);
	}
	string_path Path;
	return !Name.empty() && FS.path_exist(_game_sounds_) && FS.exist(Path, _game_sounds_, Name.c_str(), ".ogg");
}

inline bool ParticleResource(const AiJson& Data, const char* Key, xr_string& Name)
{
	if (!ResourceName(Data, Key, Name))
	{
		return false;
	}
	auto& Library = RImplementation.PSLibrary;
	if (Library.FindPED(Name.c_str()))
	{
		return true;
	}
	const PS::CPGDef* Group = Library.FindPGD(Name.c_str());
	if (!Group)
	{
		return false;
	}
	using Effect = PS::CPGDef::SEffect;
	auto EffectExists = [&](const shared_str& Reference)
	{
		return Reference.size() != 0 && Library.FindPED(Reference.c_str()) != nullptr;
	};
	for (const Effect* Entry : Group->m_Effects)
	{
		if (!Entry || !EffectExists(Entry->m_EffectName) ||
			(Entry->m_Flags.is(Effect::flOnPlayChild) && !EffectExists(Entry->m_OnPlayChildName)) ||
			(Entry->m_Flags.is(Effect::flOnBirthChild) && !EffectExists(Entry->m_OnBirthChildName)) ||
			(Entry->m_Flags.is(Effect::flOnDeadChild) && !EffectExists(Entry->m_OnDeadChildName)))
		{
			return false;
		}
	}
	return true;
}

inline bool SoundEnvironmentName(const AiJson& Data, const char* Key, xr_string& Name)
{
	if (!AiHasString(Data, Key, 200))
	{
		return false;
	}
	const auto& Raw = Data[Key].get_ref<const AiJson::string_t&>();
	if (Raw.empty())
	{
		Name.clear();
		return true;
	}
	if (Raw.find('\0') != AiJson::string_t::npos || !SndLib)
	{
		return false;
	}
	AStringVec Entries;
	SndLib->GetSoundEnvs(Entries);
	for (const xr_string& Entry : Entries)
	{
		if (AiText::Utf8(Entry.c_str()).c_str() == Raw)
		{
			Name = Entry;
			return true;
		}
	}
	return false;
}

inline bool BoxSize(const AiJson& Data, Fvector& Size)
{
	return AiVector(Data, "size_m", Size, 1, 100000) && Size.x >= 0.001f && Size.y >= 0.001f && Size.z >= 0.001f;
}

inline AiJson ShapeDetails(CEditShape& Shape, const char* Kind)
{
	AiJson Primitives = AiJson::array();
	const auto& Shapes = Shape.GetShapes();
	for (const CShapeData::shape_def& Primitive : Shapes)
	{
		if (Primitives.size() >= 128)
		{
			break;
		}
		if (Primitive.type == CShapeData::cfSphere)
		{
			Primitives.push_back({{"type", "sphere"}, {"local_center", AiVectorJson(Primitive.data.sphere.P)},
				{"local_radius", Primitive.data.sphere.R}, {"radius_m", Primitive.data.sphere.R * Shape.GetScale().x}});
		}
		else if (Primitive.type == CShapeData::cfBox)
		{
			const Fmatrix& Matrix = Primitive.data.box;
			Primitives.push_back({{"type", "box"}, {"local_center", AiVectorJson(Matrix.c)},
				{"local_axis_x", AiVectorJson(Matrix.i)}, {"local_axis_y", AiVectorJson(Matrix.j)}, {"local_axis_z", AiVectorJson(Matrix.k)}});
		}
	}
	return {{"kind", Kind}, {"primitive_count", Shapes.size()}, {"primitives", Primitives}, {"scale", AiVectorJson(Shape.GetScale())}};
}

inline bool SetShape(CEditShape& Shape, const AiJson& Changes, bool Write)
{
	if (!OnlyKeys(Changes, {"radius_m", "size_m"}) || Shape.GetShapes().size() != 1)
	{
		return false;
	}
	auto& Primitive = Shape.GetShapes().front();
	if (Primitive.type == CShapeData::cfSphere)
	{
		float Radius = Primitive.data.sphere.R * Shape.GetScale().x;
		const float Scale = Shape.GetScale().x;
		if (Changes.contains("size_m") || !std::isfinite(Scale) || Scale <= 0 || !ApplyNumbers(Changes, {{"radius_m", &Radius, 0.001f, 100000}}, true))
		{
			return false;
		}
		if (Write)
		{
			Primitive.data.sphere.R = Radius / Scale;
			Shape.ComputeBounds();
			Shape.UpdateTransform();
		}
		return true;
	}
	Fvector Size;
	if (Primitive.type != CShapeData::cfBox || Changes.contains("radius_m") || !BoxSize(Changes, Size))
	{
		return false;
	}
	// Replace absolute box dimensions only where the local box is the editor's standard unit box.
	const Fmatrix& Box = Primitive.data.box;
	if (!Box.i.similar(Fvector().set(1, 0, 0)) || !Box.j.similar(Fvector().set(0, 1, 0)) || !Box.k.similar(Fvector().set(0, 0, 1)))
	{
		return false;
	}
	if (Write)
	{
		Shape.NumSetScale(Size);
	}
	return true;
}

inline bool SetPlanar(CPlanar& Planar, const AiJson& Changes, bool Write)
{
	if (!OnlyKeys(Changes, {"size_m", "stiffness"}) ||
		!ApplyNumbers(Changes, {{"stiffness", &Planar.m_Stiffness, 0, 1}}, Write) ||
		(Changes.contains("size_m") && !SetShape(Planar, {{"size_m", Changes["size_m"]}}, Write)))
	{
		return false;
	}
	if (Write)
	{
		Planar.OnUpdateTransform();
	}
	return true;
}

inline bool SetFog(EFogVolume& Fog, const AiJson& Changes, bool Write)
{
	if (!OnlyKeys(Changes, {"size_m", "fog_kind", "fog_profile"}) ||
		(Changes.contains("fog_kind") && !Changes["fog_kind"].is_string()) ||
		(Changes.contains("size_m") && !SetShape(Fog, {{"size_m", Changes["size_m"]}}, Write)))
	{
		return false;
	}
	const xr_string Kind = AiString(Changes, "fog_kind", Fog.m_volumeType == fvEmitter ? "emitter" : "occlusion");
	if (Kind != "emitter" && Kind != "occlusion")
	{
		return false;
	}
	xr_string Profile;
	string_path Path;
	if (Changes.contains("fog_profile") && (Kind != "emitter" || !ResourceName(Changes, "fog_profile", Profile) ||
		!Profile.ends_with(".ltx") || !FS.path_exist(_game_config_) || !FS.exist(Path, _game_config_, Profile.c_str())))
	{
		return false;
	}
	if (Write)
	{
		Fog.m_volumeType = Kind == "emitter" ? fvEmitter : fvOcclusion;
		Fog.SetDrawColor(Fog.m_volumeType == fvEmitter ? 0x205050FF : 0x2050A050, 0xFF202020);
		if (!Profile.empty())
		{
			Fog.m_volume_profile = Profile.c_str();
		}
	}
	return true;
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
		AiJson Result = {{"kind", "spawn"}, {"section", AiText::Utf8(Entity->name()).c_str()}, {"respawn_seconds", Entity->RespawnTime},
			{"custom_data", AiText::Utf8(Entity->m_ini_string.c_str()).c_str()}, {"has_shape", Spawn.m_AttachedObject != nullptr},
			{"accepts_shape", Entity->shape() != nullptr}};
		if (auto* Shape = smart_cast<CEditShape*>(Spawn.m_AttachedObject))
		{
			Result["shape"] = ShapeDetails(*Shape, "shape");
			Result["shape"]["name"] = AiText::Utf8(Shape->GetName()).c_str();
		}
		return Result;
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

	static AiJson SoundEnvironmentDetails(ESoundEnvironment& Environment)
	{
		AiJson Result = AiLevelEntity::ShapeDetails(Environment, "sound_environment");
		Result["inner_environment"] = AiText::Utf8(Environment.m_EnvInner.c_str()).c_str();
		Result["outer_environment"] = AiText::Utf8(Environment.m_EnvOuter.c_str()).c_str();
		return Result;
	}

	static bool SetSoundEnvironment(ESoundEnvironment& Environment, const AiJson& Changes, bool Write)
	{
		using namespace AiLevelEntity;
		xr_string Inner, Outer;
		if (!OnlyKeys(Changes, {"size_m", "inner_environment", "outer_environment"}) ||
			(Changes.contains("inner_environment") && !SoundEnvironmentName(Changes, "inner_environment", Inner)) ||
			(Changes.contains("outer_environment") && !SoundEnvironmentName(Changes, "outer_environment", Outer)) ||
			(Changes.contains("size_m") && !SetShape(Environment, {{"size_m", Changes["size_m"]}}, Write)))
		{
			return false;
		}
		if (Write)
		{
			if (Changes.contains("inner_environment"))
			{
				Environment.m_EnvInner = Inner.c_str();
			}
			if (Changes.contains("outer_environment"))
			{
				Environment.m_EnvOuter = Outer.c_str();
			}
			Environment.OnChangeEnvs(nullptr);
		}
		return true;
	}

	static AiJson ParticleDetails(EParticlesObject& Particle)
	{
		const IParticleCustom* Visual = Particle.GetParticles();
		return {{"kind", "particle"}, {"particle", AiText::Utf8(Particle.m_RefName.c_str()).c_str()},
			{"game_type_flags", Particle.m_GameType.m_GameType.get()}, {"compiled", Visual != nullptr}};
	}

	static bool SetParticle(EParticlesObject& Particle, const AiJson& Changes, bool Write)
	{
		using namespace AiLevelEntity;
		xr_string Effect;
		u32 Flags = Particle.m_GameType.m_GameType.get();
		if (!OnlyKeys(Changes, {"particle", "game_type_flags"}) ||
			(Changes.contains("particle") && !ParticleResource(Changes, "particle", Effect)) ||
			(Changes.contains("game_type_flags") && !Unsigned(Changes, "game_type_flags", 0, 65535, Flags)))
		{
			return false;
		}
		if (Write)
		{
			if (!Effect.empty() && (!Particle.GetParticles() || !Particle.m_RefName.c_str() || Effect != Particle.m_RefName.c_str()) &&
				!Particle.Compile(Effect.c_str()))
			{
				return false;
			}
			Particle.m_GameType.m_GameType.assign(u16(Flags));
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
	if (Method == "scene.entity_get" && Params.contains("resource_kind"))
	{
		size_t Offset, Limit;
		const xr_string Kind = AiString(Params, "resource_kind");
		if (Params.contains("object_id") || Params.contains("detail_tool") || Params.contains("detail_model") ||
			Params.contains("samples") || !AiPage(Params, Offset, Limit) ||
			(Params.contains("query") && !AiHasString(Params, "query", 128)))
		{
			return Error("INVALID_PARAMS", "Query one resource catalog at a time");
		}
		const xr_string Query = AiString(Params, "query");
		Json Items = Json::array();
		size_t Count = 0;
		auto Add = [&](const char* Name, const char* Type)
		{
			if (Name && AiContains(Name, Query) && Count++ >= Offset && Items.size() < Limit)
			{
				Items.push_back({{"name", AiText::Utf8(Name).c_str()}, {"type", Type}});
			}
		};
		if (Kind == "particles")
		{
			for (const PS::CPEDef* Effect : RImplementation.PSLibrary.VecPEDs())
			{
				Add(Effect->Name(), "effect");
			}
			for (const PS::CPGDef* Group : RImplementation.PSLibrary.VecPGDs())
			{
				Add(Group->m_Name.c_str(), "group");
			}
		}
		else if (Kind == "sound_environments" && SndLib)
		{
			AStringVec Entries;
			SndLib->GetSoundEnvs(Entries);
			for (const xr_string& Entry : Entries)
			{
				Add(Entry.c_str(), "sound_environment");
			}
		}
		else
		{
			return Error("RESOURCE_CATALOG_UNAVAILABLE", "Choose particles or sound_environments");
		}
		const size_t Next = std::min(Offset, Count) + Items.size();
		return {{"ok", true}, {"kind", Kind.c_str()}, {"items", Items}, {"next_offset", Next}, {"has_more", Next < Count}, {"total", Count}};
	}
	if (Method == "scene.entity_create")
	{
		const xr_string Kind = AiString(Params, "kind");
		ObjClassID Class = OBJCLASS_DUMMY;
		xr_string Section, Source, Particle;
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
		else if (Kind == "particle" && ParticleResource(Params, "particle", Particle))
		{
			Class = OBJCLASS_PS;
		}
		else if (Kind == "shape")
		{
			Class = OBJCLASS_SHAPE;
		}
		else if (Kind == "puddle")
		{
			Class = OBJCLASS_PUDDLES;
		}
		else if (Kind == "planar")
		{
			Class = OBJCLASS_PLANARS;
		}
		else if (Kind == "sound_environment")
		{
			Class = OBJCLASS_SOUND_ENV;
		}
		else if (Kind == "fog_volume")
		{
			Class = OBJCLASS_FOG_VOL;
		}
		if (Class == OBJCLASS_DUMMY)
		{
			return Error("INVALID_ENTITY", "Choose a supported entity kind with valid creation parameters and installed resources");
		}
		const bool Volume = Class == OBJCLASS_SHAPE || Class == OBJCLASS_PUDDLES || Class == OBJCLASS_PLANARS ||
			Class == OBJCLASS_SOUND_ENV || Class == OBJCLASS_FOG_VOL;
		const xr_string Primitive = AiString(Params, "shape_type", "box");
		const bool Sphere = Class == OBJCLASS_SHAPE && Primitive == "sphere";
		Fvector Position, Rotation, Size;
		float Radius = 1;
		if (!AiVector(Params, "position_m", Position, 0, 1000000) || !AiVector(Params, "rotation_degrees", Rotation, 0, 360000) ||
			(Params.contains("shape_type") && (Class != OBJCLASS_SHAPE || (Primitive != "box" && !Sphere))) ||
			(Params.contains("radius_m") && (!Sphere || !ApplyNumbers(Params, {{"radius_m", &Radius, 0.001f, 100000}}, true))) ||
			(Params.contains("size_m") && (!Volume || Sphere || !BoxSize(Params, Size))))
		{
			return Error("INVALID_TRANSFORM", "Supply valid position, rotation, box dimensions or sphere radius");
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
		if (Params.contains("rotation_degrees"))
		{
			Object->SetRotation(Rotation.mul(PI / 180.f));
		}
		if (Class == OBJCLASS_SHAPE)
		{
			auto* Shape = static_cast<CEditShape*>(Object);
			if (Sphere)
			{
				Fsphere Geometry;
				Geometry.identity();
				Geometry.R = Radius;
				Shape->add_sphere(Geometry);
			}
			else
			{
				Shape->add_box(Fidentity);
			}
		}
		if (Params.contains("size_m"))
		{
			Object->NumSetScale(Size);
		}
		if (Class == OBJCLASS_PS && !static_cast<EParticlesObject*>(Object)->Compile(Particle.c_str()))
		{
			xr_delete(Object);
			return Error("ENTITY_CREATE_FAILED", "SDK could not compile the particle instance");
		}
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
		case OBJCLASS_PS: return AiLevelEntityAccess::ParticleDetails(*static_cast<EParticlesObject*>(Object));
		case OBJCLASS_SHAPE: return ShapeDetails(*static_cast<CEditShape*>(Object), "shape");
		case OBJCLASS_PUDDLES: return ShapeDetails(*static_cast<CEditShape*>(Object), "puddle");
		case OBJCLASS_SOUND_ENV: return AiLevelEntityAccess::SoundEnvironmentDetails(*static_cast<ESoundEnvironment*>(Object));
		case OBJCLASS_PLANARS:
		{
			auto& Planar = *static_cast<CPlanar*>(Object);
			Json Details = ShapeDetails(Planar, "planar");
			Details["stiffness"] = Planar.m_Stiffness;
			return Details;
		}
		case OBJCLASS_FOG_VOL:
		{
			auto& Fog = *static_cast<EFogVolume*>(Object);
			Json Details = ShapeDetails(Fog, "fog_volume");
			Details["fog_kind"] = Fog.m_volumeType == fvEmitter ? "emitter" : "occlusion";
			Details["fog_profile"] = AiText::Utf8(Fog.m_volume_profile.c_str()).c_str();
			Details["group_id"] = Fog.m_group_id;
			return Details;
		}
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
	if (Object->FClassID == OBJCLASS_SPAWNPOINT && (Changes.contains("attach_shape_id") || Changes.contains("detach_shape")))
	{
		auto& Spawn = *static_cast<CSpawnPoint*>(Object);
		ESceneCustomOTool* ShapeTool = Scene->GetOTool(OBJCLASS_SHAPE);
		if (Changes.size() != 1 || !ShapeTool || !ShapeTool->IsEnabled() || !ShapeTool->IsEditable())
		{
			return Error("INVALID_SPAWN_SHAPE_CHANGE", "Use one attachment operation and enable editing in the Shape tool");
		}
		CCustomObject* Shape = Spawn.m_AttachedObject;
		const bool Attach = Changes.contains("attach_shape_id");
		if (Attach)
		{
			CSE_Abstract* Entity = Spawn.m_SpawnData.GetEntity();
			if (!Entity || !Entity->shape() || Shape)
			{
				return Error("INVALID_SPAWN_SHAPE", "The spawn must accept a shape and have no existing attachment");
			}
			xr_vector<CCustomObject*> Shapes;
			if (!Resolve({{"object_id", Changes["attach_shape_id"]}}, Shapes, Failure, true))
			{
				return Failure;
			}
			Shape = Shapes.front();
			if (Shape->FClassID != OBJCLASS_SHAPE || Shape->GetOwner() ||
				static_cast<CEditShape*>(Shape)->m_shape_type == eShapeLevelBound || static_cast<CEditShape*>(Shape)->GetShapes().empty())
			{
				return Error("INVALID_SPAWN_SHAPE", "Choose a standalone editable shape with geometry");
			}
		}
		else if (Changes["detach_shape"] != true || !Shape || Scene->FindObjectByName(Shape->GetName(), Shape))
		{
			return Error("INVALID_SPAWN_SHAPE", "The attachment must exist and its name must be available in the scene");
		}
		AiSceneSnapshot Before;
		Scene->BeforeObjectChange(Object);
		if (Attach)
		{
			Scene->BeforeObjectChange(Shape);
			if (!Spawn.AttachObject(Shape))
			{
				return Error("SPAWN_SHAPE_ATTACH_FAILED", "The SDK refused the attachment");
			}
		}
		else
		{
			Spawn.DetachObject();
		}
		Json Result = {{"object", Describe(Object)}, {"details", SpawnDetails(Spawn)}};
		if (!Attach)
		{
			Result["detached"] = Describe(Shape);
		}
		FinishChange(Before, Result);
		return Result;
	}
	auto Apply = [&](bool WriteFields)
	{
		switch (Object->FClassID)
		{
		case OBJCLASS_LIGHT: return SetLight(*static_cast<CLight*>(Object), Changes, WriteFields);
		case OBJCLASS_SOUND_SRC: return AiLevelEntityAccess::SetSound(*static_cast<ESoundSource*>(Object), Changes, WriteFields);
		case OBJCLASS_SPAWNPOINT: return SetSpawn(*static_cast<CSpawnPoint*>(Object), Changes, WriteFields);
		case OBJCLASS_TERRAIN: return AiLevelEntityAccess::SetTerrain(*static_cast<CTerrain*>(Object), Changes, WriteFields);
		case OBJCLASS_PS: return AiLevelEntityAccess::SetParticle(*static_cast<EParticlesObject*>(Object), Changes, WriteFields);
		case OBJCLASS_SHAPE:
		case OBJCLASS_PUDDLES: return SetShape(*static_cast<CEditShape*>(Object), Changes, WriteFields);
		case OBJCLASS_SOUND_ENV: return AiLevelEntityAccess::SetSoundEnvironment(*static_cast<ESoundEnvironment*>(Object), Changes, WriteFields);
		case OBJCLASS_PLANARS: return SetPlanar(*static_cast<CPlanar*>(Object), Changes, WriteFields);
		case OBJCLASS_FOG_VOL: return SetFog(*static_cast<EFogVolume*>(Object), Changes, WriteFields);
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
