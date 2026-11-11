#include "stdafx.h"
#include "AiProjectTools.h"
#include "AiText.h"
#include "Editor/Utils/GitIntegration.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

namespace AiProjectTools
{
namespace Files = std::filesystem;

static Json Error(const char* Code, const char* Message)
{
	return {{"ok", false}, {"error", {{"code", Code}, {"message", Message}}}};
}

static xr_string Text(const Json& Params, const char* Key, const char* Default = "")
{
	const auto Value = Params.find(Key);
	if (Value == Params.end())
	{
		return Default;
	}
	if (!Value->is_string())
	{
		return {};
	}
	const auto& String = Value->get_ref<const Json::string_t&>();
	if (String.size() > 512 || String.find('\0') != Json::string_t::npos)
	{
		return {};
	}
	return String.c_str();
}

static bool RelativeName(xr_string& Name)
{
	if (Name.empty() || Name.size() > 200)
	{
		return false;
	}
	for (char& Character : Name)
	{
		if (Character == '\\')
		{
			Character = '/';
		}
		if (static_cast<u8>(Character) < 32 || std::strchr(":*?\"<>|", Character))
		{
			return false;
		}
	}
	size_t Start = 0;
	do
	{
		const size_t End = Name.find('/', Start);
		const xr_string Component = Name.substr(Start, End == xr_string::npos ? End : End - Start);
		if (Component.empty() || Component.back() == '.' || Component.back() == ' ' || Component == ".git")
		{
			return false;
		}
		if (End == xr_string::npos)
		{
			return true;
		}
		Start = End + 1;
	} while (Start <= Name.size());
	return false;
}

static Files::path ResolvePath(const char* Alias, const xr_string& Relative)
{
	if (!FS.path_exist(Alias))
	{
		return {};
	}
	string_path Buffer;
	FS.update_path(Buffer, Alias, "");
	std::error_code Code;
	const auto Root = Files::weakly_canonical(Files::path(Buffer), Code);
	if (Code || !Files::is_directory(Root, Code))
	{
		return {};
	}
	const auto Path = Root / Files::u8path(Relative.c_str());
	const auto Canonical = Files::weakly_canonical(Path, Code);
	if (Code)
	{
		return {};
	}
	const auto Tail = Canonical.lexically_relative(Root);
	if (Tail.empty() || Tail == "." || *Tail.begin() == "..")
	{
		return {};
	}
	return Path;
}

static bool NonemptyFile(const Files::path& Path)
{
	std::error_code Code;
	return Files::is_regular_file(Path, Code) && Files::file_size(Path, Code) > 0 && !Code;
}

class NewDirectory final
{
public:
	NewDirectory() = default;
	NewDirectory(const NewDirectory&) = delete;
	NewDirectory& operator=(const NewDirectory&) = delete;
	~NewDirectory()
	{
		if (Owned)
		{
			std::error_code Code;
			Files::remove_all(Path, Code);
		}
	}

	bool Create(const Files::path& Target)
	{
		std::error_code Code;
		Path = Target;
		Owned = Files::create_directory(Path, Code);
		return Owned;
	}

	void Keep()
	{
		Owned = false;
	}

