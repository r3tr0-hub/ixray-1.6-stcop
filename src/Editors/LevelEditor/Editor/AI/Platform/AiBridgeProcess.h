#pragma once

#include <nlohmann/json.hpp>

class AiBridgeProcessImpl;

class AiBridgeProcess final
{
public:
	AiBridgeProcess();
	~AiBridgeProcess();
	AiBridgeProcess(const AiBridgeProcess&) = delete;
	AiBridgeProcess& operator=(const AiBridgeProcess&) = delete;

	bool Start(xr_string& Error);
	void Stop();
	bool Send(const nlohmann::json& Message);
	bool Poll(nlohmann::json& Message);
	bool IsRunning() const;
	static bool OpenOfficialUrl(const xr_string& Url, xr_string& Error);

private:
	AiBridgeProcessImpl* Impl = nullptr;
};
