// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <toml++/toml.hpp>
#include <cstdlib>

namespace memory {
std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::size_t prune(Geometries &sizes, std::int64_t now, std::int64_t days) {
    const auto count = std::erase_if(sizes, [=](const auto &item) {
        const auto stamp = item.second.lastUsed;
        return days > 0 && stamp > 0 && stamp < now && now - stamp > days * 86400;
    });
    return count;
}
Result<Geometries> loadState() try {
    std::filesystem::path configRoot;
    if (const char *p = std::getenv("XDG_CONFIG_HOME"); p && std::filesystem::path(p).is_absolute())
        configRoot = p;
    else if (const char *p = std::getenv("HOME"); p && *p)
        configRoot = std::filesystem::path(p) / ".config";
    else
        return std::unexpected("No configuration directory available");
    const auto config = configRoot / "hypr-float-memory/config.toml";
    std::int64_t days = 30;
    if (std::filesystem::exists(config)) {
        const auto table = toml::parse_file(config.string());
        for (const auto &[key, node] : table)
            if (key != "retention")
                return std::unexpected("Unknown TOML section");
        if (auto *section = table.get("retention")) {
            if (!section->is_table())
                return std::unexpected("retention must be a table");
            for (const auto &[key, node] : *section->as_table())
                if (key != "max_age_days")
                    return std::unexpected("Unknown retention setting");
            if (auto *age = section->as_table()->get("max_age_days")) {
                auto value = age->value<std::int64_t>();
                if (!age->is_integer() || !value || *value < 0 || *value > 36500)
                    return std::unexpected("max_age_days must be an integer in 0..36500");
                days = *value;
            }
        }
    }
    const auto path = statePath();
    auto loaded = read(path);
    if (!loaded)
        return loaded;
    if (prune(*loaded, nowSeconds(), days) != 0) {
        if (auto result = write(path, *loaded); !result)
            return std::unexpected(result.error());
    }
    return loaded;
} catch (const std::exception &e) {
    return std::unexpected(e.what());
}
} // namespace memory
