#include <PropAnalyzer.hpp>
#include <EngineCompat/Bytecode.hpp>

#include <Helpers/String.hpp>
#include <fmt/core.h>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Script.hpp>
#include <Unreal/UnrealFlags.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <excpt.h>

namespace
{
    using namespace RC::EventViewerMod;

    struct PropSnap
    {
        const PropAnalyzer::CachedPropInfo* info{};
        RC::Unreal::UObject* owner{};
        std::vector<uint8_t> bytes;
    };

    struct PerCallSnapshot
    {
        RC::Unreal::UObject* object{};
        std::string function_name;
        std::vector<PropSnap> snaps;
        bool is_func_scope = false;
    };

    thread_local std::vector<PerCallSnapshot> tl_call_stack;
    // Counts func-scope frames currently on this thread's call stack.
    // When > 0, deep-child-monitor and cross-class GET detection activate for sub-calls.
    thread_local int tl_func_scope_depth = 0;
} // anonymous namespace

namespace RC::EventViewerMod
{
    using RC::Unreal::FFrame;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;
    using RC::Unreal::UClass;
    using RC::Unreal::FProperty;
    using RC::Unreal::FBoolProperty;
    using RC::Unreal::FNumericProperty;
    using RC::Unreal::FObjectPropertyBase;
    using RC::Unreal::Hook::ERROR_ID;

    // ─── Singleton ──────────────────────────────────────────────────────────────

    auto PropAnalyzer::GetInstance() -> PropAnalyzer&
    {
        static PropAnalyzer s_instance;
        return s_instance;
    }

    PropAnalyzer::~PropAnalyzer()
    {
        if (!m_paused) remove_hooks();
    }

    // ─── Configuration ──────────────────────────────────────────────────────────

    auto PropAnalyzer::configure(PropAnalyzerConfig cfg) -> void
    {
        m_cfg = std::move(cfg);
        clear_cache();
        clear_read_cache();
        clear_refs_cache();
        clear_func_scope_cache();
        clear_func_scope_read_cache();
    }

    auto PropAnalyzer::set_imgui_thread_id(std::thread::id id) -> void { m_imgui_id = id; }

    auto PropAnalyzer::assert_on_imgui_thread() const -> void
    {
        if (std::this_thread::get_id() != m_imgui_id)
            throw std::runtime_error("PropAnalyzer: must be called from ImGui thread");
    }

    // ─── Lifecycle ──────────────────────────────────────────────────────────────

    auto PropAnalyzer::start() -> bool
    {
        assert_on_imgui_thread();
        if (!m_paused) return true;
        if (!install_hooks()) return false;
        m_paused = false;
        m_allow_queue.test_and_set(std::memory_order_acq_rel);
        return true;
    }

    auto PropAnalyzer::stop() -> bool
    {
        assert_on_imgui_thread();
        if (m_paused) return true;
        remove_hooks();
        m_allow_queue.clear(std::memory_order_release);
        m_paused = true;
        PropAccessEntry discard;
        while (m_queue.try_dequeue(discard)) {}
        return true;
    }

    auto PropAnalyzer::is_paused() const -> bool
    {
        assert_on_imgui_thread();
        return m_paused;
    }

    // ─── Hook management ────────────────────────────────────────────────────────

