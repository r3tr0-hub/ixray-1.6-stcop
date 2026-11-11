#pragma once

namespace AiDocuments
{
template <typename TCall, typename TCheck>
inline void SmokeTest(u64 RunId, TCall&& Call, TCheck&& Check)
{
	using Json = nlohmann::json;
	struct TestResources
	{
		~TestResources()
		{
			EContext.UI->ActiveTabIndex = ActiveTab;
			EContext.UI->RequestedTabIndex = RequestedTab;
			for (const auto& File : Files)
			{
				FS.file_delete(File.c_str());
			}
		}

		bool Write(const char* Relative, const char* Contents)
		{
			string_path Path = {};
			FS.update_path(Path, _game_config_, Relative);
			if (FS.exist(Path))
			{
				return false;
			}
			IWriter* Writer = FS.w_open(Path);
			if (Writer == nullptr || !Writer->valid())
			{
				FS.w_close(Writer);
				return false;
			}
			Writer->w(Contents, xr_strlen(Contents));
			FS.w_close(Writer, true);
			Files.emplace_back(Path);
			return true;
		}

		int ActiveTab = EContext.UI->ActiveTabIndex;
		int RequestedTab = EContext.UI->RequestedTabIndex;
		xr_vector<xr_string> Files;
	} Resources;

	auto Ok = [](const Json& Result)
	{
		return Result.value("ok", false);
	};
	const Json Macros = Call("scene.macro_document", {{"action", "read"}}, true);
	Check("sdk_macros_read_native_graph", Ok(Macros) && Macros.value("total", 0) >= 1 && !Macros.value("commands", Json::array()).empty(), Macros);
	const Json Layout = Call("scene.layout_document", {{"action", "read"}}, true);
	Check("sdk_layout_read_native_widgets", Ok(Layout) && !Layout.value("types", Json::array()).empty(), Layout);

	const xr_string RunKey = std::to_string(RunId).c_str();
	const xr_string LogicName = "ai_sdk_selftest_" + RunKey + ".ltx";
	const xr_string Config = "scripts\\" + LogicName;
	const xr_string CoderPath = "configs/" + Config;
	if (!Resources.Write(Config.c_str(), "[logic]\r\nactive = walker@camp\r\n\r\n[walker@camp]\r\npath_walk = camp_walk\r\n"))
	{
		Check("sdk_document_fixture", false, Json::object());
		return;
	}
	const Json CodeOpen = Call("scene.code_document", {{"action", "open"}, {"path", CoderPath.c_str()}}, true);
	if (!Ok(CodeOpen))
	{
		Check("sdk_code_fixture_open", false, CodeOpen);
		return;
	}
	const Json CodeEdit = Call("scene.code_document", {{"action", "replace"}, {"old_text", "camp_walk"}, {"new_text", "camp_walk_new"}}, true);
	const Json CodeSave = Call("scene.code_document", {{"action", "save"}}, true);
	const Json CodeReload = Call("scene.code_document", {{"action", "reload"}}, true);
	const Json CodeRead = Call("scene.code_document", {{"action", "read"}}, true);
	const auto Lines = CodeRead.value("lines", Json::array());
	const bool FoundCode = std::any_of(Lines.begin(), Lines.end(), [](const Json& Line)
	{
		return Line.value("text", "") == xr_string_view("path_walk = camp_walk_new");
	});
	Check("sdk_code_edit_save_reload", Ok(CodeOpen) && Ok(CodeEdit) && Ok(CodeSave) && Ok(CodeReload) && FoundCode,
		{{"open", CodeOpen}, {"edit", CodeEdit}, {"save", CodeSave}, {"reload", CodeReload}, {"read", CodeRead}});

	const Json LogicOpen = Call("scene.logic_document", {{"action", "open"}, {"path", LogicName.c_str()}}, true);
	if (!Ok(LogicOpen))
	{
		Check("sdk_logic_fixture_open", false, LogicOpen);
		return;
	}
	const Json LogicEdit = Call("scene.logic_document",
		{{"action", "set_key"}, {"section", "walker@camp"}, {"key", "path_walk"}, {"value", "camp_walk_second"}}, true);
	const Json LogicSave = Call("scene.logic_document", {{"action", "save"}}, true);
	const Json LogicReload = Call("scene.logic_document", {{"action", "reload"}}, true);
	const Json LogicRead = Call("scene.logic_document", {{"action", "read"}, {"section", "walker@camp"}}, true);
	const auto Keys = LogicRead.value("keys", Json::array());
	const bool FoundKey = std::any_of(Keys.begin(), Keys.end(), [](const Json& Key)
	{
		return Key.value("key", "") == xr_string_view("path_walk") && Key.value("value", "") == xr_string_view("camp_walk_second");
	});
	Check("sdk_logic_edit_save_reload", Ok(LogicOpen) && Ok(LogicEdit) && Ok(LogicSave) && Ok(LogicReload) && FoundKey,
		{{"open", LogicOpen}, {"edit", LogicEdit}, {"save", LogicSave}, {"reload", LogicReload}, {"read", LogicRead}});

	const xr_string DialogName = "dialogs_ai_sdk_selftest_" + RunKey + ".xml";
	if (!Resources.Write(("gameplay\\" + DialogName).c_str(),
		"<game_dialogs><dialog id=\"ai_test\"><phrase_list><phrase id=\"0\"><text>ai_original</text></phrase></phrase_list></dialog></game_dialogs>\r\n"))
	{
		Check("sdk_dialog_fixture", false, Json::object());
		return;
	}
	const Json DialogOpen = Call("scene.dialog_document", {{"action", "open"}, {"path", DialogName.c_str()}}, true);
	if (!Ok(DialogOpen))
	{
		Check("sdk_dialog_fixture_open", false, DialogOpen);
		return;
	}
	const Json DialogEdit = Call("scene.dialog_document", {{"action", "set_phrase"}, {"dialog", "ai_test"}, {"phrase", "0"}, {"value", "ai_new_text"}}, true);
	const Json DialogSave = Call("scene.dialog_document", {{"action", "save"}}, true);
	const Json DialogReload = Call("scene.dialog_document", {{"action", "reload"}}, true);
	const Json DialogRead = Call("scene.dialog_document", {{"action", "read"}, {"dialog", "ai_test"}}, true);
	const auto Phrases = DialogRead.value("phrases", Json::array());
	Check("sdk_dialog_edit_save_reload", Ok(DialogOpen) && Ok(DialogEdit) && Ok(DialogSave) && Ok(DialogReload) &&
		Phrases.size() == 1 && Phrases.front().value("text", "") == xr_string_view("ai_new_text"),
		{{"open", DialogOpen}, {"edit", DialogEdit}, {"save", DialogSave}, {"reload", DialogReload}, {"read", DialogRead}});
}
}
