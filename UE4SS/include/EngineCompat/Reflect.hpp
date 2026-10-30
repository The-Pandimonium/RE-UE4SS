#pragma once

// EngineCompat::Reflect - reflection helpers that make engine-version-specific layouts irrelevant.
//
// The rule: never cast engine memory to a hand-written struct. Everything is addressed by property
// name through the engine's own reflection data, so a float FVector (UE4) and a double FVector (UE5
// large world coordinates) are handled by the same code path.

#include <Common.hpp>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <String/StringType.hpp>

namespace RC::Compat::Reflect
{
    using RC::StringType;
    using RC::Unreal::FProperty;
    using RC::Unreal::UClass;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;
    using RC::Unreal::UScriptStruct;

    // Engine-neutral value types. Always double precision; narrowed on write when the engine uses float.
    struct Vec2
    {
        double x{}, y{};
    };
    struct Vec3
    {
        double x{}, y{}, z{};
    };
    struct Rot3
    {
        double pitch{}, yaw{}, roll{};
    };
    struct Color
    {
        float r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};
    };

    // ---- Lookup ------------------------------------------------------------------------------------

    // Accepts a full path ("/Script/Engine.PlayerController"), or a bare class name ("PlayerController",
    // "BP_Player_C") which is searched for in the engine packages first and then globally.
    RC_UE4SS_API auto FindClass(const StringType& name_or_path) -> UClass*;
    // Class default object of a class given by name (used as the target for static library functions).
    RC_UE4SS_API auto DefaultObjectOf(const StringType& class_name) -> UObject*;
    // Finds a function by name on an object's class (walking the super chain).
    RC_UE4SS_API auto FindFunction(UObject* object, const StringType& name) -> UFunction*;
    // "Class:Function" or "/Script/Engine.Class:Function".
    RC_UE4SS_API auto FindFunctionByPath(const StringType& class_colon_function) -> UFunction*;
    RC_UE4SS_API auto FindProperty(UObject* object, const StringType& name) -> FProperty*;
    RC_UE4SS_API auto IsValidObject(const UObject* object) -> bool;

    // ---- Struct fields -----------------------------------------------------------------------------

    // Read/write a numeric member (X, Y, Pitch, R, ...) of a struct value, independent of whether the
    // engine stores it as float or double.
    RC_UE4SS_API auto GetStructNumber(UScriptStruct* type, const void* value, const wchar_t* field) -> std::optional<double>;
    RC_UE4SS_API auto SetStructNumber(UScriptStruct* type, void* value, const wchar_t* field, double number) -> bool;

    RC_UE4SS_API auto ReadVec3(FProperty* property, const void* value, Vec3& out) -> bool;
    RC_UE4SS_API auto WriteVec3(FProperty* property, void* value, const Vec3& in) -> bool;
    RC_UE4SS_API auto ReadVec2(FProperty* property, const void* value, Vec2& out) -> bool;
    RC_UE4SS_API auto WriteVec2(FProperty* property, void* value, const Vec2& in) -> bool;
    RC_UE4SS_API auto ReadRot3(FProperty* property, const void* value, Rot3& out) -> bool;
    RC_UE4SS_API auto WriteRot3(FProperty* property, void* value, const Rot3& in) -> bool;
    RC_UE4SS_API auto WriteColor(FProperty* property, void* value, const Color& in) -> bool;

    // ---- Generic values ----------------------------------------------------------------------------

    // Engine text form of a value ("(X=1.0,Y=2.0,Z=3.0)", "True", "/Game/Foo.Foo"). Works for every property type.
    RC_UE4SS_API auto ExportText(FProperty* property, const void* value, UObject* parent = nullptr) -> std::optional<StringType>;
    // Parses the engine text form back into the value. Returns false when the text was rejected.
    RC_UE4SS_API auto ImportText(FProperty* property, void* value, const StringType& text, UObject* owner = nullptr) -> bool;

    // Bool / integer / float / enum properties as a double. Nullopt for anything else.
    RC_UE4SS_API auto GetNumber(FProperty* property, const void* value) -> std::optional<double>;
    RC_UE4SS_API auto SetNumber(FProperty* property, void* value, double number) -> bool;
    [[nodiscard]] RC_UE4SS_API auto IsNumeric(FProperty* property) -> bool;

    // Whole-object accessors by property name.
    RC_UE4SS_API auto GetPropertyText(UObject* object, const StringType& name) -> std::optional<StringType>;
    RC_UE4SS_API auto SetPropertyText(UObject* object, const StringType& name, const StringType& text) -> bool;
    RC_UE4SS_API auto GetPropertyNumber(UObject* object, const StringType& name) -> std::optional<double>;
    RC_UE4SS_API auto SetPropertyNumber(UObject* object, const StringType& name, double number) -> bool;
    // Object-valued property (e.g. Pawn, CharacterMovement). Nullptr when absent or not an object property.
    RC_UE4SS_API auto GetObjectProperty(UObject* object, const StringType& name) -> UObject*;

    // Short, human readable type name of a property for UI ("float", "bool", "Vector", "Object<Pawn>").
    RC_UE4SS_API auto TypeName(FProperty* property) -> StringType;

    // ---- Function calls ----------------------------------------------------------------------------

    // A reflection-driven call. Parameters are addressed by name; alternates are separated with '|'
    // ("WorldContextObject|WorldContext") so one call site can span engine versions that renamed a parameter.
    //
    //   Call call(target, fn);
    //   call.SetVec3(L"NewLocation", {1, 2, 3}).SetBool(L"bTeleport", true).Invoke();
    //   auto moved = call.GetBool(L"ReturnValue");
    class RC_UE4SS_API Call
    {
      public:
        Call(UObject* target, UFunction* function);
        // Static library function by class name + function name, e.g. Call::Static(L"GameplayStatics", L"GetPlayerController").
        static auto Static(const StringType& class_name, const StringType& function_name) -> Call;
        ~Call();
        Call(Call&&) noexcept;
        Call(const Call&) = delete;
        auto operator=(const Call&) -> Call& = delete;
        auto operator=(Call&&) -> Call& = delete;

        [[nodiscard]] auto valid() const -> bool { return m_target && m_function; }
        [[nodiscard]] auto has_param(const wchar_t* names) const -> bool;
        [[nodiscard]] auto function() const -> UFunction* { return m_function; }

        auto SetBool(const wchar_t* names, bool value) -> Call&;
        auto SetNumber(const wchar_t* names, double value) -> Call&;
        auto SetObject(const wchar_t* names, UObject* value) -> Call&;
        auto SetText(const wchar_t* names, const StringType& text) -> Call&;
        auto SetVec3(const wchar_t* names, const Vec3& value) -> Call&;
        auto SetVec2(const wchar_t* names, const Vec2& value) -> Call&;
        auto SetRot3(const wchar_t* names, const Rot3& value) -> Call&;
        auto SetColor(const wchar_t* names, const Color& value) -> Call&;

        // Runs the function. False when invalid or when the engine raised while executing it.
        auto Invoke() -> bool;

        // Outputs / return value ("ReturnValue" or any out parameter).
        [[nodiscard]] auto GetBool(const wchar_t* names) const -> std::optional<bool>;
        [[nodiscard]] auto GetNumber(const wchar_t* names) const -> std::optional<double>;
        [[nodiscard]] auto GetObject(const wchar_t* names) const -> UObject*;
        [[nodiscard]] auto GetText(const wchar_t* names) const -> std::optional<StringType>;
        [[nodiscard]] auto GetVec3(const wchar_t* names) const -> std::optional<Vec3>;
        [[nodiscard]] auto GetVec2(const wchar_t* names) const -> std::optional<Vec2>;

        // Raw access for array/struct out-parameters the typed getters do not cover.
        [[nodiscard]] auto ParamProperty(const wchar_t* names) const -> FProperty* { return find_param(names); }
        [[nodiscard]] auto ParamData(const wchar_t* names) const -> void* { return param_ptr(find_param(names)); }

        // Names of the parameters (for UI / diagnostics).
        [[nodiscard]] auto describe() const -> StringType;

      private:
        [[nodiscard]] auto find_param(const wchar_t* names) const -> FProperty*;
        [[nodiscard]] auto param_ptr(FProperty* property) const -> void*;

        UObject* m_target{};
        UFunction* m_function{};
        std::vector<uint8_t> m_buffer;
        std::vector<FProperty*> m_params;
    };
} // namespace RC::Compat::Reflect
