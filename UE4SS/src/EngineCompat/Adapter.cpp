#include <EngineCompat/Adapter.hpp>

#include <algorithm>
#include <mutex>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>

namespace RC::Compat
{
    using namespace RC::Unreal;
    using Reflect::Call;

    namespace
    {
        constexpr const wchar_t* kWorldContext = L"WorldContextObject|WorldContext|Object";

        auto gameplay_statics() -> UObject*
        {
            return Reflect::DefaultObjectOf(L"GameplayStatics");
        }

        auto static_call(const wchar_t* cls, const wchar_t* fn) -> Call
        {
            return Call::Static(cls, fn);
        }

        auto last_path_segment(const StringType& path) -> StringType
        {
            const auto slash = path.rfind(L'/');
            return slash == StringType::npos ? path : path.substr(slash + 1);
        }

        auto ends_with_word(const StringType& text, const StringType& word) -> bool
        {
            if (text == word) return true;
            if (text.size() <= word.size()) return false;
            if (text.compare(text.size() - word.size(), word.size(), word) != 0) return false;
            const wchar_t before = text[text.size() - word.size() - 1];
            return before == L'.' || before == L'/' || before == L'=' || before == L'\'';
        }
    } // namespace

    // ---- Session objects -------------------------------------------------------------------------

    auto GenericAdapter::cached_player_controller() -> UObject*
    {
        const auto now = std::chrono::steady_clock::now();
        if (!m_pc_cache || !Reflect::IsValidObject(m_pc_cache) || now - m_pc_cache_time > std::chrono::seconds{1})
        {
            m_pc_cache = UObjectGlobals::FindFirstOf(L"PlayerController");
            m_pc_cache_time = now;
        }
        return m_pc_cache;
    }

