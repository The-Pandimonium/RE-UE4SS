#pragma once

#include <string>
#include <vector>

#include <Unreal/UObject.hpp>

namespace RC::Toolkit
{
    // Turns a target expression ("player_pawn.CharacterMovement", "class:BP_Door_C", ...) into live objects.
    // Must run on the game thread. Returns an empty list when nothing matches.
    // `select`: "all", "first", "nearest" (to the player pawn) or "index" (`index`-th match).
    auto resolve_targets(const std::string& expression, const std::string& select = "all", int index = 0) -> std::vector<Unreal::UObject*>;

    // Narrows an already resolved list.
    auto select_targets(std::vector<Unreal::UObject*> objects, const std::string& select, int index) -> std::vector<Unreal::UObject*>;

    // Convenience: first match or nullptr.
    auto resolve_target(const std::string& expression) -> Unreal::UObject*;

    // The built-in root names, for UI pickers.
    auto target_roots() -> const std::vector<std::string>&;
} // namespace RC::Toolkit
