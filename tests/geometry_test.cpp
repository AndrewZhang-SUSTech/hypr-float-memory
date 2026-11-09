// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <unistd.h>
void check(bool b, const char *msg) {
    if (!b)
        throw std::runtime_error(msg);
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("hfm-geometry-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        using namespace memory;
        check(!coordinate(NAN) && !coordinate(INFINITY) && !coordinate(2147483648.0), "invalid host coordinate");
        check(coordinate(-4.6) == -5 && coordinate(INT32_MIN) == INT32_MIN, "signed rounding");
        check(fitPosition({-100, 900}, 600, 400, 1280, 800) == Position{0, 400}, "visible clamp");
        check(fitPosition({300, 400}, 1400, 900, 1280, 800) == Position{0, 0}, "oversized window");
        check(fitPosition({300, 200}, 1400, 400, 1280, 800) == Position{0, 0}, "one oversized axis anchors top-left");
        check(fitPosition({300, 200}, 600, 400, 1280, 800) == Position{300, 200},
              "relative offset independent of origin");
        const auto path = root / "sizes.cbor";
        Geometries data{{{"a", "main"}, {800, 600, 100, Position{-20, 40}}}, {{"b", "main"}, {600, 400, 100, {}}}};
        check(write(path, data).has_value() && read(path).value() == data, "optional position round trip");
        auto put = [&](const nlohmann::json &doc) {
            auto bytes = nlohmann::json::to_cbor(doc);
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        };
        using nlohmann::json;
        for (const auto &pos : {json::array({2147483648LL, 0}), json::array({0, -2147483649LL}), json::array({1.5, 0}),
                                json::array({1}), json("bad")}) {
            put(json::array({2, json::array({json::array({"a", "main", 800, 600, pos, 100})})}));
            check(!read(path), "invalid persisted position rejected");
        }
        put(json::array({1, json::array({json::array({"a", "main", 800, 600, 100})})}));
        check(!read(path), "old schema rejected");
        std::latch entered{1}, release{1};
        bool first = true;
        WriterOptions options{1, std::chrono::milliseconds(0),
                              [&](const auto &file, const auto &state) -> Result<void> {
                                  if (first) {
                                      first = false;
                                      entered.count_down();
                                      release.wait();
                                  }
                                  return write(file, state);
                              }};
        {
            Writer writer(path, {}, options);
            writer.submit({"a", "main"}, {800, 600, 100, Position{0, 0}});
            entered.wait();
            for (int i = 1; i <= 5000; ++i)
                writer.submit({"a", "main"}, {800, 600, 100, Position{i, -i}});
            const bool overflow = writer.needsPump();
            release.count_down();
            check(writer.flush().has_value(), "position flush");
            check(overflow, "position-only updates exercise overflow");
            check(read(path)->at({"a", "main"}).position == Position{5000, -5000},
                  "latest position survives coalescing");
        }
        auto aged = read(path).value();
        aged[{"b", "main"}] = {800, 600, 200000, Position{1, 2}};
        check(prune(aged, 200000, 1) == 1 && aged.at({"b", "main"}).position == Position{1, 2},
              "retention preserves position");
        std::filesystem::remove_all(root);
        std::cout << "Geometry tests passed\n";
    } catch (const std::exception &e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
