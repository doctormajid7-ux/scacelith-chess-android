// Small persistent thread pool for the TTS runtime: the calling thread plus (threads - 1) helpers
// that sleep between jobs. Helpers run at below-normal priority with flush-to-zero /
// denormals-are-zero set (MXCSR is per thread); the caller sets its own (see FpGuard).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tts {

// Lowers the priority of the calling thread below normal (Windows THREAD_PRIORITY_BELOW_NORMAL,
// Linux nice +5 for this thread only).
void lowerThreadPriority();

// Flush-to-zero / denormals-are-zero for the current thread, restored on destruction.
// The saved value is the x86 MXCSR on x86 (32 bits used), the FPCR on aarch64.
struct FpGuard {
    uint64_t csr;
    FpGuard();
    ~FpGuard();
    FpGuard(const FpGuard&) = delete;
    FpGuard& operator=(const FpGuard&) = delete;
};

class ThreadPool {
public:
    explicit ThreadPool(int threads);   // total, including the caller of run(); clamped to 1..16
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int size() const { return int(workers_.size()) + 1; }
    // Calls fn(i) for every i in [0, n), spread over the threads (the caller takes part);
    // returns when all calls are done. Not re-entrant; one caller at a time. An exception thrown
    // by fn (std::bad_alloc) is rethrown here, on the caller, once no helper runs the job.
    void run(int n, const std::function<void(int)>& fn);

private:
    void workerMain();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(int)>* job_ = nullptr;
    int jobSize_ = 0;
    std::atomic<int> next_{0};
    int active_ = 0;          // helpers inside the current job
    uint64_t generation_ = 0;
    bool quit_ = false;
    std::exception_ptr error_;   // the first exception of a helper in the current job
};

}  // namespace tts
