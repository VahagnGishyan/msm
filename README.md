# MSM — Lock-Free SWMR Protocol for Dynamic Types

Reference implementation accompanying the paper
**"Lock-Free Protocol for Dynamic Types in Mutable Shared Memory"** (V.A. Gishyan, 2026).

The library exposes a flat C ABI over which a single writer and any number of
readers can share three dynamic types — `string`, growing `array`, and
key/value `object` — within one named segment, with wait-free reads and
lock-free writes under the C++20 memory model on x86-64.

---

## Repository layout

```
msm-standalone/
├── CMakeLists.txt              top-level
├── src/msm/                    the MSM library (C ABI + headers)
├── tests/                      Google Test suite (cited in paper §3.1)
│   ├── test_concurrent.cpp     5 stress tests, 1 writer + 4 readers
│   ├── test_memory_overhead.cpp RSS bounds (paper §3.5)
│   ├── test_schema_hash.cpp    schema-hash death tests
│   └── ...                     functional tests for every type
├── benchmarks/concurrency/     scalability + comparison benchmark (paper §3.2–§3.4)
└── results/                    logs of our reference runs
    ├── benchmark-release.log
    ├── tsan-clean.log
    ├── memory-overhead.log
    ├── schema-hash-test.log
    ├── valgrind-leak.log
    └── ...
```

---

## Requirements

| Component | Version |
|-----------|---------|
| Compiler  | GCC 11.4+ or Clang 14+ (C++20) |
| CMake     | 3.20+ |
| Google Test | any recent release |
| pthread   | required (linked by the benchmark) |
| ThreadSanitizer | GCC/Clang built-in, opt-in via `-DENABLE_TSAN=ON` |
| Valgrind  | optional, only for the memory-leak validation |

Platform: x86-64 Linux. Tested on Ubuntu 22.04.

---

## Build

### Release (used for all performance numbers in the paper)

```bash
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

Binaries land in `bin/`:
- `libmsm.so` — the MSM shared library
- `run-all-tests` — the full Google Test suite
- `msm-concurrency-bench` — the benchmark used in §3.2–§3.4

### Debug + ThreadSanitizer (used for the 79/79 + 0-races claim)

```bash
cmake -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build/tsan -j
```

> TSAN requires GCC/Clang. It enables `-fsanitize=thread -fno-omit-frame-pointer -g -O1`
> across **every** translation unit — partial instrumentation produces false negatives.
> Runtime overhead is roughly 5–15× slower and 5–10× more memory.

---

## Run tests

### Plain functional + concurrency suite (Release)

```bash
ctest --test-dir build/release --output-on-failure
# or directly:
./bin/run-all-tests
```

Expected: **79 / 79 tests pass**. Matches `results/tests-baseline.log`.

### Under ThreadSanitizer

```bash
ctest --test-dir build/tsan --output-on-failure
# or:
./bin/run-all-tests
```

Expected: **0 data races reported** across all five concurrent stress tests
(`ArrayPushBackNoTornRead`, `StringGrowNoTornRead`, `StringShrinkGrowNoTornRead`,
`ObjectAddRemoveNoTornRead`, `ObjectMarkerRemoveNoStaleReads`).
Matches `results/tsan-clean.log`. Whole suite finishes in ≈4.5 s — about 11×
slower than the uninstrumented Debug build (≈393 ms), confirming TSAN is live.

### Memory bounds (paper Table 4)

```bash
./bin/run-all-tests --gtest_filter='*MemoryOverhead*'
```

Asserts the RSS growth bounds for the three workloads (`array push_back × 100K`,
`string set × 1000`, `object add 1000 + remove 500`). Cross-check against
`results/memory-overhead.log`.

### Leak check with Valgrind (optional)

```bash
valgrind --leak-check=full --show-leak-kinds=all ./bin/run-all-tests
```

Expected: **0 bytes still reachable** on the functional suite. See
`results/valgrind-leak.log` for the full reference output (including the
documented 280 MB *definitely-lost* on the adversarial `ObjectAddRemove`
workload — this is the bump-allocator trade-off discussed in paper §2.4.2,
not a bug).

---

## Run benchmarks

### Scalability sweep (paper Fig. 3 / Table referenced in §3.2)

```bash
./bin/msm-concurrency-bench --section 2
```

Reports reads/s for 1, 2, 4, 8, 16 readers + 1 writer on a shared `int32` field
inside an `msm_object`. Paper numbers: 154 → 222 → 389 → 589 → 785 M reads/s.

### Comparison vs std::atomic / std::mutex / std::shared_mutex / pthread_rwlock_t

```bash
./bin/msm-concurrency-bench --section 3
```

Same workload through five different synchronization primitives at 16 readers.
Paper numbers (M reads/s): `std::atomic 8246`, **MSM 915**, `std::mutex 51`,
`std::shared_mutex 27`, `pthread_rwlock 28`.

### Single-thread latency CDF (paper Table 2)

```bash
./bin/msm-concurrency-bench --section 1
```

Reports min / p50 / p90 / p95 / p99 / p99.9 / max for every basic operation.

Full reference run is in `results/benchmark-release.log`.

---

## Reproducing each table from the paper

| Paper artifact | Command | Reference log |
|----------------|---------|---------------|
| Table 1 — TSAN-clean stress tests | `ctest --test-dir build/tsan` | `results/tsan-clean.log` |
| Table 2 — single-threaded latency | `./bin/msm-concurrency-bench --section 1` | `results/benchmark-release.log` |
| Table 3 — comparison vs locks/atomic | `./bin/msm-concurrency-bench --section 3` | `results/benchmark-release.log` |
| Table 4 — RSS overhead | `./bin/run-all-tests --gtest_filter='*MemoryOverhead*'` | `results/memory-overhead.log` |
| Fig. 3 — scalability 1+N readers | `./bin/msm-concurrency-bench --section 2` | `results/benchmark-release.log` |
| Valgrind "0 still reachable" | `valgrind ./bin/run-all-tests` | `results/valgrind-leak.log` |

---

## License

TBD — add before publication.

---

## Citation

```
Gishyan V.A.  Lock-Free Protocol for Dynamic Types in Mutable Shared Memory.
Vestnik of the Russian-Armenian (Slavonic) University, 2026.
```
