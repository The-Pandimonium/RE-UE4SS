#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Trainer/TrainerModel.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>

namespace RC::Toolkit
{
    // Snapshot of one cheat's runtime state, safe to read from the UI thread.
    struct CheatState
    {
        bool enabled{false};
        double value{1.0};
        std::string status{};
        // False while the cheat has nothing to act on in the current level (e.g. its target class has no live objects).
        // Such a cheat can be switched off but not on / pressed.
        bool available{true};
        std::string unavailable_reason{};
        // Slider bounds as currently resolved (placeholders evaluated at the last refresh).
        double min{0.0};
        double max{100.0};
        double default_value{1.0};
        bool has_current{false}; // `current` is valid
        double current{0.0};     // live value of the cheat's first property when last refreshed
        std::string bounds_note{}; // why an expression could not be resolved, if so
    };

    // Evaluates + - * / ( ) over plain numbers (also "true"/"false"). Nullopt when the text is not such an expression.
    auto evaluate_expression(const std::string& text) -> std::optional<double>;

    class TrainerEngine
    {
      public:
        static auto get() -> TrainerEngine&;

        // Registers the tick + block_function hooks. Call from on_unreal_init.
        auto install() -> void;

        auto set_directory(std::filesystem::path directory) -> void { m_directory = std::move(directory); }
        [[nodiscard]] auto directory() const -> const std::filesystem::path& { return m_directory; }

        // ---- Trainer files (any thread) ----
        auto reload() -> void;
        // Re-evaluates every slider's bounds/default and live value from the game (placeholders such as {current} and
        // {class:X.Prop}). Called by the Refresh button; also runs once after a reload.
        auto refresh_values() -> void;
        // Test hook: adds an in-memory trainer (not saved) and returns its index.
        auto add_trainer_for_test(TrainerDef trainer) -> size_t;
        [[nodiscard]] auto trainers() const -> std::vector<TrainerDef>; // copy
        auto save(const TrainerDef& trainer, const std::string& file_stem) -> bool;
        [[nodiscard]] auto file_stem_of(size_t trainer_index) const -> std::string;

        // ---- Cheat control (any thread; work is forwarded to the game thread) ----
        auto set_enabled(size_t trainer_index, size_t cheat_index, bool enabled) -> void;
        auto set_value(size_t trainer_index, size_t cheat_index, double value) -> void;
        auto press(size_t trainer_index, size_t cheat_index) -> void;
        [[nodiscard]] auto state(size_t trainer_index, size_t cheat_index) const -> CheatState;
        auto disable_all() -> void;

        // ---- Ad-hoc testing for the creator ----
        // Applies a cheat definition that is not (yet) part of a saved trainer.
        auto test_cheat(const CheatDef& cheat, bool enable, double value) -> std::string;

        // ---- Synchronous variants for tests: must be called on the game thread ----
        auto enable_now(const CheatDef& cheat, double value, std::string& status) -> bool;
        auto disable_now(const CheatDef& cheat, std::string& status) -> void;
        // Game thread: does every target of the cheat exist right now? `reason` explains a "no".
        auto check_available(const CheatDef& cheat, std::string& reason) -> bool;

      private:
        TrainerEngine() = default;

        struct Availability
        {
            bool available{true};
            std::string reason{};
        };
        struct ActionRuntime
        {
            std::vector<Unreal::UObject*> objects{};
            std::unordered_map<Unreal::UObject*, double> base_numbers{};
            std::unordered_map<Unreal::UObject*, std::wstring> base_texts{};
            double next_refresh{0.0};
            // Per-tick fast path: properties are resolved when the objects are refreshed, the wanted value is parsed once.
            std::wstring property_w{};
            std::unordered_map<Unreal::UObject*, Unreal::FProperty*> props{};
            bool value_ready{false};
            double value_key{0.0};
            bool wanted_parsed{false};
            double wanted{0.0};
            std::string wanted_text{};
            double next_text_write{0.0};
        };
        struct CheatRuntime
        {
            bool enabled{false};
            double value{1.0};
            bool value_set{false}; // the user moved the slider (so 1.0 is a real choice, not "untouched")
            std::string status{};
            std::vector<ActionRuntime> actions{};
            CheatDef def{}; // copy of the definition while enabled, so ticking does not depend on the trainer list
        };
        struct BlockEntry
        {
            std::string cheat_key{};
            std::string function{};
            Unreal::FName function_name{}; // parsed once; the ProcessEvent hook must not build strings per call
            std::string target{};
            std::string select{"all"};
            int index{0};
            std::string return_value{};
            std::vector<Unreal::UObject*> objects{};
            double next_refresh{0.0};
        };

        static auto key_of(const TrainerDef& trainer, const CheatDef& cheat) -> std::string;
        auto runtime_for(const std::string& key, size_t action_count) -> CheatRuntime&;

        auto apply_enable(const CheatDef& cheat, CheatRuntime& rt, std::string& status) -> bool;
        auto apply_disable(const CheatDef& cheat, CheatRuntime& rt, std::string& status) -> void;
        auto apply_value_change(const CheatDef& cheat, CheatRuntime& rt) -> void;
        auto run_one_shot(const ActionDef& action, double value, bool revert, std::string& status) -> bool;
        auto tick(float dt) -> void;
        auto on_process_event(Unreal::Hook::TCallbackIterationData<void>& data, Unreal::UObject* context, Unreal::UFunction* function, void* params) -> void;
        auto refresh_objects(const ActionDef& action, ActionRuntime& art) -> void;
        auto remove_blocks(const std::string& cheat_key) -> void;
        auto refresh_availability() -> void;
        struct SliderView
        {
            double min{0.0};
            double max{100.0};
            double default_value{1.0};
            bool has_current{false};
            double current{0.0};
            std::string note{};
        };
        auto compute_view(const CheatDef& cheat) -> SliderView;
        auto refresh_values_locked() -> void; // caller holds m_mutex, game thread
        // Expands {value}, {current} and {<target>.<Property>} in `text`. `current_text` overrides {current} (used to
        // hold the value a property had when a cheat was switched on).
        auto expand_text(const std::string& text, double slider, const ActionDef* action, const std::optional<std::string>& current_text = std::nullopt) -> std::string;
        auto eval_number(const std::string& expression, double slider, const ActionDef* action, double fallback, bool* resolved, std::string* note) -> double;
        std::map<std::string, SliderView> m_views;
        bool m_views_dirty{true};

        std::filesystem::path m_directory;
        mutable std::recursive_mutex m_mutex;
        std::vector<TrainerDef> m_trainers;
        std::vector<std::string> m_stems;
        std::map<std::string, CheatRuntime> m_runtime;
        std::vector<BlockEntry> m_blocks;
        std::map<std::string, Availability> m_availability;
        struct AvailExpr
        {
            std::string target{};
            std::string select{};
            int index{0};
            int count{-1};
            double evaluated_at{-1e9};
        };
        std::map<std::string, AvailExpr> m_avail_exprs;
        bool m_avail_dirty{true};
        double m_next_availability{0.0};
        double m_clock{0.0};
        std::atomic<bool> m_has_blocks{false};
        bool m_installed{false};
    };
} // namespace RC::Toolkit
