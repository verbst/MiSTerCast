#include "pch.h"
#include "MiSTerCastLib.h"
#include "Diagnostics.h"
#include "AudioCapture.h"
#include "VideoCapture.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Winmm.lib")

log_function logFunction = nullptr;
thread_local uint64_t diagnosticSession = 0;
void LogMessage(std::string message, bool error)
{
    if (logFunction != nullptr)
    {
        const std::string tagged = "[pid=" + std::to_string(GetCurrentProcessId()) +
            " tid=" + std::to_string(GetCurrentThreadId()) +
            " session=" + std::to_string(diagnosticSession) + "] " + message;
        logFunction(tagged.c_str(), error);
    }
}

std::atomic_bool stopCapture = false;
std::atomic_bool stopStream = false;
std::string targetIpString;

#include "groovymister.h"
#include "renderer_nogpu.h"

// The GroovyMister client logs to stdout, which a WPF process does not have -
// without this every CmdInit failure reason is invisible to the user.
static void GroovyLogSink(const char* message)
{
    if (message == nullptr)
        return;

    std::string text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    if (!text.empty())
        LogMessage(text);
}

void capture_screen()
{
    WorkerDiagnostics diagnostics("capture", true);
    LogMessage("Screen capture starting.");
    // Raw std::thread, so it has no apartment of its own. The capture session is created
    // on the caller's thread; this covers the WinRT calls made here. Display capture does not
    // need WinRT at all, so a failure here must not take the thread down with it.
    bool apartmentReady = false;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentReady = true;
    }
    catch (const winrt::hresult_error&) { }
    try
    {
        while (!stopCapture)
        {
            TickVideoCapture();
            diagnostics.report();
        }
    }
    catch (const std::exception& error)
    {
        LogMessage(std::string("Capture worker failed: ") + error.what(), true);
    }
    catch (const winrt::hresult_error& error)
    {
        LogMessage("Capture worker failed: " + std::to_string(static_cast<long>(error.code())), true);
    }
    if (apartmentReady)
        winrt::uninit_apartment();
    LogMessage("Screen capture stopped.");
}

void cast_screen(std::string target, StreamOptions options, SourceOptions source, uint64_t session)
{
    diagnosticSession = session;
    WorkerDiagnostics diagnostics("stream", false);
    char normalPriority[2] = {};
    const bool useNormal = GetEnvironmentVariableA("MISTERCAST_DIAG_NORMAL_PRIORITY", normalPriority,
        sizeof(normalPriority)) == 1 && normalPriority[0] == '1';
    if (!SetThreadPriority(GetCurrentThread(), useNormal ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_HIGHEST))
    {
        LogMessage("Setting cast screen thread priority failed: " + std::to_string(GetLastError()), true);
    }

    LogMessage("[stream] priority=" + std::to_string(GetThreadPriority(GetCurrentThread())) +
        " normalOverride=" + std::to_string(useNormal));
    bool audioStarted = false;
    bool audioInitAttempted = false;
    try
    {
        if (source.audio && !stopStream)
        {
            audioInitAttempted = true;
            if (InitAudioCapture())
                audioStarted = StartAudioCapture();
            if (!audioStarted)
                LogMessage("Audio capture unavailable; streaming without audio.", true);
        }
        source.audio = audioStarted;
        if (!stopStream)
        {
            LogMessage("Casting to MiSTer starting.");
            auto renderer = std::make_unique<renderer_nogpu>(target, options, source);
            while (!stopStream)
            {
                renderer->draw();
                diagnostics.report();
                if (audioStarted)
                    ReportAudioCapture();
            }
        }
    }
    catch (const std::exception& error)
    {
        LogMessage(std::string("Stream worker failed: ") + error.what(), true);
    }
    if (audioStarted)
    {
        StopAudioCapture();
        ReportAudioCapture(true);
    }
    audioBuffer = nullptr;
    if (audioInitAttempted)
        CleanupAudioCapture();
    LogMessage("Casting to MiSTer stopped.");
}

bool initialized = false;
std::mutex lifecycleMutex;
std::unique_ptr<std::thread> captureScreenTask;
std::unique_ptr<std::thread> castScreenTask;
uint64_t streamSession = 0;

static void JoinWorker(std::unique_ptr<std::thread>& worker, std::atomic_bool& stop, bool capture)
{
    stop = true;
    if (capture)
        WakeVideoCapture();
    if (worker && worker->joinable())
        worker->join();
    worker.reset();
}

