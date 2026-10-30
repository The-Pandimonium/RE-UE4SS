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

    // Resolves a target expression on the game thread and lists its properties (with live values) and functions.
    // Blocks the calling thread (UI) until the game thread answered, or times out.
    auto inspect_target(const std::string& expression) -> TargetInfo;
} // namespace RC::Toolkit
