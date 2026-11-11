#pragma once

#include "AiText.h"
#include "Nodes/Logic/UILogicEditorForm.h"
#include "Nodes/UIDialogsView.h"
#include "Nodes/UIMacroView.h"
#include "Nodes/UINodesEditorForm.h"
#include "UI/Coder/UICoderForm.h"
#include "UI/LayoutEditor/UILayoutEditorForm.h"
#include "Editor/XVisor/XVisor.h"

#include <charconv>
#include <cmath>
#include <filesystem>

namespace AiDocuments
{
using Json = nlohmann::json;

inline bool Handles(const xr_string& Method)
{
	return Method == "scene.logic_document" || Method == "scene.dialog_document" || Method == "scene.macro_document" ||
		Method == "scene.layout_document" || Method == "scene.code_document";
}

inline Json Error(const char* Code, const char* Message)
{
	return {{"ok", false}, {"code", Code}, {"message", Message}};
}

inline bool String(const Json& Params, const char* Key, xr_string& Out, size_t Limit = 200, bool EmptyAllowed = false)
{
	const auto Item = Params.find(Key);
	if (Item == Params.end() || !Item->is_string())
	{
		return false;
	}
	const auto& Text = Item->get_ref<const Json::string_t&>();
	if ((!EmptyAllowed && Text.empty()) || Text.size() > Limit || Text.find('\0') != Json::string_t::npos)
	{
		return false;
	}
	Out.assign(Text.data(), Text.size());
	return true;
}

inline xr_string String(const Json& Params, const char* Key)
{
	xr_string Out;
	String(Params, Key, Out);
	return Out;
}

inline bool Token(const xr_string& Name)
{
	return !Name.empty() && Name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@.-:") == xr_string::npos;
}

inline bool Integer(const Json& Params, const char* Key, u32& Out, u32 Max = 0x7fffffff)
{
	const auto Item = Params.find(Key);
	if (Item == Params.end() || !Item->is_number_integer())
	{
		return false;
	}
	const double Value = Item->get<double>();
	if (Value < 0 || Value > Max)
	{
		return false;
	}
	Out = static_cast<u32>(Value);
	return true;
}

inline bool Number(const Json& Params, const char* Key, float& Out, float Min = -100000.0f)
{
	const auto Item = Params.find(Key);
	if (Item == Params.end())
	{
		return true;
	}
	if (!Item->is_number())
	{
		return false;
	}
	const double Value = Item->get<double>();
	if (!std::isfinite(Value) || Value < Min || Value > 100000)
	{
		return false;
	}
	Out = static_cast<float>(Value);
	return true;
}

struct Page
{
	explicit Page(const Json& Params)
	{
		Valid = (!Params.contains("offset") || Integer(Params, "offset", Offset)) &&
			(!Params.contains("limit") || (Integer(Params, "limit", Limit, 100) && Limit > 0));
	}

	void Add(Json Item)
	{
		if (Count++ >= Offset && Items.size() < Limit)
		{
			Items.push_back(std::move(Item));
		}
	}

	void Write(Json& Result, const char* Key)
	{
		Result[Key] = std::move(Items);
		Result["total"] = Count;
		const size_t Next = std::min<size_t>(Offset, Count) + Result[Key].size();
		Result["next_offset"] = Next;
		Result["has_more"] = Next < Count;
	}

	Json Items = Json::array();
	size_t Count = 0;
	u32 Offset = 0;
	u32 Limit = 30;
	bool Valid = false;
};

inline bool RelativeFile(const xr_string& Name, const char* Alias, xr_string& Full)
{
	if (Name.empty() || Name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/\\.-") != xr_string::npos ||
		Name.front() == '/' || Name.front() == '\\')
	{
		return false;
	}
	xr_string Normalized = Name;
	std::replace(Normalized.begin(), Normalized.end(), '\\', '/');
	const std::filesystem::path Relative = Normalized.c_str();
	for (const auto& Part : Relative)
	{
		if (Part.empty() || Part == "." || Part == ".." || Part.string().ends_with("."))
		{
			return false;
		}
	}
	if (!FS.path_exist(Alias))
	{
		return false;
	}
	std::replace(Normalized.begin(), Normalized.end(), '/', Platform::kPreferredSeparator[0]);
	string_path RootBuffer = {};
	string_path FileBuffer = {};
	FS.update_path(RootBuffer, Alias, "");
	if (xr_strlen(RootBuffer) + Normalized.size() >= sizeof(FileBuffer))
	{
		return false;
	}
	FS.update_path(FileBuffer, Alias, Normalized.c_str());

	std::error_code Ec;
	const auto Root = std::filesystem::weakly_canonical(std::filesystem::u8path(AiText::Utf8(RootBuffer).c_str()), Ec);
	if (Ec)
	{
		return false;
	}
	const auto Path = std::filesystem::weakly_canonical(std::filesystem::u8path(AiText::Utf8(FileBuffer).c_str()), Ec);
	const auto Local = Path.lexically_relative(Root);
	if (Ec || Local.empty() || *Local.begin() == ".." || !FS.exist(FileBuffer))
	{
		return false;
	}
	Full = FileBuffer;
	return true;
}

inline Json Operation(bool Success)
{
	return Success ? Json{{"ok", true}} : Error("DOCUMENT_OPERATION_FAILED", "The SDK did not complete this document operation");
}

inline bool Encodable(bool Utf8, const xr_string& Text)
{
	return Utf8 || Platform::ANSI_TO_UTF8(Platform::UTF8_to_CP1251(Text)) == Text;
}
}

