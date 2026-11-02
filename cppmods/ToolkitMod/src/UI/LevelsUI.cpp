#include <UI.hpp>

#include <algorithm>
#include <atomic>
#include <glaze/glaze.hpp>
#include <mutex>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/GameThread.hpp>
#include <Helpers/String.hpp>
#include <LevelGuards.hpp>
#include <Session.hpp>

namespace RC::Toolkit::UI
{
    struct LevelNote
    {
        std::string level{};    // short name or package path
        std::string severity{}; // "hang" for a level that freezes the game
        std::string note{};
    };
    
    struct LevelNotes
    {
        std::vector<LevelNote> levels{};
        std::vector<LevelNote> unsafe_origins{};
    std::string advisory{}; // shown at the top of the tab
    };

    namespace
    {
        struct LevelRow
        {
            std::string name;
            std::string path;
            bool game_content{};
        };

        struct LevelState
        {
            std::mutex mutex;
            std::vector<LevelRow> levels;
            std::atomic<bool> loading{false};
            std::atomic<bool> loaded{false};
            std::string filter;
            std::string options;
            bool game_only{true};
            std::vector<std::string> recent;
            // Per-game notes about levels (Mods/ToolkitMod/level_notes.json): known hangs, unsafe places to travel from.
            std::vector<LevelNote> notes;
            std::vector<LevelNote> unsafe_origins;
            std::string advisory;
            bool notes_loaded{false};
            std::string pending_path;
            std::string pending_note;
            bool open_confirm{false};
        };

        auto state() -> LevelState&
        {
            static LevelState s;
            return s;
        }

        auto load_notes(LevelState& ls) -> void
        {
            if (ls.notes_loaded) return;
            ls.notes_loaded = true;
            LevelNotes loaded;
            const auto path = Session::get().root() / "level_notes.json";
            if (!glz::read_file_json<glz::opts{.error_on_unknown_keys = false}>(loaded, path.string(), std::string{}))
            {
                ls.notes = std::move(loaded.levels);
                ls.unsafe_origins = std::move(loaded.unsafe_origins);
                ls.advisory = std::move(loaded.advisory);
            }
        }

        auto note_for(const LevelState& ls, const LevelRow& row) -> const LevelNote*
        {
            for (const auto& n : ls.notes)
            {
                if (n.level == row.path || n.level == row.name) return &n;
            }
            return nullptr;
        }

        auto origin_warning(const LevelState& ls, const std::string& current_level) -> const LevelNote*
        {
            for (const auto& n : ls.unsafe_origins)
            {
                if (n.level == current_level) return &n;
            }
            return nullptr;
        }

        auto request_refresh() -> void
        {
            auto& ls = state();
            if (ls.loading.exchange(true)) return;
            Compat::GameThread::Post([] {
                auto& ls2 = state();
                std::vector<LevelRow> rows;
                try
                {
                    for (const auto& info : Compat::AdapterRegistry::Get().enumerate_levels())
                        rows.push_back({to_string(info.name), to_string(info.package_path), info.is_game_content()});
                }
                catch (...)
                {
                }
                std::lock_guard lock(ls2.mutex);
                ls2.levels = std::move(rows);
                ls2.loaded = true;
                ls2.loading = false;
            });
        }

        auto lower(std::string text) -> std::string
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    } // namespace

    auto render_levels_tab() -> void
    {
        const ScrollRegion scroll("##toolkit_scroll_levels");
        auto& ls = state();
        auto& session = Session::get();
        const SessionSnapshot snap = session.snapshot();

        if (!ls.loaded && !ls.loading) request_refresh();
        load_notes(ls);

        if (snap.has_world) ImGui::Text("Current level: %s", snap.level.c_str());
        else ImGui::TextDisabled("No world loaded");
        if (!ls.advisory.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", ls.advisory.c_str());
        if (const LevelNote* warn = origin_warning(ls, snap.level))
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "Warning: %s", warn->note.c_str());
        if (const auto& guards = LevelGuards::get().guards(); !guards.empty())
        {
            ToolkitConfig config = session.config();
            if (ImGui::Checkbox("Level guards (skip game events listed in level_notes.json)", &config.level_guards)) session.update_config(config);
            if (ImGui::IsItemHovered())
            {
                std::string text;
                for (const auto& g : guards) text += g.text_class + "::" + g.text_function + " - " + g.reason + "\n";
                ImGui::SetTooltip("%s", text.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%llu event(s) skipped", LevelGuards::get().blocked_count());
        }
        const std::string message = session.last_message();
        if (!message.empty()) ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1), "%s", message.c_str());

        if (ImGui::Button("Refresh list")) request_refresh();
        ImGui::SameLine();
        ImGui::BeginDisabled(!snap.has_world);
        if (ImGui::Button("Reload current level"))
        {
            // Match by short name so reload works whatever the package path is.
            std::lock_guard lock(ls.mutex);
            for (const auto& row : ls.levels)
            {
                if (row.name == snap.level)
                {
                    session.open_level(row.path, ls.options);
                    break;
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("game levels only", &ls.game_only);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##levelfilter", "filter...", &ls.filter);
        ImGui::SetNextItemWidth(320);
        ImGui::InputTextWithHint("Travel options", "e.g. ?listen   (optional)", &ls.options);
        if (ls.loading) ImGui::TextDisabled("Reading the asset registry...");

        std::vector<LevelRow> rows;
        {
            std::lock_guard lock(ls.mutex);
            rows = ls.levels;
        }
        const std::string needle = lower(ls.filter);

        if (!ls.recent.empty())
        {
            ImGui::Text("Recent:");
            for (const auto& path : ls.recent)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton((path.substr(path.rfind('/') + 1) + "##recent" + path).c_str())) session.open_level(path, ls.options);
            }
        }

        if (ls.open_confirm)
        {
            ImGui::OpenPopup("Open anyway?");
            ls.open_confirm = false;
        }
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Open anyway?", nullptr, ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::TextWrapped("%s", ls.pending_note.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("%s", ls.pending_path.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Open anyway"))
            {
                session.open_level(ls.pending_path, ls.options);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (!ImGui::BeginTable("##levels", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, std::max(300.0f, ImGui::GetContentRegionAvail().y)))) return;
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Package");
        ImGui::TableSetupColumn("##open", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (const LevelRow& row : rows)
        {
            if (ls.game_only && !row.game_content) continue;
            if (!needle.empty() && lower(row.path).find(needle) == std::string::npos) continue;
            ++shown;
            ImGui::PushID(row.path.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool current = snap.has_world && row.name == snap.level;
            if (current) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "%s  (current)", row.name.c_str());
            else ImGui::TextUnformatted(row.name.c_str());
            if (const LevelNote* warn = note_for(ls, row))
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "[!]");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", warn->note.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", row.path.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Open"))
            {
                // Known-bad levels, and travelling away from an unsafe level, ask first.
                const LevelNote* warn = note_for(ls, row);
                const LevelNote* origin = origin_warning(ls, snap.level);
                if (warn || origin)
                {
                    ls.pending_path = row.path;
                    ls.pending_note = warn ? warn->note : origin->note;
                    ls.open_confirm = true;
                }
                else
                {
                    session.open_level(row.path, ls.options);
                    ls.recent.erase(std::remove(ls.recent.begin(), ls.recent.end(), row.path), ls.recent.end());
                    ls.recent.insert(ls.recent.begin(), row.path);
                    if (ls.recent.size() > 6) ls.recent.pop_back();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (shown == 0 && ls.loaded) ImGui::TextDisabled("No levels match.");
    }
} // namespace RC::Toolkit::UI
