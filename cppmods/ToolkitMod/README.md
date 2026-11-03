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

### Placeholders and live sliders

Values (`value`, `off_value`, `params`) and slider bounds can use placeholders and simple arithmetic:

| Placeholder | Meaning |
| --- | --- |
| `{value}` | the slider's current value (`1`/`0` for toggles) |
| `{current}` | the current value of the action's own property (for a slider bound: of the cheat's first action) |
| `{class:daynightCycle_C.MaxTime}` | any property of the **first** instance of a target (`"class:..."` in quotes also works) |

Slider bounds are `min`/`max`/`default_value` or, to compute them from the game, `min_expr`/`max_expr`/`default_expr`, e.g.
`"max_expr": "{class:daynightCycle_C.MaxTime}", "default_expr": "{current}"`. Arithmetic (`+ - * / ( )`) works anywhere a
placeholder does. The Trainer tab's **Refresh values** button re-reads everything so sliders show up-to-date bounds,
defaults and a "now" readout. Moving a slider while its cheat is already on updates the game immediately.

### Function parameters

`call_function` parameters are not invented by hand. Like the blueprint stub generator, the creator reads the function's own
signature from the live object (every `CPF_Parm` property: inputs become editable rows, out/return values are listed under
"returns"), so you only fill in values for names that really exist. Each value is converted to the parameter's declared type
when the function is called: `bool` (`true`/`false`/`1`/`0`), integers and floats (arithmetic such as `{value} * 2`), enum
entries by name (`DORM_Awake`) or number, strings/names, object paths, and struct text. Leaving a field empty keeps the engine
default. A value that cannot be read as the declared type is reported on the cheat instead of silently becoming zero. Entries
in an older file that no longer match the function are flagged and can be removed.

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
* **Opening most levels directly freezes the game.** Not a deadlock: the game thread is running one very long operation that
  spawns `prop_C` ("cube") actors without end (10,000 per 5 s at first, slowing down as physics gets crowded; 30,000+ after a
  minute, never finishing in 10+ minutes; sound stops because the game thread never returns). Measured by counting
  `BeginPlay` events after `open`: `untitled_1` (story) 1,398 props and settles, `tutorial010` 50 and settles, while
  `untitled_0`, `sandbox/flatgrass`, `NewFolder/tutorial`, `NewFolder/tutorial2` keep growing. It happens with UE4SS's hooks and
  all C++ mods disabled, so it is the game. It happens through the game's own tutorial launcher too (`openTutor("tutorial")`).
  Ruled out: the toolkit, UE4SS hooks, the paused state, `GameInstance.CurrentLevel`, and the prop renderer actor
  (`propProcessor_C` start/tick blocked: no effect). Not ruled out: the game mode (`mainGamemode_C`) running `loadObjects` for a
  level that was never started through the save-slot launcher; blocking `loadObjects` only delayed the freeze by ~15 s.
  Working entry points: the game's launcher (New game -> Launch), `untitled_1`, `tutorial010`, `tutorial3`.
* **Do not travel away from the boot screen (`preLoad`)** before pressing Proceed: same freeze.
* The Levels tab reads `Mods/ToolkitMod/level_notes.json` (advisory text, per-level `[!]` warnings with a confirmation before
  opening, unsafe origin levels, and optional `guards`). A guard names a class and an event that is skipped for objects of
  that class (`ProcessEvent`, and Blueprint-internal calls); with no guards configured no hooks are installed.
* Debug aid: environment variable `TOOLKIT_DEBUG_OPEN=<package path>` opens that level 12 s after the main menu appears,
  through the same code path as the Levels tab.
