#include <afina/concurrency/Executor.h>

namespace Afina {
namespace Concurrency {

void perform(Executor *executor, int thread_num);

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
    unjoined_workers = low_watermark;

    state = State::kRun;
    for (int i = 0; i < low_watermark; ++i) {
        threads_finished[i] = false;
        threads[i] = std::thread(perform, this, i);
    }

    joiner_thread = std::thread(&Executor::joiner, this);
}

void Executor::Stop(bool await) {
    {
        std::unique_lock<std::mutex> _lock(state_mutex);
        if (state == State::kRun) {
            state = State::kStopping;
        }
    }

    empty_condition.notify_one();

    if (await) {
        if (joiner_thread.joinable()) {
            joiner_thread.join();
        }
    }
}

void Executor::joiner() {
    while (check_running()) {
        std::unique_lock<std::mutex> _lock(state_mutex);

        while (unjoined_workers == live_workers) {
            joinable_condition.wait(_lock);
        }

        for (int i = 0; i < high_watermark; ++i) {
            if (threads_finished[i]) {
                threads[i].join();
                unjoined_workers -= 1;
                threads_finished[i] = false;
            }
        }
    }

    for (int i = 0; i < high_watermark; ++i) {
        if (threads[i].joinable()) {
            threads[i].join();
        }
    }

    {
        std::unique_lock<std::mutex> _lock(state_mutex);
        state = State::kStopped;
    }
}

void Executor::try_create_worker() {
    if (free_workers == 0 && unjoined_workers < high_watermark) {
        for (int i = 0; i < high_watermark; ++i) {
            if (!threads[i].joinable()) {
                live_workers += 1;
                free_workers += 1;
                unjoined_workers += 1;

                threads_finished[i] = false;
                threads[i] = std::thread(perform, this, i);
                return;
            }
        }
    }
}

void perform(Executor *executor, int thread_num) {
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
        executor->threads_finished[thread_num] = true;
        executor->joinable_condition.notify_one();
    }
}

} // namespace Concurrency
} // namespace Afina