MISTERCASTLIB_API bool Initialize(log_function fnLog, capture_image_function fnCapture)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    if (initialized)
    {
        LogMessage("MiSTerCast is already initialized.", true);
        return true;
    }

    logFunction = fnLog;
    gm_set_log_sink(GroovyLogSink);
    LogMessage("Initializing MiSTerCast");
    LogMessage("[build] native " __DATE__ " " __TIME__ " pointerBits=" + std::to_string(sizeof(void*) * 8));

    source_config.syncrefresh = true;
    source_config.framedelay = 0;

    // Overwritten by the first SetStreamOptions; these are what a save file
    // predating those settings falls back to.
    stream_config.codec = CodecNLC;
    stream_config.nlcPack = NlcPackRice;
    stream_config.nearLevel = 1;
    stream_config.rgbMode = Rgb888;
    stream_config.mtu = 1500;
    stream_config.autoReconnect = true;
    stream_config.verbose = 0;
    stream_config.audioBufferMs = 40;
    stream_config.allowOversizeModes = false;

    selected_modeline.pclock = 6.700;
    selected_modeline.hactive = 320;
    selected_modeline.hbegin = 336;
    selected_modeline.hend = 367;
    selected_modeline.htotal = 426;
    selected_modeline.vactive = 240;
    selected_modeline.vbegin = 244;
    selected_modeline.vend = 247;
    selected_modeline.vtotal = 262;
    selected_modeline.interlace = 0;
    captureOutputWidth = selected_modeline.hactive;
    captureOutputHeight = selected_modeline.vactive;
    

    if (!InitializeVideoCapture(0, fnCapture))
    {
        LogMessage("Failed to initialize video capture.", true);
        CleanupVideoCapture();
        return false;
    }

    // Fill the buffers to be safe
    for (int i = 0; i < BUFFER_COUNT; i++)
        TickVideoCapture();

    stopCapture = false;
    captureScreenTask = std::make_unique<std::thread>(capture_screen);

    LogMessage("MiSTerCast ready.");

    initialized = true;
    return true;
}

MISTERCASTLIB_API bool Shutdown()
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    JoinWorker(castScreenTask, stopStream, false);
    JoinWorker(captureScreenTask, stopCapture, true);
    CleanupVideoCapture();
    delete[] videoCaptures;
    videoCaptures = nullptr;
    lastVideoCaptureIndex = 0;
    initialized = false;
    LogMessage("Shutdown complete; both workers joined.");
    return true;
}

MISTERCASTLIB_API bool StartStream(const char* targetIp)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    if (!initialized || castScreenTask || !targetIp || !*targetIp)
    {
        LogMessage("Start rejected: not initialized, already started, or empty target.", true);
        return false;
    }
    if (ValidateModelineFor(selected_modeline, stream_config.rgbMode, stream_config.allowOversizeModes) != ModelineOk)
        return false;
    LogMessage("Starting stream.");
    targetIpString = std::string(targetIp);
    stopStream = false;
    castScreenTask = std::make_unique<std::thread>(cast_screen, targetIpString,
        stream_config, source_config, ++streamSession);

    return true;
}

MISTERCASTLIB_API bool StopStream()
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    JoinWorker(castScreenTask, stopStream, false);
    LogMessage("Stop complete; stream worker joined.");
    return true;
}

MISTERCASTLIB_API bool SetStreamOptions(
    UINT8 codec,
    UINT8 nlcPack,
    UINT8 nearLevel,
    UINT8 rgbMode,
    UINT16 mtu,
    bool autoReconnect,
    UINT8 verbose,
    bool allowOversizeModes)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    stream_config.codec = codec;
    stream_config.nlcPack = (nlcPack == NlcPackRice) ? NlcPackRice : NlcPackTiled;
    stream_config.nearLevel = (nearLevel > 3) ? 3 : nearLevel;
    stream_config.rgbMode = rgbMode;
    stream_config.mtu = mtu;
    stream_config.autoReconnect = autoReconnect;
    stream_config.verbose = verbose;
    stream_config.allowOversizeModes = allowOversizeModes;
    captureLogLevel = verbose;
    diagnosticLogLevel = verbose;

    return true;
}

MISTERCASTLIB_API bool SetAudioBufferMs(UINT16 milliseconds)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    if (milliseconds > 200)
        return false;
    stream_config.audioBufferMs = milliseconds;
    return true;
}

MISTERCASTLIB_API int ValidateModeline(
    double pclock,
    UINT16 hactive,
    UINT16 hbegin,
    UINT16 hend,
    UINT16 htotal,
    UINT16 vactive,
    UINT16 vbegin,
    UINT16 vend,
    UINT16 vtotal,
    bool interlace,
    UINT8 rgbMode,
    bool allowOversizeModes)
{
    nogpu_modeline candidate = {};
    candidate.pclock = pclock;
    candidate.hactive = hactive;
    candidate.hbegin = hbegin;
    candidate.hend = hend;
    candidate.htotal = htotal;
    candidate.vactive = vactive;
    candidate.vbegin = vbegin;
    candidate.vend = vend;
    candidate.vtotal = vtotal;
    candidate.interlace = interlace;

    return (int)ValidateModelineFor(candidate, rgbMode, allowOversizeModes);
}

