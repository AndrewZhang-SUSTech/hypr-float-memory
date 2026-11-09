// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace std::chrono_literals;
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T take(memory::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error());
    return std::move(*result);
}
void take(memory::Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error());
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("hypr-float-memory-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        const auto path = root / "sizes.cbor";
        memory::Geometries aging{{{"expired", "main"}, {600, 400, 100}},
                                 {{"boundary", "main"}, {600, 400, 113600}},
                                 {{"future", "main"}, {600, 400, 900000}}};
        check(memory::prune(aging, 200000, 1) == 1, "expired records removed");
        check(aging.contains({"boundary", "main"}), "exact boundary retained");
        check(aging.at({"future", "main"}).lastUsed == 900000, "clock rollback preserves future records");
        check(memory::prune(aging, 900000, 0) == 0, "retention disabled");
        check(take(memory::read(path)).empty(), "missing state");
        memory::Geometries sizes{{{"org.example.App", "main"}, {1400, 900, 1}},
                                 {{"org.example.Other", "secondary"}, {960, 540, 1}}};
        take(memory::write(path, sizes));
        check(take(memory::read(path)) == sizes, "CBOR round trip");
        check(std::filesystem::file_size(path) < 100, "compact encoding");
        check(memory::identity("app", false) != memory::identity("app", true), "dialog identity");
        {
            memory::Writer writer(path, sizes);
            writer.flushAsync();
            take(writer.flush());
            check(writer.stats().writes == 0, "clean flush must not write");
            for (int i = 0; i < 1000; ++i)
                writer.submit({"app", "main"}, {1000 + i, 700, 1});
            sizes[{"app", "main"}] = {1999, 700, 1};
            take(writer.flush());
            check(take(memory::read(path)) == sizes, "coalesced last value and untouched entries");
            check(writer.stats().writes == 1, "burst should write once");
            for (int i = 0; i < 10; ++i) {
                writer.submit({"app", "main"}, {1999, 700, 1});
                take(writer.flush());
            }
            check(writer.stats().writes == 1, "identical values must not write");
            sizes[{"app", "main"}] = {1600, 900, 1};
            writer.submit({"app", "main"}, {1600, 900, 1});
        }
        check(take(memory::read(path)) == sizes, "jthread stop drains updates");
        {
            memory::Writer writer(root / "debounce.cbor");
            writer.submit({"a", "main"}, {400, 300, 1});
            std::this_thread::sleep_for(100ms);
            check(writer.stats().writes == 0, "debounce delays writes");
            writer.submit({"a", "main"}, {500, 300, 1});
            // Poll only in tests, with a generous bound for a loaded CI machine.
            for (int i = 0; i < 100 && writer.stats().writes == 0; ++i)
                std::this_thread::sleep_for(20ms);
            check(writer.stats().writes == 1, "debounce eventually writes without flush");
            take(writer.flush());
            check(take(memory::read(root / "debounce.cbor")).at({"a", "main"}) == memory::Geometry{500, 300, 1},
                  "debounce latest value");
        }
        {
            memory::Writer writer(root / "ownership.cbor");
            auto wrongThread = std::async(std::launch::async, [&] {
                try {
                    writer.submit({"bad", "main"}, {400, 300, 1});
                } catch (const std::logic_error &) {
                    return true;
                }
                return false;
            });
            check(wrongThread.get(), "second producer rejected");
            for (int i = 0; i < 2000; ++i) {
                writer.submit({i % 2 ? "a" : "b", "main"}, {i + 1, 800, 1});
                if (i % 100 == 0)
                    writer.flushAsync();
            }
            take(writer.flush());
            auto saved = take(memory::read(root / "ownership.cbor"));
            check(saved.at({"a", "main"}).width == 2000 && saved.at({"b", "main"}).width == 1999,
                  "async barriers preserve final values");
        }
        {
            std::ofstream bad(path, std::ios::binary);
            bad << "corrupt";
        }
        check(!memory::read(path), "corrupt state rejected");
        auto blocked = root / "not-a-directory";
        {
            std::ofstream f(blocked);
            f << "x";
        }
        {
            memory::Writer writer(blocked / "sizes.cbor");
            writer.submit({"retry", "main"}, {600, 400, 1});
            check(!writer.flush() && writer.hasError(), "I/O error is an expected failure");
            check(!writer.error().empty(), "error detail");
            std::filesystem::remove(blocked);
            take(writer.flush());
            check(!writer.hasError(), "successful retry clears error");
            check(take(memory::read(blocked / "sizes.cbor")).at({"retry", "main"}) == memory::Geometry{600, 400, 1},
                  "failed update retained for retry");
        }
        // A failed final write must terminate, not retry in a busy loop forever.
        {
            memory::Writer writer(path / "invalid-child");
            writer.submit({"a", "main"}, {400, 300, 1});
        }
        for (const auto &e : std::filesystem::recursive_directory_iterator(root))
            check(e.path().string().find(".tmp.") == std::string::npos, "temporary files cleaned up");
        std::filesystem::remove_all(root);
        std::cout << "Persistence, coalescing, debounce, shutdown, concurrency and retry tests passed\n";
    } catch (const std::exception &e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
