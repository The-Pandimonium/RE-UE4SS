#pragma once

// EngineCompat::IEngineAdapter - the engine-version abstraction layer used by the inspectors and the toolkit.
//
// An adapter answers game-level questions ("where is the player pawn?", "teleport this actor", "which
// levels exist?") without the caller knowing which Unreal Engine version it is running on. The default
// GenericAdapter implements everything through reflection (see Reflect.hpp), which already hides most
// layout differences (float vs double vectors, renamed parameters, FField vs UProperty). Family-specific
// adapters override only what genuinely differs, and mods can register their own adapters for engine
// forks or unusual versions.
//
// Threading: every method may execute game code through ProcessEvent, so call them on the game thread
// (see GameThread.hpp). Methods are safe to call with null objects and return empty results instead of throwing.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Common.hpp>
#include <EngineCompat/EngineProfile.hpp>
#include <EngineCompat/Reflect.hpp>

namespace RC::Compat
{
    using Reflect::Rot3;
    using Reflect::Vec2;
    using Reflect::Vec3;
    using Unreal::UObject;

    struct LevelInfo
    {
        StringType name;         // short name, e.g. L"Main_Menu"
        StringType package_path; // e.g. L"/Game/Maps/Main_Menu"
        [[nodiscard]] auto is_game_content() const -> bool { return package_path.starts_with(L"/Game/"); }
    };

    class RC_UE4SS_API IEngineAdapter
    {
      public:
        virtual ~IEngineAdapter() = default;

        [[nodiscard]] virtual auto name() const -> const wchar_t* = 0;
        // When several adapters support a profile, the highest priority wins.
        [[nodiscard]] virtual auto priority() const -> int { return 0; }
        [[nodiscard]] virtual auto supports(const EngineProfile& profile) const -> bool = 0;

        // ---- Session objects ----
        virtual auto world() -> UObject* = 0;
        virtual auto player_controller(int index = 0) -> UObject* = 0;
        virtual auto player_pawn(int index = 0) -> UObject* = 0;
        virtual auto game_mode() -> UObject* = 0;
        virtual auto game_state() -> UObject* = 0;
        virtual auto world_settings() -> UObject* = 0;
        virtual auto hud(UObject* player_controller) -> UObject* = 0;
        virtual auto current_level_name() -> StringType = 0;

        // ---- Actors ----
        virtual auto actor_location(UObject* actor) -> std::optional<Vec3> = 0;
        virtual auto actor_rotation(UObject* actor) -> std::optional<Rot3> = 0;
        // Teleports without sweeping, keeping the actor's current rotation when none is given.
        virtual auto teleport_actor(UObject* actor, const Vec3& location, const std::optional<Rot3>& rotation = std::nullopt) -> bool = 0;
        virtual auto set_control_rotation(UObject* player_controller, const Rot3& rotation) -> bool = 0;
        // Zeroes velocity so a teleport does not carry momentum into the destination.
        virtual auto stop_movement(UObject* actor) -> bool = 0;
        virtual auto actors_of_class(const StringType& class_name) -> std::vector<UObject*> = 0;

        // ---- Levels ----
        virtual auto enumerate_levels() -> std::vector<LevelInfo> = 0;
        virtual auto open_level(const StringType& level, const StringType& options = {}) -> bool = 0;
        virtual auto exec_console(const StringType& command) -> bool = 0;

        // ---- Presentation ----
        // World to viewport pixels (player viewport relative). False when behind the camera / no controller.
        virtual auto project_world_to_screen(UObject* player_controller, const Vec3& world_location, Vec2& out_screen) -> bool = 0;
        virtual auto set_time_dilation(double dilation) -> bool = 0;
    };

    // Default implementation: pure reflection. Works on any engine whose reflection data names the
    // standard gameplay functions, so it doubles as the fallback for versions without a dedicated adapter.
    class RC_UE4SS_API GenericAdapter : public IEngineAdapter
    {
      public:
        [[nodiscard]] auto name() const -> const wchar_t* override { return L"Generic (reflection)"; }
        [[nodiscard]] auto priority() const -> int override { return 0; }
        [[nodiscard]] auto supports(const EngineProfile&) const -> bool override { return true; }

        auto world() -> UObject* override;
        auto player_controller(int index = 0) -> UObject* override;
        auto player_pawn(int index = 0) -> UObject* override;
        auto game_mode() -> UObject* override;
        auto game_state() -> UObject* override;
        auto world_settings() -> UObject* override;
        auto hud(UObject* player_controller) -> UObject* override;
        auto current_level_name() -> StringType override;

        auto actor_location(UObject* actor) -> std::optional<Vec3> override;
        auto actor_rotation(UObject* actor) -> std::optional<Rot3> override;
        auto teleport_actor(UObject* actor, const Vec3& location, const std::optional<Rot3>& rotation = std::nullopt) -> bool override;
        auto set_control_rotation(UObject* player_controller, const Rot3& rotation) -> bool override;
        auto stop_movement(UObject* actor) -> bool override;
        auto actors_of_class(const StringType& class_name) -> std::vector<UObject*> override;

        auto enumerate_levels() -> std::vector<LevelInfo> override;
        auto open_level(const StringType& level, const StringType& options = {}) -> bool override;
        auto exec_console(const StringType& command) -> bool override;

        auto project_world_to_screen(UObject* player_controller, const Vec3& world_location, Vec2& out_screen) -> bool override;
        auto set_time_dilation(double dilation) -> bool override;

      protected:
        // Hooks for subclasses; the generic versions are what UE4 uses.
        virtual auto asset_class_is_world(UObject* asset_data_owner, Unreal::UScriptStruct* asset_data_type, const void* asset_data) -> bool;

        // First PlayerController, cached for a second so per-frame callers do not scan the whole object array.
        auto cached_player_controller() -> UObject*;

      private:
        UObject* m_pc_cache{};
        std::chrono::steady_clock::time_point m_pc_cache_time{};
    };

    // UE 4.x (4.25 - 4.27 tested). FAssetData carries the class as a plain FName in "AssetClass".
    class RC_UE4SS_API Ue4Adapter : public GenericAdapter
    {
      public:
        [[nodiscard]] auto name() const -> const wchar_t* override { return L"Unreal Engine 4.x"; }
        [[nodiscard]] auto priority() const -> int override { return 10; }
        [[nodiscard]] auto supports(const EngineProfile& profile) const -> bool override { return profile.major == 4; }
    };

    // UE 5.x. Same reflection surface; the class of an asset moved to FTopLevelAssetPath ("AssetClassPath")
    // and every vector/rotator became double precision (already handled by Reflect). Not yet tested in a game.
    class RC_UE4SS_API Ue5Adapter : public GenericAdapter
    {
      public:
        [[nodiscard]] auto name() const -> const wchar_t* override { return L"Unreal Engine 5.x"; }
        [[nodiscard]] auto priority() const -> int override { return 10; }
        [[nodiscard]] auto supports(const EngineProfile& profile) const -> bool override { return profile.major >= 5; }
    };

    class RC_UE4SS_API AdapterRegistry
    {
      public:
        // Mods can add adapters for engine forks / future versions. Ownership transfers to the registry.
        static auto Register(std::unique_ptr<IEngineAdapter> adapter) -> void;
        // The best adapter for the detected engine. Never null (falls back to GenericAdapter).
        static auto Get() -> IEngineAdapter&;
        static auto Describe() -> StringType;
        // Forces re-selection (after Register or EngineProfile::Redetect).
        static auto Reselect() -> void;
    };
} // namespace RC::Compat