    auto PropAnalyzer::install_hooks() -> bool
    {
        using namespace RC::Unreal::Hook;

        static FCallbackOptions opts{false, true, STR("PropertyAnalyzer"), STR("PropWatch")};

        m_pe_pre_id = RegisterProcessEventPreCallback(
            [this](auto&, UObject* ctx, UFunction* fn, void* parms) {
                on_pre(ctx, fn, parms);
            }, opts);

        m_pe_post_id = RegisterProcessEventPostCallback(
            [this](auto&, UObject* ctx, UFunction* fn, void*) {
                on_post(ctx, fn);
            }, opts);

        m_plsf_pre_id = RegisterProcessLocalScriptFunctionPreCallback(
            [this](auto&, UObject* ctx, FFrame& stack, void*) {
                auto* fn = stack.Node();
                if (!fn) fn = stack.CurrentNativeFunction();
                on_pre(ctx, fn, stack.Locals());
            }, opts);

        m_plsf_post_id = RegisterProcessLocalScriptFunctionPostCallback(
            [this](auto&, UObject* ctx, FFrame& stack, void*) {
                auto* fn = stack.Node();
                if (!fn) fn = stack.CurrentNativeFunction();
                on_post(ctx, fn);
            }, opts);

        m_pi_pre_id = RegisterProcessInternalPreCallback(
            [this](auto&, UObject* ctx, FFrame& stack, void*) {
                auto* fn = stack.Node();
                if (!fn) fn = stack.CurrentNativeFunction();
                on_pi_pre(ctx, fn);
            }, opts);

        if (m_pe_pre_id == ERROR_ID || m_pe_post_id == ERROR_ID ||
            m_plsf_pre_id == ERROR_ID || m_plsf_post_id == ERROR_ID ||
            m_pi_pre_id == ERROR_ID)
        {
            remove_hooks();
            return false;
        }
        return true;
    }

    auto PropAnalyzer::remove_hooks() -> void
    {
        using RC::Unreal::Hook::UnregisterCallback;
        if (m_pe_pre_id != ERROR_ID)    { UnregisterCallback(m_pe_pre_id);    m_pe_pre_id    = ERROR_ID; }
        if (m_pe_post_id != ERROR_ID)   { UnregisterCallback(m_pe_post_id);   m_pe_post_id   = ERROR_ID; }
        if (m_plsf_pre_id != ERROR_ID)  { UnregisterCallback(m_plsf_pre_id);  m_plsf_pre_id  = ERROR_ID; }
        if (m_plsf_post_id != ERROR_ID) { UnregisterCallback(m_plsf_post_id); m_plsf_post_id = ERROR_ID; }
        if (m_pi_pre_id != ERROR_ID)    { UnregisterCallback(m_pi_pre_id);    m_pi_pre_id    = ERROR_ID; }
    }

    // ─── Dequeue ────────────────────────────────────────────────────────────────

    auto PropAnalyzer::dequeue(const std::function<void(PropAccessEntry&&)>& on_entry) -> void
    {
        assert_on_imgui_thread();
        PropAccessEntry entry;
        while (m_queue.try_dequeue(entry))
            on_entry(std::move(entry));
    }

    // ─── Watched-property cache ──────────────────────────────────────────────────

    auto PropAnalyzer::clear_cache() -> void
    {
        std::unique_lock lock(m_cache_mutex);
        m_class_cache.clear();
    }

    auto PropAnalyzer::build_cache_entry(UClass* uclass) -> void
    {
        std::vector<CachedPropInfo> props;

        const bool any_filter = !m_cfg.prop_names.empty() || !m_cfg.class_tokens.empty();
        if (any_filter)
        {
            bool passes = m_cfg.class_tokens.empty();
            if (!passes)
            {
                const auto class_lower = to_lower_case(RC::to_string(uclass->GetName()));
                for (const auto& tok : m_cfg.class_tokens)
                    if (class_lower.contains(tok)) { passes = true; break; }
            }

            if (passes)
            {
                for (FProperty* prop : RC::Unreal::TFieldRange<FProperty>(uclass))
                {
                    if (!prop) continue;
                    bool include = m_cfg.prop_names.empty();
                    if (!include)
                    {
                        const auto pl = to_lower_case(RC::to_string(prop->GetName()));
                        for (const auto& w : m_cfg.prop_names)
                            if (pl == w || pl.contains(w)) { include = true; break; }
                    }
                    if (include)
                    {
                        const bool is_bool = !!RC::Unreal::CastField<FBoolProperty>(prop);
                        props.push_back({prop, RC::to_string(prop->GetName()),
                                         is_bool ? 1 : prop->GetSize(), is_bool});
                    }
                }
            }
        }

        std::unique_lock lock(m_cache_mutex);
        m_class_cache.try_emplace(uclass, std::move(props));
    }

