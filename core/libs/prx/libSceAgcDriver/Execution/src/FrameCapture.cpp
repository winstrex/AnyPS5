#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "zlib.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace AgcDriver {
namespace {

constexpr std::uint32_t PageBlockKind = 100;
constexpr std::uint32_t DeflatedFlag = 1;
constexpr std::size_t ChunkHeaderBytes = 24;

void RequireCapture(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("frame capture: " + message);
}

std::array<std::uint64_t, 2> HashPage(const std::byte* page, bool& zero) {
    std::uint64_t first = 0x9e3779b97f4a7c15ull;
    std::uint64_t second = 0xc2b2ae3d27d4eb4full;
    std::uint64_t any = 0;
    for (std::size_t offset = 0; offset < FrameCaptureFormat::PageBytes; offset += sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, page + offset, sizeof(word));
        any |= word;
        first = std::rotl((first ^ word) * 0xff51afd7ed558ccdull, 29);
        second = std::rotl((second + word) * 0x87c37b91114253d5ull, 31) ^ (second >> 17);
    }
    zero = any == 0;
    first ^= first >> 33;
    first *= 0xc4ceb9fe1a85ec53ull;
    second ^= second >> 31;
    second *= 0x4cf5ad432745937full;
    return {first ^ (second >> 29), second ^ (first >> 27)};
}

class Encoder {
public:
    void U8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) U8(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) U8(static_cast<std::uint8_t>(value >> shift));
    }
    void Bytes(std::span<const std::byte> value) {
        U64(value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void Registers(const std::vector<ShaderRegister>& registers) {
        U32(static_cast<std::uint32_t>(registers.size()));
        for (const auto& reg : registers) {
            U32(reg.offset);
            U32(reg.value);
        }
    }
    std::vector<std::byte> bytes;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> bytes) : bytes(bytes) {}
    std::uint8_t U8() {
        RequireCapture(cursor < bytes.size(), "truncated record");
        return std::to_integer<std::uint8_t>(bytes[cursor++]);
    }
    std::uint32_t U32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(U8()) << shift;
        return value;
    }
    std::uint64_t U64() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(U8()) << shift;
        return value;
    }
    bool Flag() {
        const auto value = U8();
        RequireCapture(value <= 1, "invalid flag");
        return value != 0;
    }
    std::size_t Count(std::size_t elementBytes) {
        const auto count = U64();
        RequireCapture(count <= (bytes.size() - cursor) / elementBytes, "count exceeds its record");
        return static_cast<std::size_t>(count);
    }
    std::vector<std::byte> Bytes() {
        const auto count = Count(1);
        std::vector<std::byte> value(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.begin() + static_cast<std::ptrdiff_t>(cursor + count));
        cursor += count;
        return value;
    }
    std::vector<ShaderRegister> Registers() {
        const auto count = U32();
        RequireCapture(count <= (bytes.size() - cursor) / 8, "register count exceeds its record");
        std::vector<ShaderRegister> registers(count);
        for (auto& reg : registers) {
            reg.offset = U32();
            reg.value = U32();
        }
        return registers;
    }
    void Finish() const { RequireCapture(cursor == bytes.size(), "record has trailing bytes"); }

private:
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;
};

template<typename TRecord>
constexpr std::uint32_t KindOf() {
    using Records = FrameCaptureRecord;
    std::uint32_t kind = 0;
    [&]<std::size_t... TIndex>(std::index_sequence<TIndex...>) {
        ((std::is_same_v<std::variant_alternative_t<TIndex, Records>, TRecord> ? (kind = static_cast<std::uint32_t>(TIndex) + 1) : 0), ...);
    }(std::make_index_sequence<std::variant_size_v<Records>>());
    return kind;
}

