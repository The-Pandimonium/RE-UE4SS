#pragma once

#include <atomic>
#include <cstdint>
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

      private:
        WaypointRenderer() = default;
        auto draw(Unreal::UObject* hud) -> void;
        auto tick(float dt) -> void;

        std::atomic<uint64_t> m_hud_frames{0};
        std::atomic<uint64_t> m_markers_drawn{0};
        std::atomic<uint64_t> m_draw_failures{0};
        std::atomic<bool> m_hud_created{false};
        mutable std::mutex m_status_mutex;
        std::string m_status{"no player controller yet"};
        double m_since_check{0.0};
        bool m_installed{false};
        bool m_in_draw{false};
    };
} // namespace RC::Toolkit