	Files::path Path;

private:
	bool Owned = false;
};

static bool CreateStage(NewDirectory& Stage, const Files::path& Parent)
{
	for (int Index = 0; Index < 32; ++Index)
	{
		if (Stage.Create(Parent / (".ixray-ai-stage-" + std::to_string(Index))))
		{
			return true;
		}
	}
	return false;
}

struct ThmChunk
{
	u32 Id;
	size_t Offset;
	u32 Size;
};

static bool ReadMetadata(const Files::path& Path, xr_vector<u8>& Bytes, xr_vector<ThmChunk>& Chunks, STextureParams& Params)
{
	std::error_code Code;
	if (!Files::is_regular_file(Path, Code))
	{
		return false;
	}
	const auto Size = Files::file_size(Path, Code);
	if (Code || Size < 8 || Size > 1024 * 1024)
	{
		return false;
	}
	Bytes.resize(static_cast<size_t>(Size));
	std::ifstream Stream(Path, std::ios::binary);
	if (!Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Size)))
	{
		return false;
	}
	bool HasVersion = false;
	bool HasKind = false;
	bool HasParams = false;
	for (size_t Offset = 0; Offset < Bytes.size();)
	{
		if (Bytes.size() - Offset < 8)
		{
			return false;
		}
		u32 Id, Length;
		std::memcpy(&Id, Bytes.data() + Offset, 4);
		std::memcpy(&Length, Bytes.data() + Offset + 4, 4);
		const u8* Data = Bytes.data() + Offset + 8;
		if (Length > Bytes.size() - Offset - 8 || (Id & CFS_CompressMark))
		{
			return false;
		}
		if (Id >= THM_CHUNK_VERSION && Id <= THM_CHUNK_FADE_DELAY)
		{
			for (const ThmChunk& Previous : Chunks)
			{
				if (Previous.Id == Id)
				{
					return false;
				}
			}
		}
		switch (Id)
		{
			case THM_CHUNK_VERSION: HasVersion = Length == 2 && Data[0] == 0x12 && Data[1] == 0; break;
			case THM_CHUNK_TYPE: HasKind = Length == 4 && Data[0] == 1 && Data[1] == 0 && Data[2] == 0 && Data[3] == 0; break;
			case THM_CHUNK_TEXTUREPARAM: HasParams = Length == 32; break;
			case THM_CHUNK_TEXTURE_TYPE: if (Length != 4) { return false; } break;
			case THM_CHUNK_DETAIL_EXT: if (Length < 5 || std::memchr(Data, 0, Length - 4) != Data + Length - 5) { return false; } break;
			case THM_CHUNK_MATERIAL: if (Length != 8) { return false; } break;
			case THM_CHUNK_BUMP: if (Length < 9 || std::memchr(Data + 8, 0, Length - 8) != Data + Length - 1) { return false; } break;
			case THM_CHUNK_EXT_NORMALMAP: if (!Length || std::memchr(Data, 0, Length) != Data + Length - 1) { return false; } break;
			case THM_CHUNK_FADE_DELAY: if (Length != 1) { return false; } break;
			default: break;
		}
		Chunks.push_back({Id, Offset, Length});
		Offset += 8 + Length;
	}
	if (!HasVersion || !HasKind || !HasParams)
	{
		return false;
	}
	IReader Reader(Bytes.data(), Bytes.size());
	Params.Load(Reader);
	return std::isfinite(Params.detail_scale) && std::isfinite(Params.material_weight) && std::isfinite(Params.bump_virtual_height);
}

static Json DescribeMetadata(const STextureParams& Params)
{
	return {
		{"format", static_cast<u32>(Params.fmt)}, {"type", static_cast<u32>(Params.type)},
		{"width", Params.width}, {"height", Params.height}, {"flags", Params.flags.get()},
		{"mip_filter", Params.mip_filter}, {"generate_mipmaps", Params.flags.test(STextureParams::flGenerateMipMaps)},
		{"material", static_cast<u32>(Params.material)}, {"material_weight", Params.material_weight},
		{"bump_mode", static_cast<u32>(Params.bump_mode)}, {"bump_name", AiText::Utf8(Params.bump_name.c_str()).c_str()},
		{"bump_virtual_height", Params.bump_virtual_height}, {"detail_name", AiText::Utf8(Params.detail_name.c_str()).c_str()},
		{"detail_scale", Params.detail_scale}, {"external_normal_map", AiText::Utf8(Params.ext_normal_map_name.c_str()).c_str()}
	};
}

static void WriteMetadata(CMemoryWriter& Writer, STextureParams& Params, const xr_vector<u8>& Original = {}, const xr_vector<ThmChunk>& Chunks = {})
{
	Writer.open_chunk(THM_CHUNK_VERSION);
	Writer.w_u16(0x12);
	Writer.close_chunk();
	Writer.open_chunk(THM_CHUNK_TYPE);
	Writer.w_u32(ECustomThumbnail::ETTexture);
	Writer.close_chunk();
	Params.Save(Writer);
	for (const ThmChunk& Chunk : Chunks)
	{
		if (Chunk.Id == THM_CHUNK_DATA || Chunk.Id < THM_CHUNK_VERSION || Chunk.Id > THM_CHUNK_FADE_DELAY)
		{
			Writer.w(Original.data() + Chunk.Offset, 8 + Chunk.Size);
		}
	}
}

static bool WriteBuffer(const Files::path& Path, const u8* Bytes, size_t Size)
{
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	Stream.write(reinterpret_cast<const char*>(Bytes), static_cast<std::streamsize>(Size));
	Stream.close();
	return Stream.good();
}

