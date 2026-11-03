#include <Trainer/TargetResolver.hpp>

#include <algorithm>

#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;

    namespace
    {
        auto split_chain(const std::string& expression, std::string& root, std::vector<std::string>& chain) -> void
        {
            // "object:" paths contain dots themselves, so only split the part after the last ':' for them.
            std::string rest = expression;
            if (expression.starts_with("object:"))
            {
                root = expression;
                return;
            }
            const auto dot = rest.find('.');
            if (dot == std::string::npos)
            {
                root = rest;
                return;
            }
            root = rest.substr(0, dot);
            rest = rest.substr(dot + 1);
            size_t start = 0;
            while (start <= rest.size())
            {
                const auto next = rest.find('.', start);
                chain.push_back(rest.substr(start, next == std::string::npos ? std::string::npos : next - start));
                if (next == std::string::npos) break;
                start = next + 1;
            }
        }
    } // namespace

    auto target_roots() -> const std::vector<std::string>&
    {
        static const std::vector<std::string> roots{"player_pawn", "player_controller", "game_mode", "game_state", "world", "world_settings", "game_instance", "hud"};
        return roots;
    }

    auto select_targets(std::vector<UObject*> objects, const std::string& select, int index) -> std::vector<UObject*>
    {
        objects.erase(std::remove(objects.begin(), objects.end(), nullptr), objects.end());
        if (objects.empty() || select.empty() || select == "all") return objects;
        if (select == "first") return {objects.front()};
        if (select == "index")
        {
            if (index < 0 || static_cast<size_t>(index) >= objects.size()) return {};
            return {objects[static_cast<size_t>(index)]};
        }
        if (select == "nearest")
        {
            auto& adapter = Compat::AdapterRegistry::Get();
            auto player = adapter.actor_location(adapter.player_pawn());
            if (!player) return {objects.front()};
            UObject* best = nullptr;
            double best_distance = 0.0;
            for (UObject* obj : objects)
            {
                auto location = adapter.actor_location(obj);
                if (!location) continue;
                const double dx = location->x - player->x, dy = location->y - player->y, dz = location->z - player->z;
                const double distance = dx * dx + dy * dy + dz * dz;
                if (!best || distance < best_distance) { best = obj; best_distance = distance; }
            }
            return {best ? best : objects.front()};
        }
        return objects;
    }

    auto resolve_targets(const std::string& expression, const std::string& select, int index) -> std::vector<UObject*>
    {
        std::vector<UObject*> current;
        if (expression.empty()) return current;

        std::string root;
        std::vector<std::string> chain;
        split_chain(expression, root, chain);

        auto& adapter = Compat::AdapterRegistry::Get();
        if (root == "player_pawn") current.push_back(adapter.player_pawn());
        else if (root == "player_controller") current.push_back(adapter.player_controller());
        else if (root == "game_mode") current.push_back(adapter.game_mode());
        else if (root == "game_state") current.push_back(adapter.game_state());
        else if (root == "world") current.push_back(adapter.world());
        else if (root == "world_settings") current.push_back(adapter.world_settings());
        else if (root == "game_instance") current.push_back(Compat::Reflect::GetObjectProperty(adapter.world(), L"OwningGameInstance"));
        else if (root == "hud") current.push_back(adapter.hud(adapter.player_controller()));
        else if (root.starts_with("class:"))
        {
            current = adapter.actors_of_class(ensure_str(root.substr(6)));
            if (current.empty()) // not an actor class (widget, component, manager...): search the object array instead
                UObjectGlobals::FindAllOf(ensure_str(root.substr(6)), current);
        }
        else if (root.starts_with("object:")) current.push_back(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, ensure_str(root.substr(7))));
        else if (!root.empty())
        {
            // Bare class name: all live instances (also finds non-actors through the engine's object array).
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(ensure_str(root), found);
            current = std::move(found);
        }

        for (const std::string& step : chain)
        {
            std::vector<UObject*> next;
            for (UObject* obj : current)
            {
                if (!obj) continue;
                if (UObject* child = Compat::Reflect::GetObjectProperty(obj, ensure_str(step))) next.push_back(child);
            }
            current = std::move(next);
        }

        current.erase(std::remove(current.begin(), current.end(), nullptr), current.end());
        return select_targets(std::move(current), select, index);
    }

    auto resolve_target(const std::string& expression) -> UObject*
    {
        auto all = resolve_targets(expression);
        return all.empty() ? nullptr : all.front();
    }
} // namespace RC::Toolkit
