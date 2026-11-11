#pragma once

#include "ContentEditorCommands.h"

namespace AiContentEditors
{
inline Json CheckParticleWorkflow(u64 Unique)
{
	if (!PTools || !FS.path_exist("$game_particles$"))
	{
		return Error("PARTICLE_TOOL_UNAVAILABLE", "Particle Editor or its SDK path is unavailable");
	}

	const xr_string Name = xr_string("ai_selftest_particle_") + xr_string(std::to_string(Unique).c_str());
	const auto Edit = [&](const char* Operation, Json Params = Json::object())
	{
		Params["action"] = Operation;
		Params["name"] = Name.c_str();
		return EditParticles(*PTools, Params);
	};

	const Json Created = Edit("create");
	if (!Created.value("ok", false))
	{
		return Created;
	}

	const Json Edited = Edit("set", {{"action_index", 1}, {"fields", {{"Rate", 47.5}}}});
	if (!Edited.value("ok", false))
	{
		return Edited;
	}

	const Json Saved = Edit("save");
	if (!Saved.value("ok", false))
	{
		return Saved;
	}

	string_path Path;
	FS.update_path(Path, "$game_particles$", (Name + ".pe").c_str());
	struct Cleanup
	{
		const char* Path;
		~Cleanup() { FS.file_delete(Path); }
	} Remove{Path};

	const Json Actions = Saved.value("actions", Json::array());
	const bool SavedRate = Actions.is_array() && Actions.size() > 1 &&
		Actions[1].value("fields", Json::object()).value("Rate", Json::object()).value("value", 0.0) == 47.5;
	const bool UndoChange = Edit("undo").value("changed", false);
	PS::CPEDef* Effect = PTools->FindPE(Name.c_str());
	const bool DefaultRate = Effect && Effect->m_EActionList.size() == 3 && Effect->m_EActionList[1] &&
		Effect->m_EActionList[1]->type == PAPI::PASourceID && Effect->m_EActionList[1]->_float("Rate").val == 100.f;
	const bool UndoCreation = Edit("undo").value("changed", false) && PTools->FindPE(Name.c_str()) == nullptr;
	const bool RedoCreation = Edit("redo").value("changed", false) && PTools->FindPE(Name.c_str()) != nullptr;
	const bool CleanupCreation = Edit("undo").value("changed", false) && PTools->FindPE(Name.c_str()) == nullptr;

	return {{"ok", SavedRate && UndoChange && DefaultRate && UndoCreation && RedoCreation && CleanupCreation},
		{"saved_rate", SavedRate}, {"undo_rate", UndoChange && DefaultRate}, {"undo_creation", UndoCreation},
		{"redo_creation", RedoCreation}, {"cleanup_creation", CleanupCreation}};
}
} // namespace AiContentEditors
