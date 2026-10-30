#pragma once
#include <string>

#include <Common.hpp>

namespace RC::BlueprintStubGenerator
{
    // Scans all user-made Blueprint classes, structs, and enums in the current
    // session and writes a JSON data file to outputPath.
    // The file is consumed by the ModdingAPIGenerator Unreal Editor plugin,
    // which creates the actual stub Blueprint assets.
    // Returns true on success, false if the file could not be opened.
    RC_UE4SS_API bool Generate(const std::wstring& outputPath);
}