struct AiDocumentAccess
{
	using Json = AiDocuments::Json;
	using Page = AiDocuments::Page;

	static Json Logic(const xr_string& Action, const Json& Params, Page& Results)
	{
		using namespace AiDocuments;
		auto* Form = UILogicEditorForm::Form;
		if (Form == nullptr)
		{
			return Error("EDITOR_UNAVAILABLE", "Logic editor is unavailable");
		}
		if (Action == "open")
		{
			const auto Path = String(Params, "path");
			xr_string Full;
			if (!Path.ends_with(".ltx") || !RelativeFile(xr_string("scripts/") + Path, _game_config_, Full))
			{
				return Error("INVALID_PATH", "Use an existing .ltx path relative to configs/scripts");
			}
			if (!UILogicEditorForm::OpenFile(Full))
			{
				return Operation(false);
			}
			Form->Active = static_cast<size_t>(Form->SelectRequest);
		}
		auto* Document = Form->Current();
		if (Document == nullptr)
		{
			return Error("NO_DOCUMENT", "Open a logic document first");
		}

		const xr_string SectionName = String(Params, "section");
		if (Action == "save")
		{
			if (!Encodable(Document->Utf8, Document->Logic.Save()))
			{
				return Error("ENCODING_LOSS", "The logic text has characters the original encoding cannot save");
			}
			return Operation(Form->SaveDocument(*Document));
		}
		if (Action == "reload")
		{
			return Document->Modified ? Error("UNSAVED_CHANGES", "Save or undo the changes first") : Operation(Form->Load(*Document));
		}
		if (Action == "undo" || Action == "redo")
		{
			return Operation(Form->UndoDocument(*Document, Action == "redo"));
		}
		if (Action == "add_section" || Action == "rename_section")
		{
			const xr_string NewName = Action == "add_section" ? SectionName : String(Params, "new_name");
			if (!Token(NewName) || Document->Logic.Find(NewName) != nullptr ||
				(Action == "rename_section" && Document->Logic.Find(SectionName) == nullptr))
			{
				return Error("INVALID_SECTION", "Specify a valid unused section name and an existing source for a rename");
			}
			Form->PushUndo(*Document);
			if (Action == "add_section")
			{
				Document->Logic.AddSection(NewName);
			}
			else
			{
				Document->Logic.RenameSection(SectionName, NewName);
			}
			Form->Validate(*Document);
			return {{"ok", true}, {"section", NewName.c_str()}};
		}
		if (Action == "set_key" || Action == "remove_key")
		{
			auto* Section = Document->Logic.Find(SectionName);
			const xr_string Key = String(Params, "key");
			xr_string Value;
			if (Section == nullptr || !Token(Key) ||
				(Action == "set_key" && (!String(Params, "value", Value, 4096, true) || Value.find_first_of("\r\n") != xr_string::npos)))
			{
				return Error("INVALID_KEY", "Specify an existing section, a key and a single-line value when setting it");
			}
			SLogicKey* Existing = Section->FindKey(Key.c_str());
			if (Action == "remove_key" && Existing == nullptr)
			{
				return Error("UNKNOWN_KEY", "The section does not have that key");
			}
			if (Action == "set_key" && Existing != nullptr && Existing->Value == Value)
			{
				return {{"ok", true}, {"changed", false}, {"value", Value.c_str()}};
			}
			Form->PushUndo(*Document);
			if (Action == "remove_key")
			{
				const auto Line = std::find_if(Section->Lines.begin(), Section->Lines.end(), [Existing](const SLogicLine& Item) { return &Item.Key == Existing; });
				Section->RemoveKey(static_cast<size_t>(Line - Section->Lines.begin()));
			}
			else if (Existing != nullptr)
			{
				Existing->Value = Value;
				Existing->IsCondlist = CLogicDocument::IsCondlistKey(Existing->Name, Existing->Value);
				Existing->Parse();
			}
			else
			{
				Section->AddKey(Key.c_str(), Value.c_str());
			}
			Form->Validate(*Document);
			return {{"ok", true}, {"section", SectionName.c_str()}, {"key", Key.c_str()}, {"value", Value.c_str()}, {"removed", Action == "remove_key"}};
		}
		if (Action != "read" && Action != "open")
		{
			return Error("INVALID_ACTION", "Unsupported Logic action");
		}
		Json Response = {{"ok", true}, {"file", AiText::Utf8(Document->Name.c_str()).c_str()}, {"modified", Document->Modified}};
		if (SectionName.empty())
		{
			for (const auto& Section : Document->Logic.Sections)
			{
				Results.Add({{"name", Section->Name.c_str()}, {"parents", Section->Parents.c_str()}});
			}
			Results.Write(Response, "sections");
		}
		else if (const auto* Section = Document->Logic.Find(SectionName))
		{
			for (const auto& Line : Section->Lines)
			{
				if (Line.IsKey)
				{
					Results.Add({{"key", Line.Key.Name.c_str()}, {"value", Line.Key.Value.c_str()}, {"condlist", Line.Key.IsCondlist}});
				}
			}
			Results.Write(Response, "keys");
		}
		else
		{
			return Error("UNKNOWN_SECTION", "The section does not exist");
		}
		return Response;
	}

