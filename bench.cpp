// bench.cpp — measure how fast the store is.
//
// It benchmarks the storage engine directly (not over the network), so the
// numbers reflect the engine itself:
//   - write throughput   (SETs per second)
//   - read throughput    (GETs per second)
//   - recovery time      (how long to rebuild the index from a full file)
//
// We use a separate file (bench.db) so your real data.db is never touched.

#include "kvstore.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <random>
#include <cstdio>
#include <algorithm>
#include <cstdlib>

using Clock = std::chrono::high_resolution_clock;

// Turn a duration into seconds as a double.
static double seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

int main(int argc, char** argv) {
    const int N = (argc > 1) ? std::atoi(argv[1]) : 50000; // number of keys
    const std::string path = "bench.db";
    std::remove(path.c_str()); // start from a clean file

    std::cout << "benchmarking with " << N << " keys...\n\n";

    // Pre-build the keys and values so string work isn't counted in timing.
    std::vector<std::string> keys(N), vals(N);
    for (int i = 0; i < N; ++i) {
        keys[i] = "key:" + std::to_string(i);
        vals[i] = "value-number-" + std::to_string(i);
    }

    double write_secs = 0, read_secs = 0, recover_secs = 0;
    uint64_t file_bytes = 0;

    {
        KVStore store(path);

        // --- WRITE benchmark: insert all N pairs, timed ---
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) store.set(keys[i], vals[i]);
        auto t1 = Clock::now();
        write_secs = seconds(t0, t1);

        // --- READ benchmark: read all N keys in shuffled order, timed ---
        std::vector<int> order(N);
        for (int i = 0; i < N; ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937(42));

        std::string out;
        auto t2 = Clock::now();
        for (int i = 0; i < N; ++i) store.get(keys[order[i]], out);
        auto t3 = Clock::now();
        read_secs = seconds(t2, t3);

        file_bytes = store.fileSize();
    } // store closes here

    // --- RECOVERY benchmark: reopen the file, timing the index rebuild ---
    {
        auto t4 = Clock::now();
        KVStore store(path); // constructor replays the whole file
        auto t5 = Clock::now();
        recover_secs = seconds(t4, t5);
    }

    std::remove(path.c_str()); // clean up

    // --- report ---
    auto rate = [](int n, double s) { return s > 0 ? (n / s) : 0.0; };
    std::cout << "WRITE:    " << (int)rate(N, write_secs) << " ops/sec"
              << "   (" << write_secs * 1e6 / N << " us/op)\n";
    std::cout << "READ:     " << (int)rate(N, read_secs) << " ops/sec"
              << "   (" << read_secs * 1e6 / N << " us/op)\n";
    std::cout << "RECOVERY: " << recover_secs * 1000.0 << " ms to rebuild "
              << N << " keys from disk\n";
    std::cout << "FILE:     " << file_bytes / 1024 << " KB on disk\n";
    return 0;
}
