#pragma once

#include <nlohmann/json.hpp>

class CCustomObject;

class AiSceneExecutor final
{
public:
	AiSceneExecutor();
	static bool IsMutation(const xr_string& Method);
	~AiSceneExecutor() = default;
	nlohmann::json Execute(const xr_string& Method, const nlohmann::json& Params);
	nlohmann::json CancelOperation(const xr_string& Method, const nlohmann::json& Params);
	void Invalidate();

private:
	using Json = nlohmann::json;
	struct Operation
	{
		xr_string Request;
		Json Result;
	};

	Json DispatchWorld(const xr_string& Method, const Json& Params);
	Json DispatchLevelEntity(const xr_string& Method, const Json& Params);
	Json DispatchLevelOptions(const xr_string& Method, const Json& Params);
	Json DispatchPlugins(const xr_string& Method, const Json& Params);
	Json Dispatch(const xr_string& Method, const Json& Params);
	Json Error(const char* Code, const char* Message) const;
	Json Describe(CCustomObject* Object);
	Json Status();
	void Synchronize();
	xr_vector<CCustomObject*> Objects() const;
	bool Resolve(const Json& Params, xr_vector<CCustomObject*>& Result, Json& Failure, bool IsEditable);
	CCustomObject* Find(const xr_string& Id);
	xr_string Identify(CCustomObject* Object);
	bool FinishChange(const class AiSceneSnapshot& Before, Json& Result);

	xr_string Session;
	u64 Generation = 0;
	u64 KnownEpoch = 0;
	u64 NextObjectId = 0;
	u32 KnownRevision = 0;
	xr_hash_map<xr_string, CCustomObject*> ById;
	xr_hash_map<CCustomObject*, xr_string> ByObject;
	xr_hash_map<xr_string, Operation> Operations;
};