	static Json Dialog(const xr_string& Action, const Json& Params, Page& Results)
	{
		using namespace AiDocuments;
		auto* Form = DialogEditor::Instance;
		if (Form == nullptr)
		{
			return Error("EDITOR_UNAVAILABLE", "Dialogs editor is unavailable");
		}
		if (Action == "open" || Action == "reload")
		{
			const xr_string Name = Action == "reload" ? Form->DialogFileName : String(Params, "path");
			xr_string Full;
			if (!Name.starts_with("dialogs") || !Name.ends_with(".xml") || Name.find_first_of("/\\") != xr_string::npos ||
				!RelativeFile(xr_string("gameplay/") + Name, _game_config_, Full))
			{
				return Error("INVALID_PATH", "Use an existing dialogs*.xml filename in configs/gameplay");
			}
			if (Form->Modified && (Action == "reload" || Form->DialogFileName != Name))
			{
				return Error("UNSAVED_CHANGES", "Save or undo the dialog changes first");
			}
			if (Action == "reload")
			{
				CXml::RemoveFromCache(_game_config_, Form->File.m_xml_file_name);
			}
			CXml Probe;
			if (!Probe.Load(_game_config_, "gameplay", Name.c_str()) || Probe.GetRoot() == nullptr)
			{
				return Error("DIALOG_LOAD_FAILED", "The SDK could not parse that dialog file");
			}
			if (Action == "reload")
			{
				tinyxml2::XMLPrinter Printer;
				Probe.GetRoot()->GetDocument()->Print(&Printer);
				Form->RestoreState(Printer.CStr());
				Form->LastState = Form->SavedState = Form->CaptureState();
				Form->UndoStack.clear();
				Form->RedoStack.clear();
				Form->ChangePending = false;
			}
			else if (Form->DialogFileName != Name)
			{
				Form->OpenFileInstance(xr_path(Name));
			}
		}
		if (Form->DialogFileName.empty() || Form->File.GetRoot() == nullptr)
		{
			return Error("NO_DOCUMENT", "Open a dialogs file first");
		}
		if (Action == "save")
		{
			string_path Path = {};
			FS.update_path(Path, _game_config_, Form->File.m_xml_file_name);
			IWriter* Writer = FS.w_open(Path);
			if (Writer == nullptr || !Writer->valid())
			{
				FS.w_close(Writer);
				return Error("WRITE_FAILED", "The SDK could not save the dialogs file");
			}
			const xr_string State = Form->CaptureState();
			Writer->w(State.data(), State.size());
			FS.w_close(Writer, true);
			CXml::RemoveFromCache(_game_config_, Form->File.m_xml_file_name);
			Form->SavedState = State;
			Form->Modified = false;
			return Operation(true);
		}
		if (Action == "undo" || Action == "redo")
		{
			return Operation(Form->UndoMode(Action == "redo"));
		}

		const xr_string DialogName = String(Params, "dialog");
		Json Response = {{"ok", true}, {"file", AiText::Utf8(Form->DialogFileName.c_str()).c_str()}, {"modified", Form->Modified}};
		if (DialogName.empty() && (Action == "read" || Action == "open" || Action == "reload"))
		{
			for (const auto& Pair : Form->Dialogs)
			{
				Results.Add({{"dialog", AiText::Utf8(Pair.first.c_str()).c_str()}});
			}
			Results.Write(Response, "dialogs");
			return Response;
		}
		const auto Dialog = std::find_if(Form->Dialogs.begin(), Form->Dialogs.end(),
			[&](const auto& Pair) { return AiText::Utf8(Pair.first.c_str()) == DialogName; });
		if (Action == "create_dialog")
		{
			if (!Token(DialogName) || Dialog != Form->Dialogs.end())
			{
				return Error("INVALID_DIALOG", "Specify an unused dialog ID");
			}
			auto* RootElement = Form->File.GetRoot()->ToElement();
			if (RootElement == nullptr || xr_string_view(RootElement->Name()) != "game_dialogs")
			{
				return Error("INVALID_DIALOG_FILE", "Expected a game_dialogs XML document");
			}
			auto* Element = RootElement->InsertNewChildElement("dialog");
			Element->SetAttribute("id", DialogName.c_str());
			Element->InsertNewChildElement("phrase_list");
			Form->RebuildDialogList();
			Form->MarkChanged();
			Form->CommitChange();
			return {{"ok", true}, {"dialog", DialogName.c_str()}};
		}
		if (Dialog == Form->Dialogs.end())
		{
			return Error("UNKNOWN_DIALOG", "The dialog does not exist");
		}
		auto* Root = Dialog->second->FirstChildElement("phrase_list");
		if (Root == nullptr)
		{
			return Error("NO_PHRASES", "The dialog has no phrase list");
		}
		if (Action == "set_phrase" || Action == "add_phrase" || Action == "remove_phrase" ||
			Action == "link" || Action == "unlink" || Action == "move_phrase")
		{
			const xr_string PhraseId = String(Params, "phrase");
			const xr_string Value = String(Params, "value");
			const xr_string TargetId = String(Params, "target");
			const bool HasText = Action == "set_phrase" || Action == "add_phrase";
			const bool HasTarget = Action == "link" || Action == "unlink" || Action == "move_phrase";
			if (!Token(PhraseId) || (HasText && !Token(Value)) || (HasTarget && (!Token(TargetId) || TargetId == PhraseId)) ||
				(Params.contains("branch") && !Params["branch"].is_boolean()))
			{
				return Error("INVALID_PHRASE", "Specify valid phrase IDs and a string-table ID when setting text");
			}

			auto FindPhrase = [Root](const xr_string& Id)
			{
				for (auto* Item = Root->FirstChildElement("phrase"); Item != nullptr; Item = Item->NextSiblingElement("phrase"))
				{
					if (Id == AiText::Utf8(Item->Attribute("id")))
					{
						return Item;
					}
				}
				return static_cast<tinyxml2::XMLElement*>(nullptr);
			};
			auto* Phrase = FindPhrase(PhraseId);
			auto* Target = HasTarget ? FindPhrase(TargetId) : nullptr;
			if ((Phrase == nullptr) != (Action == "add_phrase") || (HasTarget && Target == nullptr))
			{
				return Error("INVALID_PHRASE", "Use existing phrase IDs for edits and an unused ID for a new phrase");
			}
			if (Form->ChangePending)
			{
				Form->CommitChange();
			}
			const bool IsActiveDialog = Form->LastOpenDialog == Dialog->first;
			bool RefreshGraph = IsActiveDialog && Action != "set_phrase";
			bool Changed = true;
			if (Action == "add_phrase")
			{
				Phrase = Root->InsertNewChildElement("phrase");
				Phrase->SetAttribute("id", PhraseId.c_str());
				if (Params.value("branch", false))
				{
					Phrase->SetAttribute("branch", "1");
				}
				Phrase->InsertNewChildElement("text")->SetText(Value.c_str());
			}
			else if (Action == "remove_phrase")
			{
				for (auto* Item = Root->FirstChildElement("phrase"); Item != nullptr; Item = Item->NextSiblingElement("phrase"))
				{
					for (auto* Link = Item->FirstChildElement("next"); Link != nullptr;)
					{
						auto* Next = Link->NextSiblingElement("next");
						if (PhraseId == AiText::Utf8(Link->GetText()))
						{
							Item->DeleteChild(Link);
						}
						Link = Next;
					}
				}
				Root->DeleteChild(Phrase);
			}
			else if (Action == "move_phrase")
			{
				auto* Before = Target->PreviousSibling();
				Changed = Before != Phrase;
				if (Changed)
				{
					if (Before != nullptr)
					{
						Root->InsertAfterChild(Before, Phrase);
					}
					else
					{
						Root->InsertFirstChild(Phrase);
					}
				}
			}
			else if (HasTarget)
			{
				bool Linked = false;
				for (auto* Link = Phrase->FirstChildElement("next"); Link != nullptr;)
				{
					auto* Next = Link->NextSiblingElement("next");
					if (TargetId == AiText::Utf8(Link->GetText()))
					{
						Linked = true;
						if (Action == "unlink")
						{
							Phrase->DeleteChild(Link);
						}
					}
					Link = Next;
				}
				Changed = Linked != (Action == "link");
				if (Action == "link" && !Linked)
				{
					Phrase->InsertNewChildElement("next")->SetText(TargetId.c_str());
				}
			}
			else
			{
				auto* Text = Phrase->FirstChildElement("text");
				Changed = Text == nullptr || AiText::Utf8(Text->GetText()) != Value;
				if (Changed)
				{
					if (Text == nullptr)
					{
						Text = Phrase->InsertNewChildElement("text");
					}
					Text->SetText(Value.c_str());
					for (auto* Node : Form->Nodes)
					{
						if (Node->ParentNode == Phrase)
						{
							Node->TextNode = Text;
							Node->SetText(Value.c_str());
							Form->RefreshProperties = true;
						}
					}
				}
			}
			if (Changed)
			{
				if (RefreshGraph)
				{
					auto* PreviousEditor = ed::GetCurrentEditor();
					Form->RestoreState(Form->CaptureState());
					ed::SetCurrentEditor(PreviousEditor);
				}
				Form->MarkChanged();
				Form->CommitChange();
			}
			return {{"ok", true}, {"dialog", DialogName.c_str()}, {"phrase", PhraseId.c_str()}, {"value", Value.c_str()}, {"target", TargetId.c_str()}, {"changed", Changed}};
		}
		if (Action != "read" && Action != "open" && Action != "reload")
		{
			return Error("INVALID_ACTION", "Unsupported Dialogs action");
		}
		for (auto* Phrase = Root->FirstChildElement("phrase"); Phrase != nullptr; Phrase = Phrase->NextSiblingElement("phrase"))
		{
			const auto* Text = Phrase->FirstChildElement("text");
			Json Next = Json::array();
			for (auto* Link = Phrase->FirstChildElement("next"); Link != nullptr; Link = Link->NextSiblingElement("next"))
			{
				Next.push_back(AiText::Utf8(Link->GetText()).c_str());
			}
			Results.Add({{"id", AiText::Utf8(Phrase->Attribute("id")).c_str()}, {"text", AiText::Utf8(Text != nullptr ? Text->GetText() : nullptr).c_str()}, {"branch", Phrase->IntAttribute("branch") == 1}, {"next", Next}});
		}
		Response["dialog"] = DialogName.c_str();
		Results.Write(Response, "phrases");
		return Response;
	}