static bool SetMetadata(STextureParams& Params, const Json& Changes)
{
	if (!Changes.is_object() || Changes.empty())
	{
		return false;
	}
	// Changing texture type first applies the same defaults as the Image Editor.
	if (Changes.contains("type"))
	{
		const auto& Type = Changes["type"];
		if (!Type.is_number_integer() || Type.get<double>() < 0 || Type.get<double>() > STextureParams::ttTerrain)
		{
			return false;
		}
		Params.type = static_cast<STextureParams::ETType>(Type.get<u32>());
		Params.OnTypeChange(nullptr);
	}
	for (auto It = Changes.begin(); It != Changes.end(); ++It)
	{
		const auto& Key = It.key();
		const auto& Value = It.value();
		if (Key == "type")
		{
			continue;
		}
		if (Key == "generate_mipmaps")
		{
			if (!Value.is_boolean())
			{
				return false;
			}
			Params.flags.set(STextureParams::flGenerateMipMaps, Value.get<bool>());
		}
		else if (Key == "bump_name" || Key == "detail_name")
		{
			xr_string Name = Text(Changes, Key.c_str());
			if (!Value.is_string() || (!Value.get_ref<const Json::string_t&>().empty() && !RelativeName(Name)))
			{
				return false;
			}
			std::replace(Name.begin(), Name.end(), '/', '\\');
			if (Key == "bump_name")
			{
				Params.bump_name = Name.c_str();
			}
			else
			{
				Params.detail_name = Name.c_str();
			}
		}
		else if (Key == "material_weight" || Key == "detail_scale" || Key == "bump_virtual_height")
		{
			if (!Value.is_number())
			{
				return false;
			}
			const double Number = Value.get<double>();
			if (!std::isfinite(Number) || Number < 0 || Number > (Key == "material_weight" ? 1 : 1000) || (Key == "detail_scale" && Number == 0))
			{
				return false;
			}
			if (Key == "material_weight")
			{
				Params.material_weight = static_cast<float>(Number);
			}
			else if (Key == "detail_scale")
			{
				Params.detail_scale = static_cast<float>(Number);
			}
			else
			{
				Params.bump_virtual_height = static_cast<float>(Number);
			}
		}
		else
		{
			if (!Value.is_number_integer() || Value.get<double>() < 0 || Value.get<double>() > 14)
			{
				return false;
			}
			const u32 Number = Value.get<u32>();
			if (Key == "format")
			{
				Params.fmt = static_cast<STextureParams::ETFormat>(Number);
			}
			else if (Key == "mip_filter")
			{
				Params.mip_filter = Number;
			}
			else if (Key == "material" && Number <= STextureParams::tmPBR_Material)
			{
				Params.material = static_cast<STextureParams::ETMaterial>(Number);
			}
			else if (Key == "bump_mode" && Number >= STextureParams::tbmNone && Number <= STextureParams::tbmUseParallax)
			{
				Params.bump_mode = static_cast<STextureParams::ETBumpMode>(Number);
			}
			else
			{
				return false;
			}
		}
	}
	return true;
}

static Json Metadata(const Json& Input, bool Edit)
{
	xr_string Name = Text(Input, "texture");
	const xr_string Source = Text(Input, "source", "game");
	if (!RelativeName(Name) || (Source != "game" && Source != "source"))
	{
		return Error("INVALID_TEXTURE", "Specify a texture name relative to the SDK texture root");
	}
	const auto Path = ResolvePath(Source == "game" ? _game_textures_ : _textures_, Name + ".thm");
	xr_vector<u8> Bytes;
	xr_vector<ThmChunk> Chunks;
	STextureParams Params;
	if (Path.empty() || !ReadMetadata(Path, Bytes, Chunks, Params))
	{
		return Error("THM_UNAVAILABLE", "Loose texture metadata is missing or has an unsupported THM structure");
	}
	const Json Before = DescribeMetadata(Params);
	if (!Edit)
	{
		return {{"ok", true}, {"texture", Name.c_str()}, {"source", Source.c_str()}, {"metadata", Before}};
	}
	const auto Changes = Input.find("changes");
	if (Changes == Input.end() || !SetMetadata(Params, *Changes))
	{
		return Error("INVALID_THM_CHANGES", "Provide supported texture metadata fields and values");
	}
	const Json After = DescribeMetadata(Params);
	if (Before == After)
	{
		return {{"ok", true}, {"texture", Name.c_str()}, {"source", Source.c_str()}, {"metadata", After}, {"changed", false}};
	}
	NewDirectory Stage;
	if (!CreateStage(Stage, Path.parent_path()))
	{
		return Error("THM_WRITE_FAILED", "Could not create a staging directory");
	}
	const auto Temp = Stage.Path / Path.filename();
	CMemoryWriter Writer;
	WriteMetadata(Writer, Params, Bytes, Chunks);
	xr_vector<u8> Checked;
	xr_vector<ThmChunk> CheckedChunks;
	STextureParams CheckedParams;
	if (!WriteBuffer(Temp, Writer.pointer(), Writer.size()) || !ReadMetadata(Temp, Checked, CheckedChunks, CheckedParams) || DescribeMetadata(CheckedParams) != After)
	{
		return Error("THM_VERIFY_FAILED", "Could not save and verify the new THM");
	}
	CheckedChunks.clear();
	if (!ReadMetadata(Path, Checked, CheckedChunks, CheckedParams) || Checked != Bytes)
	{
		return Error("THM_CHANGED", "THM changed on disk during the edit");
	}
	std::error_code Code;
	Files::rename(Temp, Path, Code);
	if (Code)
	{
		return Error("THM_WRITE_FAILED", "Could not replace the THM");
	}
	FS.file_update(Path.string().c_str());
	return {{"ok", true}, {"texture", Name.c_str()}, {"source", Source.c_str()}, {"metadata", After}, {"changed", true}};
}

