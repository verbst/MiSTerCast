#pragma once

#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include "AudioTimeline.h"

AudioTimeline audioTimeline;
ReceiverAudioClock receiverAudioClock;
bool audioTimelineActive = false;
uint64_t audioModeRevision = 0;
uint32_t audioReconnectEpoch = 0, audioLastDisplayFrame = 0;

int64_t AudioNow100ns()
{
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return (ticks.QuadPart / frequency.QuadPart) * 10000000 +
        (ticks.QuadPart % frequency.QuadPart) * 10000000 / frequency.QuadPart;
}

// REFERENCE_TIME time units per second and per millisecond
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

// Bound each send below CMD_AUDIO's uint16 byte limit, including after stalls.
#define AUDIO_MAX_BYTES   16384
#define AUDIO_MAX_SAMPLES (AUDIO_MAX_BYTES / 2)  // int16 samples, 4096 stereo frames

unsigned int AudioWritePos = 0;                  // in int16 samples, always even
unsigned int AudioShedFrames = 0;                // stereo frames dropped to stay under the cap
std::atomic_int audioSampleRate;
int16_t* audioBuffer = nullptr;                  // points at the client's registered audio buffer

// Audio Capture
REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
UINT32 bufferFrameCount;
UINT32 numFramesAvailable;
IMMDeviceEnumerator *pEnumerator = NULL;
IMMDevice *pDevice = NULL;
IAudioClient *pAudioClient = NULL;
IAudioCaptureClient *pCaptureClient = NULL;
WAVEFORMATEX *pwfx = NULL;
bool audioFormatUsable = false;
DWORD audioComThread = 0;

struct AudioCounters
{
    uint64_t packets = 0, received = 0, converted = 0, discarded = 0, offered = 0, gated = 0;
    uint64_t silent = 0, discontinuities = 0, timestampErrors = 0;
    uint64_t positionGaps = 0, positionRegressions = 0, qpcRegressions = 0;
    uint64_t firstPosition = 0, lastPosition = 0, firstQpc = 0, lastQpc = 0;
    uint64_t validPositions = 0;
} audioCounters;
uint64_t audioExpectedPosition = 0, audioPreviousQpc = 0;
bool audioPositionValid = false;
auto audioReportStart = std::chrono::steady_clock::now();

void ReportAudioCapture(bool final = false)
{
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - audioReportStart).count();
    if (!final && seconds < 2.0)
        return;
    if (diagnosticLogLevel.load() >= 1)
    {
        const auto& a = audioCounters;
        const auto count = [](uint64_t value) { return static_cast<unsigned long long>(value); };
        char line[768];
        snprintf(line, sizeof(line),
            "[audio] elapsed=%.3fs packets=%llu received=%llu converted=%llu discarded=%llu offered=%llu gated=%llu silent=%llu discontinuities=%llu timestampErrors=%llu gapFrames=%llu positionRegressions=%llu qpcRegressions=%llu validPositions=%llu position=%llu..%llu qpc100ns=%llu..%llu final=%d",
            seconds, count(a.packets), count(a.received), count(a.converted), count(a.discarded), count(a.offered), count(a.gated),
            count(a.silent), count(a.discontinuities), count(a.timestampErrors), count(a.positionGaps),
            count(a.positionRegressions), count(a.qpcRegressions), count(a.validPositions),
            count(a.firstPosition), count(a.lastPosition), count(a.firstQpc), count(a.lastQpc), final);
        LogMessage(line);
        const auto& t = audioTimeline.counters;
        snprintf(line, sizeof(line),
            "[audio-timeline] queuedFrames=%u insertedSilence=%llu late=%llu rejected=%llu skipped=%llu rebased=%llu correction=%.2f frames rate=%.1fppm sourceError=%.1f frames maxSendGap=%.2fms final=%d",
            audioTimeline.pending(), count(t.insertedSilence), count(t.late), count(t.rejected),
            count(t.skipped), count(t.rebased), t.rateCorrectionFrames, audioTimeline.ratePpm(),
            audioTimeline.sourceErrorFrames(AudioNow100ns()), t.maxSendGap100ns / 10000.0, final);
        LogMessage(line);
        const auto& c = receiverAudioClock.counters;
        snprintf(line, sizeof(line),
            "[audio-clock] estimatedLead=%lld targetLead=%lld stale=%d staleReports=%llu reanchors=%llu final=%d",
            static_cast<long long>(audioTimeline.outputFrames() - receiverAudioClock.played()),
            static_cast<long long>(receiverAudioClock.lead()), receiverAudioClock.stale(),
            count(c.staleReports), count(c.reanchors), final);
        LogMessage(line);
    }
    audioTimeline.counters = {};
    receiverAudioClock.counters = {};
    audioCounters = {};
    audioReportStart = now;
}

