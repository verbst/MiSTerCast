#pragma once

#include "AudioProcessing.h"

// REFERENCE_TIME time units per second and per millisecond
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

std::atomic_int audioSampleRate;
int16_t* audioBuffer = nullptr;                  // points at the client's registered audio buffer
std::vector<int16_t> audioCaptureScratch;
unsigned int AudioWritePos = 0;                  // in int16 samples, always even

// Audio Capture
REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
UINT32 bufferFrameCount;
UINT32 numFramesAvailable;
IMMDeviceEnumerator *pEnumerator = NULL;
IMMDevice *pDevice = NULL;
IAudioClient *pAudioClient = NULL;
IAudioCaptureClient *pCaptureClient = NULL;
WAVEFORMATEX *pwfx = NULL;
bool audioCaptureComInitialized = false;
bool audioFormatUsable = false;

bool InitAudioCapture()
{
    HRESULT hr;

    hr = CoInitialize(nullptr);
    EXIT_ON_ERROR(hr, "CoInitialize failed");
    audioCaptureComInitialized = true;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator);
    EXIT_ON_ERROR(hr, "CoCreateInstance of MMDeviceEnumerator failed");

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr, "IMMDeviceEnumerator GetDefaultAudioEndpoint failed");

    hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        NULL, (void**)&pAudioClient);
    EXIT_ON_ERROR(hr, "IMMDevice Activate failed");

    hr = pAudioClient->GetMixFormat(&pwfx);
    EXIT_ON_ERROR(hr, "IAudioClient GetMixFormat failed");
    audioSampleRate = pwfx->nSamplesPerSec;

    // The shared-mode mix format is almost always 32-bit IEEE float, but some
    // endpoints expose other bit depths, and WAVE_FORMAT_EXTENSIBLE can carry
    // a non-float subformat at 32 bits. Verify the actual tag/subformat GUID
    // instead of assuming float-at-32-bit: reinterpreting non-float PCM as
    // float produces loud garbage, not just wrong volume.
    bool floatFormat = pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        floatFormat = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    audioFormatUsable = floatFormat && pwfx->wBitsPerSample == 32 && pwfx->nChannels != 0 &&
        pwfx->nBlockAlign >= pwfx->nChannels * sizeof(float);
    if (!audioFormatUsable)
    {
        // Degrade gracefully: keep the video stream going without audio
        // rather than aborting the whole capture session over a mix format
        // MiSTerCast cannot convert.
        LogMessage("The default audio endpoint does not expose 32-bit floating-point loopback audio. "
            "Streaming will continue without audio.", true);
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
    SAFE_RELEASE(pDevice)
    SAFE_RELEASE(pEnumerator)
    audioBuffer = nullptr;
    audioSampleRate = 0;
    audioFormatUsable = false;
    if (audioCaptureComInitialized)
    {
        CoUninitialize();
        audioCaptureComInitialized = false;
    }
}

bool StartAudioCapture()
{
    HRESULT hr = pAudioClient->Start();
    EXIT_ON_ERROR(hr, "IAudioClient Start failed");

    return true;
}

bool StopAudioCapture()
{

    HRESULT hr = pAudioClient->Stop();
    EXIT_ON_ERROR(hr, "IAudioCaptureClient Stop failed");

     return true;
}

// writeOutput is false while the transport's audio buffer is still owned by
// an outstanding non-blocking send (GroovyMister::CanWriteAudioBuffer()); the
// endpoint is still drained so it cannot back up, the samples are just
// discarded for that tick.
bool TickAudioCapture(bool writeOutput = true)
{
    audioCaptureScratch.clear();
    AudioWritePos = 0;
    UINT32 packetLength = 0;
    HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
    EXIT_ON_ERROR(hr, "IAudioCaptureClient GetNextPacketSize failed");

    const unsigned int channels = pwfx ? pwfx->nChannels : 2;

    while (packetLength != 0)
    {
        // Get the available data in the shared buffer.
        BYTE *pData;
        DWORD flags;
        hr = pCaptureClient->GetBuffer(
            &pData,
            &numFramesAvailable,
            &flags, NULL, NULL);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient GetBuffer failed");

        // Keep draining even when the format is unusable or there is nowhere
        // to put it, otherwise the endpoint buffer backs up and every later
        // packet is discontinuous.
        if (audioFormatUsable)
        {
            const bool silence = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            // WASAPI is polled once per rendered frame, so forward the accumulated
            // samples immediately so loopback capture does not add a prebuffer.
            // If rendering stalled long enough to exceed the protocol's 16-bit byte
            // count, retain the newest audio so latency cannot grow without bound.
            const size_t framesToKeep = std::min<size_t>(numFramesAvailable, mistercast::MaxAudioValuesPerCommand / 2);
            const size_t stereoValues = framesToKeep * 2;
            if (audioCaptureScratch.size() + stereoValues > mistercast::MaxAudioValuesPerCommand)
            {
                const size_t excess = audioCaptureScratch.size() + stereoValues - mistercast::MaxAudioValuesPerCommand;
                std::move(audioCaptureScratch.begin() + excess, audioCaptureScratch.end(), audioCaptureScratch.begin());
                audioCaptureScratch.resize(audioCaptureScratch.size() - excess);
            }

            const size_t writeOffset = audioCaptureScratch.size();
            audioCaptureScratch.resize(writeOffset + stereoValues);
            if (silence)
            {
                std::fill(audioCaptureScratch.begin() + writeOffset, audioCaptureScratch.end(), 0);
            }
            else
            {
                const float* samples = reinterpret_cast<const float*>(pData) +
                    static_cast<size_t>(numFramesAvailable - framesToKeep) * channels;
                if (!mistercast::ConvertFloatFramesToStereo(
                    samples,
                    framesToKeep,
                    static_cast<uint16_t>(channels),
                    audioCaptureScratch.data() + writeOffset,
                    stereoValues))
                {
                    pCaptureClient->ReleaseBuffer(numFramesAvailable);
                    LogMessage("Unable to convert captured audio to stereo PCM.", true);
                    return false;
                }
            }
        }

        hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient ReleaseBuffer failed");

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient GetNextPacketSize failed");
    }

    if (writeOutput && audioBuffer != nullptr && !audioCaptureScratch.empty())
    {
        AudioWritePos = static_cast<unsigned int>(audioCaptureScratch.size());
        std::memcpy(audioBuffer, audioCaptureScratch.data(), AudioWritePos * sizeof(int16_t));
    }

    return true;
}
