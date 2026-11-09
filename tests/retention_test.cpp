// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
void check(bool x, const char *message) {
    if (!x)
        throw std::runtime_error(message);
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("hfm-retention-" + std::to_string(getpid()));
    try {
        const auto config = root / "config/hypr-float-memory/config.toml";
        const auto state = root / "state/hypr-float-memory/sizes.cbor";
        std::filesystem::create_directories(config.parent_path());
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
        const auto now = memory::nowSeconds();
        const memory::Geometries entries{{{"expired", "main"}, {600, 400, now - 31 * 86400}},
                                         {{"fresh", "main"}, {600, 400, now}}};
        check(memory::write(state, entries).has_value(), "seed");
        auto loaded = memory::loadState();
        check(loaded && loaded->size() == 1 && loaded->contains({"fresh", "main"}), "default TTL");
        check(memory::read(state) == loaded, "pruning persisted");
        const auto mtime = std::filesystem::last_write_time(state);
        check(memory::loadState().has_value() && std::filesystem::last_write_time(state) == mtime, "no clean rewrite");
        {
            std::ofstream f(config);
            f << "[retention]\nmax_age_days = 0\n";
        }
        check(memory::write(state, entries).has_value(), "seed again");
        check(memory::loadState()->size() == 2, "disabled pruning");
        for (const char *invalid :
             {"[retention]\nmax_age_days = -1", "[retention]\nmax_age_days = 1.5", "[retention]\nmax_age_days = 36501",
              "[retention]\nunknown = 1", "[other]\nx=1", "[invalid"}) {
            {
                std::ofstream f(config);
                f << invalid;
            }
            check(!memory::loadState(), "invalid TOML rejected");
            check(memory::read(state)->size() == 2, "invalid config preserves state");
        }
        {
            std::ofstream f(config);
            f << "[retention]\nmax_age_days = 1\n";
        }
        check(memory::write(state, {{{"expired", "main"}, {600, 400, now - 2 * 86400}}}).has_value(),
              "seed expired only");
        check(memory::loadState()->empty() && memory::read(state)->empty(), "empty result persisted");
        {
            // A record without its required timestamp must be rejected.
            const unsigned char data[] = {0x82, 0x01, 0x81, 0x84, 0x61, 'a', 0x61, 'b', 0x01, 0x01};
            std::ofstream f(state, std::ios::binary);
            f.write(reinterpret_cast<const char *>(data), sizeof(data));
        }
        check(!memory::read(state), "missing timestamp rejected");
        check(!memory::write(state, {{{"invalid", "main"}, {600, 400, 0}}}), "timestamp required");
        std::filesystem::remove_all(root);
        std::cout << "Retention and TOML tests passed\n";
    } catch (const std::exception &e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