static Json ImportTexture(const Json& Input)
{
#ifndef IXR_WINDOWS
	return Error("IMPORT_UNAVAILABLE", "The SDK texture decoder is available in the Windows editor");
#else
	xr_string Name = Text(Input, "texture");
	if (!RelativeName(Name))
	{
		return Error("INVALID_TEXTURE", "Specify a source texture name relative to rawdata/textures");
	}
	const auto Source = ResolvePath(_textures_, Name + ".tga");
	const auto Dds = ResolvePath(_game_textures_, Name + ".dds");
	const auto Thm = ResolvePath(_game_textures_, Name + ".thm");
	if (Source.empty() || !NonemptyFile(Source) || Dds.empty() || Thm.empty())
	{
		return Error("IMPORT_SOURCE_MISSING", "Source TGA or SDK game texture directory is unavailable");
	}
	std::error_code Code;
	if (Files::exists(Dds, Code) || Code || Files::exists(Thm, Code) || Code)
	{
		return Error("TEXTURE_EXISTS", "This import requires new DDS and THM filenames");
	}
	const auto Format = Input.find("format");
	if (Format != Input.end() && (!Format->is_number_integer() || Format->get<double>() < 0 ||
		(Format->get<double>() > STextureParams::tfDXT5 && Format->get<double>() != STextureParams::tfRGBA)))
	{
		return Error("INVALID_TEXTURE_FORMAT", "Use DXT1, DXT1a, DXT3, DXT5, or RGBA");
	}
	// Validate the TGA header and uncompressed pixel payload before entering the SDK decoder.
	u8 Header[18]{};
	std::ifstream Stream(Source, std::ios::binary);
	if (!Stream.read(reinterpret_cast<char*>(Header), sizeof(Header)))
	{
		return Error("INVALID_SOURCE_TGA", "The source TGA header is incomplete");
	}
	const u32 Width = static_cast<u32>(Header[12]) | (static_cast<u32>(Header[13]) << 8);
	const u32 Height = static_cast<u32>(Header[14]) | (static_cast<u32>(Header[15]) << 8);
	const bool Alpha = Header[16] == 32;
	const auto SourceSize = Files::file_size(Source, Code);
	if (Code || Header[1] != 0 || Header[2] != 2 || (Header[16] != 24 && !Alpha) ||
		Width < 4 || Height < 4 || Width > 4096 || Height > 4096 || (Width & (Width - 1)) || (Height & (Height - 1)) ||
		SourceSize < 18 + Header[0] + static_cast<u64>(Width) * Height * (Header[16] / 8))
	{
		return Error("INVALID_SOURCE_TGA", "Use a complete uncompressed 24-bit or 32-bit TGA with power-of-two dimensions from 4 through 4096");
	}
	Stream.close();
	const auto SourceText = Source.string();
	auto Image = DXTUtils::GitPixels(SourceText.c_str());
	if (Image.W != Width || Image.H != Height || Image.P.size() != static_cast<size_t>(Width) * Height * 4)
	{
		return Error("IMPORT_DECODE_FAILED", "SDK could not decode the TGA pixels");
	}
	Files::create_directories(Dds.parent_path(), Code);
	if (Code)
	{
		return Error("IMPORT_WRITE_FAILED", "Could not create the destination texture directory");
	}
	NewDirectory Stage;
	if (!CreateStage(Stage, Dds.parent_path()))
	{
		return Error("IMPORT_WRITE_FAILED", "Could not create a staging directory");
	}
	STextureParams Params;
	Params.width = Width;
	Params.height = Height;
	Params.flags.set(STextureParams::flHasAlpha, Alpha);
	Params.fmt = Format == Input.end() ? (Alpha ? STextureParams::tfDXT5 : STextureParams::tfDXT1) : static_cast<STextureParams::ETFormat>(Format->get<u32>());
	const auto StagedDds = Stage.Path / Dds.filename();
	const auto StagedDdsText = StagedDds.string();
	if (DXTUtils::Compress(StagedDdsText.c_str(), Image.P.data(), nullptr, Width, Height, Width * 4, &Params, 4) != 1 || !NonemptyFile(StagedDds))
	{
		return Error("IMPORT_COMPRESSION_FAILED", "SDK could not produce a DDS");
	}
	const auto StagedThm = Stage.Path / Thm.filename();
	CMemoryWriter Writer;
	WriteMetadata(Writer, Params);
	xr_vector<u8> Bytes;
	xr_vector<ThmChunk> Chunks;
	STextureParams Checked;
	if (!WriteBuffer(StagedThm, Writer.pointer(), Writer.size()) || !ReadMetadata(StagedThm, Bytes, Chunks, Checked) || DescribeMetadata(Checked) != DescribeMetadata(Params))
	{
		return Error("IMPORT_VERIFY_FAILED", "Could not save and verify the THM");
	}
	if (!Files::copy_file(StagedDds, Dds, Files::copy_options::none, Code))
	{
		return Error("IMPORT_PUBLISH_FAILED", "Could not publish the DDS. Check the destination");
	}
	if (!Files::copy_file(StagedThm, Thm, Files::copy_options::none, Code))
	{
		Files::remove(Dds, Code);
		return Error("IMPORT_PUBLISH_FAILED", "Could not publish the THM. Check the destination");
	}
	FS.file_update(Dds.string().c_str());
	FS.file_update(Thm.string().c_str());
	return {{"ok", true}, {"texture", Name.c_str()}, {"metadata", DescribeMetadata(Checked)}, {"dds_written", true}, {"thm_written", true}};
#endif
}