void RecordAudioPacket(UINT32 frames, DWORD flags, UINT64 position, UINT64 qpc)
{
    ++audioCounters.packets;
    audioCounters.received += frames;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        audioCounters.silent += frames;
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        ++audioCounters.discontinuities;
    if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
    {
        ++audioCounters.timestampErrors;
        audioPositionValid = false;
        return;
    }
    if (audioPositionValid)
    {
        if (position > audioExpectedPosition)
            audioCounters.positionGaps += position - audioExpectedPosition;
        else if (position < audioExpectedPosition)
            ++audioCounters.positionRegressions;
        if (qpc < audioPreviousQpc)
            ++audioCounters.qpcRegressions;
    }
    if (audioCounters.validPositions++ == 0)
    {
        audioCounters.firstPosition = position;
        audioCounters.firstQpc = qpc;
    }
    audioCounters.lastPosition = position;
    audioCounters.lastQpc = qpc;
    audioExpectedPosition = position + frames;
    audioPreviousQpc = qpc;
    audioPositionValid = true;
}

std::string AudioDeviceText(const wchar_t* value)
{
    if (!value)
        return "<unavailable>";
    const int size = WideCharToMultiByte(CP_ACP, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "<unavailable>";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_ACP, 0, value, -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}

void LogAudioEndpoint()
{
    LPWSTR id = nullptr;
    IPropertyStore* properties = nullptr;
    PROPVARIANT name;
    PropVariantInit(&name);
    pDevice->GetId(&id);
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &properties)))
        properties->GetValue(PKEY_Device_FriendlyName, &name);
    LogMessage("[audio] endpoint=" + AudioDeviceText(name.vt == VT_LPWSTR ? name.pwszVal : nullptr) +
        " id=" + AudioDeviceText(id) + " role=eRender/eConsole initTid=" + std::to_string(GetCurrentThreadId()));
    PropVariantClear(&name);
    CoTaskMemFree(id);
    SAFE_RELEASE(properties);
}

bool InitAudioCapture()
{
    HRESULT hr;

    hr = CoInitialize(nullptr);
    EXIT_ON_ERROR(hr, "CoInitialize failed");
    audioComThread = GetCurrentThreadId();

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator);
    EXIT_ON_ERROR(hr, "CoCreateInstance of MMDeviceEnumerator failed");

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr, "IMMDeviceEnumerator GetDefaultAudioEndpoint failed");
    LogAudioEndpoint();

    hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        NULL, (void**)&pAudioClient);
    EXIT_ON_ERROR(hr, "IMMDevice Activate failed");

    hr = pAudioClient->GetMixFormat(&pwfx);
    EXIT_ON_ERROR(hr, "IAudioClient GetMixFormat failed");
    audioSampleRate = pwfx->nSamplesPerSec;

    bool floatFormat = pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    std::string extended;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        pwfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const auto* format = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        floatFormat = IsEqualGUID(format->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        wchar_t guid[40] = {};
        StringFromGUID2(format->SubFormat, guid, ARRAYSIZE(guid));
        extended = " validBits=" + std::to_string(format->Samples.wValidBitsPerSample) +
            " channelMask=" + std::to_string(format->dwChannelMask) + " subFormat=" + AudioDeviceText(guid);
    }
    LogMessage("[audio] format tag=" + std::to_string(pwfx->wFormatTag) +
        " rate=" + std::to_string(pwfx->nSamplesPerSec) + " channels=" + std::to_string(pwfx->nChannels) +
        " bits=" + std::to_string(pwfx->wBitsPerSample) + " blockAlign=" + std::to_string(pwfx->nBlockAlign) +
        " bytesPerSecond=" + std::to_string(pwfx->nAvgBytesPerSec) + " extraBytes=" + std::to_string(pwfx->cbSize) + extended);
    audioFormatUsable = floatFormat && pwfx->wBitsPerSample == 32 && pwfx->nChannels >= 1 &&
        pwfx->nBlockAlign == pwfx->nChannels * sizeof(float);
    if (!audioFormatUsable)
    {
        LogMessage("The Windows mix format is not packed 32-bit float. Audio is disabled.", true);
    }
    else if (pwfx->nChannels > 2)
    {
        LogMessage("Windows is mixing " + std::to_string(pwfx->nChannels) +
            " channels; front left/right will be streamed.");
    }

    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsRequestedDuration,
        0,
        pwfx,
        NULL);
    EXIT_ON_ERROR(hr, "IAudioClient Initialize failed");

    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    EXIT_ON_ERROR(hr, "IAudioClient  GetBufferSize failed");
    LogMessage("[audio] capacityFrames=" + std::to_string(bufferFrameCount) +
        " requested100ns=" + std::to_string(hnsRequestedDuration));

    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    EXIT_ON_ERROR(hr, "IAudioClient GetService failed");

    return true;
}