	static Json Macro(const xr_string& Action, const Json& Params, Page& Results)
	{
		using namespace AiDocuments;
		auto* NodesForm = UINodesEditorForm::Form;
		auto* Form = NodesForm != nullptr ? static_cast<CUIMacroView*>(NodesForm->Modes[(size_t)ENodesMode::Macros].get()) : nullptr;
		if (Form == nullptr)
		{
			return Error("EDITOR_UNAVAILABLE", "Macros editor is unavailable");
		}
		auto CanRun = [](const CUIMacroView::SCommand& Command)
		{
			return Command.Type == MacroType::eVoid &&
				(Command.Command == COMMAND_SELECT_ALL || Command.Command == COMMAND_DESELECT_ALL ||
					Command.Command == COMMAND_INVERT_SELECTION_ALL || Command.Command == COMMAND_SELECT_SNAP_OBJECTS);
		};
		if (Action == "validate" || Action == "run")
		{
			const auto* Start = Form->Find(Form->StartId);
			bool Valid = Start != nullptr && !Start->Next.empty();
			Json Issues = Json::array();
			if (!Valid)
			{
				Issues.push_back({{"code", "START_NOT_LINKED"}, {"node_id", Form->StartId}});
			}
			xr_set<int> Visited;
			xr_vector<int> Pending;
			if (Valid)
			{
				Pending = Start->Next;
			}
			while (!Pending.empty() && Visited.size() < 128)
			{
				const int Id = Pending.back();
				Pending.pop_back();
				if (!Visited.insert(Id).second || Id == Form->StartId)
				{
					Issues.push_back({{"code", "REPEATED_NODE"}, {"node_id", Id}});
					Valid = false;
					continue;
				}
				const auto* Node = Form->Find(Id);
				const auto* Command = Node != nullptr ? Form->CommandOf(*Node) : nullptr;
				if (Command == nullptr)
				{
					Issues.push_back({{"code", "UNKNOWN_NODE_OR_COMMAND"}, {"node_id", Id}});
					Valid = false;
					continue;
				}
				if (!CanRun(*Command) || !Node->First.empty() || !Node->Second.empty())
				{
					Issues.push_back({{"code", "COMMAND_REQUIRES_MANUAL_RUN"}, {"node_id", Id}, {"command", Command->Name.c_str()}});
				}
				Pending.insert(Pending.end(), Node->Next.begin(), Node->Next.end());
			}
			if (!Pending.empty())
			{
				Issues.push_back({{"code", "TOO_MANY_REACHABLE_NODES"}});
				Valid = false;
			}
			const bool Runnable = Valid && Issues.empty();
			Json Result = {{"ok", true}, {"valid", Valid}, {"runnable", Runnable}, {"reachable_nodes", Visited.size()}, {"issues", Issues}};
			if (Action == "validate")
			{
				return Result;
			}
			if (!Runnable || EContext.UI->ActiveTabIndex != 0)
			{
				Result["ok"] = false;
				Result["code"] = !Runnable ? "MACRO_REQUIRES_MANUAL_RUN" : "OPEN_LEVEL_TAB";
				Result["message"] = !Runnable ? "Only the validated selection-command graph may run via MCP" : "Open the Level Editor tab before running the selection macro";
				return Result;
			}
			Form->Exec();
			Result["messages"] = Json::array();
			for (const auto& Item : Form->Messages)
			{
				Result["messages"].push_back({{"node_id", Item.Node}, {"error", Item.IsError}, {"message", AiText::Utf8(Item.Text.c_str()).c_str()}});
				if (Item.IsError)
				{
					Result["ok"] = false;
					Result["code"] = "MACRO_RUN_FAILED";
				}
			}
			return Result;
		}
		if (Action == "save")
		{
			return Operation(Form->SaveMode());
		}
		if (Action == "reload")
		{
			return Form->Modified ? Error("UNSAVED_CHANGES", "Save or undo the macro changes first") : Operation(Form->LoadFile());
		}
		if (Action == "undo" || Action == "redo")
		{
			return Operation(Form->UndoMode(Action == "redo"));
		}
		if (Action == "link" || Action == "unlink")
		{
			u32 From = 0;
			u32 To = 0;
			if (!Integer(Params, "from", From) || !Integer(Params, "to", To) || From == To || To == Form->StartId)
			{
				return Error("INVALID_LINK", "Use different node IDs and do not target Start");
			}
			auto* Node = Form->Find(static_cast<int>(From));
			if (Node == nullptr || Form->Find(static_cast<int>(To)) == nullptr)
			{
				return Error("UNKNOWN_NODE", "Both macro nodes must exist");
			}
			const bool Linked = std::find(Node->Next.begin(), Node->Next.end(), static_cast<int>(To)) != Node->Next.end();
			if (Linked != (Action == "link"))
			{
				if (Action == "link")
				{
					xr_set<int> Visited;
					xr_vector<int> Pending = {static_cast<int>(To)};
					while (!Pending.empty())
					{
						const int Id = Pending.back();
						Pending.pop_back();
						if (Id == static_cast<int>(From))
						{
							return Error("MACRO_CYCLE", "The link would create a command loop");
						}
						if (const auto* Target = Form->Find(Id); Target != nullptr && Visited.insert(Id).second)
						{
							Pending.insert(Pending.end(), Target->Next.begin(), Target->Next.end());
						}
					}
				}
				Form->PushUndo();
				if (Action == "link")
				{
					Node->Next.push_back(static_cast<int>(To));
				}
				else
				{
					std::erase(Node->Next, static_cast<int>(To));
				}
				Form->MarkModified();
			}
			return {{"ok", true}, {"from", From}, {"next", Node->Next}};
		}
		if (Action == "add_node" || Action == "set_args" || Action == "remove_node")
		{
			u32 Id = 0;
			const bool Add = Action == "add_node";
			auto* Node = !Add && Integer(Params, "node_id", Id) ? Form->Find(static_cast<int>(Id)) : nullptr;
			if (!Add && (Node == nullptr || Node->Id == Form->StartId))
			{
				return Error("INVALID_NODE", "Specify an existing command node");
			}
			if (Action == "remove_node")
			{
				Form->PushUndo();
				Form->RemoveNode(static_cast<int>(Id));
				Form->MarkModified();
				return {{"ok", true}, {"removed_node", Id}};
			}
			const int CommandIndex = Add ? Form->FindCommand(String(Params, "command")) : Node->Command;
			if (CommandIndex < 0 || (size_t)CommandIndex >= Form->Commands.size())
			{
				return Error("UNKNOWN_COMMAND", "Use a command from the macro catalog");
			}
			const auto& Command = Form->Commands[CommandIndex];
			xr_string First = Add ? xr_string() : Node->First;
			xr_string Second = Add ? xr_string() : Node->Second;
			if ((Params.contains("first") && !String(Params, "first", First, 1024, true)) ||
				(Params.contains("second") && !String(Params, "second", Second, 1024, true)) ||
				(Command.Type == MacroType::eVoid && !First.empty()) ||
				(Command.Type != MacroType::eTwoArg && !Second.empty()))
			{
				return Error("INVALID_ARGUMENTS", "The arguments do not match this macro command");
			}
			if (Command.IntArg && !First.empty())
			{
				int Value = 0;
				const auto End = First.data() + First.size();
				const auto Conversion = std::from_chars(First.data(), End, Value);
				if (Conversion.ec != std::errc() || Conversion.ptr != End || Value < 0)
				{
					return Error("INVALID_ARGUMENTS", "The first argument must be a non-negative integer");
				}
			}
			ImVec2 Position = {180, 100};
			if (!Number(Params, "x", Position.x) || !Number(Params, "y", Position.y))
			{
				return Error("INVALID_POSITION", "Provide finite graph coordinates");
			}
			Form->PushUndo();
			if (Add)
			{
				Node = &Form->CreateNode(CommandIndex, Position);
			}
			Node->First = First;
			Node->Second = Second;
			Form->MarkModified();
			return {{"ok", true}, {"node_id", Node->Id}, {"command", Command.Name.c_str()}, {"first", First.c_str()}, {"second", Second.c_str()}};
		}
		if (Action != "read")
		{
			return Error("INVALID_ACTION", "Unsupported Macros action");
		}
		Json Response = {{"ok", true}, {"modified", Form->Modified}, {"commands", Json::array()}};
		for (const auto& Command : Form->Commands)
		{
			Response["commands"].push_back({{"name", Command.Name.c_str()}, {"arguments", static_cast<int>(Command.Type)}, {"integer_argument", Command.IntArg}, {"mcp_runnable", CanRun(Command)}});
		}
		const Json Document = Form->Write();
		for (const auto& Node : Document["nodes"])
		{
			Results.Add(Node);
		}
		Results.Write(Response, "nodes");
		return Response;
	}

