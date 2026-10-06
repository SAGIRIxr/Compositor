// 简单的行并行：把 [0, count) 切成若干段交给多个线程。
#pragma once
#include <algorithm>
#include <thread>
#include <vector>

namespace comp {

template <typename Body>
void parallelRanges(int count, Body body, int minimumPerTask = 16) {
    if (count <= 0) return;
    int threads = int(std::max(1u, std::thread::hardware_concurrency()));
    int tasks = std::min(threads, std::max(1, count / std::max(1, minimumPerTask)));
    if (tasks <= 1) { body(0, count); return; }
    std::vector<std::thread> pool;
    pool.reserve(size_t(tasks - 1));
    int chunk = (count + tasks - 1) / tasks;
    for (int t = 1; t < tasks; ++t) {
        int begin = t * chunk, end = std::min(count, begin + chunk);
        if (begin >= end) break;
        pool.emplace_back([=] { body(begin, end); });
    }
    body(0, std::min(count, chunk));
    for (auto& thread : pool) thread.join();
}

} // namespace comp
