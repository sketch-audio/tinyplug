#include "../window_context.h"

#include "win_config.h" // WIN_GRAPHICS_GPU

#if WIN_GRAPHICS_GPU
#define SK_DIRECT3D // tiny_deps builds Skia with D3D but we still need this define here.
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
#else
#include "include/core/SkSurface.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkBitmap.h"
#include <windows.h>
#endif

namespace tiny {

struct Window_context::Impl {
    HWND fWindow;
#if WIN_GRAPHICS_GPU
    sk_sp<GrDirectContext> fContext;
    // D3D resources.
    inline static constexpr int kNumFrames = 2;
    gr_cp<ID3D12Device> fDevice;
    gr_cp<ID3D12CommandQueue> fQueue;
    gr_cp<IDXGISwapChain3> fSwapChain;
    gr_cp<ID3D12Resource> fBuffers[kNumFrames];
    sk_sp<SkSurface> fSurfaces[kNumFrames];
    // Synchronization objects.
    unsigned int fBufferIndex;
    HANDLE fFenceEvent;
    gr_cp<ID3D12Fence> fFence;
    uint64_t fFenceValues[kNumFrames];
#else
    void* _drawable;
    SkBitmap fBitmap;
    sk_sp<SkSurface> fSurface;
    BITMAPINFO fBitmapInfo = {};
    int fWidth = 0;
    int fHeight = 0;
#endif
};

Window_context::Window_context() : _impl(std::make_unique<Impl>()) {}
Window_context::~Window_context() = default;

} // namespace tiny

#if WIN_GRAPHICS_GPU
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
    _impl->fWindow = static_cast<HWND>(setup.native_handle); // Otherwise in constructor
    SkASSERT(_impl->fWindow);

    GrD3DBackendContext backendContext;
    CreateD3DBackendContext(&backendContext);
    _impl->fDevice = backendContext.fDevice;
    _impl->fQueue = backendContext.fQueue;

    auto options = GrContextOptions{};
    _impl->fContext = GrDirectContext::MakeDirect3D(backendContext, options); // default;
    SkASSERT(_impl->fContext);

    // Make the swapchain
    RECT windowRect;
    GetWindowRect(_impl->fWindow, &windowRect);
    unsigned int width = windowRect.right - windowRect.left;
    unsigned int height = windowRect.bottom - windowRect.top;

    UINT dxgiFactoryFlags = 0;
    SkDEBUGCODE(dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;)

    gr_cp<IDXGIFactory4> factory;
    GR_D3D_CALL_ERRCHECK(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = _impl->kNumFrames;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    gr_cp<IDXGISwapChain1> swapChain;
    GR_D3D_CALL_ERRCHECK(factory->CreateSwapChainForHwnd(
            _impl->fQueue.get(), _impl->fWindow, &swapChainDesc, nullptr, nullptr, &swapChain));

    // We don't support fullscreen transitions.
    GR_D3D_CALL_ERRCHECK(factory->MakeWindowAssociation(_impl->fWindow, DXGI_MWA_NO_ALT_ENTER));

    GR_D3D_CALL_ERRCHECK(swapChain->QueryInterface(IID_PPV_ARGS(&_impl->fSwapChain)));

    _impl->fBufferIndex = _impl->fSwapChain->GetCurrentBackBufferIndex();

    //fSampleCount = fDisplayParams->msaaSampleCount(); // 1

    this->setupSurfaces(width, height);

    for (int i = 0; i < _impl->kNumFrames; ++i) {
        _impl->fFenceValues[i] = 10000;   // use a high value to make it easier to track these in PIX
    }
    GR_D3D_CALL_ERRCHECK(_impl->fDevice->CreateFence(_impl->fFenceValues[_impl->fBufferIndex], D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(&_impl->fFence)));

    _impl->fFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    SkASSERT(_impl->fFenceEvent);

    // fWidth = width;
    // fHeight = height;
    _size = Rect_size{static_cast<int32_t>(width), static_cast<int32_t>(height)};
}

// MARK: - teardown

