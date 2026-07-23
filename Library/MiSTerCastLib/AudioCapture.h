#pragma once

// REFERENCE_TIME time units per second and per millisecond
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

// Buffers
//
// CmdAudio takes its byte count in a uint16, so one drain must stay well under
// 65535: a larger backlog truncates, and an exact multiple of 65536 puts an
// empty CMD_AUDIO on the wire that the core rejects with UDP_ERROR. That is not
// hypothetical - the WASAPI loopback buffer below is a full second deep, so any
// stall (notably the gap between the stream starting and the first video frame)
// builds one. 16 KB is ~85 ms at 48 kHz stereo; steady state is ~3.2 KB/frame.
#define AUDIO_MAX_BYTES   16384
#define AUDIO_MAX_SAMPLES (AUDIO_MAX_BYTES / 2)  // int16 samples, 4096 stereo frames

unsigned int AudioWritePos = 0;                  // in int16 samples, always even
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

bool InitAudioCapture()
{
    HRESULT hr;

    hr = CoInitialize(nullptr);
    EXIT_ON_ERROR(hr, "CoInitialize failed");

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
    audioSampleRate = pwfx->nSamplesPerSec;
    EXIT_ON_ERROR(hr, "IAudioClient GetMixFormat failed");

    // The shared-mode mix format is 32-bit float in every shipping configuration
    // of WASAPI, and the capture path below reads it as such. Refuse rather than
    // reinterpret if that ever stops being true.
    audioFormatUsable = (pwfx->wBitsPerSample == 32 && pwfx->nChannels >= 1);
    if (!audioFormatUsable)
    {
        LogMessage("Windows is mixing at " + std::to_string(pwfx->wBitsPerSample) +
            " bits per sample, which MiSTerCast cannot convert. Audio is disabled.", true);
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

void CleanupAudioCatpure()
{
    CoTaskMemFree(pwfx);
    SAFE_RELEASE(pEnumerator)
    SAFE_RELEASE(pDevice)
    SAFE_RELEASE(pAudioClient)
    SAFE_RELEASE(pCaptureClient)
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

// Float sample to signed 16-bit LE. The clamp matters: mixers with DC filters
// and volume scaling overshoot +-1.0, and an unclamped sample wraps into an
// audible click rather than clipping.
inline int16_t AudioSampleToS16(float sample)
{
    if (sample > 1.0f)
        sample = 1.0f;
    else if (sample < -1.0f)
        sample = -1.0f;

    return (int16_t)(sample * 32767.0f);
}

bool TickAudioCapture()
{
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

        // Keep draining even when there is nowhere to put it, otherwise the
        // endpoint buffer backs up and every later packet is discontinuous.
        if (audioBuffer && audioFormatUsable)
        {
            const bool silence = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const float* pDataFloat = (const float*)pData;
            const unsigned int capFrames = AUDIO_MAX_SAMPLES / 2;

            // Shed the oldest audio so latency self-corrects after a stall
            // instead of accumulating. Done in whole stereo frames, and at most
            // one move per packet - trimming a frame at a time would be
            // quadratic on the backlog this exists to handle.
            UINT32 firstFrame = 0;
            if (numFramesAvailable > capFrames)
            {
                // This packet alone overflows; everything older is superseded.
                firstFrame = numFramesAvailable - capFrames;
                AudioWritePos = 0;
            }

            const UINT32 framesToWrite = numFramesAvailable - firstFrame;
            const unsigned int heldFrames = AudioWritePos / 2;
            if (heldFrames + framesToWrite > capFrames)
            {
                const unsigned int dropFrames = heldFrames + framesToWrite - capFrames;
                const unsigned int keepSamples = AudioWritePos - dropFrames * 2;
                memmove(audioBuffer, audioBuffer + dropFrames * 2, keepSamples * sizeof(int16_t));
                AudioWritePos = keepSamples;
            }

            for (UINT32 frame = firstFrame; frame < numFramesAvailable; frame++)
            {
                if (silence)
                {
                    audioBuffer[AudioWritePos] = 0;
                    audioBuffer[AudioWritePos + 1] = 0;
                }
                else
                {
                    const float* srcFrame = pDataFloat + (size_t)frame * channels;
                    const int16_t left = AudioSampleToS16(srcFrame[0]);
                    const int16_t right = (channels >= 2) ? AudioSampleToS16(srcFrame[1]) : left;
                    audioBuffer[AudioWritePos] = left;
                    audioBuffer[AudioWritePos + 1] = right;
                }

                AudioWritePos += 2;
            }
        }

        hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient ReleaseBuffer failed");

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        EXIT_ON_ERROR(hr, "IAudioCaptureClient GetNextPacketSize failed");
    }

    return true;
}
