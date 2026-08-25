#pragma once

#include "CaptureResources.h"
#include "SourceOptionsState.h"

#define BUFFER_COUNT 3


struct Bitmap {
    int                  width = 0;
    int                  height = 0;
    Rotation             rotation = Rotation::None;
    std::vector<uint8_t> buffer;
};

SourceOptionsState source_config;
std::atomic_uint lastVideoCaptureIndex = 0;
std::atomic<uint64_t> videoCaptureSequence = 0;
Bitmap* videoCaptures = nullptr;
int    displayIndex = 0;
ID3D11Device*           d3dDevice = nullptr;
ID3D11DeviceContext*    d3dDeviceContext = nullptr;
IDXGIOutputDuplication* desktopDuplication = nullptr;
ID3D11Texture2D*       stagingTexture = nullptr;
mistercast::StagingTextureSpec stagingTextureSpec = {};
bool                    haveStagingTextureSpec = false;
bool                    haveFrameLock = false;
capture_image_function  captureFunction;
UINT_PTR                activeWindowHandle = 0;
bool                    windowUnavailableLogged = false;
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice windowDirect3DDevice{ nullptr };
winrt::Windows::Graphics::Capture::GraphicsCaptureItem windowCaptureItem{ nullptr };
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool windowFramePool{ nullptr };
winrt::Windows::Graphics::Capture::GraphicsCaptureSession windowCaptureSession{ nullptr };
winrt::Windows::Graphics::SizeInt32 windowCaptureSize = {};

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

bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& desc)
{
    const mistercast::StagingTextureSpec requested = {
        desc.Width,
        desc.Height,
        static_cast<uint32_t>(desc.Format),
    };
    if (stagingTexture != nullptr && haveStagingTextureSpec &&
        mistercast::SameStagingTexture(stagingTextureSpec, requested))
    {
        return true;
    }

    SAFE_RELEASE(stagingTexture);
    haveStagingTextureSpec = false;
    const HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTexture);
    if (FAILED(hr))
    {
        LogMessage("D3DDevice->CreateTexture2D failed: " + std::to_string(hr), true);
        return false;
    }

    stagingTextureSpec = requested;
    haveStagingTextureSpec = true;
    LogMessage("[capture] Created reusable staging texture " +
        std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ".");
    return true;
}

