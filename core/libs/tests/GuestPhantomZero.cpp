#include "prx/libkernel/include/Api.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "SceTypes.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <source_location>

static void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Phantom zero check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
        std::abort();
    }
}

namespace {

constexpr int Fixed = 0x10;
constexpr int ReadWrite = 0x3;
constexpr std::size_t Length = 4 * 1024 * 1024;
constexpr std::size_t Alignment = 2 * 1024 * 1024;

bool allZero(const unsigned char* bytes, std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        if (bytes[index] != 0) return false;
    }
    return true;
}

void writeSome(unsigned char* bytes, std::size_t length, unsigned char value, std::size_t stride) {
    for (std::size_t offset = 0; offset < length; offset += stride) bytes[offset] = value;
}

void cycleAfter(void* address, std::int64_t& physical, const std::function<void(unsigned char*, std::int64_t)>& write) {
    auto* bytes = static_cast<unsigned char*>(address);
    write(bytes, physical);
    void* hidden = address;
    Require(sceKernelReserveVirtualRange(&hidden, Length, Fixed, 0) == 0 && hidden == address);
    Require(sceKernelReleaseDirectMemory(physical, Length) == 0);
    std::int64_t again = -1;
    Require(sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(sceKernelGetDirectMemorySize()), Length, Alignment, 0, &again) == 0);
    void* mapped = address;
    Require(sceKernelMapDirectMemory(&mapped, Length, ReadWrite, Fixed, again, 0) == 0 && mapped == address);
    Require(allZero(bytes, Length));
    physical = again;
}

void cycle(void* address, std::int64_t& physical, unsigned char value, std::size_t stride) {
    cycleAfter(address, physical, [&](unsigned char* bytes, std::int64_t) { writeSome(bytes, Length, value, stride); });
}

void writeThroughAlias(std::int64_t physical, unsigned char value) {
    void* alias = nullptr;
    Require(sceKernelReserveVirtualRange(&alias, Length, 0, Alignment) == 0 && alias != nullptr);
    void* mapped = alias;
    Require(sceKernelMapDirectMemory(&mapped, Length, ReadWrite, Fixed, physical, 0) == 0 && mapped == alias);
    writeSome(static_cast<unsigned char*>(alias), Length, value, 3 * 4096 + 5);
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(alias), Length) == 0);
}

void readFileInto(unsigned char* bytes, const std::filesystem::path& file) {
    const int handle = sceKernelOpen(file.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(handle >= 0);
    for (const std::size_t offset : {std::size_t{0}, std::size_t{4096 * 7 + 100}, Length / 2 - 3000, Length - 65536}) {
        Require(sceKernelLseek(handle, 0, 0) == 0);
        Require(sceKernelRead(handle, bytes + offset, 65536) == 65536);
    }
    Require(sceKernelClose(handle) == 0);
}

}

int main() {
    void* address = nullptr;
    Require(sceKernelReserveVirtualRange(&address, Length, 0, Alignment) == 0 && address != nullptr);
    std::int64_t physical = -1;
    Require(sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(sceKernelGetDirectMemorySize()), Length, Alignment, 0, &physical) == 0);
    void* mapped = address;
    Require(sceKernelMapDirectMemory(&mapped, Length, ReadWrite, Fixed, physical, 0) == 0 && mapped == address);
    std::memset(address, 0x5a, Length);
    cycle(address, physical, 0xa5, 1);
    cycle(address, physical, 0x11, 4096);
    cycle(address, physical, 0x22, 65536);
    cycle(address, physical, 0, 1);
    cycle(address, physical, 0x33, 3 * 4096 + 17);
    std::printf("phantom reuse after CPU writes reads zeros (5 cycles)\n");
    cycleAfter(address, physical, [](unsigned char*, std::int64_t backing) { writeThroughAlias(backing, 0x44); });
    cycleAfter(address, physical, [](unsigned char*, std::int64_t backing) { writeThroughAlias(backing, 0x55); });
    std::printf("phantom reuse after writes through another mapping of the same memory reads zeros (2 cycles)\n");
    const auto file = std::filesystem::path("anyps5-phantom-zero-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    {
        std::ofstream out(file, std::ios::binary);
        const std::vector<char> content(65536, static_cast<char>(0x7e));
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    cycleAfter(address, physical, [&](unsigned char* bytes, std::int64_t) { readFileInto(bytes, file); });
    cycleAfter(address, physical, [&](unsigned char* bytes, std::int64_t) { readFileInto(bytes, file); });
    std::filesystem::remove(file);
    std::printf("phantom reuse after kernel file reads reads zeros (2 cycles)\n");
    Require(sceKernelMunmap(reinterpret_cast<std::uint64_t>(address), Length) == 0);
    Require(sceKernelReleaseDirectMemory(physical, Length) == 0);
    return 0;
}
