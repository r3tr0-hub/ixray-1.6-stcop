#pragma once

#include <nlohmann/json.hpp>

class CSceneObject;

namespace AiMaterials
{
nlohmann::json Read(CSceneObject& Object);
nlohmann::json ValidatePatch(CSceneObject& Object, const nlohmann::json& Change);
nlohmann::json ApplyPatch(CSceneObject& Object, const nlohmann::json& Change);
nlohmann::json MissingTextures(CSceneObject& Object, size_t Offset, size_t Limit);
bool TextureExists(const char* Texture);
} // namespace AiMaterials
