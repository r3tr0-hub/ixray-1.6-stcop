#include "stdafx.h"
#include "MaterialCommands.h"
#include "AiText.h"
#include "Editor/Entry/StaticObject/SceneObject.h"
#include "../../../xrECore/Editor/EditorSceneSubmit.h"
#include "../../../../Layers/xrRenderPC_R4/blenders/Blender.h"

namespace AiMaterials
{
using Json = nlohmann::json;

static Json Error(const char* Code, const char* Message)
{
	return {{"ok", false}, {"error", {{"code", Code}, {"message", Message}}}};
}

static Json Text(const char* Value)
{
	return AiText::Utf8(Value).c_str();
}

static bool NormalizeResource(const char* Input, xr_string& Output, bool IsTexture)
{
	Output = Input ? Input : "";
	if (Output.empty() || Output.size() >= 500 || Output.front() == '/' || Output.front() == '\\')
	{
		return false;
	}
	for (char& Character : Output)
	{
		if ((u8)Character < 32 || Character == ':' || Character == '*' || Character == '?' || Character == '"' || Character == '<' || Character == '>' || Character == '|')
		{
			return false;
		}
		if (Character == '/')
		{
			Character = '\\';
		}
		if (Character >= 'A' && Character <= 'Z')
		{
			Character = char(Character - 'A' + 'a');
		}
	}
	if (Output.find("..") != xr_string::npos || Output.back() == '\\')
	{
		return false;
	}
	if (IsTexture)
	{
		const size_t Dot = Output.find_last_of('.');
		const size_t Slash = Output.find_last_of('\\');
		if (Dot != xr_string::npos && (Slash == xr_string::npos || Dot > Slash))
		{
			Output.resize(Dot);
		}
	}
	return !Output.empty();
}

static Json TextureResource(const char* Texture)
{
	xr_string Name;
	if (!NormalizeResource(Texture, Name, true))
	{
		return {{"exists", false}, {"reason", "invalid_resource_name"}};
	}
	if (Name == "$null" || Name.find("$user$") != xr_string::npos)
	{
		return {{"exists", true}, {"kind", "runtime"}, {"resource", Text(Name.c_str())}};
	}
	string_path Path;
	// Keep the renderer's resolution order; FS.exist includes mounted archives and addons.
	for (const char* Extension : {".ogm", ".avi", ".seq"})
	{
		if (FS.path_exist(_game_textures_) && FS.exist(Path, _game_textures_, Name.c_str(), Extension))
		{
			return {{"exists", true}, {"kind", "game_texture"}, {"alias", _game_textures_}, {"path", Text(Path)}};
		}
	}
	for (const char* Alias : {"$level$", "$game_saves$", _game_textures_})
	{
		if (FS.path_exist(Alias) && FS.exist(Path, Alias, Name.c_str(), ".dds"))
		{
			return {{"exists", true}, {"kind", "dds"}, {"alias", Alias}, {"path", Text(Path)}};
		}
	}
	Json Result = {{"exists", false}, {"resource", Text(Name.c_str())}, {"reason", "no_renderable_texture"}};
	if (FS.path_exist(_textures_) && FS.exist(Path, _textures_, Name.c_str(), ".tga"))
	{
		Result["source_path"] = Text(Path);
		Result["reason"] = "source_tga_requires_texture_synchronization";
	}
	return Result;
}

bool TextureExists(const char* Texture)
{
	return TextureResource(Texture).value("exists", false);
}

static Json EnsureLoaded(CSceneObject& Object)
{
	EditableObjectReadOnlyScope ReadOnlyLoad;
	if (Object.IsReferencePending())
	{
		const char* Reference = Object.m_ReferenceName.c_str();
		xr_string Name;
		if (!NormalizeResource(Reference, Name, false) || !FS.path_exist(_objects_))
		{
			return Error("missing_model", "Ссылка на модель отсутствует или недопустима.");
		}
		string_path File;
		if (!FS.exist(File, _objects_, Name.c_str(), ".object"))
		{
			return Error("missing_model", "Файл модели отсутствует в библиотеке SDK.");
		}
		if (!Object.StreamIn())
		{
			return Error("model_load_failed", "SDK не смог загрузить модель.");
		}
	}
	if (!Object.GetReference())
	{
		return Error("missing_model", "Модель экземпляра не загружена.");
	}
	return {{"ok", true}};
}

static Json SurfaceData(const CSurface& Surface, size_t Index, bool IsInternal = false)
{
	auto StringLambda = [IsInternal](const char* Value) -> Json
	{
		return IsInternal ? Json(Value ? Value : "") : Text(Value);
	};
	return {{"surface_index", Index}, {"name", StringLambda(Surface._Name())}, {"texture", StringLambda(Surface._Texture())}, {"render_shader", StringLambda(Surface._ShaderName())}, {"compiler_shader", StringLambda(Surface._ShaderXRLCName())}, {"game_material", StringLambda(Surface._GameMtlName())}, {"shared", Surface.UseShared}};
}

static xr_string PreserveResourceEncoding(const xr_string& Input, const Json& Current, bool IsTexture)
{
	xr_string RawCurrent;
	if (NormalizeResource(Current.get_ref<const Json::string_t&>().c_str(), RawCurrent, IsTexture) && AiText::Utf8(RawCurrent.c_str()) == Input)
	{
		return RawCurrent;
	}
	return Input;
}

static Json ExportValues(Json Values)
{
	for (const char* Key : {"name", "texture", "render_shader", "compiler_shader", "game_material"})
	{
		Values[Key] = Text(Values[Key].get_ref<const Json::string_t&>().c_str());
	}
	return Values;
}

Json Read(CSceneObject& Object)
{
	const Json Ready = EnsureLoaded(Object);
	if (!Ready.value("ok", false))
	{
		return Ready;
	}
	Json Surfaces = Json::array();
	for (size_t Index = 0; Index < Object.m_Surfaces.size(); ++Index)
	{
		const CSurface* Surface = Object.m_Surfaces[Index];
		if (!Surface || !Surface->m_pData.second)
		{
			return Error("invalid_surface", "SDK вернул повреждённую поверхность.");
		}
		Json Item = SurfaceData(*Surface, Index);
		Item["texture_resource"] = TextureResource(Surface->_Texture());
		Surfaces.push_back(std::move(Item));
	}
	return {{"ok", true}, {"scope", "scene_instance"}, {"reference", Text(Object.RefName())}, {"surfaces", std::move(Surfaces)}};
}

Json ValidatePatch(CSceneObject& Object, const Json& Change)
{
	if (!Change.is_object() || !Change.contains("surface_index") || !Change["surface_index"].is_number_integer())
	{
		return Error("invalid_surface_index", "Требуется целочисленный surface_index из списка материалов.");
	}
	const auto Index = Change["surface_index"].get<s64>();
	if (Index < 0)
	{
		return Error("invalid_surface_index", "Индекс поверхности должен быть неотрицательным.");
	}
	if (Object.Locked() || Object.IsDeleted())
	{
		return Error("object_not_editable", "Объект заблокирован или удалён.");
	}
	const Json Ready = EnsureLoaded(Object);
	if (!Ready.value("ok", false))
	{
		return Ready;
	}
	if (u64(Index) >= Object.m_Surfaces.size() || !Object.m_Surfaces[Index] || !Object.m_Surfaces[Index]->m_pData.second)
	{
		return Error("invalid_surface_index", "Поверхность с указанным индексом отсутствует.");
	}
	const CSurface& Surface = *Object.m_Surfaces[Index];
	Json Final = SurfaceData(Surface, size_t(Index), true);
	bool HasChange = false;
	for (const char* Key : {"texture", "render_shader", "compiler_shader", "game_material"})
	{
		if (Change.contains(Key))
		{
			if (!Change[Key].is_string())
			{
				return Error("invalid_material", "Параметры материала должны быть строками.");
			}
			xr_string Value;
			if (!NormalizeResource(Change[Key].get_ref<const Json::string_t&>().c_str(), Value, xr_strcmp(Key, "texture") == 0))
			{
				return Error("invalid_resource", "Недопустимое имя ресурса; используйте относительное имя из библиотеки SDK.");
			}
			Value = PreserveResourceEncoding(Value, Final[Key], xr_strcmp(Key, "texture") == 0);
			Final[Key] = Value.c_str();
			HasChange = true;
		}
	}
	if (!HasChange)
	{
		return Error("empty_patch", "Укажите хотя бы одно поле материала.");
	}
	const xr_string Texture = Final["texture"].get_ref<const Json::string_t&>().c_str();
	if (Texture.find('$') != xr_string::npos && Texture != "$null")
	{
		return Error("runtime_texture", "Динамические текстуры $user$ нельзя назначать через помощника.");
	}
	if (!TextureExists(Texture.c_str()))
	{
		Json Result = Error("missing_texture", "Текстура не найдена по правилам файловой системы SDK; исходный TGA требует синхронизации.");
		Result["texture_resource"] = TextureResource(Texture.c_str());
		return Result;
	}
	const char* ShaderName = Final["render_shader"].get_ref<const Json::string_t&>().c_str();
	const char* CompilerName = Final["compiler_shader"].get_ref<const Json::string_t&>().c_str();
	IBlender* RenderShader = DEV->_FindBlender(ShaderName);
	Shader_xrLC* CompilerShader = EDevice->ShaderXRLC.Get(CompilerName);
	if (!RenderShader)
	{
		return Error("missing_render_shader", "Игровой шейдер отсутствует в библиотеке SDK.");
	}
	if (!CompilerShader)
	{
		return Error("missing_compiler_shader", "Шейдер компилятора отсутствует в библиотеке SDK.");
	}
	if (!!RenderShader->canBeLMAPped() != !CompilerShader->flags.bLIGHT_Vertex)
	{
		return Error("incompatible_shaders", "Игровой шейдер несовместим с шейдером компилятора по типу освещения.");
	}
	const char* GameMaterial = Final["game_material"].get_ref<const Json::string_t&>().c_str();
	if (!GameMaterialLibraryEditors || !GameMaterialLibraryEditors->GetMaterial(GameMaterial))
	{
		return Error("missing_game_material", "Игровой материал отсутствует в библиотеке SDK.");
	}
	if (xr_strcmp(GameMaterial, "materials\\occ") == 0)
	{
		return Error("reserved_game_material", "Материал materials\\occ зарезервирован SDK.");
	}
	return {{"ok", true}, {"normalized", ExportValues(std::move(Final))}};
}

Json ApplyPatch(CSceneObject& Object, const Json& Change)
{
	const Json Validation = ValidatePatch(Object, Change);
	if (!Validation.value("ok", false))
	{
		return Validation;
	}
	const size_t Index = Change["surface_index"].get<size_t>();
	CSurface& Surface = *Object.m_Surfaces[Index];
	Json Final = SurfaceData(Surface, Index, true);
	for (const char* Key : {"texture", "render_shader", "compiler_shader", "game_material"})
	{
		if (Change.contains(Key))
		{
			xr_string Value;
			NormalizeResource(Change[Key].get_ref<const Json::string_t&>().c_str(), Value, xr_strcmp(Key, "texture") == 0);
			Value = PreserveResourceEncoding(Value, Final[Key], xr_strcmp(Key, "texture") == 0);
			Final[Key] = Value.c_str();
		}
	}
	const Json Before = SurfaceData(Surface, Index);
	bool HasChange = false;
	for (const char* Key : {"texture", "render_shader", "compiler_shader", "game_material"})
	{
		HasChange |= Before[Key] != Text(Final[Key].get_ref<const Json::string_t&>().c_str());
	}
	Json Changed = Json::array();
	if (!HasChange)
	{
		return {{"ok", true}, {"changed_surfaces", std::move(Changed)}};
	}
	// Shared data belongs to the library. A scene override always owns its copy.
	if (Surface.UseShared)
	{
		Surface.m_pData.second = new SSurfaceData(*Surface.m_pData.second);
		Surface.UseShared = false;
		Surface.UseSharedPrev = false;
	}
	Surface.SetShader(Final["render_shader"].get_ref<const Json::string_t&>().c_str());
	Surface.SetShaderXRLC(Final["compiler_shader"].get_ref<const Json::string_t&>().c_str());
	Surface.SetGameMtl(Final["game_material"].get_ref<const Json::string_t&>().c_str());
	Surface.SetTexture(Final["texture"].get_ref<const Json::string_t&>().c_str());
	Surface.RemoveImageData();
	Object.MarkInstanceSurfacesModified();
	Changed.push_back({{"surface_index", Index}, {"name", Text(Surface._Name())}, {"before", Before}, {"after", SurfaceData(Surface, Index)}});
	return {{"ok", true}, {"scope", "scene_instance"}, {"changed_surfaces", std::move(Changed)}};
}

Json MissingTextures(CSceneObject& Object, size_t Offset, size_t Limit)
{
	const Json All = Read(Object);
	if (!All.value("ok", false))
	{
		return All;
	}
	Json Missing = Json::array();
	size_t Total = 0;
	Limit = std::min(Limit, size_t(500));
	for (const Json& Surface : All["surfaces"])
	{
		if (!Surface["texture_resource"].value("exists", false))
		{
			if (Total >= Offset && Missing.size() < Limit)
			{
				Missing.push_back(Surface);
			}
			++Total;
		}
	}
	return {{"ok", true}, {"missing", std::move(Missing)}, {"total", Total}, {"offset", Offset}, {"limit", Limit}, {"has_more", Offset < Total && Total - Offset > Limit}};
}
} // namespace AiMaterials
