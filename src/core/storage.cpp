#include "core/storage.h"

#include "util/serialize.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bound {

std::optional<Bytes> MemoryStore::get(ByteSpan key) const {
    const auto found = data_.find(Bytes(key.begin(), key.end()));
    if (found == data_.end()) return std::nullopt;
    return found->second;
}

void MemoryStore::write(const WriteBatch& batch) {
    for (const WriteBatch::Op& op : batch.ops()) {
        if (op.value) {
            data_[op.key] = *op.value;
        } else {
            data_.erase(op.key);
        }
    }
}

std::optional<Bytes> prefix_successor(ByteSpan prefix) {
    Bytes next(prefix.begin(), prefix.end());
    while (!next.empty() && next.back() == 0xff) next.pop_back();
    if (next.empty()) return std::nullopt;
    ++next.back();
    return next;
}

void MemoryStore::scan(ByteSpan prefix, bool reverse, const std::function<bool(ByteSpan key, ByteSpan value)>& f) const {
    const Bytes start(prefix.begin(), prefix.end());
    const auto matches = [&](const Bytes& key) {
        return key.size() >= start.size() && std::equal(start.begin(), start.end(), key.begin());
    };
    if (!reverse) {
        for (auto it = data_.lower_bound(start); it != data_.end() && matches(it->first); ++it) {
            if (!f(it->first, it->second)) return;
        }
        return;
    }
    const std::optional<Bytes> after = prefix_successor(prefix);
    auto it = after ? data_.lower_bound(*after) : data_.end();
    while (it != data_.begin()) {
        --it;
        if (!matches(it->first) || !f(it->first, it->second)) return;
    }
}

BlockPos MemoryBlockStore::append(const Block& block) {
    blocks_.push_back(block.serialize());
    return {0, static_cast<std::uint32_t>(blocks_.size() - 1)};
}

Block MemoryBlockStore::read(BlockPos pos) const {
    if (pos.file != 0 || pos.offset >= blocks_.size()) throw std::runtime_error("no block is stored there");
    return Block::deserialize(blocks_[pos.offset]);
}

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'B', 'O', 'N', 'D'};
constexpr std::uint32_t kRecordHeader = 8;     // magic + length
constexpr std::uint32_t kMaxRecord = 64 << 20;  // far above any valid block; a guard against junk lengths

struct FileCloser {
    void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

File open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), wide_mode.c_str()) != 0) return nullptr;
    return File(f);
#else
    return File(std::fopen(path.c_str(), mode));
#endif
}

// Pushes the bytes past both the C library's buffer and the OS cache, onto the disk.
void sync(std::FILE* f, const std::filesystem::path& path) {
#ifdef _WIN32
    const bool ok = std::fflush(f) == 0 && _commit(_fileno(f)) == 0;
#else
    const bool ok = std::fflush(f) == 0 && fsync(fileno(f)) == 0;
#endif
    if (!ok) throw std::runtime_error("can't write " + path.string());
}

std::uint32_t current_size(const std::filesystem::path& path) {
    return std::filesystem::exists(path) ? static_cast<std::uint32_t>(std::filesystem::file_size(path)) : 0;
}

}  // namespace

FlatFileBlockStore::FlatFileBlockStore(std::filesystem::path dir, std::uint32_t max_file_size)
    : dir_(std::move(dir)), max_file_size_(max_file_size) {
    std::filesystem::create_directories(dir_);
    while (std::filesystem::exists(file_path(file_ + 1))) ++file_;
    size_ = current_size(file_path(file_));
}

std::filesystem::path FlatFileBlockStore::file_path(std::uint32_t file) const {
    return dir_ / std::format("blk{:05}.dat", file);
}

BlockPos FlatFileBlockStore::append(const Block& block) {
    const Bytes data = block.serialize();
    if (data.size() > kMaxRecord) throw std::runtime_error("block too large to store");
    const auto record = kRecordHeader + static_cast<std::uint32_t>(data.size());
    if (size_ > 0 && std::uint64_t{size_} + record > max_file_size_) {
        ++file_;
        size_ = current_size(file_path(file_));
    }

    ByteWriter w;
    w.bytes(kMagic);
    w.u32(static_cast<std::uint32_t>(data.size()));
    w.bytes(data);
    const std::filesystem::path path = file_path(file_);
    const File f = open_file(path, "ab");
    if (!f || std::fwrite(w.data().data(), 1, w.data().size(), f.get()) != w.data().size()) {
        throw std::runtime_error("can't write " + path.string());
    }
    sync(f.get(), path);

    const BlockPos pos{file_, size_};
    size_ += record;
    return pos;
}

Block FlatFileBlockStore::read(BlockPos pos) const {
    const std::filesystem::path path = file_path(pos.file);
    const File f = open_file(path, "rb");
    if (!f) throw std::runtime_error("can't open " + path.string());
    std::array<std::uint8_t, kRecordHeader> header{};
    // A 32-bit offset may not fit a long on Windows, where long is 32 bits; it always does on Linux.
    if (!std::in_range<long>(pos.offset) || std::fseek(f.get(), static_cast<long>(pos.offset), SEEK_SET) != 0 ||
        std::fread(header.data(), 1, header.size(), f.get()) != header.size()) {
        throw std::runtime_error(std::format("{} ends before offset {}", path.string(), pos.offset));
    }
    ByteReader r(header);
    if (!std::ranges::equal(r.bytes(kMagic.size()), kMagic)) {
        throw std::runtime_error(std::format("no block record at {} offset {}", path.string(), pos.offset));
    }
    const std::uint32_t length = r.u32();
    if (length > kMaxRecord) throw std::runtime_error("block record with an impossible length in " + path.string());
    Bytes data(length);
    if (std::fread(data.data(), 1, data.size(), f.get()) != data.size()) {
        throw std::runtime_error("block record cut short in " + path.string());
    }
    return Block::deserialize(data);
}

}  // namespace bound