static Json ExportObj(const Json& Input)
{
	xr_string Model = Text(Input, "model");
	xr_string Destination = Text(Input, "destination");
	if (!RelativeName(Model) || !RelativeName(Destination) || Destination.find('/') != xr_string::npos || Destination.size() > 64)
	{
		return Error("INVALID_EXPORT_PATH", "Specify a library model and one new destination folder under SDK import");
	}
	const auto ModelPath = ResolvePath(_objects_, Model + ".object");
	const auto ExportPath = ResolvePath(_import_, Destination);
	if (ModelPath.empty() || !NonemptyFile(ModelPath) || ExportPath.empty())
	{
		return Error("EXPORT_SOURCE_MISSING", "Library model or SDK import directory is unavailable");
	}
	const auto Filename = ModelPath.stem();
	const auto Obj = ExportPath / (Filename.native() + Files::path(".obj").native());
	const auto Mtl = ExportPath / (Filename.native() + Files::path(".mtl").native());
	const auto ObjText = Obj.string();
	const auto ModelText = ModelPath.string();
	if (ObjText.size() > sizeof(string_path) - 1 || ModelText.size() > sizeof(string_path) - 1)
	{
		return Error("EXPORT_PATH_TOO_LONG", "Model or export path exceeds the SDK path limit");
	}
	auto Release = [](CEditableObject* Object)
	{
		Lib.RemoveEditObject(Object);
	};
	std::unique_ptr<CEditableObject, decltype(Release)> Editable(Lib.CreateEditObject(ModelText.c_str()), Release);
	if (!Editable)
	{
		return Error("MODEL_LOAD_FAILED", "SDK could not load the library model");
	}
	NewDirectory ExportDirectory;
	if (!ExportDirectory.Create(ExportPath))
	{
		return Error("EXPORT_DESTINATION_EXISTS", "Could not create a new destination folder");
	}
	if (!Editable->ExportOBJ(ObjText.c_str()) || !NonemptyFile(Obj) || !NonemptyFile(Mtl))
	{
		return Error("OBJ_EXPORT_FAILED", "SDK could not produce both OBJ and MTL files");
	}
	ExportDirectory.Keep();
	const auto ObjName = Obj.u8string();
	const auto MtlName = Mtl.u8string();
	return {{"ok", true}, {"obj", reinterpret_cast<const char*>(ObjName.c_str())}, {"mtl", reinterpret_cast<const char*>(MtlName.c_str())}};
}

static const char* GitTaskState(EGitTaskState State)
{
	switch (State)
	{
		case EGitTaskState::Running: return "running";
		case EGitTaskState::Succeeded: return "succeeded";
		case EGitTaskState::Failed: return "failed";
		default: return "idle";
	}
}