std::vector<std::byte> Encode(const FrameCaptureRecord& record) {
    Encoder encoder;
    std::visit([&](const auto& value) {
        using TRecord = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<TRecord, FrameCaptureShader>) {
            encoder.U64(value.headerAddress);
            encoder.U64(value.codeAddress);
            encoder.Bytes(value.header);
            encoder.Bytes(value.code);
        } else if constexpr (std::is_same_v<TRecord, FrameCaptureShaderAbi>) {
            encoder.U64(value.shader);
            encoder.Registers(value.context);
            encoder.Registers(value.primitive);
        } else if constexpr (std::is_same_v<TRecord, FrameCaptureGraphicsStagesAbi>) {
            encoder.U64(value.stages.size());
            for (const auto stage : value.stages) encoder.U64(stage);
            encoder.Registers(value.context);
            encoder.Registers(value.primitive);
        } else if constexpr (std::is_same_v<TRecord, FrameCaptureGraphicsAbi>) {
            encoder.U64(value.vertex);
            encoder.U64(value.pixel);
            encoder.U32(value.primitiveType);
        } else if constexpr (std::is_same_v<TRecord, FrameCaptureRanges>) {
            encoder.U64(value.ranges.size());
            for (const auto& range : value.ranges) {
                encoder.U64(range.address);
                encoder.U64(range.bytes);
                encoder.U8(range.readable ? 1 : 0);
                encoder.U8(range.writable ? 1 : 0);
                encoder.U8(range.gpu ? 1 : 0);
                encoder.U64(range.committed.size());
                for (const auto& [begin, end] : range.committed) {
                    encoder.U64(begin);
                    encoder.U64(end);
                }
            }
        } else if constexpr (std::is_same_v<TRecord, FrameCapturePages>) {
            encoder.U8(value.image ? 1 : 0);
            encoder.U64(value.pages.size());
            for (const auto& page : value.pages) {
                encoder.U64(page.address);
                encoder.U32(page.pool);
            }
        } else if constexpr (std::is_same_v<TRecord, FrameCaptureSubmit>) {
            encoder.U32(value.queue);
            encoder.U64(value.address);
            encoder.U32(value.dwords);
            encoder.U8(value.flags);
        } else if constexpr (std::is_same_v<TRecord, FrameCapturePresent>) {
            encoder.U8(value.buffer ? 1 : 0);
            encoder.U8(value.opaque ? 1 : 0);
            encoder.U64(value.display.address);
            encoder.U64(value.display.pixelFormat);
            encoder.U32(value.display.width);
            encoder.U32(value.display.height);
            encoder.U32(value.display.tilingMode);
            encoder.U32(value.display.pitchInPixel);
            encoder.U64(value.display.dccAddress);
            encoder.U64(value.display.dccClearColor);
        } else if constexpr (std::is_same_v<TRecord, FrameCaptureEnd>) {
            encoder.U64(value.presents);
            encoder.U64(value.submits);
            encoder.U64(value.imagePages);
            encoder.U64(value.deltaPages);
            encoder.U64(value.poolPages);
        }
    }, record);
    return std::move(encoder.bytes);
}

