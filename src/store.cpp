// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <span>
#include <utility>

namespace memory {
namespace {
class FileDescriptor {
  public:
    explicit FileDescriptor(int fd) : fd_(fd) {
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "open state file");
    }
    ~FileDescriptor() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    int get() const noexcept { return fd_; }
    void sync() const {
        if (::fsync(fd_) != 0)
            throw std::system_error(errno, std::generic_category(), "fsync state");
    }

  private:
    int fd_;
};
struct TemporaryFile {
    const char *path;
    ~TemporaryFile() {
        if (path)
            ::unlink(path);
    }
};
void writeAll(int fd, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        auto n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::system_error(n == 0 ? EIO : errno, std::generic_category(), "write state");
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
}
} // namespace
Key identity(const std::string &app, bool transient) { return {app, transient ? "dialog" : "main"}; }
std::filesystem::path statePath() {
    if (const char *p = std::getenv("XDG_STATE_HOME"); p && std::filesystem::path(p).is_absolute())
        return std::filesystem::path(p) / "hypr-float-memory/sizes.cbor";
    if (const char *p = std::getenv("HOME"); p && *p)
        return std::filesystem::path(p) / ".local/state/hypr-float-memory/sizes.cbor";
    throw std::runtime_error("HOME and XDG_STATE_HOME are unavailable");
}
Result<Geometries> read(const std::filesystem::path &path) try {
    if (!std::filesystem::exists(path))
        return Geometries{};
    const auto length = std::filesystem::file_size(path);
    if (length > 4 * 1024 * 1024)
        throw std::runtime_error("state file exceeds 4 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    std::vector<std::uint8_t> bytes(length);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::unexpected("cannot read complete state file");
    const auto doc = nlohmann::json::from_cbor(bytes);
    if (!doc.is_array() || doc.size() != 2 || doc[0] != 2 || !doc[1].is_array() || doc[1].size() > 10000)
        throw std::runtime_error("unsupported or invalid CBOR state");
    Geometries result;
    for (const auto &row : doc[1]) {
        if (!row.is_array() || row.size() != 6 || !row[0].is_string() || !row[1].is_string() ||
            !row[2].is_number_integer() || !row[3].is_number_integer())
            throw std::runtime_error("invalid state entry");
        auto app = row[0].get<std::string>(), role = row[1].get<std::string>();
        auto w = row[2].get<std::int64_t>(), h = row[3].get<std::int64_t>();
        if (app.empty() || app.size() > 4096 || role.size() > 4096 || w < 1 || h < 1 || w > 32768 || h > 32768)
            throw std::runtime_error("invalid state dimensions or identity");
        if (!row[5].is_number_integer() || row[5] <= 0 || row[5] > INT64_MAX)
            return std::unexpected("invalid timestamp");
        const auto stamp = row[5].get<std::int64_t>();
        std::optional<Position> position;
        if (!row[4].is_null()) {
            if (!row[4].is_array() || row[4].size() != 2)
                throw std::runtime_error("invalid position");
            for (const auto &v : row[4])
                if (!v.is_number_integer() || v < INT32_MIN || v > INT32_MAX)
                    throw std::runtime_error("invalid position coordinate");
            position = Position{row[4][0].get<std::int32_t>(), row[4][1].get<std::int32_t>()};
        }
        result[{app, role}] = {static_cast<int>(w), static_cast<int>(h), stamp, position};
    }
    return result;
} catch (const std::exception &e) {
    return std::unexpected(e.what());
}
Result<void> write(const std::filesystem::path &path, const Geometries &sizes) try {
    auto rows = nlohmann::json::array();
    rows.get_ref<nlohmann::json::array_t &>().reserve(sizes.size());
    for (const auto &[key, size] : sizes) {
        if (!valid(size) || size.lastUsed <= 0)
            throw std::runtime_error("invalid size to persist");
        nlohmann::json position = nullptr;
        if (size.position)
            position = {size.position->x, size.position->y};
        rows.push_back({key.first, key.second, size.width, size.height, std::move(position), size.lastUsed});
    }
    auto document = nlohmann::json::array();
    document.push_back(2);
    document.push_back(std::move(rows));
    auto bytes = nlohmann::json::to_cbor(document);
    std::filesystem::create_directories(path.parent_path());
    auto pattern = path.string() + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    FileDescriptor file(::mkstemp(name.data()));
    TemporaryFile cleanup{name.data()};
    writeAll(file.get(), bytes);
    file.sync();
    std::filesystem::rename(name.data(), path);
    cleanup.path = nullptr;
    FileDescriptor directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    directory.sync();
    return {};
} catch (const std::exception &e) {
    return std::unexpected(e.what());
}
} // namespace memory
