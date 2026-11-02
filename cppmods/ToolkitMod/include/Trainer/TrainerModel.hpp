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
// Placeholders (in `value`, `off_value`, every entry of `params`, and in a slider's `min_expr`/`max_expr`/`default_expr`):
//   {value}                       the slider's current value ("1"/"0" for toggles)
//   {current}                     the current value of the action's own property (for a slider bound: of its first action)
//   {<target>.<Property>}         any property of the FIRST instance of a target, e.g. {class:daynightCycle_C.MaxTime} or
//                                 {player_pawn.CharacterMovement.MaxWalkSpeed}; quotes around the target are allowed
// Numeric fields also accept arithmetic on placeholders: "{class:daynightCycle_C.MaxTime} * 2", "({current} + 10) / 2".
// Unresolvable placeholders are left as written (and the numeric fallback `min`/`max`/`default_value` is used for bounds).
//
// While a slider cheat is on, moving the slider re-applies every action that uses {value} immediately.
//
// `select` / `index` narrow the matches of a class target to "just one" (first, nearest to the player, or the Nth).
// A cheat whose targets do not exist in the current level cannot be switched on.
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
        // Which of the objects matching `target` the action applies to: "all" (every instance of the class),
        // "first", "nearest" (closest to the player) or "index" (the `index`-th, 0-based).
        std::string select{"all"};
        int index{0};
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
        // Slider range. Only the drag range: any value can be typed into the box next to the slider, also outside it.
        double min{0.0};
        double max{100.0};
        double default_value{1.0};
        bool log{false}; // logarithmic slider: fine control near min, still reaches max (for ranges like 1 to 10000)
        // Slider bounds/default as expressions (see placeholders above). When non-empty they replace min/max/default_value.
        std::string min_expr{};
        std::string max_expr{};
        std::string default_expr{};
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
