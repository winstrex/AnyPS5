#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_FRAMECAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_FRAMECAPTURE_HPP

#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace AgcDriver {

struct FrameCaptureRange {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    bool readable = false;
    bool writable = false;
    bool gpu = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> committed;

    bool operator==(const FrameCaptureRange&) const = default;
};

struct FrameCapturePage {
    std::uint64_t address = 0;
    std::uint32_t pool = 0;

    bool operator==(const FrameCapturePage&) const = default;
};

struct FrameCaptureShader {
    std::uint64_t headerAddress = 0;
    std::uint64_t codeAddress = 0;
    std::vector<std::byte> header;
    std::vector<std::byte> code;
};

struct FrameCaptureShaderAbi {
    std::uint64_t shader = 0;
    std::vector<ShaderRegister> context;
    std::vector<ShaderRegister> primitive;
};

struct FrameCaptureGraphicsStagesAbi {
    std::vector<std::uint64_t> stages;
    std::vector<ShaderRegister> context;
    std::vector<ShaderRegister> primitive;
};

struct FrameCaptureGraphicsAbi {
    std::uint64_t vertex = 0;
    std::uint64_t pixel = 0;
    std::uint32_t primitiveType = 0;
};

struct FrameCaptureRanges {
    std::vector<FrameCaptureRange> ranges;
};

struct FrameCapturePages {
    bool image = false;
    std::vector<FrameCapturePage> pages;
};

struct FrameCaptureSubmit {
    std::uint32_t queue = 0;
    std::uint64_t address = 0;
    std::uint32_t dwords = 0;
    std::uint8_t flags = 0;
};

struct FrameCaptureSuspend {};

struct FrameCapturePresent {
    bool buffer = false;
    bool opaque = false;
    DisplayBuffer display{};
};

struct FrameCaptureEnd {
    std::uint64_t presents = 0;
    std::uint64_t submits = 0;
    std::uint64_t imagePages = 0;
    std::uint64_t deltaPages = 0;
    std::uint64_t poolPages = 0;
};

using FrameCaptureRecord = std::variant<FrameCaptureShader, FrameCaptureShaderAbi, FrameCaptureGraphicsStagesAbi, FrameCaptureGraphicsAbi, FrameCaptureRanges, FrameCapturePages, FrameCaptureSubmit, FrameCaptureSuspend, FrameCapturePresent, FrameCaptureEnd>;

struct FrameCaptureFormat {
    static constexpr std::string_view Magic = "APS5FCAP";
    static constexpr std::uint32_t Version = 1;
    static constexpr std::size_t PageBytes = 4096;
    static constexpr std::uint32_t ZeroPage = 0xffffffffu;
    static constexpr std::size_t BlockPages = 256;
    static constexpr std::uint64_t MaxChunkBytes = 256ull * 1024ull * 1024ull;
};

using FrameCapturePageBytes = std::span<const std::byte, FrameCaptureFormat::PageBytes>;

class FrameCaptureWriter {
public:
    explicit FrameCaptureWriter(const std::filesystem::path& path);
    FrameCaptureWriter(const FrameCaptureWriter&) = delete;
    FrameCaptureWriter& operator=(const FrameCaptureWriter&) = delete;

    std::uint32_t AddPage(FrameCapturePageBytes page);
    void Write(const FrameCaptureRecord& record);
    void Finish();
    std::uint64_t PoolPages() const { return poolPages; }
    std::uint64_t FileBytes() const { return fileBytes; }

private:
    struct HashKey {
        std::uint64_t low;
        std::uint64_t high;
        bool operator==(const HashKey&) const = default;
    };
    struct HashKeyHash {
        std::size_t operator()(const HashKey& key) const { return static_cast<std::size_t>(key.low ^ (key.high * 0x9e3779b97f4a7c15ull)); }
    };

    void flushBlock();
    void writeChunk(std::uint32_t kind, std::span<const std::byte> raw);

    std::ofstream output;
    std::unordered_map<HashKey, std::uint32_t, HashKeyHash> pool;
    std::vector<std::byte> block;
    std::uint64_t poolPages = 0;
    std::uint64_t fileBytes = 0;
    bool finished = false;
};

class FrameCaptureReader {
public:
    explicit FrameCaptureReader(const std::filesystem::path& path);

    const std::vector<FrameCaptureRecord>& Records() const { return records; }
    std::uint64_t PoolPages() const { return poolPages; }
    void ReadPage(std::uint32_t index, std::span<std::byte, FrameCaptureFormat::PageBytes> destination);

private:
    struct Block {
        std::uint32_t first;
        std::uint32_t count;
        std::uint64_t offset;
        std::uint64_t storedBytes;
        std::uint64_t rawBytes;
        bool deflated;
    };

    std::ifstream input;
    std::vector<FrameCaptureRecord> records;
    std::vector<Block> blocks;
    std::uint64_t poolPages = 0;
    std::size_t cachedBlock = static_cast<std::size_t>(-1);
    std::vector<std::byte> cache;
};

using FrameCaptureSpans = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

class FrameCaptureMemory {
public:
    explicit FrameCaptureMemory(FrameCaptureWriter& writer) : writer(writer) {}

    FrameCapturePages Image(const std::vector<FrameCaptureRange>& ranges);
    FrameCapturePages Changes(const std::vector<FrameCaptureRange>& ranges, const FrameCaptureSpans& written);
    std::uint64_t ImagePages() const { return imagePages; }
    std::uint64_t DeltaPages() const { return deltaPages; }

private:
    struct PageState {
        std::uint64_t address;
        std::array<std::uint64_t, 2> hash;
        bool zero;
    };

    static std::vector<PageState> list(const std::vector<FrameCaptureRange>& ranges);
    static void hash(std::vector<PageState>& pages, const std::vector<std::size_t>& indices);

    FrameCaptureWriter& writer;
    std::vector<PageState> pages;
    std::uint64_t imagePages = 0;
    std::uint64_t deltaPages = 0;
};

struct FrameCaptureWindow {
    static constexpr std::uint64_t MaxPresents = 256;
    std::uint64_t start = 0;
    std::uint64_t count = 0;

    static FrameCaptureWindow Parse(std::string_view text);
};

class FrameCaptureRecorder {
public:
    using RangeSource = std::function<std::vector<FrameCaptureRange>()>;
    using WriteSource = std::function<FrameCaptureSpans(const std::vector<FrameCaptureRange>&)>;

    FrameCaptureRecorder(const std::filesystem::path& path, FrameCaptureWindow window, RangeSource ranges, WriteSource writes);
    FrameCaptureRecorder(const FrameCaptureRecorder&) = delete;
    FrameCaptureRecorder& operator=(const FrameCaptureRecorder&) = delete;

    void Preparation(FrameCaptureRecord record);
    void Submit(const FrameCaptureSubmit& submit, const std::function<void()>& forward);
    void Suspend(const std::function<void()>& forward);
    void Present(const FrameCapturePresent& present, const std::function<void()>& forward);
    bool Open() const;
    bool Finished() const;

private:
    void recordChanges();
    void open();
    void finish();

    mutable std::mutex mutex;
    FrameCaptureWriter writer;
    FrameCaptureMemory memory;
    FrameCaptureWindow window;
    RangeSource ranges;
    WriteSource writes;
    std::vector<FrameCaptureRange> currentRanges;
    std::uint64_t presents = 0;
    std::uint64_t recordedPresents = 0;
    std::uint64_t recordedSubmits = 0;
    bool opened = false;
    bool finished = false;
};

}

#endif