	static Json Widget(const SLayoutWidget& Value)
	{
		return {{"widget_id", Value.Id}, {"parent_id", Value.ParentId}, {"name", Value.Name.c_str()}, {"type", LayoutWidget::GetTypeName(Value.Type)},
			{"x", Value.Pos.x}, {"y", Value.Pos.y}, {"width", Value.Size.x}, {"height", Value.Size.y}, {"text", Value.Text.c_str()},
			{"texture", Value.Texture.c_str()}, {"hidden", Value.Hidden}, {"locked", Value.Locked}};
	}

	static Json Layout(const xr_string& Action, const Json& Params, Page& Results)
	{
		using namespace AiDocuments;
		auto* Form = UILayoutEditorForm::Form;
		if (Form == nullptr)
		{
			return Error("EDITOR_UNAVAILABLE", "UI Layout editor is unavailable");
		}
		auto& Doc = Form->Doc;
		const xr_string Name = String(Params, "name");
		if (Action == "open" || Action == "create" || Action == "reload")
		{
			if (Form->Modified || Form->XmlEdited)
			{
				return Error("UNSAVED_CHANGES", "Save or undo the layout changes first");
			}
			const xr_string Target = Action == "reload" ? Doc.Name : Name;
			if (!Doc.IsValidName(Target.c_str()) || Target.size() > 80)
			{
				return Error("INVALID_LAYOUT_NAME", "Use a layout identifier up to 80 characters");
			}
			if (Action == "create")
			{
				if (CUILayoutDocument::WidgetExists(Target.c_str()) ||
					FS.exist(_game_config_, ("ui\\" + Target + ".xml").c_str()) ||
					FS.exist(_game_config_, ("ui\\" + Target + "_16.xml").c_str()) ||
					FS.exist("$game_scripts$", (Target + ".script").c_str()))
				{
					return Error("LAYOUT_EXISTS", "Choose an unused layout name");
				}
				Form->NewLayout();
				Doc.Name = Target;
				Form->ScriptSource = Doc.BuildScript();
				Form->MarkModified();
				EContext.UI->RequestedTabIndex = Form->TabIndex;
			}
			else if (!UILayoutEditorForm::OpenWidget(Target.c_str()))
			{
				return Operation(false);
			}
		}
		if (Action == "save")
		{
			return Operation(Form->SaveLayout());
		}
		if (Action == "undo" || Action == "redo")
		{
			return Operation(UILayoutEditorForm::Undo(Action == "redo"));
		}
		if (Action == "add_widget" || Action == "set_widget" || Action == "remove_widget" || Action == "reparent")
		{
			const bool Add = Action == "add_widget";
			u32 Id = 0;
			auto* Existing = !Add && Integer(Params, "widget_id", Id) ? Doc.Find(Id) : nullptr;
			if (!Add && Existing == nullptr)
			{
				return Error("UNKNOWN_WIDGET", "Specify an existing widget ID");
			}
			if (Form->XmlEdited)
			{
				return Error("PENDING_XML_EDIT", "Apply or undo the pending XML edit in the editor first");
			}
			if (Action == "remove_widget")
			{
				Form->Select(Id);
				Form->DeleteSelected();
				return {{"ok", true}, {"removed_widget", Id}};
			}
			u32 Parent = 0;
			if ((Action == "reparent" || (Add && Params.contains("parent_id"))) &&
				(!Integer(Params, "parent_id", Parent) || (Parent != 0 && Doc.Find(Parent) == nullptr)))
			{
				return Error("INVALID_PARENT", "Use an existing parent ID or 0 for the dialog");
			}
			if (Action == "reparent")
			{
				if (Id == Parent || Doc.IsAncestor(Id, Parent))
				{
					return Error("INVALID_PARENT", "A widget cannot contain itself or its ancestor");
				}
				if (Existing->ParentId != Parent)
				{
					Form->PushUndo();
					Doc.Reparent(Id, Parent);
					Form->MarkModified();
				}
				return {{"ok", true}, {"widget", Widget(*Doc.Find(Id))}};
			}
			SLayoutWidget Value;
			if (Add)
			{
				const xr_string TypeName = String(Params, "type");
				ELayoutWidget Type = ELayoutWidget::Count;
				for (u32 Index = 0; Index < (u32)ELayoutWidget::Count; ++Index)
				{
					const auto Kind = static_cast<ELayoutWidget>(Index);
					if (TypeName == LayoutWidget::GetTypeName(Kind))
					{
						Type = Kind;
						break;
					}
				}
				if (Type == ELayoutWidget::Count)
				{
					return Error("INVALID_WIDGET_TYPE", "Use a type from the layout widget catalog");
				}
				Value = LayoutWidget::MakeDefault(Type);
			}
			else
			{
				Value = *Existing;
			}
			if (Params.contains("name"))
			{
				const auto* WithName = Doc.FindByName(Name.c_str());
				if (!Doc.IsValidName(Name.c_str()) || Name.size() > 80 || (WithName != nullptr && WithName->Id != Id))
				{
					return Error("INVALID_WIDGET_NAME", "Use an unused widget identifier up to 80 characters");
				}
				Value.Name = Name;
			}
			if (!Number(Params, "x", Value.Pos.x) || !Number(Params, "y", Value.Pos.y) ||
				!Number(Params, "width", Value.Size.x, 1) || !Number(Params, "height", Value.Size.y, 1) ||
				(Params.contains("text") && (!LayoutWidget::HasText(Value.Type) || !String(Params, "text", Value.Text, 4096, true))) ||
				(Params.contains("texture") && (!LayoutWidget::HasTexture(Value.Type) || !String(Params, "texture", Value.Texture, 255, true))))
			{
				return Error("INVALID_WIDGET_PROPERTIES", "Provide valid geometry and fields supported by this widget type");
			}
			for (const char* Key : {"hidden", "locked"})
			{
				if (const auto It = Params.find(Key); It != Params.end())
				{
					if (!It->is_boolean())
					{
						return Error("INVALID_WIDGET_PROPERTIES", "hidden and locked must be boolean");
					}
					(xr_string_view(Key) == "hidden" ? Value.Hidden : Value.Locked) = It->get<bool>();
				}
			}
			Form->PushUndo();
			if (Add)
			{
				Id = Doc.Add(Value, Parent).Id;
			}
			else
			{
				*Existing = Value;
			}
			Form->Select(Id);
			Form->MarkModified();
			return {{"ok", true}, {"widget", Widget(*Doc.Find(Id))}};
		}
		if (Action != "read" && Action != "open" && Action != "create" && Action != "reload")
		{
			return Error("INVALID_ACTION", "Unsupported UI Layout action");
		}
		Json Response = {{"ok", true}, {"name", Doc.Name.c_str()}, {"modified", Form->Modified}, {"types", Json::array()}};
		for (u32 Index = 0; Index < (u32)ELayoutWidget::Count; ++Index)
		{
			Response["types"].push_back(LayoutWidget::GetTypeName(static_cast<ELayoutWidget>(Index)));
		}
		for (const auto& Item : Doc.Widgets)
		{
			Results.Add(Widget(Item));
		}
		Results.Write(Response, "widgets");
		return Response;
	}

