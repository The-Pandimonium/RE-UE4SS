#include <EngineCompat/GameThread.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UEngine.hpp>

namespace RC::Compat
{
    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::deque<GameThread::Job> jobs;
            std::unordered_map<uint64_t, GameThread::TickCallback> ticks;
            uint64_t next_tick_id{1};
            std::atomic<std::thread::id> game_thread{};
            std::atomic<bool> installed{false};
            std::atomic<bool> running{false};
            std::atomic<uint64_t> tick_count{0};
        };

        auto state() -> State&
        {
            static State s;
            return s;
        }

        // Upper bound on time spent draining queued jobs in one tick so a flood of jobs cannot hitch the game.
        constexpr auto kDrainBudget = std::chrono::milliseconds{4};

        auto run_guarded(const GameThread::Job& job) -> void
        {
            try
            {
                job();
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Warning>(STR("[EngineCompat] Game-thread job threw: {}\n"), ensure_str(e.what()));
            }
            catch (...)
            {
                Output::send<LogLevel::Warning>(STR("[EngineCompat] Game-thread job threw an unknown exception\n"));
            }
        }

        auto on_tick(float dt) -> void
        {
            auto& s = state();
            s.game_thread.store(std::this_thread::get_id());
            s.running.store(true);
            s.tick_count.fetch_add(1, std::memory_order_relaxed);

            const auto start = std::chrono::steady_clock::now();
            for (;;)
            {
                GameThread::Job job;
                {
                    std::lock_guard lock(s.mutex);
                    if (s.jobs.empty()) break;
                    job = std::move(s.jobs.front());
                    s.jobs.pop_front();
                }
                run_guarded(job);
                if (std::chrono::steady_clock::now() - start > kDrainBudget) break;
            }

            std::vector<GameThread::TickCallback> callbacks;
            {
                std::lock_guard lock(s.mutex);
                callbacks.reserve(s.ticks.size());
                for (auto& [id, cb] : s.ticks) callbacks.push_back(cb);
            }
            for (auto& cb : callbacks) run_guarded([&] { cb(dt); });
        }
    } // namespace

    auto GameThread::Install() -> void
    {
        auto& s = state();
        if (s.installed.exchange(true)) return;
        Unreal::Hook::RegisterEngineTickPreCallback(
                [](auto&, Unreal::UEngine*, float dt, bool) {
                    on_tick(dt);
                },
                Unreal::Hook::FCallbackOptions{.OwnerModName = STR("EngineCompat"), .HookName = STR("GameThread")});
    }

    auto GameThread::IsInstalled() -> bool
    {
        return state().installed.load();
    }

    auto GameThread::IsRunning() -> bool
    {
        return state().running.load();
    }

    auto GameThread::IsGameThread() -> bool
    {
        return state().game_thread.load() == std::this_thread::get_id();
    }

    auto GameThread::Post(Job job) -> void
    {
        if (IsGameThread())
        {
            run_guarded(job);
            return;
        }
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.jobs.push_back(std::move(job));
    }

    auto GameThread::Run(Job job, std::chrono::milliseconds timeout) -> bool
    {
        if (IsGameThread())
        {
            run_guarded(job);
            return true;
        }
        struct Sync
        {
            std::mutex m;
            std::condition_variable cv;
            bool done{false};
        };
        auto sync = std::make_shared<Sync>();
        Post([sync, job = std::move(job)] {
            run_guarded(job);
            std::lock_guard lock(sync->m);
            sync->done = true;
            sync->cv.notify_all();
        });
        std::unique_lock lock(sync->m);
        return sync->cv.wait_for(lock, timeout, [&] { return sync->done; });
    }

    auto GameThread::AddTick(TickCallback callback) -> uint64_t
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        const uint64_t id = s.next_tick_id++;
        s.ticks.emplace(id, std::move(callback));
        return id;
    }

    auto GameThread::RemoveTick(uint64_t id) -> void
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.ticks.erase(id);
    }

    auto GameThread::TickCount() -> uint64_t
    {
        return state().tick_count.load();
    }
} // namespace RC::Compat
