#pragma once

#define BUFFER_COUNT 3

// avg/max over one reporting window; reset after each report
struct TimingStat
{
    double sumMs = 0;
    double maxMs = 0;
    unsigned count = 0;
    void add(double ms) { sumMs += ms; if (ms > maxMs) maxMs = ms; count++; }
    double avg() const { return count ? sumMs / count : 0; }
    void reset() { sumMs = 0; maxMs = 0; count = 0; }
};

inline double MsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Capture-thread telemetry. Reported at log level 1 every 2s, then reset.
std::atomic_int captureLogLevel = 0;
struct
{
    std::chrono::steady_clock::time_point windowStart;
    unsigned ticks = 0;     // TickVideoCapture calls
    unsigned frames = 0;    // ticks that published a frame
    unsigned drained = 0;   // window frames discarded by newest-wins
    unsigned waits = 0;     // ticks that timed out with no frame
    TimingStat tick;        // whole tick, frames only
    TimingStat readback;    // CreateTexture2D + copy + Map
    TimingStat copy;        // memcpy into the triple buffer
    TimingStat preview;     // preview callback into the front end
    void reset()
    {
        windowStart = std::chrono::steady_clock::now();
        ticks = frames = drained = waits = 0;
        tick.reset(); readback.reset(); copy.reset(); preview.reset();
    }
} captureStats;

struct Bitmap {
    int                  width = 0;
    int                  height = 0;
    std::vector<uint8_t> buffer;
};

SourceOptions source_config = {};
std::atomic_uint lastVideoCaptureIndex = 0;
// Buffer the stream thread currently has open, or -1. Published so the capture thread can
// avoid it; the writer already avoids lastVideoCaptureIndex, and three buffers leave one free.
std::atomic_int activeReadIndex = -1;
Bitmap* videoCaptures = nullptr;

// Claims a buffer for the stream thread for as long as it is in scope.
struct CaptureReadLease
{
    explicit CaptureReadLease(unsigned int index) { activeReadIndex.store((int)index); }
    ~CaptureReadLease() { activeReadIndex.store(-1); }
    CaptureReadLease(const CaptureReadLease&) = delete;
    CaptureReadLease& operator=(const CaptureReadLease&) = delete;
};
int    displayIndex = 0;
ID3D11Device*           d3dDevice = nullptr;
ID3D11DeviceContext*    d3dDeviceContext = nullptr;
IDXGIOutputDuplication* desktopDuplication = nullptr;

// The crop is scaled and rotated on the GPU into a modeline-sized target, so the readback and
// everything after it cost the same whether the source is a 1X crop or a 4K window. Set from
// the modeline; the capture side never needs to know about fields.
std::atomic_uint        captureOutputWidth = 0;
std::atomic_uint        captureOutputHeight = 0;
ID2D1Factory1*          d2dFactory = nullptr;
ID2D1Device*            d2dDevice = nullptr;
ID2D1DeviceContext*     d2dContext = nullptr;
ID3D11Texture2D*        scaleTarget = nullptr;        // render target the crop is drawn into
ID2D1Bitmap1*           scaleTargetBitmap = nullptr;
ID3D11Texture2D*        scaleStaging = nullptr;       // CPU-readable copy of scaleTarget
unsigned int            scaleTargetWidth = 0;
unsigned int            scaleTargetHeight = 0;

void ReleaseScaleTargets()
{
    SAFE_RELEASE(scaleTargetBitmap);
    SAFE_RELEASE(scaleTarget);
    SAFE_RELEASE(scaleStaging);
    scaleTargetWidth = scaleTargetHeight = 0;
}