    auto PropAnalyzer::get_cached_props(UClass* uclass) -> const std::vector<CachedPropInfo>*
    {
        {
            std::shared_lock lock(m_cache_mutex);
            if (auto it = m_class_cache.find(uclass); it != m_class_cache.end())
                return &it->second;
        }
        build_cache_entry(uclass);
        std::shared_lock lock(m_cache_mutex);
        auto it = m_class_cache.find(uclass);
        return it != m_class_cache.end() ? &it->second : nullptr;
    }

    // ─── Func-scope cache ────────────────────────────────────────────────────────

    auto PropAnalyzer::clear_func_scope_cache() -> void
    {
        std::unique_lock lock(m_func_scope_cache_mutex);
        m_func_scope_cache.clear();
    }

    auto PropAnalyzer::clear_func_scope_read_cache() -> void
    {
        std::unique_lock lock(m_func_scope_read_cache_mutex);
        m_func_scope_read_cache.clear();
    }

    auto PropAnalyzer::build_func_scope_entry(UClass* uclass) -> void
    {
        std::vector<CachedPropInfo> props;

        bool passes = m_cfg.class_tokens.empty();
        if (!passes)
        {
            const auto cl = to_lower_case(RC::to_string(uclass->GetName()));
            for (const auto& tok : m_cfg.class_tokens)
                if (cl.contains(tok)) { passes = true; break; }
        }

        if (passes)
        {
            for (FProperty* prop : RC::Unreal::TFieldRange<FProperty>(uclass))
            {
                if (!prop) continue;
                const bool is_bool = !!RC::Unreal::CastField<FBoolProperty>(prop);
                props.push_back({prop, RC::to_string(prop->GetName()),
                                 is_bool ? 1 : prop->GetSize(), is_bool});
            }
        }

        std::unique_lock lock(m_func_scope_cache_mutex);
        m_func_scope_cache.try_emplace(uclass, std::move(props));
    }

    auto PropAnalyzer::get_func_scope_props(UClass* uclass) -> const std::vector<CachedPropInfo>*
    {
        {
            std::shared_lock lock(m_func_scope_cache_mutex);
            if (auto it = m_func_scope_cache.find(uclass); it != m_func_scope_cache.end())
                return &it->second;
        }
        build_func_scope_entry(uclass);
        std::shared_lock lock(m_func_scope_cache_mutex);
        auto it = m_func_scope_cache.find(uclass);
        return it != m_func_scope_cache.end() ? &it->second : nullptr;
    }

    auto PropAnalyzer::get_func_scope_reads(UFunction* fn,
                                            const std::vector<CachedPropInfo>& all_props) -> const ReadList*
    {
        {
            std::shared_lock lock(m_func_scope_read_cache_mutex);
            if (auto it = m_func_scope_read_cache.find(fn); it != m_func_scope_read_cache.end())
                return &it->second;
        }
        ReadList reads = scan_for_reads(fn, all_props);
        std::unique_lock lock(m_func_scope_read_cache_mutex);
        return &m_func_scope_read_cache.try_emplace(fn, std::move(reads)).first->second;
    }

    // Alias so the header name matches.
    auto PropAnalyzer::get_func_scope_reads_cached(UFunction* fn,
                                                    const std::vector<CachedPropInfo>& all_props)
        -> const ReadList*
    {
        return get_func_scope_reads(fn, all_props);
    }

    // ─── Object-reference cache ──────────────────────────────────────────────────

    auto PropAnalyzer::clear_refs_cache() -> void
    {
        std::unique_lock lock(m_refs_cache_mutex);
        m_refs_cache.clear();
    }

    auto PropAnalyzer::build_refs_entry(UClass* uclass) -> void
    {
        std::vector<FObjectPropertyBase*> refs;
        for (FProperty* prop : RC::Unreal::TFieldRange<FProperty>(uclass))
        {
            if (!prop) continue;
            if (auto* op = RC::Unreal::CastField<FObjectPropertyBase>(prop))
                refs.push_back(op);
        }
        std::unique_lock lock(m_refs_cache_mutex);
        m_refs_cache.try_emplace(uclass, std::move(refs));
    }

