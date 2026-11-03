#pragma once

#include <string>
#include <vector>

namespace RC::Toolkit
{
    struct PropRow
    {
        std::string name{};
        std::string type{};
        std::string value{};
        bool numeric{false};
        bool is_object{false};
    };

    struct FuncRow
    {
        std::string name{};
        std::string signature{};
        std::string owner{};
    };

    struct TargetInfo
    {
        bool found{false};
        std::string error{};
        std::string object_name{};
        std::string class_name{};
        std::vector<PropRow> properties{};
        std::vector<FuncRow> functions{};
    };

    // One input parameter of a function, as the engine declares it (the same CPF_Parm walk the blueprint stub
    // generator does). The trainer fills these in by name and converts the text to `kind` when calling.
    struct ParamRow
    {
        enum class Kind { Bool, Integer, Number, Enum, Text, Object, Struct, Other };
        std::string name{};
        std::string type{};                // "float", "bool", "Vector", "Object<Pawn>", ...
        Kind kind{Kind::Other};
        std::vector<std::string> options{}; // enum entries (short names)
        bool editable{true};               // false for kinds the trainer cannot write from text (rare)
    };

    struct FunctionInfo
    {
        bool found{false};
        std::string error{};
        std::string function_name{};
        std::vector<ParamRow> inputs{};
        std::vector<std::string> outputs{}; // "type name", informational (out params and the return value)
    };

    // Looks up `function` (optionally "Class:Function") on the first object `target` resolves to and lists its parameters.
    // Blocks the calling thread (UI) until the game thread answered, or times out.
    auto inspect_function(const std::string& target, const std::string& function) -> FunctionInfo;

    // Resolves a target expression on the game thread and lists its properties (with live values) and functions.
    // Blocks the calling thread (UI) until the game thread answered, or times out.
    auto inspect_target(const std::string& expression) -> TargetInfo;
} // namespace RC::Toolkit

namespace RC::Toolkit
{
    struct ClassRow
    {
        std::string name{};      // short name, e.g. "mainPlayer_C"
        std::string path{};      // full class path
        int instances{0};        // live non-default objects of exactly this class
        bool is_actor{false};
    };

    // Classes that currently have live instances, sorted by name. Classes (not objects) are what a trainer should
    // target, because object instances change every level load. Runs on the game thread; blocks until done.
    auto list_live_classes(bool include_non_actors) -> std::vector<ClassRow>;
} // namespace RC::Toolkit
