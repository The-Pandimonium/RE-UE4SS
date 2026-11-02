#include <LevelGuards.hpp>

#include <glaze/glaze.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Session.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;

    // Entries of the "guards" array in level_notes.json. (Not in an anonymous namespace: glaze cannot reflect those.)
    struct GuardEntry
    {
        std::string class_name{};
        std::string function{};
        std::string reason{};
    };
    struct GuardFile
    {
        std::vector<GuardEntry> guards{};
    };

    auto LevelGuards::get() -> LevelGuards&
    {
        static LevelGuards instance;
        return instance;
    }

    auto LevelGuards::load(const std::string& path) -> void
    {
        GuardFile file;
        if (glz::read_file_json<glz::opts{.error_on_unknown_keys = false}>(file, path, std::string{})) return;
        std::vector<Guard> loaded;
        for (const auto& e : file.guards)
        {
            if (e.class_name.empty() || e.function.empty()) continue;
            Guard g;
            g.text_class = e.class_name;
            g.text_function = e.function;
            g.reason = e.reason;
            loaded.push_back(std::move(g));
        }
        m_guards = std::move(loaded);
        Output::send<LogLevel::Normal>(STR("[ToolkitMod] Loaded {} level guard(s) from {}\n"), m_guards.size(), ensure_str(path));
    }

    // True (and counted/logged) when `function` on `context` is one of the guarded events.
    auto LevelGuards::matches_and_count(UObject* context, UFunction* function) -> bool
    {
        for (const Guard& g : m_guards)
        {
            if (function->GetNamePrivate() != g.function) continue;
            for (UStruct* s = context->GetClassPrivate(); s; s = s->GetSuperStruct())
            {
                if (s->GetNamePrivate() != g.class_name) continue;
                if (m_blocked.fetch_add(1, std::memory_order_relaxed) < 6)
                    Output::send<LogLevel::Normal>(STR("[ToolkitMod] Level guard: skipped {}::{} ({})\n"), ensure_str(g.text_class), ensure_str(g.text_function), ensure_str(g.reason));
                return true;
            }
        }
        return false;
    }

    auto LevelGuards::install() -> void
    {
        if (m_installed) return;
        m_installed = true;
        // No guards configured: add no hooks at all, so the feature costs nothing.
        if (m_guards.empty()) return;

        // FNames can only be created once Unreal is initialised, so not while the mod is being constructed.
        for (Guard& g : m_guards)
        {
            g.class_name = FName(ensure_str(g.text_class).c_str());
            g.function = FName(ensure_str(g.text_function).c_str());
        }

        // Events that go through ProcessEvent (native-to-Blueprint calls such as BeginPlay and Tick).
        Hook::RegisterProcessEventPreCallback(
                [this](Hook::TCallbackIterationData<void>& data, UObject* context, UFunction* function, void*) {
                    if (!function || !context || !Session::get().level_guards_enabled()) return;
                    if (matches_and_count(context, function)) data.PreventOriginalFunctionCall();
                },
                Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("LevelGuards")});

        // Blueprint-to-Blueprint calls never go through ProcessEvent, so guard those as well.
        auto script_guard = [this](Hook::TCallbackIterationData<void>& data, UObject* context, FFrame& stack, void*) {
            UFunction* function = stack.Node();
            if (!function || !context || !Session::get().level_guards_enabled()) return;
            if (matches_and_count(context, function)) data.PreventOriginalFunctionCall();
        };
        Hook::RegisterProcessLocalScriptFunctionPreCallback(script_guard, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("LevelGuardsVM")});
        Hook::RegisterProcessInternalPreCallback(script_guard, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("LevelGuardsInternal")});
    }
} // namespace RC::Toolkit
