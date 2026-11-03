#include <Session.hpp>

#include <cstdlib>
#include <format>
#include <fstream>

#include <glaze/glaze.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/GameThread.hpp>
#include <Helpers/String.hpp>
#include <LevelLauncher.hpp>

namespace RC::Toolkit
{
    using Compat::AdapterRegistry;
    using Compat::GameThread;

    auto Session::get() -> Session&
    {
        static Session instance;
        return instance;
    }

    auto Session::install() -> void
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_installed) return;
            m_installed = true;
        }
        GameThread::Install();
        GameThread::AddTick([this](float dt) { tick(dt); }, "session");
    }

    auto Session::load_config() -> void
    {
        ToolkitConfig config;
        const auto path = m_root / "config.json";
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
        {
            auto err = glz::read_file_json<glz::opts{.error_on_unknown_keys = false}>(config, path.string(), std::string{});
            if (err) Output::send<LogLevel::Warning>(STR("[ToolkitMod] config.json is invalid: {}\n"), ensure_str(glz::format_error(err)));
        }
        std::lock_guard lock(m_mutex);
        m_config = config;
        m_level_guards = config.level_guards;
    }

    auto Session::save_config() const -> void
    {
        ToolkitConfig config;
        {
            std::lock_guard lock(m_mutex);
            config = m_config;
        }
        std::error_code ec;
        std::filesystem::create_directories(m_root, ec);
        std::string buffer;
        if (glz::write<glz::opts{.prettify = true}>(config, buffer)) return;
        std::ofstream out(m_root / "config.json", std::ios::binary | std::ios::trunc);
        out << buffer;
    }

    auto Session::config() const -> ToolkitConfig
    {
        std::lock_guard lock(m_mutex);
        return m_config;
    }

    auto Session::update_config(const ToolkitConfig& config) -> void
    {
        {
            std::lock_guard lock(m_mutex);
            m_config = config;
            m_level_guards = config.level_guards;
        }
        save_config();
    }

    auto Session::snapshot() const -> SessionSnapshot
    {
        std::lock_guard lock(m_mutex);
        return m_snapshot;
    }

    auto Session::last_message() const -> std::string
    {
        std::lock_guard lock(m_mutex);
        return m_message;
    }

    auto Session::set_message(std::string message) -> void
    {
        std::lock_guard lock(m_mutex);
        m_message = std::move(message);
    }

    auto Session::has_back_position() const -> bool
    {
        std::lock_guard lock(m_mutex);
        return !m_back_stack.empty();
    }

    auto Session::refresh_now() -> SessionSnapshot
    {
        auto& adapter = AdapterRegistry::Get();
        SessionSnapshot snap;
        Unreal::UObject* world = adapter.world();
        snap.has_world = world != nullptr;
        if (world) snap.level = to_string(adapter.current_level_name());
        if (Unreal::UObject* pawn = adapter.player_pawn())
        {
            snap.has_pawn = true;
            snap.pawn_name = to_string(pawn->GetName());
            snap.pawn_class = to_string(pawn->GetClassPrivate()->GetName());
            if (auto location = adapter.actor_location(pawn)) snap.position = *location;
            if (auto rotation = adapter.actor_rotation(pawn)) snap.rotation = *rotation;
        }
        snap.updated_tick = GameThread::TickCount();
        std::lock_guard lock(m_mutex);
        m_snapshot = snap;
        return snap;
    }

    auto Session::tick(float dt) -> void
    {
        m_since_update += dt;
        if (m_since_update < 0.25) return;
        m_since_update = 0.0;
        try
        {
            const SessionSnapshot snap = refresh_now();
            // Debug aid: TOOLKIT_DEBUG_OPEN=<package path> opens that level 12 s after the main menu appears (the same path as the Levels tab).
            static double menu_seconds = 0.0;
            static bool debug_opened = false;
            if (const char* wanted = std::getenv("TOOLKIT_DEBUG_OPEN"); wanted && !debug_opened)
            {
                menu_seconds = snap.level == "menu" ? menu_seconds + 0.25 : 0.0;
                if (menu_seconds >= 12.0)
                {
                    debug_opened = true;
                    open_level(wanted);
                }
            }
        }
        catch (...)
        {
        }
    }

    auto Session::teleport_player(Vec3 position, std::optional<Rot3> rotation) -> void
    {
        GameThread::Post([this, position, rotation] {
            auto& adapter = AdapterRegistry::Get();
            Unreal::UObject* pawn = adapter.player_pawn();
            if (!pawn)
            {
                set_message("Teleport failed: no player pawn (are you in a level?)");
                return;
            }
            const auto before_location = adapter.actor_location(pawn);
            const auto before_rotation = adapter.actor_rotation(pawn);
            if (!adapter.teleport_actor(pawn, position, rotation))
            {
                set_message("Teleport failed: the engine rejected the move");
                return;
            }
            adapter.stop_movement(pawn);
            if (rotation)
            {
                if (Unreal::UObject* pc = adapter.player_controller()) adapter.set_control_rotation(pc, *rotation);
            }
            if (before_location)
            {
                std::lock_guard lock(m_mutex);
                m_back_stack.emplace_back(*before_location, before_rotation.value_or(Rot3{}));
                if (m_back_stack.size() > 32) m_back_stack.erase(m_back_stack.begin());
            }
            set_message(std::format("Teleported to ({:.0f}, {:.0f}, {:.0f})", position.x, position.y, position.z));
            refresh_now();
        });
    }

    auto Session::teleport_back() -> void
    {
        std::pair<Vec3, Rot3> target;
        {
            std::lock_guard lock(m_mutex);
            if (m_back_stack.empty()) return;
            target = m_back_stack.back();
            m_back_stack.pop_back();
        }
        GameThread::Post([this, target] {
            auto& adapter = AdapterRegistry::Get();
            Unreal::UObject* pawn = adapter.player_pawn();
            if (!pawn || !adapter.teleport_actor(pawn, target.first, target.second))
            {
                set_message("Back failed: no player pawn");
                return;
            }
            adapter.stop_movement(pawn);
            if (Unreal::UObject* pc = adapter.player_controller()) adapter.set_control_rotation(pc, target.second);
            set_message("Returned to previous position");
            refresh_now();
        });
    }

    auto Session::open_level(std::string package_path, std::string options) -> void
    {
        GameThread::Post([this, package_path, options] {
            auto& adapter = AdapterRegistry::Get();
            // Games whose menu does more than load the map get their own recipe (see LevelLauncher).
            // Games whose own loader does more than `open` get a recipe (see LevelLauncher).
            if (auto& launcher = LevelLauncher::get(); launcher.available())
            {
                std::string error;
                const bool opened = launcher.open(package_path, error);
                Output::send<LogLevel::Normal>(STR("[ToolkitMod] Level launch recipe for {}: {}\n"), ensure_str(package_path), opened ? std::wstring(L"started") : ensure_str(error));
                if (opened) set_message("Opening level " + package_path + " the way the game's menu does ...");
                else set_message("Could not open " + package_path + ": " + error);
                return;
            }
            if (adapter.open_level(ensure_str(package_path), ensure_str(options)))
                set_message("Opening level " + package_path + " ...");
            else
                set_message("Could not open level " + package_path);
        });
    }
} // namespace RC::Toolkit

namespace RC::Toolkit
{
    auto Session::set_engine_info(std::string profile, std::string adapter) -> void
    {
        std::lock_guard lock(m_mutex);
        m_engine_profile = std::move(profile);
        m_engine_adapter = std::move(adapter);
    }

    auto Session::engine_info() const -> std::pair<std::string, std::string>
    {
        std::lock_guard lock(m_mutex);
        return {m_engine_profile, m_engine_adapter};
    }
} // namespace RC::Toolkit
