#include <ToolkitMod.hpp>

#include <format>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/EngineProfile.hpp>
#include <EngineCompat/GameThread.hpp>
#include <Helpers/String.hpp>
#include <SelfTest.hpp>
#include <Session.hpp>
#include <Teleport/WaypointRenderer.hpp>
#include <Teleport/Waypoints.hpp>
#include <Trainer/TrainerEngine.hpp>
#include <UE4SSProgram.hpp>
#include <UI.hpp>

namespace RC::Toolkit
{
    namespace
    {
        auto data_root() -> std::filesystem::path
        {
            return std::filesystem::path(UE4SSProgram::get_program().get_working_directory()) / "Mods" / "ToolkitMod";
        }

        auto quick_waypoint() -> void
        {
            auto snap = Session::get().snapshot();
            if (!snap.has_pawn) return;
            auto& store = WaypointStore::get();
            Waypoint w;
            w.name = "Quick " + std::to_string(store.all().size() + 1);
            w.level = snap.level;
            w.x = snap.position.x;
            w.y = snap.position.y;
            w.z = snap.position.z;
            w.pitch = snap.rotation.pitch;
            w.yaw = snap.rotation.yaw;
            w.roll = snap.rotation.roll;
            w.visible = true;
            store.add(w);
            Session::get().set_message("Added waypoint '" + w.name + "'");
        }
    } // namespace

    ToolkitMod::ToolkitMod()
    {
        ModName = STR("ToolkitMod");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Trainer with a trainer creator, teleport menu with in-game waypoints, and a level switcher. Engine-version agnostic.");
        ModAuthors = STR("The-Pandimonium");

        Session::get().set_root(data_root());
        Session::get().load_config();
        TrainerEngine::get().set_directory(data_root() / "trainers");
        WaypointStore::get().set_path(data_root() / "data" / "waypoints.json");
        WaypointStore::get().load();
        TrainerEngine::get().reload();

        register_tab(STR("Trainer"), [](CppUserModBase*) {
            UE4SS_ENABLE_IMGUI();
            UI::render_trainer_tab();
        });
        register_tab(STR("Teleport"), [](CppUserModBase*) {
            UE4SS_ENABLE_IMGUI();
            UI::render_teleport_tab();
        });
        register_tab(STR("Levels"), [](CppUserModBase*) {
            UE4SS_ENABLE_IMGUI();
            UI::render_levels_tab();
        });
        register_tab(STR("Toolkit Engine"), [](CppUserModBase*) {
            UE4SS_ENABLE_IMGUI();
            UI::render_engine_tab();
        });

        register_keydown_event(Input::Key::F6, [] {
            if (Session::get().config().hotkeys) Compat::GameThread::Post([] { quick_waypoint(); });
        });
        register_keydown_event(Input::Key::F7, [] {
            if (Session::get().config().hotkeys) Session::get().teleport_back();
        });
    }

    ToolkitMod::~ToolkitMod()
    {
        TrainerEngine::get().disable_all();
    }

    auto ToolkitMod::on_ui_init() -> void
    {
        UE4SS_ENABLE_IMGUI();
    }

    auto ToolkitMod::on_unreal_init() -> void
    {
        Compat::GameThread::Install();
        Session::get().install();
        TrainerEngine::get().install();
        WaypointRenderer::get().install();

        // Engine detection runs reflection calls, so do it on the game thread once ticks are flowing.
        Compat::GameThread::Post([] {
            const auto& profile = Compat::EngineProfile::Get();
            auto& adapter = Compat::AdapterRegistry::Get();
            Session::get().set_engine_info(to_string(profile.to_string()), to_string(Compat::AdapterRegistry::Describe()));
            Output::send<LogLevel::Normal>(STR("[ToolkitMod] Engine {} | {}\n"), profile.to_string(), Compat::AdapterRegistry::Describe());
            (void)adapter;
            SelfTest::start_if_configured();
        });
    }
} // namespace RC::Toolkit
