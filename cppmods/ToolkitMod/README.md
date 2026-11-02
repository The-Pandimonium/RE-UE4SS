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
lists the classes that currently have live objects. Sliders have `min`/`max` (default 0 to 100) for dragging, an optional
`"log": true` for wide ranges, and a value box that accepts any number (e.g. 400) regardless of the range. A cheat whose targets do not exist in the current level is greyed out and
cannot be switched on or pressed. `assets/trainers/EngineEssentials.json` is a working example built only on stock
engine classes.

## In-game waypoint markers

Visible waypoints are shown as **marker actors**: a beam and a ball built from `/Engine/BasicShapes`, no collision, created
through reflection and reconciled once a second. They are only spawned after the player controller has existed for
10 s, because spawning while a level loads hangs Voices of the Void. Optionally the game's HUD is used for labels, but
Voices of the Void's HUD never gets a canvas (`ReceiveDrawHUD` never fires), so actors are the reliable path.
Shipping builds strip `DrawDebug*`, which is why neither debug lines nor text are used.

## Self test


Set `"selftest": true` in `Mods/ToolkitMod/config.json` and start the game; results go to
`Mods/ToolkitMod/selftest.log` (PASS / FAIL / SKIP per feature). `"selftest_level"`: a level to travel to first,
or `"wait"` to wait until something else loads a level with a real player pawn.
Environment variable `TOOLKIT_SKIPENUM=1` skips the asset-registry level enumeration step (debugging aid).

## Performance

The toolkit has a frame-cost budget that the self test enforces (`performance: cheats enabled for 15 s`): each tick
callback must average under 150 us with spikes under 4 ms. Measured in Voices of the Void with four cheats holding
values: trainer 17 us/tick, session snapshot 14 us, markers 4 us, everything together 37 us (max 0.34 ms).
`EngineCompat::GameThread::TickStats()` reports the cost per callback. How it stays cheap: `Reflect` caches property,
function and class lookups; cheats resolve their targets twice a second, not every frame; availability checks evaluate
one target per 0.1 s; `IsValidObject` is O(1) (`UObject::IsReal` scans the whole object array, ~0.5 ms).

## Known issues (Voices of the Void)

* Calling `GetAllAssets` on the asset registry's class default object crashes the game (null read inside the engine).
  Fixed: the live registry is obtained from `AssetRegistryHelpers::GetAssetRegistry()`.
* **Do not travel away from the boot screen (`preLoad`).** Opening any level before pressing Proceed leaves the game thread
  spinning inside PhysX. Reproduced with all UE4SS hooks and C++ mods disabled, so it is the game, not the toolkit. From the
  main menu level the same maps load normally (checked: `untitled_0`, `untitled_1`, `sandbox/flatgrass`, `NewFolder/tutorial2`,
  `tutorial3`, `tutorial010`).
* **`/Game/maps/NewFolder/tutorial` hangs the game** (no heartbeat from the game thread for 10+ minutes), also when the game's
  own tutorial launcher (`ui_menu_C:openTutor("tutorial", "")`) opens it. `tutorial010` and `tutorial3` work.
* The Levels tab reads `Mods/ToolkitMod/level_notes.json`: levels listed there get a `[!]` marker and ask for confirmation
  before opening, and a banner warns when the current level is an unsafe place to travel from. Add your own entries.
* Debug aid: environment variable `TOOLKIT_DEBUG_OPEN=<package path>` opens that level 12 s after the main menu appears,
  through the same code path as the Levels tab.