bool InitializeVideoCapture(int outputNumber, capture_image_function fnCapture)
{
    displayIndex = outputNumber;
    captureFunction = fnCapture;
    const UINT_PTR requestedWindowHandle = source_config.snapshot().windowHandle;

    if (videoCaptures == nullptr)
    {
        videoCaptures = new Bitmap[BUFFER_COUNT];
        for (int i = 0; i < BUFFER_COUNT; i++)
            videoCaptures[i] = Bitmap();
    }

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

    constexpr int duplicateOutputAttempts = 10;
    for (int attempt = 0; attempt < duplicateOutputAttempts; ++attempt)
    {
        hr = dxgiOutput1->DuplicateOutput(d3dDevice, &desktopDuplication);
        if (SUCCEEDED(hr) || (hr != E_ACCESSDENIED && hr != DXGI_ERROR_ACCESS_LOST))
            break;
        if (attempt + 1 < duplicateOutputAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    dxgiOutput1->Release();
    dxgiOutput1 = nullptr;
    EXIT_ON_ERROR(hr, "DxgiOutput1->DuplicateOutput failed");

    return true;
}

void CleanupVideoCapture()
{
    try
    {
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
    windowCaptureSession = nullptr;
    windowFramePool = nullptr;
    windowCaptureItem = nullptr;
    windowDirect3DDevice = nullptr;
    windowCaptureSize = {};
    activeWindowHandle = 0;
    windowUnavailableLogged = false;
    SAFE_RELEASE(stagingTexture);
    SAFE_RELEASE(desktopDuplication);
    SAFE_RELEASE(d3dDeviceContext);
    SAFE_RELEASE(d3dDevice);
    haveFrameLock = false;
    haveStagingTextureSpec = false;
    stagingTextureSpec = {};
}

bool TickVideoCapture()
{
    const SourceOptions currentSourceOptions = source_config.snapshot();
    const bool captureWindow = currentSourceOptions.windowHandle != 0;
    if ((!captureWindow && !desktopDuplication) || (captureWindow && !windowFramePool))
    {
        // Desktop switches, secure-desktop prompts, and window recreation can
        // invalidate capture temporarily. Keep the worker alive so it can
        // recover after the source becomes available again.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CleanupVideoCapture();
        InitializeVideoCapture(displayIndex, captureFunction);
        return false;
    }

    HRESULT hr;
    ID3D11Texture2D* gpuTex = nullptr;
    winrt::Windows::Graphics::SizeInt32 capturedContentSize = {};
    bool resizeWindowFramePool = false;
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
            auto frame = windowFramePool.TryGetNextFrame();
            if (!frame)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return false;
            }

            capturedContentSize = frame.ContentSize();
            if (capturedContentSize.Width <= 0 || capturedContentSize.Height <= 0)
            {
                frame.Close();
                return false;
            }
            auto surfaceAccess = frame.Surface().as<
                Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            hr = surfaceAccess->GetInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&gpuTex));
            frame.Close();
            EXIT_ON_ERROR(hr, "Getting the single-window capture texture failed");
            resizeWindowFramePool =
                capturedContentSize.Width != windowCaptureSize.Width ||
                capturedContentSize.Height != windowCaptureSize.Height;
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
    switch (currentSourceOptions.rotation)
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
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

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
        switch (currentSourceOptions.rotation)
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
        switch (currentSourceOptions.rotation)
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

    if (!EnsureStagingTexture(desc))
    {
        gpuTex->Release();
        return false;
    }

    D3D11_BOX sourceRegion;
    sourceRegion.left = xoffset;
    sourceRegion.right = xoffset + width;
    sourceRegion.top = yoffset;
    sourceRegion.bottom = yoffset + height;
    sourceRegion.front = 0;
    sourceRegion.back = 1;

    d3dDeviceContext->CopySubresourceRegion(
        stagingTexture,
        0, // sub resource
        0, //x
        0, //y
        0, //z
        gpuTex,
        0, // sub resource
        &sourceRegion);

    unsigned int nextIndex = (lastVideoCaptureIndex + 1) % BUFFER_COUNT;
    D3D11_MAPPED_SUBRESOURCE sr;
    hr = d3dDeviceContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &sr);
    if (FAILED(hr))
    {
        LogMessage("D3DDeviceContext->Map failed: " + std::to_string(hr), true);
        gpuTex->Release();
        return false;
    }

    if (videoCaptures[nextIndex].width != width || videoCaptures[nextIndex].height != height)
    {
        videoCaptures[nextIndex].width = width;
        videoCaptures[nextIndex].height = height;
        videoCaptures[nextIndex].buffer.resize(width * height * 4);
    }
    videoCaptures[nextIndex].rotation = currentSourceOptions.rotation;

    for (int y = 0; y < (int)height; y++) // TODO: Can this be improved?
        memcpy(videoCaptures[nextIndex].buffer.data() + y * width * 4, (uint8_t*)sr.pData + sr.RowPitch * y, width * 4);
    d3dDeviceContext->Unmap(stagingTexture, 0);

    if (currentSourceOptions.preview)
        captureFunction(width, height, videoCaptures[nextIndex].buffer.data());

    lastVideoCaptureIndex = nextIndex;
    videoCaptureSequence.fetch_add(1, std::memory_order_relaxed);

    gpuTex->Release();

    if (captureWindow && resizeWindowFramePool)
    {
        try
        {
            windowFramePool.Recreate(
                windowDirect3DDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                capturedContentSize);
            windowCaptureSize = capturedContentSize;
            LogMessage("[capture] Selected window resized to " +
                std::to_string(windowCaptureSize.Width) + "x" +
                std::to_string(windowCaptureSize.Height) + ".");
        }
        catch (const winrt::hresult_error& error)
        {
            LogMessage("Resizing the single-window capture pool failed: " +
                std::to_string(static_cast<long>(error.code())) + ".", true);
            CleanupVideoCapture();
            InitializeVideoCapture(displayIndex, captureFunction);
            return false;
        }
    }

    return ok;
}
