#pragma once

// EngineCompat: describes which Unreal Engine the host game is running and which engine-level
// features that implies. Everything version-dependent in the toolkit asks the profile, never
// hard-codes a version number.
//
// Detection order:
//   1. UE4SS' own detection (RC::Unreal::Version, filled in by patternsleuth / the settings override).
//   2. KismetSystemLibrary::GetEngineVersion() read through reflection. This adds the patch level and the
//      build branch ("++UE4+Release-4.27") and lets us warn when the two sources disagree.

#include <cstdint>
#include <Common.hpp>
#include <string>

namespace RC::Compat
{
    enum class EngineFamily : uint8_t
    {
        Unknown,
        UE4Legacy, // < 4.25: UProperty hierarchy
        UE4,       // 4.25 - 4.27: FField property hierarchy, float vectors
        UE5,       // 5.x: large world coordinates (double vectors), TObjectPtr
    };

    struct RC_UE4SS_API EngineProfile
    {
        int32_t major{};
        int32_t minor{};
        int32_t patch{-1};          // -1 when the engine version string could not be read
        std::wstring release_string; // e.g. L"4.27.2-0+++UE4+Release-4.27"
        std::wstring branch;         // e.g. L"++UE4+Release-4.27"
        bool debug_build{};
        bool version_sources_disagree{};

        [[nodiscard]] auto family() const -> EngineFamily;
        [[nodiscard]] auto is_at_least(int32_t maj, int32_t min) const -> bool { return major > maj || (major == maj && minor >= min); }
        [[nodiscard]] auto is_below(int32_t maj, int32_t min) const -> bool { return !is_at_least(maj, min); }

        // Feature flags. These are the questions the toolkit actually needs answered.
        [[nodiscard]] auto uses_ffield() const -> bool { return is_at_least(4, 25); }
        [[nodiscard]] auto large_world_coordinates() const -> bool { return major >= 5; }
        [[nodiscard]] auto uses_tobjectptr() const -> bool { return major >= 5; }
        [[nodiscard]] auto has_sparse_class_data() const -> bool { return is_at_least(4, 26); }
        // FAssetData::AssetClass became deprecated in favour of AssetClassPath in 5.1.
        [[nodiscard]] auto asset_data_uses_class_path() const -> bool { return is_at_least(5, 1); }

        [[nodiscard]] auto to_string() const -> std::wstring;

        // Lazily detected on first use and cached. Call after Unreal has initialised.
        static auto Get() -> const EngineProfile&;
        // Drop the cache (used by tests and when the version override changes).
        static auto Redetect() -> const EngineProfile&;
    };
} // namespace RC::Compat
