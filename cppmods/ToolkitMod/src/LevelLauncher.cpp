#include <LevelLauncher.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <glaze/glaze.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Session.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/Property/FNameProperty.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;
    using RC::Compat::GameThread;

    // The recipe sits under "launch" in level_notes.json, next to the entries LevelsUI reads. (Not in an anonymous namespace: glaze cannot reflect those.)
    struct LaunchFile
    {
        LaunchRecipe launch{};
    };

    namespace
    {
        auto lower(std::string text) -> std::string
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        auto unquote(std::string text) -> std::string
        {
            text.erase(std::remove(text.begin(), text.end(), '"'), text.end());
            return text;
        }

        auto expand_environment(const std::string& path) -> std::filesystem::path
        {
            const auto start = path.find('%');
            const auto end = start == std::string::npos ? std::string::npos : path.find('%', start + 1);
            if (end == std::string::npos) return path;
            const std::string name = path.substr(start + 1, end - start - 1);
            const char* value = std::getenv(name.c_str());
            return std::filesystem::path((value ? std::string(value) : std::string{}) + path.substr(end + 1));
        }

        auto first_object(const std::string& target) -> UObject*
        {
            auto found = resolve_targets(target, "first", 0);
            return found.empty() ? nullptr : found.front();
        }

        auto call_function(UObject* object, const std::string& function, const std::string& object_param, UObject* argument, std::string& error) -> bool
        {
            UFunction* fn = Reflect::FindFunction(object, ensure_str(function));
            if (!fn) { error = "function '" + function + "' not found on " + to_string(object->GetClassPrivate()->GetName()); return false; }
            Reflect::Call call(object, fn);
            if (!object_param.empty()) call.SetObject(ensure_str(object_param).c_str(), argument);
            if (!call.Invoke()) { error = "calling '" + function + "' failed"; return false; }
            return true;
        }
    } // namespace

    auto LevelLauncher::get() -> LevelLauncher&
    {
        static LevelLauncher instance;
        return instance;
    }

    auto LevelLauncher::load(const std::string& path) -> void
    {
        LaunchFile file;
        if (glz::read_file_json<glz::opts{.error_on_unknown_keys = false}>(file, path, std::string{})) return;
        m_recipe = std::move(file.launch);
        m_loaded = !m_recipe.apply_function.empty() && !m_recipe.open_function.empty();
        if (m_loaded) Output::send<LogLevel::Normal>(STR("[ToolkitMod] Loaded level launch recipe ({})\n"), ensure_str(m_recipe.open_function + " -> " + m_recipe.select_function + " -> " + m_recipe.apply_function));
    }

    auto LevelLauncher::install() -> void
    {
        GameThread::AddTick([this](float dt) { tick(dt); }, "level_launcher");
    }

    auto LevelLauncher::available() const -> bool
    {
        if (!m_loaded || !enabled) return false;
        return m_recipe.applies_if_class.empty() || Reflect::FindClass(ensure_str(m_recipe.applies_if_class)) != nullptr;
    }

    auto LevelLauncher::fail(const std::string& message) -> void
    {
        m_phase = Phase::Idle;
        m_override_active = false;
        Output::send<LogLevel::Warning>(STR("[ToolkitMod] Level launch recipe failed: {}\n"), ensure_str(message));
        Session::get().set_message("Could not open " + m_level + ": " + message);
    }

    // Works on a copy of a slot folder, so the player's own saves are never opened for writing by the toolkit's levels.
    auto LevelLauncher::copy_slot_folder(std::string& error) -> bool
    {
        if (m_recipe.copy_from_folder.empty() || m_recipe.work_folder.empty() || m_recipe.save_root.empty()) return true;
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = expand_environment(m_recipe.save_root);
        const fs::path source = root / m_recipe.copy_from_folder;
        const fs::path target = root / m_recipe.work_folder;
        if (fs::exists(target, ec)) return true; // keep whatever progress the copy already has
        if (!fs::exists(source, ec)) { error = "save folder '" + source.string() + "' does not exist (start the game once from its menu first)"; return false; }
        fs::copy(source, target, fs::copy_options::recursive, ec);
        if (ec) { error = "could not copy the save folder: " + ec.message(); return false; }
        Output::send<LogLevel::Normal>(STR("[ToolkitMod] Copied save folder {} to {}\n"), ensure_str(source.string()), ensure_str(target.string()));
        return true;
    }

    auto LevelLauncher::open(const std::string& package_path, std::string& error) -> bool
    {
        if (m_phase != Phase::Idle) { error = "the previous level request is still running"; return false; }
        const auto cut = package_path.find_last_of("/.");
        m_level = package_path.substr(cut == std::string::npos ? 0 : cut + 1); // the menu passes the short name

        UObject* menu = first_object(m_recipe.menu_target);
        if (!menu) { error = "the game's menu is not showing: go to the main menu first"; return false; }
        if (!copy_slot_folder(error)) return false;

        if (!m_hooked)
        {
            m_hooked = true;
            m_level_function_name = FName(ensure_str(m_recipe.level_function).c_str());
            // The level the game is told to load is swapped for ours at the moment its loader runs; everything else it
            // does, including the save slot it has just prepared, stays exactly as the menu would have left it.
            auto swap_level = [this](Hook::TCallbackIterationData<void>&, UObject*, FFrame& stack, void*) {
                if (!m_override_active.load(std::memory_order_relaxed)) return;
                UFunction* fn = stack.Node();
                if (!fn || fn->GetNamePrivate() != m_level_function_name || !stack.Locals()) return;
                for (FProperty* p : TFieldRange<FProperty>(fn))
                {
                    if (!p->HasAnyPropertyFlags(CPF_Parm) || p->GetName() != ensure_str(m_recipe.level_param)) continue;
                    if (auto* name_property = CastField<FNameProperty>(p))
                        name_property->SetPropertyValue(stack.Locals() + p->GetOffset_Internal(), FName(ensure_str(m_level).c_str()));
                }
            };
            Hook::RegisterProcessLocalScriptFunctionPreCallback(swap_level, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("LevelLauncherVM")});
            Hook::RegisterProcessInternalPreCallback(swap_level, Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("LevelLauncherInternal")});
        }

        m_menu_name = to_string(menu->GetName());
        m_dumped = false;
        if (!call_function(menu, m_recipe.open_function, {}, nullptr, error)) return false;
        m_phase = Phase::WaitList;
        m_timer = 0.0;
        return true;
    }

    // True when the widget lives inside the menu instance this request started on.
    auto LevelLauncher::in_menu(UObject* widget) const -> bool
    {
        return widget && !m_menu_name.empty() && to_string(widget->GetFullName()).find(m_menu_name + ".") != std::string::npos;
    }

    // The save slot screen of that same menu instance (there can be an older one in memory, with nothing selected).
    auto LevelLauncher::slot_screen() const -> UObject*
    {
        for (UObject* screen : resolve_targets(m_recipe.slots_target, "all", 0))
        {
            if (in_menu(screen)) return screen;
        }
        return nullptr;
    }

    // Leaves a description of what the slot screen contains, to adjust a recipe from.
    auto LevelLauncher::dump_slot_screen() -> void
    {
        std::ofstream dump(Session::get().root() / "launch_debug.txt", std::ios::trunc);
        for (const std::string& cls : {m_recipe.folder_class, std::string("uicomp_saveSlot_C"), std::string("ui_saveSlots_C")})
        {
            auto objects = resolve_targets("class:" + cls, "all", 0);
            dump << "== " << cls << ": " << objects.size() << " object(s)\n";
            int shown = 0;
            for (UObject* object : objects)
            {
                if (++shown > 6) break;
                dump << "-- " << to_string(object->GetFullName()) << "\n";
                for (FProperty* p : TFieldRange<FProperty>(object->GetClassPrivate()))
                {
                    std::string value;
                    if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(object), object)) value = to_string(*text);
                    if (value.size() > 160) value.resize(160);
                    dump << "   " << to_string(p->GetName()) << " = " << value << "\n";
                }
            }
        }
    }

    auto LevelLauncher::tick(float dt) -> void
    {
        if (m_phase == Phase::Idle) return;
        m_timer += dt;
        std::string error;

        if (m_phase == Phase::WaitList && m_timer >= 1.0)
        {
            // Pick the slot entry of the working folder, as a click on it would: find the folder's widget by its path, then
            // take the entry it holds.
            const std::string folder = m_recipe.work_folder.empty() ? m_recipe.copy_from_folder : m_recipe.work_folder;
            UObject* chosen = nullptr;
            for (UObject* widget : resolve_targets("class:" + m_recipe.folder_class, "all", 0))
            {
                if (!in_menu(widget)) continue; // an older menu instance may still be in memory
                auto path = Reflect::GetPropertyText(widget, ensure_str(m_recipe.folder_path_property));
                if (!path) continue;
                std::string text = lower(unquote(to_string(*path)));
                std::replace(text.begin(), text.end(), '\\', '/');
                while (!text.empty() && text.back() == '/') text.pop_back();
                if (text.empty() || text.substr(text.find_last_of('/') + 1) != lower(folder)) continue;
                chosen = Reflect::GetObjectProperty(widget, ensure_str(m_recipe.entry_property));
                break;
            }
            UObject* slots = slot_screen();
            if (!chosen) dump_slot_screen();
            if (!chosen || !slots) return fail(!slots ? "the save slot screen did not appear" : "no save folder '" + folder + "' in the slot list");
            // With no select_param the function is the entry's own click handler (it also sets up the rules panel that Apply needs).
            const bool on_entry = m_recipe.select_param.empty();
            if (!call_function(on_entry ? chosen : slots, m_recipe.select_function, m_recipe.select_param, chosen, error)) return fail(error);
            m_phase = Phase::WaitApply;
            m_timer = 0.0;
        }
        else if (m_phase == Phase::WaitApply && m_timer >= 1.5)
        {
            UObject* slots = slot_screen();
            if (!slots) return fail("the save slot screen went away");
            m_override_active = true;
            if (!call_function(slots, m_recipe.apply_function, {}, nullptr, error)) return fail(error);
            Output::send<LogLevel::Normal>(STR("[ToolkitMod] Level launch recipe: applied slot, loading {}\n"), ensure_str(m_level));
            m_phase = Phase::WaitDone;
            m_timer = 0.0;
        }
        else if (m_phase == Phase::WaitDone && m_timer >= 3.0 && !m_dumped)
        {
            m_dumped = true; // state of the slot screen after Apply, for diagnosing a launch that did not start
            dump_slot_screen();
        }
        else if (m_phase == Phase::WaitDone && m_timer >= 8.0)
        {
            m_override_active = false;
            m_phase = Phase::Idle;
        }
    }
} // namespace RC::Toolkit

namespace RC::Toolkit
{
    auto LevelLauncher::menu_showing() const -> bool
    {
        return first_object(m_recipe.menu_target) != nullptr;
    }
} // namespace RC::Toolkit
