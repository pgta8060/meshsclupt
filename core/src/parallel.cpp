#include "sculpt/parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace sculpt {
namespace {

thread_local bool tInsideParallel = false;

class Pool {
public:
    static Pool& get() {
        // Leaked on purpose: joining threads from a static destructor can
        // deadlock inside a DLL being unloaded. Hosts call shutdownParallel().
        static Pool* pool = new Pool();
        return *pool;
    }

    void setThreads(unsigned threads) {
        std::lock_guard<std::mutex> lock(configMutex_);
        stop();
        desired_ = std::max(1u, std::min(threads, 64u));
    }

    unsigned threads() {
        std::lock_guard<std::mutex> lock(configMutex_);
        return desired_;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(configMutex_);
        stop();
    }

    void run(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& fn) {
        std::unique_lock<std::mutex> config(configMutex_);  // One parallel loop at a time.
        if (desired_ <= 1) {
            config.unlock();
            fn(0, count);
            return;
        }
        start();

        const std::size_t chunks = (count + grain - 1) / grain;
        Job job{&fn, count, grain, chunks};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            ++generation_;
        }
        wake_.notify_all();
        work(job);  // The caller helps.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Also wait for every worker to leave the job: one may still hold a
            // pointer to it even though all chunks are finished.
            done_.wait(lock, [&] { return job.finished.load() == job.chunks && active_ == 0; });
            job_ = nullptr;
        }
    }

private:
    struct Job {
        const std::function<void(std::size_t, std::size_t)>* fn;
        std::size_t count;
        std::size_t grain;
        std::size_t chunks;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> finished{0};
    };

    Pool() {
        const unsigned hw = std::thread::hardware_concurrency();
        desired_ = std::max(1u, std::min(hw == 0 ? 1u : hw, 16u));
    }

    void work(Job& job) {
        tInsideParallel = true;
        std::size_t c;
        while ((c = job.next.fetch_add(1)) < job.chunks) {
            const std::size_t begin = c * job.grain;
            const std::size_t end = std::min(job.count, begin + job.grain);
            (*job.fn)(begin, end);
            if (job.finished.fetch_add(1) + 1 == job.chunks) {
                std::lock_guard<std::mutex> lock(mutex_);
                done_.notify_all();
            }
        }
        tInsideParallel = false;
    }

    void workerLoop() {
        std::uint64_t seen = 0;
        for (;;) {
            Job* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || (job_ && generation_ != seen); });
                if (quit_) return;
                seen = generation_;
                job = job_;
                ++active_;
            }
            work(*job);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_;
            }
            done_.notify_all();
        }
    }

    void start() {  // configMutex_ held.
        if (!workers_.empty()) return;
        quit_ = false;
        for (unsigned i = 1; i < desired_; ++i) workers_.emplace_back([this] { workerLoop(); });
    }

    void stop() {  // configMutex_ held.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : workers_) t.join();
        workers_.clear();
    }

    std::mutex configMutex_;
    unsigned desired_ = 1;
    std::vector<std::thread> workers_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    Job* job_ = nullptr;
    unsigned active_ = 0;  // Workers currently inside work().
    std::uint64_t generation_ = 0;
    bool quit_ = false;
};

}  // namespace

void parallelFor(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    if (tInsideParallel || count < 2 * grain) {
        fn(0, count);
        return;
    }
    Pool::get().run(count, grain, fn);
}

void setParallelThreadCount(unsigned threads) { Pool::get().setThreads(threads); }

unsigned parallelThreadCount() { return Pool::get().threads(); }

void shutdownParallel() { Pool::get().shutdown(); }

}  // namespace sculpt
