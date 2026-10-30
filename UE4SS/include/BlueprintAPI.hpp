#pragma once

#include <Common.hpp>

namespace RC::BlueprintAPI
{
    // Register the ProcessEvent pre-hook and all phantom function handlers.
    // Call once after Unreal is fully initialized (after fire_unreal_init_for_cpp_mods).
    RC_UE4SS_API auto Initialize() -> void;
}
