#include <PropAnalyzerClient.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <UE4SSProgram.hpp>
#include <Helpers/String.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <fmt/core.h>
#include <glaze/glaze.hpp>

static std::vector<std::string> pa_split_by_comma(const std::string& str)
{
    std::vector<std::string> result;
    if (str.empty()) return result;

    const std::string_view sv{str};
    size_t start = 0;

    auto trim = [](std::string_view v) -> std::string_view {
        const auto lead = v.find_first_not_of(" \t\n\r\f\v");
        if (lead == std::string_view::npos) return {};
        v.remove_prefix(lead);
        const auto trail = v.find_last_not_of(" \t\n\r\f\v");
        if (trail == std::string_view::npos) return {};
        return v.substr(0, trail + 1);
    };

    while (start <= sv.size())
    {
        size_t end = sv.find(',', start);
        if (end == std::string_view::npos) end = sv.size();
        const auto tok = trim(sv.substr(start, end - start));
        if (!tok.empty()) result.emplace_back(to_lower_case(tok));
        if (end == sv.size()) break;
        start = end + 1;
    }
    return result;
}

namespace RC::EventViewerMod
{
    using namespace std::literals::string_literals;

    // ─── PropAccessEntry render / to_string ─────────────────────────────────────

    auto PropAccessEntry::render(bool with_context_menu) const -> void
    {
        constexpr auto SET_COLOR = ImVec4{1.0f, 0.55f, 0.1f, 1.0f}; // orange
        constexpr auto GET_COLOR = ImVec4{0.3f, 0.9f, 0.4f, 1.0f};  // green

        std::string label;
        if (access_type == EPropAccessType::Set)
        {
            ImGui::TextColored(SET_COLOR, "[SET]");
            label = fmt::format("{} ({})  ::  {}  |  {}  |  {} \xE2\x86\x92 {}",
                object_name, class_name, function_name, property_name, old_value, new_value);
        }
        else
        {
            ImGui::TextColored(GET_COLOR, "[GET]");
            label = fmt::format("{} ({})  ::  {}  |  {}  |  {}",
                object_name, class_name, function_name, property_name, old_value);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());

        if (with_context_menu)
        {
            ImGui::SetItemTooltip("Right-click for options");
            if (ImGui::BeginPopupContextItem("PAEntryPopup"))
            {
                auto copy = [](const std::string& s) {
                    ImGui::LogToClipboard();
                    ImVec2 dummy{};
                    ImGui::LogRenderedText(&dummy, s.c_str(), s.c_str() + s.size());
                    ImGui::LogFinish();
                };
                if (ImGui::MenuItem("Copy Object Name"))   copy(object_name);
                if (ImGui::MenuItem("Copy Class Name"))    copy(class_name);
                if (ImGui::MenuItem("Copy Function Name")) copy(function_name);
                if (ImGui::MenuItem("Copy Property Name")) copy(property_name);
                ImGui::Separator();
                if (ImGui::MenuItem("Copy Full Line"))     copy(to_string());
                ImGui::EndPopup();
            }
        }
    }

    auto PropAccessEntry::to_string() const -> std::string
    {
        if (access_type == EPropAccessType::Set)
            return fmt::format("[SET]  {} ({})  ::  {}  |  {}  |  {} -> {}\n",
                object_name, class_name, function_name, property_name, old_value, new_value);
        return fmt::format("[GET]  {} ({})  ::  {}  |  {}  |  {}\n",
            object_name, class_name, function_name, property_name, old_value);
    }

    // ─── PropAnalyzerClient ─────────────────────────────────────────────────────

    PropAnalyzerClient::PropAnalyzerClient() : m_analyzer(PropAnalyzer::GetInstance())
    {
        const auto wd = std::filesystem::path{StringType{UE4SSProgram::get_program().get_working_directory()}};
        const auto mod_root = wd / "Mods" / "EventViewerMod";
        m_cfg_path = mod_root / "config" / "pa_settings.json";
        m_dump_dir = mod_root / "captures";

        std::error_code ec;
        std::filesystem::create_directories(m_cfg_path.parent_path(), ec);
        std::filesystem::create_directories(m_dump_dir, ec);

        load_state();
    }

    auto PropAnalyzerClient::GetInstance() -> PropAnalyzerClient&
    {
        static PropAnalyzerClient s_instance;
        return s_instance;
    }

    auto PropAnalyzerClient::request_save_state() -> void
    {
        m_state.needs_save.test_and_set(std::memory_order_release);
    }

    auto PropAnalyzerClient::check_save_request() -> bool
    {
        if (m_state.needs_save.test(std::memory_order_acquire))
        {
            save_state();
            m_state.needs_save.clear(std::memory_order_release);
            return true;
        }
        return false;
    }

    // ─── Top-level render ───────────────────────────────────────────────────────

