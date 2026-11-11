#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace AiProjectTools
{
using Json = nlohmann::json;

std::optional<Json> Dispatch(const xr_string& Method, const Json& Params);
Json SelfTestMetadata(const std::filesystem::path& TestOutput);
}