    auto PropAnalyzer::get_class_refs(UClass* uclass) -> const std::vector<FObjectPropertyBase*>*
    {
        {
            std::shared_lock lock(m_refs_cache_mutex);
            if (auto it = m_refs_cache.find(uclass); it != m_refs_cache.end())
                return &it->second;
        }
        build_refs_entry(uclass);
        std::shared_lock lock(m_refs_cache_mutex);
        auto it = m_refs_cache.find(uclass);
        return it != m_refs_cache.end() ? &it->second : nullptr;
    }

    // ─── Bytecode scanner ────────────────────────────────────────────────────────

    auto PropAnalyzer::clear_read_cache() -> void
    {
        std::unique_lock lock(m_read_cache_mutex);
        m_function_read_cache.clear();
    }

    auto PropAnalyzer::scan_for_reads(UFunction* fn,
                                      const std::vector<CachedPropInfo>& watched) -> ReadList
    {
        ReadList reads;
        if (watched.empty() || !fn) return reads;

        // Which opcodes embed a property pointer, and how wide that pointer is, depends on the engine version.
        // EngineCompat answers that, so this scanner stays version agnostic.
        for (FProperty* candidate : RC::Compat::ScanVariableReferences(fn))
        {
            for (const auto& info : watched)
            {
                if (info.prop != candidate) continue;
                reads.emplace_back(info.prop, info.prop_name);
                break;
            }
        }
        return reads;
    }

    auto PropAnalyzer::get_function_reads(UFunction* fn,
                                          const std::vector<CachedPropInfo>& watched) -> const ReadList*
    {
        {
            std::shared_lock lock(m_read_cache_mutex);
            if (auto it = m_function_read_cache.find(fn); it != m_function_read_cache.end())
                return &it->second;
        }
        ReadList reads = scan_for_reads(fn, watched);
        std::unique_lock lock(m_read_cache_mutex);
        return &m_function_read_cache.try_emplace(fn, std::move(reads)).first->second;
    }

    // ─── GET emission (shared by PE and PI hooks) ─────────────────────────────────

    auto PropAnalyzer::emit_get_entries(UObject* context, UFunction* fn, bool is_func_scope) -> void
    {
        if (!context || !fn) return;

        UClass* uclass = context->GetClassPrivate();
        if (!uclass) return;

        const std::vector<CachedPropInfo>* class_props = is_func_scope
            ? get_func_scope_props(uclass)
            : get_cached_props(uclass);

        // ── Same-class GETs ───────────────────────────────────────────────────
        const ReadList* reads = nullptr;
        if (class_props && !class_props->empty())
        {
            reads = is_func_scope
                ? get_func_scope_reads(fn, *class_props)
                : get_function_reads(fn, *class_props);
        }

        if (!reads || reads->empty()) return;

        const std::string obj_name = RC::to_string(context->GetName());
        const std::string cls_name = RC::to_string(uclass->GetName());
        const std::string fn_name  = RC::to_string(fn->GetName());

        for (const auto& [read_prop, read_prop_name] : *reads)
            {
                PropAccessEntry entry;
                entry.serial        = m_serial.fetch_add(1, std::memory_order_relaxed);
                entry.access_type   = EPropAccessType::Get;
                entry.object_name   = obj_name;
                entry.class_name    = cls_name;
                entry.function_name = fn_name;
                entry.property_name = read_prop_name;

                if (class_props)
                {
                    for (const auto& info : *class_props)
                    {
                        if (info.prop != read_prop) continue;
                        if (info.is_bool)
                        {
                            auto* bp = RC::Unreal::CastField<FBoolProperty>(read_prop);
                            entry.old_value = bp->GetPropertyValueInContainer(context) ? "true" : "false";
                        }
                        else
                        {
                            const void* vp = read_prop->ContainerPtrToValuePtr<void>(context);
                            entry.old_value = serialize_value(info, vp);
                        }
                        break;
                    }
                }
                m_queue.enqueue(std::move(entry));
        }
    }

    // ─── ProcessInternal pre-hook ────────────────────────────────────────────────

