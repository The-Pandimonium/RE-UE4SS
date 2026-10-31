#pragma once

#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace RC::Toolkit
{
    constexpr const char* kGlobalLevel = "*";

    struct Waypoint
    {
        int id{};
        std::string name{};
        std::string level{kGlobalLevel}; // short level name the waypoint belongs to, or "*" for every level
        double x{}, y{}, z{};
        double pitch{}, yaw{}, roll{};
        bool visible{false};              // draw a marker in the game world
        std::array<float, 3> color{1.0f, 0.78f, 0.2f};
        std::string note{};
    };

    struct WaypointFile
    {
        int next_id{1};
        std::vector<Waypoint> waypoints{};
    };

    // Persistent waypoint list (JSON). All access is thread-safe; the game thread reads it every frame for the
    // in-game markers while the UI thread edits it.
    class WaypointStore
    {
      public:
        static auto get() -> WaypointStore&;

        auto set_path(std::filesystem::path path) -> void { m_path = std::move(path); }
        auto load() -> bool;
        auto save() const -> bool;

        auto add(Waypoint waypoint) -> int; // returns the new id
        auto remove(int id) -> bool;
        auto update(const Waypoint& waypoint) -> bool;
        auto find(int id) const -> std::optional<Waypoint>;
        auto set_visible(int id, bool visible) -> void;

        // Waypoints that apply to `level` (its own plus the global ones).
        auto for_level(const std::string& level) const -> std::vector<Waypoint>;
        auto all() const -> std::vector<Waypoint>;
        auto any_visible() const -> bool;

      private:
        mutable std::mutex m_mutex;
        std::filesystem::path m_path;
        WaypointFile m_data;
    };
} // namespace RC::Toolkit