FrameCaptureRecord Decode(std::uint32_t kind, std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    FrameCaptureRecord record;
    switch (kind) {
    case KindOf<FrameCaptureShader>(): {
        FrameCaptureShader value;
        value.headerAddress = decoder.U64();
        value.codeAddress = decoder.U64();
        value.header = decoder.Bytes();
        value.code = decoder.Bytes();
        record = std::move(value);
        break;
    }
    case KindOf<FrameCaptureShaderAbi>(): {
        FrameCaptureShaderAbi value;
        value.shader = decoder.U64();
        value.context = decoder.Registers();
        value.primitive = decoder.Registers();
        record = std::move(value);
        break;
    }
    case KindOf<FrameCaptureGraphicsStagesAbi>(): {
        FrameCaptureGraphicsStagesAbi value;
        value.stages.resize(decoder.Count(8));
        for (auto& stage : value.stages) stage = decoder.U64();
        value.context = decoder.Registers();
        value.primitive = decoder.Registers();
        record = std::move(value);
        break;
    }
    case KindOf<FrameCaptureGraphicsAbi>(): {
        FrameCaptureGraphicsAbi value;
        value.vertex = decoder.U64();
        value.pixel = decoder.U64();
        value.primitiveType = decoder.U32();
        record = value;
        break;
    }
    case KindOf<FrameCaptureRanges>(): {
        FrameCaptureRanges value;
        value.ranges.resize(decoder.Count(27));
        for (auto& range : value.ranges) {
            range.address = decoder.U64();
            range.bytes = decoder.U64();
            range.readable = decoder.Flag();
            range.writable = decoder.Flag();
            range.gpu = decoder.Flag();
            range.committed.resize(decoder.Count(16));
            for (auto& [begin, end] : range.committed) {
                begin = decoder.U64();
                end = decoder.U64();
                RequireCapture(begin < end && begin >= range.address && end - range.address <= range.bytes, "committed span outside its range");
                RequireCapture(begin % FrameCaptureFormat::PageBytes == 0 && end % FrameCaptureFormat::PageBytes == 0, "committed span is not page aligned");
            }
        }
        record = std::move(value);
        break;
    }
    case KindOf<FrameCapturePages>(): {
        FrameCapturePages value;
        value.image = decoder.Flag();
        value.pages.resize(decoder.Count(12));
        for (auto& page : value.pages) {
            page.address = decoder.U64();
            page.pool = decoder.U32();
            RequireCapture(page.address % FrameCaptureFormat::PageBytes == 0, "page address is not page aligned");
        }
        record = std::move(value);
        break;
    }
    case KindOf<FrameCaptureSubmit>(): {
        FrameCaptureSubmit value;
        value.queue = decoder.U32();
        value.address = decoder.U64();
        value.dwords = decoder.U32();
        value.flags = decoder.U8();
        record = value;
        break;
    }
    case KindOf<FrameCaptureSuspend>():
        record = FrameCaptureSuspend{};
        break;
    case KindOf<FrameCapturePresent>(): {
        FrameCapturePresent value;
        value.buffer = decoder.Flag();
        value.opaque = decoder.Flag();
        value.display.address = decoder.U64();
        value.display.pixelFormat = decoder.U64();
        value.display.width = decoder.U32();
        value.display.height = decoder.U32();
        value.display.tilingMode = decoder.U32();
        value.display.pitchInPixel = decoder.U32();
        value.display.dccAddress = decoder.U64();
        value.display.dccClearColor = decoder.U64();
        record = value;
        break;
    }
    case KindOf<FrameCaptureEnd>(): {
        FrameCaptureEnd value;
        value.presents = decoder.U64();
        value.submits = decoder.U64();
        value.imagePages = decoder.U64();
        value.deltaPages = decoder.U64();
        value.poolPages = decoder.U64();
        record = value;
        break;
    }
    default:
        RequireCapture(false, "unknown record kind " + std::to_string(kind));
    }
    decoder.Finish();
    return record;
}

void PutU32(std::byte* out, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) out[index] = static_cast<std::byte>(value >> (8 * index));
}

void PutU64(std::byte* out, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) out[index] = static_cast<std::byte>(value >> (8 * index));
}

std::uint32_t GetU32(const std::byte* in) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) value |= std::to_integer<std::uint32_t>(in[index]) << (8 * index);
    return value;
}

std::uint64_t GetU64(const std::byte* in) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) value |= std::to_integer<std::uint64_t>(in[index]) << (8 * index);
    return value;
}

std::vector<std::byte> Inflate(std::span<const std::byte> stored, std::uint64_t rawBytes) {
    std::vector<std::byte> raw(static_cast<std::size_t>(rawBytes));
    z_stream stream{};
    RequireCapture(inflateInit(&stream) == Z_OK, "inflateInit failed");
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(stored.data()));
    stream.avail_in = static_cast<uInt>(stored.size());
    stream.next_out = reinterpret_cast<Bytef*>(raw.data());
    stream.avail_out = static_cast<uInt>(raw.size());
    const auto status = inflate(&stream, Z_FINISH);
    const bool complete = status == Z_STREAM_END && stream.avail_out == 0 && stream.avail_in == 0;
    inflateEnd(&stream);
    RequireCapture(complete, "corrupt compressed chunk");
    return raw;
}

}