    auto PropAnalyzer::on_pi_pre(UObject* context, UFunction* function) -> void
    {
        if (!m_cfg.scan_pi_gets) return;
        if (!m_allow_queue.test(std::memory_order_acquire)) return;
        if (!context || !function) return;

        const bool func_scope = (tl_func_scope_depth > 0) || [&]() -> bool {
            if (m_cfg.func_patterns.empty()) return false;
            const auto fnl = to_lower_case(RC::to_string(function->GetName()));
            for (const auto& pat : m_cfg.func_patterns)
                if (fnl == pat || fnl.contains(pat)) return true;
            return false;
        }();

        emit_get_entries(context, function, func_scope);
    }

    // ─── ProcessEvent/PLSF hooks (SET detection + optional PE GET detection) ─────

    auto PropAnalyzer::on_pre(UObject* context, UFunction* function, const void* parms) -> void
    {
        if (!m_allow_queue.test(std::memory_order_acquire)) return;
        if (!context || !function) return;

        UClass* uclass = context->GetClassPrivate();
        if (!uclass) return;

        // ── Func-scope determination ──────────────────────────────────────────
        const bool func_scope_direct = [&]() -> bool {
            if (m_cfg.func_patterns.empty()) return false;
            const auto fnl = to_lower_case(RC::to_string(function->GetName()));
            for (const auto& pat : m_cfg.func_patterns)
                if (fnl == pat || fnl.contains(pat)) return true;
            return false;
        }();

        // A child call is in func-scope context when deep_child_monitor is on AND we're
        // already inside a func-scope frame on this thread.
        const bool use_func_scope = func_scope_direct ||
            (m_cfg.deep_child_monitor && tl_func_scope_depth > 0);

        // ── Optional: GET detection via PE hook ───────────────────────────────
        // Useful for functions that don't go through ProcessInternal (e.g. native thunks
        // called from within a Blueprint, or belt-and-suspenders for custom events).
        if (m_cfg.scan_pe_gets)
            emit_get_entries(context, function, use_func_scope);

        // ── Build snapshot frame ──────────────────────────────────────────────
        auto make_snap = [](const CachedPropInfo& info, UObject* owner) -> PropSnap {
            PropSnap snap;
            snap.info  = &info;
            snap.owner = owner;
            if (info.is_bool)
            {
                auto* bp   = RC::Unreal::CastField<FBoolProperty>(info.prop);
                snap.bytes = {bp->GetPropertyValueInContainer(owner) ? uint8_t{1} : uint8_t{0}};
            }
            else
            {
                const void* vp = info.prop->ContainerPtrToValuePtr<void>(owner);
                snap.bytes.resize(static_cast<size_t>(info.prop_size));
                std::memcpy(snap.bytes.data(), vp, snap.bytes.size());
            }
            return snap;
        };

        PerCallSnapshot frame;
        frame.object        = context;
        frame.function_name = RC::to_string(function->GetName());

        auto snapshot_obj = [&](UObject* obj, const std::vector<CachedPropInfo>* cached) {
            if (!cached || cached->empty()) return;
            for (const auto& info : *cached)
                frame.snaps.push_back(make_snap(info, obj));
        };

        if (func_scope_direct)
        {
            frame.is_func_scope = true;
            ++tl_func_scope_depth;
        }

        // 1. Context object's own properties.
        snapshot_obj(context, use_func_scope ? get_func_scope_props(uclass)
                                             : get_cached_props(uclass));

        // 2. One level of FObjectProperty references stored on the context.
        //    In func-scope or deep-child mode, use func-scope props so ALL properties of
        //    referenced objects are tracked, not just the watched-name-filtered set.
        const std::vector<FObjectPropertyBase*>* obj_refs = get_class_refs(uclass);
        if (obj_refs)
        {
            for (FObjectPropertyBase* obj_field : *obj_refs)
            {
                if (!obj_field) continue;
                void* vp      = obj_field->ContainerPtrToValuePtr<void>(context);
                UObject* robj = obj_field->GetObjectPropertyValue(vp);
                if (!robj) continue;
                UClass* rcls  = robj->GetClassPrivate();
                if (rcls) snapshot_obj(robj, use_func_scope ? get_func_scope_props(rcls)
                                                            : get_cached_props(rcls));
            }
        }

        // 3. UObject* function parameters: snapshot the objects passed into this call.
        //    Handles patterns like SetProperty(widget, value) where widget is passed in.
        if (parms)
        {
            for (FProperty* param : RC::Unreal::TFieldRange<FProperty>(function))
            {
                if (!param) continue;
                if (!param->HasAnyPropertyFlags(RC::Unreal::CPF_Parm)) continue;
                if (param->HasAnyPropertyFlags(RC::Unreal::CPF_ReturnParm)) continue;
                auto* op = RC::Unreal::CastField<FObjectPropertyBase>(param);
                if (!op) continue;
                const void* pvp    = param->ContainerPtrToValuePtr<void>(parms);
                UObject* param_obj = op->GetObjectPropertyValue(pvp);
                if (!param_obj) continue;
                UClass* pcls = param_obj->GetClassPrivate();
                if (pcls) snapshot_obj(param_obj, use_func_scope ? get_func_scope_props(pcls)
                                                                 : get_cached_props(pcls));
            }
        }

        tl_call_stack.push_back(std::move(frame));
    }

