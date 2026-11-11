#include "stdafx.h"
#include "UIAiAssistant.h"

#include <chrono>
#include <algorithm>
#include <cfloat>
#include <cmath>

static u64 AiNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static xr_string AiString(const nlohmann::json& Value, const char* Key, const char* Fallback = "")
{
	auto Iter = Value.find(Key);
	if (Iter == Value.end() || !Iter->is_string())
	{
		return Fallback;
	}
	return Iter->get_ref<const nlohmann::json::string_t&>().c_str();
}

static bool AiChangesEditor(const xr_string& Method)
{
	return Method == "scene.select" || Method == "scene.focus" || Method == "scene.transform" || Method == "scene.duplicate" || Method == "scene.place" || Method == "scene.undo" || Method == "scene.redo" || Method == "scene.material_set" || Method == "scene.save_copy";
}

static void AiRemovePrivateData(nlohmann::json& Data)
{
	if (Data.is_object())
	{
		for (auto Iter = Data.begin(); Iter != Data.end(); ++Iter)
		{
			const auto& Key = Iter.key();
			if (Key == "data" || Key == "access_token" || Key == "refresh_token" || Key == "id_token" || Key == "authorization" || Key == "authUrl")
			{
				Iter.value() = "[omitted]";
			}
			else
			{
				AiRemovePrivateData(Iter.value());
			}
		}
	}
	else if (Data.is_array())
	{
		for (auto& Item : Data)
		{
			AiRemovePrivateData(Item);
		}
	}
}

static xr_string AiDisplayData(const nlohmann::json& Data)
{
	nlohmann::json Copy = Data;
	AiRemovePrivateData(Copy);
	xr_string Text = Copy.dump(2, ' ', false, nlohmann::json::error_handler_t::replace).c_str();
	if (Text.size() > 32768)
	{
		Text.resize(32768);
		Text += "\n[Результат сокращён в панели]";
	}
	return Text;
}

static u32 AiCrc32(const u8* Bytes, size_t Count)
{
	u32 Crc = 0xffffffff;
	for (size_t Index = 0; Index < Count; ++Index)
	{
		Crc ^= Bytes[Index];
		for (u32 Bit = 0; Bit < 8; ++Bit)
		{
			Crc = (Crc >> 1) ^ (0xedb88320 & (0 - (Crc & 1)));
		}
	}
	return Crc ^ 0xffffffff;
}

static void AiAppendU32(xr_vector<u8>& Bytes, u32 Value)
{
	Bytes.push_back(u8(Value >> 24));
	Bytes.push_back(u8(Value >> 16));
	Bytes.push_back(u8(Value >> 8));
	Bytes.push_back(u8(Value));
}

static void AiPngChunk(xr_vector<u8>& Png, const char* Name, const xr_vector<u8>& Data)
{
	AiAppendU32(Png, u32(Data.size()));
	const size_t Offset = Png.size();
	Png.insert(Png.end(), Name, Name + 4);
	Png.insert(Png.end(), Data.begin(), Data.end());
	AiAppendU32(Png, AiCrc32(Png.data() + Offset, Data.size() + 4));
}