static Json ReadGit(const Json& Input)
{
	if (Git == nullptr || !Git->IsRepository)
	{
		return Error("GIT_UNAVAILABLE", "The SDK did not detect a local Git repository");
	}
	size_t Offset = 0;
	size_t Limit = 100;
	for (const char* Key : {"offset", "limit"})
	{
		const auto Value = Input.find(Key);
		if (Value == Input.end())
		{
			continue;
		}
		const bool IsLimit = std::strcmp(Key, "limit") == 0;
		if (!Value->is_number_integer() || Value->get<double>() < (IsLimit ? 1 : 0) || Value->get<double>() > (IsLimit ? 200 : 1000000))
		{
			return Error("INVALID_PAGE", "Use a nonnegative offset and limit from 1 through 200");
		}
		(IsLimit ? Limit : Offset) = Value->get<size_t>();
	}
	const xr_string View = Text(Input, "view", "status");
	Json Items = Json::array();
	Json Result;
	size_t Count = 0;
	const auto Append = [&](auto&& MakeItem)
	{
		if (Count++ >= Offset && Items.size() < Limit)
		{
			Items.push_back(MakeItem());
		}
	};
	{
		xrSRWLockGuard Guard(Git->Mutex, true);
		Result = {{"ok", true}, {"view", View.c_str()}, {"branch", AiText::Utf8(Git->Branch.c_str()).c_str()},
			{"upstream", AiText::Utf8(Git->Upstream.c_str()).c_str()}, {"ahead", Git->Ahead}, {"behind", Git->Behind},
			{"busy", Git->Busy.load()}, {"last_task", Git->TaskName.c_str()}, {"task_state", GitTaskState(Git->TaskState.load())}};
		if (View == "status")
		{
			xr_vector<xr_string> Names;
			Names.reserve(Git->Files.size());
			for (const auto& [Path, Info] : Git->Files)
			{
				Names.push_back(Path);
			}
			std::sort(Names.begin(), Names.end());
			for (const auto& Path : Names)
			{
				const auto& Info = Git->Files.at(Path);
				Append([&]
				{
					return Json{{"path", AiText::Utf8(Path.c_str()).c_str()}, {"status", GitStatusText(Info)}, {"staged", Info.Staged}, {"unstaged", Info.Unstaged}, {"lfs", Info.Lfs}};
				});
			}
		}
		else if (View == "history")
		{
			for (const auto& Commit : Git->History)
			{
				Append([&]
				{
					return Json{{"hash", Commit.Hash.c_str()}, {"author", AiText::Utf8(Commit.Author.c_str()).c_str()},
						{"date", Commit.Date.c_str()}, {"subject", AiText::Utf8(Commit.Subject.c_str()).c_str()}};
				});
			}
		}
		else if (View == "branches")
		{
			for (const auto& Branch : Git->Branches)
			{
				Append([&] { return Json(AiText::Utf8(Branch.c_str()).c_str()); });
			}
		}
		else
		{
			return Error("INVALID_GIT_VIEW", "Use status, history, or branches");
		}
	}
	const size_t NextOffset = std::min(Offset, Count) + Items.size();
	Result["items"] = std::move(Items);
	Result["total"] = Count;
	Result["next_offset"] = NextOffset;
	Result["has_more"] = NextOffset < Count;
	return Result;
}

static Json WriteGit(const Json& Input)
{
	if (Git == nullptr)
	{
		return Error("GIT_UNAVAILABLE", "SDK Git integration is unavailable");
	}
	if (Git->Busy.load())
	{
		return Error("GIT_BUSY", "An SDK Git task is already running");
	}
	const xr_string Action = Text(Input, "action");
	if (Action == "refresh")
	{
		Git->RefreshStatus(true);
		if (!Git->IsRepository)
		{
			return Error("GIT_UNAVAILABLE", "The SDK did not detect a local Git repository");
		}
	}
	else if (!Git->IsRepository)
	{
		return Error("GIT_UNAVAILABLE", "The SDK did not detect a local Git repository");
	}
	else if (Action == "commit")
	{
		const xr_string Message = Text(Input, "message");
		if (Message.empty() || Message.find_first_not_of(" \t\r\n") == xr_string::npos)
		{
			return Error("INVALID_COMMIT_MESSAGE", "Provide a nonempty commit message");
		}
		{
			xrSRWLockGuard Guard(Git->Mutex, true);
			if (std::none_of(Git->Files.begin(), Git->Files.end(), [](const auto& Entry) { return Entry.second.Staged; }))
			{
				return Error("GIT_NOTHING_STAGED", "Stage changes before committing");
			}
		}
		Git->Commit(Message);
	}
	else if (Action == "stage" || Action == "unstage")
	{
		xr_string Path = Text(Input, "path");
		if (!RelativeName(Path))
		{
			return Error("INVALID_GIT_PATH", "Provide one repository-relative file from the SDK Git status");
		}
		xr_strlwr(Path);
		xr_string Root;
		{
			xrSRWLockGuard Guard(Git->Mutex, true);
			const auto Found = Git->Files.find(Path);
			if (Found == Git->Files.end() || Found->second.IsDirectory || (Action == "unstage" && !Found->second.Staged))
			{
				return Error("GIT_PATH_UNAVAILABLE", "File is absent from the relevant SDK Git status");
			}
			Root = Git->RepositoryRoot;
		}
		const xr_path File(Files::path(Root.c_str()) / Files::u8path(Path.c_str()));
		if (Action == "stage")
		{
			Git->Stage(File);
		}
		else
		{
			Git->Unstage(File);
		}
	}
	else
	{
		return Error("INVALID_GIT_ACTION", "Use refresh, stage, unstage, or commit");
	}
	return {{"ok", true}, {"status", "submitted"}, {"action", Action.c_str()}, {"completion_verified", false}};
}

