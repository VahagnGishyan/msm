// MSM concurrency benchmark.
//
// Three sections:
//   1. Latency CDF — single-threaded, 100K iterations per op; reports
//      p50/p90/p95/p99/p99.9 from sorted samples.
//   2. Scalability — 1 writer + N readers (N = 1, 2, 4, 8, 16); reports
//      aggregate read throughput (ops/sec).
//   3. Comparison — the same workload over std::mutex, std::shared_mutex,
//      pthread_rwlock, and std::atomic for reference.
//
// Output is CSV on stdout, suitable for plotting or tabulation.
//
// Build the cmake target msm-concurrency-bench (Release is recommended for
// clean numbers; under Debug+TSAN the values are dominated by instrumentation).
//
// Usage:
//   ./msm-concurrency-bench                # all sections (default)
//   ./msm-concurrency-bench latency        # latency CDF only
//   ./msm-concurrency-bench scalability    # scalability curve only
//   ./msm-concurrency-bench comparison     # primitive comparison only

#include "msm_allocator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

namespace
{

// --- Percentile helper (sorted samples) ---
double percentile(std::vector<double>& samples, double p)
{
    if (samples.empty()) return 0.0;
    if (!std::is_sorted(samples.begin(), samples.end()))
        std::sort(samples.begin(), samples.end());
    double idx = (samples.size() - 1) * p;
    std::size_t lo = static_cast<std::size_t>(idx);
    std::size_t hi = std::min(lo + 1, samples.size() - 1);
    double frac = idx - lo;
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
}

// --- Latency measurement: single op repeated N times, sampled per op ---
template<typename OpFn>
void measure_latency(const char* op_name, OpFn op, std::size_t iterations)
{
    std::vector<double> samples;
    samples.reserve(iterations);

    // Warmup
    for (std::size_t i = 0; i < std::min<std::size_t>(1000, iterations); ++i) op();

    for (std::size_t i = 0; i < iterations; ++i)
    {
        auto t0 = clk::now();
        op();
        auto t1 = clk::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns);
    }

    std::sort(samples.begin(), samples.end());

    double p50  = percentile(samples, 0.50);
    double p90  = percentile(samples, 0.90);
    double p95  = percentile(samples, 0.95);
    double p99  = percentile(samples, 0.99);
    double p999 = percentile(samples, 0.999);
    double min_v = samples.front();
    double max_v = samples.back();

    // CSV row: op, iters, min, p50, p90, p95, p99, p99.9, max (ns)
    std::printf("%s,%zu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
                op_name, iterations, min_v, p50, p90, p95, p99, p999, max_v);
}

