#include <afina/concurrency/Executor.h>

namespace Afina {
namespace Concurrency {

void perform(Executor *executor);

bool Executor::check_running() {
    std::unique_lock<std::mutex> _lock(state_mutex);
    return state == State::kRun;
}

bool Executor::check_tasks_left() {
    std::unique_lock<std::mutex> _lock(state_mutex);
    return !tasks.empty();
}

void Executor::Start() {
    std::unique_lock<std::mutex> _lock(state_mutex);

    live_workers = low_watermark;
    free_workers = low_watermark;

    state = State::kRun;
    for (int i = 0; i < low_watermark; ++i) {
        auto worker = std::thread(perform, this);
        worker.detach();
    }
}

void Executor::Stop(bool await) {
    {
        std::unique_lock<std::mutex> _lock(state_mutex);
        if (state == State::kRun) {
            state = State::kStopping;
            empty_condition.notify_one();
        }
    }

    if (await) {
        std::unique_lock<std::mutex> _lock(state_mutex);
        while (live_workers > 0) {
            finish_condition.wait(_lock);
        }
    }
}

void Executor::try_create_worker() {
    if (free_workers == 0 && live_workers < high_watermark) {
        live_workers += 1;
        free_workers += 1;

        auto worker = std::thread(perform, this);
        worker.detach();
    }
}

void perform(Executor *executor) {
    std::chrono::time_point<std::chrono::high_resolution_clock> wait_limit;

    while (executor->check_running() || executor->check_tasks_left()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> _lock(executor->state_mutex);

            wait_limit = std::chrono::high_resolution_clock::now() + executor->idle_time;

            // wait for new task
            while (executor->tasks.empty() && std::chrono::high_resolution_clock::now() < wait_limit) {
                // if thread pool is stopping
                if (executor->state != Executor::State::kRun) {
                    break;
                }
                executor->empty_condition.wait_until(_lock, wait_limit);
            }

            if (executor->state == Executor::State::kRun) {
                // if woken up because of idleness
                if (executor->tasks.empty() && std::chrono::high_resolution_clock::now() >= wait_limit) {
                    if (executor->live_workers > executor->low_watermark) {
                        break;
                    } else {
                        while (executor->tasks.empty()) {
                            // if thread pool is stopping
                            if (executor->state != Executor::State::kRun) {
                                break;
                            }
                            executor->empty_condition.wait_until(_lock, wait_limit);
                        }
                    }
                }
            }
            if (executor->state != Executor::State::kRun) {
                if (executor->tasks.empty()) {
                    // notify next waiting worker
                    executor->empty_condition.notify_one();
                    break;
                }
            }

            executor->free_workers -= 1;

            task = executor->tasks.front();
            executor->tasks.pop_front();
        }
        task();

        executor->free_workers += 1;
    }

    {
        std::unique_lock<std::mutex> _lock(executor->state_mutex);
        executor->free_workers -= 1;
        executor->live_workers -= 1;
        if (executor->live_workers == 0) {
            executor->finish_condition.notify_one();
        }
    }
}

} // namespace Concurrency
} // namespace Afina