// Allocates the output pair once per size; a mode change recreates it.
bool EnsureScaleTargets(unsigned int width, unsigned int height)
{
    if (scaleTarget && scaleTargetWidth == width && scaleTargetHeight == height)
        return true;
    ReleaseScaleTargets();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &scaleTarget);
    EXIT_ON_ERROR(hr, "CreateTexture2D for the scale target failed");

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = d3dDevice->CreateTexture2D(&desc, nullptr, &scaleStaging);
    EXIT_ON_ERROR(hr, "CreateTexture2D for the scale staging texture failed");

    IDXGISurface* surface = nullptr;
    hr = scaleTarget->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(&surface));
    EXIT_ON_ERROR(hr, "QueryInterface IDXGISurface on the scale target failed");
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    hr = d2dContext->CreateBitmapFromDxgiSurface(surface, &props, &scaleTargetBitmap);
    surface->Release();
    EXIT_ON_ERROR(hr, "CreateBitmapFromDxgiSurface for the scale target failed");

    scaleTargetWidth = width;
    scaleTargetHeight = height;
    return true;
}
bool                    haveFrameLock = false;
capture_image_function  captureFunction;
std::atomic_bool        hasNewSourceOptions;
SourceOptions           currentSourceOptions;
SourceOptions           newSourceOptions;

UINT_PTR                activeWindowHandle = 0;
bool                    windowUnavailableLogged = false;
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice windowDirect3DDevice{ nullptr };
winrt::Windows::Graphics::Capture::GraphicsCaptureItem windowCaptureItem{ nullptr };
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool windowFramePool{ nullptr };
winrt::Windows::Graphics::Capture::GraphicsCaptureSession windowCaptureSession{ nullptr };
winrt::Windows::Graphics::SizeInt32 windowCaptureSize = {};
winrt::event_token         windowFrameToken{};
std::mutex                 windowFrameMutex;
std::condition_variable    windowFrameCv;
// Bumped by FrameArrived and by WakeVideoCapture. The worker samples it before draining
// and waits for a change, so a signal raised during the drain cannot be missed.
uint64_t                   windowFrameSignal = 0;

// Also unblocks a worker parked in TickVideoCapture. Call after setting stopCapture, or
// the callers that spin on capturing_screen wait out the frame timeout.
void WakeVideoCapture()
{
    {
        std::lock_guard<std::mutex> guard(windowFrameMutex);
        windowFrameSignal++;
    }
    windowFrameCv.notify_all();
}

// Per-window capture; Desktop Duplication is display-only. Needs Win10 1903+.
bool InitializeWindowCapture(UINT_PTR windowHandle, IDXGIDevice* dxgiDevice)
{
    const HWND window = reinterpret_cast<HWND>(windowHandle);
    if (!IsWindow(window))
    {
        LogMessage("The selected capture window no longer exists.", true);
        return false;
    }
    // Before 1903 the runtime class is not registered and IsSupported throws rather than
    // returning false, so it needs its own handler. Uncaught it reaches either the P/Invoke
    // boundary or the capture thread, and terminates.
    bool supported = false;
    try { supported = winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported(); }
    catch (const winrt::hresult_error&) { }
    if (!supported)
    {
        LogMessage("Single-window capture requires Windows 10 version 1903 or newer.", true);
        return false;
    }

    try
    {
        auto itemInterop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        winrt::check_hresult(itemInterop->CreateForWindow(
            window,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(windowCaptureItem)));

        winrt::com_ptr<IInspectable> inspectableDevice;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice,
            inspectableDevice.put()));
        windowDirect3DDevice = inspectableDevice.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        windowCaptureSize = windowCaptureItem.Size();
        if (windowCaptureSize.Width <= 0 || windowCaptureSize.Height <= 0)
        {
            LogMessage("The selected capture window has no drawable area.", true);
            return false;
        }

        windowFramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            windowDirect3DDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            windowCaptureSize);

        // Free-threaded, so this runs on a threadpool thread, not the capture worker.
        windowFrameToken = windowFramePool.FrameArrived([](auto&&, auto&&) { WakeVideoCapture(); });

        windowCaptureSession = windowFramePool.CreateCaptureSession(windowCaptureItem);
        windowCaptureSession.StartCapture();
        activeWindowHandle = windowHandle;
        windowUnavailableLogged = false;
        LogMessage("[capture] Single-window capture started at " +
            std::to_string(windowCaptureSize.Width) + "x" +
            std::to_string(windowCaptureSize.Height) + ".");
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        LogMessage("Starting single-window capture failed: " +
            std::to_string(static_cast<long>(error.code())) + ".", true);
        return false;
    }
}

