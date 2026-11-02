#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <Unreal/NameTypes.hpp>

namespace RC::Toolkit
{
    // Per-game protection against level content that freezes the game on load.
    //
    // Some Voices of the Void levels contain an outdated `propProcessor_C` ("propRenderer") actor that, on BeginPlay,
    // spawns `cube` props without end; physics slows to a crawl and the game looks frozen. A guard names a class and an
    // event: while guards are enabled, that event is skipped for objects of that class (including subclasses).
    // Guards are listed in Mods/ToolkitMod/level_notes.json under "guards" ({class_name, function, reason}) and can be switched off in the Levels tab.
    class LevelGuards
    {
      public:
        struct Guard
        {
            Unreal::FName class_name{};
            Unreal::FName function{};
            std::string text_class{};
            std::string text_function{};
            std::string reason{};
        };

        static auto get() -> LevelGuards&;
        auto install() -> void;
        auto load(const std::string& path) -> void;

        [[nodiscard]] auto blocked_count() const -> uint64_t { return m_blocked.load(); }
        [[nodiscard]] auto guards() const -> const std::vector<Guard>& { return m_guards; }

      private:
        LevelGuards() = default;
        auto matches_and_count(Unreal::UObject* context, Unreal::UFunction* function) -> bool;
        std::vector<Guard> m_guards;
        std::atomic<uint64_t> m_blocked{0};
        bool m_installed{false};
    };
} // namespace RC::Toolkit