static xr_vector<u8> AiEncodePng(const U32Vec& Pixels, u32 Width, u32 Height)
{
	xr_vector<u8> Raw;
	Raw.reserve((Width * 4 + 1) * Height);
	for (u32 Row = 0; Row < Height; ++Row)
	{
		Raw.push_back(0);
		for (u32 Column = 0; Column < Width; ++Column)
		{
			const u32 Pixel = Pixels[Row * Width + Column];
			Raw.push_back(u8(Pixel >> 16));
			Raw.push_back(u8(Pixel >> 8));
			Raw.push_back(u8(Pixel));
			Raw.push_back(255);
		}
	}

	// A standard zlib stream with stored DEFLATE blocks needs no runtime encoder DLL.
	xr_vector<u8> Zlib = {0x78, 0x01};
	Zlib.reserve(Raw.size() + Raw.size() / 65535 * 5 + 16);
	for (size_t Offset = 0; Offset < Raw.size();)
	{
		const u16 Count = u16(std::min<size_t>(65535, Raw.size() - Offset));
		const u16 Inverse = u16(~Count);
		Zlib.push_back(Offset + Count == Raw.size() ? 1 : 0);
		Zlib.push_back(u8(Count));
		Zlib.push_back(u8(Count >> 8));
		Zlib.push_back(u8(Inverse));
		Zlib.push_back(u8(Inverse >> 8));
		Zlib.insert(Zlib.end(), Raw.begin() + Offset, Raw.begin() + Offset + Count);
		Offset += Count;
	}
	u32 AdlerA = 1;
	u32 AdlerB = 0;
	for (u8 Byte : Raw)
	{
		AdlerA = (AdlerA + Byte) % 65521;
		AdlerB = (AdlerB + AdlerA) % 65521;
	}
	AiAppendU32(Zlib, (AdlerB << 16) | AdlerA);

	xr_vector<u8> Png = {137, 80, 78, 71, 13, 10, 26, 10};
	xr_vector<u8> Header;
	AiAppendU32(Header, Width);
	AiAppendU32(Header, Height);
	Header.insert(Header.end(), {8, 6, 0, 0, 0});
	AiPngChunk(Png, "IHDR", Header);
	AiPngChunk(Png, "IDAT", Zlib);
	AiPngChunk(Png, "IEND", {});
	return Png;
}

static xr_string AiBase64(const xr_vector<u8>& Bytes)
{
	static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	xr_string Result;
	Result.reserve(((Bytes.size() + 2) / 3) * 4);
	for (size_t Offset = 0; Offset < Bytes.size(); Offset += 3)
	{
		u32 Word = u32(Bytes[Offset]) << 16;
		if (Offset + 1 < Bytes.size())
		{
			Word |= u32(Bytes[Offset + 1]) << 8;
		}
		if (Offset + 2 < Bytes.size())
		{
			Word |= Bytes[Offset + 2];
		}
		Result.push_back(Alphabet[(Word >> 18) & 63]);
		Result.push_back(Alphabet[(Word >> 12) & 63]);
		Result.push_back(Offset + 1 < Bytes.size() ? Alphabet[(Word >> 6) & 63] : '=');
		Result.push_back(Offset + 2 < Bytes.size() ? Alphabet[Word & 63] : '=');
	}
	return Result;
}

nlohmann::json AiCaptureViewport(const nlohmann::json& Params)
{
	if (EDevice == nullptr || EContext.UI == nullptr || !EContext.UI->RT)
	{
		return {{"ok", false}, {"error", {{"code", "viewport_unavailable"}, {"message", "Viewport is not ready"}}}};
	}
	u32 MaxWidth = 960;
	if (auto Iter = Params.find("width"); Iter != Params.end())
	{
		if (!Iter->is_number() || !std::isfinite(Iter->get<double>()) || Iter->get<double>() < 64 || Iter->get<double>() > 1280)
		{
			return {{"ok", false}, {"error", {{"code", "invalid_dimensions"}, {"message", "width must be between 64 and 1280"}}}};
		}
		MaxWidth = u32(Iter->get<double>());
	}
	const u32 SourceWidth = EContext.UI->RT->dwWidth;
	const u32 SourceHeight = EContext.UI->RT->dwHeight;
	if (SourceWidth == 0 || SourceHeight == 0)
	{
		return {{"ok", false}, {"error", {{"code", "viewport_unavailable"}, {"message", "Viewport has zero size"}}}};
	}
	const double Factor = std::min({1.0, double(MaxWidth) / SourceWidth, 720.0 / SourceHeight});
	const u32 Width = std::max(1u, u32(SourceWidth * Factor));
	const u32 Height = std::max(1u, u32(SourceHeight * Factor));
	U32Vec Pixels;
	if (!EDevice->MakeScreenshot(Pixels, Width, Height) || Pixels.size() != size_t(Width) * Height)
	{
		return {{"ok", false}, {"error", {{"code", "viewport_capture_failed"}, {"message", "Renderer failed to capture the current camera"}}}};
	}
	const xr_string Encoded = AiBase64(AiEncodePng(Pixels, Width, Height));
	return {{"ok", true}, {"mime_type", "image/png"}, {"data", Encoded.c_str()}, {"width", Width}, {"height", Height}, {"captured_at_ms", AiNowMs()}, {"source", "current_editor_camera"}};
}

