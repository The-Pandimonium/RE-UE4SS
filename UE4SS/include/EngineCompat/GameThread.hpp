#pragma once

// EngineCompat::GameThread - run work on the Unreal game thread.
//
// UE4SS mod callbacks and the ImGui window run on their own threads. Anything that executes game code
// (ProcessEvent, writing gameplay properties) must run on the game thread, so mods post work here and it
// is drained once per engine tick.

#include <chrono>
#include <cstdint>
#include <functional>

#include <Common.hpp>

namespace RC::Compat
{
    class RC_UE4SS_API GameThread
    {
      public:
        using Job = std::function<void()>;
        using TickCallback = std::function<void(float delta_seconds)>;

        // Idempotent. Call once Unreal is initialised (e.g. from CppUserModBase::on_unreal_init).
        static auto Install() -> void;
        [[nodiscard]] static auto IsInstalled() -> bool;
        // True once the first engine tick has run (jobs are being drained).
        [[nodiscard]] static auto IsRunning() -> bool;
        [[nodiscard]] static auto IsGameThread() -> bool;

        // Queue a job for the next tick. Runs immediately if already on the game thread.
        static auto Post(Job job) -> void;
        // Queue a job and block until it ran or the timeout elapsed. Returns true when the job completed.
        static auto Run(Job job, std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) -> bool;

        // Called every engine tick on the game thread. Returns an id for RemoveTick.
        static auto AddTick(TickCallback callback) -> uint64_t;
        static auto RemoveTick(uint64_t id) -> void;
        // Engine ticks seen since Install (cheap liveness / timing signal for tests).
        [[nodiscard]] static auto TickCount() -> uint64_t;
    };
} // namespace RC::Compat
