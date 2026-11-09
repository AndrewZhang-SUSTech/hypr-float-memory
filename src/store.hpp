// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <optional>
#include <cmath>
#include <limits>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace memory {
struct Position {
    std::int32_t x = 0, y = 0;
    bool operator==(const Position &) const = default;
};
// Check before rounding or narrowing coordinates received from the compositor.
inline std::optional<std::int32_t> coordinate(double value) {
    if (!std::isfinite(value))
        return {};
    const double rounded = std::round(value);
    if (rounded < INT32_MIN || rounded > INT32_MAX)
        return {};
    return static_cast<std::int32_t>(rounded);
}
inline Position fitPosition(Position pos, int width, int height, int areaWidth, int areaHeight) {
    if (width > areaWidth || height > areaHeight)
        return {0, 0};
    return {std::clamp(pos.x, 0, std::max(0, areaWidth - width)),
            std::clamp(pos.y, 0, std::max(0, areaHeight - height))};
}
struct Geometry {
    int width = 1200;
    int height = 720;
    std::int64_t lastUsed = 0;
    std::optional<Position> position{};
    bool operator==(const Geometry &) const = default;
};
using Key = std::pair<std::string, std::string>;
using Geometries = std::map<Key, Geometry>;
template <typename T> using Result = std::expected<T, std::string>;
[[nodiscard]] constexpr bool valid(Geometry s) noexcept {
    return s.width > 0 && s.height > 0 && s.width <= 32768 && s.height <= 32768;
}
Key identity(const std::string &app, bool transient);
std::filesystem::path statePath();
[[nodiscard]] Result<Geometries> read(const std::filesystem::path &path);
[[nodiscard]] Result<void> write(const std::filesystem::path &path, const Geometries &sizes);
std::int64_t nowSeconds();
std::size_t prune(Geometries &sizes, std::int64_t now, std::int64_t days);
[[nodiscard]] Result<Geometries> loadState();
struct WriterStats {
    uint64_t updates = 0, coalesced = 0, wakeups = 0, writes = 0, overflows = 0;
};
struct WriterOptions {
    std::size_t queueCapacity = 256;
    std::chrono::milliseconds debounce{300};
    // Optional sink for deterministic I/O tests. Called only by the worker.
    std::function<Result<void>(const std::filesystem::path &, const Geometries &)> sink;
};
// submit, pump, flush, stats, and destruction belong to the creating thread.
// No Hyprland/Lua objects are sent to the worker.
class Writer {
  public:
    explicit Writer(std::filesystem::path path, Geometries initial = {}, WriterOptions options = {});
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    void submit(const Key &key, Geometry size);
    void pump();
    [[nodiscard]] bool needsPump() const;
    void flushAsync();
    [[nodiscard]] Result<void> flush();
    [[nodiscard]] bool hasError() const noexcept;
    [[nodiscard]] std::string error();
    [[nodiscard]] WriterStats stats();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace memory