// Section 1: Latency CDF.
void run_latency_benchmarks()
{
    std::printf("# Section 1: Latency CDF (single-threaded, time per op, nanoseconds)\n");
    std::printf("operation,iterations,min,p50,p90,p95,p99,p99.9,max\n");

    constexpr std::size_t N = 100000;

    // --- Array push_back (prepare + commit) ---
    {
        auto* slot = msm_alloc("bench_array_push", 24, nullptr);
        std::memset(slot, 0, 24);
        measure_latency("array_push_back_int32", [&]() {
            auto* elem = msm_array_prepare_push(slot, 8);
            if (elem) {
                std::int32_t v = 42;
                std::memcpy(elem + 4, &v, 4);
                msm_array_commit_push(slot);
            }
        }, N);
        msm_free("bench_array_push");
    }

    // --- Array random read ---
    {
        auto* slot = msm_alloc("bench_array_read", 24, nullptr);
        std::memset(slot, 0, 24);
        for (std::uint32_t i = 0; i < 10000; ++i) {
            auto* e = msm_array_prepare_push(slot, 8);
            std::int32_t v = static_cast<std::int32_t>(i);
            std::memcpy(e + 4, &v, 4);
            msm_array_commit_push(slot);
        }
        std::uint32_t idx = 0;
        measure_latency("array_read_int32", [&]() {
            auto* e = msm_array_get(slot, idx % 10000, 8);
            if (e) {
                volatile std::int32_t v;
                std::memcpy(const_cast<std::int32_t*>(&v), e + 4, 4);
                (void)v;
            }
            ++idx;
        }, N);
        msm_free("bench_array_read");
    }

    // --- String set (always copies) ---
    {
        auto* slot = msm_alloc("bench_string_set", 24, nullptr);
        std::memset(slot, 0, 24);
        const char fixed_str[] = "Hello, MSM benchmark!";
        std::uint32_t len = static_cast<std::uint32_t>(std::strlen(fixed_str));
        measure_latency("string_set_const_len", [&]() {
            msm_string_set(slot, fixed_str, len);
        }, N);
        msm_free("bench_string_set");
    }

    // --- String get ---
    {
        auto* slot = msm_alloc("bench_string_get", 24, nullptr);
        std::memset(slot, 0, 24);
        msm_string_set(slot, "Hello, MSM benchmark!", 21);
        measure_latency("string_get", [&]() {
            const char* s = msm_string_get(slot);
            volatile char c = s[0];
            (void)c;
        }, N);
        msm_free("bench_string_get");
    }

    // --- Object set int32 (existing field — in-place value write) ---
    {
        auto* slot = msm_alloc("bench_object_set", 24, nullptr);
        std::memset(slot, 0, 24);
        msm_object_set_int32(slot, "k", 0);
        measure_latency("object_set_int32_existing", [&]() {
            msm_object_set_int32(slot, "k", 1);
        }, N);
        msm_free("bench_object_set");
    }

    // --- Object get int32 ---
    {
        auto* slot = msm_alloc("bench_object_get", 24, nullptr);
        std::memset(slot, 0, 24);
        msm_object_set_int32(slot, "k", 42);
        measure_latency("object_get_int32", [&]() {
            volatile std::int32_t v = msm_object_get_int32(slot, "k");
            (void)v;
        }, N);
        msm_free("bench_object_get");
    }

    // --- Object add new field (CoW on grow) ---
    {
        // Restart object for each batch — we want to measure add cost
        constexpr std::size_t M = 1000;
        std::vector<double> samples;
        samples.reserve(M);
        for (int iter = 0; iter < 10; ++iter) {
            auto* slot = msm_alloc(
                ("bench_object_add_" + std::to_string(iter)).c_str(), 24, nullptr);
            std::memset(slot, 0, 24);
            for (std::size_t i = 0; i < M; ++i) {
                std::string name = "field_" + std::to_string(i);
                auto t0 = clk::now();
                msm_object_set_int32(slot, name.c_str(), static_cast<std::int32_t>(i));
                auto t1 = clk::now();
                samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
            msm_free(("bench_object_add_" + std::to_string(iter)).c_str());
        }
        std::sort(samples.begin(), samples.end());
        std::printf("%s,%zu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
                    "object_add_new_field", samples.size(),
                    samples.front(),
                    percentile(samples, 0.50),
                    percentile(samples, 0.90),
                    percentile(samples, 0.95),
                    percentile(samples, 0.99),
                    percentile(samples, 0.999),
                    samples.back());
    }

    // --- Object remove (CoW) ---
    {
        constexpr std::size_t M = 200; // fewer because each remove is O(N)
        std::vector<double> samples;
        for (int iter = 0; iter < 10; ++iter) {
            auto* slot = msm_alloc(
                ("bench_object_remove_" + std::to_string(iter)).c_str(), 24, nullptr);
            std::memset(slot, 0, 24);
            // Pre-populate
            for (std::size_t i = 0; i < M; ++i) {
                std::string name = "f" + std::to_string(i);
                msm_object_set_int32(slot, name.c_str(), static_cast<std::int32_t>(i));
            }
            // Measure removes
            for (std::size_t i = 0; i < M; ++i) {
                std::string name = "f" + std::to_string(i);
                auto t0 = clk::now();
                msm_object_remove(slot, name.c_str());
                auto t1 = clk::now();
                samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
            msm_free(("bench_object_remove_" + std::to_string(iter)).c_str());
        }
        std::sort(samples.begin(), samples.end());
        std::printf("%s,%zu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
                    "object_remove_cow", samples.size(),
                    samples.front(),
                    percentile(samples, 0.50),
                    percentile(samples, 0.90),
                    percentile(samples, 0.95),
                    percentile(samples, 0.99),
                    percentile(samples, 0.999),
                    samples.back());
    }
}

// Section 2: Scalability curve.
//
// One writer thread continuously updates an int32 in shared memory while N
// reader threads continuously read it; we report aggregate read throughput
// (ops/sec) across all readers. This isolates the lock-free SWMR scaling
// characteristics.
void run_scalability_benchmark()
{
    std::printf("# Section 2: Scalability — 1 writer + N readers, aggregate reads/sec\n");
    std::printf("readers,duration_s,total_reads,reads_per_sec,reads_per_reader_per_sec\n");

    const int reader_counts[] = {1, 2, 4, 8, 16};
    constexpr int DURATION_SECONDS = 2;

    for (int n : reader_counts)
    {
        auto* slot = msm_alloc("scale_test", 24, nullptr);
        std::memset(slot, 0, 24);
        // Initialize as object with a single int32 field "v"
        msm_object_set_int32(slot, "v", 0);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> total_reads{0};

        auto writer = [&]() {
            std::int32_t i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                msm_object_set_int32(slot, "v", ++i);
            }
        };

        auto reader = [&]() {
            std::uint64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                volatile std::int32_t v = msm_object_get_int32(slot, "v");
                (void)v;
                ++local;
            }
            total_reads.fetch_add(local, std::memory_order_relaxed);
        };

        std::thread w(writer);
        std::vector<std::thread> readers;
        for (int i = 0; i < n; ++i) readers.emplace_back(reader);

        auto t0 = clk::now();
        std::this_thread::sleep_for(std::chrono::seconds(DURATION_SECONDS));
        stop.store(true, std::memory_order_relaxed);
        w.join();
        for (auto& r : readers) r.join();
        auto t1 = clk::now();

        double seconds = std::chrono::duration<double>(t1 - t0).count();
        std::uint64_t reads = total_reads.load();
        double rps = reads / seconds;
        double rps_per_reader = rps / n;

        std::printf("%d,%.2f,%llu,%.0f,%.0f\n",
                    n, seconds, static_cast<unsigned long long>(reads),
                    rps, rps_per_reader);

        msm_free("scale_test");
    }
}

