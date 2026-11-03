#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

namespace RC::Toolkit
{
    // Some games do more than load a map when you start a level from their menu. Voices of the Void builds a save slot first
    // (real save objects, a slot and folder name, a random token); handed none, or an empty one, most maps spawn props
    // without end. A launch recipe in level_notes.json tells the toolkit how to drive the game's OWN menu code to do that:
    //
    //   "launch": {
    //     "applies_if_class": "mainGameInstance_C",
    //     "menu_target": "class:ui_menu_C",       "open_function": "openSandbox",     // shows the slot list
    //     "slots_target": "class:ui_saveSlots_C", "select_function": "selectSlot_sub", "select_param": "slotSub",
    //     "apply_function": "BndEvt__button_apply_...",                                 // what the Apply button runs
    //     "folder_class": "uicomp_saveSlotFolder_C", "folder_path_property": "Path", "entry_property": "slotMain",
    //     "level_function": "loadLevel", "level_param": "Level",                        // the level argument is replaced
    //     "save_root": "%LOCALAPPDATA%/VotV/Saved/SaveGames", "copy_from_folder": "first", "work_folder": "toolkit_sandbox"
    //   }
    //
    // The slot folder `copy_from_folder` is copied to `work_folder` and that copy is used, so the player's own saves are
    // never touched. The game's loader then runs exactly as it does for the menu; only the level it is told to load differs.
    struct LaunchRecipe
    {
        std::string description{};
        std::string applies_if_class{};
        std::string menu_target{"class:ui_menu_C"};
        std::string open_function{};
        std::string slots_target{};
        std::string select_function{};
        std::string select_param{};
        std::string apply_function{};
        std::string folder_class{};         // widget class that represents one save folder in the list
        std::string folder_path_property{}; // its property holding the folder path
        std::string entry_property{};       // its object property holding the slot entry to select (e.g. the main slot)
        std::string level_function{"loadLevel"};
        std::string level_param{"Level"};
        std::string save_root{};
        std::string copy_from_folder{};
        std::string work_folder{};
    };

    class LevelLauncher
    {
      public:
        static auto get() -> LevelLauncher&;
        auto load(const std::string& path) -> void;
        auto install() -> void; // game thread, after Unreal init

        // Game thread. True when a recipe exists and its trigger class is present in this game.
        [[nodiscard]] auto available() const -> bool;
        // Game thread. True when the game's menu is on screen, which the recipe needs (it drives that menu).
        [[nodiscard]] auto menu_showing() const -> bool;
        [[nodiscard]] auto description() const -> const std::string& { return m_recipe.description; }
        // Game thread. Starts the recipe for `package_path` (a /Game/... map path). The rest runs over the next frames;
        // problems are reported through the Levels tab message. `error` explains an immediate failure.
        auto open(const std::string& package_path, std::string& error) -> bool;

        std::atomic<bool> enabled{true}; // the Levels tab can switch the recipe off to compare with a plain `open`

      private:
        LevelLauncher() = default;
        enum class Phase { Idle, WaitList, WaitApply, WaitDone };
        auto tick(float dt) -> void;
        auto fail(const std::string& message) -> void;
        auto copy_slot_folder(std::string& error) -> bool;
        auto dump_slot_screen() -> void;
        [[nodiscard]] auto in_menu(Unreal::UObject* widget) const -> bool;
        [[nodiscard]] auto slot_screen() const -> Unreal::UObject*;

        LaunchRecipe m_recipe{};
        bool m_loaded{false};
        bool m_hooked{false};
        Phase m_phase{Phase::Idle};
        double m_timer{0};
        std::string m_level;                    // short level name to load
        bool m_dumped{false};
        std::string m_menu_name;                // the menu instance the request started on; its widgets are the ones to use
        std::atomic<bool> m_override_active{false};
        Unreal::FName m_level_function_name{};  // parsed once Unreal is up
    };
} // namespace RC::Toolkit
