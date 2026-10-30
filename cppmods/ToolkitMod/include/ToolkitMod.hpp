#pragma once

#include <Mod/CppUserModBase.hpp>

namespace RC::Toolkit
{
    // UE4SS Toolkit: trainer + trainer creator, teleport menu with waypoints, level switcher.
    // All engine access goes through EngineCompat, so the same binary works on every engine version the
    // adapter registry knows about.
    class ToolkitMod : public CppUserModBase
    {
      public:
        ToolkitMod();
        ~ToolkitMod() override;

        auto on_unreal_init() -> void override;
        auto on_ui_init() -> void override;
    };
} // namespace RC::Toolkit
