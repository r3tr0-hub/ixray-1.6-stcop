#pragma once

#include <nlohmann/json.hpp>
#include "Editor/AI/SceneExecutor.h"
#include "Editor/AI/Platform/AiBridgeProcess.h"

nlohmann::json AiCaptureViewport(const nlohmann::json& Params);

class UIAiAssistant final
{
public:
	UIAiAssistant();
	~UIAiAssistant();

	static void Show();
	static void Tick();
	static void Draw();
	static void Shutdown();

private:
	struct Message
	{
		xr_string Role;
		xr_string Text;
		xr_string ItemId;
	};

	void Start();
	void Update();
	void Render();
	void Request(const char* Method, const nlohmann::json& Params = nlohmann::json::object());
	void Receive(const nlohmann::json& Data);
	void AddMessage(const char* Role, const xr_string& Text, const xr_string& ItemId = "");
	void DrawButtons();
	void DrawModels();

	static UIAiAssistant* Instance;
	AiSceneExecutor Executor;
	AiBridgeProcess Process;
	xr_vector<Message> Messages;
	xr_hash_map<xr_string, xr_string> Pending;
	nlohmann::json Models = nlohmann::json::array();
	nlohmann::json SceneStatus = nlohmann::json::object();
	nlohmann::json Confirmation = nlohmann::json::object();
	xr_string SelectedModel;
	xr_string AuthStatus = "Не выполнен вход";
	xr_string McpStatus = "Ожидание";
	xr_string ServerStatus = "Не запущен";
	xr_string DeviceCode;
	xr_string DeviceUrl;
	xr_string DiagnosticPath;
	char Input[16384] = {};
	u64 NextId = 0;
	u64 NextStatusTime = 0;
	bool IsOpen = true;
	bool IsRunningTurn = false;
	bool NeedsScroll = false;
	bool HasStarted = false;
};