auto Window_context::teardown() -> void
{
    CloseHandle(_impl->fFenceEvent);
    _impl->fFence.reset(nullptr);

    for (int i = 0; i < _impl->kNumFrames; ++i) {
        _impl->fSurfaces[i].reset(nullptr);
        _impl->fBuffers[i].reset(nullptr);
    }

    _impl->fSwapChain.reset(nullptr);
    _impl->fQueue.reset(nullptr);
    _impl->fDevice.reset(nullptr);
}

// MARK: - drawing

auto Window_context::set_drawable(void*) -> void
{
    // Apple-only.
}

auto Window_context::begin_draw() -> void
{
    // Update the frame index.
    const UINT64 currentFenceValue = _impl->fFenceValues[_impl->fBufferIndex];
    _impl->fBufferIndex = _impl->fSwapChain->GetCurrentBackBufferIndex();

    // If the last frame for this buffer index is not done, wait until it is ready.
    if (_impl->fFence->GetCompletedValue() < _impl->fFenceValues[_impl->fBufferIndex]) {
        GR_D3D_CALL_ERRCHECK(_impl->fFence->SetEventOnCompletion(_impl->fFenceValues[_impl->fBufferIndex], _impl->fFenceEvent));
        WaitForSingleObjectEx(_impl->fFenceEvent, INFINITE, FALSE);
    }

    // Set the fence value for the next frame.
    _impl->fFenceValues[_impl->fBufferIndex] = currentFenceValue + 1;

    //return fSurfaces[fBufferIndex];
}

auto Window_context::get_canvas() -> Canvas
{
    if (!_impl->fSurfaces[_impl->fBufferIndex]) {
        return Canvas{nullptr};
    }
    auto* canvas = _impl->fSurfaces[_impl->fBufferIndex]->getCanvas();
    canvas->clear(SK_ColorBLACK);
    canvas->resetMatrix();
    return Canvas{canvas};
}

auto Window_context::end_draw() -> void
{
    // submitToGpu
    if (auto dc = _impl->fContext.get()) { // directContext()
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
    SkSurface* surface = _impl->fSurfaces[_impl->fBufferIndex].get();

    GrFlushInfo info;
    _impl->fContext->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, info);
    _impl->fContext->submit();

    GR_D3D_CALL_ERRCHECK(_impl->fSwapChain->Present(1, 0));

    // Schedule a Signal command in the queue.
    GR_D3D_CALL_ERRCHECK(_impl->fQueue->Signal(_impl->fFence.get(), _impl->fFenceValues[_impl->fBufferIndex]));
}

