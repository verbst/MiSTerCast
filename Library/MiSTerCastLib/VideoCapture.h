#pragma once

#define BUFFER_COUNT 3


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
    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported())
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
        // BGRA_SUPPORT: required by CreateDirect3D11DeviceFromDXGIDevice, unused otherwise.
        hr = D3D11CreateDevice(nullptr, driverTypes[i], nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, (UINT)numFeatureLevels,
            D3D11_SDK_VERSION, &d3dDevice, &featureLevel, &d3dDeviceContext);
        if (SUCCEEDED(hr))
            break;
    }

    EXIT_ON_ERROR(hr, "D3D11CreateDevice failed");

    IDXGIDevice* dxgiDevice = nullptr;
    hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    EXIT_ON_ERROR(hr, "D3DDevice->QueryInterface failed");

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
                    windowFrame.Close();
                windowFrame = next;
            }

            if (!windowFrame)
            {
                // Block like the desktop path's AcquireNextFrame(32). A window with static
                // content produces no frames, so the timeout doubles as the liveness recheck.
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
            return false;

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
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;

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

    desc.Width = width;
    desc.Height = height;

    ID3D11Texture2D* cpuTex = nullptr;
    hr = d3dDevice->CreateTexture2D(&desc, nullptr, &cpuTex);
    EXIT_ON_ERROR(hr, "D3DDevice->CreateTexture2D failed");

    D3D11_BOX sourceRegion;
    sourceRegion.left = xoffset;
    sourceRegion.right = xoffset + width;
    sourceRegion.top = yoffset;
    sourceRegion.bottom = yoffset + height;
    sourceRegion.front = 0;
    sourceRegion.back = 1;

    d3dDeviceContext->CopySubresourceRegion(
        cpuTex,
        0, // sub resource
        0, //x
        0, //y
        0, //z
        gpuTex,
        0, // sub resource
        &sourceRegion);

    // Skip the published buffer and the one the stream thread has open. Resizing a buffer
    // it is reading reallocates under it, which Full Source on a dragged window hits often.
    // Two buffers are excluded at most, so one bump past the published index always lands.
    const unsigned int publishedIndex = lastVideoCaptureIndex.load();
    unsigned int nextIndex = (publishedIndex + 1) % BUFFER_COUNT;
    if ((int)nextIndex == activeReadIndex.load())
        nextIndex = (nextIndex + 1) % BUFFER_COUNT;
    D3D11_MAPPED_SUBRESOURCE sr;
    hr = d3dDeviceContext->Map(cpuTex, 0, D3D11_MAP_READ, 0, &sr);
    EXIT_ON_ERROR(hr, "D3DDeviceContext->Map failed");

    if (videoCaptures[nextIndex].width != width || videoCaptures[nextIndex].height != height)
    {
        videoCaptures[nextIndex].width = width;
        videoCaptures[nextIndex].height = height;
        videoCaptures[nextIndex].buffer.resize(width * height * 4);
    }

    for (int y = 0; y < (int)height; y++) // TODO: Can this be improved?
        memcpy(videoCaptures[nextIndex].buffer.data() + y * width * 4, (uint8_t*)sr.pData + sr.RowPitch * y, width * 4);
    d3dDeviceContext->Unmap(cpuTex, 0);

    if (currentSourceOptions.preview)
        captureFunction(width, height, videoCaptures[nextIndex].buffer.data());

    lastVideoCaptureIndex = nextIndex;

    cpuTex->Release();
    gpuTex->Release();
    if (windowFrame)
        windowFrame.Close();

    return ok;
}

void SetSourceOptions(const SourceOptions* sourceOptions)
{
    newSourceOptions = *sourceOptions;
    hasNewSourceOptions = true;
}