#include <BlueprintAPI.hpp>

#include <cstdint>
#include <cstdlib>
#include <excpt.h>
#include <cwctype>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FAssetData.hpp>
#include <Unreal/GameplayStatics.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/Property/FNameProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FStrProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/Property/NumericPropertyTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/World.hpp>

namespace RC::BlueprintAPI
{
    using namespace RC::Unreal;

    // Track owner class names we've already logged to avoid spam.
    static std::unordered_set<std::wstring> s_seen_owner_names;

    // Name of the Blueprint Function Library class that is allowed to define
    // phantom functions. Modify this constant to the short name of the
    // Blueprint library (UClass) authors must place functions in.
    static const StringType kAllowedBlueprintLibrary = STR("UE4SS_BPLibrary");

    // Returns true when the given UFunction belongs to the allowed blueprint
    // library. Uses GetOuterPrivate() to avoid Cast<UClass> silently returning
    // null for UBlueprintGeneratedClass.
    static bool IsFunctionInAllowedLibraryUncached(UFunction* fn)
    {
        if (!fn) return false;
        UObject* outer = fn->GetOuterPrivate();
        if (!outer) return false;
        const StringType outerName = outer->GetName();
        if (outerName.find(kAllowedBlueprintLibrary) != StringType::npos)
            return true;
        // Fallback: check the full name in case the outer name alone doesn't
        // contain the library token.
        try { return fn->GetFullName().find(kAllowedBlueprintLibrary) != StringType::npos; }
        catch (...) { return false; }
    }

    // This runs from the ProcessEvent / ProcessInternal / ProcessLocalScriptFunction pre-hooks, i.e. for *every*
    // Blueprint function call in the game. Building the name strings above each time is fatal in a Blueprint-heavy
    // game: Voices of the Void runs millions of script calls while a level loads, and the per-call string allocations
    // slowed level loading to a crawl that looks like a hang. The answer only depends on the function, so memoise it.
    // The cached entry also records the function's name index so a UFunction address reused after garbage collection
    // (level unload) is re-evaluated instead of trusted.
    static bool IsFunctionInAllowedLibrary(UFunction* fn)
    {
        struct Entry { uint32_t name_index; bool allowed; };
        static std::shared_mutex s_mutex;
        static std::unordered_map<UFunction*, Entry> s_cache;

        if (!fn) return false;
        const uint32_t name_index = fn->GetNamePrivate().GetComparisonIndex().ToUnstableInt();
        {
            std::shared_lock lock(s_mutex);
            if (auto it = s_cache.find(fn); it != s_cache.end() && it->second.name_index == name_index) return it->second.allowed;
        }
        const bool allowed = IsFunctionInAllowedLibraryUncached(fn);
        std::unique_lock lock(s_mutex);
        s_cache[fn] = {name_index, allowed};
        return allowed;
    }

    // After a handler runs inside a ProcessInternal/VM hook, the handler
    // has written output values into stack.Locals() at property offsets.
    // The caller expects:
    //   - return value at the `result` pointer passed to ProcessInternal
    //   - out parameters at addresses in the stack.OutParms() linked list
    // This helper propagates those values so the caller actually receives them.
    static void PropagateOutParms(UFunction* fn, FFrame& stack, void* result)
    {
        uint8_t* locals = reinterpret_cast<uint8_t*>(stack.Locals());
        if (!locals) return;

        // Copy return value to result pointer (for EX_CallMath / direct ProcessInternal call).
        if (result)
        {
            for (FProperty* p : fn->ForEachProperty())
            {
                if (p->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    std::memcpy(result, locals + p->GetOffset_Internal(), static_cast<size_t>(p->GetSize()));
                    break;
                }
            }
        }

        // Copy each out-param from Locals to the caller's storage address.
        for (FOutParmRec* rec = stack.OutParms(); rec; rec = rec->NextOutParm)
        {
            FProperty* prop = rec->Property;
            if (!prop || !rec->PropAddr) continue;
            if (!prop->HasAnyPropertyFlags(CPF_OutParm)) continue;
            std::memcpy(rec->PropAddr, locals + prop->GetOffset_Internal(), static_cast<size_t>(prop->GetSize()));
        }
    }

    // Track unhandled function names we've already logged to avoid spamming
    // the output for common engine callbacks.
    static std::unordered_set<std::wstring> s_unhandled_seen;

    // ─────────────────────────────────────────────────────────────────────────
    // Static cache — built once at startup from the asset registry and
    // GUObjectArray. Levels and actor classes don't change between worlds,
    // so no invalidation is needed. Pure nodes (GetLevels, GetAllActorClasses)
    // return from this cache instantly.
    // ─────────────────────────────────────────────────────────────────────────

    static struct
    {
        bool                      built        = false;
        std::vector<std::wstring> levels;
        std::vector<UObject*>     actorClasses;
    } s_staticCache;

