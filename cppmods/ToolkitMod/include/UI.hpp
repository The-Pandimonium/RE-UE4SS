#pragma once

namespace RC::Toolkit::UI
{
    // ImGui tab bodies. Each is called every frame from the UE4SS debugging window (ImGui thread). They only read
    // snapshots and forward actions to the game thread; none of them touch engine objects directly.
    auto render_trainer_tab() -> void;
    auto render_teleport_tab() -> void;
    auto render_levels_tab() -> void;
    auto render_engine_tab() -> void;
} // namespace RC::Toolkit::UI