UIAiAssistant* UIAiAssistant::Instance = nullptr;

UIAiAssistant::UIAiAssistant() = default;

UIAiAssistant::~UIAiAssistant()
{
	Process.Stop();
}

void UIAiAssistant::Show()
{
	if (Instance == nullptr)
	{
		Instance = new UIAiAssistant();
	}
	Instance->IsOpen = true;
	if (!Instance->HasStarted)
	{
		Instance->Start();
	}
}

void UIAiAssistant::Tick()
{
	if (Instance != nullptr)
	{
		Instance->Update();
	}
}

void UIAiAssistant::Draw()
{
	if (Instance != nullptr && Instance->IsOpen)
	{
		Instance->Render();
	}
}

void UIAiAssistant::Shutdown()
{
	xr_delete(Instance);
}

void UIAiAssistant::Start()
{
	xr_string Error;
	HasStarted = true;
	for (const auto& Data : EditorRequests)
	{
		Executor.CancelOperation(AiString(Data, "method"), Data.value("params", nlohmann::json::object()));
	}
	EditorRequests.clear();
	CancelledRequests.clear();
	Pending.clear();
	if (!Process.Start(Error))
	{
		ServerStatus = "Ошибка запуска";
		AddMessage("Ошибка", Error);
		return;
	}
	ServerStatus = "Запуск…";
	IsRejectingCommands = false;
	Request("connection.check");
	Request("auth.status");
	Request("models.list");
}

void UIAiAssistant::AddMessage(const char* Role, const xr_string& Text, const xr_string& ItemId)
{
	if (Text.empty())
	{
		return;
	}
	Messages.push_back({Role, Text, ItemId});
	if (Messages.size() > 1000)
	{
		Messages.erase(Messages.begin());
	}
	NeedsScroll = true;
}

void UIAiAssistant::Request(const char* Method, const nlohmann::json& Params)
{
	if (!Process.IsRunning())
	{
		AddMessage("Ошибка", "Служебный процесс недоступен. Нажмите «Переподключить». ");
		return;
	}
	const xr_string Id = "ui-" + xr_string(std::to_string(++NextId).c_str());
	Pending[Id] = Method;
	if (!Process.Send({{"type", "ui.request"}, {"id", Id.c_str()}, {"method", Method}, {"params", Params}}))
	{
		Pending.erase(Id);
		AddMessage("Ошибка", "Не удалось отправить запрос служебному процессу.");
	}
}

void UIAiAssistant::Update()
{
	nlohmann::json Data;
	// Drain control messages before executing any queued scene operation. A cancel
	// following a request in the same channel therefore wins before that frame.
	for (u32 Count = 0; Count < 256 && Process.Poll(Data); ++Count)
	{
		const xr_string Type = AiString(Data, "type");
		if (Type == "editor.request")
		{
			if (EditorRequests.size() >= 256)
			{
				const xr_string Method = AiString(Data, "method");
				const nlohmann::json Params = Data.contains("params") && Data["params"].is_object() ? Data["params"] : nlohmann::json::object();
				Process.Send({{"type", "editor.response"}, {"id", Data.value("id", nlohmann::json())}, {"result", Executor.CancelOperation(Method, Params)}});
			}
			else
			{
				EditorRequests.push_back(std::move(Data));
			}
		}
		else if (Type == "editor.cancel")
		{
			if (Data.contains("ids") && Data["ids"].is_array())
			{
				for (const auto& Id : Data["ids"])
				{
					if (Id.is_string() && Id.get_ref<const nlohmann::json::string_t&>().size() < 200 && CancelledRequests.size() < 4096)
					{
						CancelledRequests.insert(Id.get_ref<const nlohmann::json::string_t&>().c_str());
					}
				}
			}
		}
		else
		{
			Receive(Data);
		}
	}
	for (u32 Count = 0; Count < 4 && !EditorRequests.empty(); ++Count)
	{
		Data = std::move(EditorRequests.front());
		EditorRequests.pop_front();
		const xr_string Id = AiString(Data, "id");
		const xr_string Method = AiString(Data, "method");
		const nlohmann::json Params = Data.contains("params") && Data["params"].is_object() ? Data["params"] : nlohmann::json::object();
		nlohmann::json Result;
		const bool IsCancelled = CancelledRequests.erase(Id) != 0;
		if ((IsRejectingCommands && AiChangesEditor(Method)) || !Process.IsRunning() || IsCancelled)
		{
			Result = Executor.CancelOperation(Method, Params);
		}
		else
		{
			Result = Executor.Execute(Method, Params);
		}
		Process.Send({{"type", "editor.response"}, {"id", Data.value("id", nlohmann::json())}, {"result", std::move(Result)}});
	}
	if (AiNowMs() >= NextStatusTime)
	{
		SceneStatus = Executor.Execute("scene.status", nlohmann::json::object());
		NextStatusTime = AiNowMs() + 500;
		if (HasStarted && !Process.IsRunning())
		{
			ServerStatus = "Отключён";
			McpStatus = "Отключён";
			IsRunningTurn = false;
		}
	}
}

