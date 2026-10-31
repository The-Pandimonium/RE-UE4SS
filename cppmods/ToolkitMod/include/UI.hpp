#pragma once

#include <imgui.h>

namespace RC::Toolkit::UI
{
    // ImGui tab bodies. Each is called every frame from the UE4SS debugging window (ImGui thread). They only read
    // snapshots and forward actions to the game thread; none of them touch engine objects directly.
    auto render_trainer_tab() -> void;
    auto render_teleport_tab() -> void;
    auto render_levels_tab() -> void;
    auto render_engine_tab() -> void;

    // Puts a tab's content in a scrolling region (vertical and horizontal) so nothing is cut off when the window is
    // smaller than the content. Declare one at the top of a tab body; it also closes correctly on early returns.
    struct ScrollRegion
    {
        explicit ScrollRegion(const char* id) { ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar); }
        ~ScrollRegion() { ImGui::EndChild(); }
        ScrollRegion(const ScrollRegion&) = delete;
        ScrollRegion& operator=(const ScrollRegion&) = delete;
    };
} // namespace RC::Toolkit::UI
