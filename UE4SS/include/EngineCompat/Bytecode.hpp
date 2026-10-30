#pragma once

// EngineCompat::Bytecode - the parts of Blueprint bytecode that depend on the engine version.
//
// The inspectors scan UFunction::Script for opcodes that embed a property pointer (instance/default/local
// variable reads). Which opcodes exist, and how wide the embedded pointer is, depends on the engine, so the
// inspectors ask this profile instead of hard-coding the answer.

#include <cstdint>
#include <vector>

#include <Common.hpp>
#include <EngineCompat/EngineProfile.hpp>

namespace RC::Unreal
{
    class UFunction;
    class FProperty;
}

namespace RC::Compat
{
    struct RC_UE4SS_API BytecodeProfile
    {
        // Size of the pointer that follows a variable opcode (FProperty* / UProperty*).
        uint32_t property_pointer_size{sizeof(void*)};
        // Opcodes whose operand is a pointer to a property of the executing object.
        std::vector<uint8_t> object_variable_opcodes;

        [[nodiscard]] auto is_object_variable_opcode(uint8_t opcode) const -> bool;
        static auto For(const EngineProfile& profile) -> BytecodeProfile;
        static auto Current() -> const BytecodeProfile&;
    };

    // Scans a function's script for object-variable operands and returns the distinct property pointers found.
    RC_UE4SS_API auto ScanVariableReferences(Unreal::UFunction* function) -> std::vector<Unreal::FProperty*>;
} // namespace RC::Compat