    auto PropAnalyzerClient::render() -> void
    {
        if (!m_imgui_thread_id_set)
        {
            m_analyzer.set_imgui_thread_id(std::this_thread::get_id());
            m_imgui_thread_id_set = true;
        }

        check_save_request();

        if (ImGui::Checkbox("Enable##PA", &m_state.enabled))
        {
            request_save_state();
            if (!m_state.enabled)
            {
                m_analyzer.stop();
                m_state.started = false;
                m_state.history.clear();
            }
        }

        if (!m_state.enabled) return;

        render_controls();

        if (m_state.started)
        {
            m_analyzer.dequeue([this](PropAccessEntry&& e) {
                m_state.history.push_back(std::move(e));
            });
        }

        render_history();

        if (ImGui::BeginPopupModal("PA Saved File", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::Text("Saved to %s", m_state.last_save_path.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ─── Controls ───────────────────────────────────────────────────────────────

    auto PropAnalyzerClient::render_controls() -> void
    {
        bool apply = false;
        auto& cfg = m_state.config;

        // ── Properties ──
        const bool props_enter = ImGui::InputText(
            "Properties##PA", &m_state.props_input,
            ImGuiInputTextFlags_ElideLeft | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Comma-separated property names to watch (case-insensitive substring match)");
        ImGui::SameLine();
        if (props_enter || ImGui::Button("Apply##PAProps")) apply = true;
        ImGui::SameLine();
        if (ImGui::Button("Clear##PAProps")) { m_state.props_input.clear(); apply = true; }

        // ── Classes/Objects ──
        const bool classes_enter = ImGui::InputText(
            "Classes/Objects##PA", &m_state.classes_input,
            ImGuiInputTextFlags_ElideLeft | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Comma-separated class or object name substrings to restrict monitoring (empty = all)");
        ImGui::SameLine();
        if (classes_enter || ImGui::Button("Apply##PAClasses")) apply = true;
        ImGui::SameLine();
        if (ImGui::Button("Clear##PAClasses")) { m_state.classes_input.clear(); apply = true; }

        // ── Functions ──
        const bool funcs_enter = ImGui::InputText(
            "Functions##PA", &m_state.funcs_input,
            ImGuiInputTextFlags_ElideLeft | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Comma-separated function name substrings. Matching functions snapshot ALL properties (ignores Properties filter), capturing every read and write.");
        ImGui::SameLine();
        if (funcs_enter || ImGui::Button("Apply##PAFuncs")) apply = true;
        ImGui::SameLine();
        if (ImGui::Button("Clear##PAFuncs")) { m_state.funcs_input.clear(); apply = true; }

        if (apply) { apply_inputs(); request_save_state(); }

        // ── GET detection options ──────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::TextDisabled("GET detection:");
        ImGui::SameLine();
        if (ImGui::Checkbox("PI##PA", &cfg.scan_pi_gets))  request_save_state();
        ImGui::SetItemTooltip(
            "Scan bytecode in the ProcessInternal pre-hook (Blueprint VM entry point).\n"
            "Covers all Blueprint functions and custom events. Recommended ON.");
        ImGui::SameLine();
        if (ImGui::Checkbox("PE##PA", &cfg.scan_pe_gets))  request_save_state();
        ImGui::SetItemTooltip(
            "Also scan bytecode in the ProcessEvent/PLSF pre-hook.\n"
            "Catches cases where a function goes through ProcessEvent but not ProcessInternal\n"
            "(e.g. native C++ thunks). May produce duplicate GET entries for Blueprint functions\n"
            "that go through both hooks. Off by default.");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("SET detection:");
        ImGui::SameLine();
        if (ImGui::Checkbox("Deep child##PA", &cfg.deep_child_monitor)) request_save_state();
        ImGui::SetItemTooltip(
            "When inside a func-scope frame, use func-scope props (all properties, class-filtered)\n"
            "for every nested ProcessEvent/PLSF call. This makes mid-function object creation\n"
            "and retrieval visible: the first call made ON a newly-created object will snapshot\n"
            "all its properties and detect subsequent writes. Can be noisy; off by default.");

        // ── Action buttons ──
        ImGui::Spacing();
        if (ImGui::Button(m_state.started ? "Stop##PA" : "Start##PA"))
        {
            if (!m_state.started)
            {
                apply_inputs();
                m_analyzer.configure(build_config());
                m_state.started = m_analyzer.start();
            }
            else
            {
                m_analyzer.stop();
                m_state.started = false;
            }
        }
        ImGui::SetItemTooltip("Start or stop property capture");

        ImGui::SameLine();
        if (ImGui::Button("Clear##PACurrent") && !m_state.history.empty())
            m_state.history.clear();
        ImGui::SetItemTooltip("Clear the capture history");

        ImGui::SameLine();
        if (ImGui::Button("Save##PACurrent")) save_history(false);
        ImGui::SetItemTooltip("Save visible history to a timestamped file");

        ImGui::SameLine();
        if (ImGui::Button("Save All##PA")) save_history(true);
        ImGui::SetItemTooltip("Save all history to a timestamped file");

        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Checkbox("Show Reads##PA",  &m_state.show_reads))  request_save_state();
        ImGui::SameLine();
        if (ImGui::Checkbox("Show Writes##PA", &m_state.show_writes)) request_save_state();

        ImGui::TextDisabled("Captured: %zu entries", m_state.history.size());
    }

    // ─── History view ────────────────────────────────────────────────────────────

    auto PropAnalyzerClient::render_history() -> void
    {
        if (m_state.history.empty()) return;

        const auto area    = ImGui::GetContentRegionAvail();
        const auto& pad    = ImGui::GetStyle().WindowPadding;
        const auto& scroll = ImGui::GetStyle().ScrollbarSize;
        const ImVec2 sz{area.x - (pad.x + scroll), area.y - ((pad.y + scroll) * 2.0f)};

        ImGui::BeginChild("##PAView", sz, ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const bool with_menus = !m_state.started;
        int id = 0;
        for (const auto& entry : m_state.history)
        {
            if (entry.is_disabled) continue;
            if (entry.access_type == EPropAccessType::Get && !m_state.show_reads)  continue;
            if (entry.access_type == EPropAccessType::Set && !m_state.show_writes) continue;
            ImGui::PushID(id++);
            entry.render(with_menus);
            ImGui::PopID();
        }

        if (m_state.started) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    // ─── Filter + config helpers ─────────────────────────────────────────────────

    auto PropAnalyzerClient::apply_inputs() -> void
    {
        m_state.watched_prop_names = pa_split_by_comma(m_state.props_input);
        m_state.class_tokens       = pa_split_by_comma(m_state.classes_input);
        m_state.func_patterns      = pa_split_by_comma(m_state.funcs_input);
    }

    auto PropAnalyzerClient::build_config() const -> PropAnalyzerConfig
    {
        PropAnalyzerConfig cfg = m_state.config;
        cfg.prop_names    = m_state.watched_prop_names;
        cfg.class_tokens  = m_state.class_tokens;
        cfg.func_patterns = m_state.func_patterns;
        return cfg;
    }

    // ─── Persistence ─────────────────────────────────────────────────────────────

    auto PropAnalyzerClient::save_state() -> void
    {
        std::error_code ec;
        std::filesystem::create_directories(m_cfg_path.parent_path(), ec);

        const auto& cfg = m_state.config;
        std::unordered_map<std::string, std::string> m;
        m.emplace("Enabled",          std::to_string(m_state.enabled));
        m.emplace("PropsInput",       m_state.props_input);
        m.emplace("ClassesInput",     m_state.classes_input);
        m.emplace("FuncsInput",       m_state.funcs_input);
        m.emplace("ShowReads",        std::to_string(m_state.show_reads));
        m.emplace("ShowWrites",       std::to_string(m_state.show_writes));
        m.emplace("ScanPIGets",       std::to_string(cfg.scan_pi_gets));
        m.emplace("ScanPEGets",       std::to_string(cfg.scan_pe_gets));
        m.emplace("DeepChildMonitor", std::to_string(cfg.deep_child_monitor));

        (void)glz::write_file_json(m, m_cfg_path.string(), std::string{});
    }

    auto PropAnalyzerClient::load_state() -> void
    {
        if (!std::filesystem::exists(m_cfg_path) || !std::filesystem::is_regular_file(m_cfg_path))
            return;

        std::unordered_map<std::string, std::string> m{};
        auto ec = glz::read_file_json(m, m_cfg_path.string(), std::string{});
        if (ec.ec != glz::error_code::none) return;

        auto get_bool = [&](const char* key, bool def) -> bool {
            auto it = m.find(key);
            return it == m.end() ? def : (it->second != "0");
        };
        auto get_str  = [&](const char* key) -> std::string {
            auto it = m.find(key);
            return it == m.end() ? std::string{} : it->second;
        };

        try
        {
            m_state.enabled       = get_bool("Enabled",    false);
            m_state.props_input   = get_str("PropsInput");
            m_state.classes_input = get_str("ClassesInput");
            m_state.funcs_input   = get_str("FuncsInput");
            m_state.show_reads    = get_bool("ShowReads",  true);
            m_state.show_writes   = get_bool("ShowWrites", true);

            auto& cfg = m_state.config;
            cfg.scan_pi_gets       = get_bool("ScanPIGets",       true);
            cfg.scan_pe_gets       = get_bool("ScanPEGets",       false);
            cfg.deep_child_monitor = get_bool("DeepChildMonitor", false);

            apply_inputs();
        }
        catch (...) {}
    }

    // ─── Save capture ─────────────────────────────────────────────────────────────

    auto PropAnalyzerClient::save_history(const bool all) -> void
    {
        if (m_state.history.empty()) return;

        std::error_code ec;
        std::filesystem::create_directories(m_dump_dir, ec);

        const auto now   = std::chrono::system_clock::now();
        const auto now_t = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm{};
        localtime_s(&local_tm, &now_t);

        std::ostringstream oss;
        oss << std::put_time(&local_tm, "%Y-%m-%d %H-%M-%S");

        const auto path = m_dump_dir / ("PropertyAnalyzer Capture "s + oss.str() + ".txt");
        std::ofstream file{path};
        if (!file.is_open()) return;

        for (const auto& entry : m_state.history)
        {
            if (!all && entry.is_disabled) continue;
            file << entry.to_string();
        }

        file.close();
        m_state.last_save_path = path.string();
        ImGui::OpenPopup("PA Saved File");
    }

} // namespace RC::EventViewerMod
