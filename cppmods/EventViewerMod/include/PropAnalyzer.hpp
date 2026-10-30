#pragma once

// PropertyAnalyzer: capture backend (hook installation + property snapshot queueing).
//
// SET detection — ProcessEvent/PLSF pre/post hooks:
//   Pre-hook snapshots watched property values on the context object (and, optionally,
//   one level of stored object references). Post-hook compares snapshots; changed
//   values are enqueued as Set entries.
//
// GET detection — bytecode scan (ProcessInternal and/or ProcessEvent pre-hooks):
//   Scans a UFunction's Script bytecode once for EX_InstanceVariable opcodes whose
//   embedded FProperty* matches a watched property. Result is cached per-function.
//   Cross-class scanning resolves FProperty* owners via GetTypedOwner<UClass>() and
//   emits entries for properties on classes other than the immediate context.
//
// Func-scope mode (matched by m_func_patterns):
//   When a function name matches a pattern, ALL class properties are snapshotted
//   (prop-name filter ignored), and GET detection uses the full property list.
//   Deep-child-monitor extends this to every nested call inside the func-scope frame.
//
// Thread-safety contract:
//   configure() and start()/stop() must be called from the ImGui thread.
//   dequeue() must be called from the ImGui thread.
//   Hook callbacks run on arbitrary game threads; they only read caches and enqueue.

#include <atomic>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <PropAnalyzerStructs.hpp>

#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <concurrentqueue.h>

namespace RC::EventViewerMod
{
    class PropAnalyzer
    {
      public:
        static auto GetInstance() -> PropAnalyzer&;

        // [Thread-ImGui] Apply configuration. Must be called while hooks are not installed.
        auto configure(PropAnalyzerConfig cfg) -> void;

        // [Thread-ImGui]
        auto start() -> bool;
        auto stop() -> bool;
        [[nodiscard]] auto is_paused() const -> bool;
        auto set_imgui_thread_id(std::thread::id id) -> void;

        // [Thread-ImGui] Drains all available entries, calling on_entry for each.
        auto dequeue(const std::function<void(PropAccessEntry&&)>& on_entry) -> void;

        ~PropAnalyzer();

        PropAnalyzer(const PropAnalyzer&) = delete;
        PropAnalyzer& operator=(const PropAnalyzer&) = delete;

      private:
        PropAnalyzer() = default;

        auto assert_on_imgui_thread() const -> void;
        auto install_hooks() -> bool;
        auto remove_hooks() -> void;

        auto on_pre(RC::Unreal::UObject* context,
                    RC::Unreal::UFunction* function,
                    const void* parms) -> void;
        auto on_post(RC::Unreal::UObject* context,
                     RC::Unreal::UFunction* function) -> void;

      public:
        struct CachedPropInfo
        {
            RC::Unreal::FProperty* prop{};
            std::string prop_name;
            int32_t prop_size{};
            bool is_bool{};
        };

      private:
        // ─── Watched-property cache (prop-name + class-token filtered) ───────────

        auto build_cache_entry(RC::Unreal::UClass* uclass) -> void;
        auto get_cached_props(RC::Unreal::UClass* uclass) -> const std::vector<CachedPropInfo>*;
        auto clear_cache() -> void;

        using ReadList = std::vector<std::pair<RC::Unreal::FProperty*, std::string>>;

        // ─── Func-scope cache (all-props, class-token filtered) ──────────────────

        auto build_func_scope_entry(RC::Unreal::UClass* uclass) -> void;
        auto get_func_scope_props(RC::Unreal::UClass* uclass) -> const std::vector<CachedPropInfo>*;
        auto get_func_scope_reads(RC::Unreal::UFunction* fn,
                                  const std::vector<CachedPropInfo>& all_props) -> const ReadList*;
        auto clear_func_scope_cache() -> void;
        auto clear_func_scope_read_cache() -> void;

        // ─── Object-reference cache (one-level cross-object SET detection) ───────

        auto build_refs_entry(RC::Unreal::UClass* uclass) -> void;
        auto get_class_refs(RC::Unreal::UClass* uclass)
            -> const std::vector<RC::Unreal::FObjectPropertyBase*>*;
        auto clear_refs_cache() -> void;

        // ─── GET detection (bytecode scan) ───────────────────────────────────────

        // Scan fn's Script for EX_InstanceVariable opcodes that match watched_props.
        static auto scan_for_reads(RC::Unreal::UFunction* fn,
                                   const std::vector<CachedPropInfo>& watched_props) -> ReadList;

        auto get_function_reads(RC::Unreal::UFunction* fn,
                                const std::vector<CachedPropInfo>& watched_props) -> const ReadList*;
        auto get_func_scope_reads_cached(RC::Unreal::UFunction* fn,
                                         const std::vector<CachedPropInfo>& all_props)
            -> const ReadList*;
        auto clear_read_cache() -> void;

        // Emit GET entries for fn called on context.
        // Called from both the PE pre-hook and the PI pre-hook.
        auto emit_get_entries(RC::Unreal::UObject* context,
                              RC::Unreal::UFunction* fn,
                              bool is_func_scope) -> void;

        // PI pre-hook (Blueprint VM entry point).
        auto on_pi_pre(RC::Unreal::UObject* context, RC::Unreal::UFunction* function) -> void;

        // Serialize a property value to a display string.
        static auto serialize_value(const CachedPropInfo& info, const void* value_ptr) -> std::string;
        static auto serialize_bool_byte(uint8_t byte) -> std::string;

        // ─── State ───────────────────────────────────────────────────────────────

        moodycamel::ConcurrentQueue<PropAccessEntry> m_queue{};
        std::atomic_uint64_t m_serial{0};

        PropAnalyzerConfig m_cfg{};

        std::unordered_map<RC::Unreal::UClass*, std::vector<CachedPropInfo>> m_class_cache;
        std::shared_mutex m_cache_mutex;

        std::unordered_map<RC::Unreal::UClass*, std::vector<RC::Unreal::FObjectPropertyBase*>> m_refs_cache;
        std::shared_mutex m_refs_cache_mutex;

        std::unordered_map<RC::Unreal::UFunction*, ReadList> m_function_read_cache;
        std::shared_mutex m_read_cache_mutex;

        std::unordered_map<RC::Unreal::UClass*, std::vector<CachedPropInfo>> m_func_scope_cache;
        std::shared_mutex m_func_scope_cache_mutex;

        std::unordered_map<RC::Unreal::UFunction*, ReadList> m_func_scope_read_cache;
        std::shared_mutex m_func_scope_read_cache_mutex;

        bool m_paused = true;
        std::thread::id m_imgui_id{};
        std::atomic_flag m_allow_queue;

        RC::Unreal::Hook::GlobalCallbackId m_pe_pre_id{RC::Unreal::Hook::ERROR_ID};
        RC::Unreal::Hook::GlobalCallbackId m_pe_post_id{RC::Unreal::Hook::ERROR_ID};
        RC::Unreal::Hook::GlobalCallbackId m_plsf_pre_id{RC::Unreal::Hook::ERROR_ID};
        RC::Unreal::Hook::GlobalCallbackId m_plsf_post_id{RC::Unreal::Hook::ERROR_ID};
        RC::Unreal::Hook::GlobalCallbackId m_pi_pre_id{RC::Unreal::Hook::ERROR_ID};
    };
} // namespace RC::EventViewerMod
