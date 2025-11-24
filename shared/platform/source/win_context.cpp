#include "../window_context.h"

#include "include/core/SkColorSpace.h"

#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <d3d12sdklayers.h>

// MARK: - D3D helpers (see skia tools D3DTestUtils.cpp)

void get_hardware_adapter(IDXGIFactory4* pFactory, IDXGIAdapter1** ppAdapter) {
    *ppAdapter = nullptr;
    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        IDXGIAdapter1* pAdapter = nullptr;
        if (DXGI_ERROR_NOT_FOUND == pFactory->EnumAdapters1(adapterIndex, &pAdapter)) {
            // No more adapters to enumerate.
            break;
        }

        // Check to see if the adapter supports Direct3D 12, but don't create the
        // actual device yet.
        if (SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device),
                                        nullptr))) {
            *ppAdapter = pAdapter;
            return;
        }
        pAdapter->Release();
    }
}

bool CreateD3DBackendContext(GrD3DBackendContext* ctx, bool isProtected = false) {
#if defined(SK_ENABLE_D3D_DEBUG_LAYER)
    // Enable the D3D12 debug layer.
    {
        gr_cp<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
        }
    }
#endif
    // Create the device
    gr_cp<IDXGIFactory4> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    gr_cp<IDXGIAdapter1> hardwareAdapter;
    get_hardware_adapter(factory.get(), &hardwareAdapter);

    gr_cp<ID3D12Device> device;
    if (!SUCCEEDED(D3D12CreateDevice(hardwareAdapter.get(),
                                     D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&device)))) {
        return false;
    }

    // Create the command queue
    gr_cp<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (!SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
        return false;
    }

    ctx->fAdapter = hardwareAdapter;
    ctx->fDevice = device;
    ctx->fQueue = queue;
    // TODO: set up protected memory
    ctx->fProtectedContext = /*isProtected ? GrProtected::kYes :*/ GrProtected::kNo;

    return true;
}

// MARK: - window context

#define GR_D3D_CALL_ERRCHECK(X)                                         \
    do {                                                                \
        HRESULT result = X;                                             \
        SkASSERT(SUCCEEDED(result));                                    \
        if (!SUCCEEDED(result)) {                                       \
            SkDebugf("Failed Direct3D call. Error: 0x%08lx\n", result); \
        }                                                               \
    } while (false)

using namespace Microsoft::WRL;

