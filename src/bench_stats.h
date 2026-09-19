// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace minikv {

struct LatencySummary {
    std::size_t samples = 0;
    double min = 0, max = 0, mean = 0, p50 = 0, p95 = 0, p99 = 0;
};

inline LatencySummary SummarizeLatencies(std::vector<double> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        return values[static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1];
    };
    return {values.size(), values.front(), values.back(),
            std::accumulate(values.begin(), values.end(), 0.0) / values.size(),
            percentile(0.50), percentile(0.95), percentile(0.99)};
}

}  // namespace minikv
