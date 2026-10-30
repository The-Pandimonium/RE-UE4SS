#pragma once

// Trainer data model. A trainer is a JSON file of cheats; a cheat is a toggle, button or slider made of one
// or more actions. Everything is addressed by name so a trainer written for one build of a game keeps
// working across engine versions (see EngineCompat) and across game updates that keep the property names.
//
// Action kinds
//   set_property     Write `value` to `property` on the target. Toggles restore `off_value` (or the old value) when switched off.
//   freeze_property  Re-apply `value` to `property` every tick. `mode`: "set" (default), "at_least", "at_most".
//   scale_property   Hold `property` at (value it had when enabled) * factor. The factor is `scale`, or the slider value.
//   call_function    Call `function` on the target with `params`. Toggles call `off_function` / `off_params` when switched off.
//   block_function   Skip every call of `function` made on the target; optionally pretend it returned `return_value`.
//   console          Run `value` as a console command (`off_value` when a toggle is switched off).
//   time_scale       Global time dilation: `value` (or the slider value) while on, 1.0 when off.
//
// `value`, `off_value` and every entry of `params` may contain the placeholder {value}, which is replaced by the
// slider's current value (or "1"/"0" for toggles).
//
// Target syntax: <root>[.<Property>...]   roots: player_pawn, player_controller, game_mode, game_state, world,
// world_settings, hud, class:<ClassName>, object:<full object path>.
//   e.g. "player_pawn.CharacterMovement"  "class:BP_Door_C"  "object:/Game/Maps/Main.Main:PersistentLevel.Light_1"

#include <map>
#include <string>
#include <vector>

namespace RC::Toolkit
{
    struct ActionDef
    {
        std::string kind{"set_property"};
        std::string target{"player_pawn"};
        std::string property{};
        std::string value{};
        std::string off_value{};
        std::string mode{"set"};
        double scale{1.0};
        std::string function{};
        std::map<std::string, std::string> params{};
        std::string off_function{};
        std::map<std::string, std::string> off_params{};
        std::string return_value{};
    };

    struct CheatDef
    {
        std::string id{};
        std::string label{};
        std::string description{};
        std::string type{"toggle"}; // toggle | button | slider
        double min{0.0};
        double max{1.0};
        double default_value{1.0};
        std::vector<ActionDef> actions{};
    };

    struct TrainerDef
    {
        std::string name{};
        std::string description{};
        std::string author{};
        std::string game{}; // informational; matched against the executable name when present
        int version{1};
        std::vector<CheatDef> cheats{};
    };
} // namespace RC::Toolkit