void UIAiAssistant::Receive(const nlohmann::json& Data)
{
	if (!Data.is_object())
	{
		return;
	}
	const xr_string Type = AiString(Data, "type");
	const nlohmann::json Params = Data.contains("params") && Data["params"].is_object() ? Data["params"] : nlohmann::json::object();
	if (Type == "ui.response")
	{
		const xr_string Id = AiString(Data, "id");
		auto Iter = Pending.find(Id);
		const xr_string Method = Iter == Pending.end() ? "" : Iter->second;
		if (Iter != Pending.end())
		{
			Pending.erase(Iter);
		}
		if (Data.contains("error") && !Data["error"].is_null())
		{
			if (Method == "chat.send")
			{
				IsRunningTurn = false;
			}
			AddMessage("Ошибка", AiDisplayData(Data["error"]));
			return;
		}
		const nlohmann::json Result = Data.contains("result") ? Data["result"] : nlohmann::json::object();
		if (Method == "auth.status" || Method == "auth.login" || Method == "auth.login_device")
		{
			if (Result.contains("account"))
			{
				AuthStatus = Result["account"].is_null() ? "Не выполнен вход" : "Выполнен вход через ChatGPT";
			}
			DeviceCode = AiString(Result, "userCode");
			DeviceUrl = AiString(Result, "verificationUrl");
			if (!DeviceCode.empty())
			{
				AddMessage("Вход", "Откройте " + DeviceUrl + " и введите код: " + DeviceCode);
			}
		}
		else if (Method == "auth.logout")
		{
			AuthStatus = "Не выполнен вход";
			DeviceCode.clear();
			DeviceUrl.clear();
		}
		else if (Method == "models.list")
		{
			Models = Result.is_array() ? Result : (Result.is_object() ? Result.value("models", Result.value("data", nlohmann::json::array())) : nlohmann::json::array());
		}
		else if (Method == "diagnostics.collect")
		{
			DiagnosticPath = AiString(Result, "path");
			AddMessage("Диагностика", AiDisplayData(Result));
		}
		else if (Method == "connection.check" || Method == "connection.reconnect")
		{
			AddMessage("Подключение", AiDisplayData(Result));
		}
		return;
	}
	if (Type != "ui.event")
	{
		return;
	}
	const xr_string Event = AiString(Data, "event");
	if (Event == "agent.delta")
	{
		const xr_string Delta = AiString(Params, "delta");
		const xr_string ItemId = AiString(Params, "itemId");
		if (!Messages.empty() && Messages.back().Role == "Ассистент" && Messages.back().ItemId == ItemId)
		{
			Messages.back().Text += Delta;
			NeedsScroll = true;
		}
		else
		{
			AddMessage("Ассистент", Delta, ItemId);
		}
	}
	else if (Event == "browser.open")
	{
		xr_string Error;
		if (!AiBridgeProcess::OpenOfficialUrl(AiString(Params, "url"), Error))
		{
			AddMessage("Ошибка входа", Error);
		}
		else
		{
			AuthStatus = "Ожидание входа в браузере…";
		}
	}
	else if (Event == "chat.state")
	{
		const xr_string State = AiString(Params, "state");
		IsRunningTurn = State == "running" || State == "starting" || State == "canceling";
		if (State == "canceling" || State == "failed")
		{
			IsRejectingCommands = true;
		}
		if (State == "interrupted")
		{
			AddMessage("Система", "Запрос остановлен. Уже выполненные изменения можно отменить штатным Undo.");
		}
	}
	else if (Event == "auth.status")
	{
		AuthStatus = Params.contains("account") && !Params["account"].is_null() ? "Выполнен вход через ChatGPT" : "Не выполнен вход";
	}
	else if (Event == "chat.history")
	{
		if (Params.contains("messages") && Params["messages"].is_array())
		{
			Messages.clear();
			for (const auto& Entry : Params["messages"])
			{
				AddMessage(AiString(Entry, "role") == "user" ? "Вы" : "Ассистент", AiString(Entry, "text"));
			}
		}
	}
	else if (Event == "confirmation.dismissed")
	{
		if (AiString(Params, "id") == AiString(Confirmation, "id"))
		{
			Confirmation = nlohmann::json::object();
		}
	}
	else if (Event == "models")
	{
		Models = Params.value("models", nlohmann::json::array());
	}
	else if (Event == "status")
	{
		McpStatus = AiString(Params, "mcp", McpStatus.c_str());
		ServerStatus = AiString(Params, "appServer", ServerStatus.c_str());
	}
	else if (Event == "tool.started" || Event == "tool.completed")
	{
		AddMessage(Event == "tool.started" ? "Инструмент →" : "Результат ←", AiDisplayData(Params));
	}
	else if (Event == "confirmation.requested" || Event == "approval.request")
	{
		Confirmation = Params;
	}
	else if (Event == "error")
	{
		AddMessage("Ошибка", AiDisplayData(Params));
	}
}

