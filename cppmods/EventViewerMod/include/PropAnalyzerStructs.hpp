#pragma once

// PropertyAnalyzer: shared data types for the property access monitoring feature.
//
// PropAccessEntry is a single detected property write event captured by PropAnalyzer
// and consumed by PropAnalyzerClient. Entries are passed through a ConcurrentQueue
// from game threads to the ImGui thread.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace RC::EventViewerMod
{
    enum class EPropAccessType : uint8_t
    {
        Set, // Property value changed during this function call (snapshot comparison)
        Get, // Property was read by this function (Blueprint bytecode scan)
    };

    struct PropAccessEntry
    {
        uint64_t serial{};
        EPropAccessType access_type{EPropAccessType::Set};
        std::string object_name;     // GetName() of the context UObject
        std::string class_name;      // GetName() of the context UClass
        std::string function_name;   // GetName() of the executing UFunction
        std::string property_name;   // matched watched property name
        std::string old_value;       // serialized value before the call
        std::string new_value;       // serialized value after the call
        bool is_disabled = false;    // filtered by current UI settings

        auto render(bool with_context_menu) const -> void;
        auto to_string() const -> std::string;
    };

    struct PropAnalyzerConfig
    {
        std::vector<std::string> prop_names;    // watched property name substrings
        std::vector<std::string> class_tokens;  // class filter substrings (empty = all)
        std::vector<std::string> func_patterns; // func-scope trigger patterns

        // GET detection: scan Blueprint bytecode for EX_InstanceVariable reads.
        // scan_pe_gets – fires in the ProcessEvent/PLSF pre-hook. Catches functions that
        //   go through ProcessEvent but not ProcessInternal (e.g. native C++ thunks where
        //   the call graph includes Blueprint-side reads, or as belt-and-suspenders).
        // scan_pi_gets – fires in the ProcessInternal pre-hook (the Blueprint VM entry
        //   point). This is the primary scanner; most Blueprint functions are covered here.
        // scan_cross_class – when scanning bytecode, also resolve EX_InstanceVariable
        //   pointers that belong to classes other than the current context (e.g. a property
        //   read on a widget passed in by reference or stored in a field).
        bool scan_pe_gets      = false;
        bool scan_pi_gets      = true;

        // When inside a func-scope frame (tl_func_scope_depth > 0), use func-scope props
        // (all class properties, ignoring the prop-name filter) for every nested
        // ProcessEvent/PLSF call. This makes mid-function object creation/retrieval
        // visible: the first time any function is called ON the new object, we snapshot
        // all its properties and detect subsequent writes.
        bool deep_child_monitor = false;
    };

    struct PropAnalyzerUIState
    {
        bool enabled = false;
        bool started = false;
        bool show_reads  = true;
        bool show_writes = true;

        // Comma-separated property names (raw user input, displayed as-is).
        std::string props_input;
        // Comma-separated class/object name substrings (empty = all objects).
        std::string classes_input;
        // Comma-separated function name substrings. When a function matches, all
        // properties on the context object are snapshotted (ignores props_input).
        std::string funcs_input;

        // Resolved token lists (lower-cased).
        std::vector<std::string> watched_prop_names;
        std::vector<std::string> class_tokens;
        std::vector<std::string> func_patterns;

        PropAnalyzerConfig config{};

        std::vector<PropAccessEntry> history;
        std::atomic_flag needs_save = ATOMIC_FLAG_INIT;
        std::string last_save_path;
    };
} // namespace RC::EventViewerMod