// Section 3: Comparison — lock-free MSM vs std::mutex, std::shared_mutex,
// pthread_rwlock, and std::atomic.
//
// Same workload as Section 2 (1 writer + N readers, int update + read) but
// implemented over a plain int with various synchronization primitives, so the
// lock-free path can be contrasted against the common blocking alternatives.
// Output is ops/sec per primitive, per reader count.

template<typename Locked>
double run_locked_scalability(int n_readers, int duration_s, Locked& locked)
{
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total_reads{0};

    auto writer = [&]() {
        std::int32_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            locked.write(++i);
        }
    };

    auto reader = [&]() {
        std::uint64_t local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            volatile std::int32_t v = locked.read();
            (void)v;
            ++local;
        }
        total_reads.fetch_add(local, std::memory_order_relaxed);
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < n_readers; ++i) readers.emplace_back(reader);

    auto t0 = clk::now();
    std::this_thread::sleep_for(std::chrono::seconds(duration_s));
    stop.store(true, std::memory_order_relaxed);
    w.join();
    for (auto& r : readers) r.join();
    auto t1 = clk::now();

    double seconds = std::chrono::duration<double>(t1 - t0).count();
    std::uint64_t reads = total_reads.load();
    return reads / seconds;
}

struct MutexLocked
{
    mutable std::mutex m;
    std::int32_t v = 0;
    void write(std::int32_t x) { std::lock_guard<std::mutex> g(m); v = x; }
    std::int32_t read() const { std::lock_guard<std::mutex> g(m); return v; }
};

struct SharedMutexLocked
{
    mutable std::shared_mutex m;
    std::int32_t v = 0;
    void write(std::int32_t x) { std::unique_lock<std::shared_mutex> g(m); v = x; }
    std::int32_t read() const { std::shared_lock<std::shared_mutex> g(m); return v; }
};

