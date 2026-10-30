#include <EngineCompat/Bytecode.hpp>

#include <algorithm>
#include <cstring>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Script.hpp>

namespace RC::Compat
{
    using namespace RC::Unreal;

    auto BytecodeProfile::is_object_variable_opcode(uint8_t opcode) const -> bool
    {
        return std::find(object_variable_opcodes.begin(), object_variable_opcodes.end(), opcode) != object_variable_opcodes.end();
    }

    auto BytecodeProfile::For(const EngineProfile& profile) -> BytecodeProfile
    {
        BytecodeProfile out;
        // The operand is a raw FProperty* (4.25+) or UProperty* (earlier): a native pointer either way on the
        // 64-bit builds UE4SS supports.
        out.property_pointer_size = sizeof(void*);
        out.object_variable_opcodes = {static_cast<uint8_t>(EX_InstanceVariable), static_cast<uint8_t>(EX_DefaultVariable)};
        // Blueprint sparse class data was introduced in 4.26; before that 0x6C is not a variable opcode.
        if (profile.has_sparse_class_data()) out.object_variable_opcodes.push_back(static_cast<uint8_t>(EX_ClassSparseDataVariable));
        return out;
    }

    auto BytecodeProfile::Current() -> const BytecodeProfile&
    {
        static BytecodeProfile cached = For(EngineProfile::Get());
        static bool resolved = EngineProfile::Get().major > 0;
        if (!resolved && EngineProfile::Get().major > 0)
        {
            cached = For(EngineProfile::Get());
            resolved = true;
        }
        return cached;
    }

    auto ScanVariableReferences(UFunction* function) -> std::vector<FProperty*>
    {
        std::vector<FProperty*> found;
        if (!function) return found;

        const BytecodeProfile& bytecode = BytecodeProfile::Current();
        auto& script = function->GetScript();
        const uint8_t* data = script.GetData();
        const int32_t size = script.Num();
        const auto ptr_size = static_cast<int32_t>(bytecode.property_pointer_size);
        if (!data || size < 1 + ptr_size) return found;

        for (int32_t i = 0; i <= size - 1 - ptr_size; ++i)
        {
            if (!bytecode.is_object_variable_opcode(data[i])) continue;
            FProperty* candidate = nullptr;
            std::memcpy(&candidate, data + i + 1, static_cast<size_t>(ptr_size));
            if (!candidate) continue;
            if (std::find(found.begin(), found.end(), candidate) == found.end()) found.push_back(candidate);
        }
        return found;
    }
} // namespace RC::Compat
