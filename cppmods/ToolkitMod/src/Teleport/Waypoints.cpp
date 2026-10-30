#include <Teleport/Waypoints.hpp>

#include <algorithm>
#include <fstream>

#include <glaze/glaze.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

namespace RC::Toolkit
{
    auto WaypointStore::get() -> WaypointStore&
    {
        static WaypointStore instance;
        return instance;
    }

    auto WaypointStore::load() -> bool
    {
        std::lock_guard lock(m_mutex);
        std::error_code ec;
        if (!std::filesystem::exists(m_path, ec))
        {
            m_data = {};
            return true;
        }
        WaypointFile file;
        auto err = glz::read_file_json<glz::opts{.error_on_unknown_keys = false}>(file, m_path.string(), std::string{});
        if (err)
        {
            Output::send<LogLevel::Warning>(STR("[ToolkitMod] Could not read waypoints: {}\n"), RC::ensure_str(glz::format_error(err)));
            return false;
        }
        m_data = std::move(file);
        int max_id = 0;
        for (const auto& w : m_data.waypoints) max_id = std::max(max_id, w.id);
        m_data.next_id = std::max(m_data.next_id, max_id + 1);
        return true;
    }

    auto WaypointStore::save() const -> bool
    {
        std::lock_guard lock(m_mutex);
        std::error_code ec;
        std::filesystem::create_directories(m_path.parent_path(), ec);
        std::string buffer;
        if (auto err = glz::write<glz::opts{.prettify = true}>(m_data, buffer)) return false;
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << buffer;
        return static_cast<bool>(out);
    }

    auto WaypointStore::add(Waypoint waypoint) -> int
    {
        int id = 0;
        {
            std::lock_guard lock(m_mutex);
            waypoint.id = m_data.next_id++;
            id = waypoint.id;
            m_data.waypoints.push_back(std::move(waypoint));
        }
        save();
        return id;
    }

    auto WaypointStore::remove(int id) -> bool
    {
        bool removed = false;
        {
            std::lock_guard lock(m_mutex);
            auto it = std::remove_if(m_data.waypoints.begin(), m_data.waypoints.end(), [&](const Waypoint& w) { return w.id == id; });
            removed = it != m_data.waypoints.end();
            m_data.waypoints.erase(it, m_data.waypoints.end());
        }
        if (removed) save();
        return removed;
    }

    auto WaypointStore::update(const Waypoint& waypoint) -> bool
    {
        bool found = false;
        {
            std::lock_guard lock(m_mutex);
            for (auto& w : m_data.waypoints)
            {
                if (w.id == waypoint.id)
                {
                    w = waypoint;
                    found = true;
                    break;
                }
            }
        }
        if (found) save();
        return found;
    }

    auto WaypointStore::find(int id) const -> std::optional<Waypoint>
    {
        std::lock_guard lock(m_mutex);
        for (const auto& w : m_data.waypoints)
        {
            if (w.id == id) return w;
        }
        return std::nullopt;
    }

    auto WaypointStore::set_visible(int id, bool visible) -> void
    {
        {
            std::lock_guard lock(m_mutex);
            for (auto& w : m_data.waypoints)
            {
                if (w.id == id) w.visible = visible;
            }
        }
        save();
    }

    auto WaypointStore::for_level(const std::string& level) const -> std::vector<Waypoint>
    {
        std::lock_guard lock(m_mutex);
        std::vector<Waypoint> out;
        for (const auto& w : m_data.waypoints)
        {
            if (w.level == kGlobalLevel || w.level == level) out.push_back(w);
        }
        return out;
    }

    auto WaypointStore::all() const -> std::vector<Waypoint>
    {
        std::lock_guard lock(m_mutex);
        return m_data.waypoints;
    }
} // namespace RC::Toolkit