bool InitializeVideoCapture(int outputNumber, capture_image_function fnCapture)
{
    displayIndex = outputNumber;
    captureFunction = fnCapture;

    // Defaults are for the first init only. TickVideoCapture re-inits to recover, and
    // clearing the live options there drops the capture window and zeroes the crop size.
    if (videoCaptures == nullptr)
    {
        videoCaptures = new Bitmap[BUFFER_COUNT];
        currentSourceOptions = {};
        currentSourceOptions.alignment = Alignment::Center;
        currentSourceOptions.audio = 1;
        currentSourceOptions.syncrefresh = true;
        currentSourceOptions.cropmode = CropMode::Full43;
    }
    const UINT_PTR requestedWindowHandle = source_config.windowHandle;
    currentSourceOptions.windowHandle = requestedWindowHandle;
    captureStats.reset();

    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hDesk)
    {
        LogMessage("Failed to open desktop", true);
        return false;
    }

    // Attach desktop to this thread
    // Is this required? Should we do this on the capture thread?
    SetThreadDesktop(hDesk);
    CloseDesktop(hDesk);
    hDesk = nullptr;

    HRESULT hr = S_OK;

    D3D_DRIVER_TYPE driverTypes[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    auto numDriverTypes = ARRAYSIZE(driverTypes);

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1 };
    auto numFeatureLevels = ARRAYSIZE(featureLevels);

    D3D_FEATURE_LEVEL featureLevel;
    for (size_t i = 0; i < numDriverTypes; i++) {
        // BGRA_SUPPORT: required by Direct2D interop and CreateDirect3D11DeviceFromDXGIDevice.
        hr = D3D11CreateDevice(nullptr, driverTypes[i], nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, (UINT)numFeatureLevels,
            D3D11_SDK_VERSION, &d3dDevice, &featureLevel, &d3dDeviceContext);
        if (SUCCEEDED(hr))
            break;
    }

    EXIT_ON_ERROR(hr, "D3D11CreateDevice failed");

    IDXGIDevice* dxgiDevice = nullptr;
    hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    EXIT_ON_ERROR(hr, "D3DDevice->QueryInterface failed");

    // Direct2D on the same device does the crop/scale/rotate. Only the capture thread uses
    // the context once it exists; BGRA_SUPPORT above is what D2D interop requires.
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr,
        reinterpret_cast<void**>(&d2dFactory));
    EXIT_ON_ERROR(hr, "D2D1CreateFactory failed");
    hr = d2dFactory->CreateDevice(dxgiDevice, &d2dDevice);
    EXIT_ON_ERROR(hr, "ID2D1Factory1->CreateDevice failed");
    hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);
    EXIT_ON_ERROR(hr, "ID2D1Device->CreateDeviceContext failed");

    if (requestedWindowHandle != 0)
    {
        const bool initializedWindow = InitializeWindowCapture(requestedWindowHandle, dxgiDevice);
        dxgiDevice->Release();
        dxgiDevice = nullptr;
        return initializedWindow;
    }
    activeWindowHandle = 0;

    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
    dxgiDevice->Release();
    dxgiDevice = nullptr;
    EXIT_ON_ERROR(hr, "DxgiDevice->GetParent failed");

    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(displayIndex, &dxgiOutput);
    dxgiAdapter->Release();
    dxgiAdapter = nullptr;
    EXIT_ON_ERROR(hr, "DxgiAdapter->EnumOutputs faile");

    // DXGI_OUTPUT_DESC        outputDesc;
    // hr = dxgiOutput->GetDesc(&outputDesc);
    // EXIT_ON_ERROR(hr, "DxgiOutput->GetDesc faile");

    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(dxgiOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    dxgiOutput = nullptr;
    EXIT_ON_ERROR(hr, "DxgiOutput->QueryInterface faile");

    hr = dxgiOutput1->DuplicateOutput(d3dDevice, &desktopDuplication);
    dxgiOutput1->Release();
    dxgiOutput1 = nullptr;
    EXIT_ON_ERROR(hr, "DxgiOutput1->DuplicateOutput failed");

    return true;
}

