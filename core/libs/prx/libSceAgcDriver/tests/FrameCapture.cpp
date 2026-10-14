#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::FrameCaptureFormat;
using AgcDriver::FrameCaptureRange;
using AgcDriver::FrameCaptureReader;
using AgcDriver::FrameCaptureRecord;
using AgcDriver::FrameCaptureRecorder;
using AgcDriver::FrameCaptureWindow;
using AgcDriver::FrameCaptureWriter;
using Page = std::array<std::byte, FrameCaptureFormat::PageBytes>;
using Memory = std::map<std::uint64_t, Page>;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void Reject(TAction action, const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, message);
}

class Directory {
public:
    Directory() {
        path = std::filesystem::temp_directory_path() / ("anyps5-frame-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(path), "could not create test directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

class Region {
public:
    explicit Region(std::size_t pages) : storage((pages + 1) * FrameCaptureFormat::PageBytes) {
        const auto raw = reinterpret_cast<std::uintptr_t>(storage.data());
        base = reinterpret_cast<std::byte*>((raw + FrameCaptureFormat::PageBytes - 1) & ~std::uintptr_t{FrameCaptureFormat::PageBytes - 1});
        bytes = pages * FrameCaptureFormat::PageBytes;
    }
    std::byte* Page(std::size_t index) { return base + index * FrameCaptureFormat::PageBytes; }
    void Fill(std::size_t index, std::uint32_t seed) {
        for (std::size_t offset = 0; offset < FrameCaptureFormat::PageBytes; offset += 4) {
            const auto value = seed * 2654435761u + static_cast<std::uint32_t>(offset);
            std::memcpy(Page(index) + offset, &value, 4);
        }
    }
    void Zero(std::size_t index) { std::memset(Page(index), 0, FrameCaptureFormat::PageBytes); }
    FrameCaptureRange Range() const {
        const auto address = reinterpret_cast<std::uint64_t>(base);
        return {address, bytes, true, true, true, {{address, address + bytes}}};
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(base); }

private:
    std::vector<std::byte> storage;
    std::byte* base = nullptr;
    std::size_t bytes = 0;
};

Memory Snapshot(const std::vector<FrameCaptureRange>& ranges) {
    Memory memory;
    for (const auto& range : ranges) {
        for (const auto& [begin, end] : range.committed) {
            for (auto page = begin; page < end; page += FrameCaptureFormat::PageBytes) std::memcpy(memory[page].data(), reinterpret_cast<const void*>(page), FrameCaptureFormat::PageBytes);
        }
    }
    return memory;
}

void CheckWindow() {
    const auto window = FrameCaptureWindow::Parse("1150:8");
    Require(window.start == 1150 && window.count == 8, "window parse");
    for (const auto* text : {"", "8", ":8", "8:", "a:8", "8:b", "8:0", "8:257", "8:2x", "-1:2", "1:-2", "18446744073709551615:1"}) Reject([&] { FrameCaptureWindow::Parse(text); }, std::string("window accepted ") + text);
}

void CheckRoundTrip() {
    Directory directory;
    const auto path = directory.path / "capture.aps5fc";
    Page first{};
    Page second{};
    Page zero{};
    first.fill(std::byte{0x5a});
    second[17] = std::byte{1};
    std::uint32_t firstIndex = 0;
    std::uint32_t secondIndex = 0;
    {
        FrameCaptureWriter writer(path);
        Require(writer.AddPage(zero) == FrameCaptureFormat::ZeroPage, "zero page entered the pool");
        firstIndex = writer.AddPage(first);
        secondIndex = writer.AddPage(second);
        Require(writer.AddPage(first) == firstIndex && writer.PoolPages() == 2, "identical pages were not shared");
        writer.Write(AgcDriver::FrameCaptureShader{0x1000, 0x2000, {std::byte{1}, std::byte{2}}, {std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}}});
        writer.Write(AgcDriver::FrameCaptureShaderAbi{0x1000, {{0x10, 0x20}}, {{0x242, 4}}});
        writer.Write(AgcDriver::FrameCaptureGraphicsStagesAbi{{0x1000, 0x3000}, {{0x11, 0x21}}, {}});
        writer.Write(AgcDriver::FrameCaptureGraphicsAbi{0x1000, 0x3000, 7});
        writer.Write(AgcDriver::FrameCaptureRanges{{{0x10000, 0x3000, true, false, true, {{0x10000, 0x12000}}}}});
        writer.Write(AgcDriver::FrameCapturePages{true, {{0x10000, firstIndex}, {0x11000, secondIndex}}});
        writer.Write(AgcDriver::FrameCaptureSubmit{2, 0x10040, 16, 1});
        writer.Write(AgcDriver::FrameCaptureSuspend{});
        writer.Write(AgcDriver::FrameCapturePages{false, {{0x11000, FrameCaptureFormat::ZeroPage}}});
        writer.Write(AgcDriver::FrameCapturePresent{true, true, {0x40000, 3, 1920, 1080, 1, 1920, 0x50000, 9}});
        writer.Write(AgcDriver::FrameCaptureEnd{1, 1, 2, 1, writer.PoolPages()});
        writer.Finish();
    }
    FrameCaptureReader reader(path);
    const auto& records = reader.Records();
    Require(records.size() == 11 && reader.PoolPages() == 2, "record count");
    const auto& shader = std::get<AgcDriver::FrameCaptureShader>(records[0]);
    Require(shader.headerAddress == 0x1000 && shader.codeAddress == 0x2000 && shader.header.size() == 2 && shader.code.size() == 4 && shader.code[3] == std::byte{6}, "shader record");
    const auto& abi = std::get<AgcDriver::FrameCaptureShaderAbi>(records[1]);
    Require(abi.shader == 0x1000 && abi.context.size() == 1 && abi.context[0].value == 0x20 && abi.primitive[0].offset == 0x242, "shader ABI record");
    const auto& stages = std::get<AgcDriver::FrameCaptureGraphicsStagesAbi>(records[2]);
    Require(stages.stages.size() == 2 && stages.stages[1] == 0x3000 && stages.primitive.empty(), "graphics stages record");
    Require(std::get<AgcDriver::FrameCaptureGraphicsAbi>(records[3]).primitiveType == 7, "graphics ABI record");
    const auto& ranges = std::get<AgcDriver::FrameCaptureRanges>(records[4]).ranges;
    Require(ranges.size() == 1 && ranges[0].gpu && !ranges[0].writable && ranges[0].committed[0].second == 0x12000, "ranges record");
    const auto& image = std::get<AgcDriver::FrameCapturePages>(records[5]);
    Require(image.image && image.pages.size() == 2 && image.pages[1].pool == secondIndex, "image record");
    const auto& submit = std::get<AgcDriver::FrameCaptureSubmit>(records[6]);
    Require(submit.queue == 2 && submit.address == 0x10040 && submit.dwords == 16 && submit.flags == 1, "submit record");
    Require(std::holds_alternative<AgcDriver::FrameCaptureSuspend>(records[7]), "suspend record");
    Require(std::get<AgcDriver::FrameCapturePages>(records[8]).pages[0].pool == FrameCaptureFormat::ZeroPage, "delta record");
    const auto& present = std::get<AgcDriver::FrameCapturePresent>(records[9]);
    Require(present.buffer && present.display.width == 1920 && present.display.dccAddress == 0x50000 && present.display.dccClearColor == 9, "present record");
    Page read{};
    reader.ReadPage(secondIndex, read);
    Require(read == second, "pool page content");
    reader.ReadPage(firstIndex, read);
    Require(read == first, "shared pool page content");
    reader.ReadPage(FrameCaptureFormat::ZeroPage, read);
    Require(read == zero, "zero page content");
    Reject([&] { reader.ReadPage(2, read); }, "pool page out of range accepted");
    Reject([&] { FrameCaptureWriter again(path); }, "an existing capture was overwritten");

    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto rewrite = [&](const std::string& name, std::vector<char> content) {
        const auto changed = directory.path / name;
        std::ofstream output(changed, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        return changed;
    };
    auto magic = bytes;
    magic[0] = 'X';
    Reject([&] { FrameCaptureReader bad(rewrite("magic.aps5fc", magic)); }, "wrong magic accepted");
    auto version = bytes;
    version[8] = 2;
    Reject([&] { FrameCaptureReader bad(rewrite("version.aps5fc", version)); }, "unknown version accepted");
    auto truncated = bytes;
    truncated.resize(truncated.size() - 3);
    Reject([&] { FrameCaptureReader bad(rewrite("truncated.aps5fc", truncated)); }, "truncated capture accepted");
}

void ApplyRanges(Memory& model, std::set<std::uint64_t>& committed, const std::vector<FrameCaptureRange>& ranges) {
    std::set<std::uint64_t> next;
    for (const auto& range : ranges) {
        for (const auto& [begin, end] : range.committed) {
            for (auto page = begin; page < end; page += FrameCaptureFormat::PageBytes) next.insert(page);
        }
    }
    for (const auto page : committed) {
        if (!next.contains(page)) model.erase(page);
    }
    for (const auto page : next) {
        if (!committed.contains(page)) model[page] = Page{};
    }
    committed = std::move(next);
}

void CheckRecorder() {
    Directory directory;
    const auto path = directory.path / "frames.aps5fc";
    Region large(4160);
    Region added(16);
    for (std::size_t index = 0; index < 600; ++index) large.Fill(index, static_cast<std::uint32_t>(index));
    for (std::size_t index = 600; index < 700; ++index) large.Fill(index, 7);
    for (std::size_t index = 0; index < 16; ++index) added.Fill(index, static_cast<std::uint32_t>(1000 + index));
    std::vector<FrameCaptureRange> ranges{large.Range()};
    std::vector<Memory> expected;
    AgcDriver::FrameCaptureSpans reported;
    Memory gpuOriginal;
    std::size_t forwarded = 0;
    const auto seen = [&] {
        auto memory = Snapshot(ranges);
        for (const auto& [address, page] : gpuOriginal) memory[address] = page;
        expected.push_back(std::move(memory));
        ++forwarded;
    };
    const auto cpuFill = [&](Region& region, std::size_t index, std::uint32_t seed) {
        region.Fill(index, seed);
        const auto address = reinterpret_cast<std::uint64_t>(region.Page(index));
        reported.emplace_back(address, address + FrameCaptureFormat::PageBytes);
    };
    const auto cpuZero = [&](Region& region, std::size_t index) {
        region.Zero(index);
        const auto address = reinterpret_cast<std::uint64_t>(region.Page(index));
        reported.emplace_back(address + 64, address + 128);
    };
    bool baseline = false;
    const auto writes = [&](const std::vector<FrameCaptureRange>&) {
        auto spans = std::move(reported);
        reported.clear();
        if (!baseline) spans.clear();
        baseline = true;
        return spans;
    };
    const auto none = [&] { ++forwarded; };
    {
        FrameCaptureRecorder recorder(path, FrameCaptureWindow{1, 2}, [&] { return ranges; }, writes);
        recorder.Preparation(AgcDriver::FrameCaptureShader{0x1000, 0x2000, {std::byte{9}}, {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}});
        recorder.Submit({0, large.Address(), 4, 0}, none);
        recorder.Present({false, true, {}}, none);
        Require(!recorder.Open(), "window opened at present 0");
        recorder.Present({false, true, {}}, none);
        Require(recorder.Open(), "window did not open after present 1");
        expected.push_back(Snapshot(ranges));

        cpuZero(large, 3);
        cpuFill(large, 4, 0);
        cpuFill(large, 5, 5000);
        cpuFill(large, 4000, 6000);
        std::memcpy(gpuOriginal[reinterpret_cast<std::uint64_t>(large.Page(10))].data(), large.Page(10), FrameCaptureFormat::PageBytes);
        large.Fill(10, 9999);
        reported.emplace_back(reinterpret_cast<std::uint64_t>(large.Page(11)), reinterpret_cast<std::uint64_t>(large.Page(12)));
        recorder.Submit({1, large.Address() + 64, 8, 0}, seen);
        Reject([&] { recorder.Submit({1, large.Address() - 4096, 8, 0}, none); }, "a command buffer outside the ranges was accepted");

        ranges.push_back(added.Range());
        cpuFill(large, 6, 6006);
        recorder.Submit({2, added.Address(), 16, 0}, seen);
        recorder.Suspend(none);

        ranges.pop_back();
        cpuFill(large, 7, 7007);
        recorder.Present({true, true, {large.Address(), 0, 64, 64, 0, 64, 0, 0}}, none);
        cpuZero(large, 0);
        recorder.Submit({0, large.Address(), 4, 0}, seen);
        recorder.Present({false, false, {}}, none);
        Require(recorder.Finished(), "window did not close after two presents");
        recorder.Submit({0, large.Address(), 4, 0}, none);
    }
    Require(forwarded == 10, "every call must reach the driver");

    FrameCaptureReader reader(path);
    Memory model;
    std::set<std::uint64_t> committed;
    std::size_t submits = 0;
    std::size_t presents = 0;
    bool imaged = false;
    for (const auto& record : reader.Records()) {
        if (const auto* changedRanges = std::get_if<AgcDriver::FrameCaptureRanges>(&record)) {
            ApplyRanges(model, committed, changedRanges->ranges);
        } else if (const auto* pages = std::get_if<AgcDriver::FrameCapturePages>(&record)) {
            Require(pages->image != imaged, "the memory image must come first and once");
            imaged = true;
            for (const auto& page : pages->pages) {
                Require(committed.contains(page.address), "page outside the committed ranges");
                reader.ReadPage(page.pool, model[page.address]);
            }
            if (pages->image) Require(model == expected[0], "memory image differs from the window start");
        } else if (std::holds_alternative<AgcDriver::FrameCaptureSubmit>(record)) {
            ++submits;
            Require(submits < expected.size() && model == expected[submits], "memory at submission " + std::to_string(submits) + " differs");
        } else if (std::holds_alternative<AgcDriver::FrameCapturePresent>(record)) {
            ++presents;
        }
    }
    Require(std::holds_alternative<AgcDriver::FrameCaptureShader>(reader.Records().front()), "preparation before the window was not recorded");
    Require(submits == 3 && presents == 2, "recorded call counts");
    const auto& end = std::get<AgcDriver::FrameCaptureEnd>(reader.Records().back());
    Require(end.presents == 2 && end.submits == 3, "end record counts");
    Require(end.deltaPages == 4 + 16 + 1 + 1 + 1, "only CPU-written pages and new commits are deltas");
    Require(end.imagePages == 700, "image must hold only the non-zero pages");
    Require(reader.PoolPages() == 600 + 2 + 16 + 2, "the pool must hold each distinct page once");
}

}

int main() {
    try {
        CheckWindow();
        CheckRoundTrip();
        CheckRecorder();
        std::puts("frame capture: window parsing, record round trip, malformed files and byte-exact memory at every recorded submission without GPU writes passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "frame capture test: %s\n", error.what());
        return 1;
    }
}
