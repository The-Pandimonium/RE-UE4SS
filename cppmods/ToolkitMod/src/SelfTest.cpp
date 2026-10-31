#include <SelfTest.hpp>

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <BlueprintStubGenerator.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/Bytecode.hpp>
#include <EngineCompat/EngineProfile.hpp>
#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Session.hpp>
#include <Teleport/WaypointRenderer.hpp>
#include <Teleport/Waypoints.hpp>
#include <Trainer/Inspect.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Trainer/TrainerEngine.hpp>

namespace RC::Toolkit::SelfTest
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;
    using Compat::AdapterRegistry;
    using Compat::GameThread;

    namespace
    {
        struct Outcome
        {
            bool done{true};
            bool pass{true};
            std::string detail{};
        };
        auto pending() -> Outcome { return {false, false, {}}; }
        auto pass(std::string detail = {}) -> Outcome { return {true, true, std::move(detail)}; }
        auto fail(std::string detail) -> Outcome { return {true, false, std::move(detail)}; }

        struct Step
        {
            std::string name;
            int timeout_ticks;
            std::function<Outcome(int tick_in_step)> run;
        };

        struct Runner
        {
            std::vector<Step> steps;
            size_t current{0};
            int tick_in_step{0};
            int passed{0};
            int failed{0};
            int skipped{0};
            std::filesystem::path log_path;
            uint64_t tick_id{0};
            bool finished{false};

            // Shared state between steps.
            UObject* pawn_before_reload{nullptr};
            Compat::Vec3 start_position{};
            std::string level_before;
            std::string level_path_before;
            std::vector<Compat::LevelInfo> levels;
            int waypoint_id{0};
            std::optional<double> original_value;
        };

        std::unique_ptr<Runner> g_runner;

        auto log_line(const std::string& line) -> void
        {
            Output::send<LogLevel::Normal>(STR("[ToolkitMod:SelfTest] {}\n"), ensure_str(line));
            if (g_runner)
            {
                std::ofstream out(g_runner->log_path, std::ios::app);
                out << line << "\n";
            }
        }

        auto pawn() -> UObject* { return AdapterRegistry::Get().player_pawn(); }

        auto near(const Compat::Vec3& a, const Compat::Vec3& b, double tolerance) -> bool
        {
            return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
        }

        auto dump_object(UObject* object, const std::filesystem::path& path) -> void
        {
            if (!object) return;
            std::ofstream out(path, std::ios::trunc);
            out << "Object: " << to_string(object->GetFullName()) << "\n\n";
            for (FProperty* p : TFieldRange<FProperty>(object->GetClassPrivate()))
            {
                std::string value;
                if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(object), object)) value = to_string(*text);
                if (value.size() > 200) value.resize(200);
                out << to_string(p->GetName()) << "\t" << to_string(Reflect::TypeName(p)) << "\t" << value << "\n";
            }
        }

        auto make_cheat(const std::string& id, const std::string& type, std::vector<ActionDef> actions) -> CheatDef
        {
            CheatDef c;
            c.id = id;
            c.label = id;
            c.type = type;
            c.actions = std::move(actions);
            return c;
        }

        auto build_steps(Runner& r) -> void
        {
            auto& steps = r.steps;
            const ToolkitConfig config = Session::get().config();
            const auto root = Session::get().root();

            steps.push_back({"engine profile", 5, [](int) {
                                 const auto& p = Compat::EngineProfile::Get();
                                 if (p.major <= 0) return fail("version not detected");
                                 return pass(to_string(p.to_string()) + " | " + to_string(AdapterRegistry::Describe()) +
                                             (p.version_sources_disagree ? " | WARNING: version sources disagree" : ""));
                             }});

            steps.push_back({"bytecode profile", 5, [](int) {
                                 const auto& bp = Compat::BytecodeProfile::Current();
                                 return pass(std::format("pointer={} opcodes={}", bp.property_pointer_size, bp.object_variable_opcodes.size()));
                             }});

            steps.push_back({"world available", 3600, [](int) {
                                 return AdapterRegistry::Get().world() ? pass(to_string(AdapterRegistry::Get().current_level_name())) : pending();
                             }});

            steps.push_back({"level enumeration", 30, [&r, root](int) {
                                 if (std::getenv("TOOLKIT_SKIPENUM")) return pass("SKIPPED: TOOLKIT_SKIPENUM");
                                 r.levels = AdapterRegistry::Get().enumerate_levels();
                                 if (r.levels.empty()) return fail("asset registry returned no worlds");
                                 std::ofstream out(root / "levels.txt", std::ios::trunc);
                                 int game_levels = 0;
                                 for (const auto& l : r.levels)
                                 {
                                     out << to_string(l.package_path) << "\n";
                                     if (l.is_game_content()) ++game_levels;
                                 }
                                 return pass(std::format("{} worlds ({} game content), written to levels.txt", r.levels.size(), game_levels));
                             }});

            // Travelling while the game is still booting is unsafe in Voices of the Void (its preLoad level is busy
            // loading and the engine ends up spinning inside PhysX), so give the game time to settle first.
            steps.push_back({"let the game finish booting", 7200, [](int tick) {
                                 static const auto started = std::chrono::steady_clock::now();
                                 (void)tick;
                                 return std::chrono::steady_clock::now() - started > std::chrono::seconds{25} ? pass() : pending();
                             }});

            // If we are sitting in a menu, optionally travel to a gameplay level first.
            steps.push_back({"enter gameplay level", 7200, [&r, config](int tick) {
                                 auto& adapter = AdapterRegistry::Get();
                                 if (config.selftest_level == "wait")
                                 {
                                     // Someone else (the player, another mod) travels; wait for the real player pawn.
                                     UObject* p = adapter.player_pawn();
                                     if (p && p->GetClassPrivate()->GetName() != L"Pawn") return pass("player pawn " + to_string(p->GetClassPrivate()->GetName()) + " in " + to_string(adapter.current_level_name()));
                                     return pending();
                                 }
                                 static std::optional<std::chrono::steady_clock::time_point> opened_at;
                                 if (adapter.player_pawn() && (config.selftest_level.empty() || to_string(adapter.current_level_name()) == config.selftest_level))
                                     return pass("pawn present in '" + to_string(adapter.current_level_name()) + "'");
                                 if (tick == 0 && !config.selftest_level.empty())
                                 {
                                     for (const auto& l : r.levels)
                                     {
                                         if (to_string(l.name) == config.selftest_level || to_string(l.package_path) == config.selftest_level)
                                         {
                                             opened_at = std::chrono::steady_clock::now();
                                             adapter.open_level(l.package_path);
                                             log_line("opening " + to_string(l.package_path));
                                             return pending();
                                         }
                                     }
                                     if (config.selftest_level.starts_with("/")) { opened_at = std::chrono::steady_clock::now(); adapter.open_level(ensure_str(config.selftest_level)); log_line("opening (direct) " + config.selftest_level); return pending(); }
                                     return fail("selftest_level '" + config.selftest_level + "' not found among the enumerated worlds");
                                 }
                                 return pending();
                             }});

            steps.push_back({"player pawn", 7200, [root](int) {
                                 UObject* p = pawn();
                                 if (!p) return pending();
                                 auto location = AdapterRegistry::Get().actor_location(p);
                                 if (!location) return fail("pawn has no K2_GetActorLocation");
                                 dump_object(p, root / "pawn_properties.txt");
                                 return pass(std::format("{} at ({:.0f}, {:.0f}, {:.0f})", to_string(p->GetClassPrivate()->GetName()), location->x, location->y, location->z));
                             }});

            steps.push_back({"settle (let the level finish loading)", 600, [](int tick) { return tick >= 180 ? pass() : pending(); }});

            steps.push_back({"class picker data", 30, [](int) {
                                 auto rows = list_live_classes(false);
                                 if (rows.empty()) return fail("no live actor classes listed");
                                 UObject* p = pawn();
                                 const std::string pawn_class = p ? to_string(p->GetClassPrivate()->GetName()) : "";
                                 bool found = false;
                                 for (const auto& row : rows) found = found || row.name == pawn_class;
                                 if (!found) return fail("pawn class " + pawn_class + " missing from the picker");
                                 auto targets = resolve_targets("class:" + pawn_class);
                                 if (targets.empty() || targets.front() != p) return fail("class:" + pawn_class + " did not resolve to the pawn");
                                 auto all = list_live_classes(true);
                                 return pass(std::format("{} actor classes, {} with non-actors; class:{} resolves to the live pawn", rows.size(), all.size(), pawn_class));
                             }});
steps.push_back({"target selection + availability", 30, [](int) {                                 UObject* p = pawn();                                 const std::string cls = p ? to_string(p->GetClassPrivate()->GetName()) : "";                                 const auto all = resolve_targets("class:" + cls, "all");                                 const auto first = resolve_targets("class:" + cls, "first");                                 const auto nearest = resolve_targets("class:" + cls, "nearest");                                 const auto far_index = resolve_targets("class:" + cls, "index", 999);                                 if (all.empty() || first.size() != 1 || nearest.size() != 1 || !far_index.empty())                                     return fail(std::format("all={} first={} nearest={} index999={}", all.size(), first.size(), nearest.size(), far_index.size()));                                 ActionDef present, missing;                                 present.kind = missing.kind = "set_property";                                 present.target = "class:" + cls;                                 missing.target = "class:ThisClassDoesNotExist_C";                                 std::string why_present, why_missing;                                 auto& engine = TrainerEngine::get();                                 const bool a = engine.check_available(make_cheat("avail_present", "toggle", {present}), why_present);                                 const bool b = engine.check_available(make_cheat("avail_missing", "toggle", {missing}), why_missing);                                 if (!a) return fail("existing class reported unavailable: " + why_present);                                 if (b) return fail("missing class reported available");                                 return pass(std::format("all={} first=1 nearest=1 index999=0; missing class -> '{}'", all.size(), why_missing));                             }});

            steps.push_back({"teleport and back", 30, [&r](int) {
                                 auto& adapter = AdapterRegistry::Get();
                                 UObject* p = pawn();
                                 auto start = adapter.actor_location(p);
                                 if (!start) return fail("no start location");
                                 r.start_position = *start;
                                 Compat::Vec3 target{start->x + 400.0, start->y + 400.0, start->z + 150.0};
                                 Session::get().teleport_player(target);
                                 auto moved = adapter.actor_location(p);
                                 if (!moved || !near(*moved, target, 50.0))
                                     return fail(moved ? std::format("moved to ({:.0f},{:.0f},{:.0f}), wanted ({:.0f},{:.0f},{:.0f})", moved->x, moved->y, moved->z, target.x, target.y, target.z)
                                                       : "no location after teleport");
                                 Session::get().teleport_back();
                                 auto back = adapter.actor_location(p);
                                 if (!back || !near(*back, *start, 50.0)) return fail("back did not return to start");
                                 return pass(std::format("teleported {:.0f}uu and returned", std::hypot(400.0, 400.0)));
                             }});

            // The slider path exactly as the UI drives it: enable, then move the slider while enabled.
            steps.push_back({"trainer slider (UI path)", 2000, [&r](int tick) {
                                 static size_t ti = 0;
                                 static UObject* p = nullptr;
                                 static double base = 0;
                                 auto& engine = TrainerEngine::get();
                                 auto height = [&] { return Reflect::GetPropertyNumber(p, L"BaseEyeHeight").value_or(-1.0); };
                                 if (tick == 0)
                                 {
                                     p = pawn();
                                     auto h = Reflect::GetPropertyNumber(p, L"BaseEyeHeight");
                                     if (!h) return pass("SKIPPED: pawn has no BaseEyeHeight");
                                     base = *h;
                                     ActionDef scale;
                                     scale.kind = "scale_property"; scale.target = "player_pawn"; scale.property = "BaseEyeHeight";
                                     TrainerDef t;
                                     t.name = "SliderTest";
                                     t.cheats = {make_cheat("slider_scale", "slider", {scale})};
                                     t.cheats[0].min = 0.5; t.cheats[0].max = 4.0; t.cheats[0].default_value = 2.0;
                                     ti = engine.add_trainer_for_test(t);
                                     return pending();
                                 }
                                 const auto state = engine.state(ti, 0);
                                 if (tick < 60) return pending(); // let availability settle
                                 if (tick == 60)
                                 {
                                     if (!state.available) return fail("slider cheat unavailable: " + state.unavailable_reason);
                                     // Untouched slider: enabling uses the default (2.0).
                                     engine.set_enabled(ti, 0, true); // runs inline on the game thread
                                     if (std::abs(height() - base * 2.0) > 0.01) return fail(std::format("enable: BaseEyeHeight {} expected {} (default 2.0)", height(), base * 2.0));
                                     engine.set_enabled(ti, 0, false);
                                     // Slider moved to exactly 1.0 while off, then enabled: 1.0 is a real choice, not "untouched".
                                     engine.set_value(ti, 0, 1.0);
                                     engine.set_enabled(ti, 0, true);
                                     if (std::abs(height() - base * 1.0) > 0.01) return fail(std::format("slider set to 1.0 then enabled: BaseEyeHeight {} expected {}", height(), base));
                                     engine.set_value(ti, 0, 3.0);
                                     if (std::abs(height() - base * 3.0) > 0.01) return fail(std::format("slider to 3.0: BaseEyeHeight {} expected {}", height(), base * 3.0));
                                     engine.set_value(ti, 0, 0.5);
                                     if (std::abs(height() - base * 0.5) > 0.01) return fail(std::format("slider to 0.5: BaseEyeHeight {} expected {}", height(), base * 0.5));
                                     // Large values (e.g. 400) work too: the range only limits dragging.
                                     engine.set_value(ti, 0, 400.0);
                                     if (std::abs(height() - base * 400.0) > 0.5) return fail(std::format("slider to 400: BaseEyeHeight {} expected {}", height(), base * 400.0));
                                     engine.set_value(ti, 0, 0.5);
                                     // The game fighting back: the cheat must re-apply the slider value.
                                     Reflect::SetPropertyNumber(p, L"BaseEyeHeight", base);
                                     return pending();
                                 }
                                 if (tick < 90) return pending();
                                 const double held = height();
                                 engine.set_enabled(ti, 0, false);
                                 const double restored = height();
                                 if (std::abs(held - base * 0.5) > 0.01) return fail(std::format("not re-applied after the game reset it: {}", held));
                                 if (std::abs(restored - base) > 0.01) return fail(std::format("not restored on disable: {} vs {}", restored, base));
                                 return pass(std::format("BaseEyeHeight {} -> x2 default -> x3 -> x400 -> x0.5, held, restored", base));
                             }});

            steps.push_back({"microbench (informational)", 600, [](int) {
                                 using clock = std::chrono::steady_clock;
                                 auto& adapter = AdapterRegistry::Get();
                                 UObject* p = pawn();
                                 FProperty* prop = Reflect::FindProperty(p, L"bCanBeDamaged");
                                 auto time_us = [](int n, auto&& fn) {
                                     const auto t0 = clock::now();
                                     for (int i = 0; i < n; ++i) fn();
                                     return std::chrono::duration<double, std::micro>(clock::now() - t0).count() / n;
                                 };
                                 std::string out;
                                 out += std::format("IsValidObject {:.2f}us; ", time_us(2000, [&] { (void)Reflect::IsValidObject(p); }));
                                 out += std::format("FindProperty(cached) {:.2f}us; ", time_us(2000, [&] { (void)Reflect::FindProperty(p, L"bCanBeDamaged"); }));
                                 out += std::format("GetNumber {:.2f}us; ", time_us(2000, [&] { (void)Reflect::GetNumber(prop, prop->ContainerPtrToValuePtr<void>(p)); }));
                                 out += std::format("world() {:.2f}us; ", time_us(200, [&] { (void)adapter.world(); }));
                                 out += std::format("player_controller() {:.2f}us; ", time_us(200, [&] { (void)adapter.player_controller(); }));
                                 out += std::format("player_pawn() {:.2f}us; ", time_us(200, [&] { (void)adapter.player_pawn(); }));
                                 out += std::format("actor_location {:.2f}us; ", time_us(200, [&] { (void)adapter.actor_location(p); }));
                                 out += std::format("actor_rotation {:.2f}us; ", time_us(200, [&] { (void)adapter.actor_rotation(p); }));
                                 out += std::format("current_level_name {:.2f}us; ", time_us(200, [&] { (void)adapter.current_level_name(); }));
                                 out += std::format("resolve player_pawn {:.2f}us; ", time_us(200, [&] { (void)resolve_targets("player_pawn"); }));
                                 out += std::format("resolve class:Pawn {:.2f}us; ", time_us(50, [&] { (void)resolve_targets("class:Pawn"); }));
                                 return pass(out);
                             }});

            // Frame-cost budget: with a typical set of cheats enabled the toolkit must stay invisible in the frame time.
            steps.push_back({"performance: cheats enabled for 15 s", 100000, [](int tick) {
                                 static std::vector<CheatDef> cheats;
                                 static std::chrono::steady_clock::time_point began;
                                 auto& engine = TrainerEngine::get();
                                 if (tick == 0)
                                 {
                                     ActionDef a;
                                     a.target = "player_pawn";
                                     auto freeze_bool = a, freeze_num = a, scale_num = a, block = a;
                                     freeze_bool.kind = "freeze_property"; freeze_bool.property = "bCanBeDamaged"; freeze_bool.value = "false";
                                     freeze_num.kind = "freeze_property"; freeze_num.property = "BaseEyeHeight"; freeze_num.value = "64"; freeze_num.mode = "at_least";
                                     scale_num.kind = "scale_property"; scale_num.property = "BaseEyeHeight"; scale_num.scale = 1.0;
                                     block.kind = "block_function"; block.function = "ReceiveAnyDamage";
                                     cheats = {make_cheat("perf_a", "toggle", {freeze_bool}), make_cheat("perf_b", "toggle", {freeze_num}),
                                               make_cheat("perf_c", "slider", {scale_num}), make_cheat("perf_d", "toggle", {block, freeze_bool})};
                                     for (auto& c : cheats) { std::string status; engine.enable_now(c, 1.0, status); }
                                     (void)GameThread::TickStats(true);
                                     began = std::chrono::steady_clock::now();
                                     return pending();
                                 }
                                 if (std::chrono::steady_clock::now() - began < std::chrono::seconds{15}) return pending();
                                 auto stats = GameThread::TickStats(true);
                                 for (auto& c : cheats) { std::string status; engine.disable_now(c, status); }
                                 std::string detail;
                                 bool over_budget = false;
                                 for (const auto& s : stats)
                                 {
                                     detail += std::format("{} avg {:.1f}us max {:.0f}us; ", s.name, s.average_us, s.max_us);
                                     if (s.name != "selftest" && s.name != "jobs" && (s.average_us > 150.0 || s.max_us > 4000.0)) over_budget = true;
                                 }
                                 return over_budget ? fail("over the frame budget: " + detail) : pass(detail);
                             }});

            steps.push_back({"waypoint persistence", 5, [&r, root](int) {
                                 auto& store = WaypointStore::get();
                                 auto snap = Session::get().refresh_now();
                                 Waypoint w;
                                 w.name = "SelfTest marker";
                                 w.level = snap.level;
                                 const double yaw = snap.rotation.yaw * 3.14159265358979 / 180.0;
                                 w.x = snap.position.x + std::cos(yaw) * 600.0;
                                 w.y = snap.position.y + std::sin(yaw) * 600.0;
                                 w.z = snap.position.z;
                                 w.visible = true;
                                 r.waypoint_id = store.add(w);
                                 store.load(); // round trip through the JSON file
                                 auto back = store.find(r.waypoint_id);
                                 if (!back || std::abs(back->x - w.x) > 0.01 || !back->visible) return fail("waypoint did not survive save/load");
                                 return pass("saved to " + (root / "data" / "waypoints.json").string());
                             }});

            steps.push_back({"in-game waypoint markers", 2400, [](int tick) {
                                 auto& renderer = WaypointRenderer::get();
                                 if (tick < 1000) return pending(); // markers need ~10 s of stable world
                                 if (renderer.marker_actor_count() > 0 && renderer.marker_spawn_failures() == 0)
                                     return pass(std::format("{} marker actor(s) spawned in the world; HUD frames={} ({})", renderer.marker_actor_count(), renderer.hud_frames(), renderer.hud_status()));
                                 if (renderer.hud_frames() == 0) return fail("no marker actors and the game never called AHUD::ReceiveDrawHUD; " + renderer.hud_status());
                                 if (renderer.markers_drawn() == 0)
                                     return fail(std::format("HUD frames={} but nothing drawn (marker behind camera or draw calls failing: {} failures)", renderer.hud_frames(), renderer.draw_failures()));
                                 return pass(std::format("hud frames={} markers drawn={} ({})", renderer.hud_frames(), renderer.markers_drawn(), renderer.hud_status()));
                             }});

            steps.push_back({"trainer: set_property + restore", 5, [](int) {
                                 UObject* p = pawn();
                                 auto before = Reflect::GetPropertyNumber(p, L"bCanBeDamaged");
                                 if (!before) return pass("SKIPPED: pawn has no bCanBeDamaged");
                                 ActionDef a;
                                 a.kind = "set_property";
                                 a.target = "player_pawn";
                                 a.property = "bCanBeDamaged";
                                 a.value = *before != 0.0 ? "false" : "true";
                                 auto cheat = make_cheat("selftest_set", "toggle", {a});
                                 auto status = TrainerEngine::get().test_cheat(cheat, true, 1.0);
                                 auto during = Reflect::GetPropertyNumber(p, L"bCanBeDamaged");
                                 TrainerEngine::get().test_cheat(cheat, false, 1.0);
                                 auto after = Reflect::GetPropertyNumber(p, L"bCanBeDamaged");
                                 if (!during || *during == *before) return fail("value did not change: " + status);
                                 if (!after || *after != *before) return fail("value not restored");
                                 return pass(std::format("bCanBeDamaged {} -> {} -> {}", *before, *during, *after));
                             }});

            steps.push_back({"trainer: scale_property slider (holds against the game)", 400, [&r](int tick) {
                                 UObject* p = pawn();
                                 UObject* movement = Reflect::GetObjectProperty(p, L"CharacterMovement");
                                 if (!movement) return pass("SKIPPED: pawn has no CharacterMovement");
                                 static ActionDef a;
                                 static CheatDef cheat;
                                 if (tick == 0)
                                 {
                                     r.original_value = Reflect::GetPropertyNumber(movement, L"MaxWalkSpeed");
                                     if (!r.original_value) return pass("SKIPPED: no MaxWalkSpeed");
                                     a = {};
                                     a.kind = "scale_property";
                                     a.target = "player_pawn.CharacterMovement";
                                     a.property = "MaxWalkSpeed";
                                     a.scale = 1.0;
                                     cheat = make_cheat("selftest_scale", "slider", {a});
                                     auto status = TrainerEngine::get().test_cheat(cheat, true, 2.0);
                                     auto now = Reflect::GetPropertyNumber(movement, L"MaxWalkSpeed");
                                     if (!now || std::abs(*now - *r.original_value * 2.0) > 0.5) return fail("scale not applied: " + status);
                                     // The game resets the value behind our back; the cheat must win it back within a few ticks.
                                     Reflect::SetPropertyNumber(movement, L"MaxWalkSpeed", *r.original_value);
                                     return pending();
                                 }
                                 if (tick < 40) return pending();
                                 auto held = Reflect::GetPropertyNumber(movement, L"MaxWalkSpeed");
                                 TrainerEngine::get().test_cheat(cheat, false, 1.0);
                                 auto restored = Reflect::GetPropertyNumber(movement, L"MaxWalkSpeed");
                                 if (!held || std::abs(*held - *r.original_value * 2.0) > 0.5) return fail(std::format("cheat did not re-apply (value {})", held ? *held : -1.0));
                                 if (!restored || std::abs(*restored - *r.original_value) > 0.5) return fail("original value not restored");
                                 return pass(std::format("MaxWalkSpeed {} x2 held, restored", *r.original_value));
                             }});

            steps.push_back({"trainer: freeze_property at_least", 400, [&r](int tick) {
                                 UObject* p = pawn();
                                 UObject* movement = Reflect::GetObjectProperty(p, L"CharacterMovement");
                                 if (!movement) return pass("SKIPPED: pawn has no CharacterMovement");
                                 static CheatDef cheat;
                                 if (tick == 0)
                                 {
                                     r.original_value = Reflect::GetPropertyNumber(movement, L"JumpZVelocity");
                                     if (!r.original_value) return pass("SKIPPED: no JumpZVelocity");
                                     ActionDef a;
                                     a.kind = "freeze_property";
                                     a.target = "player_pawn.CharacterMovement";
                                     a.property = "JumpZVelocity";
                                     a.mode = "at_least";
                                     a.value = std::format("{}", *r.original_value + 500.0);
                                     cheat = make_cheat("selftest_freeze", "toggle", {a});
                                     TrainerEngine::get().test_cheat(cheat, true, 1.0);
                                     Reflect::SetPropertyNumber(movement, L"JumpZVelocity", *r.original_value);
                                     return pending();
                                 }
                                 if (tick < 40) return pending();
                                 auto held = Reflect::GetPropertyNumber(movement, L"JumpZVelocity");
                                 TrainerEngine::get().test_cheat(cheat, false, 1.0);
                                 auto restored = Reflect::GetPropertyNumber(movement, L"JumpZVelocity");
                                 if (!held || *held < *r.original_value + 499.0) return fail("freeze did not re-apply");
                                 if (!restored || std::abs(*restored - *r.original_value) > 0.5) return fail("original value not restored");
                                 return pass(std::format("JumpZVelocity {} held at {}", *r.original_value, *held));
                             }});

            steps.push_back({"trainer: call_function with params and revert", 5, [](int) {
                                 UObject* p = pawn();
                                 auto hidden_before = Reflect::GetPropertyNumber(p, L"bHidden");
                                 if (!hidden_before) return pass("SKIPPED: pawn has no bHidden");
                                 ActionDef a;
                                 a.kind = "call_function";
                                 a.target = "player_pawn";
                                 a.function = "SetActorHiddenInGame";
                                 a.params["bNewHidden"] = "true";
                                 a.off_function = "SetActorHiddenInGame";
                                 a.off_params["bNewHidden"] = "false";
                                 auto cheat = make_cheat("selftest_call", "toggle", {a});
                                 auto status = TrainerEngine::get().test_cheat(cheat, true, 1.0);
                                 auto during = Reflect::GetPropertyNumber(p, L"bHidden");
                                 TrainerEngine::get().test_cheat(cheat, false, 1.0);
                                 auto after = Reflect::GetPropertyNumber(p, L"bHidden");
                                 if (!during || *during != 1.0) return fail("function call had no effect: " + status);
                                 if (!after || *after != 0.0) return fail("revert call had no effect");
                                 return pass("SetActorHiddenInGame true/false");
                             }});

            steps.push_back({"trainer: block_function with return override", 5, [](int) {
                                 auto& adapter = AdapterRegistry::Get();
                                 UObject* p = pawn();
                                 auto real = adapter.actor_location(p);
                                 ActionDef a;
                                 a.kind = "block_function";
                                 a.target = "player_pawn";
                                 a.function = "K2_GetActorLocation";
                                 a.return_value = "(X=11,Y=22,Z=33)";
                                 auto cheat = make_cheat("selftest_block", "toggle", {a});
                                 // The block target list refreshes on a timer, so force a refresh by enabling and ticking is
                                 // not possible inside one tick; resolve synchronously through enable_now.
                                 std::string status;
                                 TrainerEngine::get().enable_now(cheat, 1.0, status);
                                 return pass("armed: " + status);
                             }});

            steps.push_back({"trainer: block_function takes effect", 120, [](int tick) {
                                 auto& adapter = AdapterRegistry::Get();
                                 UObject* p = pawn();
                                 if (tick < 30) return pending(); // let the target list refresh
                                 auto blocked = adapter.actor_location(p);
                                 CheatDef cheat = make_cheat("selftest_block", "toggle", {});
                                 ActionDef a;
                                 a.kind = "block_function";
                                 a.target = "player_pawn";
                                 a.function = "K2_GetActorLocation";
                                 cheat.actions.push_back(a);
                                 std::string status;
                                 TrainerEngine::get().disable_now(cheat, status);
                                 auto real = adapter.actor_location(p);
                                 if (!blocked || std::abs(blocked->x - 11.0) > 0.01 || std::abs(blocked->y - 22.0) > 0.01) return fail("blocked call did not return the override");
                                 if (!real || std::abs(real->x - 11.0) < 0.01) return fail("block was not removed");
                                 return pass(std::format("override returned ({:.0f},{:.0f},{:.0f}), real call restored", blocked->x, blocked->y, blocked->z));
                             }});

            steps.push_back({"console command + time dilation", 5, [](int) {
                                 auto& adapter = AdapterRegistry::Get();
                                 const bool console = adapter.exec_console(L"stat none");
                                 const bool dilation = adapter.set_time_dilation(1.0);
                                 if (!dilation) return fail("could not set time dilation");
                                 return pass(std::format("console={} time_dilation={}", console, dilation));
                             }});

            steps.push_back({"blueprint stub export", 30, [root](int) {
                                 const auto path = (root / "blueprint_stubs.json").wstring();
                                 const bool ok = BlueprintStubGenerator::Generate(path);
                                 std::error_code ec;
                                 const auto size = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
                                 if (!ok || size == 0) return fail("exporter produced no output");
                                 return pass(std::format("{} bytes", size));
                             }});

            steps.push_back({"level switcher: reload current level", 7200, [&r](int tick) {
                                 auto& adapter = AdapterRegistry::Get();
                                 if (tick == 0)
                                 {
                                     r.pawn_before_reload = pawn();
                                     r.level_before = to_string(adapter.current_level_name());
                                     for (const auto& l : r.levels)
                                     {
                                         if (to_string(l.name) == r.level_before) r.level_path_before = to_string(l.package_path);
                                     }
                                     if (r.level_path_before.empty()) return fail("current level '" + r.level_before + "' is not in the level list");
                                     Session::get().open_level(r.level_path_before);
                                     return pending();
                                 }
                                 if (tick < 120) return pending();
                                 UObject* p = pawn();
                                 if (!p || p == r.pawn_before_reload) return pending();
                                 return pass(std::format("reloaded '{}' ({} ticks), new pawn spawned", r.level_before, tick));
                             }});

            steps.push_back({"cleanup", 5, [&r](int) {
                                 if (r.waypoint_id) WaypointStore::get().remove(r.waypoint_id);
                                 return pass();
                             }});
        }

        auto finish() -> void
        {
            auto& r = *g_runner;
            if (r.finished) return;
            r.finished = true;
            log_line(std::format("SELFTEST DONE passed={} failed={}", r.passed, r.failed));
            GameThread::RemoveTick(r.tick_id);
            if (Session::get().config().selftest_quit)
            {
                log_line("quitting the game (selftest_quit)");
                AdapterRegistry::Get().exec_console(L"quit");
            }
        }

        auto on_tick(float) -> void
        {
            if (!g_runner || g_runner->finished) return;
            auto& r = *g_runner;
            if (r.current >= r.steps.size())
            {
                finish();
                return;
            }
            Step& step = r.steps[r.current];
            Outcome outcome;
            try
            {
                outcome = step.run(r.tick_in_step);
            }
            catch (const std::exception& e)
            {
                outcome = fail(std::string("exception: ") + e.what());
            }
            catch (...)
            {
                outcome = fail("unknown exception");
            }
            if (!outcome.done)
            {
                if (++r.tick_in_step > step.timeout_ticks)
                {
                    outcome = fail("timed out");
                }
                else return;
            }
            const bool skipped = outcome.pass && outcome.detail.starts_with("SKIPPED");
            log_line(std::format("{} {}{}{}", skipped ? "SKIP" : outcome.pass ? "PASS" : "FAIL", step.name, outcome.detail.empty() ? "" : ": ", outcome.detail));
            if (skipped) ++r.skipped;
            else if (outcome.pass) ++r.passed;
            else ++r.failed;
            ++r.current;
            r.tick_in_step = 0;
        }
    } // namespace

    auto start_if_configured() -> void
    {
        if (!Session::get().config().selftest) return;
        g_runner = std::make_unique<Runner>();
        g_runner->log_path = Session::get().root() / "selftest.log";
        std::error_code ec;
        std::filesystem::create_directories(Session::get().root(), ec);
        std::ofstream(g_runner->log_path, std::ios::trunc) << "ToolkitMod self test\n";
        build_steps(*g_runner);
        g_runner->tick_id = GameThread::AddTick(on_tick, "selftest");
        log_line("self test started");
    }
} // namespace RC::Toolkit::SelfTest
