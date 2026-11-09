// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <unistd.h>
int main() {
    using clock = std::chrono::steady_clock;
    const auto root = std::filesystem::temp_directory_path() / ("hfm-bench-" + std::to_string(getpid()));
    std::cout << "records,mean_ns,p99_ns\n";
    for (int count : {10, 100, 1000}) {
        memory::Geometries initial;
        for (int i = 0; i < count; ++i)
            initial[{"app-" + std::to_string(i), "main"}] = {1200, 720, 1};
        memory::Writer writer(root / "sizes.cbor", initial);
        const memory::Key key{"app-0", "main"};
        std::vector<double> samples;
        samples.reserve(5000);
        double sum = 0;
        for (int i = 0; i < 5000; ++i) {
            const auto start = clock::now();
            writer.submit(key, {800 + i % 500, 600, 1});
            const auto ns = std::chrono::duration<double, std::nano>(clock::now() - start).count();
            samples.push_back(ns);
            sum += ns;
        }
        if (auto result = writer.flush(); !result) {
            std::cerr << result.error() << '\n';
            return 1;
        }
        std::ranges::sort(samples);
        std::cout << count << ',' << sum / samples.size() << ',' << samples[4950] << '\n';
    }
    std::filesystem::remove_all(root);
}