    auto PropAnalyzer::on_post(UObject* context, UFunction* /*function*/) -> void
    {
        if (tl_call_stack.empty()) return;

        PerCallSnapshot frame = std::move(tl_call_stack.back());
        tl_call_stack.pop_back();

        if (frame.is_func_scope && tl_func_scope_depth > 0)
            --tl_func_scope_depth;

        if (!m_allow_queue.test(std::memory_order_acquire)) return;
        if (frame.snaps.empty() || !context) return;

        for (const PropSnap& snap : frame.snaps)
        {
            if (!snap.info || !snap.info->prop || !snap.owner) continue;

            UObject* owner = snap.owner;
            bool changed = false;
            std::string old_val, new_val;

            if (snap.info->is_bool)
            {
                auto* bp = RC::Unreal::CastField<FBoolProperty>(snap.info->prop);
                const uint8_t cur = bp->GetPropertyValueInContainer(owner) ? 1u : 0u;
                changed = cur != snap.bytes[0];
                if (changed)
                {
                    old_val = serialize_bool_byte(snap.bytes[0]);
                    new_val = serialize_bool_byte(cur);
                }
            }
            else
            {
                const void* cur = snap.info->prop->ContainerPtrToValuePtr<void>(owner);
                changed = std::memcmp(snap.bytes.data(), cur, snap.bytes.size()) != 0;
                if (changed)
                {
                    old_val = serialize_value(*snap.info, snap.bytes.data());
                    new_val = serialize_value(*snap.info, cur);
                }
            }

            if (changed)
            {
                UClass* oc = owner->GetClassPrivate();
                PropAccessEntry entry;
                entry.serial        = m_serial.fetch_add(1, std::memory_order_relaxed);
                entry.access_type   = EPropAccessType::Set;
                entry.object_name   = RC::to_string(owner->GetName());
                entry.class_name    = oc ? RC::to_string(oc->GetName()) : "<unknown>";
                entry.function_name = frame.function_name;
                entry.property_name = snap.info->prop_name;
                entry.old_value     = std::move(old_val);
                entry.new_value     = std::move(new_val);
                m_queue.enqueue(std::move(entry));
            }
        }
    }

    // ─── Value serialization ─────────────────────────────────────────────────────

    auto PropAnalyzer::serialize_bool_byte(const uint8_t byte) -> std::string
    {
        return byte ? "true" : "false";
    }

    // Forward declaration for mutual recursion.
    static auto serialize_prop(RC::Unreal::FProperty* prop, const void* vp, int depth) -> std::string;

