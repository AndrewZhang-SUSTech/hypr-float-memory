// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <unistd.h>
using namespace std::chrono_literals;
void check(bool v, const char *message) {
    if (!v)
        throw std::runtime_error(message);
}
void success(memory::Result<void> r) {
    if (!r)
        throw std::runtime_error(r.error());
}
struct Gate {
    std::latch entered{1}, release{1};
    std::atomic<bool> first{true}, persisted{false};
    memory::Result<void> operator()(const std::filesystem::path &path, const memory::Geometries &sizes) {
        if (first.exchange(false)) {
            entered.count_down();
            release.wait();
        }
        auto result = memory::write(path, sizes);
        persisted.store(true);
        return result;
    }
};
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("hfm-queue-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(root);
        {
            Gate gate;
            memory::Writer writer(root / "overflow.cbor", {}, {1, 0ms, std::ref(gate)});
            writer.submit({"seed", "main"}, {500, 400, 1});
            gate.entered.wait();
            memory::Geometries expected{{{"seed", "main"}, {500, 400, 1}}};
            for (int i = 1; i <= 5000; ++i) {
                const memory::Key key{"app-" + std::to_string(i % 7), "main"};
                const memory::Geometry size{500 + i, 400, 1};
                expected[key] = size;
                writer.submit(key, size);
            }
            check(writer.needsPump() && writer.stats().overflows > 0 && writer.stats().coalesced > 0,
                  "overflow path exercised");
            writer.flushAsync();
            gate.release.count_down();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (writer.needsPump() && std::chrono::steady_clock::now() < deadline) {
                writer.pump();
                std::this_thread::sleep_for(1ms);
            }
            check(!writer.needsPump(), "async producer retries drain overflow and barrier");
            success(writer.flush());
            check(memory::read(root / "overflow.cbor").value() == expected, "latest accepted update survives overflow");
        }
        {
            Gate gate;
            memory::Writer writer(root / "barrier.cbor", {}, {2, 0ms, std::ref(gate)});
            writer.submit({"app", "main"}, {600, 400, 1});
            gate.entered.wait();
            std::jthread release([&] {
                std::this_thread::sleep_for(50ms);
                gate.release.count_down();
            });
            success(writer.flush());
            check(gate.persisted.load(), "flush acknowledges storage, not just dequeue");
        }
        {
            Gate gate;
            auto writer = std::make_unique<memory::Writer>(root / "shutdown.cbor", memory::Geometries{},
                                                           memory::WriterOptions{1, 0ms, std::ref(gate)});
            writer->submit({"app", "main"}, {600, 400, 1});
            gate.entered.wait();
            for (int i = 0; i < 1000; ++i)
                writer->submit({"app", "main"}, {1000 + i, 400, 1});
            check(writer->needsPump(), "shutdown begins with overflow");
            std::jthread release([&] {
                std::this_thread::sleep_for(50ms);
                gate.release.count_down();
            });
            writer.reset();
            check(memory::read(root / "shutdown.cbor")->at({"app", "main"}).width == 1999,
                  "shutdown drains final pending value");
        }
        {
            memory::Writer writer(root / "model.cbor", {}, {3, 1ms, {}});
            memory::Geometries model;
            uint32_t random = 0x12345678;
            for (int i = 0; i < 12000; ++i) {
                random = random * 1664525U + 1013904223U;
                memory::Key key{"app-" + std::to_string(random % 97), "main"};
                memory::Geometry value{static_cast<int>(random % 10000) + 1, 400, 1};
                writer.submit(key, value);
                model[key] = value;
                if (i % 37 == 0)
                    writer.flushAsync();
                if (i % 11 == 0)
                    writer.pump();
                if (i % 311 == 0) {
                    success(writer.flush());
                    check(memory::read(root / "model.cbor").value() == model, "barrier matches sequential model");
                }
            }
            success(writer.flush());
            check(memory::read(root / "model.cbor").value() == model, "final model matches");
        }
        std::filesystem::remove_all(root);
        std::cout << "Overflow, async retries, durability barriers, shutdown and sequential-model tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
