#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::size_t BlockBytes = 256 * 1024;
constexpr std::uint32_t CounterOffset = 0x10;
constexpr std::uint32_t CounterBytes = 16;
constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Results = 4;

constexpr std::array<std::uint32_t, 25> AppendProgram{
    0x34060082,
    0x7e0c02ff, 0x5eed0001u, 0x7e0e02ff, 0x5eed0002u,
    0xbefc03ff, 0x0000ffff,
    0xd8fa0010, 0x04000000,
    0xd8f60014, 0x05000000,
    0xbf800000, 0xbf800000,
    0xd8fa0018, 0x06000000,
    0xd8f6001c, 0x07000000,
    0xbf8cc07f,
    0xe0702000, 0x80000403, 0xe0702004, 0x80000503, 0xe0702008, 0x80000603,
    0xbf810000,
};

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("GDS queued dispatch order: " + what);
}

std::uint8_t* GuestBlock() {
#ifdef _WIN32
    auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, BlockBytes));
#endif
    check(block != nullptr, "cannot allocate a guest block");
    std::memset(block, 0, BlockBytes);
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    return block;
}

std::uint64_t Address(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::array<std::uint32_t, 7> FillGds(std::uint32_t value) {
    return {0xc0055000u, (2u << 29u) | (1u << 20u), value, 0u, CounterOffset, 0u, CounterBytes};
}

std::array<std::uint32_t, 7> CopyGdsTo(std::uint64_t destination) {
    return {0xc0055000u, 1u << 29u, CounterOffset, 0u, static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u), CounterBytes};
}

template <std::size_t Count>
void Append(std::vector<std::uint32_t>& words, const std::array<std::uint32_t, Count>& packet) {
    words.insert(words.end(), packet.begin(), packet.end());
}

std::array<std::uint32_t, 3> SetShaderRegister(std::uint32_t offset, std::uint32_t value) {
    return {0xc0017600u, offset, value};
}

void Submit(std::vector<std::uint32_t> words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

void AppendDispatch(std::vector<std::uint32_t>& words, const void* code, const void* output) {
    const auto program = Address(code);
    const auto out = Address(output);
    Append(words, SetShaderRegister(0x20c, static_cast<std::uint32_t>(program >> 8u)));
    Append(words, SetShaderRegister(0x20d, static_cast<std::uint32_t>(program >> 40u) & 0xffu));
    Append(words, SetShaderRegister(0x207, Threads));
    Append(words, SetShaderRegister(0x208, 1));
    Append(words, SetShaderRegister(0x209, 1));
    Append(words, SetShaderRegister(0x213, 4u << 1u));
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(out), static_cast<std::uint32_t>((out >> 32u) & 0xffffu) | (4u << 16u), Threads * Results, 0x11016facu};
    for (std::uint32_t i = 0; i < 4; ++i) Append(words, SetShaderRegister(0x240 + i, descriptor[i]));
    Append(words, std::array<std::uint32_t, 5>{0xc0031500u, 1, 1, 1, 0x8041u});
}

struct Outcome {
    std::uint32_t dispatchRead = 0;
    std::array<std::uint32_t, 4> counters{};
};

Outcome Round(std::uint8_t* code, std::uint8_t* output, std::uint8_t* readBack, std::uint8_t* held, bool holdQueue) {
    std::vector<std::uint32_t> words;
    Append(words, FillGds(500u));
    Submit(std::move(words));
    AgcDriverWaitIdle_nid_postfix();
    std::memset(output, 0, Threads * Results * 4);
    std::memset(readBack, 0, CounterBytes);
    std::atomic<bool> release{false};
    if (holdQueue) {
        const auto address = Address(held);
        DrawPipeline::Queue0().Enqueue([&release] {
            while (!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }, {{address, address + 16}});
    }
    words.clear();
    AppendDispatch(words, code, output);
    Append(words, FillGds(3000u));
    Append(words, CopyGdsTo(Address(readBack)));
    Submit(std::move(words));
    if (holdQueue) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release.store(true, std::memory_order_release);
    }
    AgcDriverWaitIdle_nid_postfix();
    Outcome outcome;
    AgcDriver::GuestMemory::Read(Address(output), std::as_writable_bytes(std::span(&outcome.dispatchRead, 1)), 4);
    AgcDriver::GuestMemory::Read(Address(readBack), std::as_writable_bytes(std::span(outcome.counters)), 4);
    return outcome;
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const int rounds = argc > 1 ? std::atoi(argv[1]) : 20;
        check(rounds > 0, "the round count must be positive");
        auto* readBack = GuestBlock();
        auto* held = GuestBlock();
        auto* code = GuestBlock();
        auto* output = GuestBlock();
        std::memcpy(code, AppendProgram.data(), sizeof(AppendProgram));
        std::vector<std::uint32_t> words;
        AppendDispatch(words, code, output);
        Submit(std::move(words));
        AgcDriverWaitIdle_nid_postfix();
        std::string first;
        int wrong[2] = {0, 0};
        for (int round = 0; round < rounds; ++round) {
            for (int hold = 0; hold < 2; ++hold) {
                const auto outcome = Round(code, output, readBack, held, hold == 1);
                const bool right = outcome.dispatchRead == 500u && outcome.counters[0] == 3000u && outcome.counters[1] == 3000u && outcome.counters[2] == 3000u && outcome.counters[3] == 3000u;
                if (right) continue;
                ++wrong[hold];
                if (first.empty()) {
                    first = std::string(hold == 1 ? "behind a held item" : "without a held item") + ", round " + std::to_string(round) + ": the dispatch before the fill read " + std::to_string(outcome.dispatchRead) + " (500 expected), the GDS after the fill holds " +
                            std::to_string(outcome.counters[0]) + "/" + std::to_string(outcome.counters[1]) + "/" + std::to_string(outcome.counters[2]) + "/" + std::to_string(outcome.counters[3]) + " (3000 expected)";
                }
            }
        }
        std::printf("GDS queued dispatch order: %d rounds, %s GDS; wrong without a held item %d, behind a held item %d\n", rounds, AgcDriver::Pm4::GdsImportable() ? "shared" : "private", wrong[0], wrong[1]);
        check(first.empty(), first);
        std::printf("GDS queued dispatch order tests passed\n");
        LibcRunShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