void CleanupAudioCapture()
{
    CoTaskMemFree(pwfx);
    pwfx = nullptr;
    SAFE_RELEASE(pCaptureClient)
    SAFE_RELEASE(pAudioClient)
    SAFE_RELEASE(pEnumerator)
    SAFE_RELEASE(pDevice)
    audioBuffer = nullptr;
    audioFormatUsable = false;
    if (audioComThread == GetCurrentThreadId())
        CoUninitialize();
    else if (audioComThread)
        LogMessage("Audio COM cleanup called on a different thread from Initialize.", true);
    audioComThread = 0;
}

bool StartAudioCapture()
{
    audioTimelineActive = false;
    audioModeRevision = 0;
    audioReconnectEpoch = audioLastDisplayFrame = 0;
    audioTimeline.reset(0, 0, AudioNow100ns());
    AudioWritePos = AudioShedFrames = 0;
    audioCounters = {};
    audioPositionValid = false;
    audioReportStart = std::chrono::steady_clock::now();
    if (!pAudioClient || !audioFormatUsable)
        return false;
    HRESULT hr = pAudioClient->Reset();
    EXIT_ON_ERROR(hr, "IAudioClient Reset failed");
    hr = pAudioClient->Start();
    LogMessage("[audio] Start hr=" + std::to_string(hr));
    EXIT_ON_ERROR(hr, "IAudioClient Start failed");

    return true;
}

bool StopAudioCapture()
{

    HRESULT hr = pAudioClient->Stop();
    LogMessage("[audio] Stop hr=" + std::to_string(hr));
    EXIT_ON_ERROR(hr, "IAudioCaptureClient Stop failed");

     return true;
}

bool TickAudioCapture(bool accepting, unsigned bufferMs, uint32_t displayFrame,
    double fieldMs, uint64_t modeRevision, uint32_t reconnectEpoch)
{
    AudioWritePos = 0;
    const bool resetClock = !audioTimelineActive || modeRevision != audioModeRevision ||
        reconnectEpoch != audioReconnectEpoch || displayFrame < audioLastDisplayFrame;
    if (accepting && resetClock)
    {
        const int64_t now = AudioNow100ns();
        audioTimeline.reset(audioSampleRate.load(), bufferMs, now);
        receiverAudioClock.reset(audioSampleRate.load(), bufferMs, fieldMs, displayFrame, now);
        audioModeRevision = modeRevision;
        audioReconnectEpoch = reconnectEpoch;
        LogMessage("[audio-timeline] started bufferMs=" + std::to_string(bufferMs));
    }
    audioTimelineActive = accepting;
    audioLastDisplayFrame = displayFrame;
    UINT32 packetLength = 0;
    HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
    EXIT_ON_ERROR(hr, "IAudioCaptureClient GetNextPacketSize failed");

    const unsigned int channels = pwfx ? pwfx->nChannels : 2;

    while (packetLength != 0)
    {
        // Get the available data in the shared buffer.
        BYTE *pData;
        DWORD flags;
        UINT64 devicePosition = 0, qpcPosition = 0;
        hr = pCaptureClient->GetBuffer(
            &pData,
            &numFramesAvailable,
            &flags, &devicePosition, &qpcPosition);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient GetBuffer failed");
        RecordAudioPacket(numFramesAvailable, flags, devicePosition, qpcPosition);
        if (accepting && audioBuffer && audioFormatUsable)
        {
            const auto before = audioTimeline.counters;
            audioTimeline.push(reinterpret_cast<const float*>(pData), numFramesAvailable, channels,
                (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, devicePosition, static_cast<int64_t>(qpcPosition),
                (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0,
                (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0);
            const uint64_t dropped = audioTimeline.counters.late - before.late +
                audioTimeline.counters.rejected - before.rejected;
            audioCounters.discarded += dropped;
            audioCounters.converted += numFramesAvailable - dropped;
            AudioShedFrames += static_cast<unsigned>(dropped);
        }
        else
            audioCounters.gated += numFramesAvailable;

        hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient ReleaseBuffer failed");

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient GetNextPacketSize failed");
    }

    if (accepting && audioBuffer)
    {
        const int64_t now = AudioNow100ns();
        int64_t due = receiverAudioClock.due(displayFrame, now);
        if (due - audioTimeline.outputFrames() > AUDIO_MAX_SAMPLES / 2)
        {
            receiverAudioClock.reanchor(displayFrame, now, audioTimeline.outputFrames());
            audioTimeline.rebaseToNow(now);
            due = receiverAudioClock.due(displayFrame, now);
        }
        AudioWritePos = audioTimeline.render(now, due, audioBuffer, AUDIO_MAX_SAMPLES / 2) * 2;
    }
    return true;
}
