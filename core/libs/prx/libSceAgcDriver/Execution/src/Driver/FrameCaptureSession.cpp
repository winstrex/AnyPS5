#include "prx/libSceAgcDriver/Execution/include/Driver/FrameCaptureSession.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace AgcDriver::DriverDetail {
namespace {

std::vector<FrameCaptureRange> GpuRanges() {
    std::vector<FrameCaptureRange> result;
    const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    for (const auto& range : lease) {
        if (!range->gpu || !range->readable) continue;
        FrameCaptureRange captured{range->address, range->bytes, range->readable, range->writable, range->gpu, {}};
        for (const auto& [begin, end] : GuestMemory::CommittedRanges(range->address, range->bytes)) {
            const auto first = (begin + FrameCaptureFormat::PageBytes - 1) & ~std::uint64_t{FrameCaptureFormat::PageBytes - 1};
            const auto last = end & ~std::uint64_t{FrameCaptureFormat::PageBytes - 1};
            if (first < last) captured.committed.emplace_back(first, last);
        }
        result.push_back(std::move(captured));
    }
    return result;
}

class CpuWrites {
public:
    FrameCaptureSpans operator()(const std::vector<FrameCaptureRange>& ranges) {
        for (const auto& range : ranges) {
            for (const auto& [begin, end] : range.committed) GuestMemory::CollectWritesUncached(begin, end - begin);
        }
        const auto generation = GuestMemory::TrackerGeneration();
        FrameCaptureSpans written;
        if (since != 0) {
            for (const auto& range : ranges) {
                for (const auto& [begin, end] : range.committed) {
                    const auto first = begin & ~(BlockBytes - 1);
                    const auto count = static_cast<std::size_t>((end - first + BlockBytes - 1) / BlockBytes);
                    std::vector<std::uint64_t> generations(count, since);
                    std::vector<std::uint8_t> changed(count);
                    std::vector<std::uint8_t> cpu(count);
                    GuestMemory::ChangedBlocks(begin, end - begin, generations, changed, cpu);
                    for (std::size_t block = 0; block < count; ++block) {
                        if (cpu[block] == GuestMemory::BlockUnchanged) continue;
                        written.emplace_back(std::max(begin, first + block * BlockBytes), std::min(end, first + (block + 1) * BlockBytes));
                    }
                }
            }
        }
        since = generation;
        return written;
    }

private:
    static constexpr std::uint64_t BlockBytes = 65536;
    std::uint64_t since = 0;
};

std::unique_ptr<FrameCaptureRecorder> StartFrameCapture() {
    const char* path = std::getenv("APS5_FRAME_CAPTURE");
    if (path == nullptr) return nullptr;
    const char* frames = std::getenv("APS5_FRAME_CAPTURE_FRAMES");
    if (frames == nullptr) throw std::runtime_error("frame capture: APS5_FRAME_CAPTURE needs APS5_FRAME_CAPTURE_FRAMES=<start>:<count>");
    const auto window = FrameCaptureWindow::Parse(frames);
    if (!GuestMemory::WriteWatched()) throw std::runtime_error("frame capture: guest memory is not write-watched, so CPU writes cannot be told from GPU writes");
    return std::make_unique<FrameCaptureRecorder>(path, window, GpuRanges, CpuWrites{});
}

}

FrameCaptureRecorder* ActiveFrameCapture() {
    static const auto recorder = StartFrameCapture();
    return recorder.get();
}

FrameCaptureShader CapturedShader(const Shader* shader) {
    Shader fields;
    std::memcpy(&fields, static_cast<const void*>(shader), sizeof(Shader));
    const auto* header = reinterpret_cast<const std::byte*>(shader);
    const auto* code = static_cast<const std::byte*>(const_cast<const void*>(fields.code));
    return {reinterpret_cast<std::uint64_t>(shader), reinterpret_cast<std::uint64_t>(code), {header, header + fields.header_size}, {code, code + fields.shader_size}};
}

}
