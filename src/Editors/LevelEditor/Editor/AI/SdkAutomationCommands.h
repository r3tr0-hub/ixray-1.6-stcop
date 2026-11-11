#pragma once

#include "Plugins/PluginLua.h"

namespace AiSdkAutomation
{
static xr_string PluginId(const IPluginBase& Plugin)
{
	string_path Plugins;
	FS.update_path(Plugins, "$fs_root$", "plugins");
	std::error_code Code;
	const auto Root = std::filesystem::canonical(std::filesystem::u8path(Plugins), Code);
	if (Code)
	{
		return {};
	}
	const auto Path = std::filesystem::canonical(std::filesystem::u8path(Plugin.Path.c_str()), Code);
	if (Code || !AiFileWritten(Path))
	{
		return {};
	}
	const auto Relative = Path.lexically_relative(Root);
	if (Relative.empty() || Relative == "." || *Relative.begin() == "..")
	{
		return {};
	}
	return AiPathText(Relative);
}

#ifdef IXR_WINDOWS
static std::wstring QuoteArgument(const std::wstring& Argument)
{
	std::wstring Result(1, L'"');
	size_t Backslashes = 0;
	for (wchar_t Character : Argument)
	{
		if (Character == L'\\')
		{
			Backslashes++;
			continue;
		}
		Result.append(Character == L'"' ? 2 * Backslashes + 1 : Backslashes, L'\\');
		Result += Character;
		Backslashes = 0;
	}
	Result.append(2 * Backslashes, L'\\');
	Result += L'"';
	return Result;
}
#endif
}

inline AiSceneExecutor::Json AiSceneExecutor::DispatchPlugins(const xr_string& Method, const Json& Params)
{
	if (Method != "scene.plugin_list" && Method != "scene.plugin_run")
	{
		return nullptr;
	}

	const auto& Plugins = CPluginsManagers::Instance().Plugins;
	if (Method == "scene.plugin_list")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit))
		{
			return Error("INVALID_PAGE", "Provide a valid page");
		}
		size_t Total = 0;
		Json Items = Json::array();
		for (const IPluginBase* Plugin : Plugins)
		{
			const xr_string Id = AiSdkAutomation::PluginId(*Plugin);
			if (Id.empty() || Total++ < Offset || Items.size() >= Limit)
			{
				continue;
			}
			Json Arguments = Json::array();
			for (const auto& [Name, Description] : Plugin->InputArgsName)
			{
				Arguments.push_back({{"name", Name.c_str()}, {"description", AiText::Utf8(Description.c_str()).c_str()}, {"from_scene", Plugin->Type == EPluginType::Lua && Name == "level"}});
			}
			Items.push_back({{"id", Id.c_str()}, {"language", Plugin->Type == EPluginType::Lua ? "lua" : "python"}, {"description", AiText::Utf8(Plugin->Desc.c_str()).c_str()}, {"arguments", Arguments}});
		}
		const size_t Next = std::min(Offset, Total) + Items.size();
		return {{"ok", true}, {"plugins", Items}, {"total", Total}, {"next_offset", Next}, {"has_more", Next < Total}};
	}

	if (!AiHasString(Params, "plugin_id", 240) || AiString(Params, "plugin_id").empty())
	{
		return Error("INVALID_PLUGIN", "Provide an installed plugin ID from scene.plugin_list");
	}
	if (LTools->IsCompilerRunning() || LTools->IsGameRunning())
	{
		return Error("SDK_BUSY", "Wait for the current SDK process");
	}
	const xr_string Id = AiString(Params, "plugin_id");
	IPluginBase* Plugin = nullptr;
	for (IPluginBase* Candidate : Plugins)
	{
		if (AiSdkAutomation::PluginId(*Candidate) == Id)
		{
			Plugin = Candidate;
			break;
		}
	}
	if (Plugin == nullptr)
	{
		return Error("UNKNOWN_PLUGIN", "Select an installed plugin listed by the editor");
	}

	const auto Values = Params.find("arguments");
	if (Values != Params.end() && !Values->is_object())
	{
		return Error("INVALID_ARGUMENTS", "Plugin arguments must be a string map");
	}
	xr_map<xr_string, xr_string> Arguments;
	if (Values != Params.end())
	{
		for (const auto& [Key, Value] : Values->items())
		{
			const xr_string Name = Key.c_str();
			if (!Plugin->InputArgsName.contains(Name) || (Plugin->Type == EPluginType::Lua && Name == "level") ||
				!Value.is_string() || Value.get_ref<const Json::string_t&>().size() > 255 ||
				Value.get_ref<const Json::string_t&>().find('\0') != Json::string_t::npos)
			{
				return Error("INVALID_ARGUMENTS", "Supply only the named arguments declared by the plugin");
			}
			Arguments[Name] = Value.get_ref<const Json::string_t&>().c_str();
		}
	}
	for (const auto& [Name, Description] : Plugin->InputArgsName)
	{
		if (!Arguments.contains(Name) && (Plugin->Type != EPluginType::Lua || Name != "level"))
		{
			return Error("MISSING_PLUGIN_ARGUMENT", "Supply each declared plugin argument, using an empty string where needed");
		}
	}
	if (Plugin->Type == EPluginType::Lua)
	{
		if (!static_cast<CPluginLua*>(Plugin)->RunChecked(Arguments))
		{
			return Error("PLUGIN_FAILED", "The Lua plugin failed; check the SDK log and whether a level is open");
		}
		return {{"ok", true}, {"status", "completed"}, {"completion_verified", true}, {"plugin_id", Id.c_str()}};
	}

#ifdef IXR_WINDOWS
	wchar_t Python[32768] = {};
	const auto Length = SearchPathW(nullptr, L"python.exe", nullptr, std::size(Python), Python, nullptr);
	if (Length == 0 || Length >= std::size(Python))
	{
		return Error("PYTHON_MISSING", "Install Python and make python.exe available on PATH");
	}
	string_path Root;
	FS.update_path(Root, "$fs_root$", "");
	const auto WorkingDirectory = std::filesystem::u8path(Root);
	const auto PluginPath = std::filesystem::absolute(std::filesystem::u8path(Plugin->Path.c_str()));
	std::wstring Command = L"python.exe ";
	Command += AiSdkAutomation::QuoteArgument(PluginPath.wstring());
	if (Plugin->IsSimple())
	{
		if (Scene->full_name.empty())
		{
			return Error("SAVE_REQUIRED", "Open or save a level before starting a level plugin");
		}
		Arguments["level"] = Scene->full_name;
	}
	for (const auto& [Name, Value] : Arguments)
	{
		if (Name.empty() || Name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != xr_string::npos)
		{
			return Error("INVALID_PLUGIN_ARGUMENT_NAME", "A Python plugin argument name is not a supported command-line option");
		}
		const std::wstring Option = L"-" + std::filesystem::u8path(Name.c_str()).wstring() + L"=" + std::filesystem::u8path(Value.c_str()).wstring();
		Command += L" ";
		Command += AiSdkAutomation::QuoteArgument(Option);
	}
	if (!LTools->RunAssistantProcess(Python, Command.data(), WorkingDirectory.c_str()))
	{
		return Error("PLUGIN_START_FAILED", "The Python plugin could not start");
	}
	return {{"ok", true}, {"status", "running"}, {"completion_verified", false}, {"plugin_id", Id.c_str()}, {"compiler_run_id", LTools->GetCompilerRunId()}};
#else
	return Error("PYTHON_UNAVAILABLE", "This SDK build cannot launch Python plugins through the assistant");
#endif
}
