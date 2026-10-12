#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

extern "C" std::uint32_t APS5_VABI arc4random_nid_postfix();

static void Report(const char* name, const std::function<std::uint32_t()>& draw) {
    for (int trial = 0; trial < 3; ++trial) {
        std::vector<std::uint32_t> words(65536);
        const auto start = std::chrono::steady_clock::now();
        for (auto& word : words) word = draw();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::map<std::uint32_t, int> counts;
        for (const auto word : words) ++counts[word];
        std::size_t repeats = 0;
        std::string top;
        for (const auto& [value, count] : counts) {
            repeats += static_cast<std::size_t>(count - 1);
            if (count > 1 && top.size() < 200) top += " " + std::to_string(value) + "x" + std::to_string(count);
        }
        std::fprintf(stderr, "[diag] %s trial %d: %.3f s, repeats %zu:%s\n", name, trial, seconds, repeats, top.c_str());
    }
}

int main() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0 || line.rfind("flags", 0) == 0) {
            const bool flags = line.rfind("flags", 0) == 0;
            std::fprintf(stderr, "[diag] %s\n", flags ? ((line.find(" rdseed") != std::string::npos ? std::string("rdseed yes") : std::string("rdseed no")) + (line.find(" rdrand") != std::string::npos ? ", rdrand yes" : ", rdrand no")).c_str() : line.c_str());
            if (flags) break;
        }
    }
    std::random_device defaultDevice;
    std::fprintf(stderr, "[diag] default random_device entropy %.1f\n", defaultDevice.entropy());
    Report("arc4random (HostEntropy)", [] { return arc4random_nid_postfix(); });
    Report("default", [&] { return static_cast<std::uint32_t>(defaultDevice()); });
    for (const char* token : {"rdseed", "rdrand", "/dev/urandom", "getentropy"}) {
        try {
            std::random_device device(token);
            Report(token, [&] { return static_cast<std::uint32_t>(device()); });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[diag] %s unavailable: %s\n", token, error.what());
        }
    }
    return 1;
}