static bool AiButton(const char* Label)
{
	const float Width = ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2;
	if (Width > ImGui::GetContentRegionAvail().x && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x + 1)
	{
		ImGui::NewLine();
	}
	const bool IsPressed = ImGui::Button(Label);
	ImGui::SameLine();
	return IsPressed;
}

void UIAiAssistant::DrawButtons()
{
	if (AiButton("Войти через ChatGPT"))
	{
		Request("auth.login");
	}
	if (AiButton("Вход по коду"))
	{
		Request("auth.login_device");
	}
	if (AiButton("Выйти"))
	{
		IsRejectingCommands = true;
		Request("auth.logout");
	}
	if (AiButton("Проверить подключение"))
	{
		Request("connection.check");
		Request("auth.status");
		Request("models.list");
	}
	if (AiButton("Переподключить"))
	{
		if (Process.IsRunning())
		{
			Request("connection.reconnect");
		}
		else
		{
			Process.Stop();
			Start();
		}
	}
	if (AiButton("Собрать диагностику"))
	{
		Request("diagnostics.collect");
	}
	ImGui::NewLine();
}

void UIAiAssistant::DrawModels()
{
	if (!Models.is_array())
	{
		return;
	}
	if (SelectedModel.empty())
	{
		for (const auto& Model : Models)
		{
			if (SelectedModel.empty() || (Model.contains("isDefault") && Model["isDefault"] == true))
			{
				SelectedModel = AiString(Model, "model", AiString(Model, "id").c_str());
			}
		}
	}
	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##ai_model", SelectedModel.empty() ? "Модели ещё не загружены" : SelectedModel.c_str()))
	{
		for (const auto& Model : Models)
		{
			const xr_string Id = AiString(Model, "model", AiString(Model, "id").c_str());
			const xr_string Label = AiString(Model, "displayName", Id.c_str());
			if (ImGui::Selectable(Label.c_str(), Id == SelectedModel))
			{
				SelectedModel = Id;
			}
		}
		ImGui::EndCombo();
	}
}