struct PthreadRwlockLocked
{
    mutable pthread_rwlock_t rw;
    std::int32_t v = 0;
    PthreadRwlockLocked() { pthread_rwlock_init(&rw, nullptr); }
    ~PthreadRwlockLocked() { pthread_rwlock_destroy(&rw); }
    void write(std::int32_t x) {
        pthread_rwlock_wrlock(&rw); v = x; pthread_rwlock_unlock(&rw);
    }
    std::int32_t read() const {
        pthread_rwlock_rdlock(&rw); std::int32_t r = v; pthread_rwlock_unlock(&rw);
        return r;
    }
};

struct AtomicLocked
{
    mutable std::atomic<std::int32_t> v{0};
    void write(std::int32_t x) { v.store(x, std::memory_order_release); }
    std::int32_t read() const { return v.load(std::memory_order_acquire); }
};

void run_comparison_benchmark()
{
    std::printf("# Section 3: MSM vs std::mutex vs std::shared_mutex vs pthread_rwlock vs std::atomic<int32_t>\n");
    std::printf("# 1 writer + N readers, aggregate reads/sec for shared int32 update/read.\n");
    std::printf("primitive,readers,reads_per_sec\n");

    constexpr int DURATION = 2;
    const int reader_counts[] = {1, 2, 4, 8, 16};

    for (int n : reader_counts)
    {
        // MSM (object_set/get_int32 — full lock-free SWMR through msm protocol)
        {
            auto* slot = msm_alloc("cmp_msm", 24, nullptr);
            std::memset(slot, 0, 24);
            msm_object_set_int32(slot, "v", 0);

            std::atomic<bool> stop{false};
            std::atomic<std::uint64_t> total_reads{0};

            auto writer = [&]() {
                std::int32_t i = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    msm_object_set_int32(slot, "v", ++i);
                }
            };
            auto reader = [&]() {
                std::uint64_t local = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    volatile std::int32_t v = msm_object_get_int32(slot, "v");
                    (void)v;
                    ++local;
                }
                total_reads.fetch_add(local, std::memory_order_relaxed);
            };

            std::thread w(writer);
            std::vector<std::thread> readers;
            for (int i = 0; i < n; ++i) readers.emplace_back(reader);

            auto t0 = clk::now();
            std::this_thread::sleep_for(std::chrono::seconds(DURATION));
            stop.store(true, std::memory_order_relaxed);
            w.join();
            for (auto& r : readers) r.join();
            auto t1 = clk::now();

            double rps = total_reads.load() / std::chrono::duration<double>(t1 - t0).count();
            std::printf("msm_object,%d,%.0f\n", n, rps);
            msm_free("cmp_msm");
        }

        // std::atomic<int32_t> — baseline reference for "best possible" lock-free
        {
            AtomicLocked l;
            double rps = run_locked_scalability(n, DURATION, l);
            std::printf("std_atomic,%d,%.0f\n", n, rps);
        }

        // std::mutex
        {
            MutexLocked l;
            double rps = run_locked_scalability(n, DURATION, l);
            std::printf("std_mutex,%d,%.0f\n", n, rps);
        }

        // std::shared_mutex (multiple readers can hold simultaneously)
        {
            SharedMutexLocked l;
            double rps = run_locked_scalability(n, DURATION, l);
            std::printf("std_shared_mutex,%d,%.0f\n", n, rps);
        }

        // pthread_rwlock (POSIX equivalent of shared_mutex)
        {
            PthreadRwlockLocked l;
            double rps = run_locked_scalability(n, DURATION, l);
            std::printf("pthread_rwlock,%d,%.0f\n", n, rps);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::printf("# msm-concurrency-bench\n");
    std::printf("# All times in nanoseconds unless noted.\n");
    std::printf("#\n");

    bool run_lat = (argc < 2) || std::strcmp(argv[1], "latency") == 0;
    bool run_scale = (argc < 2) || std::strcmp(argv[1], "scalability") == 0;
    bool run_cmp = (argc < 2) || std::strcmp(argv[1], "comparison") == 0;

    if (run_lat) run_latency_benchmarks();
    if (run_lat && (run_scale || run_cmp)) std::printf("\n");
    if (run_scale) run_scalability_benchmark();
    if (run_scale && run_cmp) std::printf("\n");
    if (run_cmp) run_comparison_benchmark();

    return 0;
}
