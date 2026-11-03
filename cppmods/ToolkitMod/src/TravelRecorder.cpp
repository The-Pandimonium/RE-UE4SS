#include <TravelRecorder.hpp>

#include <algorithm>
#include <format>
#include <cstdlib>
#include <fstream>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Session.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;
    using RC::Compat::GameThread;

    namespace
    {
        // Functions through which a Blueprint (or the console) asks the engine to travel.
        auto is_travel_function(const std::string& name) -> bool
        {
            static const char* const names[] = {"OpenLevel", "OpenLevelBySoftObjectPtr", "ExecuteConsoleCommand", "ConsoleCommand", "ClientTravel",
                                                "ServerTravel", "StreamLevel", "LoadStreamLevel", "LoadGameFromSlot", "SaveGameToSlot",
                                                "DoesSaveGameExist", "ClientTravelInternal",
                                                // Voices of the Void's own launch path (found with the first recordings).
                                                "loadLevel", "transition", "setSaveSlotObject", "tryLaunch", "isAreaLevelRestricted", "getMainDataSlotName",
                                                "fromGamemode_getMainDataSlotName", "selectSlot_sub", "assignSlotForRuleChange", "openSandbox", "setGamemode",
                                                "setGameinst", "getSandboxMaps",
                                                "BndEvt__button_apply_K2Node_ComponentBoundEvent_9_OnButtonClickedEvent__DelegateSignature",
                                                "BndEvt__button_select_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature"};
            return std::any_of(std::begin(names), std::end(names), [&](const char* n) { return name == n; });
        }

        auto lower(std::string text) -> std::string
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        auto interesting(const std::string& text) -> bool
        {
            static const char* const words[] = {"sandbox", "start", "newgame", "new_game", "load", "map", "level", "travel", "open", "slot", "mode", "launch", "play", "menu", "world"};
            const std::string t = lower(text);
            return std::any_of(std::begin(words), std::end(words), [&](const char* w) { return t.find(w) != std::string::npos; });
        }

        auto snapshot_properties(UObject* object) -> std::map<std::string, std::string>
        {
            std::map<std::string, std::string> out;
            if (!object) return out;
            for (FProperty* p : TFieldRange<FProperty>(object->GetClassPrivate()))
            {
                std::string value;
                if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(object), object)) value = to_string(*text);
                if (value.size() > 300) value.resize(300);
                out[to_string(p->GetName())] = value;
            }
            return out;
        }

        auto object_named(const std::string& target) -> UObject*
        {
            auto objects = resolve_targets(target, "first", 0);
            return objects.empty() ? nullptr : objects.front();
        }
    } // namespace

    auto TravelRecorder::get() -> TravelRecorder&
    {
        static TravelRecorder instance;
        return instance;
    }

    auto TravelRecorder::install() -> void
    {
        GameThread::AddTick([this](float dt) { tick(dt); }, "travel_recorder");
    }

    auto TravelRecorder::status() const -> std::string
    {
        std::lock_guard lock(m_mutex);
        return m_status;
    }

    auto TravelRecorder::elapsed_ms() const -> double
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started).count();
    }

    namespace
    {
        // Our own engine calls (reading properties for the trace) must not be recorded, or re-enter the recorder's lock.
        thread_local bool t_inside_recorder = false;
        struct InsideRecorder
        {
            bool previous{t_inside_recorder};
            InsideRecorder() { t_inside_recorder = true; }
            ~InsideRecorder() { t_inside_recorder = previous; }
        };

        auto file_safe(std::string text) -> std::string
        {
            for (char& c : text)
            {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
            }
            return text;
        }
    } // namespace

    auto TravelRecorder::arm() -> void
    {
        {
            std::lock_guard lock(m_mutex);
            m_status = "starting...";
        }
        GameThread::Post([this] {
            InsideRecorder guard;
            if (!m_hooks)
            {
                m_hooks = true;
                // Hooks stay installed once added; they do nothing unless the recorder is armed.
                Hook::RegisterProcessEventPreCallback(
                        [this](Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void* params) {
                            if (m_armed.load(std::memory_order_relaxed) && function) note(context, function, params, false);
                        },
                        Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("TravelRecorder")});
                // For Blueprint-to-Blueprint calls the new frame's locals already hold the evaluated input parameters.
                auto script = [this](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void*) {
                    if (m_armed.load(std::memory_order_relaxed) && stack.Node()) note(context, stack.Node(), stack.Locals(), false);
                };
                auto script_after = [this](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void*) {
                    if (m_armed.load(std::memory_order_relaxed) && stack.Node()) note(context, stack.Node(), stack.Locals(), true);
                };
                Hook::RegisterProcessLocalScriptFunctionPreCallback(script, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("TravelRecorderVM")});
                Hook::RegisterProcessInternalPreCallback(script, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("TravelRecorderInternal")});
                Hook::RegisterProcessLocalScriptFunctionPostCallback(script_after, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("TravelRecorderVMAfter")});
                Hook::RegisterProcessInternalPostCallback(script_after, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("TravelRecorderInternalAfter")});
            }
            begin_segment();
            m_armed = true;
        });
    }

    // Starts a fresh recording from the current level (also used after each level change, so a whole menu walk is covered).
    auto TravelRecorder::begin_segment() -> void
    {
        const std::string level = to_string(Compat::AdapterRegistry::Get().current_level_name());
        auto before = snapshot_properties(object_named("game_instance"));
        std::lock_guard lock(m_mutex);
        m_seen.clear();
        m_travel_calls.clear();
        m_next_sequence = 0;
        m_sequence_at_change = 0;
        m_changed = false;
        m_level_after.clear();
        m_level_before = level;
        m_instance_before = std::move(before);
        m_started = std::chrono::steady_clock::now();
        m_status = "recording in '" + level + "': now open a level (through the game's own menu)";
    }

    auto TravelRecorder::disarm() -> void
    {
        GameThread::Post([this] {
            InsideRecorder guard;
            if (!m_armed) return;
            write_trace("partial");
            m_armed = false;
            std::lock_guard lock(m_mutex);
            m_status = "stopped";
        });
    }

    // May run on any thread that calls into the engine, so only touch our own tables.
    auto TravelRecorder::note(UObject* context, UFunction* function, void* params, bool after) -> void
    {
        if (t_inside_recorder) return;
        InsideRecorder guard;
        std::string travel_line;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_seen.find(function);
            if (it == m_seen.end())
            {
                if (after) return; // the pre-call hook always runs first
                Seen s;
                UObject* outer = function->GetOuterPrivate();
                s.owner = outer ? to_string(outer->GetName()) : std::string{"?"};
                s.name = to_string(function->GetName());
                s.sequence = m_next_sequence++;
                s.first_ms = elapsed_ms();
                it = m_seen.emplace(function, std::move(s)).first;
            }
            if (!after) ++it->second.count;
            if (!params || !is_travel_function(it->second.name) || m_travel_calls.size() >= 400) return;
            travel_line = std::format("{:.0f} ms  {} {}::{}", elapsed_ms(), after ? "RETURN" : "CALL  ", it->second.owner, it->second.name);
        }

        // Remember what the travel-related calls were handed (before the call) and what they ended up with (after it).
        std::string args;
        for (FProperty* p : TFieldRange<FProperty>(function))
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            const bool is_output = p->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm);
            if (is_output && !after) continue;
            if (!is_output && after) continue; // inputs were listed with the call
            std::string value;
            if (auto text = Reflect::ExportText(p, static_cast<uint8_t*>(params) + p->GetOffset_Internal(), context)) value = to_string(*text);
            if (value.size() > 200) value.resize(200);
            args += (args.empty() ? "" : ", ") + to_string(p->GetName()) + "=" + value;
        }
        travel_line += "(" + args + ")  on " + (context ? to_string(context->GetName()) : std::string{"?"});

        // The game instance as it is at this very call: the menu prepares it before it asks to open a level.
        if (!after)
        {
            std::map<std::string, std::string> before;
            {
                std::lock_guard lock(m_mutex);
                before = m_instance_before;
            }
            std::string changes;
            for (const auto& [name, value] : snapshot_properties(object_named("game_instance")))
            {
                auto found = before.find(name);
                if (found == before.end() || found->second != value)
                    changes += "\n        game_instance." + name + " = " + (value.size() > 80 ? value.substr(0, 80) + "..." : value);
            }
            if (!changes.empty()) travel_line += changes;
        }

        std::lock_guard lock(m_mutex);
        if (!m_travel_calls.empty() && m_travel_calls.back() == travel_line) return; // the same call reported by two hooks
        m_travel_calls.push_back(std::move(travel_line));
    }

    auto TravelRecorder::tick(float dt) -> void
    {
        m_clock += dt;
        // TOOLKIT_RECORD_TRAVEL=1 arms the recorder shortly after start (for runs without anyone at the keyboard).
        if (!m_requested_arm && m_clock > 3.0 && std::getenv("TOOLKIT_RECORD_TRAVEL"))
        {
            m_requested_arm = true;
            arm();
        }
        // TOOLKIT_OPEN_LEVEL=<package path> opens that level through the level switcher once the menu has been up a while,
        // so the level switcher can be tried without anyone clicking through the game's menu.
        if (!m_opened_test_level)
        {
            if (const char* wanted = std::getenv("TOOLKIT_OPEN_LEVEL"))
            {
                if (to_string(Compat::AdapterRegistry::Get().current_level_name()) == "menu")
                {
                    if (m_menu_since == 0.0) m_menu_since = m_clock;
                    if (m_clock - m_menu_since > 8.0)
                    {
                        m_opened_test_level = true;
                        m_test_opened_at = m_clock;
                        Session::get().open_level(wanted);
                    }
                }
            }
        }
        // If the level did not change within 15 s of the test open, write down what happened anyway.
        else if (m_test_opened_at > 0.0 && m_clock - m_test_opened_at > 15.0)
        {
            m_test_opened_at = 0.0;
            if (m_armed && !m_changed) disarm();
        }
        if (!m_armed) return;
        InsideRecorder guard;
        const std::string level = to_string(Compat::AdapterRegistry::Get().current_level_name());
        if (!m_changed)
        {
            if (!level.empty() && level != m_level_before)
            {
                m_changed = true;
                m_changed_at = m_clock;
                {
                    std::lock_guard lock(m_mutex);
                    m_level_after = level;
                    m_sequence_at_change = m_next_sequence;
                    m_status = "level changed to '" + level + "'; collecting the aftermath...";
                }
                write_trace("stage1"); // in case the level then freezes the game
            }
            return;
        }
        if (m_clock - m_changed_at > 5.0f)
        {
            write_trace("final");
            std::string done;
            {
                std::lock_guard lock(m_mutex);
                done = "'" + m_level_before + "' -> '" + m_level_after + "' written to travel_trace_" + file_safe(m_level_before) + "_to_" + file_safe(m_level_after) + ".txt";
            }
            begin_segment(); // keep recording: the next level change gets its own trace
            std::lock_guard lock(m_mutex);
            m_status = "last: " + done + "; recording in '" + m_level_before + "'";
        }
    }

    auto TravelRecorder::write_trace(const std::string& stage) -> void
    {
        // Copy what was recorded, then read the game without holding the lock (those reads may call into hooked code).
        std::unordered_map<UFunction*, Seen> seen_copy;
        std::vector<std::string> travel_calls;
        std::map<std::string, std::string> instance_before;
        std::string level_before, level_after;
        uint64_t sequence_at_change;
        bool changed;
        {
            std::lock_guard lock(m_mutex);
            seen_copy = m_seen;
            travel_calls = m_travel_calls;
            instance_before = m_instance_before;
            level_before = m_level_before;
            level_after = m_level_after;
            sequence_at_change = m_sequence_at_change;
            changed = m_changed;
        }

        const std::string file = changed ? "travel_trace_" + file_safe(level_before) + "_to_" + file_safe(level_after) + ".txt" : "travel_trace_" + file_safe(level_before) + "_partial.txt";
        std::ofstream out(Session::get().root() / file, std::ios::trunc);
        out << "Travel trace (" << stage << ")\n";
        out << "level before: " << level_before << "\nlevel after: " << (changed ? level_after : std::string{"(unchanged)"}) << "\n\n";

        out << "== Travel-related calls seen through ProcessEvent (arguments) ==\n";
        if (travel_calls.empty()) out << "(none: the menu opens the level through a path that does not go through ProcessEvent)\n";
        for (const auto& line : travel_calls) out << line << "\n";

        out << "\n== GameInstance properties that changed since recording started ==\n";
        const auto after = snapshot_properties(object_named("game_instance"));
        int differing = 0;
        for (const auto& [name, value] : after)
        {
            auto it = instance_before.find(name);
            if (it == instance_before.end() || it->second != value)
            {
                out << name << ": " << (it == instance_before.end() ? std::string{"(absent)"} : it->second) << "  ->  " << value << "\n";
                ++differing;
            }
        }
        if (!differing) out << "(none)\n";

        if (changed)
        {
            UObject* mode = object_named("game_mode");
            out << "\n== Game mode " << (mode ? to_string(mode->GetClassPrivate()->GetName()) : std::string{"(none)"}) << " ==\n";
            if (mode)
            {
                // OptionsString holds the "?key=value" options the level was opened with, i.e. what the menu passed along.
                if (auto options = Reflect::GetPropertyText(mode, L"OptionsString")) out << "OptionsString: '" << to_string(*options) << "'\n";
                for (const auto& [name, value] : snapshot_properties(mode)) out << name << " = " << value << "\n";
            }
            out << "\n== WorldSettings ==\n";
            for (const auto& [name, value] : snapshot_properties(object_named("world_settings"))) out << name << " = " << value << "\n";
        }

        std::vector<const Seen*> ordered;
        for (const auto& [function, seen] : seen_copy) ordered.push_back(&seen);
        std::sort(ordered.begin(), ordered.end(), [](const Seen* a, const Seen* b) { return a->sequence < b->sequence; });
        auto phase = [&](const Seen* s) { return (!changed || s->sequence < sequence_at_change) ? "[B]" : "[A]"; };

        out << "\n== Functions that look relevant (first-seen order; [B] = before the level changed, [A] = after) ==\n";
        for (const Seen* s : ordered)
        {
            if (!interesting(s->owner + "::" + s->name) || s->count > 5000) continue;
            out << std::format("{} {:>8.0f} ms  x{:<6} {}::{}\n", phase(s), s->first_ms, s->count, s->owner, s->name);
        }
        out << "\n== Every function seen (first-seen order) ==\n";
        for (const Seen* s : ordered) out << std::format("{} {:>8.0f} ms  x{:<6} {}::{}\n", phase(s), s->first_ms, s->count, s->owner, s->name);
    }
} // namespace RC::Toolkit
