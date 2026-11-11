#include "stdafx.h"
#include "PluginLua.h"

#include "../../../xrScripts/linker.h"
#include "../../../xrScripts/script_engine.h"

#include <fstream>
#include <lua.h>

CPluginLua::CPluginLua()
{
	Type = EPluginType::Lua;
}

void CPluginLua::Run()
{
	xr_map<xr_string, xr_string> Arguments;
	for (const auto& [Name, Value] : InputArgsValues)
	{
		Arguments[Name] = Value;
	}
	RunChecked(Arguments);
}

bool CPluginLua::RunChecked(const xr_map<xr_string, xr_string>& Arguments)
{
	lua_State* State = g_pScriptEngine ? g_pScriptEngine->lua() : nullptr;
	const size_t Extension = Scene->full_name.rfind(".level");
	if (State == nullptr || (Extension == xr_string::npos && (IsSimple() || InputArgsName.contains("level"))))
	{
		return false;
	}

	const int Base = lua_gettop(State);
	lua_pushnil(State);
	lua_setglobal(State, "main");
	if (luaL_dofile(State, Path.c_str()) != 0)
	{
		Msg("! Error loading plugin %s: %s", Name.c_str(), lua_tostring(State, -1));
		lua_settop(State, Base);
		return false;
	}
	lua_getglobal(State, "main");
	if (!lua_isfunction(State, -1))
	{
		Msg("! Missing plugin entry function main in %s", Name.c_str());
		lua_settop(State, Base);
		return false;
	}

	const xr_string LevelDirectory = Extension == xr_string::npos ? "" : Scene->full_name.substr(0, Extension) + "\\";
	int Count = 0;
	if (IsSimple())
	{
		lua_pushstring(State, LevelDirectory.c_str());
		Count = 1;
	}
	else
	{
		for (const auto& [Key, Description] : InputArgsName)
		{
			const auto Value = Arguments.find(Key);
			lua_pushstring(State, Key == "level" ? LevelDirectory.c_str() : (Value != Arguments.end() ? Value->second.c_str() : ""));
			Count++;
		}
	}

	const bool Success = lua_pcall(State, Count, 0, 0) == 0;
	if (!Success)
	{
		Msg("! Plugin %s failed: %s", Name.c_str(), lua_tostring(State, -1));
	}
	lua_settop(State, Base);
	return Success;
}

xr_string CPluginLua::ReadDesc() const
{
	std::ifstream file(Path.data());

	xr_string line;
	xr_string Result = "Not found description!";

	const std::string_view prefix = "-- desc: ";
	const std::string_view prefix_input = "-- input: ";

	while (std::getline(file, line))
	{
		if (line.rfind(prefix, 0) == 0)
		{
			size_t pos = line.find(':');
			if (pos != std::string::npos && pos + 1 < line.size())
			{
				Result = line.substr(pos + 1);
			}
		}
		else if (line.rfind(prefix_input, 0) == 0)
		{
			std::regex pattern(R"(\[([^,]+), ([^\]]+)\])");

			std::smatch match;
			xr_string::const_iterator searchStart(line.cbegin());

			while (std::regex_search(searchStart, line.cend(), match, pattern))
			{
				InputArgsName[match[1].str().c_str()] = match[2].str().c_str();
				searchStart = match.suffix().first;
			}
		}
	}

	return Result;
}