    auto GenericAdapter::world() -> UObject*
    {
        if (UObject* pc = cached_player_controller())
        {
            if (UObject* w = pc->GetWorld()) return w;
        }
        UObject* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
            if (obj && obj->GetClassPrivate() && obj->GetClassPrivate()->GetName() == L"World" && !obj->HasAnyFlags(RF_ClassDefaultObject) &&
                obj->GetName().find(L"Transient") == StringType::npos)
            {
                found = obj;
                return LoopAction::Break;
            }
            return LoopAction::Continue;
        });
        return found;
    }

    auto GenericAdapter::player_controller(int index) -> UObject*
    {
        if (UObject* w = world())
        {
            auto call = static_call(L"GameplayStatics", L"GetPlayerController");
            call.SetObject(kWorldContext, w).SetNumber(L"PlayerIndex", index);
            if (call.Invoke())
            {
                if (UObject* pc = call.GetObject(L"ReturnValue")) return pc;
            }
        }
        if (index == 0) return cached_player_controller();
        return nullptr;
    }

    auto GenericAdapter::player_pawn(int index) -> UObject*
    {
        if (UObject* pc = player_controller(index))
        {
            if (UObject* pawn = Reflect::GetObjectProperty(pc, L"Pawn")) return pawn;
            if (UObject* pawn = Reflect::GetObjectProperty(pc, L"AcknowledgedPawn")) return pawn;
        }
        if (UObject* w = world())
        {
            auto call = static_call(L"GameplayStatics", L"GetPlayerPawn");
            call.SetObject(kWorldContext, w).SetNumber(L"PlayerIndex", index);
            if (call.Invoke()) return call.GetObject(L"ReturnValue");
        }
        return nullptr;
    }

    auto GenericAdapter::game_mode() -> UObject*
    {
        if (UObject* w = world())
        {
            auto call = static_call(L"GameplayStatics", L"GetGameMode");
            call.SetObject(kWorldContext, w);
            if (call.Invoke()) return call.GetObject(L"ReturnValue");
        }
        return UObjectGlobals::FindFirstOf(L"GameModeBase");
    }

    auto GenericAdapter::game_state() -> UObject*
    {
        if (UObject* w = world())
        {
            auto call = static_call(L"GameplayStatics", L"GetGameState");
            call.SetObject(kWorldContext, w);
            if (call.Invoke()) return call.GetObject(L"ReturnValue");
        }
        return UObjectGlobals::FindFirstOf(L"GameStateBase");
    }

    auto GenericAdapter::world_settings() -> UObject*
    {
        UObject* w = world();
        if (UObject* ws = w ? Reflect::GetObjectProperty(w, L"PersistentLevel") : nullptr)
        {
            if (UObject* settings = Reflect::GetObjectProperty(ws, L"WorldSettings")) return settings;
        }
        return UObjectGlobals::FindFirstOf(L"WorldSettings");
    }

    auto GenericAdapter::hud(UObject* pc) -> UObject*
    {
        return pc ? Reflect::GetObjectProperty(pc, L"MyHUD") : nullptr;
    }

    auto GenericAdapter::current_level_name() -> StringType
    {
        if (UObject* w = world())
        {
            auto call = static_call(L"GameplayStatics", L"GetCurrentLevelName");
            call.SetObject(kWorldContext, w).SetBool(L"bRemovePrefixString", true);
            if (call.Invoke())
            {
                if (auto text = call.GetText(L"ReturnValue"); text && !text->empty()) return *text;
            }
            // Fallback: the world object is named after its map.
            return last_path_segment(w->GetName());
        }
        return {};
    }

    // ---- Actors ----------------------------------------------------------------------------------

    auto GenericAdapter::actor_location(UObject* actor) -> std::optional<Vec3>
    {
        if (!actor) return std::nullopt;
        auto call = Call(actor, Reflect::FindFunction(actor, L"K2_GetActorLocation"));
        if (call.Invoke()) return call.GetVec3(L"ReturnValue");
        return std::nullopt;
    }

    auto GenericAdapter::actor_rotation(UObject* actor) -> std::optional<Rot3>
    {
        if (!actor) return std::nullopt;
        auto call = Call(actor, Reflect::FindFunction(actor, L"K2_GetActorRotation"));
        if (!call.Invoke()) return std::nullopt;
        FProperty* p = call.ParamProperty(L"ReturnValue");
        Rot3 r;
        if (p && Reflect::ReadRot3(p, call.ParamData(L"ReturnValue"), r)) return r;
        return std::nullopt;
    }

    auto GenericAdapter::teleport_actor(UObject* actor, const Vec3& location, const std::optional<Rot3>& rotation) -> bool
    {
        if (!actor) return false;
        Rot3 rot;
        if (rotation) rot = *rotation;
        else if (auto current = actor_rotation(actor)) rot = *current;

        auto tp = Call(actor, Reflect::FindFunction(actor, L"K2_TeleportTo"));
        if (tp.valid())
        {
            tp.SetVec3(L"DestLocation", location).SetRot3(L"DestRotation", rot);
            if (tp.Invoke() && tp.GetBool(L"ReturnValue").value_or(false)) return true;
        }
        // Fallback for actors where TeleportTo is rejected (e.g. no collision-free spot found).
        auto set = Call(actor, Reflect::FindFunction(actor, L"K2_SetActorLocation"));
        if (!set.valid()) return false;
        set.SetVec3(L"NewLocation", location).SetBool(L"bSweep", false).SetBool(L"bTeleport", true);
        return set.Invoke() && set.GetBool(L"ReturnValue").value_or(true);
    }

    auto GenericAdapter::set_control_rotation(UObject* pc, const Rot3& rotation) -> bool
    {
        if (!pc) return false;
        auto call = Call(pc, Reflect::FindFunction(pc, L"SetControlRotation"));
        if (!call.valid()) return false;
        call.SetRot3(L"NewRotation", rotation);
        return call.Invoke();
    }

    auto GenericAdapter::stop_movement(UObject* actor) -> bool
    {
        if (!actor) return false;
        // Characters keep velocity in their movement component; plain pawns use MovementComponent.
        for (const wchar_t* comp : {L"CharacterMovement", L"MovementComponent"})
        {
            if (UObject* movement = Reflect::GetObjectProperty(actor, comp))
            {
                auto call = Call(movement, Reflect::FindFunction(movement, L"StopMovementImmediately"));
                if (call.Invoke()) return true;
            }
        }
        return false;
    }

    auto GenericAdapter::actors_of_class(const StringType& class_name) -> std::vector<UObject*>
    {
        std::vector<UObject*> result;
        UObject* w = world();
        UClass* cls = Reflect::FindClass(class_name);
        if (!w || !cls) return result;

        auto call = static_call(L"GameplayStatics", L"GetAllActorsOfClass");
        call.SetObject(kWorldContext, w).SetObject(L"ActorClass", cls);
        if (!call.Invoke()) return result;
        // The engine allocated the array with its own allocator; reading element pointers is safe, freeing is Call's job.
        auto* out = static_cast<TArray<UObject*>*>(call.ParamData(L"OutActors"));
        if (!out) return result;
        for (int32 i = 0; i < out->Num(); ++i)
        {
            if ((*out)[i]) result.push_back((*out)[i]);
        }
        return result;
    }

    // ---- Levels ----------------------------------------------------------------------------------

    auto GenericAdapter::asset_class_is_world(UObject*, UScriptStruct* type, const void* data) -> bool
    {
        // UE <5.1 stores a plain FName in "AssetClass"; 5.1+ stores a FTopLevelAssetPath in "AssetClassPath".
        // Exporting either through the engine's text form and matching the trailing identifier covers both.
        for (const wchar_t* field : {L"AssetClassPath", L"AssetClass"})
        {
            for (FProperty* p : TFieldRange<FProperty>(type))
            {
                if (p->GetName() != field) continue;
                if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(data)))
                {
                    StringType t = *text;
                    // "(PackageName=/Script/Engine,AssetName=World)" -> "World"
                    if (const auto key = t.find(L"AssetName="); key != StringType::npos)
                    {
                        t = t.substr(key + 10);
                        t = t.substr(0, t.find_first_of(L",)"));
                    }
                    return ends_with_word(t, L"World");
                }
            }
        }
        return false;
    }

    auto GenericAdapter::enumerate_levels() -> std::vector<LevelInfo>
    {
        std::vector<LevelInfo> levels;

        UFunction* get_all = Reflect::FindFunctionByPath(L"/Script/AssetRegistry.AssetRegistry:GetAllAssets");
        if (!get_all)
        {
            Output::send<LogLevel::Warning>(STR("[EngineCompat] AssetRegistry:GetAllAssets not found; cannot enumerate levels\n"));
            return levels;
        }

        // The live registry must come from AssetRegistryHelpers::GetAssetRegistry(), which returns a TScriptInterface
        // whose first member is the registry object. Calling GetAllAssets on the class default object instead
        // dereferences registry state that only exists on the live object and crashes the game (null read inside the
        // engine) - this is what happens on 4.27 where the CDO is the only AssetRegistryImpl in GUObjectArray.
        UObject* registry = nullptr;
        {
            Call helper = Call::Static(L"AssetRegistryHelpers", L"GetAssetRegistry");
            if (helper.valid() && helper.Invoke())
            {
                if (void* data = helper.ParamData(L"ReturnValue"))
                {
                    registry = *static_cast<UObject**>(data); // FScriptInterface::ObjectPointer
                    if (!Reflect::IsValidObject(registry)) registry = nullptr;
                }
            }
        }
        if (!registry)
        {
            Output::send<LogLevel::Warning>(STR("[EngineCompat] No asset registry object found; cannot enumerate levels\n"));
            int shown = 0;
            UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                if (obj && obj->GetClassPrivate() && obj->GetClassPrivate()->GetName().find(L"Registry") != StringType::npos && shown++ < 8)
                    Output::send<LogLevel::Normal>(STR("[EngineCompat]   registry candidate: {} (class {})\n"), obj->GetFullName(), obj->GetClassPrivate()->GetFullName());
                return LoopAction::Continue;
            });
            return levels;
        }

        Call call(registry, get_all);
        call.SetBool(L"bIncludeOnlyOnDiskAssets", true);
        if (!call.Invoke()) return levels;

        auto* array_prop = CastField<FArrayProperty>(call.ParamProperty(L"OutAssetData"));
        auto* array_data = call.ParamData(L"OutAssetData");
        auto* inner = array_prop ? CastField<FStructProperty>(array_prop->GetInner()) : nullptr;
        if (!array_data || !inner) return levels;

        UScriptStruct* asset_type = inner->GetStruct();
        // Walk with the engine's runtime element size; compile-time sizeof(FAssetData) is not reliable across versions.
        const int32 stride = inner->GetElementSize();
        auto* script_array = static_cast<FScriptArray*>(array_data);
        const int32 num = script_array->Num();
        auto* base = static_cast<uint8_t*>(script_array->GetData());

        FProperty* package_name = nullptr;
        for (FProperty* p : TFieldRange<FProperty>(asset_type))
        {
            if (p->GetName() == L"PackageName") package_name = p;
        }
        if (!package_name || stride <= 0) return levels;

        for (int32 i = 0; i < num; ++i)
        {
            const void* element = base + static_cast<ptrdiff_t>(i) * stride;
            if (!asset_class_is_world(registry, asset_type, element)) continue;
            auto path = Reflect::ExportText(package_name, package_name->ContainerPtrToValuePtr<void>(element));
            if (!path || path->empty() || (*path)[0] != L'/') continue;
            levels.push_back({last_path_segment(*path), *path});
        }
        std::sort(levels.begin(), levels.end(), [](const LevelInfo& a, const LevelInfo& b) { return a.package_path < b.package_path; });
        return levels;
    }

    auto GenericAdapter::open_level(const StringType& level, const StringType& options) -> bool
    {
        UObject* w = world();
        if (!w || level.empty()) return false;
        // Same path the in-game console takes ("open <map>"), which Voices of the Void tolerates.
        if (exec_console(L"open " + level + options)) return true;
        auto call = static_call(L"GameplayStatics", L"OpenLevel");
        call.SetObject(kWorldContext, w).SetText(L"LevelName", level).SetBool(L"bAbsolute", true).SetText(L"Options", options);
        return call.Invoke();
    }

    auto GenericAdapter::exec_console(const StringType& command) -> bool
    {
        UObject* w = world();
        if (!w || command.empty()) return false;
        auto call = static_call(L"KismetSystemLibrary", L"ExecuteConsoleCommand");
        call.SetObject(kWorldContext, w).SetText(L"Command", command).SetObject(L"SpecificPlayer", player_controller());
        if (call.Invoke()) return true;
        // Older/forked engines: ask the controller directly.
        if (UObject* pc = player_controller())
        {
            auto pc_call = Call(pc, Reflect::FindFunction(pc, L"ConsoleCommand"));
            pc_call.SetText(L"Command", command).SetBool(L"bWriteToLog", false);
            return pc_call.Invoke();
        }
        return false;
    }

    // ---- Presentation ----------------------------------------------------------------------------

    auto GenericAdapter::project_world_to_screen(UObject* pc, const Vec3& location, Vec2& out) -> bool
    {
        if (!pc) return false;
        auto call = Call(pc, Reflect::FindFunction(pc, L"ProjectWorldLocationToScreen"));
        if (!call.valid()) return false;
        call.SetVec3(L"WorldLocation", location).SetBool(L"bPlayerViewportRelative", false);
        if (!call.Invoke() || !call.GetBool(L"ReturnValue").value_or(false)) return false;
        auto screen = call.GetVec2(L"ScreenLocation");
        if (!screen) return false;
        out = *screen;
        return true;
    }

    auto GenericAdapter::set_time_dilation(double dilation) -> bool
    {
        UObject* w = world();
        if (!w) return false;
        auto call = static_call(L"GameplayStatics", L"SetGlobalTimeDilation");
        call.SetObject(kWorldContext, w).SetNumber(L"TimeDilation", dilation);
        return call.Invoke();
    }

    // ---- Registry --------------------------------------------------------------------------------

    namespace
    {
        std::mutex g_registry_mutex;
        std::vector<std::unique_ptr<IEngineAdapter>> g_adapters;
        IEngineAdapter* g_selected = nullptr;

        auto ensure_builtin_adapters() -> void
        {
            if (!g_adapters.empty()) return;
            g_adapters.push_back(std::make_unique<GenericAdapter>());
            g_adapters.push_back(std::make_unique<Ue4Adapter>());
            g_adapters.push_back(std::make_unique<Ue5Adapter>());
        }

        auto select_locked() -> IEngineAdapter&
        {
            ensure_builtin_adapters();
            const EngineProfile& profile = EngineProfile::Get();
            IEngineAdapter* best = nullptr;
            for (auto& adapter : g_adapters)
            {
                if (!adapter->supports(profile)) continue;
                if (!best || adapter->priority() > best->priority()) best = adapter.get();
            }
            if (profile.major > 0) g_selected = best; // do not pin a choice made before the version is known
            return *best;
        }
    } // namespace

    auto AdapterRegistry::Register(std::unique_ptr<IEngineAdapter> adapter) -> void
    {
        std::lock_guard lock(g_registry_mutex);
        ensure_builtin_adapters();
        g_adapters.push_back(std::move(adapter));
        g_selected = nullptr;
    }

    auto AdapterRegistry::Get() -> IEngineAdapter&
    {
        std::lock_guard lock(g_registry_mutex);
        return g_selected ? *g_selected : select_locked();
    }

    auto AdapterRegistry::Reselect() -> void
    {
        std::lock_guard lock(g_registry_mutex);
        g_selected = nullptr;
    }

    auto AdapterRegistry::Describe() -> StringType
    {
        std::lock_guard lock(g_registry_mutex);
        auto& selected = g_selected ? *g_selected : select_locked();
        StringType out = std::format(L"Active adapter: {}  |  Registered: ", selected.name());
        bool first = true;
        for (auto& adapter : g_adapters)
        {
            if (!first) out += L", ";
            first = false;
            out += adapter->name();
        }
        return out;
    }
} // namespace RC::Compat
