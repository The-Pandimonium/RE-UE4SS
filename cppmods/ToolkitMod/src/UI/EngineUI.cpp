#include <UI.hpp>

#include <imgui.h>

#include <EngineCompat/GameThread.hpp>
#include <Session.hpp>
#include <Teleport/WaypointRenderer.hpp>

namespace RC::Toolkit::UI
{
    auto render_engine_tab() -> void
    {
        const ScrollRegion scroll("##toolkit_scroll_engine");
        auto& session = Session::get();
        const auto [profile, adapter] = session.engine_info();

        ImGui::Text("Engine");
        ImGui::SameLine(120);
        ImGui::TextUnformatted(profile.empty() ? "detecting..." : profile.c_str());
        ImGui::Text("Adapter");
        ImGui::SameLine(120);
        ImGui::TextWrapped("%s", adapter.empty() ? "-" : adapter.c_str());
        ImGui::Text("Game thread");
        ImGui::SameLine(120);
        ImGui::Text("%s  (%llu engine ticks)", Compat::GameThread::IsRunning() ? "running" : "waiting for first tick", Compat::GameThread::TickCount());

        const SessionSnapshot snap = session.snapshot();
        ImGui::Text("World");
        ImGui::SameLine(120);
        ImGui::Text("%s", snap.has_world ? snap.level.c_str() : "none");
        ImGui::Text("Pawn");
        ImGui::SameLine(120);
        ImGui::Text("%s", snap.has_pawn ? (snap.pawn_name + " (" + snap.pawn_class + ")").c_str() : "none");

        ImGui::Separator();
        ToolkitConfig config = session.config();
        bool changed = false;
        changed |= ImGui::Checkbox("Hotkeys: F6 = quick waypoint, F7 = back", &config.hotkeys);
        if (changed) session.update_config(config);

        ImGui::Separator();
        ImGui::TextWrapped("The toolkit talks to the engine through an adapter chosen from the detected version, so the same menus run on any Unreal Engine "
                           "version the adapter registry supports. Mods can register their own adapters with EngineCompat::AdapterRegistry::Register.");
        ImGui::TextDisabled("Data folder: %s", session.root().string().c_str());
        const auto& renderer = WaypointRenderer::get();
        ImGui::TextDisabled("HUD frames seen: %llu", renderer.hud_frames());
    }
} // namespace RC::Toolkit::UI