FrameCaptureWriter::FrameCaptureWriter(const std::filesystem::path& path) {
    RequireCapture(!std::filesystem::exists(path), path.string() + " already exists");
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(path, std::ios::binary | std::ios::trunc);
    std::array<std::byte, 16> header{};
    std::memcpy(header.data(), FrameCaptureFormat::Magic.data(), FrameCaptureFormat::Magic.size());
    PutU32(header.data() + 8, FrameCaptureFormat::Version);
    PutU32(header.data() + 12, static_cast<std::uint32_t>(FrameCaptureFormat::PageBytes));
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    fileBytes = header.size();
}

std::uint32_t FrameCaptureWriter::AddPage(FrameCapturePageBytes page) {
    RequireCapture(!finished, "page added after the capture finished");
    bool zero = false;
    const auto hash = HashPage(page.data(), zero);
    if (zero) return FrameCaptureFormat::ZeroPage;
    const auto [found, inserted] = pool.try_emplace(HashKey{hash[0], hash[1]}, static_cast<std::uint32_t>(poolPages));
    if (!inserted) return found->second;
    RequireCapture(poolPages < FrameCaptureFormat::ZeroPage, "page pool is full");
    block.insert(block.end(), page.begin(), page.end());
    ++poolPages;
    if (block.size() == FrameCaptureFormat::BlockPages * FrameCaptureFormat::PageBytes) flushBlock();
    return found->second;
}

void FrameCaptureWriter::Write(const FrameCaptureRecord& record) {
    RequireCapture(!finished, "record written after the capture finished");
    flushBlock();
    const auto bytes = Encode(record);
    writeChunk(static_cast<std::uint32_t>(record.index()) + 1, bytes);
}

void FrameCaptureWriter::Finish() {
    if (finished) return;
    flushBlock();
    output.close();
    finished = true;
}

void FrameCaptureWriter::flushBlock() {
    if (block.empty()) return;
    writeChunk(PageBlockKind, block);
    block.clear();
}

void FrameCaptureWriter::writeChunk(std::uint32_t kind, std::span<const std::byte> raw) {
    RequireCapture(raw.size() <= FrameCaptureFormat::MaxChunkBytes, "record exceeds 256 MiB");
    std::vector<std::byte> stored(compressBound(static_cast<uLong>(raw.size())));
    auto length = static_cast<uLongf>(stored.size());
    RequireCapture(compress2(reinterpret_cast<Bytef*>(stored.data()), &length, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), Z_BEST_SPEED) == Z_OK, "compression failed");
    const bool deflated = length < raw.size();
    const auto payload = deflated ? std::span<const std::byte>(stored.data(), length) : raw;
    std::array<std::byte, ChunkHeaderBytes> header{};
    PutU32(header.data(), kind);
    PutU32(header.data() + 4, deflated ? DeflatedFlag : 0);
    PutU64(header.data() + 8, raw.size());
    PutU64(header.data() + 16, payload.size());
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    fileBytes += header.size() + payload.size();
}

