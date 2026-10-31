#include <UI.hpp>

#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <Session.hpp>
#include <Teleport/WaypointRenderer.hpp>
#include <Teleport/Waypoints.hpp>

namespace RC::Toolkit::UI
{
    namespace
    {
        struct TeleportState
        {
            std::string new_name{};
            bool new_visible{true};
            bool new_global{false};
            float new_color[3]{1.0f, 0.78f, 0.2f};
            bool show_all_levels{false};
            double manual[3]{0, 0, 0};
            bool manual_dirty{true};
            std::string filter{};
        };

        auto state() -> TeleportState&
        {
            static TeleportState s;
            return s;
        }

        auto distance(const Waypoint& w, const Vec3& p) -> double
        {
            return std::sqrt((w.x - p.x) * (w.x - p.x) + (w.y - p.y) * (w.y - p.y) + (w.z - p.z) * (w.z - p.z));
        }
    } // namespace

    auto render_teleport_tab() -> void
    {
        const ScrollRegion scroll("##toolkit_scroll_teleport");
        auto& ts = state();
        auto& session = Session::get();
        auto& store = WaypointStore::get();
        const SessionSnapshot snap = session.snapshot();
        ToolkitConfig config = session.config();

        // ---- Where am I ----
        if (!snap.has_world) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1), "No world loaded yet");
        else ImGui::Text("Level: %s", snap.level.c_str());
        if (snap.has_pawn)
        {
            ImGui::Text("Player: %s (%s)", snap.pawn_name.c_str(), snap.pawn_class.c_str());
            ImGui::Text("Position  X %.1f   Y %.1f   Z %.1f      Yaw %.1f", snap.position.x, snap.position.y, snap.position.z, snap.rotation.yaw);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy"))
                ImGui::SetClipboardText(std::format("{:.1f} {:.1f} {:.1f}", snap.position.x, snap.position.y, snap.position.z).c_str());
        }
        else
        {
            ImGui::TextDisabled("No player pawn - load into a level to teleport.");
        }
        const std::string message = session.last_message();
        if (!message.empty()) ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1), "%s", message.c_str());

        // ---- Settings ----
        ImGui::Separator();
        bool changed = ImGui::Checkbox("Show waypoint markers in game", &config.hud_markers);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        float max_distance = static_cast<float>(config.marker_max_distance / 100.0);
        if (ImGui::InputFloat("max distance (m, 0 = unlimited)", &max_distance, 0, 0, "%.0f"))
        {
            config.marker_max_distance = std::max(0.0f, max_distance) * 100.0;
            changed = true;
        }
        if (changed) session.update_config(config);
        auto& renderer = WaypointRenderer::get();
        ImGui::TextDisabled("Marker actors in world: %d (spawn failures %llu)   |   %s   |   HUD frames: %llu   drawn: %llu", renderer.marker_actor_count(), renderer.marker_spawn_failures(), renderer.hud_status().c_str(), renderer.hud_frames(), renderer.markers_drawn());
        if (renderer.marker_actor_count() == 0)
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1), "No marker actors yet: they appear ~10 s after a level finishes loading, for waypoints with Show ticked.");

        // ---- Add waypoint ----
        ImGui::Separator();
        ImGui::Text("New waypoint");
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##newname", "name", &ts.new_name);
        ImGui::SameLine();
        ImGui::Checkbox("visible in game", &ts.new_visible);
        ImGui::SameLine();
        ImGui::Checkbox("all levels", &ts.new_global);
        ImGui::SameLine();
        ImGui::ColorEdit3("##newcolor", ts.new_color, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::BeginDisabled(!snap.has_pawn);
        if (ImGui::Button("Add at current position"))
        {
            Waypoint w;
            w.name = ts.new_name.empty() ? "Waypoint " + std::to_string(store.all().size() + 1) : ts.new_name;
            w.level = ts.new_global ? kGlobalLevel : snap.level;
            w.x = snap.position.x;
            w.y = snap.position.y;
            w.z = snap.position.z;
            w.pitch = snap.rotation.pitch;
            w.yaw = snap.rotation.yaw;
            w.roll = snap.rotation.roll;
            w.visible = ts.new_visible;
            w.color = {ts.new_color[0], ts.new_color[1], ts.new_color[2]};
            store.add(w);
            ts.new_name.clear();
        }
        ImGui::EndDisabled();

        // ---- Manual teleport ----
        ImGui::Separator();
        ImGui::Text("Teleport to coordinates");
        if (ts.manual_dirty && snap.has_pawn)
        {
            ts.manual[0] = snap.position.x;
            ts.manual[1] = snap.position.y;
            ts.manual[2] = snap.position.z;
            ts.manual_dirty = false;
        }
        ImGui::SetNextItemWidth(110);
        ImGui::InputDouble("X", &ts.manual[0], 0, 0, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputDouble("Y", &ts.manual[1], 0, 0, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputDouble("Z", &ts.manual[2], 0, 0, "%.1f");
        ImGui::SameLine();
        ImGui::BeginDisabled(!snap.has_pawn);
        if (ImGui::Button("Teleport")) session.teleport_player({ts.manual[0], ts.manual[1], ts.manual[2]});
        ImGui::SameLine();
        if (ImGui::Button("Use current")) ts.manual_dirty = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!session.has_back_position());
        if (ImGui::Button("Back")) session.teleport_back();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Return to where you were before the last teleport (F7)");

        // ---- Waypoint list ----
        ImGui::Separator();
        ImGui::Text("Waypoints");
        ImGui::SameLine();
        ImGui::Checkbox("show other levels", &ts.show_all_levels);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##wpfilter", "filter...", &ts.filter);

        auto list = ts.show_all_levels ? store.all() : store.for_level(snap.level);
        if (list.empty())
        {
            ImGui::TextDisabled("No waypoints for this level yet.");
            return;
        }
        if (!ImGui::BeginTable("##waypoints", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
            return;
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Position");
        ImGui::TableSetupColumn("Dist (m)", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 230);
        ImGui::TableHeadersRow();

        std::string needle = ts.filter;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (Waypoint wp : list)
        {
            if (!needle.empty())
            {
                std::string lower = wp.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find(needle) == std::string::npos) continue;
            }
            ImGui::PushID(wp.id);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool visible = wp.visible;
            if (ImGui::Checkbox("##vis", &visible)) store.set_visible(wp.id, visible);

            ImGui::TableNextColumn();
            float color[3] = {wp.color[0], wp.color[1], wp.color[2]};
            if (ImGui::ColorEdit3("##col", color, ImGuiColorEditFlags_NoInputs))
            {
                wp.color = {color[0], color[1], color[2]};
                store.update(wp);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            std::string name = wp.name;
            ImGui::InputText("##name", &name);
            if (ImGui::IsItemDeactivatedAfterEdit() && !name.empty())
            {
                wp.name = name;
                store.update(wp);
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(wp.level == kGlobalLevel ? "(all)" : wp.level.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%.0f, %.0f, %.0f", wp.x, wp.y, wp.z);

            ImGui::TableNextColumn();
            if (snap.has_pawn && (wp.level == kGlobalLevel || wp.level == snap.level)) ImGui::Text("%.0f", distance(wp, snap.position) / 100.0);
            else ImGui::TextDisabled("-");

            ImGui::TableNextColumn();
            const bool here = wp.level == kGlobalLevel || wp.level == snap.level;
            ImGui::BeginDisabled(!snap.has_pawn || !here);
            if (ImGui::SmallButton("Teleport")) session.teleport_player({wp.x, wp.y, wp.z}, Rot3{wp.pitch, wp.yaw, wp.roll});
            ImGui::SameLine();
            if (ImGui::SmallButton("Move here"))
            {
                wp.x = snap.position.x;
                wp.y = snap.position.y;
                wp.z = snap.position.z;
                wp.pitch = snap.rotation.pitch;
                wp.yaw = snap.rotation.yaw;
                wp.roll = snap.rotation.roll;
                store.update(wp);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) store.remove(wp.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
} // namespace RC::Toolkit::UI