auto Window_context::on_resized() -> void
{
    // Clean up any outstanding resources in command lists
    _impl->fContext->flush();
    _impl->fContext->submit(GrSyncCpu::kYes);

    // release the previous surface and backbuffer resources
    for (int i = 0; i < _impl->kNumFrames; ++i) {
        // Let present complete
        if (_impl->fFence->GetCompletedValue() < _impl->fFenceValues[i]) {
            GR_D3D_CALL_ERRCHECK(_impl->fFence->SetEventOnCompletion(_impl->fFenceValues[i], _impl->fFenceEvent));
            WaitForSingleObjectEx(_impl->fFenceEvent, INFINITE, FALSE);
        }
        _impl->fSurfaces[i].reset(nullptr);
        _impl->fBuffers[i].reset(nullptr);
    }

    // Derive size from window handle.
    RECT windowRect;
    GetWindowRect(_impl->fWindow, &windowRect);
    unsigned int width = windowRect.right - windowRect.left;
    unsigned int height = windowRect.bottom - windowRect.top;

    GR_D3D_CALL_ERRCHECK(_impl->fSwapChain->ResizeBuffers(0, width, height,
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
    for (int i = 0; i < _impl->kNumFrames; ++i) {
        GR_D3D_CALL_ERRCHECK(_impl->fSwapChain->GetBuffer(i, IID_PPV_ARGS(&_impl->fBuffers[i])));

        SkASSERT(_impl->fBuffers[i]->GetDesc().Width == (UINT64)width &&
                 _impl->fBuffers[i]->GetDesc().Height == (UINT64)height);

        info.fResource = _impl->fBuffers[i];

        // Display params defaults.
        const auto fSampleCount = 1;
        auto surfaceProps = SkSurfaceProps(0, kRGB_H_SkPixelGeometry);
        
        if (fSampleCount > 1) {
            GrBackendTexture backendTexture(width, height, info);
            _impl->fSurfaces[i] = SkSurfaces::WrapBackendTexture(_impl->fContext.get(),
                                                          backendTexture,
                                                          kTopLeft_GrSurfaceOrigin,
                                                          fSampleCount,
                                                          kRGBA_8888_SkColorType,
                                                          nullptr, // color space (also Display params default.)
                                                          &surfaceProps);
        } else {
            GrBackendRenderTarget backendRT(width, height, info);
            _impl->fSurfaces[i] = SkSurfaces::WrapBackendRenderTarget(_impl->fContext.get(),
                                                               backendRT,
                                                               kTopLeft_GrSurfaceOrigin,
                                                               kRGBA_8888_SkColorType,
                                                               nullptr, // color space (also Display params default.)
                                                               &surfaceProps);
        }
    }
}

} // namespace tiny
#else
// MARK: - CPU
namespace tiny {

auto Window_context::setup(const Setup& setup) -> void
{
    _impl->fWindow = static_cast<HWND>(setup.native_handle);
    SkASSERT(_impl->fWindow);

    RECT rect;
    GetClientRect(_impl->fWindow, &rect);

    _impl->fWidth  = rect.right - rect.left;
    _impl->fHeight = rect.bottom - rect.top;

    this->setupSurfaces(_impl->fWidth, _impl->fHeight);

    _size = Rect_size{static_cast<int32_t>(_impl->fWidth), static_cast<int32_t>(_impl->fHeight)};
}

auto Window_context::teardown() -> void
{
    _impl->fSurface.reset();
    _impl->fBitmap.reset();
}

auto Window_context::set_drawable(void* drawable) -> void
{
    // On Windows this is our HDC
    _impl->_drawable = drawable;
}

auto Window_context::begin_draw() -> void
{
    //
}

auto Window_context::get_canvas() -> Canvas
{
    if (!_impl->fSurface)
        return Canvas{nullptr};

    auto* canvas = _impl->fSurface->getCanvas();
    canvas->clear(SK_ColorBLACK);
    canvas->resetMatrix();
    return Canvas{canvas};
}

auto Window_context::end_draw() -> void
{
    if (!_impl->_drawable || !_impl->fBitmap.getPixels())
        return;

    HDC hdc = static_cast<HDC>(_impl->_drawable);

    StretchDIBits(
        hdc,
        0, 0, _impl->fWidth, _impl->fHeight,
        0, 0, _impl->fWidth, _impl->fHeight,
        _impl->fBitmap.getPixels(),
        &_impl->fBitmapInfo,
        DIB_RGB_COLORS,
        SRCCOPY
    );
}

auto Window_context::on_resized() -> void
{
    RECT rect;
    GetClientRect(_impl->fWindow, &rect);

    _impl->fWidth  = rect.right - rect.left;
    _impl->fHeight = rect.bottom - rect.top;

    this->setupSurfaces(_impl->fWidth, _impl->fHeight);

    _size = Rect_size{static_cast<int32_t>(_impl->fWidth), static_cast<int32_t>(_impl->fHeight)};
}

// MARK: - private

auto Window_context::setupSurfaces(int width, int height) -> void
{
    _impl->fWidth  = width;
    _impl->fHeight = height;

    SkImageInfo info = SkImageInfo::Make(
        width,
        height,
        kBGRA_8888_SkColorType,
        kPremul_SkAlphaType
    );

    _impl->fBitmap.allocPixels(info);

    _impl->fSurface = SkSurfaces::WrapPixels(
        info,
        _impl->fBitmap.getPixels(),
        _impl->fBitmap.rowBytes()
    );

    // Configure BITMAPINFO for StretchDIBits
    ZeroMemory(&_impl->fBitmapInfo, sizeof(_impl->fBitmapInfo));
    _impl->fBitmapInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    _impl->fBitmapInfo.bmiHeader.biWidth       = width;
    _impl->fBitmapInfo.bmiHeader.biHeight      = -height; // top-down DIB
    _impl->fBitmapInfo.bmiHeader.biPlanes      = 1;
    _impl->fBitmapInfo.bmiHeader.biBitCount    = 32;
    _impl->fBitmapInfo.bmiHeader.biCompression = BI_RGB;
}

} // namespace tiny
#endif