void UIAiAssistant::Render()
{
	ImGui::SetNextWindowSize(ImVec2(600, 680), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(340, 320), ImVec2(FLT_MAX, FLT_MAX));
	if (!ImGui::Begin("ИИ-помощник###IXRayAiAssistant", &IsOpen))
	{
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("ChatGPT: %s  |  MCP: %s  |  Codex: %s", AuthStatus.c_str(), McpStatus.c_str(), ServerStatus.c_str());
	const xr_string SceneName = AiString(SceneStatus, "scene_name", "Нет сцены");
	const size_t SelectedCount = SceneStatus.contains("selected_count") && SceneStatus["selected_count"].is_number_integer() && SceneStatus["selected_count"].get<s64>() >= 0 ? SceneStatus["selected_count"].get<size_t>() : 0;
	ImGui::TextWrapped("Сцена: %s  |  Выделено: %zu", SceneName.c_str(), SelectedCount);
	DrawButtons();
	DrawModels();
	if (!DeviceCode.empty())
	{
		ImGui::TextWrapped("Код входа: %s\nАдрес: %s", DeviceCode.c_str(), DeviceUrl.c_str());
		if (ImGui::SmallButton("Копировать код"))
		{
			ImGui::SetClipboardText(DeviceCode.c_str());
		}
	}
	ImGui::Separator();
	const float FooterHeight = ImGui::GetTextLineHeightWithSpacing() * 7 + ImGui::GetStyle().ItemSpacing.y * 4;
	const float HistoryHeight = std::max(ImGui::GetTextLineHeightWithSpacing() * 3, ImGui::GetContentRegionAvail().y - FooterHeight);
	if (ImGui::BeginChild("##ai_history", ImVec2(0, HistoryHeight), true))
	{
		for (size_t Index = 0; Index < Messages.size(); ++Index)
		{
			const Message& Entry = Messages[Index];
			ImGui::PushID(int(Index));
			ImGui::TextUnformatted(Entry.Role.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Копировать"))
			{
				ImGui::SetClipboardText(Entry.Text.c_str());
			}
			ImGui::PushTextWrapPos(0);
			ImGui::TextUnformatted(Entry.Text.c_str());
			ImGui::PopTextWrapPos();
			ImGui::Separator();
			ImGui::PopID();
		}
		if (NeedsScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 100)
		{
			ImGui::SetScrollHereY(1);
		}
		NeedsScroll = false;
	}
	ImGui::EndChild();
	ImGui::InputTextMultiline("##ai_message", Input, sizeof(Input), ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 3), ImGuiInputTextFlags_None);
	ImGui::BeginDisabled(IsRunningTurn || Input[0] == 0 || !Process.IsRunning());
	if (AiButton("Отправить"))
	{
		AddMessage("Вы", Input);
		IsRejectingCommands = false;
		Request("chat.send", {{"text", Input}, {"model", SelectedModel.empty() ? nlohmann::json() : nlohmann::json(SelectedModel.c_str())}});
		IsRunningTurn = true;
		Input[0] = 0;
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!IsRunningTurn);
	if (AiButton("Остановить"))
	{
		IsRejectingCommands = true;
		Request("chat.cancel");
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(IsRunningTurn);
	if (AiButton("Новый диалог"))
	{
		Request("chat.new");
		Messages.clear();
	}
	ImGui::EndDisabled();
	ImGui::NewLine();
	if (!DiagnosticPath.empty() && ImGui::SmallButton("Копировать путь диагностики"))
	{
		ImGui::SetClipboardText(DiagnosticPath.c_str());
	}
	if (!Confirmation.empty())
	{
		ImGui::OpenPopup("Подтвердить изменение###AiConfirm");
	}
	if (ImGui::BeginPopupModal("Подтвердить изменение###AiConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (Confirmation.empty())
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::TextWrapped("%s", AiString(Confirmation, "title", "Подтвердите пакет изменений").c_str());
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45);
		const xr_string Details = AiString(Confirmation, "details");
		ImGui::TextUnformatted(Details.empty() ? AiDisplayData(Confirmation).c_str() : Details.c_str());
		ImGui::PopTextWrapPos();
		if (ImGui::Button("Подтвердить"))
		{
			Request("approval.respond", {{"id", Confirmation.value("id", nlohmann::json())}, {"approved", true}});
			Confirmation = nlohmann::json::object();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Отмена"))
		{
			Request("approval.respond", {{"id", Confirmation.value("id", nlohmann::json())}, {"approved", false}});
			Confirmation = nlohmann::json::object();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::End();
}
