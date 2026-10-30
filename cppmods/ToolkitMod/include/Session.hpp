#pragma once

// Session: the game-thread-updated view of "what is the player doing right now" plus the actions the
// menus share (teleport, back, open level). The UI thread reads snapshots; every action is forwarded to the
// game thread through EngineCompat::GameThread.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <EngineCompat/Adapter.hpp>

namespace RC::Toolkit
{
    using Compat::Rot3;
    using Compat::Vec3;

    struct SessionSnapshot
    {
        bool has_world{false};
        bool has_pawn{false};
        std::string level{};      // short level name
        std::string pawn_name{};
        std::string pawn_class{};
        Vec3 position{};
        Rot3 rotation{};
        uint64_t updated_tick{0};
    };

    struct ToolkitConfig
    {
        bool hud_markers{true};         // draw visible waypoints in the game world
        bool hotkeys{true};             // F6 quick waypoint, F7 back
        double marker_max_distance{0};  // 0 = unlimited
        bool selftest{false};           // run the in-game self test once a pawn exists
        bool selftest_quit{false};      // close the game when the self test finishes
        std::string selftest_level{};   // level to open first when starting in a menu (empty = stay)
    };

    class Session
    {
      public:
        static auto get() -> Session&;

        auto set_root(std::filesystem::path root) -> void { m_root = std::move(root); }
        [[nodiscard]] auto root() const -> const std::filesystem::path& { return m_root; }

        auto install() -> void;
        auto load_config() -> void;
        auto save_config() const -> void;
        [[nodiscard]] auto config() const -> ToolkitConfig;
        auto update_config(const ToolkitConfig& config) -> void;

        [[nodiscard]] auto snapshot() const -> SessionSnapshot;

        // ---- Actions (any thread) ----
        // Teleports the player pawn. The previous position is remembered for `teleport_back`.
        auto teleport_player(Vec3 position, std::optional<Rot3> rotation = std::nullopt) -> void;
        auto teleport_back() -> void;
        [[nodiscard]] auto has_back_position() const -> bool;
        auto open_level(std::string package_path, std::string options = {}) -> void;

        // Engine description captured on the game thread at startup (profile + adapter), for the Engine tab.
        auto set_engine_info(std::string profile, std::string adapter) -> void;
        [[nodiscard]] auto engine_info() const -> std::pair<std::string, std::string>;

        // Last result of an action, for display.
        [[nodiscard]] auto last_message() const -> std::string;
        auto set_message(std::string message) -> void;

        // ---- Game thread helpers ----
        // Re-reads the world right now (used by tests and by actions that need fresh data).
        auto refresh_now() -> SessionSnapshot;

      private:
        Session() = default;
        auto tick(float dt) -> void;

        std::filesystem::path m_root;
        mutable std::mutex m_mutex;
        SessionSnapshot m_snapshot;
        ToolkitConfig m_config;
        std::vector<std::pair<Vec3, Rot3>> m_back_stack;
        std::string m_message;
        std::string m_engine_profile;
        std::string m_engine_adapter;
        double m_since_update{1.0};
        bool m_installed{false};
    };
} // namespace RC::Toolkit
