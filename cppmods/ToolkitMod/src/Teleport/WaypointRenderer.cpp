#include <Teleport/WaypointRenderer.hpp>

#include <cmath>
#include <format>
#include <unordered_set>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Session.hpp>
#include <Teleport/Waypoints.hpp>
#include <Unreal/Hooks/Hooks.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;

    auto WaypointRenderer::get() -> WaypointRenderer&
    {
        static WaypointRenderer instance;
        return instance;
    }

    auto WaypointRenderer::hud_status() const -> std::string
    {
        std::lock_guard lock(m_status_mutex);
        return m_status;
    }

    // Once a second: report the HUD state and create a HUD when markers are wanted but the game has none.
    auto WaypointRenderer::tick(float dt) -> void
    {
        m_since_log += dt;
        if (m_since_log >= 5.0)
        {
            m_since_log = 0.0;
            Output::send<LogLevel::Normal>(STR("[ToolkitMod:Markers] {} | hud frames={} drawn={} behind={} projectFailed={} failures={} | marker actors={} spawnFailures={}\n"), ensure_str(hud_status()), m_hud_frames.load(), m_markers_drawn.load(), m_behind_camera.load(), m_project_failed.load(), m_draw_failures.load(), m_marker_count.load(), m_spawn_failures.load());
        }
        m_since_check += dt;
        if (m_since_check < 1.0) return;
        m_since_check = 0.0;

        auto& adapter = Compat::AdapterRegistry::Get();
        UObject* pc = adapter.player_controller();
        // The world counts as stable once the same player controller has existed for a while. Creating actors while a
        // level is still loading is unsafe in some games (Voices of the Void hangs in PhysX), so the HUD fallback below
        // waits for that.
        if (!pc || pc != m_last_pc)
        {
            m_last_pc = pc;
            m_stable_seconds = 0.0;
            m_missing_hud_checks = 0;
        }
        else
        {
            m_stable_seconds += 1.0;
        }
        std::string status;
        if (!pc)
        {
            status = "no player controller";
        }
        else if (UObject* hud = adapter.hud(pc))
        {
            status = "HUD: " + to_string(hud->GetClassPrivate()->GetName()) + (m_hud_created ? " (created by toolkit)" : " (game's own)");
            if (auto show = Reflect::GetPropertyNumber(hud, L"bShowHUD"); show && *show == 0.0) status += ", bShowHUD=false";
        }
        else
        {
            status = "the game has no HUD";
            ++m_missing_hud_checks;
            const bool wanted = Session::get().config().hud_markers && !WaypointStore::get().all().empty() && m_stable_seconds >= 10.0 && m_missing_hud_checks >= 3;
            if (wanted)
            {
                UClass* hud_class = Reflect::FindClass(L"HUD");
                Reflect::Call call(pc, Reflect::FindFunction(pc, L"ClientSetHUD"));
                if (hud_class && call.valid())
                {
                    call.SetObject(L"NewHUDClass", hud_class);
                    if (call.Invoke())
                    {
                        m_hud_created = true;
                        status = "created a HUD for waypoint markers";
                        Output::send<LogLevel::Normal>(STR("[ToolkitMod] The game has no HUD; created one for waypoint markers\n"));
                    }
                }
            }
        }
        {
            std::lock_guard lock(m_status_mutex);
            m_status = status;
        }
        // Actor markers only once the world has been stable for a while (spawning during level load is unsafe).
        if (pc && m_stable_seconds >= 10.0) reconcile_markers();
    }

    auto WaypointRenderer::install() -> void
    {
        if (m_installed) return;
        m_installed = true;
        Compat::GameThread::AddTick([this](float dt) { tick(dt); });
        Hook::RegisterProcessEventPreCallback(
                [this](Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*) {
                    // Cheap rejection first: this hook sees every Blueprint/native event in the game.
                    // Compare FNames, not strings: building a string per call is far too slow for a Blueprint-heavy game.
                    static const FName draw_hud_name(STR("ReceiveDrawHUD"));
                    if (!function || function->GetNamePrivate() != draw_hud_name) return;
                    if (m_in_draw || !context) return;
                    m_hud_frames.fetch_add(1, std::memory_order_relaxed);
                    draw(context);
                },
                Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("WaypointHud")});
    }

    auto WaypointRenderer::draw(UObject* hud) -> void
    {
        const ToolkitConfig config = Session::get().config();
        if (!config.hud_markers) return;

        const SessionSnapshot snap = Session::get().snapshot();
        auto waypoints = WaypointStore::get().for_level(snap.level);
        waypoints.erase(std::remove_if(waypoints.begin(), waypoints.end(), [](const Waypoint& w) { return !w.visible; }), waypoints.end());
        if (waypoints.empty()) return;

        // Our own draw calls go through ProcessEvent, which would re-enter this hook for functions we call.
        m_in_draw = true;
        struct Guard
        {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{m_in_draw};

        UFunction* project = Reflect::FindFunction(hud, L"Project");
        UFunction* draw_rect = Reflect::FindFunction(hud, L"DrawRect");
        UFunction* draw_text = Reflect::FindFunction(hud, L"DrawText");
        UFunction* draw_line = Reflect::FindFunction(hud, L"DrawLine");
        if (!project || !draw_rect || !draw_text)
        {
            m_draw_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        for (const Waypoint& wp : waypoints)
        {
            const double dx = wp.x - snap.position.x, dy = wp.y - snap.position.y, dz = wp.z - snap.position.z;
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (config.marker_max_distance > 0.0 && distance > config.marker_max_distance) continue;

            Reflect::Call proj(hud, project);
            proj.SetVec3(L"Location", {wp.x, wp.y, wp.z});
            if (!proj.Invoke())
            {
                m_draw_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            auto screen = proj.GetVec3(L"ReturnValue");
            if (!screen) { m_project_failed.fetch_add(1, std::memory_order_relaxed); continue; }
            if (screen->z <= 0.0) { m_behind_camera.fetch_add(1, std::memory_order_relaxed); continue; } // behind the camera

            const double sx = screen->x, sy = screen->y;
            const Reflect::Color color{wp.color[0], wp.color[1], wp.color[2], 1.0f};
            const Reflect::Color shadow{0.0f, 0.0f, 0.0f, 0.85f};

            // Diamond-ish marker: a filled square with a darker outline, plus a pole down to the point.
            Reflect::Call outline(hud, draw_rect);
            outline.SetColor(L"RectColor", shadow).SetNumber(L"ScreenX", sx - 7).SetNumber(L"ScreenY", sy - 7).SetNumber(L"ScreenW", 14).SetNumber(L"ScreenH", 14).Invoke();
            Reflect::Call fill(hud, draw_rect);
            fill.SetColor(L"RectColor", color).SetNumber(L"ScreenX", sx - 5).SetNumber(L"ScreenY", sy - 5).SetNumber(L"ScreenW", 10).SetNumber(L"ScreenH", 10).Invoke();
            if (draw_line)
            {
                Reflect::Call pole(hud, draw_line);
                pole.SetNumber(L"StartScreenX", sx).SetNumber(L"StartScreenY", sy + 7).SetNumber(L"EndScreenX", sx).SetNumber(L"EndScreenY", sy + 26);
                pole.SetColor(L"LineColor", color).SetNumber(L"LineThickness", 2).Invoke();
            }

            const std::wstring label = ensure_str(wp.name) + std::format(L"  [{:.0f}m]", distance / 100.0);
            Reflect::Call text(hud, draw_text);
            text.SetText(L"Text", label).SetColor(L"TextColor", color).SetNumber(L"ScreenX", sx + 12).SetNumber(L"ScreenY", sy - 8).SetNumber(L"Scale", 1.0);
            text.SetBool(L"bScalePosition", false);
            if (text.Invoke()) m_markers_drawn.fetch_add(1, std::memory_order_relaxed);
            else m_draw_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
} // namespace RC::Toolkit