FrameCaptureReader::FrameCaptureReader(const std::filesystem::path& path) {
    input.open(path, std::ios::binary);
    RequireCapture(input.is_open(), "cannot open " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(input.tellg());
    input.seekg(0);
    std::array<std::byte, 16> header{};
    RequireCapture(size >= header.size() && input.read(reinterpret_cast<char*>(header.data()), header.size()), "file is too short");
    RequireCapture(std::memcmp(header.data(), FrameCaptureFormat::Magic.data(), FrameCaptureFormat::Magic.size()) == 0, "not a frame capture");
    RequireCapture(GetU32(header.data() + 8) == FrameCaptureFormat::Version, "unsupported version " + std::to_string(GetU32(header.data() + 8)));
    RequireCapture(GetU32(header.data() + 12) == FrameCaptureFormat::PageBytes, "unsupported page size");
    std::uint64_t offset = header.size();
    bool ended = false;
    while (offset < size) {
        RequireCapture(!ended, "data after the end record");
        std::array<std::byte, ChunkHeaderBytes> chunk{};
        RequireCapture(size - offset >= chunk.size() && input.read(reinterpret_cast<char*>(chunk.data()), chunk.size()), "truncated chunk header");
        const auto kind = GetU32(chunk.data());
        const auto flags = GetU32(chunk.data() + 4);
        const auto rawBytes = GetU64(chunk.data() + 8);
        const auto storedBytes = GetU64(chunk.data() + 16);
        offset += chunk.size();
        RequireCapture((flags & ~DeflatedFlag) == 0, "unknown chunk flags");
        RequireCapture(rawBytes <= FrameCaptureFormat::MaxChunkBytes && storedBytes <= size - offset, "truncated chunk");
        const bool deflated = (flags & DeflatedFlag) != 0;
        RequireCapture(deflated || storedBytes == rawBytes, "stored chunk size mismatch");
        if (kind == PageBlockKind) {
            const auto count = rawBytes / FrameCaptureFormat::PageBytes;
            RequireCapture(count != 0 && count <= FrameCaptureFormat::BlockPages && rawBytes % FrameCaptureFormat::PageBytes == 0, "malformed page block");
            blocks.push_back({static_cast<std::uint32_t>(poolPages), static_cast<std::uint32_t>(count), offset, storedBytes, rawBytes, deflated});
            poolPages += count;
            input.seekg(static_cast<std::streamoff>(offset + storedBytes));
        } else {
            std::vector<std::byte> stored(static_cast<std::size_t>(storedBytes));
            RequireCapture(static_cast<bool>(input.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()))), "truncated record");
            auto record = Decode(kind, deflated ? Inflate(stored, rawBytes) : stored);
            if (const auto* pages = std::get_if<FrameCapturePages>(&record)) {
                for (const auto& page : pages->pages) RequireCapture(page.pool == FrameCaptureFormat::ZeroPage || page.pool < poolPages, "page refers to a pool page that is not written before it");
            }
            ended = std::holds_alternative<FrameCaptureEnd>(record);
            records.push_back(std::move(record));
        }
        offset += storedBytes;
    }
    RequireCapture(ended, "capture is incomplete (no end record)");
    const auto& end = std::get<FrameCaptureEnd>(records.back());
    RequireCapture(end.poolPages == poolPages, "end record disagrees with the page pool");
}

void FrameCaptureReader::ReadPage(std::uint32_t index, std::span<std::byte, FrameCaptureFormat::PageBytes> destination) {
    if (index == FrameCaptureFormat::ZeroPage) {
        std::memset(destination.data(), 0, destination.size());
        return;
    }
    RequireCapture(index < poolPages, "pool page out of range");
    const auto found = std::upper_bound(blocks.begin(), blocks.end(), index, [](std::uint32_t value, const Block& block) { return value < block.first; });
    const auto blockIndex = static_cast<std::size_t>(found - blocks.begin()) - 1;
    const auto& block = blocks[blockIndex];
    if (cachedBlock != blockIndex) {
        input.clear();
        input.seekg(static_cast<std::streamoff>(block.offset));
        std::vector<std::byte> stored(static_cast<std::size_t>(block.storedBytes));
        RequireCapture(static_cast<bool>(input.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()))), "truncated page block");
        cache = block.deflated ? Inflate(stored, block.rawBytes) : std::move(stored);
        cachedBlock = blockIndex;
    }
    std::memcpy(destination.data(), cache.data() + std::size_t{index - block.first} * FrameCaptureFormat::PageBytes, destination.size());
}

std::vector<FrameCaptureMemory::PageState> FrameCaptureMemory::list(const std::vector<FrameCaptureRange>& ranges) {
    std::vector<PageState> result;
    for (const auto& range : ranges) {
        for (const auto& [begin, end] : range.committed) {
            RequireCapture(begin % FrameCaptureFormat::PageBytes == 0 && end % FrameCaptureFormat::PageBytes == 0, "committed span is not page aligned");
            for (auto page = begin; page < end; page += FrameCaptureFormat::PageBytes) result.push_back({page, {}, false});
        }
    }
    std::sort(result.begin(), result.end(), [](const PageState& left, const PageState& right) { return left.address < right.address; });
    RequireCapture(std::adjacent_find(result.begin(), result.end(), [](const PageState& left, const PageState& right) { return left.address == right.address; }) == result.end(), "ranges overlap");
    return result;
}

