#include <Trainer/TrainerEngine.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>

#include <glaze/glaze.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <EngineCompat/Adapter.hpp>
#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;
    using RC::Compat::GameThread;

    namespace
    {
        constexpr double kRefreshSeconds = 0.25;

        auto parse_number(const std::string& text, double& out) -> bool
        {
            const char* begin = text.data();
            const char* end = begin + text.size();
            while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
            if (begin == end) return false;
            auto [ptr, ec] = std::from_chars(begin, end, out);
            return ec == std::errc{} && ptr == end;
        }

        auto parse_bool_text(std::string text, double& out) -> bool
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (text == "true" || text == "on" || text == "yes") { out = 1.0; return true; }
            if (text == "false" || text == "off" || text == "no") { out = 0.0; return true; }
            return false;
        }

        auto number_to_string(double v) -> std::string
        {
            return std::format("{}", v);
        }

        auto substitute(std::string text, double value) -> std::string
        {
            const std::string token = "{value}";
            for (auto pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos))
            {
                const std::string repl = number_to_string(value);
                text.replace(pos, token.size(), repl);
                pos += repl.size();
            }
            return text;
        }

        // Writes `text` into the named property: numbers and booleans go through the numeric path (so "1", "true",
        // "0.5" all work for bool/int/float/enum properties), everything else through the engine's own text importer.
        auto write_value(UObject* object, const std::string& property, const std::string& text, std::string& error) -> bool
        {
            if (!object) { error = "no object"; return false; }
            const StringType name = ensure_str(property);
            FProperty* p = Reflect::FindProperty(object, name);
            if (!p) { error = "property '" + property + "' not found on " + to_string(object->GetClassPrivate()->GetName()); return false; }
            void* ptr = p->ContainerPtrToValuePtr<void>(object);
            if (Reflect::IsNumeric(p))
            {
                double d = 0.0;
                if (parse_number(text, d) || parse_bool_text(text, d)) return Reflect::SetNumber(p, ptr, d) || (error = "could not write number", false);
            }
            if (Reflect::ImportText(p, ptr, ensure_str(text), object)) return true;
            error = "engine rejected value '" + text + "' for " + property;
            return false;
        }

        auto read_number(UObject* object, const std::string& property) -> std::optional<double>
        {
            return Reflect::GetPropertyNumber(object, ensure_str(property));
        }

        auto split_function_path(const std::string& function, std::string& class_part, std::string& name_part) -> void
        {
            const auto colon = function.rfind(':');
            if (colon == std::string::npos)
            {
                name_part = function;
                return;
            }
            class_part = function.substr(0, colon);
            name_part = function.substr(colon + 1);
        }
    } // namespace

    auto TrainerEngine::get() -> TrainerEngine&
    {
        static TrainerEngine instance;
        return instance;
    }

    auto TrainerEngine::key_of(const TrainerDef& trainer, const CheatDef& cheat) -> std::string
    {
        return trainer.name + "/" + (cheat.id.empty() ? cheat.label : cheat.id);
    }

    auto TrainerEngine::runtime_for(const std::string& key, size_t action_count) -> CheatRuntime&
    {
        auto& rt = m_runtime[key];
        if (rt.actions.size() != action_count) rt.actions.resize(action_count);
        return rt;
    }

    auto TrainerEngine::install() -> void
    {
        std::lock_guard lock(m_mutex);
        if (m_installed) return;
        m_installed = true;
        GameThread::Install();
        GameThread::AddTick([this](float dt) { tick(dt); });
        Hook::RegisterProcessEventPreCallback(
                [this](Hook::TCallbackIterationData<void>& data, UObject* context, UFunction* function, void* params) {
                    on_process_event(data, context, function, params);
                },
                Hook::FCallbackOptions{.OwnerModName = STR("ToolkitMod"), .HookName = STR("TrainerBlockFunction")});
    }

    // ---- Files -----------------------------------------------------------------------------------

    auto TrainerEngine::reload() -> void
    {
        std::vector<TrainerDef> loaded;
        std::vector<std::string> stems;
        std::error_code ec;
        if (std::filesystem::exists(m_directory, ec))
        {
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(m_directory, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files)
            {
                TrainerDef def;
                auto err = glz::read_file_json<glz::opts{.error_on_unknown_keys = false}>(def, file.string(), std::string{});
                if (err)
                {
                    Output::send<LogLevel::Warning>(STR("[ToolkitMod] Could not parse trainer '{}': {}\n"), file.wstring(), ensure_str(glz::format_error(err)));
                    continue;
                }
                if (def.name.empty()) def.name = file.stem().string();
                loaded.push_back(std::move(def));
                stems.push_back(file.stem().string());
            }
        }
        std::lock_guard lock(m_mutex);
        m_trainers = std::move(loaded);
        m_stems = std::move(stems);
        Output::send<LogLevel::Normal>(STR("[ToolkitMod] Loaded {} trainer file(s) from {}\n"), m_trainers.size(), m_directory.wstring());
    }

    auto TrainerEngine::trainers() const -> std::vector<TrainerDef>
    {
        std::lock_guard lock(m_mutex);
        return m_trainers;
    }

    auto TrainerEngine::file_stem_of(size_t index) const -> std::string
    {
        std::lock_guard lock(m_mutex);
        return index < m_stems.size() ? m_stems[index] : std::string{};
    }

    auto TrainerEngine::save(const TrainerDef& trainer, const std::string& stem) -> bool
    {
        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
        const auto path = m_directory / (stem + ".json");
        std::string buffer;
        if (auto err = glz::write<glz::opts{.prettify = true}>(trainer, buffer)) return false;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << buffer;
        out.close();
        reload();
        return true;
    }

    // ---- Control from any thread -----------------------------------------------------------------

    auto TrainerEngine::state(size_t ti, size_t ci) const -> CheatState
    {
        std::lock_guard lock(m_mutex);
        if (ti >= m_trainers.size() || ci >= m_trainers[ti].cheats.size()) return {};
        const auto& trainer = m_trainers[ti];
        const auto& cheat = trainer.cheats[ci];
        auto it = m_runtime.find(key_of(trainer, cheat));
        if (it == m_runtime.end()) return {false, cheat.default_value, {}};
        return {it->second.enabled, it->second.value, it->second.status};
    }

    auto TrainerEngine::set_enabled(size_t ti, size_t ci, bool enabled) -> void
    {
        TrainerDef trainer;
        CheatDef cheat;
        {
            std::lock_guard lock(m_mutex);
            if (ti >= m_trainers.size() || ci >= m_trainers[ti].cheats.size()) return;
            trainer = m_trainers[ti];
            cheat = trainer.cheats[ci];
        }
        const std::string key = key_of(trainer, cheat);
        GameThread::Post([this, key, cheat, enabled] {
            std::lock_guard lock(m_mutex);
            auto& rt = runtime_for(key, cheat.actions.size());
            if (enabled == rt.enabled) return;
            if (enabled)
            {
                if (rt.value == 1.0 && cheat.type == "slider") rt.value = cheat.default_value;
                rt.def = cheat;
                rt.enabled = apply_enable(cheat, rt, rt.status);
                if (!rt.enabled) apply_disable(cheat, rt, rt.status);
            }
            else
            {
                apply_disable(cheat, rt, rt.status);
                rt.enabled = false;
            }
        });
    }

    auto TrainerEngine::set_value(size_t ti, size_t ci, double value) -> void
    {
        TrainerDef trainer;
        CheatDef cheat;
        {
            std::lock_guard lock(m_mutex);
            if (ti >= m_trainers.size() || ci >= m_trainers[ti].cheats.size()) return;
            trainer = m_trainers[ti];
            cheat = trainer.cheats[ci];
            runtime_for(key_of(trainer, cheat), cheat.actions.size()).value = value;
        }
        const std::string key = key_of(trainer, cheat);
        GameThread::Post([this, key, cheat] {
            std::lock_guard lock(m_mutex);
            auto& rt = runtime_for(key, cheat.actions.size());
            if (rt.enabled) apply_value_change(cheat, rt);
        });
    }

    auto TrainerEngine::press(size_t ti, size_t ci) -> void
    {
        TrainerDef trainer;
        CheatDef cheat;
        double value = 1.0;
        {
            std::lock_guard lock(m_mutex);
            if (ti >= m_trainers.size() || ci >= m_trainers[ti].cheats.size()) return;
            trainer = m_trainers[ti];
            cheat = trainer.cheats[ci];
            value = runtime_for(key_of(trainer, cheat), cheat.actions.size()).value;
            if (cheat.type != "slider") value = 1.0;
        }
        const std::string key = key_of(trainer, cheat);
        GameThread::Post([this, key, cheat, value] {
            std::string status;
            bool ok = true;
            for (const auto& action : cheat.actions) ok = run_one_shot(action, value, false, status) && ok;
            std::lock_guard lock(m_mutex);
            runtime_for(key, cheat.actions.size()).status = ok ? "OK" : status;
        });
    }

    auto TrainerEngine::disable_all() -> void
    {
        GameThread::Post([this] {
            std::lock_guard lock(m_mutex);
            for (size_t ti = 0; ti < m_trainers.size(); ++ti)
            {
                for (const auto& cheat : m_trainers[ti].cheats)
                {
                    auto it = m_runtime.find(key_of(m_trainers[ti], cheat));
                    if (it == m_runtime.end() || !it->second.enabled) continue;
                    apply_disable(cheat, it->second, it->second.status);
                    it->second.enabled = false;
                }
            }
        });
    }

    auto TrainerEngine::test_cheat(const CheatDef& cheat, bool enable, double value) -> std::string
    {
        std::string result = "timed out waiting for the game thread";
        GameThread::Run([&] {
            std::string status;
            if (enable)
            {
                if (cheat.type == "toggle" || cheat.type == "slider") { enable_now(cheat, value, status); }
                else
                {
                    bool ok = true;
                    for (const auto& a : cheat.actions) ok = run_one_shot(a, value, false, status) && ok;
                    if (ok) status = "OK";
                }
            }
            else
            {
                disable_now(cheat, status);
            }
            result = status;
        });
        return result;
    }

    auto TrainerEngine::enable_now(const CheatDef& cheat, double value, std::string& status) -> bool
    {
        std::lock_guard lock(m_mutex);
        const std::string key = "__test__/" + (cheat.id.empty() ? cheat.label : cheat.id);
        auto& rt = runtime_for(key, cheat.actions.size());
        if (rt.enabled) apply_disable(cheat, rt, status);
        rt.value = value;
        rt.def = cheat;
        rt.enabled = apply_enable(cheat, rt, rt.status);
        status = rt.status;
        if (!rt.enabled) apply_disable(cheat, rt, status);
        return rt.enabled;
    }

    auto TrainerEngine::disable_now(const CheatDef& cheat, std::string& status) -> void
    {
        std::lock_guard lock(m_mutex);
        const std::string key = "__test__/" + (cheat.id.empty() ? cheat.label : cheat.id);
        auto it = m_runtime.find(key);
        if (it == m_runtime.end()) { status = "not active"; return; }
        apply_disable(cheat, it->second, status);
        it->second.enabled = false;
        status = "OK";
    }

    // ---- Game-thread internals -------------------------------------------------------------------

    auto TrainerEngine::refresh_objects(const ActionDef& action, ActionRuntime& art) -> void
    {
        art.objects = resolve_targets(action.target);
        art.next_refresh = m_clock + kRefreshSeconds;
    }

    auto TrainerEngine::run_one_shot(const ActionDef& action, double value, bool revert, std::string& status) -> bool
    {
        const std::string& kind = action.kind;
        if (kind == "console")
        {
            const std::string cmd = substitute(revert ? action.off_value : action.value, value);
            if (cmd.empty()) return true;
            if (Compat::AdapterRegistry::Get().exec_console(ensure_str(cmd))) return true;
            status = "console command failed: " + cmd;
            return false;
        }
        if (kind == "time_scale")
        {
            double v = 1.0;
            if (!revert && !parse_number(substitute(action.value.empty() ? "{value}" : action.value, value), v)) v = value;
            if (Compat::AdapterRegistry::Get().set_time_dilation(v)) return true;
            status = "could not set time dilation";
            return false;
        }

        auto objects = resolve_targets(action.target);
        if (objects.empty())
        {
            status = "target '" + action.target + "' not found";
            return false;
        }

        if (kind == "set_property" || kind == "freeze_property")
        {
            const std::string text = substitute(revert ? action.off_value : action.value, value);
            if (revert && text.empty()) return true;
            bool ok = true;
            for (UObject* obj : objects) ok = write_value(obj, action.property, text, status) && ok;
            return ok;
        }
        if (kind == "scale_property")
        {
            bool ok = true;
            for (UObject* obj : objects)
            {
                auto base = read_number(obj, action.property);
                if (!base) { status = "property '" + action.property + "' is not numeric"; ok = false; continue; }
                ok = write_value(obj, action.property, number_to_string(*base * action.scale * (revert ? 1.0 : value)), status) && ok;
            }
            return ok;
        }
        if (kind == "call_function")
        {
            const std::string& fn_name = revert ? action.off_function : action.function;
            if (fn_name.empty()) return true;
            const auto& params = revert ? action.off_params : action.params;
            bool ok = true;
            for (UObject* obj : objects)
            {
                std::string class_part, name_part;
                split_function_path(fn_name, class_part, name_part);
                UFunction* fn = Reflect::FindFunction(obj, ensure_str(name_part));
                if (!fn) { status = "function '" + fn_name + "' not found on " + to_string(obj->GetClassPrivate()->GetName()); ok = false; continue; }
                Reflect::Call call(obj, fn);
                for (const auto& [pname, pvalue] : params)
                {
                    const std::wstring wname = ensure_str(pname);
                    if (!call.has_param(wname.c_str())) { status = "function '" + fn_name + "' has no parameter '" + pname + "'"; ok = false; continue; }
                    call.SetText(wname.c_str(), ensure_str(substitute(pvalue, value)));
                }
                if (!call.Invoke()) { status = "calling '" + fn_name + "' failed"; ok = false; }
            }
            return ok;
        }
        status = "unknown action kind '" + kind + "'";
        return false;
    }

    auto TrainerEngine::apply_enable(const CheatDef& cheat, CheatRuntime& rt, std::string& status) -> bool
    {
        status.clear();
        bool all_ok = true;
        std::string last_error;
        for (size_t i = 0; i < cheat.actions.size(); ++i)
        {
            const ActionDef& action = cheat.actions[i];
            ActionRuntime& art = rt.actions[i];
            const std::string& kind = action.kind;
            std::string error;

            if (kind == "set_property" || kind == "freeze_property" || kind == "scale_property")
            {
                refresh_objects(action, art);
                art.base_numbers.clear();
                art.base_texts.clear();
                if (art.objects.empty()) { last_error = "target '" + action.target + "' not found"; all_ok = false; continue; }
                for (UObject* obj : art.objects)
                {
                    FProperty* p = Reflect::FindProperty(obj, ensure_str(action.property));
                    if (!p) { last_error = "property '" + action.property + "' not found on " + to_string(obj->GetClassPrivate()->GetName()); all_ok = false; continue; }
                    if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(obj), obj)) art.base_texts[obj] = *text;
                    if (auto number = Reflect::GetNumber(p, p->ContainerPtrToValuePtr<void>(obj))) art.base_numbers[obj] = *number;
                    bool ok = true;
                    if (kind == "scale_property")
                    {
                        if (!art.base_numbers.contains(obj)) { error = "property '" + action.property + "' is not numeric"; ok = false; }
                        else ok = write_value(obj, action.property, number_to_string(art.base_numbers[obj] * action.scale * (cheat.type == "slider" ? rt.value : 1.0)), error);
                    }
                    else ok = write_value(obj, action.property, substitute(action.value, rt.value), error);
                    if (!ok) { last_error = error; all_ok = false; }
                }
            }
            else if (kind == "block_function")
            {
                BlockEntry entry;
                entry.cheat_key = "";
                entry.function = action.function.empty() ? action.property : action.function;
                entry.target = action.target;
                {
                    std::string class_part, name_part;
                    split_function_path(entry.function, class_part, name_part);
                    entry.function_name = FName(ensure_str(name_part).c_str());
                }
                entry.return_value = action.return_value;
                // Find the cheat key from the runtime map (the caller holds the lock).
                for (auto& [k, v] : m_runtime)
                {
                    if (&v == &rt) entry.cheat_key = k;
                }
                m_blocks.push_back(std::move(entry));
                m_has_blocks = true;
            }
            else if (!run_one_shot(action, rt.value, false, error))
            {
                last_error = error;
                all_ok = false;
            }
        }
        status = all_ok ? "OK" : last_error;
        return all_ok;
    }

    auto TrainerEngine::apply_disable(const CheatDef& cheat, CheatRuntime& rt, std::string& status) -> void
    {
        status.clear();
        for (size_t i = 0; i < cheat.actions.size() && i < rt.actions.size(); ++i)
        {
            const ActionDef& action = cheat.actions[i];
            ActionRuntime& art = rt.actions[i];
            const std::string& kind = action.kind;
            std::string error;

            if (kind == "set_property" || kind == "freeze_property" || kind == "scale_property")
            {
                for (UObject* obj : art.objects)
                {
                    if (!Reflect::IsValidObject(obj)) continue;
                    FProperty* p = Reflect::FindProperty(obj, ensure_str(action.property));
                    if (!p) continue;
                    if (!action.off_value.empty() && kind != "scale_property") write_value(obj, action.property, substitute(action.off_value, 0.0), error);
                    else if (auto it = art.base_texts.find(obj); it != art.base_texts.end())
                    {
                        if (auto number = art.base_numbers.find(obj); number != art.base_numbers.end() && Reflect::IsNumeric(p))
                            Reflect::SetNumber(p, p->ContainerPtrToValuePtr<void>(obj), number->second);
                        else Reflect::ImportText(p, p->ContainerPtrToValuePtr<void>(obj), it->second, obj);
                    }
                }
                art.objects.clear();
                art.base_numbers.clear();
                art.base_texts.clear();
            }
            else if (kind == "block_function")
            {
                for (auto& [k, v] : m_runtime)
                {
                    if (&v == &rt) remove_blocks(k);
                }
            }
            else
            {
                run_one_shot(action, rt.value, true, error);
            }
        }
        status = "off";
    }

    auto TrainerEngine::apply_value_change(const CheatDef& cheat, CheatRuntime& rt) -> void
    {
        // Slider moved while enabled: re-apply value-dependent actions immediately.
        for (size_t i = 0; i < cheat.actions.size() && i < rt.actions.size(); ++i)
        {
            const ActionDef& action = cheat.actions[i];
            std::string error;
            if (action.kind == "scale_property")
            {
                for (UObject* obj : rt.actions[i].objects)
                {
                    auto base = rt.actions[i].base_numbers.find(obj);
                    if (base != rt.actions[i].base_numbers.end() && Reflect::IsValidObject(obj))
                        write_value(obj, action.property, number_to_string(base->second * action.scale * rt.value), error);
                }
            }
            else if (action.kind == "freeze_property" && action.value.find("{value}") != std::string::npos)
            {
                for (UObject* obj : rt.actions[i].objects)
                {
                    if (Reflect::IsValidObject(obj)) write_value(obj, action.property, substitute(action.value, rt.value), error);
                }
            }
            else if (action.kind == "time_scale" || action.kind == "console" || action.kind == "call_function")
            {
                if (action.value.find("{value}") != std::string::npos || action.kind == "time_scale") run_one_shot(action, rt.value, false, error);
            }
        }
    }

    auto TrainerEngine::tick(float dt) -> void
    {
        std::lock_guard lock(m_mutex);
        m_clock += dt;

        // Resolve block targets on a timer so the ProcessEvent hook never has to touch the engine itself.
        for (auto& block : m_blocks)
        {
            if (m_clock >= block.next_refresh)
            {
                block.objects = resolve_targets(block.target);
                block.next_refresh = m_clock + kRefreshSeconds;
            }
        }

        for (auto& [key, rt] : m_runtime)
        {
            {
                if (!rt.enabled) continue;
                const CheatDef& cheat = rt.def;
                for (size_t i = 0; i < cheat.actions.size() && i < rt.actions.size(); ++i)
                {
                    const ActionDef& action = cheat.actions[i];
                    if (action.kind != "freeze_property" && action.kind != "scale_property") continue;
                    ActionRuntime& art = rt.actions[i];
                    if (m_clock >= art.next_refresh)
                    {
                        auto fresh = resolve_targets(action.target);
                        for (UObject* obj : fresh)
                        {
                            if (art.base_numbers.contains(obj) || art.base_texts.contains(obj)) continue;
                            if (FProperty* p = Reflect::FindProperty(obj, ensure_str(action.property)))
                            {
                                if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(obj), obj)) art.base_texts[obj] = *text;
                                if (auto number = Reflect::GetNumber(p, p->ContainerPtrToValuePtr<void>(obj))) art.base_numbers[obj] = *number;
                            }
                        }
                        art.objects = std::move(fresh);
                        art.next_refresh = m_clock + kRefreshSeconds;
                    }
                    for (UObject* obj : art.objects)
                    {
                        if (!Reflect::IsValidObject(obj)) continue;
                        FProperty* p = Reflect::FindProperty(obj, ensure_str(action.property));
                        if (!p) continue;
                        void* ptr = p->ContainerPtrToValuePtr<void>(obj);
                        std::string error;
                        if (action.kind == "scale_property")
                        {
                            auto base = art.base_numbers.find(obj);
                            if (base == art.base_numbers.end()) continue;
                            const double wanted = base->second * action.scale * (cheat.type == "slider" ? rt.value : 1.0);
                            auto current = Reflect::GetNumber(p, ptr);
                            if (!current || std::abs(*current - wanted) > 1e-4) Reflect::SetNumber(p, ptr, wanted);
                            continue;
                        }
                        const std::string text = substitute(action.value, rt.value);
                        double wanted = 0.0;
                        const bool numeric = Reflect::IsNumeric(p) && (parse_number(text, wanted) || parse_bool_text(text, wanted));
                        if (numeric)
                        {
                            auto current = Reflect::GetNumber(p, ptr);
                            const bool differs = !current || std::abs(*current - wanted) > 1e-4;
                            bool write = differs;
                            if (action.mode == "at_least") write = !current || *current < wanted;
                            else if (action.mode == "at_most") write = !current || *current > wanted;
                            if (write) Reflect::SetNumber(p, ptr, wanted);
                        }
                        else
                        {
                            write_value(obj, action.property, text, error);
                        }
                    }
                }
            }
        }
    }

    auto TrainerEngine::remove_blocks(const std::string& cheat_key) -> void
    {
        m_blocks.erase(std::remove_if(m_blocks.begin(), m_blocks.end(), [&](const BlockEntry& b) { return b.cheat_key == cheat_key; }), m_blocks.end());
        m_has_blocks = !m_blocks.empty();
    }

    auto TrainerEngine::on_process_event(Hook::TCallbackIterationData<void>& data, UObject* context, UFunction* function, void* params) -> void
    {
        if (!function || !context) return;
        if (!m_has_blocks.load(std::memory_order_relaxed)) return;
        std::unique_lock lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock() || m_blocks.empty()) return;

        for (const auto& block : m_blocks)
        {
            if (function->GetNamePrivate() != block.function_name) continue;
            if (!block.target.empty() && block.target != "any" &&
                std::find(block.objects.begin(), block.objects.end(), context) == block.objects.end())
                continue;

            if (!block.return_value.empty() && params)
            {
                for (FProperty* p : TFieldRange<FProperty>(function))
                {
                    if (p->HasAnyPropertyFlags(CPF_ReturnParm))
                    {
                        Reflect::ImportText(p, static_cast<uint8_t*>(params) + p->GetOffset_Internal(), ensure_str(block.return_value), context);
                        break;
                    }
                }
            }
            data.PreventOriginalFunctionCall();
            return;
        }
    }
} // namespace RC::Toolkit