static xr_string Utf8Path(const Files::path& Path)
{
	const auto Name = Path.u8string();
	return reinterpret_cast<const char*>(Name.c_str());
}

class PackTask final
{
public:
	Json Start(const Json& Input)
	{
		if (Process)
		{
			return Error("PACK_BUSY", "Poll scene.pack_status before starting another pack operation");
		}
		if (!FS.path_exist(_game_data_))
		{
			return Error("PACK_SOURCE_UNAVAILABLE", "The SDK game data root is unavailable");
		}
		xr_string Folder = Text(Input, "folder");
		const xr_string Mode = Text(Input, "mode", "normal");
		if ((Input.contains("folder") && !RelativeName(Folder)) || (Mode != "normal" && Mode != "fast" && Mode != "store"))
		{
			return Error("INVALID_PACK_PARAMS", "Use a gamedata subfolder and normal, fast, or store mode");
		}
		std::error_code Code;
		Files::path Source;
		if (Folder.empty())
		{
			string_path Root;
			FS.update_path(Root, _game_data_, "");
			Source = Files::weakly_canonical(Files::path(Root), Code);
			if (Source.filename().empty())
			{
				Source = Source.parent_path();
			}
		}
		else
		{
			Source = ResolvePath(_game_data_, Folder);
		}
		if (Code || Source.empty() || Source == Source.root_path() || !Files::is_directory(Source, Code))
		{
			return Error("PACK_SOURCE_UNAVAILABLE", "The source gamedata directory does not exist");
		}
		const char* Bin = SDL_GetBasePath();
		if (Bin == nullptr)
		{
			return Error("PACK_TOOL_UNAVAILABLE", "Could not resolve the SDK binary directory");
		}
#ifdef IXR_WINDOWS
		const auto Executable = Files::u8path(Bin) / "xrCompress.exe";
#else
		const auto Executable = Files::u8path(Bin) / "xrCompress";
#endif
		if (!NonemptyFile(Executable))
		{
			return Error("PACK_TOOL_UNAVAILABLE", "The SDK bin directory does not contain xrCompress");
		}
		const xr_string ExecName = Utf8Path(Executable);
		const xr_string SourceName = Utf8Path(Source);
		if (SourceName.size() >= sizeof(string_path) - 1)
		{
			return Error("PACK_PATH_TOO_LONG", "The source directory exceeds the SDK path limit");
		}
		// xrCompress scans its whole command line for these flags.
		for (const char* Flag : {"-unpack", "-diff", "-store", "-fast", "-ltx", "-header"})
		{
			if (ExecName.find(Flag) != xr_string::npos || SourceName.find(Flag) != xr_string::npos)
			{
				return Error("INVALID_PACK_PATH", "The SDK compressor cannot handle an option-like path");
			}
		}
		Target = Source;
		const auto Existing = FindOutputs(Code);
		if (Code)
		{
			return Error("PACK_OUTPUT_UNAVAILABLE", "Could not inspect the archive destination directory");
		}
		if (!Existing.empty())
		{
			return Error("PACK_OUTPUT_EXISTS", "Archive files already exist beside this directory");
		}
		const char* Arguments[] = {ExecName.c_str(), SourceName.c_str(), Mode == "normal" ? nullptr : Mode == "fast" ? "-fast" : "-store", nullptr};
		Process.reset(SDL_CreateProcess(Arguments, false));
		if (!Process)
		{
			return Error("PACK_LAUNCH_FAILED", "The SDK compressor process did not start");
		}
		Result = {{"ok", true}, {"status", "running"}, {"folder", SourceName.c_str()}};
		return Result;
	}

