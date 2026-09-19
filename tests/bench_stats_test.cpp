// SPDX-License-Identifier: Apache-2.0

#include "bench_stats.h"
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    const auto check = [](bool ok) {
        if (!ok) throw std::runtime_error("latency summary mismatch");
    };
    const auto empty = minikv::SummarizeLatencies({});
    check(empty.samples == 0 && empty.p99 == 0 && empty.mean == 0);
    const auto one = minikv::SummarizeLatencies({3.5});
    check(one.samples == 1 && one.min == 3.5 && one.p99 == 3.5 && one.max == 3.5);
    std::vector<double> samples;
    for (int i = 100; i > 0; --i) samples.push_back(i);
    const auto result = minikv::SummarizeLatencies(samples);
    check(result.samples == 100 && result.min == 1 && result.max == 100);
    check(result.p50 == 50 && result.p95 == 95 && result.p99 == 99 && result.mean == 50.5);
    const auto small = minikv::SummarizeLatencies({8, 2, 4});
    check(small.p50 == 4 && small.p95 == 8 && small.p99 == 8);
    std::cout << "PASS: exact nearest-rank latency statistics\n";
}
