#pragma once

#include <string>
#include <vector>

#include <Unreal/UObject.hpp>

namespace RC::Toolkit
{
    // Turns a target expression ("player_pawn.CharacterMovement", "class:BP_Door_C", ...) into live objects.
    // Must run on the game thread. Returns an empty list when nothing matches.
    auto resolve_targets(const std::string& expression) -> std::vector<Unreal::UObject*>;

    // Convenience: first match or nullptr.
    auto resolve_target(const std::string& expression) -> Unreal::UObject*;

    // The built-in root names, for UI pickers.
    auto target_roots() -> const std::vector<std::string>&;
} // namespace RC::Toolkit