void CleanupVideoCapture()
{
    try
    {
        // Revoke before closing, or a queued FrameArrived can land on a torn-down pool.
        if (windowFramePool && windowFrameToken.value != 0)
            windowFramePool.FrameArrived(windowFrameToken);
        if (windowCaptureSession)
            windowCaptureSession.Close();
        if (windowFramePool)
            windowFramePool.Close();
    }
    catch (const winrt::hresult_error& error)
    {
        LogMessage("Stopping single-window capture failed: " +
            std::to_string(static_cast<long>(error.code())) + ".", true);
    }
    windowFrameToken = {};
    windowCaptureSession = nullptr;
    windowFramePool = nullptr;
    windowCaptureItem = nullptr;
    windowDirect3DDevice = nullptr;
    windowCaptureSize = {};
    activeWindowHandle = 0;
    windowUnavailableLogged = false;
    ReleaseScaleTargets();
    SAFE_RELEASE(d2dContext);
    SAFE_RELEASE(d2dDevice);
    SAFE_RELEASE(d2dFactory);
    SAFE_RELEASE(desktopDuplication);
    SAFE_RELEASE(d3dDeviceContext);
    SAFE_RELEASE(d3dDevice);
    haveFrameLock = false;
}

bool TickVideoCapture()
{
    if (hasNewSourceOptions)
    {
        currentSourceOptions = newSourceOptions;
        hasNewSourceOptions = false;
    }

    const bool captureWindow = currentSourceOptions.windowHandle != 0;
    if ((!captureWindow && !desktopDuplication) || (captureWindow && !windowFramePool))
    {
        // A desktop switch, secure-desktop prompt or window recreation invalidates capture.
        // Rebuild rather than exiting, so the worker recovers when the source comes back.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CleanupVideoCapture();
        InitializeVideoCapture(displayIndex, captureFunction);
        return false;
    }

    // Reported here rather than after a frame, so a stalled source still shows up.
    if (captureLogLevel.load() >= 1 && MsSince(captureStats.windowStart) >= 2000)
    {
        const Bitmap& last = videoCaptures[lastVideoCaptureIndex];
        char line[256];
        snprintf(line, sizeof(line),
            "[capture] %dx%d ticks=%u frames=%u drained=%u waits=%u | tick %.1f/%.1fms readback %.1f/%.1f copy %.1f/%.1f preview %.1f/%.1f",
            last.width, last.height,
            captureStats.ticks, captureStats.frames, captureStats.drained, captureStats.waits,
            captureStats.tick.avg(), captureStats.tick.maxMs,
            captureStats.readback.avg(), captureStats.readback.maxMs,
            captureStats.copy.avg(), captureStats.copy.maxMs,
            captureStats.preview.avg(), captureStats.preview.maxMs);
        LogMessage(line);
        captureStats.reset();
    }
    captureStats.ticks++;
    const auto tickStart = std::chrono::steady_clock::now();

    HRESULT hr;
    ID3D11Texture2D* gpuTex = nullptr;
    winrt::Windows::Graphics::SizeInt32 capturedContentSize = {};
    // Function scope: the frame owns the surface we copy out of, so it has to outlive the
    // copy below. Releasing it early hands the surface back to the pool mid-copy.
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame windowFrame{ nullptr };

    if (captureWindow)
    {
        const HWND window = reinterpret_cast<HWND>(currentSourceOptions.windowHandle);
        if (!IsWindow(window))
        {
            if (!windowUnavailableLogged)
            {
                LogMessage("The selected capture window was closed. Choose another window.", true);
                windowUnavailableLogged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return false;
        }
        if (IsIconic(window))
        {
            if (!windowUnavailableLogged)
            {
                LogMessage("The selected capture window is minimized; capture will resume when it is restored.");
                windowUnavailableLogged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(32));
            return false;
        }

        try
        {
            uint64_t signalSeen;
            {
                std::lock_guard<std::mutex> guard(windowFrameMutex);
                signalSeen = windowFrameSignal;
            }

            // TryGetNextFrame returns the oldest queued frame. Take the newest instead:
            // the stream thread is paced by the modeline and only ever wants the latest.
            for (;;)
            {
                auto next = windowFramePool.TryGetNextFrame();
                if (!next)
                    break;
                if (windowFrame)
                {
                    windowFrame.Close();
                    captureStats.drained++;
                }
                windowFrame = next;
            }

            if (!windowFrame)
            {
                // Block like the desktop path's AcquireNextFrame(32). A window with static
                // content produces no frames, so the timeout doubles as the liveness recheck.
                captureStats.waits++;
                std::unique_lock<std::mutex> lock(windowFrameMutex);
                windowFrameCv.wait_for(lock, std::chrono::milliseconds(32),
                    [signalSeen] { return windowFrameSignal != signalSeen; });
                return false;
            }

            capturedContentSize = windowFrame.ContentSize();
            if (capturedContentSize.Width <= 0 || capturedContentSize.Height <= 0)
                return false;

            // Pool textures are fixed at the size the pool was built with, so without this
            // a window grown past its start size stays cropped to it. Recreate keeps the
            // pool object, so the FrameArrived registration survives.
            if (capturedContentSize.Width != windowCaptureSize.Width ||
                capturedContentSize.Height != windowCaptureSize.Height)
            {
                windowFrame.Close();
                windowFrame = nullptr;
                windowCaptureSize = capturedContentSize;
                windowFramePool.Recreate(
                    windowDirect3DDevice,
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    windowCaptureSize);
                return false;
            }

            auto surfaceAccess = windowFrame.Surface().as<
                Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            hr = surfaceAccess->GetInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&gpuTex));
            EXIT_ON_ERROR(hr, "Getting the single-window capture texture failed");
            windowUnavailableLogged = false;
        }
        catch (const winrt::hresult_error& error)
        {
            LogMessage("Acquiring the selected window failed: " +
                std::to_string(static_cast<long>(error.code())) + ".", true);
            CleanupVideoCapture();
            InitializeVideoCapture(displayIndex, captureFunction);
            return false;
        }
    }
    else
    {
        // Release right before acquiring the next desktop frame.
        if (haveFrameLock)
        {
            haveFrameLock = false;
            desktopDuplication->ReleaseFrame();
        }

        IDXGIResource* deskRes = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        hr = desktopDuplication->AcquireNextFrame(32, &frameInfo, &deskRes);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            captureStats.waits++;
            return false;
        }

        if (FAILED(hr))
        {
            // Try to reinitialize and capture next frame
            LogMessage("Acquire failed: " + std::to_string(hr), true);
            CleanupVideoCapture();
            InitializeVideoCapture(displayIndex, captureFunction);
            return false;
        }

        haveFrameLock = true;
        hr = deskRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&gpuTex));
        deskRes->Release();
        deskRes = nullptr;
        EXIT_ON_ERROR(hr, "Query Interface for ID3D11Texture2D failed");
    }

    bool ok = true;

    unsigned int width;
    unsigned int height;
    switch (source_config.rotation)
    {
    case Rotation::CW90:
    case Rotation::CCW90:
        width = currentSourceOptions.height;
        height = currentSourceOptions.width;
        break;
    default:
        width = currentSourceOptions.width;
        height = currentSourceOptions.height;
        break;
    }

    D3D11_TEXTURE2D_DESC desc;
    gpuTex->GetDesc(&desc);
    const unsigned int sourceWidth = captureWindow
        ? (std::min)(desc.Width, static_cast<unsigned int>(capturedContentSize.Width))
        : desc.Width;
    const unsigned int sourceHeight = captureWindow
        ? (std::min)(desc.Height, static_cast<unsigned int>(capturedContentSize.Height))
        : desc.Height;

    switch (currentSourceOptions.cropmode)
    {
    case CropMode::Custom:
    case CropMode::X1:
    case CropMode::X2:
    case CropMode::X3:
    case CropMode::X4:
    case CropMode::X5:
        break;
    case CropMode::Full43:
        switch (source_config.rotation)
        {
        case Rotation::CW90:
        case Rotation::CCW90:
            width = sourceHeight * 3 / 4;
            break;
        default:
            width = sourceHeight * 4 / 3;
            break;
        }
        height = sourceHeight;
        break;
    case CropMode::Full54:
        switch (source_config.rotation)
        {
        case Rotation::CW90:
        case Rotation::CCW90:
            width = sourceHeight * 4 / 5;
            break;
        default:
            width = sourceHeight * 5 / 4;
            break;
        }
        height = sourceHeight;
        break;
    case CropMode::FullSource:
        width = sourceWidth;
        height = sourceHeight;
        break;
    default:
        break;
    }

    if (width > sourceWidth)
        width = sourceWidth;
    if (height > sourceHeight)
        height = sourceHeight;

    int xoffset = currentSourceOptions.xoffset;
    int yoffset = currentSourceOptions.yoffset;
    switch (currentSourceOptions.alignment)
    {
    case Alignment::Center:
        xoffset += sourceWidth / 2 - width / 2;
        yoffset += sourceHeight / 2 - height / 2;
        break;
    case Alignment::TopLeft:
        break;
    case Alignment::Top:
        xoffset += sourceWidth / 2 - width / 2;
        break;
    case Alignment::TopRight:
        xoffset += sourceWidth - width;
        break;
    case Alignment::Right:
        xoffset += sourceWidth - width;
        yoffset += sourceHeight / 2 - height / 2;
        break;
    case Alignment::BottomRight:
        xoffset += sourceWidth - width;
        yoffset += sourceHeight - height;
        break;
    case Alignment::Bottom:
        xoffset += sourceWidth / 2 - width / 2;
        yoffset += sourceHeight - height;
        break;
    case Alignment::BottomLeft:
        yoffset += sourceHeight - height;
        break;
    case Alignment::Left:
        yoffset += sourceHeight / 2 - height / 2;
    default:
        break;
    }

    if (xoffset < 0)
        xoffset = 0;
    else if (xoffset + width > sourceWidth)
        xoffset = sourceWidth - width;

    if (yoffset < 0)
        yoffset = 0;
    else if (yoffset + height > sourceHeight)
        yoffset = sourceHeight - height;

    // Output is the modeline's active area; before one is set, the crop size keeps the
    // preview working.
    unsigned int outWidth = captureOutputWidth.load();
    unsigned int outHeight = captureOutputHeight.load();
    if (outWidth == 0 || outHeight == 0)
    {
        outWidth = width;
        outHeight = height;
    }

    ID2D1Bitmap1* sourceBitmap = nullptr;
    auto fail = [&](const char* message) -> bool
    {
        LogMessage(std::string(message) + ": " + std::to_string(hr), true);
        SAFE_RELEASE(sourceBitmap);
        gpuTex->Release();
        return false;
    };

    if (!EnsureScaleTargets(outWidth, outHeight))
    {
        gpuTex->Release();
        return false;
    }

    const auto readbackStart = std::chrono::steady_clock::now();

    // Wrap the capture texture for D2D; this is a view, not a copy.
    IDXGISurface* sourceSurface = nullptr;
    hr = gpuTex->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(&sourceSurface));
    if (FAILED(hr))
        return fail("QueryInterface IDXGISurface on the capture texture failed");
    const D2D1_BITMAP_PROPERTIES1 sourceProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    hr = d2dContext->CreateBitmapFromDxgiSurface(sourceSurface, &sourceProps, &sourceBitmap);
    sourceSurface->Release();
    if (FAILED(hr))
        return fail("CreateBitmapFromDxgiSurface on the capture texture failed");

    // Rotation names describe the monitor, so the image turns the other way. For the 90
    // degree cases the unrotated destination is outHeight x outWidth about the centre; the
    // rotation then lands it on the full output.
    const D2D1_RECT_F sourceRect = D2D1::RectF(
        (float)xoffset, (float)yoffset, (float)(xoffset + width), (float)(yoffset + height));
    const D2D1_POINT_2F centre = D2D1::Point2F(outWidth / 2.0f, outHeight / 2.0f);
    const D2D1_RECT_F transposedRect = D2D1::RectF(
        centre.x - outHeight / 2.0f, centre.y - outWidth / 2.0f,
        centre.x + outHeight / 2.0f, centre.y + outWidth / 2.0f);
    D2D1_RECT_F destRect = D2D1::RectF(0.0f, 0.0f, (float)outWidth, (float)outHeight);
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
    switch (source_config.rotation)
    {
    case Rotation::CW90:
        destRect = transposedRect;
        transform = D2D1::Matrix3x2F::Rotation(-90.0f, centre);
        break;
    case Rotation::CCW90:
        destRect = transposedRect;
        transform = D2D1::Matrix3x2F::Rotation(90.0f, centre);
        break;
    case Rotation::Flip180:
        transform = D2D1::Matrix3x2F::Rotation(180.0f, centre);
        break;
    default:
        break;
    }

    d2dContext->SetTarget(scaleTargetBitmap);
    d2dContext->BeginDraw();
    d2dContext->SetTransform(transform);
    d2dContext->DrawBitmap(sourceBitmap, &destRect, 1.0f,
        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &sourceRect);
    d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
    hr = d2dContext->EndDraw();
    d2dContext->SetTarget(nullptr);
    if (FAILED(hr))
        return fail("Direct2D EndDraw failed");
    SAFE_RELEASE(sourceBitmap);

    d3dDeviceContext->CopyResource(scaleStaging, scaleTarget);

    // Skip the published buffer and the one the stream thread has open. Resizing a buffer
    // it is reading reallocates under it. Two buffers are excluded at most, so one bump past
    // the published index always lands.
    const unsigned int publishedIndex = lastVideoCaptureIndex.load();
    unsigned int nextIndex = (publishedIndex + 1) % BUFFER_COUNT;
    if ((int)nextIndex == activeReadIndex.load())
        nextIndex = (nextIndex + 1) % BUFFER_COUNT;
    D3D11_MAPPED_SUBRESOURCE sr;
    hr = d3dDeviceContext->Map(scaleStaging, 0, D3D11_MAP_READ, 0, &sr);
    if (FAILED(hr))
        return fail("D3DDeviceContext->Map failed");
    captureStats.readback.add(MsSince(readbackStart));

    if (videoCaptures[nextIndex].width != (int)outWidth || videoCaptures[nextIndex].height != (int)outHeight)
    {
        videoCaptures[nextIndex].width = (int)outWidth;
        videoCaptures[nextIndex].height = (int)outHeight;
        videoCaptures[nextIndex].buffer.resize((size_t)outWidth * outHeight * 4);
    }

    const auto copyStart = std::chrono::steady_clock::now();
    for (unsigned int y = 0; y < outHeight; y++)
        memcpy(videoCaptures[nextIndex].buffer.data() + (size_t)y * outWidth * 4, (uint8_t*)sr.pData + (size_t)sr.RowPitch * y, (size_t)outWidth * 4);
    d3dDeviceContext->Unmap(scaleStaging, 0);
    captureStats.copy.add(MsSince(copyStart));

    if (currentSourceOptions.preview)
    {
        const auto previewStart = std::chrono::steady_clock::now();
        captureFunction((int)outWidth, (int)outHeight, videoCaptures[nextIndex].buffer.data());
        captureStats.preview.add(MsSince(previewStart));
    }

    lastVideoCaptureIndex = nextIndex;

    gpuTex->Release();
    if (windowFrame)
        windowFrame.Close();

    captureStats.frames++;
    captureStats.tick.add(MsSince(tickStart));

    return ok;
}

void SetSourceOptions(const SourceOptions* sourceOptions)
{
    newSourceOptions = *sourceOptions;
    hasNewSourceOptions = true;
}