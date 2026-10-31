#include <UI.hpp>

#include <algorithm>
#include <cstring>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <Session.hpp>
#include <Trainer/Inspect.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Trainer/TrainerEngine.hpp>

namespace RC::Toolkit::UI
{
    namespace
    {
        const char* const kActionKinds[] = {"set_property", "freeze_property", "scale_property", "call_function", "block_function", "console", "time_scale"};
        const char* const kCheatTypes[] = {"toggle", "button", "slider"};
        const char* const kFreezeModes[] = {"set", "at_least", "at_most"};

        template <size_t N>
        auto combo_string(const char* label, std::string& value, const char* const (&items)[N]) -> bool
        {
            int current = 0;
            for (size_t i = 0; i < N; ++i)
            {
                if (value == items[i]) current = static_cast<int>(i);
            }
            bool changed = false;
            if (ImGui::BeginCombo(label, items[current]))
            {
                for (size_t i = 0; i < N; ++i)
                {
                    if (ImGui::Selectable(items[i], static_cast<int>(i) == current))
                    {
                        value = items[i];
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        struct CreatorState
        {
            TrainerDef draft{};
            std::string file_stem{"MyTrainer"};
            int selected_cheat{-1};
            std::string message{};
            std::string test_status{};

            // Browser popup
            TargetInfo info{};
            std::string info_target{};
            std::string filter{};
            size_t browse_action{0};
            bool browse_functions{false};
            bool open_browser{false};

            // Class picker popup
            std::vector<ClassRow> classes{};
            std::string class_filter{};
            size_t class_action{0};
            bool include_non_actors{false};
            bool open_classes{false};
        };

        auto creator() -> CreatorState&
        {
            static CreatorState state;
            return state;
        }

        auto status_color(const std::string& status) -> ImVec4
        {
            if (status.empty() || status == "OK" || status == "off") return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
            return ImVec4(0.95f, 0.45f, 0.35f, 1.0f);
        }

        auto render_cheats() -> void
        {
            auto& engine = TrainerEngine::get();
            const auto trainers = engine.trainers();

            if (ImGui::Button("Reload trainer files")) engine.reload();
            ImGui::SameLine();
            if (ImGui::Button("Disable all cheats")) engine.disable_all();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", engine.directory().string().c_str());

            if (trainers.empty())
            {
                ImGui::TextWrapped("No trainer files found. Use the creator below, or drop .json trainers into the folder above.");
            }

            for (size_t ti = 0; ti < trainers.size(); ++ti)
            {
                const TrainerDef& trainer = trainers[ti];
                ImGui::PushID(static_cast<int>(ti));
                if (ImGui::CollapsingHeader(trainer.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (!trainer.description.empty()) ImGui::TextWrapped("%s", trainer.description.c_str());
                    for (size_t ci = 0; ci < trainer.cheats.size(); ++ci)
                    {
                        const CheatDef& cheat = trainer.cheats[ci];
                        CheatState state = engine.state(ti, ci);
                        ImGui::PushID(static_cast<int>(ci));
                        const char* label = cheat.label.empty() ? cheat.id.c_str() : cheat.label.c_str();
                        // A cheat with nothing to act on in this level cannot be switched on or pressed (it can still be
                        // switched off if it was already on).
                        const bool blocked = !state.available && !state.enabled;
                        if (blocked) ImGui::BeginDisabled();
                        if (cheat.type == "button")
                        {
                            if (ImGui::Button(label)) engine.press(ti, ci);
                        }
                        else
                        {
                            bool on = state.enabled;
                            if (ImGui::Checkbox(label, &on)) engine.set_enabled(ti, ci, on);
                            if (cheat.type == "slider")
                            {
                                ImGui::SameLine();
                                float value = static_cast<float>(state.value);
                                ImGui::SetNextItemWidth(220);
                                if (ImGui::SliderFloat("##value", &value, static_cast<float>(cheat.min), static_cast<float>(cheat.max), "%.2f"))
                                {
                                    engine.set_value(ti, ci, value);
                                    // Moving the slider is an intent to use it: switch the cheat on.
                                    if (!state.enabled) engine.set_enabled(ti, ci, true);
                                }
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set the value (Ctrl+click to type). Moving it switches the cheat on.");
                            }
                        }
                        if (blocked)
                        {
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not available: %s", state.unavailable_reason.c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled("(no target in this level)");
                        }
                        else if (ImGui::IsItemHovered() && !cheat.description.empty()) ImGui::SetTooltip("%s", cheat.description.c_str());
                        if (!state.status.empty() && state.status != "off" && !blocked)
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(status_color(state.status), "%s", state.status.c_str());
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
            }
        }

        auto open_browser(CreatorState& cs, size_t action_index, bool functions) -> void
        {
            cs.browse_action = action_index;
            cs.browse_functions = functions;
            cs.info_target = cs.draft.cheats[static_cast<size_t>(cs.selected_cheat)].actions[action_index].target;
            cs.info = inspect_target(cs.info_target);
            cs.filter.clear();
            cs.open_browser = true;
        }

        auto open_class_picker(CreatorState& cs, size_t action_index) -> void
        {
            cs.class_action = action_index;
            cs.classes = list_live_classes(cs.include_non_actors);
            cs.class_filter.clear();
            cs.open_classes = true;
        }

        // Lists the classes that currently have live objects. A trainer targets a class (all its instances), never a
        // single object, because object instances are recreated on every level load.
        auto render_class_picker(CreatorState& cs) -> void
        {
            if (cs.open_classes)
            {
                ImGui::OpenPopup("Class picker");
                cs.open_classes = false;
            }
            ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_Appearing);
            if (!ImGui::BeginPopupModal("Class picker", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

            ActionDef& action = cs.draft.cheats[static_cast<size_t>(cs.selected_cheat)].actions[cs.class_action];

            ImGui::TextWrapped("Pick the class to target. Every live instance of it is affected, so the cheat keeps working after level loads.");
            ImGui::SetNextItemWidth(240);
            ImGui::InputTextWithHint("##classfilter", "filter...", &cs.class_filter);
            ImGui::SameLine();
            if (ImGui::Checkbox("include non-actors (widgets, components, managers)", &cs.include_non_actors)) cs.classes = list_live_classes(cs.include_non_actors);
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh")) cs.classes = list_live_classes(cs.include_non_actors);

            std::string needle = cs.class_filter;
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (cs.classes.empty()) ImGui::TextDisabled("No classes found - is a level loaded?");
            if (ImGui::BeginChild("##classrows", ImVec2(0, -34), ImGuiChildFlags_Borders))
            {
                if (ImGui::BeginTable("##classes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
                {
                    ImGui::TableSetupColumn("Class");
                    ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("##use", ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < cs.classes.size(); ++i)
                    {
                        const ClassRow& row = cs.classes[i];
                        if (!needle.empty())
                        {
                            std::string lower = row.name;
                            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (lower.find(needle) == std::string::npos) continue;
                        }
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.name.c_str());
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.path.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", row.instances);
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("Use"))
                        {
                            action.target = "class:" + row.name;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        auto render_browser(CreatorState& cs) -> void
        {
            if (cs.open_browser)
            {
                ImGui::OpenPopup("Target browser");
                cs.open_browser = false;
            }
            ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Appearing);
            if (!ImGui::BeginPopupModal("Target browser", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

            CheatDef& cheat = cs.draft.cheats[static_cast<size_t>(cs.selected_cheat)];
            ActionDef& action = cheat.actions[cs.browse_action];

            ImGui::Text("Target:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(320);
            ImGui::InputText("##browse_target", &cs.info_target);
            ImGui::SameLine();
            if (ImGui::Button("Inspect")) cs.info = inspect_target(cs.info_target);
            ImGui::SameLine();
            if (ImGui::RadioButton("Properties", !cs.browse_functions)) cs.browse_functions = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Functions", cs.browse_functions)) cs.browse_functions = true;

            if (!cs.info.found)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", cs.info.error.c_str());
            }
            else
            {
                ImGui::Text("%s  (%s)", cs.info.object_name.c_str(), cs.info.class_name.c_str());
                ImGui::SetNextItemWidth(260);
                ImGui::InputTextWithHint("##filter", "filter...", &cs.filter);
                std::string needle = cs.filter;
                std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto matches = [&](const std::string& text) {
                    if (needle.empty()) return true;
                    std::string lower = text;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return lower.find(needle) != std::string::npos;
                };

                if (ImGui::BeginChild("##rows", ImVec2(0, -34), ImGuiChildFlags_Borders))
                {
                    if (!cs.browse_functions)
                    {
                        if (ImGui::BeginTable("##props", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
                        {
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("Type");
                            ImGui::TableSetupColumn("Value");
                            ImGui::TableSetupColumn("##use", ImGuiTableColumnFlags_WidthFixed, 150);
                            ImGui::TableHeadersRow();
                            for (size_t i = 0; i < cs.info.properties.size(); ++i)
                            {
                                const PropRow& row = cs.info.properties[i];
                                if (!matches(row.name)) continue;
                                ImGui::PushID(static_cast<int>(i));
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(row.name.c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextDisabled("%s", row.type.c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(row.value.c_str());
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton("Use"))
                                {
                                    action.property = row.name;
                                    if (action.value.empty() || action.kind == "set_property") action.value = row.value;
                                    ImGui::CloseCurrentPopup();
                                }
                                if (row.is_object)
                                {
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Into"))
                                    {
                                        // Descend: target becomes <target>.<Property>, e.g. player_pawn.CharacterMovement.
                                        cs.info_target += "." + row.name;
                                        cs.info = inspect_target(cs.info_target);
                                        action.target = cs.info_target;
                                    }
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    }
                    else if (ImGui::BeginTable("##funcs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
                    {
                        ImGui::TableSetupColumn("Function");
                        ImGui::TableSetupColumn("Parameters");
                        ImGui::TableSetupColumn("##use", ImGuiTableColumnFlags_WidthFixed, 60);
                        ImGui::TableHeadersRow();
                        for (size_t i = 0; i < cs.info.functions.size(); ++i)
                        {
                            const FuncRow& row = cs.info.functions[i];
                            if (!matches(row.name)) continue;
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(row.name.c_str());
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("declared on %s", row.owner.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", row.signature.c_str());
                            ImGui::TableNextColumn();
                            if (ImGui::SmallButton("Use"))
                            {
                                action.function = row.name;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
            }
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        auto render_map_editor(const char* label, std::map<std::string, std::string>& params) -> void
        {
            ImGui::TextDisabled("%s", label);
            std::string remove_key;
            for (auto& [key, value] : params)
            {
                ImGui::PushID(key.c_str());
                ImGui::SetNextItemWidth(160);
                ImGui::TextUnformatted(key.c_str());
                ImGui::SameLine(190);
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("##value", &value);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) remove_key = key;
                ImGui::PopID();
            }
            if (!remove_key.empty()) params.erase(remove_key);
            static std::string new_key;
            ImGui::PushID(label);
            ImGui::SetNextItemWidth(160);
            ImGui::InputTextWithHint("##newkey", "parameter name", &new_key);
            ImGui::SameLine();
            if (ImGui::SmallButton("Add parameter") && !new_key.empty())
            {
                params.emplace(new_key, "");
                new_key.clear();
            }
            ImGui::PopID();
        }

        auto render_action_editor(CreatorState& cs, CheatDef& cheat, size_t index) -> bool
        {
            ActionDef& action = cheat.actions[index];
            bool remove = false;
            ImGui::PushID(static_cast<int>(index));
            const std::string title = "Action " + std::to_string(index + 1) + ": " + action.kind + " " + action.property + action.function;
            if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SetNextItemWidth(200);
                combo_string("kind", action.kind, kActionKinds);

                const bool needs_target = action.kind != "console" && action.kind != "time_scale";
                if (needs_target)
                {
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("target", &action.target);
                    ImGui::SameLine();
                    if (ImGui::BeginCombo("##roots", "roots...", ImGuiComboFlags_NoPreview | ImGuiComboFlags_WidthFitPreview))
                    {
                        for (const auto& root : target_roots())
                        {
                            if (ImGui::Selectable(root.c_str())) action.target = root;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Pick class...")) open_class_picker(cs, index);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose from the classes that currently have live objects");

                    // A class can have many live objects: apply to all of them, or to just one.
                    static const char* const select_labels[] = {"all instances of the class", "just the first one", "the one nearest to the player", "the Nth one (index)"};
                    static const char* const select_values[] = {"all", "first", "nearest", "index"};
                    int current = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (action.select == select_values[i]) current = i;
                    }
                    ImGui::SetNextItemWidth(260);
                    if (ImGui::BeginCombo("applies to", select_labels[current]))
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            if (ImGui::Selectable(select_labels[i], i == current)) action.select = select_values[i];
                        }
                        ImGui::EndCombo();
                    }
                    if (action.select == "index")
                    {
                        ImGui::SameLine();
                        int index = action.index;
                        ImGui::SetNextItemWidth(90);
                        if (ImGui::InputInt("##select_index", &index)) action.index = std::max(0, index);
                    }
                }

                const bool is_property = action.kind == "set_property" || action.kind == "freeze_property" || action.kind == "scale_property";
                if (is_property)
                {
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("property", &action.property);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Browse...")) open_browser(cs, index, false);
                }
                if (action.kind == "scale_property")
                {
                    double scale = action.scale;
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::InputDouble("scale", &scale)) action.scale = scale;
                    ImGui::SameLine();
                    ImGui::TextDisabled("(multiplied by the slider value for slider cheats)");
                }
                if (action.kind == "set_property" || action.kind == "freeze_property")
                {
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("value", &action.value);
                    ImGui::SameLine();
                    ImGui::TextDisabled("{value} = slider");
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("off value", &action.off_value);
                    ImGui::SameLine();
                    ImGui::TextDisabled("empty = restore old value");
                }
                if (action.kind == "freeze_property")
                {
                    ImGui::SetNextItemWidth(200);
                    combo_string("mode", action.mode, kFreezeModes);
                }
                if (action.kind == "call_function" || action.kind == "block_function")
                {
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("function", &action.function);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Browse...")) open_browser(cs, index, true);
                }
                if (action.kind == "call_function")
                {
                    render_map_editor("parameters", action.params);
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("off function", &action.off_function);
                    if (!action.off_function.empty()) render_map_editor("off parameters", action.off_params);
                }
                if (action.kind == "block_function")
                {
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("return value", &action.return_value);
                    ImGui::SameLine();
                    ImGui::TextDisabled("optional, text form");
                }
                if (action.kind == "console" || action.kind == "time_scale")
                {
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("value", &action.value);
                    if (action.kind == "console")
                    {
                        ImGui::SetNextItemWidth(260);
                        ImGui::InputText("off value", &action.off_value);
                    }
                }
                if (ImGui::SmallButton("Remove action")) remove = true;
                ImGui::TreePop();
            }
            ImGui::PopID();
            return remove;
        }

        auto render_creator() -> void
        {
            auto& cs = creator();
            auto& engine = TrainerEngine::get();
            const auto trainers = engine.trainers();

            ImGui::TextWrapped("Build cheats from live game data: pick a target, browse its properties or functions, and test the result before saving. "
                               "Saved trainers appear above and work for anyone who drops the .json file in the trainers folder.");

            if (ImGui::BeginCombo("Load existing", "choose..."))
            {
                for (size_t i = 0; i < trainers.size(); ++i)
                {
                    if (ImGui::Selectable(trainers[i].name.c_str()))
                    {
                        cs.draft = trainers[i];
                        cs.file_stem = engine.file_stem_of(i);
                        cs.selected_cheat = trainers[i].cheats.empty() ? -1 : 0;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("New trainer"))
            {
                cs.draft = TrainerDef{};
                cs.draft.name = "My Trainer";
                cs.file_stem = "MyTrainer";
                cs.selected_cheat = -1;
            }

            ImGui::SetNextItemWidth(260);
            ImGui::InputText("Trainer name", &cs.draft.name);
            ImGui::SetNextItemWidth(260);
            ImGui::InputText("File name", &cs.file_stem);
            ImGui::SetNextItemWidth(260);
            ImGui::InputText("Author", &cs.draft.author);
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("Description", &cs.draft.description);

            ImGui::Separator();
            ImGui::Text("Cheats in this trainer");
            for (size_t i = 0; i < cs.draft.cheats.size(); ++i)
            {
                const std::string name = (cs.draft.cheats[i].label.empty() ? "(unnamed)" : cs.draft.cheats[i].label) + "##cheat" + std::to_string(i);
                if (ImGui::Selectable(name.c_str(), cs.selected_cheat == static_cast<int>(i))) cs.selected_cheat = static_cast<int>(i);
            }
            if (ImGui::Button("Add cheat"))
            {
                CheatDef cheat;
                cheat.id = "cheat_" + std::to_string(cs.draft.cheats.size() + 1);
                cheat.label = "New cheat";
                cheat.actions.push_back(ActionDef{});
                cs.draft.cheats.push_back(cheat);
                cs.selected_cheat = static_cast<int>(cs.draft.cheats.size()) - 1;
            }
            if (cs.selected_cheat >= 0 && cs.selected_cheat < static_cast<int>(cs.draft.cheats.size()))
            {
                ImGui::SameLine();
                if (ImGui::Button("Delete cheat"))
                {
                    cs.draft.cheats.erase(cs.draft.cheats.begin() + cs.selected_cheat);
                    cs.selected_cheat = std::min(cs.selected_cheat, static_cast<int>(cs.draft.cheats.size()) - 1);
                }
            }

            if (cs.selected_cheat >= 0 && cs.selected_cheat < static_cast<int>(cs.draft.cheats.size()))
            {
                CheatDef& cheat = cs.draft.cheats[static_cast<size_t>(cs.selected_cheat)];
                ImGui::Separator();
                ImGui::SetNextItemWidth(180);
                ImGui::InputText("id", &cheat.id);
                ImGui::SetNextItemWidth(260);
                ImGui::InputText("label", &cheat.label);
                ImGui::SetNextItemWidth(420);
                ImGui::InputText("tooltip", &cheat.description);
                ImGui::SetNextItemWidth(160);
                combo_string("type", cheat.type, kCheatTypes);
                if (cheat.type == "slider")
                {
                    ImGui::SetNextItemWidth(100);
                    ImGui::InputDouble("min", &cheat.min);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    ImGui::InputDouble("max", &cheat.max);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    ImGui::InputDouble("default", &cheat.default_value);
                }

                size_t remove_index = cheat.actions.size();
                for (size_t a = 0; a < cheat.actions.size(); ++a)
                {
                    if (render_action_editor(cs, cheat, a)) remove_index = a;
                }
                if (remove_index < cheat.actions.size()) cheat.actions.erase(cheat.actions.begin() + static_cast<ptrdiff_t>(remove_index));
                if (ImGui::Button("Add action")) cheat.actions.push_back(ActionDef{});

                ImGui::Separator();
                double test_value = cheat.type == "slider" ? cheat.default_value : 1.0;
                if (ImGui::Button("Test: apply")) cs.test_status = engine.test_cheat(cheat, true, test_value);
                ImGui::SameLine();
                if (ImGui::Button("Test: revert")) cs.test_status = engine.test_cheat(cheat, false, test_value);
                if (!cs.test_status.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(status_color(cs.test_status), "%s", cs.test_status.c_str());
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Save trainer"))
            {
                cs.message = engine.save(cs.draft, cs.file_stem) ? "Saved " + cs.file_stem + ".json" : "Could not write the trainer file";
            }
            if (!cs.message.empty())
            {
                ImGui::SameLine();
                ImGui::TextUnformatted(cs.message.c_str());
            }

            if (cs.selected_cheat >= 0 && cs.selected_cheat < static_cast<int>(cs.draft.cheats.size()))
            {
                render_browser(cs);
                render_class_picker(cs);
            }
        }
    } // namespace

    auto render_trainer_tab() -> void
    {
        render_cheats();
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Trainer creator")) render_creator();
    }
} // namespace RC::Toolkit::UI