void FrameCaptureMemory::hash(std::vector<PageState>& pages, const std::vector<std::size_t>& indices) {
    const auto hashRange = [&](std::size_t from, std::size_t to) {
        for (auto index = from; index < to; ++index) {
            auto& page = pages[indices[index]];
            page.hash = HashPage(reinterpret_cast<const std::byte*>(page.address), page.zero);
        }
    };
    const auto workers = indices.size() < 4096 ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (workers == 1) {
        hashRange(0, indices.size());
        return;
    }
    std::vector<std::thread> threads;
    const auto share = (indices.size() + workers - 1) / workers;
    for (unsigned worker = 0; worker < workers; ++worker) {
        const auto from = std::min(indices.size(), worker * share);
        const auto to = std::min(indices.size(), from + share);
        threads.emplace_back(hashRange, from, to);
    }
    for (auto& thread : threads) thread.join();
}

FrameCapturePages FrameCaptureMemory::Image(const std::vector<FrameCaptureRange>& ranges) {
    RequireCapture(pages.empty() && imagePages == 0, "memory image taken twice");
    pages = list(ranges);
    std::vector<std::size_t> all(pages.size());
    for (std::size_t index = 0; index < all.size(); ++index) all[index] = index;
    hash(pages, all);
    FrameCapturePages image{true, {}};
    for (const auto& page : pages) {
        if (page.zero) continue;
        image.pages.push_back({page.address, writer.AddPage(FrameCapturePageBytes(reinterpret_cast<const std::byte*>(page.address), FrameCaptureFormat::PageBytes))});
    }
    imagePages = image.pages.size();
    return image;
}

FrameCapturePages FrameCaptureMemory::Changes(const std::vector<FrameCaptureRange>& ranges, const FrameCaptureSpans& written) {
    auto current = list(ranges);
    auto spans = written;
    std::sort(spans.begin(), spans.end());
    std::vector<std::size_t> stale;
    std::vector<bool> known(current.size(), false);
    auto previous = pages.begin();
    auto span = spans.begin();
    for (std::size_t index = 0; index < current.size(); ++index) {
        auto& page = current[index];
        while (previous != pages.end() && previous->address < page.address) ++previous;
        while (span != spans.end() && span->second <= page.address) ++span;
        const bool touched = span != spans.end() && span->first < page.address + FrameCaptureFormat::PageBytes;
        if (previous != pages.end() && previous->address == page.address) {
            known[index] = true;
            page.hash = previous->hash;
            page.zero = previous->zero;
            if (!touched) continue;
        }
        stale.push_back(index);
    }
    std::vector<std::array<std::uint64_t, 2>> before(stale.size());
    for (std::size_t index = 0; index < stale.size(); ++index) before[index] = current[stale[index]].hash;
    hash(current, stale);
    FrameCapturePages changes{false, {}};
    for (std::size_t index = 0; index < stale.size(); ++index) {
        const auto& page = current[stale[index]];
        if (known[stale[index]] ? before[index] == page.hash : page.zero) continue;
        const auto pool = page.zero ? FrameCaptureFormat::ZeroPage : writer.AddPage(FrameCapturePageBytes(reinterpret_cast<const std::byte*>(page.address), FrameCaptureFormat::PageBytes));
        changes.pages.push_back({page.address, pool});
    }
    pages = std::move(current);
    deltaPages += changes.pages.size();
    return changes;
}

FrameCaptureWindow FrameCaptureWindow::Parse(std::string_view text) {
    const auto separator = text.find(':');
    RequireCapture(separator != std::string_view::npos, "the frame window must be <start>:<count>");
    FrameCaptureWindow window;
    const auto number = [](std::string_view digits, std::uint64_t& value) {
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        return !digits.empty() && error == std::errc{} && end == digits.data() + digits.size();
    };
    RequireCapture(number(text.substr(0, separator), window.start) && number(text.substr(separator + 1), window.count), "the frame window must be <start>:<count> in whole numbers");
    RequireCapture(window.count >= 1 && window.count <= MaxPresents, "the frame count must be 1 through " + std::to_string(MaxPresents));
    RequireCapture(window.start <= std::numeric_limits<std::uint64_t>::max() - window.count, "the frame window overflows");
    return window;
}