    // In UE4.25+, FField::ClassPrivate is the first pointer-sized member.
    // Returns true when that pointer is not in the valid Windows user-space range,
    // which indicates a legacy/corrupt FFieldClass and must be skipped.
    // Catches both near-null values (e.g. 0x02 → AV at 0x02+0x10=0x12) and
    // the all-ones sentinel (0xFFFF... → wrong CastFlags, wrong CastField results).
    static bool has_invalid_class_private(const RC::Unreal::FField* field) noexcept
    {
        // Validate field ptr itself before dereferencing (chain pointers can be garbage).
        const auto fp = reinterpret_cast<uintptr_t>(field);
        if (fp < 0x10000ULL || fp >= 0x0000800000000000ULL) return true;
        // Validate ClassPrivate (first pointer-sized member in UE4.25+ FField layout).
        const auto cp = *reinterpret_cast<const uintptr_t*>(field);
        return cp < 0x10000ULL || cp >= 0x0000800000000000ULL;
    }

    // Collects FProperty* children of ustruct into arr[0..max_count).
    // Returns the number stored. Uses __try/__except to survive structs whose
    // FFieldClass chain contains sentinel/garbage entries (which would crash
    // TFieldIterator::IterateToNext_425Plus). Fields with sentinel ClassPrivate
    // are filtered out before they reach the caller.
    //
    // IMPORTANT: this function must contain no C++ objects with non-trivial
    // destructors (MSVC C2712 rule for __try in functions requiring unwinding).
    static int collect_struct_props(RC::Unreal::UScriptStruct* ustruct,
                                    RC::Unreal::FProperty** arr,
                                    int max_count) noexcept
    {
        int count = 0;
        __try
        {
            for (RC::Unreal::FProperty* prop :
                 RC::Unreal::TFieldRange<RC::Unreal::FProperty>(ustruct))
            {
                if (count >= max_count) break;
                // Skip fields whose ClassPrivate is a sentinel — they would give
                // wrong CastField results and produce garbage output.
                if (has_invalid_class_private(prop)) continue;
                arr[count++] = prop;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return count;
    }

    static auto serialize_struct_fields(RC::Unreal::UScriptStruct* ustruct,
                                        const void* data, int depth) -> std::string
    {
        if (!ustruct || !data) return "<null>";

        RC::Unreal::FProperty* props[64];
        const int count = collect_struct_props(ustruct, props, 64);

        std::string out = "(";
        bool first = true;
        for (int i = 0; i < count; ++i)
        {
            RC::Unreal::FProperty* child = props[i];
            if (!child) continue;
            if (!first) out += ", ";
            first = false;
            out += RC::to_string(child->GetName());
            out += '=';
            out += serialize_prop(child, child->ContainerPtrToValuePtr<void>(data), depth + 1);
        }
        out += ')';
        return out;
    }

    
    static std::string indent_str(int depth)
    {
        return std::string(depth * 2, ' ');
    }

    
    static std::string serialize_struct_fields(RC::Unreal::UStruct* s, const void* data, int depth)
    {
        using namespace RC::Unreal;

        std::string out = "{\n";

        for (FProperty* p : s->ForEachProperty())
        {
            const void* field_ptr = p->ContainerPtrToValuePtr<void>(data);

            out += indent_str(depth + 1);
            out += RC::to_string(p->GetName());
            out += ": ";
            out += serialize_prop(p, field_ptr, depth + 1);
            out += "\n";
        }

        out += indent_str(depth) + "}";
        return out;
    }



    static auto serialize_prop(RC::Unreal::FProperty* prop, const void* vp, int depth) -> std::string
    {
        if (!prop || !vp) return "<null>";

        using namespace RC::Unreal;

        if (auto* bp = CastField<FBoolProperty>(prop))
            return bp->GetPropertyValue(vp) ? "true" : "false";

        if (auto* np = CastField<FNumericProperty>(prop))
        {
            if (np->IsInteger())
                return fmt::format("{}", np->GetSignedIntPropertyValue(const_cast<void*>(vp)));
            return fmt::format("{:.6g}", np->GetFloatingPointPropertyValue(vp));
        }

        if (auto* op = CastField<FObjectPropertyBase>(prop))
        {
            const UObject* obj = op->GetObjectPropertyValue(vp);
            return obj ? RC::to_string(obj->GetName()) : "None";
        }

        // FString
        if (auto* sp = CastField<FStrProperty>(prop))
        {
            const FString& str = sp->GetPropertyValue(vp);
            const wchar_t* chars = str.GetCharArray().GetData();
            return fmt::format("\"{}\"", chars ? RC::to_string(std::wstring(chars)) : std::string{});
        }

        if (auto* np = CastField<FNameProperty>(prop))
        {
            const FName name = np->GetPropertyValue(vp);
            return RC::to_string(name.ToString());
        }

        if (auto* tp = CastField<FTextProperty>(prop))
        {
            const FText text = tp->GetPropertyValue(vp);
            return fmt::format("\"{}\"", RC::to_string(text.ToString()));
        }

        // Recurse into structs up to depth 4 to avoid runaway output on deeply nested types.
        if (depth < 4)
        {
            if (auto* sp = CastField<FStructProperty>(prop))
            {
                return serialize_struct_fields(sp->GetStruct(), vp, depth);
            }
        }

        if (auto* ar = CastField<FArrayProperty>(prop))
        {
            FScriptArrayHelper helper(ar, vp);

            std::string out = "[";
            if (helper.Num() > 0) out += "\n";

            for (int32 i = 0; i < helper.Num(); ++i)
            {
                const void* elem_ptr = helper.GetRawPtr(i);

                out += indent_str(depth + 1);
                out += serialize_prop(ar->GetInner(), elem_ptr, depth + 1);

                if (i + 1 < helper.Num()) out += ",";
                out += "\n";
            }

            out += indent_str(depth) + "]";
            return out;
        }

        // Set
        if (auto* set = CastField<FSetProperty>(prop))
        {
            FScriptSetHelper helper(set, vp);

            std::string out = "{";
            bool first = true;

            for (int32 i = 0; i < helper.GetMaxIndex(); ++i)
            {
                if (!helper.IsValidIndex(i)) continue;

                const void* elem_ptr = helper.GetElementPtr(i);

                if (!first) out += ", ";
                out += serialize_prop(set->GetElementProp(), elem_ptr, depth + 1);

                first = false;
            }

            out += "}";
            return out;
        }

        // Map
        if (auto* map = CastField<FMapProperty>(prop))
        {
            auto* script_map = const_cast<FScriptMap*>(static_cast<const FScriptMap*>(vp));
            const FScriptMapLayout& layout = map->GetMapLayout();

            std::string out = "{";
            if (script_map->Num() > 0) out += "\n";

            for (int32 i = 0; i < script_map->GetMaxIndex(); ++i)
            {
                if (!script_map->IsValidIndex(i)) continue;

                const auto* pair = static_cast<const uint8_t*>(script_map->GetData(i, layout));
                const void* key_ptr = pair;
                const void* val_ptr = pair + layout.ValueOffset;

                out += indent_str(depth + 1);

                out += serialize_prop(map->GetKeyProp(), key_ptr, depth + 1);
                out += ": ";
                out += serialize_prop(map->GetValueProp(), val_ptr, depth + 1);

                out += "\n";
            }

            out += indent_str(depth) + "}";
            return out;
        }

        // Fallback: hex dump (capped at 16 bytes for nested props).
        const auto* bytes = static_cast<const uint8_t*>(vp);
        const int32_t show = std::min(prop->GetSize(), depth == 0 ? 32 : 16);
        std::string hex;
        hex.reserve(static_cast<size_t>(show) * 3);
        for (int32_t b = 0; b < show; ++b)
        {
            hex += fmt::format("{:02X}", bytes[b]);
            if (b + 1 < show) hex += ' ';
        }
        if (prop->GetSize() > show) hex += "...";
        return hex;
    }

    auto PropAnalyzer::serialize_value(const CachedPropInfo& info, const void* vp) -> std::string
    {
        if (!vp) return "<null>";

        // Snapshot bools are stored as a raw byte (0/1) extracted via GetPropertyValueInContainer.
        // For display, use the live property to re-read properly — but for the snapshot comparison
        // path the caller already knows it's a bool byte and calls serialize_bool_byte instead.
        if (info.is_bool)
        {
            auto* bp = RC::Unreal::CastField<FBoolProperty>(info.prop);
            return bp->GetPropertyValue(vp) ? "true" : "false";
        }

        return serialize_prop(info.prop, vp, 0);
    }

} // namespace RC::EventViewerMod