	Json Status()
	{
		if (!Process)
		{
			return Result;
		}
		int ExitCode = 0;
		if (!SDL_WaitProcess(Process.get(), false, &ExitCode))
		{
			return Result;
		}
		Process.reset();
		Json Archives = Json::array();
		std::error_code Code;
		for (const auto& Output : FindOutputs(Code))
		{
			if (NonemptyFile(Output))
			{
				Archives.push_back(Utf8Path(Output).c_str());
			}
		}
		const bool Succeeded = ExitCode == 0 && !Code && !Archives.empty();
		Result["ok"] = Succeeded;
		Result["status"] = Succeeded ? "succeeded" : "failed";
		Result["exit_code"] = ExitCode;
		Result["archives"] = std::move(Archives);
		if (!Succeeded)
		{
			Result["error"] = {{"code", "PACK_FAILED"}, {"message", "Compressor failed or produced no nonempty archives"}};
		}
		return Result;
	}

private:
	xr_vector<Files::path> FindOutputs(std::error_code& Code) const
	{
		xr_vector<Files::path> Paths;
		xr_string Prefix = Utf8Path(Target.filename());
		xr_strlwr(Prefix);
		const xr_string PackPrefix = Prefix + ".pack_#";
		const xr_string XdbPrefix = Prefix + ".xdb";
		const auto IsVolume = [](const xr_string& Filename, const xr_string& Stem)
		{
			return Filename.size() > Stem.size() && Filename.compare(0, Stem.size(), Stem) == 0 &&
				Filename.find_first_not_of("0123456789", Stem.size()) == xr_string::npos;
		};
		for (Files::directory_iterator It(Target.parent_path(), Code), End; !Code && It != End; It.increment(Code))
		{
			xr_string Filename = Utf8Path(It->path().filename());
			xr_strlwr(Filename);
			if (IsVolume(Filename, PackPrefix) || IsVolume(Filename, XdbPrefix))
			{
				Paths.push_back(It->path());
			}
		}
		std::sort(Paths.begin(), Paths.end());
		return Paths;
	}

	std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)> Process{nullptr, SDL_DestroyProcess};
	Files::path Target;
	Json Result = {{"ok", true}, {"status", "idle"}};
};

static PackTask& Compressor()
{
	static PackTask Task;
	return Task;
}

Json SelfTestMetadata(const Files::path& TestOutput)
{
	const auto Path = TestOutput / "ai-project-tools.thm";
	STextureParams Original;
	Original.width = 16;
	Original.height = 32;
	CMemoryWriter Fixture;
	WriteMetadata(Fixture, Original);
	Fixture.open_chunk(0x8199);
	Fixture.w_u32(0xABCDEF01);
	Fixture.close_chunk();

	xr_vector<u8> Bytes;
	xr_vector<ThmChunk> Chunks;
	STextureParams Edited;
	if (!WriteBuffer(Path, Fixture.pointer(), Fixture.size()) || !ReadMetadata(Path, Bytes, Chunks, Edited) ||
		!SetMetadata(Edited, {{"format", 3}, {"generate_mipmaps", false}, {"material_weight", 0.5}, {"bump_name", "ai_test/checker"}}))
	{
		return Error("SELFTEST_THM_SETUP", "Could not prepare and edit the THM fixture");
	}
	CMemoryWriter Writer;
	WriteMetadata(Writer, Edited, Bytes, Chunks);
	Chunks.clear();
	STextureParams Checked;
	if (!WriteBuffer(Path, Writer.pointer(), Writer.size()) || !ReadMetadata(Path, Bytes, Chunks, Checked) ||
		DescribeMetadata(Checked) != DescribeMetadata(Edited) || Chunks.empty() || Chunks.back().Id != 0x8199)
	{
		return Error("SELFTEST_THM_ROUND_TRIP", "Could not reopen the edited THM with its unknown chunk preserved");
	}
	const Json Metadata = DescribeMetadata(Checked);
	Chunks.clear();
	if (!WriteBuffer(Path, Writer.pointer(), Writer.size() - 1) || ReadMetadata(Path, Bytes, Chunks, Checked))
	{
		return Error("SELFTEST_THM_VALIDATION", "A truncated THM passed validation");
	}
	std::error_code Code;
	Files::remove(Path, Code);
	return {{"ok", true}, {"metadata", Metadata}};
}

std::optional<Json> Dispatch(const xr_string& Method, const Json& Params)
{
	if (Method == "scene.texture_thm" || Method == "scene.texture_thm_set")
	{
		return Metadata(Params, Method == "scene.texture_thm_set");
	}
	if (Method == "scene.texture_import")
	{
		return ImportTexture(Params);
	}
	if (Method == "scene.dcc_export_obj")
	{
		return ExportObj(Params);
	}
	if (Method == "scene.pack_start")
	{
		return Compressor().Start(Params);
	}
	if (Method == "scene.pack_status")
	{
		return Compressor().Status();
	}
	if (Method == "scene.git_read")
	{
		return ReadGit(Params);
	}
	if (Method == "scene.git_write")
	{
		return WriteGit(Params);
	}
	return std::nullopt;
}
}