    // Find the UAssetRegistry singleton in GUObjectArray.
    static UAssetRegistry* FindAssetRegistry()
    {
        UAssetRegistry* result = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t)
        {
            if (result) return LoopAction::Break;
            if (!obj || !obj->GetClassPrivate()) return LoopAction::Continue;
            if (obj->GetClassPrivate()->GetName().find(STR("AssetRegistry")) != StringType::npos)
                result = static_cast<UAssetRegistry*>(obj);
            return LoopAction::Continue;
        });
        return result;
    }

    // Returns true if `s` is a user/game-made class — not a pure engine class.
    // Blueprint-compiled classes always end with "_C".
    // Native game classes live in packages whose path contains "/Game/".
    static bool IsUserMadeClass(UStruct* s)
    {
        if (!s) return false;
        const StringType name = s->GetName();
        // Blueprint-generated class names always end with _C
        if (name.size() >= 2 && name.back() == L'C' && name[name.size() - 2] == L'_')
            return true;
        // Check if the owning package is under /Game/
        UObject* pkg = s->GetOutermost();
        if (pkg)
        {
            const StringType pkgPath = pkg->GetName();
            if (pkgPath.find(STR("/Game/")) != StringType::npos)
                return true;
        }
        return false;
    }

    static void BuildStaticCache()
    {
        if (s_staticCache.built) return;
        s_staticCache.built = true;

        // ── Levels from asset registry ──────────────────────────────────────
        UAssetRegistry* ar = FindAssetRegistry();
        if (!ar)
        {
            Output::send<LogLevel::Warning>(STR("[BlueprintAPI] UAssetRegistry not found — level cache empty\n"));
        }
        else
        {
            // GetAssetsByClass with a bare FName misses entries in UE5 where the
            // registry uses FTopLevelAssetPath. Use GetAllAssets and filter manually.
            TArray<FAssetData> assets;
            ar->GetAllAssets(assets, true); // true = on-disk assets only

            // sizeof(FAssetData) in UE4SS (compile-time) may differ from the engine's
            // actual FAssetData size at runtime. Using operator[] with the wrong stride
            // either reads garbage or, when sizeof > engineStride, goes out of bounds.
            // Walk the raw buffer with the runtime stride instead.
            const int32_t realStride = FAssetData::StaticSize();
            if (realStride <= 0)
            {
                Output::send<LogLevel::Warning>(STR("[BlueprintAPI] FAssetData::StaticSize() returned 0 — level cache empty\n"));
            }
            else
            {
                auto* base = reinterpret_cast<uint8_t*>(assets.GetData());
                for (int32_t i = 0; i < assets.Num(); ++i)
                {
                    auto* entry = reinterpret_cast<FAssetData*>(base + static_cast<ptrdiff_t>(i) * realStride);
                    if (entry->AssetClass().ToString() != STR("World")) continue;
                    StringType path = entry->PackageName().ToString();
                    if (path.empty() || path[0] != L'/') continue;
                    auto slash = path.rfind(L'/');
                    s_staticCache.levels.push_back(
                        slash != StringType::npos
                            ? std::wstring(path.c_str() + slash + 1)
                            : std::wstring(path.c_str()));
                }
            }
        }

        // ── Actor classes from GUObjectArray ────────────────────────────────
        std::unordered_set<UClass*> seen;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
            auto* cls = Cast<UClass>(obj);
            if (!cls || seen.count(cls)) return LoopAction::Continue;
            if (!cls->IsChildOf(AActor::StaticClass())) return LoopAction::Continue;
            // Only cache user/Blueprint classes — skip pure engine classes
            if (!IsUserMadeClass(cls)) return LoopAction::Continue;
            seen.insert(cls);
            s_staticCache.actorClasses.push_back(cls);
            return LoopAction::Continue;
        });

        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] Static cache built: {} levels, {} actor classes\n"),
            s_staticCache.levels.size(), s_staticCache.actorClasses.size());
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Diagnostic: dump function parameter layout and parm buffer pointer
    static void DumpFunctionParms(UFunction* fn, void* parms)
    {
        if (!fn) return;
        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] Dumping parms for '{}':\n"), fn->GetName().c_str());
        for (FProperty* p : fn->ForEachProperty())
        {
            auto name = p->GetName();
            auto off = p->GetOffset_Internal();
            bool isParm = p->HasAnyPropertyFlags(CPF_Parm);
            bool isOut = p->HasAnyPropertyFlags(CPF_OutParm);
            bool isRet = p->HasAnyPropertyFlags(CPF_ReturnParm);
            Output::send<LogLevel::Normal>(STR("  prop='{}' off=0x{:X} parm={} out={} ret={} class={}\n"),
                                           name.c_str(),
                                           off,
                                           isParm,
                                           isOut,
                                           isRet,
                                           p->GetClass().GetFName().ToString().c_str());
            if (parms && isRet)
            {
                auto ptr = reinterpret_cast<uint8_t*>(parms) + off;
                Output::send<LogLevel::Normal>(STR("    -> ret ptr = {}\n"), reinterpret_cast<void*>(ptr));
            }
        }
    }

    // Property kind classification — uses class name to avoid template
    // constraint issues when compiling inside UE4SS core.
    // ─────────────────────────────────────────────────────────────────────────

    enum class EPropKind
    {
        Int,
        Float,
        Bool,
        Str,
        Name,
        Object,
        Other
    };

    static EPropKind ClassifyProp(FProperty* prop)
    {
        const StringType cn = prop->GetClass().GetFName().ToString();
        if (cn == STR("Int8Property") || cn == STR("Int16Property") || cn == STR("IntProperty") || cn == STR("Int64Property") || cn == STR("ByteProperty") ||
            cn == STR("UInt16Property") || cn == STR("UInt32Property") || cn == STR("UInt64Property"))
            return EPropKind::Int;
        if (cn == STR("FloatProperty") || cn == STR("DoubleProperty")) return EPropKind::Float;
        if (cn == STR("BoolProperty")) return EPropKind::Bool;
        if (cn == STR("StrProperty")) return EPropKind::Str;
        if (cn == STR("NameProperty")) return EPropKind::Name;
        if (cn == STR("ObjectProperty") || cn == STR("ClassProperty") || cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
            cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty"))
            return EPropKind::Object;
        return EPropKind::Other;
    }

    // Read integer value from property (handles all int widths).
    static int32_t ReadInt(FProperty* prop, void* vp)
    {
        const StringType cn = prop->GetClass().GetFName().ToString();
        if (cn == STR("Int8Property")) return static_cast<int32_t>(*static_cast<int8_t*>(vp));
        if (cn == STR("Int16Property")) return static_cast<int32_t>(*static_cast<int16_t*>(vp));
        if (cn == STR("Int64Property")) return static_cast<int32_t>(*static_cast<int64_t*>(vp));
        if (cn == STR("ByteProperty")) return static_cast<int32_t>(*static_cast<uint8_t*>(vp));
        return *static_cast<int32_t*>(vp); // IntProperty
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Parms buffer helpers
    // ─────────────────────────────────────────────────────────────────────────

    // Return a typed pointer into the parms buffer for the property named `name`.
    template <typename T>
    static T* ParamPtr(UFunction* fn, void* parms, const StringType& name)
    {
        for (FProperty* p : fn->ForEachProperty())
            if (p->GetName() == name)
                return reinterpret_cast<T*>(static_cast<uint8_t*>(parms) + p->GetOffset_Internal());
        return nullptr;
    }

    // Find an FProperty on a UFunction by exact name (used when we need the
    // FProperty* itself, e.g. to cast to FArrayProperty for FScriptArrayHelper).
    static FProperty* FindParam(UFunction* fn, const StringType& name)
    {
        for (FProperty* p : fn->ForEachProperty())
            if (p->GetName() == name) return p;
        return nullptr;
    }

    // Find the Nth output parameter (CPF_OutParm or CPF_ReturnParm) whose
    // FProperty class name matches `propClass` (e.g. "ArrayProperty",
    // "StrProperty"). `index` lets callers disambiguate when multiple outputs
    // share the same type — 0 means the first match.
    template <typename T>
    static T* FindOutParam(UFunction* fn, void* parms, const StringType& propClass, int index = 0)
    {
        int found = 0;
        for (FProperty* p : fn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;
            if (p->GetClass().GetFName().ToString() != propClass) continue;
            if (found++ == index)
                return reinterpret_cast<T*>(static_cast<uint8_t*>(parms) + p->GetOffset_Internal());
        }
        return nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // World helpers
    // ─────────────────────────────────────────────────────────────────────────

    static UObject* FindWorldContext()
    {
        UObject* world = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
            if (world) return LoopAction::Break;
            if (obj && obj->GetClassPrivate() && obj->GetClassPrivate()->GetName() == STR("World"))
            {
                if (obj->GetName().find(STR("Transient")) == StringType::npos) world = obj;
            }
            return LoopAction::Continue;
        });
        return world;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Blueprint struct helpers — UE4SS_BP_ACTOR and friends
    // ─────────────────────────────────────────────────────────────────────────

    // Cached pointers to the UserDefinedStructs. Populated lazily on first use.
    static struct
    {
        UScriptStruct* Actor      = nullptr; // UE4SS_BP_ACTOR
        UScriptStruct* Functions  = nullptr; // UE4SS_BP_FUNCTIONS
        UScriptStruct* Properties = nullptr; // UE4SS_BP_PROPERTIES
        UScriptStruct* PropList   = nullptr; // UE4SS_BP_PROPLIST
        UScriptStruct* Types      = nullptr; // UE4SS_BP_TYPES
        UScriptStruct* Struct     = nullptr; // UE4SS_BP_STRUCT
        UScriptStruct* Class      = nullptr; // UE4SS_BP_CLASS
    } s_bpStructs;

    // Find a UserDefinedStruct by short asset name.
    static UScriptStruct* FindScriptStruct(const StringType& name)
    {
        UScriptStruct* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
            if (found) return LoopAction::Break;
            auto* s = Cast<UScriptStruct>(obj);
            if (s && s->GetName() == name) found = s;
            return LoopAction::Continue;
        });
        return found;
    }

    // Resolve all structs. Returns false if any are missing.
    static bool EnsureBPStructs()
    {
        if (!s_bpStructs.Actor)      s_bpStructs.Actor      = FindScriptStruct(STR("UE4SS_BP_ACTOR"));
        if (!s_bpStructs.Functions)  s_bpStructs.Functions  = FindScriptStruct(STR("UE4SS_BP_FUNCTIONS"));
        if (!s_bpStructs.Properties) s_bpStructs.Properties = FindScriptStruct(STR("UE4SS_BP_PROPERTIES"));
        if (!s_bpStructs.Types)      s_bpStructs.Types      = FindScriptStruct(STR("UE4SS_BP_TYPES"));
        if (!s_bpStructs.PropList)   s_bpStructs.PropList   = FindScriptStruct(STR("UE4SS_BP_PROPLIST"));
        if (!s_bpStructs.Struct)     s_bpStructs.Struct     = FindScriptStruct(STR("UE4SS_BP_STRUCT"));
        if (!s_bpStructs.Class)      s_bpStructs.Class      = FindScriptStruct(STR("UE4SS_BP_CLASS"));
        return s_bpStructs.Actor && s_bpStructs.Functions &&
               s_bpStructs.Properties && s_bpStructs.PropList &&
               s_bpStructs.Types && s_bpStructs.Struct;
    }

    // In UE4.25+, FField::ClassPrivate is the first pointer-sized member.
    // Returns true when that value is outside the valid Windows 64-bit user-space range,
    // indicating a corrupt/legacy FFieldClass pointer that must not be dereferenced.
    static bool ffield_class_private_invalid(const FField* f) noexcept
    {
        // Validate f itself before dereferencing (GetChildProperties/GetNext can return garbage).
        const auto fp = reinterpret_cast<uintptr_t>(f);
        if (fp < 0x10000ULL || fp >= 0x0000800000000000ULL) return true;
        // Validate ClassPrivate (first pointer-sized member in UE4.25+ FField layout).
        const auto cp = *reinterpret_cast<const uintptr_t*>(f);
        return cp < 0x10000ULL || cp >= 0x0000800000000000ULL;
    }

    // Collect up to max_count FField* children of `first` into arr[].
    // Uses __try/__except to survive access violations from corrupt FFieldClass chains.
    // MUST NOT contain C++ objects with non-trivial destructors (MSVC C2712 rule).
    static int collect_ffield_chain(FField* first, FField** arr, int max_count) noexcept
    {
        int count = 0;
        __try
        {
            for (FField* f = first; f && count < max_count; f = f->GetNextFieldAsProperty())
            {
                if (ffield_class_private_invalid(f)) break;
                arr[count++] = f;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return count;
    }

    // Return a property on a UserDefinedStruct by friendly-name prefix (case-insensitive).
    // UserDefinedStruct fields are mangled: "FriendlyName_Index_GUID".
    // Matching the text before the first underscore is unambiguous within a struct.
    static FProperty* FindStructField(UScriptStruct* s, const StringType& prefix)
    {
        if (!s) return nullptr;

        FField* fields[128];
        const int count = collect_ffield_chain(s->GetChildProperties(), fields, 128);

        for (int i = 0; i < count; ++i)
        {
            FProperty* p = CastField<FProperty>(fields[i]);
            if (!p) continue;
            const StringType n = p->GetName();
            if (n.size() > prefix.size() && n[prefix.size()] == L'_')
            {
                // Case-insensitive compare of the prefix portion
                bool match = true;
                for (size_t j = 0; j < prefix.size(); ++j)
                {
                    if (std::towlower(n[j]) != std::towlower(prefix[j])) { match = false; break; }
                }
                if (match) return p;
            }
        }
        return nullptr;
    }

    // Case-insensitive wstring comparison.
    static bool WStrICmp(const StringType& a, const StringType& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return true;
    }

    // Match a property name against a user-supplied friendly name, case-insensitively.
    // Handles both UserDefinedStruct mangled names ("FriendlyName_Idx_GUID") and
    // native names (exact).
    static bool PropertyNameMatches(const StringType& propName, const StringType& friendly)
    {
        if (WStrICmp(propName, friendly)) return true;
        // Mangled: prefix before first underscore must match
        if (propName.size() > friendly.size() && propName[friendly.size()] == L'_')
        {
            bool match = true;
            for (size_t i = 0; i < friendly.size(); ++i)
                if (std::towlower(propName[i]) != std::towlower(friendly[i])) { match = false; break; }
            return match;
        }
        return false;
    }

    // Enum selector values matching UE4SS_BP_VALUE_ENUM display order.
    enum class EBPValueType : uint8_t
    {
        Boolean   = 0,
        Byte      = 1,
        Integer   = 2,
        Integer64 = 3,
        Float     = 4,
        Name      = 5,
        String    = 6,
        Text      = 7,
        Vector    = 8,
        Rotator   = 9,
        Transform = 10,
    };

    // Write a typed value (and its type selector) into a UE4SS_BP_TYPES instance.
    // `typesPtr` points to the start of the struct memory (already initialized).
    // `srcProp`  is the source FProperty (used for type classification and value copy).
    // `srcContainer` is the UObject/struct that owns the value; may be null for
    //   function parameters where we only need type information (value stays zero).
    static void WriteBPTypes(void* typesPtr, UScriptStruct* typesStruct,
                             FProperty* srcProp, void* srcContainer)
    {
        FProperty* selectorProp = FindStructField(typesStruct, STR("TypeSelector"));

        auto setSelector = [&](EBPValueType t) {
            if (selectorProp)
                *static_cast<uint8_t*>(selectorProp->ContainerPtrToValuePtr<void>(typesPtr)) =
                    static_cast<uint8_t>(t);
        };

        const StringType cn = srcProp->GetClass().GetFName().ToString();

        if (cn == STR("BoolProperty"))
        {
            setSelector(EBPValueType::Boolean);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Boolean"));
                if (dst)
                {
                    void* srcValue = srcProp->ContainerPtrToValuePtr<void>(srcContainer);
                    *static_cast<bool*>(dst->ContainerPtrToValuePtr<void>(typesPtr)) =
                        static_cast<FBoolProperty*>(srcProp)->GetPropertyValue(srcValue);
                }
            }
        }
        else if (cn == STR("ByteProperty"))
        {
            setSelector(EBPValueType::Byte);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Byte"));
                if (dst)
                    *static_cast<uint8_t*>(dst->ContainerPtrToValuePtr<void>(typesPtr)) =
                        *static_cast<uint8_t*>(srcProp->ContainerPtrToValuePtr<void>(srcContainer));
            }
        }
        else if (cn == STR("Int8Property") || cn == STR("Int16Property") ||
                 cn == STR("IntProperty")  || cn == STR("UInt16Property") ||
                 cn == STR("UInt32Property"))
        {
            setSelector(EBPValueType::Integer);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Integer"));
                if (dst)
                    *static_cast<int32_t*>(dst->ContainerPtrToValuePtr<void>(typesPtr)) =
                        ReadInt(srcProp, srcProp->ContainerPtrToValuePtr<void>(srcContainer));
            }
        }
        else if (cn == STR("Int64Property") || cn == STR("UInt64Property"))
        {
            setSelector(EBPValueType::Integer64);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Integer64"));
                if (dst)
                {
                    void* srcValue = srcProp->ContainerPtrToValuePtr<void>(srcContainer);
                    int64_t v = (cn == STR("UInt64Property"))
                        ? static_cast<int64_t>(*static_cast<uint64_t*>(srcValue))
                        : *static_cast<int64_t*>(srcValue);
                    *static_cast<int64_t*>(dst->ContainerPtrToValuePtr<void>(typesPtr)) = v;
                }
            }
        }
        else if (cn == STR("FloatProperty") || cn == STR("DoubleProperty"))
        {
            setSelector(EBPValueType::Float);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Float"));
                if (dst)
                {
                    void* srcValue = srcProp->ContainerPtrToValuePtr<void>(srcContainer);
                    float v = (cn == STR("DoubleProperty"))
                        ? static_cast<float>(*static_cast<double*>(srcValue))
                        : *static_cast<float*>(srcValue);
                    *static_cast<float*>(dst->ContainerPtrToValuePtr<void>(typesPtr)) = v;
                }
            }
        }
        else if (cn == STR("NameProperty"))
        {
            setSelector(EBPValueType::Name);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Name"));
                if (dst)
                    srcProp->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(typesPtr),
                                               srcProp->ContainerPtrToValuePtr<void>(srcContainer));
            }
        }
        else if (cn == STR("StrProperty"))
        {
            setSelector(EBPValueType::String);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("String"));
                if (dst)
                    srcProp->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(typesPtr),
                                               srcProp->ContainerPtrToValuePtr<void>(srcContainer));
            }
        }
        else if (cn == STR("TextProperty"))
        {
            setSelector(EBPValueType::Text);
            if (srcContainer)
            {
                FProperty* dst = FindStructField(typesStruct, STR("Text"));
                if (dst)
                    srcProp->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(typesPtr),
                                               srcProp->ContainerPtrToValuePtr<void>(srcContainer));
            }
        }
        else if (cn == STR("StructProperty"))
        {
            auto* sp = static_cast<FStructProperty*>(srcProp);
            const StringType sn = sp->GetStruct()->GetName();
            if (sn == STR("Vector") || sn == STR("Vector3d") || sn == STR("Vector3f"))
            {
                setSelector(EBPValueType::Vector);
                if (srcContainer)
                {
                    FProperty* dst = FindStructField(typesStruct, STR("Vector"));
                    if (dst)
                        srcProp->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(typesPtr),
                                                   srcProp->ContainerPtrToValuePtr<void>(srcContainer));
                }
            }
            else if (sn == STR("Rotator") || sn == STR("Rotator3d") || sn == STR("Rotator3f"))
            {
                setSelector(EBPValueType::Rotator);
                if (srcContainer)
                {
                    FProperty* dst = FindStructField(typesStruct, STR("Rotator"));
                    if (dst)
                        srcProp->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(typesPtr),
                                                   srcProp->ContainerPtrToValuePtr<void>(srcContainer));
                }
            }
            else if (sn == STR("Transform") || sn == STR("Transform3d") || sn == STR("Transform3f"))
            {
                setSelector(EBPValueType::Transform);
                if (srcContainer)
                {
                    FProperty* dst = FindStructField(typesStruct, STR("Transform"));
                    if (dst)
                        srcProp->CopyCompleteValue(dst->ContainerPtrToValuePtr<void>(typesPtr),
                                                   srcProp->ContainerPtrToValuePtr<void>(srcContainer));
                }
            }
            // Other struct types: selector stays at default (Boolean/0), value stays zero.
        }
        // ObjectProperty and other unsupported types: left as zero/default.
    }

    // Write one UE4SS_BP_PROPERTIES entry.
    // `dest` points to an already-initialized struct instance.
    // `srcProp` and `srcContainer` are used to populate Value; pass srcContainer=null
    //   for function parameters (type info only, value stays zero).
    static void WriteBPProperty(void* dest, UScriptStruct* propStruct, UScriptStruct* typesStruct,
                                const FName& name, uintptr_t address,
                                FProperty* srcProp, void* srcContainer)
    {
        // Name field (FName)
        FProperty* nameProp = FindStructField(propStruct, STR("Name"));
        if (nameProp)
            *static_cast<FName*>(nameProp->ContainerPtrToValuePtr<void>(dest)) = name;

        // Address field (FString)
        FProperty* addrProp = FindStructField(propStruct, STR("Address"));
        if (addrProp)
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"0x%llX", static_cast<unsigned long long>(address));
            *static_cast<FString*>(addrProp->ContainerPtrToValuePtr<void>(dest)) = FString(buf);
        }

        // Value field (UE4SS_BP_TYPES struct)
        if (srcProp)
        {
            FProperty* valueProp = FindStructField(propStruct, STR("Value"));
            if (valueProp)
                WriteBPTypes(valueProp->ContainerPtrToValuePtr<void>(dest),
                             typesStruct, srcProp, srcContainer);
        }
    }

    // Write one UE4SS_BP_FUNCTIONS entry.
    // Populates Name, Address, and Inputs array.
    // Outputs are not stored here — ExecuteFunction/ExecuteFunctionForMulti
    // collect live output values into their own Results out-pin after calling.
    static void WriteBPFunction(void* dest, UScriptStruct* fnStruct, UScriptStruct* propStruct,
                                UScriptStruct* typesStruct, UFunction* ufn)
    {
        // Name (FName)
        FProperty* nameProp = FindStructField(fnStruct, STR("Name"));
        if (nameProp)
            *static_cast<FName*>(nameProp->ContainerPtrToValuePtr<void>(dest)) =
                FName(ufn->GetName().c_str());

        // Address (FString — pointer to the UFunction object)
        FProperty* addrProp = FindStructField(fnStruct, STR("Address"));
        if (addrProp)
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"0x%llX",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ufn)));
            *static_cast<FString*>(addrProp->ContainerPtrToValuePtr<void>(dest)) = FString(buf);
        }

        // Collect input-only parameters, excluding unsupported types.
        // CPF_OutParm parameters are skipped — callers get output values via Results.
        std::vector<FProperty*> inputs;
        for (FProperty* p : ufn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            if (p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;
            const StringType cn = p->GetClass().GetFName().ToString();
            if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                cn == STR("MulticastDelegateProperty") ||
                cn == STR("MulticastInlineDelegateProperty") ||
                cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                cn == STR("InterfaceProperty"))
                continue;
            inputs.push_back(p);
        }

        FProperty* arrProp = FindStructField(fnStruct, STR("Inputs"));
        if (!arrProp) return;
        auto* arrFProp = static_cast<FArrayProperty*>(arrProp);
        FScriptArrayHelper helper(arrFProp, arrProp->ContainerPtrToValuePtr<void>(dest));
        helper.AddValues(static_cast<int32_t>(inputs.size()));
        for (int32_t i = 0; i < static_cast<int32_t>(inputs.size()); ++i)
        {
            FProperty* param = inputs[i];
            WriteBPProperty(
                helper.GetRawPtr(i), propStruct, typesStruct,
                FName(param->GetName().c_str()),
                reinterpret_cast<uintptr_t>(param),
                param,
                nullptr); // no container — type info only for parameters
        }
    }

    // Populate a UE4SS_BP_ACTOR struct from a live actor.
    // `dest` must point to an already-initialized struct instance of s_bpStructs.Actor size.
    // `filterEngine` = true → only include properties/functions from user/Blueprint class levels.
    static void WriteActorStruct(void* dest, UObject* actor, bool filterEngine = false)
    {
        UScriptStruct* actorStruct = s_bpStructs.Actor;
        UScriptStruct* fnStruct    = s_bpStructs.Functions;
        UScriptStruct* propStruct  = s_bpStructs.Properties;
        UScriptStruct* typesStruct = s_bpStructs.Types;

        // Name (FName)
        FProperty* nameProp = FindStructField(actorStruct, STR("Name"));
        if (nameProp)
            *static_cast<FName*>(nameProp->ContainerPtrToValuePtr<void>(dest)) =
                FName(actor->GetName().c_str());

        // Address (FString)
        FProperty* addrProp = FindStructField(actorStruct, STR("Address"));
        if (addrProp)
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"0x%llX",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(actor)));
            *static_cast<FString*>(addrProp->ContainerPtrToValuePtr<void>(dest)) = FString(buf);
        }

        // ActorPointer (UObject*)
        FProperty* ptrProp = FindStructField(actorStruct, STR("ActorPointer"));
        if (ptrProp)
        {
            auto* op = static_cast<FObjectPropertyBase*>(ptrProp);
            op->SetObjectPropertyValue(ptrProp->ContainerPtrToValuePtr<void>(dest), actor);
        }

        // Properties array — one entry per non-param property on the actor's class
        FProperty* propsProp = FindStructField(actorStruct, STR("Properties"));
        if (propsProp)
        {
            UClass* cls = actor->GetClassPrivate();
            if (cls)
            {
                // When filterEngine is on, collect properties from the engine boundary
                // (the first non-user class in the hierarchy) so we can exclude them.
                std::unordered_set<FProperty*> engineProps;
                if (filterEngine)
                {
                    for (UStruct* s = cls->GetSuperStruct(); s; s = s->GetSuperStruct())
                    {
                        if (!IsUserMadeClass(s))
                        {
                            for (FProperty* p : s->ForEachProperty())
                                engineProps.insert(p);
                            break;
                        }
                    }
                }

                // Collect eligible properties first so we know the count.
                std::vector<FProperty*> eligible;
                for (FProperty* p : cls->ForEachProperty())
                {
                    if (p->HasAnyPropertyFlags(CPF_Parm)) continue;
                    if (filterEngine && engineProps.count(p)) continue;
                    const StringType cn = p->GetClass().GetFName().ToString();
                    // Skip unsupported containers.
                    if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                        cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                        cn == STR("MulticastDelegateProperty") ||
                        cn == STR("MulticastInlineDelegateProperty") ||
                        cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                        cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                        cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                        cn == STR("InterfaceProperty"))
                        continue;
                    eligible.push_back(p);
                }

                auto* arrFProp = static_cast<FArrayProperty*>(propsProp);
                FScriptArrayHelper helper(arrFProp,
                                         propsProp->ContainerPtrToValuePtr<void>(dest));
                helper.AddValues(static_cast<int32_t>(eligible.size()));
                for (int32_t i = 0; i < static_cast<int32_t>(eligible.size()); ++i)
                {
                    FProperty* p = eligible[i];
                    WriteBPProperty(
                        helper.GetRawPtr(i), propStruct, typesStruct,
                        FName(p->GetName().c_str()),
                        reinterpret_cast<uintptr_t>(p->ContainerPtrToValuePtr<void>(actor)),
                        p,
                        actor);
                }
            }
        }

        // Functions array — one entry per unique UFunction in the class hierarchy.
        // With filterEngine, only include functions from user/Blueprint class levels.
        FProperty* fnsProp = FindStructField(actorStruct, STR("Functions"));
        if (fnsProp)
        {
            UClass* cls = actor->GetClassPrivate();
            if (cls)
            {
                std::vector<UFunction*> fns;
                std::unordered_set<std::wstring> seen;
                for (UStruct* s = cls; s; s = s->GetSuperStruct())
                {
                    if (filterEngine && !IsUserMadeClass(s)) continue;
                    for (UFunction* ufn : s->ForEachFunction())
                    {
                        std::wstring fn_name(ufn->GetName().c_str());
                        if (seen.count(fn_name)) continue;
                        seen.insert(fn_name);
                        fns.push_back(ufn);
                    }
                }

                auto* arrFProp = static_cast<FArrayProperty*>(fnsProp);
                FScriptArrayHelper helper(arrFProp,
                                         fnsProp->ContainerPtrToValuePtr<void>(dest));
                helper.AddValues(static_cast<int32_t>(fns.size()));
                for (int32_t i = 0; i < static_cast<int32_t>(fns.size()); ++i)
                    WriteBPFunction(helper.GetRawPtr(i), fnStruct, propStruct, typesStruct, fns[i]);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Handlers
    // ─────────────────────────────────────────────────────────────────────────

    // GetLevels() → TArray<String>
    //   Returns the short name of every level found in the asset registry.
    //   Pure node — served instantly from the lazily-built static cache.
    static void Handle_GetLevels(UObject*, UFunction* fn, void* parms)
    {
        BuildStaticCache(); // no-op after first call

        auto* ret = FindOutParam<TArray<FString>>(fn, parms, STR("ArrayProperty"));
        if (!ret) return;

        for (const auto& name : s_staticCache.levels)
            ret->Add(FString(name.c_str()));

        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("success"));
        if (outSuccess) *outSuccess = true;
    }

    // Populate a UE4SS_BP_CLASS struct from a UClass*.
    // `dest` must point to an already-initialized struct instance of s_bpStructs.Class size.
    // `filterEngine` = true → only include properties/functions from user/Blueprint class levels.
    static void WriteClassStruct(void* dest, UClass* cls, bool filterEngine = false)
    {
        UScriptStruct* clsStruct   = s_bpStructs.Class;
        UScriptStruct* fnStruct    = s_bpStructs.Functions;
        UScriptStruct* propStruct  = s_bpStructs.Properties;
        UScriptStruct* typesStruct = s_bpStructs.Types;

        // Name (FName)
        FProperty* nameProp = FindStructField(clsStruct, STR("Name"));
        if (nameProp)
            *static_cast<FName*>(nameProp->ContainerPtrToValuePtr<void>(dest)) =
                FName(cls->GetName().c_str());

        // Address (FString) — hex pointer to the UClass
        FProperty* addrProp = FindStructField(clsStruct, STR("Address"));
        if (addrProp)
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"0x%llX",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cls)));
            *static_cast<FString*>(addrProp->ContainerPtrToValuePtr<void>(dest)) = FString(buf);
        }

        // ClassReference (TSubclassOf<AActor> / object property holding UClass*)
        FProperty* refProp = FindStructField(clsStruct, STR("ClassReference"));
        if (refProp)
        {
            auto* op = static_cast<FObjectPropertyBase*>(refProp);
            op->SetObjectPropertyValue(refProp->ContainerPtrToValuePtr<void>(dest), cls);
        }

       

        // DefaultProperties — CDO properties (same filter logic as WriteActorStruct)
        FProperty* propsProp = FindStructField(clsStruct, STR("DefaultProperties"));
        if (propsProp)
        {
            UObject* cdo = cls->GetClassDefaultObject();
            if (cdo)
            {
                // Build the engine-property exclusion set when filterEngine is on
                std::unordered_set<FProperty*> engineProps;
                if (filterEngine)
                {
                    for (UStruct* s = cls->GetSuperStruct(); s; s = s->GetSuperStruct())
                    {
                        if (!IsUserMadeClass(s))
                        {
                            for (FProperty* p : s->ForEachProperty())
                                engineProps.insert(p);
                            break;
                        }
                    }
                }

                std::vector<FProperty*> eligible;
                for (FProperty* p : cls->ForEachProperty())
                {
                    StringType test = p->GetName();
                    if (p->HasAnyPropertyFlags(CPF_Parm)) continue;
                    if (filterEngine && engineProps.count(p)) continue;
                    const StringType cn = p->GetClass().GetFName().ToString();
                    if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                        cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                        cn == STR("MulticastDelegateProperty") ||
                        cn == STR("MulticastInlineDelegateProperty") ||
                        cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                        cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                        cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                        cn == STR("InterfaceProperty"))
                        continue;
                    eligible.push_back(p);
                }

                auto* arrFProp = static_cast<FArrayProperty*>(propsProp);
                FScriptArrayHelper helper(arrFProp,
                                         propsProp->ContainerPtrToValuePtr<void>(dest));
                helper.AddValues(static_cast<int32_t>(eligible.size()));
                for (int32_t i = 0; i < static_cast<int32_t>(eligible.size()); ++i)
                {
                    FProperty* p = eligible[i];
                    WriteBPProperty(
                        helper.GetRawPtr(i), propStruct, typesStruct,
                        FName(p->GetName().c_str()),
                        reinterpret_cast<uintptr_t>(p->ContainerPtrToValuePtr<void>(cdo)),
                        p,
                        cdo);
                }
            }
        }

      

        // DefaultFunctions — class hierarchy; with filterEngine only user class levels
        
        FProperty* fnsProp = FindStructField(clsStruct, STR("DefaultFunctions"));
        if (fnsProp)
        {
            std::vector<UFunction*> fns;
            std::unordered_set<std::wstring> seen;
            for (UStruct* s = cls; s; s = s->GetSuperStruct())
            {
                if (filterEngine && !IsUserMadeClass(s)) continue;
                for (UFunction* ufn : s->ForEachFunction())
                {
                    std::wstring fn_name(ufn->GetName().c_str());
                    if (seen.count(fn_name)) continue;
                    seen.insert(fn_name);
                    fns.push_back(ufn);
                }
            }

            auto* arrFProp = static_cast<FArrayProperty*>(fnsProp);
            FScriptArrayHelper helper(arrFProp,
                                      fnsProp->ContainerPtrToValuePtr<void>(dest));
            helper.AddValues(static_cast<int32_t>(fns.size()));
            for (int32_t i = 0; i < static_cast<int32_t>(fns.size()); ++i)
                WriteBPFunction(helper.GetRawPtr(i), fnStruct, propStruct, typesStruct, fns[i]);
        }


        
    }

    // GetAllActorClasses(FilterEngine: bool) → TArray<UE4SS_BP_CLASS>
    //   Returns every user/Blueprint UClass that derives from AActor.
    //   Pure node — served instantly from the lazily-built static cache.
    static void Handle_GetAllActorClasses(UObject*, UFunction* fn, void* parms)
    {
        BuildStaticCache(); // no-op after first call
        if (!EnsureBPStructs() || !s_bpStructs.Class) return;

        // Locate the ArrayProperty out-param (TArray<UE4SS_BP_CLASS>)
        FProperty* arrProp = nullptr;
        void*      arrRaw  = nullptr;
        for (FProperty* op : fn->ForEachProperty())
        {
            if (!op->HasAnyPropertyFlags(CPF_OutParm)) continue;
            if (op->GetClass().GetFName().ToString() != STR("ArrayProperty")) continue;
            arrProp = op;
            arrRaw  = static_cast<uint8_t*>(parms) + op->GetOffset_Internal();
            break;
        }
        if (!arrProp || !arrRaw) return;

        // "FilterEngine" input — when true, omit engine-inherited properties/functions
        bool filterEngine = false;
        auto* filterSlot = ParamPtr<bool>(fn, parms, STR("FilterEngine"));
        if (filterSlot) filterEngine = *filterSlot;

        UScriptStruct* clsStruct    = s_bpStructs.Class;
        UScriptStruct* propStruct   = s_bpStructs.Properties;
        UScriptStruct* typesStruct  = s_bpStructs.Types;
        UScriptStruct* fnStruct     = s_bpStructs.Functions;

        const int32_t clsSz   = clsStruct  ? clsStruct->GetStructureSize()  : 0;
        const int32_t propSz  = propStruct  ? propStruct->GetStructureSize() : 0;
        const int32_t typesSz = typesStruct ? typesStruct->GetStructureSize(): 0;
        const int32_t fnSz    = fnStruct    ? fnStruct->GetStructureSize()   : 0;

        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetAllActorClasses: {} classes, FilterEngine={}\n"
                "  UE4SS_BP_CLASS size    = {} bytes\n"
                "  UE4SS_BP_PROPERTIES sz = {} bytes\n"
                "  UE4SS_BP_TYPES size    = {} bytes\n"
                "  UE4SS_BP_FUNCTIONS sz  = {} bytes\n"),
            s_staticCache.actorClasses.size(), filterEngine,
            clsSz, propSz, typesSz, fnSz);

        // Pre-pass: count props and functions per class, log the top offenders
        int64_t totalProps = 0, totalFns = 0;
        int64_t estimatedBytes = 0;
        for (UObject* obj : s_staticCache.actorClasses)
        {
            auto* c = static_cast<UClass*>(obj);

            // Count properties (with engine exclusion set if filterEngine)
            std::unordered_set<FProperty*> engineProps;
            if (filterEngine)
            {
                for (UStruct* s = c->GetSuperStruct(); s; s = s->GetSuperStruct())
                {
                    if (!IsUserMadeClass(s))
                    {
                        for (FProperty* p : s->ForEachProperty()) engineProps.insert(p);
                        break;
                    }
                }
            }
            int32_t propCount = 0;
            for (FProperty* p : c->ForEachProperty())
            {
                if (p->HasAnyPropertyFlags(CPF_Parm)) continue;
                if (filterEngine && engineProps.count(p)) continue;
                const StringType cn = p->GetClass().GetFName().ToString();
                if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                    cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                    cn == STR("MulticastDelegateProperty") ||
                    cn == STR("MulticastInlineDelegateProperty") ||
                    cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                    cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                    cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                    cn == STR("InterfaceProperty"))
                    continue;
                ++propCount;
            }

            // Count functions, and also count their filtered params to estimate
            // the true parameter array footprint.
            int32_t fnCount = 0;
            int32_t fnParamCount = 0;
            for (UStruct* s = c; s; s = s->GetSuperStruct())
            {
                if (filterEngine && !IsUserMadeClass(s)) continue;
                for (UFunction* ufn : s->ForEachFunction())
                {
                    ++fnCount;
                    for (FProperty* p : ufn->ForEachProperty())
                    {
                        if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
                        if (p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;
                        const StringType cn = p->GetClass().GetFName().ToString();
                        if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                            cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                            cn == STR("MulticastDelegateProperty") ||
                            cn == STR("MulticastInlineDelegateProperty") ||
                            cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                            cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                            cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                            cn == STR("InterfaceProperty"))
                            continue;
                        ++fnParamCount;
                    }
                }
            }

            totalProps += propCount;
            totalFns   += fnCount;

            // Estimate: struct header + props + fn structs + fn param entries
            int64_t clsEstimate = clsSz
                + (int64_t)propCount    * propSz
                + (int64_t)fnCount      * fnSz
                + (int64_t)fnParamCount * propSz; // each param is a UE4SS_BP_PROPERTIES
            estimatedBytes += clsEstimate;

            Output::send<LogLevel::Normal>(
                STR("[BlueprintAPI]   '{}': props={} fns={} fnParams={} est={}B\n"),
                c->GetName().c_str(), propCount, fnCount, fnParamCount, clsEstimate);
        }

        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetAllActorClasses totals: totalProps={} totalFns={} estimatedBytes={} (~{} MB)\n"),
            totalProps, totalFns, estimatedBytes, estimatedBytes / (1024 * 1024));

        auto* arrFProp = static_cast<FArrayProperty*>(arrProp);
        FScriptArrayHelper helper(arrFProp, arrRaw);
        helper.AddValues(static_cast<int32_t>(s_staticCache.actorClasses.size()));

        for (int32_t i = 0; i < static_cast<int32_t>(s_staticCache.actorClasses.size()); ++i)
        {
            void* slot = helper.GetRawPtr(i);
            clsStruct->InitializeStruct(slot);
            WriteClassStruct(slot, static_cast<UClass*>(s_staticCache.actorClasses[i]), filterEngine);
        }

        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));
        if (outSuccess) *outSuccess = true;
    }

    // GetAllActors(Type: Class) → TArray<UE4SS_BP_ACTOR>
    //   Returns every live actor in the current world as fully populated structs.
    //   Optional Type filter narrows results by class.
    static void Handle_GetAllActors(UObject*, UFunction* fn, void* parms)
    {
        // ── Struct resolution ─────────────────────────────────────────────
        bool bActor  = (s_bpStructs.Actor      != nullptr) || (s_bpStructs.Actor      = FindScriptStruct(STR("UE4SS_BP_ACTOR")),      s_bpStructs.Actor      != nullptr);
        bool bFuncs  = (s_bpStructs.Functions  != nullptr) || (s_bpStructs.Functions  = FindScriptStruct(STR("UE4SS_BP_FUNCTIONS")),  s_bpStructs.Functions  != nullptr);
        bool bProps  = (s_bpStructs.Properties != nullptr) || (s_bpStructs.Properties = FindScriptStruct(STR("UE4SS_BP_PROPERTIES")), s_bpStructs.Properties != nullptr);
        bool bTypes  = (s_bpStructs.Types      != nullptr) || (s_bpStructs.Types      = FindScriptStruct(STR("UE4SS_BP_TYPES")),      s_bpStructs.Types      != nullptr);
        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetAllActors structs: Actor={} Functions={} Properties={} Types={}\n"),
            bActor, bFuncs, bProps, bTypes);
        if (!bActor || !bFuncs || !bProps || !bTypes) return;

        // ── Type filter ───────────────────────────────────────────────────
        auto* typeSlot = ParamPtr<UObject*>(fn, parms, STR("Type"));
        UClass* cls = (typeSlot && *typeSlot) ? static_cast<UClass*>(*typeSlot) : AActor::StaticClass();
        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] GetAllActors type filter: {}\n"),
            cls ? cls->GetName().c_str() : STR("(null)"));

        // ── FilterEngine ──────────────────────────────────────────────────
        bool filterEngine = false;
        auto* filterSlot = ParamPtr<bool>(fn, parms, STR("FilterEngine"));
        if (filterSlot) filterEngine = *filterSlot;

        // ── World ─────────────────────────────────────────────────────────
        UObject* world = FindWorldContext();
        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] GetAllActors world: {}\n"),
            world ? world->GetName().c_str() : STR("(null)"));
        if (!world) return;

        // ── Actor query ───────────────────────────────────────────────────
        TArray<AActor*> actors;
        UGameplayStatics::GetAllActorsOfClass(world, cls, actors);
        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] GetAllActors found {} actors\n"), actors.Num());
        if (actors.Num() == 0) return;

        // ── Find the ArrayProperty out-param ──────────────────────────────
        FProperty* outProp = nullptr;
        for (FProperty* p : fn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;
            if (p->GetClass().GetFName().ToString() == STR("ArrayProperty")) { outProp = p; break; }
        }
        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] GetAllActors outProp: {}\n"),
            outProp ? outProp->GetName().c_str() : STR("(null)"));
        if (!outProp) return;

        auto* arrFProp = static_cast<FArrayProperty*>(outProp);
        void* rawArr = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        FScriptArrayHelper helper(arrFProp, rawArr);
        const int32_t added = helper.AddValues(actors.Num());
        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetAllActors helper.Num()={} after AddValues({}), first index={}\n"),
            helper.Num(), actors.Num(), added);

        for (int32_t i = 0; i < actors.Num(); ++i)
        {
            if (!actors[i]) continue;
            WriteActorStruct(helper.GetRawPtr(i), actors[i], filterEngine);
        }
        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] GetAllActors done, wrote {} structs\n"), actors.Num());

        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));
        if (outSuccess) *outSuccess = true;
    }

    // GetActor(Name: String) → UE4SS_BP_ACTOR
    //   Finds the first live actor whose short name matches the input string.
    //   Returns a zero-initialized struct when no match is found.
    static void Handle_GetActor(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs())
        {
            Output::send<LogLevel::Warning>(STR("[BlueprintAPI] GetActor: UE4SS_BP_ACTOR struct not found\n"));
            return;
        }

        auto* nameSlot = ParamPtr<FString>(fn, parms, STR("Name"));
        if (!nameSlot) return;

        const wchar_t* rawName = nameSlot->GetCharArray().GetData();
        if (!rawName || rawName[0] == L'\0') return;
        StringType targetName(rawName);

        UObject* world = FindWorldContext();
        if (!world) return;

        TArray<AActor*> actors;
        UGameplayStatics::GetAllActorsOfClass(world, AActor::StaticClass(), actors);

        UObject* found = nullptr;
        for (int32_t i = 0; i < actors.Num(); ++i)
        {
            if (actors[i] && actors[i]->GetName() == targetName)
            {
                found = actors[i];
                break;
            }
        }
        if (!found) return;

        // The out-param is a StructProperty (single UE4SS_BP_ACTOR instance).
        FProperty* outProp = nullptr;
        for (FProperty* p : fn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;
            if (p->GetClass().GetFName().ToString() == STR("StructProperty")) { outProp = p; break; }
        }
        if (!outProp) return;

        void* dest = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        WriteActorStruct(dest, found);
    }

    // GetFunctionProperties(Actor: Object, FunctionName: String)
    //   Finds the named function on the actor's class and returns its parameter layout.
    //   Out: InputInts     (Map<String, int>)    — input params of each int type
    //   Out: InputFloats   (Map<String, float>)  — input params of each float type
    //   Out: InputBools    (Map<String, bool>)   — input params of each bool type
    //   Out: InputStrings  (Map<String, String>) — input params of each string/name type
    //   Out: OutputInts    (Map<String, int>)    — output params of each int type
    //   Out: OutputFloats  (Map<String, float>)
    //   Out: OutputBools   (Map<String, bool>)
    //   Out: OutputStrings (Map<String, String>)
    static void Handle_GetFunctionProperties(UObject*, UFunction* fn, void* parms)
    {
        auto* actorSlot = ParamPtr<UObject*>(fn, parms, STR("Actor"));
        auto* fnNameSlot = ParamPtr<FString>(fn, parms, STR("FunctionName"));
        auto* inInts = ParamPtr<TMap<FString, int32_t>>(fn, parms, STR("InputInts"));
        auto* inFloats = ParamPtr<TMap<FString, float>>(fn, parms, STR("InputFloats"));
        auto* inBools = ParamPtr<TMap<FString, bool>>(fn, parms, STR("InputBools"));
        auto* inStrings = ParamPtr<TMap<FString, FString>>(fn, parms, STR("InputStrings"));
        auto* outInts = ParamPtr<TMap<FString, int32_t>>(fn, parms, STR("OutputInts"));
        auto* outFloats = ParamPtr<TMap<FString, float>>(fn, parms, STR("OutputFloats"));
        auto* outBools = ParamPtr<TMap<FString, bool>>(fn, parms, STR("OutputBools"));
        auto* outStrings = ParamPtr<TMap<FString, FString>>(fn, parms, STR("OutputStrings"));

        if (!actorSlot || !*actorSlot || !fnNameSlot) return;

        // Read the function name from parms
        const wchar_t* rawName = fnNameSlot->GetCharArray().GetData();
        if (!rawName) return;
        StringType targetName(rawName);

        // Find the UFunction by short name in the actor's class hierarchy
        UFunction* targetFn = nullptr;
        UClass* cls = (*actorSlot)->GetClassPrivate();
        for (UStruct* s = cls; s && !targetFn; s = s->GetSuperStruct())
        {
            for (UFunction* ufn : s->ForEachFunction())
            {
                if (ufn->GetName() == targetName)
                {
                    targetFn = ufn;
                    break;
                }
            }
        }
        if (!targetFn) return;

        // Walk parameters and sort into input/output maps by type
        for (FProperty* p : targetFn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;

            bool isReturn = p->HasAnyPropertyFlags(CPF_ReturnParm);
            bool isOut = p->HasAnyPropertyFlags(CPF_OutParm) && !isReturn;

            FString key(p->GetName());
            auto* iI = isOut ? outInts : inInts;
            auto* iF = isOut ? outFloats : inFloats;
            auto* iB = isOut ? outBools : inBools;
            auto* iS = isOut ? outStrings : inStrings;
            if (!iI || !iF || !iB || !iS) continue;

            switch (ClassifyProp(p))
            {
            case EPropKind::Int:
                iI->Add(key, 0);
                break;
            case EPropKind::Float:
                iF->Add(key, 0.0f);
                break;
            case EPropKind::Bool:
                iB->Add(key, false);
                break;
            case EPropKind::Str: // fall through — report the property type name
            case EPropKind::Name:
                iS->Add(key, FString(p->GetClass().GetFName().ToString()));
                break;
            case EPropKind::Object:
                iS->Add(key, FString(STR("Object")));
                break;
            default:
                break;
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // FScriptMap helpers — type-erased add and find for TMap<int32, T>
    //
    // UE4SS doesn't ship FScriptMapHelper, so we drive FScriptMap directly.
    // All four hook functions (hash, equality, ctor, dtor) are delegated to
    // the FProperty vtable so correctness is handled by the engine.
    // ─────────────────────────────────────────────────────────────────────────

    // Add a (int32 key, value) pair to a FScriptMap backed by `mapProp`.
    // `value` must point to an initialized instance of the value type.
    static void ScriptMapAdd(FScriptMap* map, FMapProperty* mapProp,
                             int32_t key, const void* value)
    {
        FProperty* keyProp = mapProp->GetKeyProp();
        FProperty* valProp = mapProp->GetValueProp();
        const FScriptMapLayout& layout = mapProp->GetMapLayout();

        map->Add(
            &key, value, layout,
            [keyProp](const void* k)           { return keyProp->GetValueTypeHash(k); },
            [keyProp](const void* a, const void* b) { return keyProp->Identical(a, b, 0); },
            [keyProp, &key](void* dest)         { keyProp->CopyCompleteValue(dest, &key); },
            [valProp, value](void* dest)        { valProp->InitializeValue(dest); valProp->CopyCompleteValue(dest, value); },
            [valProp, value](void* dest)        { valProp->CopyCompleteValue(dest, value); },
            [keyProp](void* dest)               { keyProp->DestroyValue(dest); },
            [valProp](void* dest)               { valProp->DestroyValue(dest); }
        );
    }

    // Return a pointer to the value in `map` for the given int32 key, or null.
    static void* ScriptMapFind(FScriptMap* map, FMapProperty* mapProp, int32_t key)
    {
        FProperty* keyProp = mapProp->GetKeyProp();
        const FScriptMapLayout& layout = mapProp->GetMapLayout();
        return map->FindValue(
            &key, layout,
            [keyProp](const void* k)            { return keyProp->GetValueTypeHash(k); },
            [keyProp](const void* a, const void* b) { return keyProp->Identical(a, b, 0); }
        );
    }

    // Iterate every valid (key, value) pair in `map`. Callback receives
    // (key_ptr, value_ptr) for each live entry.
    static void ScriptMapForEach(FScriptMap* map, FMapProperty* mapProp,
                                 const std::function<void(const void*, void*)>& cb)
    {
        const FScriptMapLayout& layout = mapProp->GetMapLayout();
        const int32_t maxIdx = map->GetMaxIndex();
        for (int32_t i = 0; i < maxIdx; ++i)
        {
            if (!map->IsValidIndex(i)) continue;
            auto* pair = static_cast<uint8_t*>(map->GetData(i, layout));
            cb(pair, pair + layout.ValueOffset);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // UE4SS_BP_STRUCT helpers
    // ─────────────────────────────────────────────────────────────────────────

    // Given a UScriptStruct and a source container, populate a UE4SS_BP_STRUCT
    // instance at `dest`. All HasBeenChanged entries start as false.
    static void ConvertStructToUE4SS(void* dest, UScriptStruct* srcStructType, void* srcData)
    {
        UScriptStruct* bpStruct   = s_bpStructs.Struct;
        UScriptStruct* propStruct = s_bpStructs.Properties;
        UScriptStruct* typeStruct = s_bpStructs.Types;

        // Name — FName of the source struct type
        FProperty* nameProp = FindStructField(bpStruct, STR("Name"));
        if (nameProp)
            *static_cast<FName*>(nameProp->ContainerPtrToValuePtr<void>(dest)) =
                FName(srcStructType->GetName().c_str());

        // Address — hex address of the source struct instance
        FProperty* addrProp = FindStructField(bpStruct, STR("Address"));
        if (addrProp)
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"0x%llX",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(srcData)));
            *static_cast<FString*>(addrProp->ContainerPtrToValuePtr<void>(dest)) = FString(buf);
        }

        // Collect eligible properties (same filter as WriteActorStruct)
        std::vector<FProperty*> fields;
        for (FProperty* p : srcStructType->ForEachProperty())
        {
            const StringType cn = p->GetClass().GetFName().ToString();
            if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                cn == STR("MulticastDelegateProperty") ||
                cn == STR("MulticastInlineDelegateProperty") ||
                cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                cn == STR("InterfaceProperty"))
                continue;
            fields.push_back(p);
        }

        // Structure field: TMap<int32, UE4SS_BP_PROPERTIES>
        FProperty* structureProp = FindStructField(bpStruct, STR("Structure"));
        FProperty* changedProp   = FindStructField(bpStruct, STR("HasBeenChanged"));

        FMapProperty* structMapProp  = structureProp ? static_cast<FMapProperty*>(structureProp) : nullptr;
        FMapProperty* changedMapProp = changedProp   ? static_cast<FMapProperty*>(changedProp)   : nullptr;

        if (!structMapProp || !changedMapProp) return;

        FScriptMap* structMap  = static_cast<FScriptMap*>(structureProp->ContainerPtrToValuePtr<void>(dest));
        FScriptMap* changedMap = static_cast<FScriptMap*>(changedProp->ContainerPtrToValuePtr<void>(dest));

        // We need a temporary buffer for one UE4SS_BP_PROPERTIES instance.
        const int32_t propSize = propStruct->GetStructureSize();
        std::vector<uint8_t> propBuf(static_cast<size_t>(propSize));
        propStruct->InitializeStruct(propBuf.data());

        for (int32_t i = 0; i < static_cast<int32_t>(fields.size()); ++i)
        {
            FProperty* field = fields[i];

            // Populate the temporary UE4SS_BP_PROPERTIES buffer
            propStruct->InitializeStruct(propBuf.data()); // re-init for each entry
            WriteBPProperty(propBuf.data(), propStruct, typeStruct,
                            FName(field->GetName().c_str()),
                            reinterpret_cast<uintptr_t>(field->ContainerPtrToValuePtr<void>(srcData)),
                            field, srcData);

            // Insert into Structure map
            ScriptMapAdd(structMap, structMapProp, i, propBuf.data());

            // Insert false into HasBeenChanged map
            bool notChanged = false;
            ScriptMapAdd(changedMap, changedMapProp, i, &notChanged);
        }

        propStruct->DestroyStruct(propBuf.data());
    }

    // Apply changed fields from a UE4SS_BP_STRUCT back onto the original struct memory.
    // Returns true when at least one field was applied.
    static bool FlashStructFromUE4SS(const void* src, UObject* owner)
    {
        UScriptStruct* bpStruct   = s_bpStructs.Struct;
        UScriptStruct* propStruct = s_bpStructs.Properties;
        UScriptStruct* typeStruct = s_bpStructs.Types;

        // Read Address — locate the original struct in memory
        FProperty* addrProp = FindStructField(bpStruct, STR("Address"));
        if (!addrProp) return false;
        const FString& addrStr = *static_cast<const FString*>(addrProp->ContainerPtrToValuePtr<const void>(src));
        const wchar_t* addrRaw = addrStr.GetCharArray().GetData();
        if (!addrRaw || addrRaw[0] == L'\0') return false;

        uintptr_t rawAddr = 0;
        // Parse hex address (may or may not have 0x prefix)
        const wchar_t* parse = addrRaw;
        if (parse[0] == L'0' && (parse[1] == L'x' || parse[1] == L'X')) parse += 2;
        for (; *parse; ++parse)
        {
            rawAddr <<= 4;
            if      (*parse >= L'0' && *parse <= L'9') rawAddr |= static_cast<uintptr_t>(*parse - L'0');
            else if (*parse >= L'a' && *parse <= L'f') rawAddr |= static_cast<uintptr_t>(*parse - L'a' + 10);
            else if (*parse >= L'A' && *parse <= L'F') rawAddr |= static_cast<uintptr_t>(*parse - L'A' + 10);
        }
        if (!rawAddr) return false;
        void* structData = reinterpret_cast<void*>(rawAddr);

        // Read Name — find UScriptStruct to resolve property layout
        FProperty* nameProp = FindStructField(bpStruct, STR("Name"));
        if (!nameProp) return false;
        const FName& structName = *static_cast<const FName*>(nameProp->ContainerPtrToValuePtr<const void>(src));
        StringType structNameStr = structName.ToString();

        // Find the UScriptStruct type so we can look up property offsets by name
        UScriptStruct* srcStructType = FindScriptStruct(structNameStr);
        if (!srcStructType)
        {
            // Also try native structs — find FProperty on owner's class that matches the address
            // and use its struct type
            if (owner)
            {
                UClass* cls = owner->GetClassPrivate();
                for (FProperty* p : cls->ForEachProperty())
                {
                    const StringType cn = p->GetClass().GetFName().ToString();
                    if (cn != STR("StructProperty")) continue;
                    if (reinterpret_cast<uintptr_t>(p->ContainerPtrToValuePtr<void>(owner)) == rawAddr)
                    {
                        srcStructType = static_cast<FStructProperty*>(p)->GetStruct();
                        break;
                    }
                }
            }
            if (!srcStructType) return false;
        }

        // Get HasBeenChanged and Structure map fields
        FProperty* changedProp   = FindStructField(bpStruct, STR("HasBeenChanged"));
        FProperty* structureProp = FindStructField(bpStruct, STR("Structure"));
        if (!changedProp || !structureProp) return false;

        FScriptMap* changedMap = static_cast<FScriptMap*>(changedProp->ContainerPtrToValuePtr<void>(const_cast<void*>(src)));
        FScriptMap* structMap  = static_cast<FScriptMap*>(structureProp->ContainerPtrToValuePtr<void>(const_cast<void*>(src)));
        FMapProperty* changedMapProp = static_cast<FMapProperty*>(changedProp);
        FMapProperty* structMapProp  = static_cast<FMapProperty*>(structureProp);

        bool anyApplied = false;

        ScriptMapForEach(changedMap, changedMapProp, [&](const void* keyPtr, void* valPtr) {
            if (!*static_cast<const bool*>(valPtr)) return; // not changed

            int32_t idx = *static_cast<const int32_t*>(keyPtr);
            void* propEntry = ScriptMapFind(structMap, structMapProp, idx);
            if (!propEntry) return;

            // Read the field name from UE4SS_BP_PROPERTIES.Name
            FProperty* entryNameProp = FindStructField(propStruct, STR("Name"));
            if (!entryNameProp) return;
            const FName& fieldName = *static_cast<const FName*>(
                entryNameProp->ContainerPtrToValuePtr<void>(propEntry));
            StringType fieldNameStr = fieldName.ToString();

            // Find the matching FProperty on the source struct by name (case-insensitive)
            FProperty* targetProp = nullptr;
            for (FProperty* p : srcStructType->ForEachProperty())
            {
                if (PropertyNameMatches(p->GetName(), fieldNameStr)) { targetProp = p; break; }
            }
            if (!targetProp) return;

            void* targetAddr = static_cast<uint8_t*>(structData) + targetProp->GetOffset_Internal();

            // Read UE4SS_BP_TYPES.TypeSelector and write the right field
            FProperty* valueProp = FindStructField(propStruct, STR("Value"));
            if (!valueProp) return;
            void* typesPtr = valueProp->ContainerPtrToValuePtr<void>(propEntry);

            FProperty* selectorProp = FindStructField(typeStruct, STR("TypeSelector"));
            if (!selectorProp) return;
            auto sel = static_cast<EBPValueType>(
                *static_cast<uint8_t*>(selectorProp->ContainerPtrToValuePtr<void>(typesPtr)));

            const StringType tgtClass = targetProp->GetClass().GetFName().ToString();

            auto copyTypesField = [&](const StringType& fieldPrefix) {
                FProperty* src = FindStructField(typeStruct, fieldPrefix);
                if (src) targetProp->CopyCompleteValue(targetAddr,
                             src->ContainerPtrToValuePtr<void>(typesPtr));
            };

            switch (sel)
            {
            case EBPValueType::Boolean: {
                if (tgtClass == STR("BoolProperty"))
                {
                    FProperty* boolSrc = FindStructField(typeStruct, STR("Boolean"));
                    if (boolSrc)
                        static_cast<FBoolProperty*>(targetProp)->SetPropertyValue(
                            targetAddr,
                            *static_cast<bool*>(boolSrc->ContainerPtrToValuePtr<void>(typesPtr)));
                }
                break;
            }
            case EBPValueType::Byte: {
                FProperty* byteSrc = FindStructField(typeStruct, STR("Byte"));
                if (byteSrc) std::memcpy(targetAddr, byteSrc->ContainerPtrToValuePtr<void>(typesPtr), 1);
                break;
            }
            case EBPValueType::Integer: {
                FProperty* intSrc = FindStructField(typeStruct, STR("Integer"));
                if (intSrc)
                {
                    int32_t v = *static_cast<int32_t*>(intSrc->ContainerPtrToValuePtr<void>(typesPtr));
                    std::memcpy(targetAddr, &v, static_cast<size_t>(targetProp->GetSize()));
                }
                break;
            }
            case EBPValueType::Integer64: {
                FProperty* i64Src = FindStructField(typeStruct, STR("Integer64"));
                if (i64Src)
                {
                    int64_t v = *static_cast<int64_t*>(i64Src->ContainerPtrToValuePtr<void>(typesPtr));
                    std::memcpy(targetAddr, &v, static_cast<size_t>(targetProp->GetSize()));
                }
                break;
            }
            case EBPValueType::Float: {
                FProperty* floatSrc = FindStructField(typeStruct, STR("Float"));
                if (floatSrc)
                {
                    float v = *static_cast<float*>(floatSrc->ContainerPtrToValuePtr<void>(typesPtr));
                    if (tgtClass == STR("DoubleProperty"))
                    { double d = static_cast<double>(v); std::memcpy(targetAddr, &d, 8); }
                    else std::memcpy(targetAddr, &v, 4);
                }
                break;
            }
            case EBPValueType::Name:      copyTypesField(STR("Name"));      break;
            case EBPValueType::String:    copyTypesField(STR("String"));    break;
            case EBPValueType::Text:      copyTypesField(STR("Text"));      break;
            case EBPValueType::Vector:    copyTypesField(STR("Vector"));    break;
            case EBPValueType::Rotator:   copyTypesField(STR("Rotator"));   break;
            case EBPValueType::Transform: copyTypesField(STR("Transform")); break;
            default: return;
            }
            anyApplied = true;
        });

        return anyApplied;
    }

    // ConvertStruct(Object: UObject*, Struct: FString) → Result: UE4SS_BP_STRUCT
    //   Converts a named struct property on an object into a UE4SS_BP_STRUCT.
    static void Handle_ConvertStruct(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs())
        {
            Output::send<LogLevel::Warning>(STR("[BlueprintAPI] ConvertStruct: required structs not found\n"));
            return;
        }

        auto* objSlot    = ParamPtr<UObject*>(fn, parms, STR("Object"));
        auto* structName = ParamPtr<FString>(fn, parms, STR("Struct"));
        if (!objSlot || !*objSlot || !structName) return;

        UObject* obj = *objSlot;
        const wchar_t* rawName = structName->GetCharArray().GetData();
        if (!rawName || rawName[0] == L'\0') return;
        StringType fieldName(rawName);

        // Find the struct property by name on the object (case-insensitive)
        FStructProperty* srcProp = nullptr;
        UClass* cls = obj->GetClassPrivate();
        for (FProperty* p : cls->ForEachProperty())
        {
            if (p->GetClass().GetFName().ToString() != STR("StructProperty")) continue;
            if (PropertyNameMatches(p->GetName(), fieldName))
            {
                srcProp = static_cast<FStructProperty*>(p); break;
            }
        }
        if (!srcProp) return;

        void* srcData = srcProp->ContainerPtrToValuePtr<void>(obj);
        UScriptStruct* srcType = srcProp->GetStruct();

        // Output: "Result" (UE4SS_BP_STRUCT)
        FProperty* outProp = FindParam(fn, STR("Result"));
        if (!outProp) return;

        void* dest = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        ConvertStructToUE4SS(dest, srcType, srcData);
    }

    // FlashNewStruct(Owner: UObject*, NewStruct: UE4SS_BP_STRUCT) → Success: bool
    //   Applies changed fields from a UE4SS_BP_STRUCT back to the original struct.
    static void Handle_FlashNewStruct(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs())
        {
            Output::send<LogLevel::Warning>(STR("[BlueprintAPI] FlashNewStruct: required structs not found\n"));
            return;
        }

        auto* ownerSlot = ParamPtr<UObject*>(fn, parms, STR("Owner"));
        UObject* owner  = (ownerSlot && *ownerSlot) ? *ownerSlot : nullptr;

        // Input: "NewStruct" (UE4SS_BP_STRUCT with changes applied)
        FProperty* newStructProp = FindParam(fn, STR("NewStruct"));
        if (!newStructProp) return;
        const void* newStructData = static_cast<const uint8_t*>(parms) + newStructProp->GetOffset_Internal();

        bool success = FlashStructFromUE4SS(newStructData, owner);

        auto* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));
        if (outSuccess) *outSuccess = success;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Shared helpers for property set and function execution
    // ─────────────────────────────────────────────────────────────────────────

    // Apply a single UE4SS_BP_PROPERTIES entry to a live UObject.
    // Returns true on success.
    static bool ApplyPropertyToObject(UObject* target, const void* propEntry)
    {
        UScriptStruct* propStruct  = s_bpStructs.Properties;
        UScriptStruct* typeStruct  = s_bpStructs.Types;

        FProperty* entryNameProp = FindStructField(propStruct, STR("Name"));
        if (!entryNameProp) return false;
        const FName& fieldName = *static_cast<const FName*>(
            entryNameProp->ContainerPtrToValuePtr<const void>(propEntry));
        StringType fieldNameStr = fieldName.ToString();

        // Find the FProperty on the target
        FProperty* targetProp = nullptr;
        UClass* cls = target->GetClassPrivate();
        for (FProperty* p : cls->ForEachProperty())
        {
            if (PropertyNameMatches(p->GetName(), fieldNameStr)) { targetProp = p; break; }
        }
        if (!targetProp) return false;

        void* targetAddr = targetProp->ContainerPtrToValuePtr<void>(target);

        FProperty* valueProp    = FindStructField(propStruct, STR("Value"));
        FProperty* selectorProp = valueProp ? FindStructField(typeStruct, STR("TypeSelector")) : nullptr;
        if (!valueProp || !selectorProp) return false;

        void* typesPtr = valueProp->ContainerPtrToValuePtr<void>(const_cast<void*>(propEntry));
        auto sel = static_cast<EBPValueType>(
            *static_cast<uint8_t*>(selectorProp->ContainerPtrToValuePtr<void>(typesPtr)));
        const StringType tgtClass = targetProp->GetClass().GetFName().ToString();

        auto copyTypesField = [&](const StringType& fieldPrefix) {
            FProperty* src = FindStructField(typeStruct, fieldPrefix);
            if (src) targetProp->CopyCompleteValue(targetAddr, src->ContainerPtrToValuePtr<void>(typesPtr));
        };

        switch (sel)
        {
        case EBPValueType::Boolean: {
            if (tgtClass == STR("BoolProperty"))
            {
                FProperty* boolSrc = FindStructField(typeStruct, STR("Boolean"));
                if (boolSrc)
                    static_cast<FBoolProperty*>(targetProp)->SetPropertyValue(
                        targetAddr,
                        *static_cast<bool*>(boolSrc->ContainerPtrToValuePtr<void>(typesPtr)));
            }
            break;
        }
        case EBPValueType::Byte: {
            FProperty* src = FindStructField(typeStruct, STR("Byte"));
            if (src) std::memcpy(targetAddr, src->ContainerPtrToValuePtr<void>(typesPtr), 1);
            break;
        }
        case EBPValueType::Integer: {
            FProperty* src = FindStructField(typeStruct, STR("Integer"));
            if (src)
            {
                int32_t v = *static_cast<int32_t*>(src->ContainerPtrToValuePtr<void>(typesPtr));
                std::memcpy(targetAddr, &v, static_cast<size_t>(targetProp->GetSize()));
            }
            break;
        }
        case EBPValueType::Integer64: {
            FProperty* src = FindStructField(typeStruct, STR("Integer64"));
            if (src)
            {
                int64_t v = *static_cast<int64_t*>(src->ContainerPtrToValuePtr<void>(typesPtr));
                std::memcpy(targetAddr, &v, static_cast<size_t>(targetProp->GetSize()));
            }
            break;
        }
        case EBPValueType::Float: {
            FProperty* src = FindStructField(typeStruct, STR("Float"));
            if (src)
            {
                float v = *static_cast<float*>(src->ContainerPtrToValuePtr<void>(typesPtr));
                if (tgtClass == STR("DoubleProperty")) { double d = static_cast<double>(v); std::memcpy(targetAddr, &d, 8); }
                else std::memcpy(targetAddr, &v, 4);
            }
            break;
        }
        case EBPValueType::Name:      copyTypesField(STR("Name"));      break;
        case EBPValueType::String:    copyTypesField(STR("String"));    break;
        case EBPValueType::Text:      copyTypesField(STR("Text"));      break;
        case EBPValueType::Vector:    copyTypesField(STR("Vector"));    break;
        case EBPValueType::Rotator:   copyTypesField(STR("Rotator"));   break;
        case EBPValueType::Transform: copyTypesField(STR("Transform")); break;
        default: return false;
        }
        return true;
    }

    // Execute a UFunction (looked up by address in UE4SS_BP_FUNCTIONS.Address) on a
    // target object. Collects output parameters into a TArray<UE4SS_BP_PROPERTIES>.
    // Returns true if the call succeeded.
    static bool ExecuteUFunction(UObject* target, const void* fnEntry,
                                 TArray<void*>* outResults, UScriptStruct* propStruct,
                                 UScriptStruct* typeStruct)
    {
        UScriptStruct* fnStruct = s_bpStructs.Functions;

        // Parse function address
        FProperty* addrProp = FindStructField(fnStruct, STR("Address"));
        if (!addrProp) return false;
        const FString& addrStr = *static_cast<const FString*>(
            addrProp->ContainerPtrToValuePtr<const void>(fnEntry));
        const wchar_t* addrRaw = addrStr.GetCharArray().GetData();
        if (!addrRaw || addrRaw[0] == L'\0') return false;

        uintptr_t rawAddr = 0;
        const wchar_t* parse = addrRaw;
        if (parse[0] == L'0' && (parse[1] == L'x' || parse[1] == L'X')) parse += 2;
        for (; *parse; ++parse)
        {
            rawAddr <<= 4;
            if      (*parse >= L'0' && *parse <= L'9') rawAddr |= static_cast<uintptr_t>(*parse - L'0');
            else if (*parse >= L'a' && *parse <= L'f') rawAddr |= static_cast<uintptr_t>(*parse - L'a' + 10);
            else if (*parse >= L'A' && *parse <= L'F') rawAddr |= static_cast<uintptr_t>(*parse - L'A' + 10);
        }
        if (!rawAddr) return false;
        UFunction* ufn = reinterpret_cast<UFunction*>(rawAddr);

        // Allocate and zero parms buffer
        const uint16_t parmsSize = ufn->GetParmsSize();
        if (parmsSize == 0) { target->ProcessEvent(ufn, nullptr); return true; }

        std::vector<uint8_t> parmsBuf(parmsSize, 0);
        uint8_t* parms = parmsBuf.data();

        // Initialize each parameter
        for (FProperty* p : ufn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            p->InitializeValue(parms + p->GetOffset_Internal());
        }

        target->ProcessEvent(ufn, parms);

        // Collect output parameters if caller wants them
        if (outResults)
        {
            for (FProperty* p : ufn->ForEachProperty())
            {
                if (!p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;

                // Allocate a UE4SS_BP_PROPERTIES buffer
                const int32_t propSize = propStruct->GetStructureSize();
                void* propBuf = std::malloc(static_cast<size_t>(propSize));
                propStruct->InitializeStruct(propBuf);

                WriteBPProperty(propBuf, propStruct, typeStruct,
                    FName(p->GetName().c_str()),
                    reinterpret_cast<uintptr_t>(parms + p->GetOffset_Internal()),
                    p,
                    parms); // parms is the container for the output values

                outResults->Add(propBuf);
            }
        }

        // Destroy parameter values
        for (FProperty* p : ufn->ForEachProperty())
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            p->DestroyValue(parms + p->GetOffset_Internal());
        }

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // New handlers
    // ─────────────────────────────────────────────────────────────────────────

    // GetUpdatedActor(Actor: UE4SS_BP_ACTOR) → (Found Actor: UE4SS_BP_ACTOR, Success: bool)
    //   Re-queries the live actor at the stored address and returns a fresh struct.
    static void Handle_GetUpdatedActor(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));

        // Input: "Actor" (UE4SS_BP_ACTOR struct by value)
        FProperty* inProp = FindParam(fn, STR("Actor"));
        if (!inProp) { if (outSuccess) *outSuccess = false; return; }
        const void* actorStructSrc = static_cast<uint8_t*>(parms) + inProp->GetOffset_Internal();

        // Parse Address field to recover the live actor pointer
        FProperty* addrProp = FindStructField(s_bpStructs.Actor, STR("Address"));
        if (!addrProp) { if (outSuccess) *outSuccess = false; return; }
        const FString& addrStr = *static_cast<const FString*>(
            addrProp->ContainerPtrToValuePtr<const void>(actorStructSrc));
        const wchar_t* addrRaw = addrStr.GetCharArray().GetData();
        if (!addrRaw || addrRaw[0] == L'\0') { if (outSuccess) *outSuccess = false; return; }

        uintptr_t rawAddr = 0;
        const wchar_t* parse = addrRaw;
        if (parse[0] == L'0' && (parse[1] == L'x' || parse[1] == L'X')) parse += 2;
        for (; *parse; ++parse)
        {
            rawAddr <<= 4;
            if      (*parse >= L'0' && *parse <= L'9') rawAddr |= static_cast<uintptr_t>(*parse - L'0');
            else if (*parse >= L'a' && *parse <= L'f') rawAddr |= static_cast<uintptr_t>(*parse - L'a' + 10);
            else if (*parse >= L'A' && *parse <= L'F') rawAddr |= static_cast<uintptr_t>(*parse - L'A' + 10);
        }
        if (!rawAddr) { if (outSuccess) *outSuccess = false; return; }
        UObject* actor = reinterpret_cast<UObject*>(rawAddr);

        // Output: "Found Actor" (UE4SS_BP_ACTOR struct)
        FProperty* outProp = FindParam(fn, STR("Found Actor"));
        if (!outProp) { if (outSuccess) *outSuccess = false; return; }

        void* dest = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        // Preserve FilterEngine from the original actor struct isn't possible here,
        // so default to false (full data) for GetUpdatedActor.
        WriteActorStruct(dest, actor, false);
        if (outSuccess) *outSuccess = true;
    }

    // GetFunctionFromLibraries() → (Found Functions: Array<UE4SS_BP_FUNCTIONS>, Success: bool)
    //   Scans all loaded BlueprintFunctionLibrary classes and returns their functions.
    static void Handle_GetFunctionFromLibraries(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        // Output: "Found Functions" (Array<UE4SS_BP_FUNCTIONS>)
        FProperty* outProp = FindParam(fn, STR("Found Functions"));
        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));
        if (!outProp) { if (outSuccess) *outSuccess = false; return; }

        auto* arrFProp = static_cast<FArrayProperty*>(outProp);
        void* rawArr = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        FScriptArrayHelper helper(arrFProp, rawArr);

        UScriptStruct* fnStruct   = s_bpStructs.Functions;
        UScriptStruct* propStruct = s_bpStructs.Properties;
        UScriptStruct* typeStruct = s_bpStructs.Types;

        // Collect all UFunction objects from BlueprintFunctionLibrary subclasses
        std::vector<UFunction*> collected;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
            auto* cls = Cast<UClass>(obj);
            if (!cls) return LoopAction::Continue;
            // Check if this class derives from BlueprintFunctionLibrary by name
            bool isBPFL = false;
            for (UStruct* s = cls; s; s = s->GetSuperStruct())
            {
                if (s->GetName().find(STR("BlueprintFunctionLibrary")) != StringType::npos)
                { isBPFL = true; break; }
            }
            if (!isBPFL) return LoopAction::Continue;
            for (UFunction* ufn : cls->ForEachFunction())
                collected.push_back(ufn);
            return LoopAction::Continue;
        });

        if (collected.empty()) { if (outSuccess) *outSuccess = false; return; }

        helper.AddValues(static_cast<int32_t>(collected.size()));
        for (int32_t i = 0; i < static_cast<int32_t>(collected.size()); ++i)
            WriteBPFunction(helper.GetRawPtr(i), fnStruct, propStruct, typeStruct, collected[i]);

        if (outSuccess) *outSuccess = true;
    }

    // ExecuteFunction(Target: UObject*, Function: UE4SS_BP_FUNCTIONS)
    //   → (Result: Array<UE4SS_BP_PROPERTIES>, Success: bool)
    static void Handle_ExecuteFunction(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        auto* targetSlot = ParamPtr<UObject*>(fn, parms, STR("Target"));
        if (!targetSlot || !*targetSlot) return;
        UObject* target = *targetSlot;

        // Input: "Function" (UE4SS_BP_FUNCTIONS struct)
        FProperty* fnProp = FindParam(fn, STR("Function"));
        if (!fnProp) return;
        const void* fnEntry = static_cast<uint8_t*>(parms) + fnProp->GetOffset_Internal();

        // Output: "Result" (Array<UE4SS_BP_PROPERTIES>)
        FProperty* outProp = FindParam(fn, STR("Result"));
        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));

        // Collect results as raw heap pointers first
        TArray<void*> rawResults;
        bool ok = ExecuteUFunction(target, fnEntry, &rawResults,
                                   s_bpStructs.Properties, s_bpStructs.Types);

        if (outProp && rawResults.Num() > 0)
        {
            auto* arrFProp = static_cast<FArrayProperty*>(outProp);
            void* rawArr   = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
            FScriptArrayHelper helper(arrFProp, rawArr);
            helper.AddValues(rawResults.Num());
            for (int32_t i = 0; i < rawResults.Num(); ++i)
            {
                const int32_t sz = s_bpStructs.Properties->GetStructureSize();
                std::memcpy(helper.GetRawPtr(i), rawResults[i], static_cast<size_t>(sz));
                s_bpStructs.Properties->DestroyStruct(rawResults[i]);
                std::free(rawResults[i]);
            }
        }
        else
        {
            for (int32_t i = 0; i < rawResults.Num(); ++i)
            {
                s_bpStructs.Properties->DestroyStruct(rawResults[i]);
                std::free(rawResults[i]);
            }
        }

        if (outSuccess) *outSuccess = ok;
    }

    // ExecuteFunctionForMulti(Targets: Array<UObject*>, Function: UE4SS_BP_FUNCTIONS)
    //   → (Results: Map<int32, UE4SS_BP_PROPLIST>, Success: bool)
    static void Handle_ExecuteFunctionForMulti(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        // Input: "Targets" (Array<Object>)
        FProperty* targetsProp = FindParam(fn, STR("Targets"));
        if (!targetsProp) return;
        void* targetsArr = static_cast<uint8_t*>(parms) + targetsProp->GetOffset_Internal();
        auto* targetsFProp = static_cast<FArrayProperty*>(targetsProp);
        FScriptArrayHelper targetsHelper(targetsFProp, targetsArr);

        // Input: "Function" (UE4SS_BP_FUNCTIONS struct)
        FProperty* fnProp = FindParam(fn, STR("Function"));
        if (!fnProp) return;
        const void* fnEntry = static_cast<uint8_t*>(parms) + fnProp->GetOffset_Internal();

        // Output: "Results" (TMap<int32, UE4SS_BP_PROPLIST>)
        FProperty* resultsProp = FindParam(fn, STR("Results"));
        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("success"));

        bool anyOk = false;
        for (int32_t ti = 0; ti < targetsHelper.Num(); ++ti)
        {
            UObject* target = *reinterpret_cast<UObject**>(targetsHelper.GetRawPtr(ti));
            if (!target) continue;

            TArray<void*> rawResults;
            if (!ExecuteUFunction(target, fnEntry, &rawResults,
                                  s_bpStructs.Properties, s_bpStructs.Types))
                continue;

            anyOk = true;

            if (resultsProp && rawResults.Num() > 0)
            {
                auto* mapFProp = static_cast<FMapProperty*>(resultsProp);
                FScriptMap* resultsMap = static_cast<FScriptMap*>(
                    resultsProp->ContainerPtrToValuePtr<void>(parms));

                // Build a UE4SS_BP_PROPLIST to hold all output props for this target
                UScriptStruct* plStruct = s_bpStructs.PropList;
                const int32_t plSize    = plStruct->GetStructureSize();
                std::vector<uint8_t> plBuf(static_cast<size_t>(plSize));
                plStruct->InitializeStruct(plBuf.data());

                FProperty* plArrProp = FindStructField(plStruct, STR("Properties"));
                if (plArrProp)
                {
                    auto* plArrFProp = static_cast<FArrayProperty*>(plArrProp);
                    FScriptArrayHelper plHelper(plArrFProp,
                        plArrProp->ContainerPtrToValuePtr<void>(plBuf.data()));
                    plHelper.AddValues(rawResults.Num());
                    for (int32_t i = 0; i < rawResults.Num(); ++i)
                    {
                        const int32_t sz = s_bpStructs.Properties->GetStructureSize();
                        std::memcpy(plHelper.GetRawPtr(i), rawResults[i], static_cast<size_t>(sz));
                    }
                }

                ScriptMapAdd(resultsMap, mapFProp, ti, plBuf.data());
                plStruct->DestroyStruct(plBuf.data());
            }

            for (int32_t i = 0; i < rawResults.Num(); ++i)
            {
                s_bpStructs.Properties->DestroyStruct(rawResults[i]);
                std::free(rawResults[i]);
            }
        }

        if (outSuccess) *outSuccess = anyOk;
    }

    // SetProperty(Targets: UObject*, NewValue: UE4SS_BP_PROPERTIES) → Success: bool
    static void Handle_SetProperty(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        auto* targetSlot = ParamPtr<UObject*>(fn, parms, STR("Targets"));
        if (!targetSlot || !*targetSlot) return;

        // Input: "NewValue" (UE4SS_BP_PROPERTIES struct)
        FProperty* valProp = FindParam(fn, STR("NewValue"));
        if (!valProp) return;
        const void* propEntry = static_cast<uint8_t*>(parms) + valProp->GetOffset_Internal();

        bool ok = ApplyPropertyToObject(*targetSlot, propEntry);

        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("Success"));
        if (outSuccess) *outSuccess = ok;
    }

    // SetPropertyForMulti(Targets: Array<UObject*>, NewValue: UE4SS_BP_PROPERTIES)
    //   → Success: bool
    static void Handle_SetPropertyForMulti(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        // Input: "Targets" (Array<Object>)
        FProperty* targetsProp = FindParam(fn, STR("Targets"));
        if (!targetsProp) return;
        auto* arrFProp = static_cast<FArrayProperty*>(targetsProp);
        void* rawArr   = static_cast<uint8_t*>(parms) + targetsProp->GetOffset_Internal();
        FScriptArrayHelper helper(arrFProp, rawArr);

        // Input: "NewValue" (UE4SS_BP_PROPERTIES struct)
        FProperty* valProp = FindParam(fn, STR("NewValue"));
        if (!valProp) return;
        const void* propEntry = static_cast<uint8_t*>(parms) + valProp->GetOffset_Internal();

        bool anyOk = false;
        for (int32_t i = 0; i < helper.Num(); ++i)
        {
            UObject* target = *reinterpret_cast<UObject**>(helper.GetRawPtr(i));
            if (target && ApplyPropertyToObject(target, propEntry)) anyOk = true;
        }

        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("success"));
        if (outSuccess) *outSuccess = anyOk;
    }

    // GetFunctionByClass(Target: class, FilterEngine: bool)
    //   → (FoundFunctions: Array<UE4SS_BP_FUNCTIONS>, Success: bool)
    //   Returns all functions on the given UClass (with optional engine filtering).
    static void Handle_GetFunctionByClass(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        auto* targetSlot = ParamPtr<UClass*>(fn, parms, STR("Target"));
        if (!targetSlot || !*targetSlot) return;
        UClass* cls = *targetSlot;

        bool filterEngine = false;
        auto* filterSlot = ParamPtr<bool>(fn, parms, STR("FilterEngine"));
        if (filterSlot) filterEngine = *filterSlot;

        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetFunctionByClass: Target='{}' FilterEngine={}\n"),
            cls->GetName().c_str(), filterEngine);

        FProperty* outProp = FindParam(fn, STR("FoundFunctions"));
        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("success"));
        if (!outProp) { if (outSuccess) *outSuccess = false; return; }

        std::vector<UFunction*> fns;
        std::unordered_set<std::wstring> seen;
        for (UStruct* s = cls; s; s = s->GetSuperStruct())
        {
            if (filterEngine && !IsUserMadeClass(s)) continue;
            for (UFunction* ufn : s->ForEachFunction())
            {
                std::wstring fn_name(ufn->GetName().c_str());
                if (seen.count(fn_name)) continue;
                seen.insert(fn_name);
                fns.push_back(ufn);
            }
        }

        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetFunctionByClass: collected {} functions\n"),
            fns.size());
        for (UFunction* ufn : fns)
            Output::send<LogLevel::Normal>(STR("[BlueprintAPI]   fn='{}'\n"), ufn->GetName().c_str());

        auto* arrFProp = static_cast<FArrayProperty*>(outProp);
        void* rawArr = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        FScriptArrayHelper helper(arrFProp, rawArr);
        helper.AddValues(static_cast<int32_t>(fns.size()));
        for (int32_t i = 0; i < static_cast<int32_t>(fns.size()); ++i)
            WriteBPFunction(helper.GetRawPtr(i), s_bpStructs.Functions, s_bpStructs.Properties, s_bpStructs.Types, fns[i]);

        if (outSuccess) *outSuccess = !fns.empty();
        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetFunctionByClass: done, success={}\n"),
            !fns.empty());
    }

    // GetPropertiesForObject(Target: UObject*, FilterEngine: bool)
    //   → (Properties: Array<UE4SS_BP_PROPERTIES>, Success: bool)
    //   Returns the live property values for the given object.
    static void Handle_GetPropertiesForObject(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs()) return;

        auto* targetSlot = ParamPtr<UObject*>(fn, parms, STR("Target"));
        if (!targetSlot || !*targetSlot) return;
        UObject* target = *targetSlot;

        bool filterEngine = false;
        auto* filterSlot = ParamPtr<bool>(fn, parms, STR("FilterEngine"));
        if (filterSlot) filterEngine = *filterSlot;

        UClass* cls = target->GetClassPrivate();
        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetPropertiesForObject: Target='{}' Class='{}' FilterEngine={}\n"),
            target->GetName().c_str(),
            cls ? cls->GetName().c_str() : STR("<null>"),
            filterEngine);

        FProperty* outProp = FindParam(fn, STR("Properties"));
        bool* outSuccess = ParamPtr<bool>(fn, parms, STR("success"));
        if (!outProp) { if (outSuccess) *outSuccess = false; return; }

        if (!cls) { if (outSuccess) *outSuccess = false; return; }

        // Engine boundary exclusion set
        std::unordered_set<FProperty*> engineProps;
        if (filterEngine)
        {
            for (UStruct* s = cls->GetSuperStruct(); s; s = s->GetSuperStruct())
            {
                if (!IsUserMadeClass(s))
                {
                    for (FProperty* p : s->ForEachProperty())
                        engineProps.insert(p);
                    break;
                }
            }
        }

        std::vector<FProperty*> eligible;
        for (FProperty* p : cls->ForEachProperty())
        {
            if (p->HasAnyPropertyFlags(CPF_Parm)) continue;
            if (filterEngine && engineProps.count(p)) continue;
            const StringType cn = p->GetClass().GetFName().ToString();
            if (cn == STR("MapProperty") || cn == STR("SetProperty") ||
                cn == STR("ArrayProperty") || cn == STR("DelegateProperty") ||
                cn == STR("MulticastDelegateProperty") ||
                cn == STR("MulticastInlineDelegateProperty") ||
                cn == STR("ObjectProperty") || cn == STR("ClassProperty") ||
                cn == STR("WeakObjectProperty") || cn == STR("SoftObjectProperty") ||
                cn == STR("SoftClassProperty") || cn == STR("LazyObjectProperty") ||
                cn == STR("InterfaceProperty"))
                continue;
            eligible.push_back(p);
        }

        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetPropertiesForObject: {} eligible properties\n"),
            eligible.size());
        for (FProperty* p : eligible)
            Output::send<LogLevel::Normal>(
                STR("[BlueprintAPI]   prop='{}' type='{}'\n"),
                p->GetName().c_str(),
                p->GetClass().GetFName().ToString().c_str());

        auto* arrFProp = static_cast<FArrayProperty*>(outProp);
        void* rawArr = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        FScriptArrayHelper helper(arrFProp, rawArr);
        helper.AddValues(static_cast<int32_t>(eligible.size()));
        for (int32_t i = 0; i < static_cast<int32_t>(eligible.size()); ++i)
        {
            FProperty* p = eligible[i];
            WriteBPProperty(
                helper.GetRawPtr(i), s_bpStructs.Properties, s_bpStructs.Types,
                FName(p->GetName().c_str()),
                reinterpret_cast<uintptr_t>(p->ContainerPtrToValuePtr<void>(target)),
                p, target);
        }

        if (outSuccess) *outSuccess = true;
        Output::send<LogLevel::Normal>(
            STR("[BlueprintAPI] GetPropertiesForObject: done\n"));
    }

    // GetActorClassList() → (Actors: Array<class<Actor>>)
    //   Returns raw UClass* pointers for all cached user actor classes.
    //   Lightweight alternative to GetAllActorClasses (no struct building).
    static void Handle_GetActorClassList(UObject*, UFunction* fn, void* parms)
    {
        BuildStaticCache();

        FProperty* outProp = FindParam(fn, STR("Actors"));
        if (!outProp) return;

        auto* arrFProp = static_cast<FArrayProperty*>(outProp);
        void* rawArr = static_cast<uint8_t*>(parms) + outProp->GetOffset_Internal();
        FScriptArrayHelper helper(arrFProp, rawArr);
        helper.AddValues(static_cast<int32_t>(s_staticCache.actorClasses.size()));
        for (int32_t i = 0; i < static_cast<int32_t>(s_staticCache.actorClasses.size()); ++i)
        {
            UClass* cls = static_cast<UClass*>(s_staticCache.actorClasses[i]);
            std::memcpy(helper.GetRawPtr(i), &cls, sizeof(UClass*));
        }
    }

    // PrintUE4SS(Object: UObject*, Struct: FString) → Result: UE4SS_BP_STRUCT
    //   Converts a named struct property on an object into a UE4SS_BP_STRUCT.
    static void Handle_PrintUE4SS(UObject*, UFunction* fn, void* parms)
    {
        if (!EnsureBPStructs())
        {
            Output::send<LogLevel::Warning>(STR("[BlueprintAPI] PrintUE4SS: required structs not found\n"));
            return;
        }

        auto* structName = ParamPtr<FString>(fn, parms, STR("Message"));
        if (!structName) return;


        const wchar_t* rawName = structName->GetCharArray().GetData();
        if (!rawName || rawName[0] == L'\0') return;
        StringType fieldName(rawName);

        Output::send<LogLevel::Default>(fieldName);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Registry
    // ─────────────────────────────────────────────────────────────────────────

    using HandlerFn = std::function<void(UObject*, UFunction*, void*)>;

    static const std::unordered_map<std::wstring, HandlerFn> s_handlers = {
            {L"GetLevels",                Handle_GetLevels},
            {L"GetAllActorClasses",       Handle_GetAllActorClasses},
            {L"GetAllActors",             Handle_GetAllActors},
            {L"GetUpdatedActor",          Handle_GetUpdatedActor},
            {L"GetFunctionFromLibraries", Handle_GetFunctionFromLibraries},
            {L"ExecuteFunction",          Handle_ExecuteFunction},
            {L"ExecuteFunctionForMulti",  Handle_ExecuteFunctionForMulti},
            {L"SetProperty",              Handle_SetProperty},
            {L"SetPropertyForMulti",      Handle_SetPropertyForMulti},
            {L"ConvertStruct",            Handle_ConvertStruct},
            {L"FlashNewStruct",           Handle_FlashNewStruct},
            {L"GetFunctionByClass",       Handle_GetFunctionByClass},
            {L"GetPropertiesForObject",   Handle_GetPropertiesForObject},
            {L"GetActorClassList",        Handle_GetActorClassList},
            {L"PrintUE4SS",               Handle_PrintUE4SS},
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Initialization
    // ─────────────────────────────────────────────────────────────────────────

    auto Initialize() -> void
    {
        Hook::RegisterProcessEventPreCallback(
                [](Hook::TCallbackIterationData<void>& info, UObject* ctx, UFunction* fn, void* parms) {
                    // Check whether we have a handler for this function name first.
                    // Avoid running the owner-class diagnostics for functions we
                    // don't handle.
                    // Look up handler by function name first and only proceed for
                    // functions we handle. Blueprint function libraries declare
                    // static functions that may not have an obvious owning
                    // UClass in every engine variant, so allow handlers to run
                    // based on name match alone.
                    if (!IsFunctionInAllowedLibrary(fn)) return;
                    auto it = s_handlers.find(std::wstring(fn->GetName().c_str()));
                    if (it == s_handlers.end()) return;

                    Output::send<LogLevel::Normal>(STR("[BlueprintAPI] '{}' called on '{}'\n"), fn->GetName().c_str(), (ctx ? ctx->GetName().c_str() : STR("null")));

                    try
                    {
                        it->second(ctx, fn, parms);
                    }
                    catch (const std::exception& e)
                    {
                        Output::send<LogLevel::Warning>(STR("[BlueprintAPI] Exception in handler for '{}': {}\n"),
                                                        fn->GetName().c_str(),
                                                        RC::ensure_str(e.what()).c_str());
                    }
                    catch (...)
                    {
                        Output::send<LogLevel::Warning>(STR("[BlueprintAPI] Unknown exception in handler for '{}'\n"), fn->GetName().c_str());
                    }

                    info.PreventOriginalFunctionCall();
                },
                Hook::FCallbackOptions{
                        .OwnerModName = STR("UE4SS_Core"),
                        .HookName = STR("BlueprintAPIDispatch"),
                });

        // Some blueprint functions (pure/compiled-in) execute via the VM path
        // rather than through UObject::ProcessEvent. Register pre-hooks for
        // those codepaths so phantom functions are handled regardless of how
        // they're invoked.
        Hook::RegisterProcessLocalScriptFunctionPreCallback(
                [](Hook::TCallbackIterationData<void>& info, UObject* ctx, FFrame& stack, void* result) {
                    UFunction* fn = stack.Node();
                    if (!fn) return;

                    // Look up the handler first to avoid owner-class diagnostics for
                    // functions we won't handle.
                    if (!IsFunctionInAllowedLibrary(fn)) return;
                    auto it = s_handlers.find(std::wstring(fn->GetName().c_str()));
                    if (it == s_handlers.end())
                    {
                        std::wstring name(fn->GetName().c_str());
                        if (!s_unhandled_seen.count(name))
                        {
                            s_unhandled_seen.insert(name);
                            Output::send<LogLevel::Normal>(STR("[BlueprintAPI] Unhandled function '{}' seen (VM)\n"), fn->GetName().c_str());
                        }
                        return;
                    }
                    

                    Output::send<LogLevel::Normal>(STR("[BlueprintAPI] '{}' (VM) called on '{}'\n"),
                                                   fn->GetName().c_str(),
                                                   (ctx ? ctx->GetName().c_str() : STR("null")));

                    // Diagnostic dump of parameter layout and buffers to help
                    // investigate why return values may not be observed in pure
                    // / compiled-in functions.
                    DumpFunctionParms(fn, reinterpret_cast<void*>(stack.Locals()));
                    if (stack.OutParms())
                    {
                        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] OutParms list present: {}\n"), reinterpret_cast<void*>(stack.OutParms()));
                        // Iterate OutParms linked list
                        for (FOutParmRec* rec = stack.OutParms(); rec; rec = rec->NextOutParm)
                        {
                            if (rec->Property)
                            {
                                Output::send<LogLevel::Normal>(STR("  OutParm: '{}' addr={}\n"),
                                                               rec->Property->GetName().c_str(),
                                                               reinterpret_cast<void*>(rec->PropAddr));
                            }
                        }
                    }

                    // In the UE4 BP VM, input arrays (and other non-primitive inputs)
                    // are passed by-reference through the OutParms linked list even when
                    // they are input-only in the Blueprint graph. The locals slot at the
                    // property offset is left empty/zero. Sync OutParms → locals so that
                    // handlers can read all inputs uniformly via parms + offset.
                    if (stack.OutParms())
                    {
                        uint8_t* locals = reinterpret_cast<uint8_t*>(stack.Locals());
                        for (FOutParmRec* rec = stack.OutParms(); rec; rec = rec->NextOutParm)
                        {
                            FProperty* prop = rec->Property;
                            if (!prop || !rec->PropAddr) continue;
                            if (prop->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
                            std::memcpy(locals + prop->GetOffset_Internal(),
                                        rec->PropAddr,
                                        static_cast<size_t>(prop->GetSize()));
                        }
                    }

                    try
                    {
                        it->second(ctx, fn, reinterpret_cast<void*>(stack.Locals()));
                        PropagateOutParms(fn, stack, result);
                    }
                    catch (const std::exception& e)
                    {
                        Output::send<LogLevel::Warning>(STR("[BlueprintAPI] Exception in VM handler for '{}': {}\n"),
                                                        fn->GetName().c_str(),
                                                        RC::ensure_str(e.what()).c_str());
                    }
                    catch (...)
                    {
                        Output::send<LogLevel::Warning>(STR("[BlueprintAPI] Unknown exception in VM handler for '{}'\n"), fn->GetName().c_str());
                    }

                    info.PreventOriginalFunctionCall();
                },
                Hook::FCallbackOptions{
                        .OwnerModName = STR("UE4SS_Core"),
                        .HookName = STR("BlueprintAPI_VMDispatch"),
                });

        Hook::RegisterProcessInternalPreCallback(
                [](Hook::TCallbackIterationData<void>& info, UObject* ctx, FFrame& stack, void* result) {
                    UFunction* fn = stack.Node();
                    if (!fn) return;

                    // Look up the handler first to avoid owner-class diagnostics for
                    // functions we won't handle.
                    if (!IsFunctionInAllowedLibrary(fn)) return;
                    auto it = s_handlers.find(std::wstring(fn->GetName().c_str()));
                    if (it == s_handlers.end())
                    {
                        std::wstring name(fn->GetName().c_str());
                        if (!s_unhandled_seen.count(name))
                        {
                            s_unhandled_seen.insert(name);
                            Output::send<LogLevel::Normal>(STR("[BlueprintAPI] Unhandled function '{}' seen (ProcessInternal)\n"), fn->GetName().c_str());
                        }
                        return;
                    }
                    

                    Output::send<LogLevel::Normal>(STR("[BlueprintAPI] '{}' (ProcessInternal) called on '{}'\n"),
                                                   fn->GetName().c_str(),
                                                   (ctx ? ctx->GetName().c_str() : STR("null")));

                    DumpFunctionParms(fn, reinterpret_cast<void*>(stack.Locals()));
                    if (stack.OutParms())
                    {
                        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] OutParms list present: {}\n"), reinterpret_cast<void*>(stack.OutParms()));
                        for (FOutParmRec* rec = stack.OutParms(); rec; rec = rec->NextOutParm)
                        {
                            if (rec->Property)
                            {
                                Output::send<LogLevel::Normal>(STR("  OutParm: '{}' addr={}\n"),
                                                               rec->Property->GetName().c_str(),
                                                               reinterpret_cast<void*>(rec->PropAddr));
                            }
                        }
                    }

                    // Sync OutParms → locals so handlers can read input arrays/structs
                    // uniformly via parms + offset. UE4 BP passes non-primitive inputs
                    // (arrays, structs) by-ref through OutParms even when input-only.
                    if (stack.OutParms())
                    {
                        uint8_t* locals = reinterpret_cast<uint8_t*>(stack.Locals());
                        for (FOutParmRec* rec = stack.OutParms(); rec; rec = rec->NextOutParm)
                        {
                            FProperty* prop = rec->Property;
                            if (!prop || !rec->PropAddr) continue;
                            if (prop->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
                            std::memcpy(locals + prop->GetOffset_Internal(),
                                        rec->PropAddr,
                                        static_cast<size_t>(prop->GetSize()));
                        }
                    }

                    try
                    {
                        it->second(ctx, fn, reinterpret_cast<void*>(stack.Locals()));
                        PropagateOutParms(fn, stack, result);
                    }
                    catch (const std::exception& e)
                    {
                        Output::send<LogLevel::Warning>(STR("[BlueprintAPI] Exception in ProcessInternal handler for '{}': {}\n"),
                                                        fn->GetName().c_str(),
                                                        RC::ensure_str(e.what()).c_str());
                    }
                    catch (...)
                    {
                        Output::send<LogLevel::Warning>(STR("[BlueprintAPI] Unknown exception in ProcessInternal handler for '{}'\n"), fn->GetName().c_str());
                    }

                    info.PreventOriginalFunctionCall();
                },
                Hook::FCallbackOptions{
                        .OwnerModName = STR("UE4SS_Core"),
                        .HookName = STR("BlueprintAPI_ProcessInternal"),
                });

        // NOTE: BuildStaticCache() is NOT called here.
        // At this point in the engine lifecycle, game Blueprint classes are not yet
        // loaded into GUObjectArray. The cache is built lazily on first use of
        // GetLevels or GetAllActorClasses, which only fire after the world is running.

        Output::send<LogLevel::Normal>(STR("[BlueprintAPI] Initialized — {} phantom functions registered\n"), s_handlers.size());
    }

} // namespace RC::BlueprintAPI