#pragma once

namespace RC::Toolkit::SelfTest
{
    // Runs the in-game self test when config.json has "selftest": true.
    //
    // The test drives the real features against the live game (teleport, waypoints, in-game markers, every
    // trainer action kind, level enumeration, the level switcher, the blueprint stub exporter) and writes
    // PASS/FAIL lines to <Mods>/ToolkitMod/selftest.log. It is how the toolkit is verified on a new game or a new
    // engine version: enable it, start the game, read the log.
    auto start_if_configured() -> void;
} // namespace RC::Toolkit::SelfTest
