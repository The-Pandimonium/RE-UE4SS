// ToolkitMod: Windows DLL entry point (UE4SS loads this module and calls start_mod).

#include <ToolkitMod.hpp>

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new RC::Toolkit::ToolkitMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