	static Json Coder(const xr_string& Action, const Json& Params, Page& Results)
	{
		using namespace AiDocuments;
		auto* Form = UICoderForm::Form;
		if (Form == nullptr)
		{
			return Error("EDITOR_UNAVAILABLE", "Coder is unavailable");
		}
		if (Action == "open")
		{
			xr_string Path = String(Params, "path");
			std::replace(Path.begin(), Path.end(), '\\', '/');
			xr_string Full;
			if ((!Path.starts_with("scripts/") && !Path.starts_with("configs/")) ||
				!RelativeFile(Path, _game_data_, Full) || !UICoderForm::CanOpen(Full))
			{
				return Error("INVALID_PATH", "Use an existing supported Coder file in gamedata/scripts or gamedata/configs");
			}
			if (!UICoderForm::OpenFile(Full))
			{
				return Operation(false);
			}
			Form->Active = static_cast<size_t>(Form->SelectRequest);
		}
		if (Form->Active >= Form->Documents.size())
		{
			return Error("NO_DOCUMENT", "Open a Coder file first");
		}
		auto& Document = *Form->Documents[Form->Active];
		auto& View = *Document.View;
		if (Action == "reload")
		{
			return Document.Modified ? Error("UNSAVED_CHANGES", "Save or undo the code changes first") : Operation(Form->Load(Document));
		}
		if (Action == "save" || Action == "validate")
		{
			const xr_string Text = UICoderForm::ReadText(View);
			if (!Encodable(Document.Utf8, Text))
			{
				return Error("ENCODING_LOSS", "The code contains characters the original encoding cannot save");
			}
			if (Document.Kind == UICoderForm::EFileKind::Lua)
			{
				xr_string Message;
				int Line = 0;
				if (!CXVisor::Compile(Document.Name.c_str(), Text, Message, Line))
				{
					return {{"ok", false}, {"code", "LUA_COMPILE_ERROR"}, {"message", AiText::Utf8(Message.c_str()).c_str()}, {"line", Line}};
				}
			}
			else if (Action == "validate")
			{
				return Error("VALIDATION_UNAVAILABLE", "Coder syntax validation is available for Lua scripts");
			}
			return Operation(Action == "validate" || Form->SaveDocument(Document));
		}
		if (Action == "undo" || Action == "redo")
		{
			const bool Redo = Action == "redo";
			if (Redo ? !View.CanRedo() : !View.CanUndo())
			{
				return Error("NO_HISTORY", "The code document has no matching undo history");
			}
			if (Redo)
			{
				View.Redo();
			}
			else
			{
				View.Undo();
			}
			Document.Text = UICoderForm::ReadText(View);
			Document.Modified = Document.Text != Document.SavedText;
			return Operation(true);
		}
		const xr_string Current = UICoderForm::ReadText(View);
		if (Action == "replace")
		{
			xr_string Old;
			xr_string New;
			if (!String(Params, "old_text", Old, 8192) || !String(Params, "new_text", New, 8192, true) ||
				Old.find('\r') != xr_string::npos || New.find('\r') != xr_string::npos || View.IsReadOnly())
			{
				return Error("INVALID_REPLACEMENT", "Specify old and new text using LF line breaks");
			}
			const size_t At = Current.find(Old);
			if (At == xr_string::npos || Current.find(Old, At + 1) != xr_string::npos)
			{
				return Error("NON_UNIQUE_MATCH", "The existing text must match exactly once");
			}
			if (Old == New)
			{
				return {{"ok", true}, {"changed", false}};
			}
			auto Coordinates = [&View, &Current](size_t Offset)
			{
				int Line = 0;
				size_t Begin = 0;
				for (size_t Index = 0; Index < Offset; ++Index)
				{
					if (Current[Index] == '\n')
					{
						++Line;
						Begin = Index + 1;
					}
				}
				return TextEditor::Coordinates(Line, View.GetLineCharacterColumn(Line, static_cast<int>(Offset - Begin)));
			};
			const auto Begin = Coordinates(At);
			const auto End = Coordinates(At + Old.size());
			View.SetCursorPosition(End);
			View.SetSelection(Begin, End);
			View.ReplaceSelection(New.c_str());
			Document.Text = UICoderForm::ReadText(View);
			Document.Modified = Document.Text != Document.SavedText;
			return {{"ok", true}, {"changed", true}, {"line", Begin.mLine + 1}};
		}
		if (Action != "read" && Action != "open")
		{
			return Error("INVALID_ACTION", "Unsupported Coder action");
		}
		Json Response = {{"ok", true}, {"file", AiText::Utf8(Document.Name.c_str()).c_str()}, {"modified", Document.Modified}};
		size_t Begin = 0;
		while (Begin <= Current.size())
		{
			size_t End = Current.find('\n', Begin);
			if (End == xr_string::npos)
			{
				End = Current.size();
			}
			Results.Add({{"line", Results.Count + 1}, {"text", xr_string(Current.substr(Begin, End - Begin)).c_str()}});
			Begin = End + 1;
		}
		Results.Write(Response, "lines");
		return Response;
	}
};

namespace AiDocuments
{
inline Json Dispatch(const xr_string& Method, const Json& Params)
{
	const xr_string Action = String(Params, "action");
	Page Results(Params);
	if (Action.empty() || !Results.Valid)
	{
		return Error("INVALID_PARAMS", "Specify an action and valid pagination");
	}
	if (Method == "scene.logic_document")
	{
		return AiDocumentAccess::Logic(Action, Params, Results);
	}
	if (Method == "scene.dialog_document")
	{
		return AiDocumentAccess::Dialog(Action, Params, Results);
	}
	if (Method == "scene.macro_document")
	{
		return AiDocumentAccess::Macro(Action, Params, Results);
	}
	if (Method == "scene.layout_document")
	{
		return AiDocumentAccess::Layout(Action, Params, Results);
	}
	if (Method == "scene.code_document")
	{
		return AiDocumentAccess::Coder(Action, Params, Results);
	}
	return Error("UNKNOWN_METHOD", "This is not an SDK document command");
}
}
