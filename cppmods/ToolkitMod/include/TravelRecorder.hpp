#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Unreal/UObject.hpp>

namespace RC::Toolkit
{
    // Finds out what the game itself does to open a level. Arm it, then use the game's own menu (e.g. Start Game ->
    // Sandbox). Every Blueprint/engine function called is noted (first-seen order, count, time), the GameInstance is compared
    // before and after, and once the world changes the game mode's options string and properties are written, all to
    // Mods/ToolkitMod/travel_trace.txt. That is the recipe the level switcher has to repeat.
    class TravelRecorder
    {
      public:
        static auto get() -> TravelRecorder&;
        auto install() -> void; // game thread, after Unreal init
        auto arm() -> void;     // any thread
        auto disarm() -> void;
        [[nodiscard]] auto armed() const -> bool { return m_armed.load(); }
        [[nodiscard]] auto status() const -> std::string;

      private:
        TravelRecorder() = default;
        struct Seen
        {
            std::string owner{};
            std::string name{};
            uint64_t count{0};
            uint64_t sequence{0};
            double first_ms{0};
        };
        auto note(Unreal::UObject* context, Unreal::UFunction* function, void* params, bool after) -> void;
        auto tick(float dt) -> void;
        auto write_trace(const std::string& stage) -> void;
        auto begin_segment() -> void;
        auto elapsed_ms() const -> double;

        std::atomic<bool> m_armed{false};
        bool m_hooks{false};
        mutable std::mutex m_mutex;
        std::unordered_map<Unreal::UFunction*, Seen> m_seen;
        std::vector<std::string> m_travel_calls; // arguments of OpenLevel and friends, as they were called
        std::map<std::string, std::string> m_instance_before;
        std::string m_level_before;
        std::string m_level_after;
        std::string m_status{"idle"};
        uint64_t m_next_sequence{0};
        uint64_t m_sequence_at_change{0};
        bool m_changed{false};
        bool m_requested_arm{false};
        bool m_opened_test_level{false};
        double m_test_opened_at{0};
        double m_menu_since{0};
        double m_changed_at{0};
        double m_clock{0};
        std::chrono::steady_clock::time_point m_started{};
    };
} // namespace RC::Toolkit
