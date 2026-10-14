#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/FrameCaptureSession.hpp"
#include <mutex>

namespace AgcDriver {

void Submit(const Packet* packet, std::uint32_t queue) try {
    const auto forward = [&] { DriverDetail::Driver::Get().Submit(packet, queue); };
    if (auto* capture = DriverDetail::ActiveFrameCapture()) capture->Submit({queue, reinterpret_cast<std::uint64_t>(packet->addr), packet->dw_num, packet->flags}, forward);
    else forward();
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

void WaitIdle() {
    DriverDetail::Driver::Get().WaitIdle();
}

void Shutdown() {
    DriverDetail::Driver::Get().Shutdown();
}

void RegisterShader(const Shader* shader) {
    DriverDetail::Driver::Get().RegisterShader(shader);
    if (auto* capture = DriverDetail::ActiveFrameCapture()) capture->Preparation(DriverDetail::CapturedShader(shader));
}

void SuspendPoint() {
    const auto forward = [] { DriverDetail::Driver::Get().SuspendPoint(); };
    if (auto* capture = DriverDetail::ActiveFrameCapture()) capture->Suspend(forward);
    else forward();
}

void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    DriverDetail::Driver::Get().RegisterVideoOutput(handle, output);
}

void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    DriverDetail::Driver::Get().UnregisterVideoOutput(handle, output);
}

void AttachWindow(const PresentationWindow& window) {
    DriverDetail::Driver::Get().AttachWindow(window);
}

void PresentClear(const PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    const auto forward = [&] { DriverDetail::Driver::Get().Present(window, nullptr, opaque, gpuReady, context); };
    if (auto* capture = DriverDetail::ActiveFrameCapture()) capture->Present({false, opaque, {}}, forward);
    else forward();
}

void PresentBuffer(const PresentationWindow& window, const DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    const auto forward = [&] { DriverDetail::Driver::Get().Present(window, &buffer, true, gpuReady, context); };
    if (auto* capture = DriverDetail::ActiveFrameCapture()) capture->Present({true, true, buffer}, forward);
    else forward();
}

void ReleaseWindow(void* window) {
    DriverDetail::Driver::Get().ReleaseWindow(window);
}

void ReportFailure(std::exception_ptr error) {
    DriverDetail::Driver::Get().ReportFailure(error);
}

}

extern "C" void AgcDriverRaiseWorkerThreadPriority_nid_postfix(const char* role) {
    AgcDriver::RaiseWorkerThreadPriority(role);
}

extern "C" void AgcDriverWaitIdle_nid_postfix() try {
    AgcDriver::WaitIdle();
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

static std::mutex& VulkanLoaderMutex() {
    static std::mutex mutex;
    return mutex;
}

extern "C" void AgcDriverLockVulkanLoader_nid_postfix() {
    VulkanLoaderMutex().lock();
}

extern "C" void AgcDriverUnlockVulkanLoader_nid_postfix() {
    VulkanLoaderMutex().unlock();
}

extern "C" void AgcDriverShutdown_nid_postfix() {
    AgcDriver::Shutdown();
}

extern "C" void AgcDriverRegisterShader_nid_postfix(const Shader* shader) try {
    AgcDriver::RegisterShader(shader);
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

extern "C" void AgcDriverSuspendPoint_nid_postfix() try {
    AgcDriver::SuspendPoint();
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

extern "C" void AgcDriverRegisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::RegisterVideoOutput(handle, output);
}

extern "C" void AgcDriverUnregisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::UnregisterVideoOutput(handle, output);
}

extern "C" void AgcDriverAttachWindow_nid_postfix(const AgcDriver::PresentationWindow& window) {
    AgcDriver::AttachWindow(window);
}

extern "C" void AgcDriverPresentClear_nid_postfix(const AgcDriver::PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentClear(window, opaque, gpuReady, context);
}

extern "C" void AgcDriverPresentBuffer_nid_postfix(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentBuffer(window, buffer, gpuReady, context);
}

extern "C" void AgcDriverReleaseWindow_nid_postfix(void* window) {
    AgcDriver::ReleaseWindow(window);
}

extern "C" void AgcDriverReportFailure_nid_postfix(std::exception_ptr error) {
    AgcDriver::ReportFailure(error);
}

extern "C" void AgcDriverResolveShaderAbi_nid_postfix(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) try {
    AgcDriver::DriverDetail::Driver::Get().ResolveShaderAbi(shader, context, primitive);
    if (auto* capture = AgcDriver::DriverDetail::ActiveFrameCapture()) capture->Preparation(AgcDriver::FrameCaptureShaderAbi{reinterpret_cast<std::uint64_t>(shader), {context.begin(), context.end()}, {primitive.begin(), primitive.end()}});
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

extern "C" void AgcDriverResolveGraphicsAbi_nid_postfix(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) try {
    AgcDriver::DriverDetail::Driver::Get().ResolveGraphicsAbi(vertex, pixel, primitiveType);
    if (auto* capture = AgcDriver::DriverDetail::ActiveFrameCapture()) capture->Preparation(AgcDriver::FrameCaptureGraphicsAbi{reinterpret_cast<std::uint64_t>(vertex), reinterpret_cast<std::uint64_t>(pixel), primitiveType});
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}
extern "C" void AgcDriverResolveGraphicsStagesAbi_nid_postfix(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) try {
    AgcDriver::DriverDetail::Driver::Get().ResolveGraphicsStagesAbi(stages, context, primitive);
    if (auto* capture = AgcDriver::DriverDetail::ActiveFrameCapture()) {
        AgcDriver::FrameCaptureGraphicsStagesAbi record{{}, {context.begin(), context.end()}, {primitive.begin(), primitive.end()}};
        for (const auto* stage : stages) record.stages.push_back(reinterpret_cast<std::uint64_t>(stage));
        capture->Preparation(std::move(record));
    }
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}
