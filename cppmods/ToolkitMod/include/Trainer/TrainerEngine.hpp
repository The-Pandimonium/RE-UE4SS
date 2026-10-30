#pragma once

#include <atomic>
#include <filesystem>
#include <map>
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
    };

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

      private:
        TrainerEngine() = default;

        struct ActionRuntime
        {
            std::vector<Unreal::UObject*> objects{};
            std::unordered_map<Unreal::UObject*, double> base_numbers{};
            std::unordered_map<Unreal::UObject*, std::wstring> base_texts{};
            double next_refresh{0.0};
        };
        struct CheatRuntime
        {
            bool enabled{false};
            double value{1.0};
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

        std::filesystem::path m_directory;
        mutable std::recursive_mutex m_mutex;
        std::vector<TrainerDef> m_trainers;
        std::vector<std::string> m_stems;
        std::map<std::string, CheatRuntime> m_runtime;
        std::vector<BlockEntry> m_blocks;
        double m_clock{0.0};
        std::atomic<bool> m_has_blocks{false};
        bool m_installed{false};
    };
} // namespace RC::Toolkit
