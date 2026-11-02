# Voices of the Void 0.10.0: why most levels freeze when opened directly

Method: from the main menu, `open <map>` (also tried through the game's own launcher), then a Lua mod logged the world at
`LoadMap` post (before any `BeginPlay`): world settings, game mode/state classes, level streaming, key `GameInstance`
values, every actor split into **placed** (no runtime name suffix) and **runtime** (`_2147xxxxxx`), and a per-class
`BeginPlay` count for the first ~20 s. Raw dumps: `votv-inventory-*.txt`.

## What freezes and what does not

| Map | placed actors / classes | runtime actors at load | `prop_C` spawned | result |
| --- | --- | --- | --- | --- |
| `untitled_1` (story) | 7,786 / 594 | 1,532 (59 classes) | 1,398, then stops | loads |
| `tutorial010` | - | - | 50, then stops | loads |
| `untitled_0` | 214 / 40 | 48 | 22,000 in 20 s, 32,000+ later | never finishes |
| `sandbox/flatgrass` | 226 / 44 | 48 | 30,000+ | never finishes |
| `NewFolder/tutorial2`, `NewFolder/tutorial` | similar small maps | | 30,000+ | never finishes |

The game thread is not deadlocked: it runs one long operation that keeps spawning `prop_C` actors (`Name = cube`),
10,000 per 5 s at first, slowing as physics gets crowded. Sound stops because the game thread never returns.

## What is different between `untitled_1` and the freezing maps

* The freezing maps are small, mostly empty test maps: ~220 placed actors in ~40 classes, **every one of which is also placed in
  `untitled_1`** (so no unusual actor class is responsible). `untitled_1` also places 527 `prop_C`, 76 `prop_drive_C`, 8
  `NewBlueprint_C`; the small maps place 0 `prop_C`, 35 `prop_drive_C`, 1 `NewBlueprint_C`.
* `WorldSettings.DefaultGameMode`: `mainGamemode_C` in `untitled_1`, **nil** in the small maps (the game mode is still
  `mainGamemode_C` at runtime, from the project default). `KillZ` -15,000,000 vs -1,048,575.
* At load `untitled_1` already has 1,532 runtime actors (838 `actorChipPile_C`, 257 `prop_swinger_C`, 257 `swingdoor_C`,
  AI controllers, ...): the game mode populated the level. The small maps get only the 48 engine/ticker actors.
* The prop renderer actor is placed in both (`propRenderer_2` in `untitled_1`, the older name `propProcessor_2` in the small
  maps) with identical state at `BeginPlay` (`propsNames_len = 0`, `objectsNames_len = 0`, `fins = 0`).

## What it is NOT (each tested)

* Not the toolkit, UE4SS hooks, or any C++ mod (reproduces with all hooks off and only a Lua mod).
* Not the save slot: `loadedSlotName`/`folderSlotName` are empty in `untitled_1` too, and `GameInstance` values are identical
  before opening every map. Setting `CurrentLevel` first does not help.
* Not the pause state, and not travelling from the menu vs. from `preLoad` alone (opening from `preLoad` freezes everything).
* Not the prop renderer start/tick (blocked: no effect), not `mainGamemode_C::loadObjects` alone (blocked: freeze delayed ~15 s),
  and forcing the game mode with `?game=/Game/main/mainGamemode.mainGamemode_C` does not change anything.

## Open

What spawns the `cube` props is still unknown. The game never calls `spawnPropThroughGamemode`/`spawnObjectFromSaveData`
(counted), and `intComs_propRenderer_finishProps` never fires. Next experiments: hook `prop_C::UserConstructionScript` and
walk the Blueprint call stack of the first spawn; or compare `mainGamemode_C` variable values right after `BeginPlay` in
`untitled_1` and `untitled_0`.
