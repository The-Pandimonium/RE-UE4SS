#include <EngineCompat/EngineProfile.hpp>
#include <EngineCompat/Reflect.hpp>

#include <cwchar>
#include <format>
#include <mutex>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UnrealVersion.hpp>

namespace RC::Compat
{
    namespace
    {
        std::mutex g_mutex;
        EngineProfile g_profile;
        bool g_detected = false;

        // "4.27.2-0+++UE4+Release-4.27" -> major 4, minor 27, patch 2, branch "++UE4+Release-4.27"
        auto parse_release_string(const std::wstring& text, EngineProfile& out) -> void
        {
            int major = 0, minor = 0, patch = 0;
            if (std::swscanf(text.c_str(), L"%d.%d.%d", &major, &minor, &patch) >= 2)
            {
                out.patch = patch;
                if (out.major == 0)
                {
                    out.major = major;
                    out.minor = minor;
                }
                else if (out.major != major || out.minor != minor)
                {
                    out.version_sources_disagree = true;
                }
            }
            const auto plus = text.find(L'+');
            if (plus != std::wstring::npos) out.branch = text.substr(plus + 1);
        }

        auto detect() -> EngineProfile
        {
            EngineProfile profile;
            profile.major = Unreal::Version::Major;
            profile.minor = Unreal::Version::Minor;
            profile.debug_build = Unreal::Version::DebugBuild;

            auto call = Reflect::Call::Static(L"KismetSystemLibrary", L"GetEngineVersion");
            if (call.valid() && call.Invoke())
            {
                if (auto text = call.GetText(L"ReturnValue"))
                {
                    profile.release_string = *text;
                    parse_release_string(*text, profile);
                }
            }
            if (profile.version_sources_disagree)
            {
                Output::send<LogLevel::Warning>(STR("[EngineCompat] UE4SS reports {}.{} but the engine reports '{}'. Trusting the engine string.\n"),
                                                Unreal::Version::Major,
                                                Unreal::Version::Minor,
                                                profile.release_string);
                int major = 0, minor = 0;
                if (std::swscanf(profile.release_string.c_str(), L"%d.%d", &major, &minor) == 2)
                {
                    profile.major = major;
                    profile.minor = minor;
                }
            }
            return profile;
        }
    } // namespace

    auto EngineProfile::family() const -> EngineFamily
    {
        if (major <= 0) return EngineFamily::Unknown;
        if (major >= 5) return EngineFamily::UE5;
        return uses_ffield() ? EngineFamily::UE4 : EngineFamily::UE4Legacy;
    }

    auto EngineProfile::to_string() const -> std::wstring
    {
        const wchar_t* fam = family() == EngineFamily::UE5 ? L"UE5" : family() == EngineFamily::UE4 ? L"UE4" : family() == EngineFamily::UE4Legacy ? L"UE4 (pre-FField)" : L"unknown";
        std::wstring version = std::format(L"{}.{}", major, minor);
        if (patch >= 0) version += std::format(L".{}", patch);
        return std::format(L"{} [{}]{}{}", version, fam, branch.empty() ? L"" : L" " + branch, debug_build ? L" (debug build)" : L"");
    }

    auto EngineProfile::Get() -> const EngineProfile&
    {
        std::lock_guard lock(g_mutex);
        if (!g_detected)
        {
            g_profile = detect();
            // Only cache once we have a usable answer; early calls (before Unreal init) must not stick.
            g_detected = g_profile.major > 0;
        }
        return g_profile;
    }

    auto EngineProfile::Redetect() -> const EngineProfile&
    {
        {
            std::lock_guard lock(g_mutex);
            g_detected = false;
        }
        return Get();
    }
} // namespace RC::Compat