namespace tiny {

// MARK: - setup

auto Window_context::setup(const Setup& setup) -> void
{
    fWindow = static_cast<HWND>(setup.native_handle); // Otherwise in constructor
    SkASSERT(fWindow);

    GrD3DBackendContext backendContext;
    CreateD3DBackendContext(&backendContext);
    fDevice = backendContext.fDevice;
    fQueue = backendContext.fQueue;

    auto options = GrContextOptions{};
    fContext = GrDirectContext::MakeDirect3D(backendContext, options); // default;
    SkASSERT(fContext);

    // Make the swapchain
    RECT windowRect;
    GetWindowRect(fWindow, &windowRect);
    unsigned int width = windowRect.right - windowRect.left;
    unsigned int height = windowRect.bottom - windowRect.top;

    UINT dxgiFactoryFlags = 0;
    SkDEBUGCODE(dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;)

    gr_cp<IDXGIFactory4> factory;
    GR_D3D_CALL_ERRCHECK(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kNumFrames;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    gr_cp<IDXGISwapChain1> swapChain;
    GR_D3D_CALL_ERRCHECK(factory->CreateSwapChainForHwnd(
            fQueue.get(), fWindow, &swapChainDesc, nullptr, nullptr, &swapChain));

    // We don't support fullscreen transitions.
    GR_D3D_CALL_ERRCHECK(factory->MakeWindowAssociation(fWindow, DXGI_MWA_NO_ALT_ENTER));

    GR_D3D_CALL_ERRCHECK(swapChain->QueryInterface(IID_PPV_ARGS(&fSwapChain)));

    fBufferIndex = fSwapChain->GetCurrentBackBufferIndex();

    //fSampleCount = fDisplayParams->msaaSampleCount(); // 1

    this->setupSurfaces(width, height);

    for (int i = 0; i < kNumFrames; ++i) {
        fFenceValues[i] = 10000;   // use a high value to make it easier to track these in PIX
    }
    GR_D3D_CALL_ERRCHECK(fDevice->CreateFence(fFenceValues[fBufferIndex], D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(&fFence)));

    fFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    SkASSERT(fFenceEvent);

    // fWidth = width;
    // fHeight = height;
    _size = Rect_size{static_cast<int32_t>(width), static_cast<int32_t>(height)};
}

// MARK: - teardown

auto Window_context::teardown() -> void
{
    CloseHandle(fFenceEvent);
    fFence.reset(nullptr);

    for (int i = 0; i < kNumFrames; ++i) {
        fSurfaces[i].reset(nullptr);
        fBuffers[i].reset(nullptr);
    }

    fSwapChain.reset(nullptr);
    fQueue.reset(nullptr);
    fDevice.reset(nullptr);
}

// MARK: - drawing

auto Window_context::begin_draw() -> void
{
    // Update the frame index.
    const UINT64 currentFenceValue = fFenceValues[fBufferIndex];
    fBufferIndex = fSwapChain->GetCurrentBackBufferIndex();

    // If the last frame for this buffer index is not done, wait until it is ready.
    if (fFence->GetCompletedValue() < fFenceValues[fBufferIndex]) {
        GR_D3D_CALL_ERRCHECK(fFence->SetEventOnCompletion(fFenceValues[fBufferIndex], fFenceEvent));
        WaitForSingleObjectEx(fFenceEvent, INFINITE, FALSE);
    }

    // Set the fence value for the next frame.
    fFenceValues[fBufferIndex] = currentFenceValue + 1;

    //return fSurfaces[fBufferIndex];
}

auto Window_context::get_canvas() -> Canvas
{
    if (!fSurfaces[fBufferIndex]) {
        return Canvas{nullptr};
    }
    auto* canvas = fSurfaces[fBufferIndex]->getCanvas();
    canvas->clear(SK_ColorBLACK);
    canvas->resetMatrix();
    return Canvas{canvas};
}

auto Window_context::end_draw() -> void
{
    // submitToGpu
    if (auto dc = fContext.get()) { // directContext()
        GrFlushInfo info;
        // if (statsCallback) {
        //     auto callback = std::make_unique<GpuTimerCallback>(std::move(statsCallback));
        //     info.fFinishedContext = callback.release();
        //     info.fFinishedWithStatsProc = [](GrGpuFinishedContext context,
        //                                      const skgpu::GpuStats& stats) {
        //         std::unique_ptr<GpuTimerCallback> callback{static_cast<GpuTimerCallback*>(context)};
        //         (*callback)(stats.elapsedTime);
        //     };
        //     info.fGpuStatsFlags = skgpu::GpuStatsFlags::kElapsedTime;
        // }
        dc->flush(info);
        dc->submit();
        //return;
    }
    // if (statsCallback) {
    //     statsCallback(0);
    // }

    // onSwapBuffers
    SkSurface* surface = fSurfaces[fBufferIndex].get();

    GrFlushInfo info;
    fContext->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, info);
    fContext->submit();

    GR_D3D_CALL_ERRCHECK(fSwapChain->Present(1, 0));

    // Schedule a Signal command in the queue.
    GR_D3D_CALL_ERRCHECK(fQueue->Signal(fFence.get(), fFenceValues[fBufferIndex]));
}

auto Window_context::on_resized() -> void
{
    // Clean up any outstanding resources in command lists
    fContext->flush();
    fContext->submit(GrSyncCpu::kYes);

    // release the previous surface and backbuffer resources
    for (int i = 0; i < kNumFrames; ++i) {
        // Let present complete
        if (fFence->GetCompletedValue() < fFenceValues[i]) {
            GR_D3D_CALL_ERRCHECK(fFence->SetEventOnCompletion(fFenceValues[i], fFenceEvent));
            WaitForSingleObjectEx(fFenceEvent, INFINITE, FALSE);
        }
        fSurfaces[i].reset(nullptr);
        fBuffers[i].reset(nullptr);
    }

    // Derive size from window handle.
    RECT windowRect;
    GetWindowRect(fWindow, &windowRect);
    unsigned int width = windowRect.right - windowRect.left;
    unsigned int height = windowRect.bottom - windowRect.top;

    GR_D3D_CALL_ERRCHECK(fSwapChain->ResizeBuffers(0, width, height,
                                                   DXGI_FORMAT_R8G8B8A8_UNORM, 0));

    this->setupSurfaces(width, height);

    // fWidth = width;
    // fHeight = height;
    _size = Rect_size{static_cast<int32_t>(width), static_cast<int32_t>(height)};
}

// MARK: - private

auto Window_context::setupSurfaces(int width, int height) -> void
{
    // set up base resource info
    GrD3DTextureResourceInfo info(nullptr,
                                  nullptr,
                                  D3D12_RESOURCE_STATE_PRESENT,
                                  DXGI_FORMAT_R8G8B8A8_UNORM,
                                  1,
                                  1,
                                  0);
    for (int i = 0; i < kNumFrames; ++i) {
        GR_D3D_CALL_ERRCHECK(fSwapChain->GetBuffer(i, IID_PPV_ARGS(&fBuffers[i])));

        SkASSERT(fBuffers[i]->GetDesc().Width == (UINT64)width &&
                 fBuffers[i]->GetDesc().Height == (UINT64)height);

        info.fResource = fBuffers[i];

        // Display params defaults.
        const auto fSampleCount = 1;
        auto surfaceProps = SkSurfaceProps(0, kRGB_H_SkPixelGeometry);
        
        if (fSampleCount > 1) {
            GrBackendTexture backendTexture(width, height, info);
            fSurfaces[i] = SkSurfaces::WrapBackendTexture(fContext.get(),
                                                          backendTexture,
                                                          kTopLeft_GrSurfaceOrigin,
                                                          fSampleCount,
                                                          kRGBA_8888_SkColorType,
                                                          nullptr, // color space (also Display params default.)
                                                          &surfaceProps);
        } else {
            GrBackendRenderTarget backendRT(width, height, info);
            fSurfaces[i] = SkSurfaces::WrapBackendRenderTarget(fContext.get(),
                                                               backendRT,
                                                               kTopLeft_GrSurfaceOrigin,
                                                               kRGBA_8888_SkColorType,
                                                               nullptr, // color space (also Display params default.)
                                                               &surfaceProps);
        }
    }
}

} // namespace tiny