MISTERCASTLIB_API bool SetModeline(
    double pclock,
    UINT16 hactive,
    UINT16 hbegin,
    UINT16 hend,
    UINT16 htotal,
    UINT16 vactive,
    UINT16 vbegin,
    UINT16 vend,
    UINT16 vtotal,
    bool interlace)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    LogMessage("SetModeline called");

    nogpu_modeline candidate = {};
    candidate.pclock = pclock;
    candidate.hactive = hactive;
    candidate.hbegin = hbegin;
    candidate.hend = hend;
    candidate.htotal = htotal;
    candidate.vactive = vactive;
    candidate.vbegin = vbegin;
    candidate.vend = vend;
    candidate.vtotal = vtotal;
    candidate.interlace = interlace;

    // Refuse rather than blit past the client's buffer or drive the core's PLL
    // into an undefined state. The previous mode stays in force.
    ModelineValidation result = ValidateModelineFor(
        candidate, stream_config.rgbMode, stream_config.allowOversizeModes);
    if (result != ModelineOk)
    {
        LogMessage(ModelineValidationText(result), true);
        return false;
    }

    {
        std::lock_guard<std::mutex> modeGuard(modelineMutex);
        selected_modeline = candidate;
        ++modelineRevision;
    }
    captureOutputWidth = candidate.hactive;
    captureOutputHeight = candidate.vactive;

    return true;
}

// Capture cannot be swapped under the worker, so stop it, swap, then restart. Options are
// published while it is stopped: publish earlier and the dying worker consumes them instead.
static bool RestartVideoCapture()
{
    JoinWorker(captureScreenTask, stopCapture, true);
    CleanupVideoCapture();
    const bool ok = InitializeVideoCapture(source_config.display, captureFunction);
    SetSourceOptions(&source_config);
    stopCapture = false;
    captureScreenTask = std::make_unique<std::thread>(capture_screen);
    return ok;
}

MISTERCASTLIB_API bool SetSource(
    UINT8 display,
    bool audio,
    bool preview,
    UINT8 alignment,
    UINT8 cropmode,
    UINT16 xcrop,
    UINT16 ycrop,
    INT16 xoffset,
    INT16 yoffset,
    UINT8 rotation)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    source_config.display = display;
    source_config.audio = audio;
    source_config.preview = preview;
    source_config.alignment = (Alignment)alignment;
    source_config.cropmode = (CropMode)cropmode;
    source_config.width = xcrop;
    source_config.height = ycrop;
    source_config.xoffset = xoffset;
    source_config.yoffset = yoffset;
    source_config.rotation = rotation;

    switch (cropmode)
    {
    case CropMode::X1:
        source_config.width = selected_modeline.hactive;
        source_config.height = selected_modeline.vactive;
        break;
    case CropMode::X2:
        source_config.width = selected_modeline.hactive * 2;
        source_config.height = selected_modeline.vactive * 2;
        break;
    case CropMode::X3:
        source_config.width = selected_modeline.hactive * 3;
        source_config.height = selected_modeline.vactive * 3;
        break;
    case CropMode::X4:
        source_config.width = selected_modeline.hactive * 4;
        source_config.height = selected_modeline.vactive * 4;
        break;
    case CropMode::X5:
        source_config.width = selected_modeline.hactive * 5;
        source_config.height = selected_modeline.vactive * 5;
        break;
    default:
        break;
    }

    if (displayIndex != source_config.display)
        RestartVideoCapture();
    else
        SetSourceOptions(&source_config);

    return true;
}

MISTERCASTLIB_API bool SetCaptureWindow(UINT_PTR windowHandle)
{
    std::lock_guard<std::mutex> guard(lifecycleMutex);
    if (!initialized)
    {
        LogMessage("MiSTerCast must be initialized before selecting a capture window.", true);
        return false;
    }
    if (windowHandle != 0 && !IsWindow(reinterpret_cast<HWND>(windowHandle)))
    {
        LogMessage("The selected capture window no longer exists.", true);
        return false;
    }
    if (source_config.windowHandle == windowHandle)
        return true;

    source_config.windowHandle = windowHandle;

    if (!RestartVideoCapture())
        LogMessage("Failed to initialize the selected video capture source.", true);

    return true;
}
