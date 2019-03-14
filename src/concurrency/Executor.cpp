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
        joiner_thread.join();
    }
}

void Executor::joiner() {
    std::cerr << "Joiner: successfully launched!" << std::endl;
    while (check_running()) {
        std::unique_lock<std::mutex> _lock(state_mutex);

        std::cerr << "Joiner: waiting for a prey!" << std::endl;
        while (unjoined_workers == live_workers) {
            joinable_condition.wait(_lock);
        }
        std::cerr << "Joiner: awakened!" << std::endl;

        for (int i = 0; i < high_watermark; ++i) {
            if (threads_finished[i]) {
                std::cerr << "Joiner: joining thread: " << i << std::endl;
                threads[i].join();
                unjoined_workers -= 1;
                threads_finished[i] = false;
                std::cerr << "Joiner: successfully joined!" << std::endl;
            }
        }
    }

    std::cerr << "Joiner: thread pool is stopping" << std::endl;
    for (int i = 0; i < high_watermark; ++i) {
        if (threads[i].joinable()) {
            std::cerr << "Joiner: joining thread: " << i << std::endl;
            threads[i].join();
            std::cerr << "Joiner: successfully joined!" << std::endl;
        }
    }

    sleep(2);

    {
        std::unique_lock<std::mutex> _lock(state_mutex);
        state = State::kStopped;
        std::cerr << "Joiner: successfully stopped thread pool!" << std::endl;
    }
}

void Executor::try_create_worker() {
    std::cerr << "Execute: trying to create new thread" << std::endl;
    if (free_workers == 0 && unjoined_workers < high_watermark) {
        std::cerr << "Execute: creating new worker" << std::endl;
        for (int i = 0; i < high_watermark; ++i) {
            if (!threads[i].joinable()) {
                live_workers += 1;
                free_workers += 1;
                unjoined_workers += 1;

                threads_finished[i] = false;
                threads[i] = std::thread(perform, this, i);
                std::cerr << "Execute: new worker successfully created" << std::endl;
                return;
            }
        }
        std::cerr << "Execute: failed to create new worker (THIS SHOULD NOT HAPPEN!!!)" << std::endl;
        return;
    }

    if (free_workers == 0) {
        std::cerr << "Execute: thread limit exceeded" << std::endl;
    } else {
        std::cerr << "Execute: new thread is unnecessary" << std::endl;
    }
    
}



void perform(Executor *executor, int thread_num) {
    {
        std::unique_lock<std::mutex> _lock(executor->state_mutex);
        std::cerr << "Started new thread: " << thread_num << std::endl;
    }

    std::chrono::time_point<std::chrono::high_resolution_clock> wait_limit;

    while (executor->check_running() || executor->check_tasks_left()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> _lock(executor->state_mutex);

            wait_limit = std::chrono::high_resolution_clock::now() + executor->idle_time;

            // wait for new task
            std::cerr << "Waiting for a task: " << thread_num << std::endl;
            while (executor->tasks.empty() && std::chrono::high_resolution_clock::now() < wait_limit) {
                // if thread pool is stopping
                if (executor->state != Executor::State::kRun) {
                    std::cerr << "Woken because of stop (idle check): " << thread_num << std::endl;
                    break;
                }
                executor->empty_condition.wait_until(_lock, wait_limit);
            }
            std::cerr << "Waiting for a task complete: " << thread_num << std::endl;

            if (executor->state == Executor::State::kRun) {
                // if woken up because of idleness
                if (executor->tasks.empty() && std::chrono::high_resolution_clock::now() >= wait_limit) {
                    if (executor->live_workers > executor->low_watermark) {
                        std::cerr << "Can die: " << thread_num << std::endl;
                        break;
                    } else {
                        std::cerr << "Waiting for a task (idle must): " << thread_num << std::endl;
                        while (executor->tasks.empty()) {
                            // if thread pool is stopping
                            if (executor->state != Executor::State::kRun) {
                                std::cerr << "Woken because of stop (idle must): " << thread_num << std::endl;
                                break;
                            }
                            executor->empty_condition.wait_until(_lock, wait_limit);
                        }
                    }
                }
            } 
            if (executor->state != Executor::State::kRun) {
                std::cerr << "Thread pool is being stopped: " << thread_num << std::endl;
                if (executor->tasks.empty()) {
                    std::cerr << "No tasks left: " << thread_num << std::endl;
                    executor->empty_condition.notify_one();
                    break;
                }
                std::cerr << "There are tasks left: " << thread_num << std::endl;
            }

            executor->free_workers -= 1;

            task = executor->tasks.front();
            executor->tasks.pop_front();

            std::cerr << "Got a task: " << thread_num << std::endl;
        }
        task();

        sleep(2.5);
        std::cerr << "Task finished: " << thread_num << std::endl;

        executor->free_workers += 1;
    }

    {
        std::unique_lock<std::mutex> _lock(executor->state_mutex);
        executor->free_workers -= 1;
        executor->live_workers -= 1;
        executor->threads_finished[thread_num] = true;
        executor->joinable_condition.notify_one();
        std::cerr << "Kill thread: " << thread_num << std::endl;
    }
}

} // namespace Concurrency
} // namespace Afina
