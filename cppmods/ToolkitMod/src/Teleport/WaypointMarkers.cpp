#include <Teleport/WaypointRenderer.hpp>

#include <format>
#include <map>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Session.hpp>
#include <Teleport/Waypoints.hpp>
#include <Unreal/UObjectGlobals.hpp>

// Marker actors: the HUD-independent way waypoints are shown.
//
// Some games never draw their HUD (Voices of the Void's HUD has no canvas, so ReceiveDrawHUD never fires), so visible
// waypoints are also shown as real actors: a tall beam and a ball made from the engine's basic shapes, without
// collision. /Engine/BasicShapes is cooked into the game, and everything is created through reflection, so this works
// on any engine version.

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;

    auto WaypointRenderer::spawn_part(UObject* world, const wchar_t* mesh_path, double x, double y, double z, double sx, double sy, double sz,
                                      const float (&color)[3]) -> UObject*
    {
        UClass* actor_class = Reflect::FindClass(L"/Script/Engine.StaticMeshActor");
        UObject* mesh = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, mesh_path);
        UObject* material = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, L"/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
        if (!actor_class || !mesh) return nullptr;

        const StringType transform = std::format(L"(Rotation=(X=0,Y=0,Z=0,W=1),Translation=(X={},Y={},Z={}),Scale3D=(X={},Y={},Z={}))", x, y, z, sx, sy, sz);
        auto begin = Reflect::Call::Static(L"GameplayStatics", L"BeginDeferredActorSpawnFromClass");
        begin.SetObject(L"WorldContextObject|WorldContext", world).SetObject(L"ActorClass", actor_class).SetText(L"SpawnTransform", transform);
        begin.SetText(L"CollisionHandlingOverride", L"AlwaysSpawn");
        if (!begin.valid() || !begin.Invoke()) return nullptr;
        UObject* actor = begin.GetObject(L"ReturnValue");
        if (!actor) return nullptr;

        UObject* component = Reflect::GetObjectProperty(actor, L"StaticMeshComponent");
        if (component)
        {
            // Movable first: a static component refuses SetStaticMesh at runtime.
            Reflect::Call(component, Reflect::FindFunction(component, L"SetMobility")).SetText(L"NewMobility", L"Movable").Invoke();
            Reflect::Call(component, Reflect::FindFunction(component, L"SetStaticMesh")).SetObject(L"NewMesh", mesh).Invoke();
            Reflect::Call(component, Reflect::FindFunction(component, L"SetCollisionEnabled")).SetText(L"NewType", L"NoCollision").Invoke();
        }
        auto finish = Reflect::Call::Static(L"GameplayStatics", L"FinishSpawningActor");
        finish.SetObject(L"Actor", actor).SetText(L"SpawnTransform", transform);
        finish.Invoke();

        if (component && material)
        {
            Reflect::Call mid(component, Reflect::FindFunction(component, L"CreateDynamicMaterialInstance"));
            mid.SetNumber(L"ElementIndex", 0).SetObject(L"SourceMaterial", material);
            if (mid.Invoke())
            {
                if (UObject* instance = mid.GetObject(L"ReturnValue"))
                {
                    Reflect::Call set(instance, Reflect::FindFunction(instance, L"SetVectorParameterValue"));
                    set.SetText(L"ParameterName", L"Color").SetColor(L"Value", {color[0], color[1], color[2], 1.0f}).Invoke();
                }
            }
        }
        return actor;
    }

    auto WaypointRenderer::destroy_marker(int id) -> void
    {
        auto it = m_markers.find(id);
        if (it == m_markers.end()) return;
        for (UObject* actor : {it->second.beam, it->second.ball})
        {
            if (!actor || !Reflect::IsValidObject(actor)) continue;
            Reflect::Call(actor, Reflect::FindFunction(actor, L"K2_DestroyActor")).Invoke();
        }
        m_markers.erase(it);
    }

    // Once a second, on the game thread: make the set of marker actors match the visible waypoints.
    auto WaypointRenderer::reconcile_markers() -> void
    {
        auto& adapter = Compat::AdapterRegistry::Get();
        UObject* world = adapter.world();
        if (world != m_marker_world)
        {
            m_markers.clear(); // the old world, and its actors, are gone
            m_marker_world = world;
        }

        const ToolkitConfig config = Session::get().config();
        std::map<int, Waypoint> desired;
        if (world && config.hud_markers)
        {
            for (const Waypoint& w : WaypointStore::get().for_level(Session::get().snapshot().level))
            {
                if (w.visible) desired.emplace(w.id, w);
            }
        }

        auto signature_of = [](const Waypoint& w) {
            return std::format("{},{},{},{},{},{}", w.x, w.y, w.z, w.color[0], w.color[1], w.color[2]);
        };

        // Remove markers that are no longer wanted, changed, or whose actors the engine already destroyed.
        std::vector<int> to_remove;
        for (const auto& [id, marker] : m_markers)
        {
            auto want = desired.find(id);
            const bool alive = Reflect::IsValidObject(marker.beam) && Reflect::IsValidObject(marker.ball);
            if (!alive || want == desired.end() || signature_of(want->second) != marker.signature) to_remove.push_back(id);
        }
        for (int id : to_remove) destroy_marker(id);

        // The pawn must exist: before that the level is still starting up and spawning is unsafe.
        if (!adapter.player_pawn()) return;
        for (const auto& [id, w] : desired)
        {
            if (m_markers.contains(id)) continue;
            Marker marker;
            marker.signature = signature_of(w);
            const float color[3] = {w.color[0], w.color[1], w.color[2]};
            // Beam: a 100uu cylinder scaled to 20x20x600; ball on top.
            marker.beam = spawn_part(world, L"/Engine/BasicShapes/Cylinder.Cylinder", w.x, w.y, w.z + 300.0, 0.2, 0.2, 6.0, color);
            marker.ball = spawn_part(world, L"/Engine/BasicShapes/Sphere.Sphere", w.x, w.y, w.z + 650.0, 1.5, 1.5, 1.5, color);
            if (!marker.beam || !marker.ball)
            {
                m_spawn_failures.fetch_add(1, std::memory_order_relaxed);
                Output::send<LogLevel::Warning>(STR("[ToolkitMod] Could not spawn marker actors for waypoint '{}'\n"), ensure_str(w.name));
            }
            m_markers[id] = marker; // kept even on partial failure so it is cleaned up when the waypoint changes
        }
        m_marker_count = static_cast<int>(m_markers.size());
    }
} // namespace RC::Toolkit
