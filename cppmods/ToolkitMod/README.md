# ToolkitMod

A UE4SS C++ mod with four ImGui tabs in the UE4SS window:

| Tab | What it does |
| --- | --- |
| **Trainer** | Runs trainer files (`Mods/ToolkitMod/trainers/*.json`) and contains the **trainer creator**: pick a target, browse its live properties/functions, test the cheat, save it as JSON. |
| **Teleport** | Waypoints per level (or global), teleport by waypoint or coordinates, "Back", F6 quick waypoint, F7 back. Waypoints with **Visible** ticked are drawn in the game view. |
| **Levels** | Lists every world in the asset registry, filter, open, reload, travel options, recent levels. |
| **Toolkit Engine** | Detected engine version, active adapter, game-thread status, HUD diagnostics. |

## Version-agnostic design

Everything engine-specific goes through `UE4SS/include/EngineCompat/` (part of the UE4SS core, shared by all mods):

* `EngineProfile` – detects the engine (UE4SS' own detection plus `KismetSystemLibrary::GetEngineVersion`) and exposes
  feature flags (`uses_ffield`, `large_world_coordinates`, ...). Nothing compares version numbers directly.
* `Reflect` – reads/writes values **by property name** through reflection (float vs double vectors, renamed parameters),
  `Reflect::Call` calls any UFunction with named parameters. It ships its own typed value import/export because
  `FProperty::ExportText` is not resolvable on every engine (it is missing on 4.27).
* `IEngineAdapter` / `AdapterRegistry` – "player pawn", "teleport", "open level", "enumerate levels", "project to screen".
  `GenericAdapter` is pure reflection; `Ue4Adapter` / `Ue5Adapter` are the per-family hooks; mods can
  `AdapterRegistry::Register` their own for forks.
* `BytecodeProfile` – which Blueprint opcodes embed property pointers (used by the PropertyAnalyzer).
* `GameThread` – runs work on the Unreal game thread (UI/mod threads never call game code directly).

Status: developed and tested against **UE 4.27.2** (Voices of the Void). The UE5 adapter exists but has never run in a game.

## Trainer files

See `include/Trainer/TrainerModel.hpp` for the full schema. A cheat is a toggle, button or slider made of actions:
`set_property`, `freeze_property` (`set` / `at_least` / `at_most`), `scale_property`, `call_function`,
`block_function`, `console`, `time_scale`. Targets: `player_pawn`, `player_controller`, `game_mode`, `game_state`,
`world`, `world_settings`, `hud`, `class:<Name>`, `object:<path>`, with `.Property` chains
(`player_pawn.CharacterMovement`). Targets are **classes**, not objects (objects are recreated on every level load); each action
has `select`: `all` (every instance), `first`, `nearest` (to the player) or `index` (the Nth). The creator's **Pick class...**
lists the classes that currently have live objects. A cheat whose targets do not exist in the current level is greyed out and
cannot be switched on or pressed. `assets/trainers/EngineEssentials.json` is a working example built only on stock
engine classes.

## In-game waypoint markers

Shipping builds strip `DrawDebug*`, so markers are painted through the game's HUD (`ReceiveDrawHUD` pre-hook +
`HUD::Project/DrawRect/DrawText`). If a game has no HUD, one is created with `PlayerController::ClientSetHUD`.

## Self test

Set `"selftest": true` in `Mods/ToolkitMod/config.json` and start the game; results go to
`Mods/ToolkitMod/selftest.log` (PASS / FAIL / SKIP per feature). `"selftest_level"`: a level to travel to first,
or `"wait"` to wait until something else loads a level with a real player pawn.
Environment variable `TOOLKIT_SKIPENUM=1` skips the asset-registry level enumeration step (debugging aid).

## Known issues (Voices of the Void)

* Calling `GetAllAssets` on the asset registry's class default object crashes the game (null read inside the engine).
  Fixed: the live registry is obtained from `AssetRegistryHelpers::GetAssetRegistry()`.
* Travelling to large levels (e.g. `sb_area0`) directly from `preLoad` never finishes (game thread spins inside PhysX),
  with or without this mod. Small maps (`sandbox/flatgrass`, `untitled_0`) load when travel is started from Lua.
  Travel started by the toolkit itself (level switcher, self test) is **not yet reliable** in this game - under
  investigation.
