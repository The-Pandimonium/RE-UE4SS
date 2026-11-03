#pragma once

#include <atomic>
#include <string>

namespace RC::Toolkit
{
    // Some games open a level through their own loader instead of the engine's `open`. Voices of the Void's menu, boot screen
    // and every other travel call `lib_C::loadLevel(Level, Option, <world context>)` with the same constant `Option` token; a
    // plain `open` passes none, and then most maps run an endless prop spawner (the game looks frozen). So the recipe is just:
    // call that loader the way the game does. Found with the travel recorder; nothing else (no menu, no save slot) is needed.
    //
    //   "launch": {
    //     "applies_if_class": "mainGameInstance_C",        // recipe only used when this class exists
    //     "level_class": "lib_C", "level_function": "loadLevel", "level_param": "Level", "option_param": "Option",
    //     "option_property": "NewVar_1",                   // GameInstance property where the game keeps the token (read live)
    //     "option_default": "PQXYyeofZ8cr5rJD4YXLVw"      // used when that property is still empty (e.g. on the boot screen)
    //   }
    struct LaunchRecipe
    {
        std::string description{};
        std::string applies_if_class{};
        std::string level_class{"lib_C"};
        std::string level_function{"loadLevel"};
        std::string level_param{"Level"};
        std::string option_param{"Option"};
        std::string option_property{};
        std::string option_default{};
    };

    class LevelLauncher
    {
      public:
        static auto get() -> LevelLauncher&;
        auto load(const std::string& path) -> void;

        // Game thread. True when a recipe exists and its trigger class is present in this game.
        [[nodiscard]] auto available() const -> bool;
        [[nodiscard]] auto description() const -> const std::string& { return m_recipe.description; }
        // Game thread. Opens `package_path` (a /Game/... map path or a short level name) through the game's own loader.
        // Works from any level that has a game mode, and from the boot screen. `error` explains a failure.
        auto open(const std::string& package_path, std::string& error) -> bool;

        std::atomic<bool> enabled{true}; // the Levels tab can switch the recipe off to compare with a plain `open`

      private:
        LevelLauncher() = default;
        LaunchRecipe m_recipe{};
        bool m_loaded{false};
    };
} // namespace RC::Toolkit
