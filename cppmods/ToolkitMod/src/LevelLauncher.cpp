#include <LevelLauncher.hpp>

#include <algorithm>
#include <cstdlib>

#include <glaze/glaze.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;

    // The recipe sits under "launch" in level_notes.json, next to the entries LevelsUI reads. (Not in an anonymous namespace: glaze cannot reflect those.)
    struct LaunchFile
    {
        LaunchRecipe launch{};
    };

    namespace
    {
        auto unquote(std::string text) -> std::string
        {
            text.erase(std::remove(text.begin(), text.end(), '"'), text.end());
            return text;
        }

        // Blueprint library functions take a hidden world-context object (the game mode is as good as any). Left empty,
        // they cannot find the game mode and quietly do nothing.
        auto fill_world_context(Reflect::Call& call, UFunction* function) -> void
        {
            if (!function) return;
            UObject* context = nullptr;
            for (FProperty* p : TFieldRange<FProperty>(function))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm) || !CastField<FObjectPropertyBase>(p)) continue;
                const std::wstring name = p->GetName();
                if (!name.starts_with(L"__WorldContext") && name != L"WorldContextObject" && name != L"WorldContext") continue;
                if (!context)
                {
                    auto found = resolve_targets("game_mode", "first", 0);
                    if (found.empty()) found = resolve_targets("world", "first", 0);
                    if (found.empty()) return;
                    context = found.front();
                }
                call.SetObject(name.c_str(), context);
            }
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
        m_loaded = !m_recipe.level_function.empty() && !m_recipe.level_class.empty();
        if (m_loaded) Output::send<LogLevel::Normal>(STR("[ToolkitMod] Loaded level launch recipe ({}::{})\n"), ensure_str(m_recipe.level_class), ensure_str(m_recipe.level_function));
    }

    auto LevelLauncher::available() const -> bool
    {
        if (!m_loaded || !enabled) return false;
        return m_recipe.applies_if_class.empty() || Reflect::FindClass(ensure_str(m_recipe.applies_if_class)) != nullptr;
    }

    auto LevelLauncher::open(const std::string& package_path, std::string& error) -> bool
    {
        // "/Game/maps/sandbox/flatgrass" -> "flatgrass": the game's own callers pass the short name.
        const auto cut = package_path.find_last_of("/.");
        const std::string level = package_path.substr(cut == std::string::npos ? 0 : cut + 1);

        // The token: what the game itself keeps after its first load, else the recorded constant.
        std::string option;
        if (const char* forced = std::getenv("TOOLKIT_OPTION")) option = forced; // experiments only
        else
        {
            if (!m_recipe.option_property.empty())
            {
                auto instance = resolve_targets("game_instance", "first", 0);
                if (!instance.empty())
                {
                    if (auto value = Reflect::GetPropertyText(instance.front(), ensure_str(m_recipe.option_property))) option = unquote(to_string(*value));
                }
            }
            if (option.empty() || option == "None") option = m_recipe.option_default;
        }

        Reflect::Call call = Reflect::Call::Static(ensure_str(m_recipe.level_class), ensure_str(m_recipe.level_function));
        if (!call.valid()) { error = "level loader " + m_recipe.level_class + "::" + m_recipe.level_function + " not found"; return false; }
        call.SetText(ensure_str(m_recipe.level_param).c_str(), ensure_str(level));
        if (!m_recipe.option_param.empty()) call.SetText(ensure_str(m_recipe.option_param).c_str(), ensure_str(option));
        fill_world_context(call, call.function());
        if (!call.Invoke()) { error = "calling the game's level loader failed"; return false; }
        return true;
    }
} // namespace RC::Toolkit
