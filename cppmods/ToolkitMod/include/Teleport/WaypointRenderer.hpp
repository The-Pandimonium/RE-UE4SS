#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <Unreal/UObject.hpp>

namespace RC::Toolkit
{
    // Draws waypoints whose "visible" flag is set directly into the game's view.
    //
    // Shipping builds compile out the DrawDebug* helpers, so markers are painted through the game's own HUD:
    // a pre-hook on AHUD::ReceiveDrawHUD (which the engine calls every frame while the canvas is valid) uses the
    // HUD's Project/DrawRect/DrawLine/DrawText functions. Those are plain gameplay-module functions that exist in
    // every engine version, and Reflect::Call takes care of float-vs-double parameter layouts.
    //
    // Some games configure no HUD class at all, so the player controller never owns an AHUD and nothing would ever
    // be drawn. When markers are wanted but there is no HUD, one is created with APlayerController::ClientSetHUD.
    class WaypointRenderer
    {
      public:
        static auto get() -> WaypointRenderer&;
        auto install() -> void;

        // Diagnostics for the UI / self-test.
        [[nodiscard]] auto hud_frames() const -> uint64_t { return m_hud_frames.load(); }
        [[nodiscard]] auto markers_drawn() const -> uint64_t { return m_markers_drawn.load(); }
        [[nodiscard]] auto draw_failures() const -> uint64_t { return m_draw_failures.load(); }
        [[nodiscard]] auto hud_created_by_toolkit() const -> bool { return m_hud_created.load(); }
        [[nodiscard]] auto hud_status() const -> std::string;
        // Marker actors currently alive in the world (the HUD-independent way waypoints are shown).
        [[nodiscard]] auto marker_actor_count() const -> int { return m_marker_count.load(); }
        [[nodiscard]] auto marker_spawn_failures() const -> uint64_t { return m_spawn_failures.load(); }

      private:
        WaypointRenderer() = default;
        auto draw(Unreal::UObject* hud) -> void;
        auto tick(float dt) -> void;
        auto reconcile_markers() -> void;
        auto destroy_marker(int waypoint_id) -> void;
        auto spawn_part(Unreal::UObject* world, const wchar_t* mesh_path, double x, double y, double z, double sx, double sy, double sz,
                        const float (&color)[3]) -> Unreal::UObject*;

        std::atomic<uint64_t> m_hud_frames{0};
        std::atomic<uint64_t> m_markers_drawn{0};
        std::atomic<uint64_t> m_draw_failures{0};
        std::atomic<bool> m_hud_created{false};
        std::atomic<uint64_t> m_behind_camera{0};
        std::atomic<uint64_t> m_project_failed{0};
        double m_since_log{0.0};
        Unreal::UObject* m_last_pc{nullptr};
        double m_stable_seconds{0.0};
        int m_missing_hud_checks{0};
        struct Marker
        {
            Unreal::UObject* beam{};
            Unreal::UObject* ball{};
            std::string signature{};
        };
        std::map<int, Marker> m_markers; // game thread only
        Unreal::UObject* m_marker_world{nullptr};
        std::atomic<int> m_marker_count{0};
        std::atomic<uint64_t> m_spawn_failures{0};
        mutable std::mutex m_status_mutex;
        std::string m_status{"no player controller yet"};
        double m_since_check{0.0};
        bool m_installed{false};
        bool m_in_draw{false};
    };
} // namespace RC::Toolkit
