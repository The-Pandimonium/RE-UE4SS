#pragma once

// PropertyAnalyzer: ImGui-facing front-end.
//
// PropAnalyzerClient owns the UI state, drives PropAnalyzer (start/stop/configure),
// dequeues PropAccessEntry items each frame, and renders the "Property Analyzer" tab.
//
// Threading: render() and all methods must be called only from the ImGui thread.

#include <filesystem>
#include <memory>

#include <PropAnalyzerStructs.hpp>
#include <PropAnalyzer.hpp>

namespace RC::EventViewerMod
{
    class PropAnalyzerClient
    {
      public:
        // [Thread-ImGui] Called each frame from the registered ImGui tab callback.
        auto render() -> void;

        // [Thread-Any] Signal that UI state should be written to disk on the next frame.
        auto request_save_state() -> void;

        static auto GetInstance() -> PropAnalyzerClient&;

      private:
        PropAnalyzerClient();

        auto render_controls() -> void;
        auto render_history() -> void;

        auto apply_inputs() -> void;
        auto build_config() const -> PropAnalyzerConfig;

        auto save_state() -> void;
        auto load_state() -> void;
        auto check_save_request() -> bool;

        auto save_history(bool all) -> void;

        PropAnalyzerUIState m_state{};
        PropAnalyzer& m_analyzer;

        std::filesystem::path m_cfg_path{};
        std::filesystem::path m_dump_dir{};

        bool m_imgui_thread_id_set = false;
    };
} // namespace RC::EventViewerMod