FrameCaptureRecorder::FrameCaptureRecorder(const std::filesystem::path& path, FrameCaptureWindow window, RangeSource ranges, WriteSource writes) : writer(path), memory(writer), window(window), ranges(std::move(ranges)), writes(std::move(writes)) {}

void FrameCaptureRecorder::Preparation(FrameCaptureRecord record) {
    std::lock_guard lock(mutex);
    if (finished) return;
    writer.Write(record);
}

void FrameCaptureRecorder::Submit(const FrameCaptureSubmit& submit, const std::function<void()>& forward) {
    {
        std::lock_guard lock(mutex);
        if (opened && !finished) {
            recordChanges();
            const auto begin = submit.address;
            const auto end = begin + std::uint64_t{submit.dwords} * sizeof(std::uint32_t);
            const bool covered = std::any_of(currentRanges.begin(), currentRanges.end(), [&](const FrameCaptureRange& range) {
                return std::any_of(range.committed.begin(), range.committed.end(), [&](const auto& span) { return span.first <= begin && end <= span.second; });
            });
            char address[32];
            std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(begin));
            RequireCapture(covered, std::string("the command buffer at ") + address + " is outside the captured GPU allocations");
            writer.Write(submit);
            ++recordedSubmits;
        }
    }
    forward();
}

void FrameCaptureRecorder::Suspend(const std::function<void()>& forward) {
    {
        std::lock_guard lock(mutex);
        if (opened && !finished) {
            recordChanges();
            writer.Write(FrameCaptureSuspend{});
        }
    }
    forward();
}

void FrameCaptureRecorder::Present(const FrameCapturePresent& present, const std::function<void()>& forward) {
    {
        std::lock_guard lock(mutex);
        if (opened && !finished) {
            recordChanges();
            writer.Write(present);
            if (++recordedPresents == window.count) finish();
        }
    }
    forward();
    std::lock_guard lock(mutex);
    if (!opened && ++presents == window.start + 1) open();
}

bool FrameCaptureRecorder::Open() const {
    std::lock_guard lock(mutex);
    return opened && !finished;
}

bool FrameCaptureRecorder::Finished() const {
    std::lock_guard lock(mutex);
    return finished;
}

void FrameCaptureRecorder::recordChanges() {
    auto next = ranges();
    if (next != currentRanges) {
        currentRanges = std::move(next);
        writer.Write(FrameCaptureRanges{currentRanges});
    }
    auto changes = memory.Changes(currentRanges, writes(currentRanges));
    if (!changes.pages.empty()) writer.Write(changes);
}

void FrameCaptureRecorder::open() {
    currentRanges = ranges();
    writes(currentRanges);
    writer.Write(FrameCaptureRanges{currentRanges});
    std::uint64_t committed = 0;
    for (const auto& range : currentRanges) {
        for (const auto& [begin, end] : range.committed) committed += end - begin;
    }
    writer.Write(memory.Image(currentRanges));
    opened = true;
    std::fprintf(stderr, "[frame-capture] opened after present %llu: %zu ranges, %.1f MiB committed, %llu non-zero pages, %llu distinct\n", static_cast<unsigned long long>(window.start), currentRanges.size(), static_cast<double>(committed) / 1048576.0, static_cast<unsigned long long>(memory.ImagePages()), static_cast<unsigned long long>(writer.PoolPages()));
}

void FrameCaptureRecorder::finish() {
    writer.Write(FrameCaptureEnd{recordedPresents, recordedSubmits, memory.ImagePages(), memory.DeltaPages(), writer.PoolPages()});
    writer.Finish();
    finished = true;
    std::fprintf(stderr, "[frame-capture] finished: %llu presents, %llu submissions, %llu page changes, %llu distinct pages, %.1f MiB file\n", static_cast<unsigned long long>(recordedPresents), static_cast<unsigned long long>(recordedSubmits), static_cast<unsigned long long>(memory.DeltaPages()), static_cast<unsigned long long>(writer.PoolPages()), static_cast<double>(writer.FileBytes()) / 1048576.0);
}

}
