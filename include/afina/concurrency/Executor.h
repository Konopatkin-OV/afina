#ifndef AFINA_CONCURRENCY_EXECUTOR_H
#define AFINA_CONCURRENCY_EXECUTOR_H

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// for debugging
#include <iostream>
#include <unistd.h>

namespace Afina {
namespace Concurrency {

/**
 * # Thread pool
 */
class Executor {
    enum class State {
        // Threadpool is fully operational, tasks could be added and get executed
        kRun,

        // Threadpool is on the way to be shutdown, no ned task could be added, but existing will be
        // completed as requested
        kStopping,

        // Threadppol is stopped
        kStopped
    };

public:
    Executor(int th_min, int th_max, int q_max, int wait_max)
        : low_watermark(th_min), high_watermark(th_max), max_queue_size(q_max), idle_time(wait_max) {}
    ~Executor() { this->Stop(true); }

    /**
     * Signal thread pool to stop, it will stop accepting new jobs and close threads just after each become
     * free. All enqueued jobs will be complete.
     *
     * In case if await flag is true, call won't return until all background jobs are done and all threads are stopped
     */
    void Stop(bool await = false);

    /**
     * Initialize worker threads
     */
    void Start();

    /**
     * Add function to be executed on the threadpool. Method returns true in case if task has been placed
     * onto execution queue, i.e scheduled for execution and false otherwise.
     *
     * That function doesn't wait for function result. Function could always be written in a way to notify caller about
     * execution finished by itself
     */
    template <typename F, typename... Types> bool Execute(F &&func, Types... args) {
        // Prepare "task"
        auto exec = std::bind(std::forward<F>(func), std::forward<Types>(args)...);

        {
            std::unique_lock<std::mutex> lock(this->state_mutex);
            if (state != State::kRun) {
                return false;
            }

            if (tasks.size() >= max_queue_size) {
                return false;
            }

            try_create_worker();

            // Enqueue new task
            tasks.push_back(exec);
        }
        empty_condition.notify_one();
        return true;
    }

private:
    // No copy/move/assign allowed
    Executor(const Executor &);            // = delete;
    Executor(Executor &&);                 // = delete;
    Executor &operator=(const Executor &); // = delete;
    Executor &operator=(Executor &&);      // = delete;

    bool check_running();

    bool check_tasks_left();

    /**
     * Try to create new worker if high_watermark is not reached yet
     * Does not lock mutex and must be called inside unique_lock block
     */
    void try_create_worker();

    /**
     * Main function that all pool threads are running. It polls internal task queue and execute tasks
     */
    friend void perform(Executor *executor);

    /**
     * Mutex to protect state below from concurrent modification
     */
    std::mutex state_mutex;

    /**
     * Conditional variable to await new data in case of empty queue
     */
    std::condition_variable empty_condition;

    /**
     * Conditional variable to await all workers finish their tasks
     */
    std::condition_variable finish_condition;

    /**
     * Task queue
     */
    std::deque<std::function<void()>> tasks;

    // thread pool parameters
    int low_watermark;
    int high_watermark;
    int max_queue_size;
    std::chrono::milliseconds idle_time;

    // thread pool state variables
    int live_workers;
    int free_workers;

    /**
     * Flag to stop bg threads
     */
    State state;
};

} // namespace Concurrency
} // namespace Afina

#endif // AFINA_CONCURRENCY_EXECUTOR_H
