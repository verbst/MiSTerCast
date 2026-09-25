// Groovy_MiSTer communication adapted from the Groovy_Mame source.
// See https://github.com/antonioginer/GroovyMAME for original source

// Original license:
// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Antonio Giner, Sergi Clara

// Modification by Shane Lynch

#pragma once

#include <stdexcept>

uint64_t CurrentTicks()
{
    // use the standard library clock function
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

uint64_t TicksPerSecond() noexcept
{
    LARGE_INTEGER val;
    QueryPerformanceFrequency(&val);
    return val.QuadPart;
}

void SleepTicks(uint64_t duration) noexcept
{
    std::this_thread::sleep_for(std::chrono::high_resolution_clock::duration(duration));
}

inline double get_ms(uint64_t ticks) { return (double)ticks / TicksPerSecond() * 1000; };

// nogpu UDP server
#define UDP_PORT 32100

// Largest active area a fixed-frequency CRT should be asked to sync. Advisory
// rather than protective - the byte budget below is the one that keeps us inside
// the client's frame buffer - so the user can acknowledge past it.
#define CRT_ENVELOPE_WIDTH  1024
#define CRT_ENVELOPE_HEIGHT 576

#pragma pack(1)

typedef struct nogpu_modeline
{
    double    pclock;
    uint16_t  hactive;
    uint16_t  hbegin;
    uint16_t  hend;
    uint16_t  htotal;
    uint16_t  vactive;
    uint16_t  vbegin;
    uint16_t  vend;
    uint16_t  vtotal;
    bool  interlace;
} nogpu_modeline;

nogpu_modeline selected_modeline = {};

#pragma pack()

std::mutex modelineMutex;
uint64_t modelineRevision = 1;

// Applied at the next CmdInit - see StreamOptions in MiSTerCastLib.h.
StreamOptions stream_config = {};

inline uint8_t BytesPerPixel(uint8_t rgbMode)
{
    switch (rgbMode)
    {
    case RgbMode::Rgba8888: return 4;
    case RgbMode::Rgb565:   return 2;
    default:                return 3;
    }
}

//============================================================
//  ValidateModelineFor
//
//  Groovy integration handoff section 4.7. MiSTerCast accepts modelines typed by
//  the user and from modelines.dat, with no switchres preset bounding them, so
//  this is the only thing standing between a mistyped mode and the FPGA - and
//  the only thing keeping a large one inside the client's fixed frame buffer.
//============================================================

inline ModelineValidation ValidateModelineFor(const nogpu_modeline& m, uint8_t rgbMode, bool allowOversize)
{
    // Well-formedness: blanking must enclose the active area, or the core's PLL
    // is driven into an undefined state.
    if (!std::isfinite(m.pclock) || m.pclock <= 0.0 || m.hactive == 0 || m.vactive == 0 ||
        (m.interlace && (m.vactive & 1)))
        return ModelineMalformed;
    if (m.hbegin < m.hactive || m.hend < m.hbegin || m.htotal <= m.hend)
        return ModelineMalformed;
    if (m.vbegin < m.vactive || m.vend < m.vbegin || m.vtotal <= m.vend)
        return ModelineMalformed;

    // Byte budget. Mirrors the client's own m_RGBSize arithmetic: the halving
    // applies to interlaced field blits only. Nothing in the client clamps this.
    uint64_t bytes = (uint64_t)m.hactive * m.vactive * BytesPerPixel(rgbMode);
    if (m.interlace)
        bytes >>= 1;
    if (bytes > BUFFER_SIZE)
        return ModelineOverByteBudget;

    if (!allowOversize && (m.hactive > CRT_ENVELOPE_WIDTH || m.vactive > CRT_ENVELOPE_HEIGHT))
        return ModelineOverCrtEnvelope;

    return ModelineOk;
}

inline const char* ModelineValidationText(ModelineValidation result)
{
    switch (result)
    {
    case ModelineMalformed:
        return "Modeline rejected: require ordered blanking, a finite positive pixel clock, and an even interlaced height.";
    case ModelineOverByteBudget:
        return "Modeline rejected: the frame is larger than the Groovy client's buffer. Reduce the resolution or pick a smaller RGB mode.";
    case ModelineOverCrtEnvelope:
        return "Modeline rejected: larger than a fixed-frequency CRT should be asked to sync. Tick 'Allow oversize modes' to override.";
    default:
        return "Modeline accepted.";
    }
}

// renderer_nogpu is the information for the current screen
class renderer_nogpu
{
public:
    renderer_nogpu(std::string targetip, StreamOptions options, SourceOptions source)
        : m_options(options), m_source(source), m_targetip(targetip)
    {
    }

    ~renderer_nogpu();
    int create();
    void draw();
    void save() {}
    void record() {}
    void toggle_fsfx() {}
    void add_audio_to_recording();

private:
    // npgpu private members
    GroovyMister groovyMister;
    bool m_initialized = false;
    bool m_first_blit = true;
    uint32_t m_lastReconnectEpoch = 0;
    int m_frame = 0;
    int m_field = 0;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
    int m_vtotal = 0;
    int m_vsync_scanline = 0;
    double m_period = 16.666667;
    double m_line_period = 0.064;
    double m_frame_delay = 0.0;
    double m_fd_margin = 1.5;
    nogpu_modeline m_current_mode = {};
    uint64_t m_modeRevision = 0;
    const StreamOptions m_options;
    const SourceOptions m_source;

    uint64_t time_start = 0;
    uint64_t time_entry = 0;
    uint64_t time_blit = 0;
    uint64_t time_exit = 0;
    uint64_t time_frame[16] = {};
    unsigned time_frame_index = 0;
    unsigned time_frame_count = 0;
    uint64_t time_frame_avg = 0;
    uint64_t time_frame_dm = 0;
    uint64_t time_sleep = uint64_t(TicksPerSecond() / 1000.0); // 1 ms

    // Stream-thread telemetry. Reported at log level 1 every 2s, then reset.
    struct
    {
        uint64_t windowStart = 0;
        uint64_t lastEntry = 0;
        unsigned draws = 0;
        unsigned late = 0;          // pack + blit exceeded the frame period
        unsigned frameskip = 0;     // draws where the core reported vgaFrameskip
        unsigned audioFrames = 0;   // stereo frames retained after capture drain
        unsigned audioShed = 0;
        TimingStat period, pack, audio, blit, sync;
    } m_stats;
    void nogpu_report_stats();

    int m_sockfd = -1; //INVALID_SOCKET;
    sockaddr_in m_server_addr;
    std::string m_targetip;

    bool nogpu_init();
    bool nogpu_switch_video_mode();
    void nogpu_register_frametime(uint64_t frametime);
    bool nogpu_pack_frame(char* fb);
};

//============================================================
//  renderer_nogpu::create
//============================================================

int renderer_nogpu::create()
{
    return 0;
}

//============================================================
//  renderer_nogpu::~renderer_nogpu
//============================================================

renderer_nogpu::~renderer_nogpu()
{
    // Wait for fpga to flush last blit
    SleepTicks(uint64_t(m_period * time_sleep));

    LogMessage("Sending CMD_CLOSE...");
    // Issued from the thread that owns the socket, which is what lets the core
    // return to connection-search instead of freezing on the last frame.
    groovyMister.CmdClose();
}

//============================================================
//  renderer_nogpu::nogpu_pack_frame
//
//  The capture thread has already scaled and rotated the frame to the modeline's
//  active area on the GPU, so this is a row repack: pick the field's rows and
//  convert BGRA to the wire format. The capture is DXGI BGRA and the wire wants
//  B,G,R, so for RGB888/RGBA the first three bytes copy straight across.
//============================================================

bool renderer_nogpu::nogpu_pack_frame(char* fb)
{
    const size_t bytes = (size_t)m_width * m_height * BytesPerPixel(m_options.rgbMode);
    if (!fb || bytes == 0 || bytes > BUFFER_SIZE)
    {
        LogMessage("Frame packing rejected: invalid destination size.", true);
        return false;
    }
    const unsigned int drawIndex = lastVideoCaptureIndex;
    const CaptureReadLease lease(drawIndex); // keeps the capture thread off this buffer
    const Bitmap& capture = videoCaptures[drawIndex];
    const uint8_t* src = capture.buffer.data();

    if (capture.width <= 0 || capture.height <= 0 || src == nullptr)
        return false;

    if (capture.buffer.size() < (size_t)capture.width * capture.height * 4)
        return false;

    // A mode change can land before the capture thread has retargeted; clamp rather than
    // read past the buffer.
    const unsigned int cols = (std::min)(m_width, (unsigned int)capture.width);
    const size_t rowStride = (size_t)capture.width * 4;
    // Field 0 takes the odd rows. That is the phase the CPU sampler produced (it offset
    // field 0 by half a step) and the core is tuned to, so keep it.
    const unsigned int rowStep = m_current_mode.interlace ? 2 : 1;
    const unsigned int rowBase = m_current_mode.interlace ? (m_field == 0 ? 1u : 0u) : 0u;

    switch (m_options.rgbMode)
    {
    case RgbMode::Rgba8888:
        for (unsigned int y = 0; y < m_height; y++)
        {
            const unsigned int srcRow = (std::min)(y * rowStep + rowBase, (unsigned int)capture.height - 1);
            memcpy(fb + (size_t)y * m_width * 4, src + (size_t)srcRow * rowStride, (size_t)cols * 4);
        }
        break;

    case RgbMode::Rgb565:
        for (unsigned int y = 0; y < m_height; y++)
        {
            const unsigned int srcRow = (std::min)(y * rowStep + rowBase, (unsigned int)capture.height - 1);
            const uint8_t* s = src + (size_t)srcRow * rowStride;
            uint16_t* dst = (uint16_t*)(fb + (size_t)y * m_width * 2);
            for (unsigned int x = 0; x < cols; x++, s += 4, dst++)
            {
                // Little-endian uint16: R in 15:11, G in 10:5, B in 4:0.
                *dst = (uint16_t)(((s[2] & 0xF8) << 8) |
                                  ((s[1] & 0xFC) << 3) |
                                  ( s[0] >> 3));
            }
        }
        break;

    default: // RgbMode::Rgb888
        for (unsigned int y = 0; y < m_height; y++)
        {
            const unsigned int srcRow = (std::min)(y * rowStep + rowBase, (unsigned int)capture.height - 1);
            const uint8_t* s = src + (size_t)srcRow * rowStride;
            char* dst = fb + (size_t)y * m_width * 3;
            for (unsigned int x = 0; x < cols; x++, s += 4, dst += 3)
            {
                dst[0] = (char)s[0]; // B
                dst[1] = (char)s[1]; // G
                dst[2] = (char)s[2]; // R
            }
        }
        break;
    }
    return true;
}

//============================================================
//  renderer_nogpu::draw
//============================================================
void renderer_nogpu::draw()
{
    // initialize nogpu right before first blit; retry with a backoff on failure
    // instead of giving up forever (a wrong IP / core not yet running must not
    // pin this thread at 100% CPU doing nothing).
    if (!m_initialized)
    {
        m_initialized = nogpu_init();
        if (m_initialized)
        {
            LogMessage("Done.");
        }
        else
        {
            SleepTicks(TicksPerSecond()); // back off before the next connection attempt
            return;
        }
    }

    // The client changes its byte count in CmdSwitchres; pack only an acknowledged mode.
    if (!nogpu_switch_video_mode())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return;
    }

    // A reconnect restarts the core's own frame counter (resetSessionState()
    // zeroes fpga.* + the client's m_frame at the top of every CmdInit) - realign
    // ours to match, or the catch-up logic below (which only corrects the host
    // being BEHIND the core) would leave a stale, much-larger m_frame diverged
    // from the fresh session indefinitely.
    uint32_t reconnectEpoch = groovyMister.reconnectEpoch();
    if (reconnectEpoch != m_lastReconnectEpoch)
    {
        m_lastReconnectEpoch = reconnectEpoch;
        m_frame = 0;
    }

    m_frame++;

    if (groovyMister.fpga.frame > (uint32_t)m_frame)
        m_frame = groovyMister.fpga.frame + 1;

    // get current field for interlaced mode
    if (m_current_mode.interlace)
        m_field = !groovyMister.fpga.vgaF1 ^ ((m_frame - groovyMister.fpga.frame) % 2);
    else
        m_field = 0;

    const uint64_t drawEntry = CurrentTicks();
    if (!m_stats.windowStart)
        m_stats.windowStart = drawEntry;
    if (m_stats.lastEntry)
        m_stats.period.add(get_ms(drawEntry - m_stats.lastEntry));
    m_stats.lastEntry = drawEntry;
    m_stats.draws++;

    if (!nogpu_pack_frame(groovyMister.getPBufferBlit(m_field)))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }
    const uint64_t packEnd = CurrentTicks();
    m_stats.pack.add(get_ms(packEnd - drawEntry));

    time_entry = CurrentTicks();

    if (m_source.syncrefresh && m_first_blit)
    {
        time_start = time_entry;
        time_blit = time_entry;
        time_exit = time_entry;

        m_first_blit = false;
        m_frame = 0;

        // Skip blitting first frame, so we avoid glitches while MAME loads roms
        return;
    }

    int vsync_offset = 0;

    if (m_source.framedelay == 0)
        // automatic
        m_frame_delay = std::max((double)(m_period - std::max(m_fd_margin, get_ms(time_frame_dm))) / m_period, 0.0);
    else
    {
        // user defined
        m_frame_delay = (double)(m_source.framedelay) / 10.0;
        vsync_offset = 0;// window().machine().video().vsync_offset();
    }

    // Update vsync scanline
    m_vsync_scanline = std::min<int>(int((m_current_mode.vtotal) * m_frame_delay + vsync_offset + 1), m_current_mode.vtotal);

    // Blit now
    groovyMister.CmdBlit(m_frame, m_field, 0/*m_vsync_scanline*/, 15000, 0);
    const uint64_t blitEnd = CurrentTicks();
    m_stats.blit.add(get_ms(blitEnd - packEnd));
    if (get_ms(blitEnd - drawEntry) > m_period)
        m_stats.late++;

    groovyMister.WaitSync();
    m_stats.sync.add(get_ms(CurrentTicks() - blitEnd));
    if (groovyMister.fpga.vgaFrameskip)
        m_stats.frameskip++;

    // Send against the latest receiver field count, not the host's frame clock.
    const uint64_t audioStart = CurrentTicks();
    if (m_source.audio)
    {
        if (!TickAudioCapture(groovyMister.isConnected() && groovyMister.fpga.audio,
            m_options.audioBufferMs, groovyMister.fpga.frame, m_period,
            m_modeRevision, groovyMister.reconnectEpoch()))
            throw std::runtime_error("Audio capture failed; stopping stream.");
        if (AudioWritePos > 0)
        {
            m_stats.audioFrames += AudioWritePos / 2;
            add_audio_to_recording();
        }
    }
    m_stats.audio.add(get_ms(CurrentTicks() - audioStart));

    time_blit = CurrentTicks();
    nogpu_register_frametime(time_entry - time_exit);
    time_exit = CurrentTicks();

    if (m_options.verbose >= 1 && get_ms(time_exit - m_stats.windowStart) >= 2000)
        nogpu_report_stats();

    return;
}

//============================================================
//  renderer_nogpu::nogpu_report_stats
//============================================================

void renderer_nogpu::nogpu_report_stats()
{
    const unsigned shed = AudioShedFrames - m_stats.audioShed;
    char line[256];
    snprintf(line, sizeof(line),
        "[stream] draws=%u period %.1f/%.1fms pack %.1f/%.1f audio %.1f/%.1f blit %.1f/%.1f sync %.1f/%.1f late=%u frameskip=%u | audio retained=%u shed=%u",
        m_stats.draws,
        m_stats.period.avg(), m_stats.period.maxMs,
        m_stats.pack.avg(), m_stats.pack.maxMs,
        m_stats.audio.avg(), m_stats.audio.maxMs,
        m_stats.blit.avg(), m_stats.blit.maxMs,
        m_stats.sync.avg(), m_stats.sync.maxMs,
        m_stats.late, m_stats.frameskip,
        m_stats.audioFrames, shed);
    LogMessage(line);

    m_stats.windowStart = time_exit;
    m_stats.draws = m_stats.late = m_stats.frameskip = m_stats.audioFrames = 0;
    m_stats.audioShed = AudioShedFrames;
    m_stats.period.reset(); m_stats.pack.reset(); m_stats.audio.reset();
    m_stats.blit.reset(); m_stats.sync.reset();
}

//============================================================
//  renderer_nogpu::nogpu_init
//============================================================

bool renderer_nogpu::nogpu_init()
{
    // Reset current mode
    m_current_mode = {};

    // The client maps anything outside 22050/44100/48000 to "audio off" in
    // CMD_INIT, which is invisible from here - so say so plainly instead.
    uint32_t soundRate = 0;
    uint8_t soundChan = 0;
    if (m_source.audio)
    {
        switch (audioSampleRate)
        {
        case 22050:
        case 44100:
        case 48000:
            soundRate = (uint32_t)audioSampleRate;
            soundChan = 2; // capture is downmixed to stereo
            LogMessage("Audio rate " + std::to_string((int)audioSampleRate) + " Hz, stereo.");
            break;
        default:
            LogMessage("Windows is mixing at " + std::to_string((int)audioSampleRate) +
                " Hz, which Groovy does not accept (22050, 44100 or 48000 only). Audio is disabled - "
                "set your playback device's format to 48000 Hz and restart the stream.", true);
            break;
        }
    }

    groovyMister.setVerbose(m_options.verbose);
    groovyMister.setAutoReconnect(m_options.autoReconnect ? 1 : 0);

    // Pre-init only: these ride CMD_INIT byte[1] and must precede it.
    if (m_options.codec == CodecNLC)
    {
        groovyMister.setNlcPack(m_options.nlcPack);
        groovyMister.setNearLevel(m_options.nearLevel);
    }

    std::string summary = "Sending CMD_INIT... codec " + std::to_string(m_options.codec);
    if (m_options.codec == CodecNLC)
    {
        summary += (m_options.nlcPack == NlcPackRice) ? " (NLC, Rice pack" : " (NLC, TILED pack";
        summary += ", NEAR " + std::to_string(m_options.nearLevel) + ")";
    }
    summary += ", rgb mode " + std::to_string(m_options.rgbMode) +
               ", mtu " + std::to_string(m_options.mtu) +
               ", auto-reconnect " + (m_options.autoReconnect ? "on" : "off");
    LogMessage(summary);

    int ret = groovyMister.CmdInit(
        m_targetip.c_str(),
        UDP_PORT,
        m_options.codec,
        soundRate,
        soundChan,
        m_options.rgbMode,
        m_options.mtu);

    if (ret == 0)
    {
        audioBuffer = (int16_t*)groovyMister.getPBufferAudio();
        return true;
    }
    else
    {
        LogMessage("Groovy MiSTer API failed to initialize!", true);
        return false;
    }
}

//============================================================
//  renderer_nogpu::nogpu_switch_video_mode()
//============================================================

bool renderer_nogpu::nogpu_switch_video_mode()
{
    nogpu_modeline pending;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> guard(modelineMutex);
        pending = selected_modeline;
        revision = modelineRevision;
    }
    if (revision == m_modeRevision)
        return true;
    if (ValidateModelineFor(pending, m_options.rgbMode, m_options.allowOversizeModes) != ModelineOk)
    {
        LogMessage("Pending modeline is invalid for this stream's RGB format.", true);
        return false;
    }
    const nogpu_modeline* mode = &pending;

    // Send new modeline to nogpu
    LogMessage("Sending CMD_SWITCHRES...");

    int ret = groovyMister.CmdSwitchres(
        mode->pclock,
        mode->hactive,
        mode->hbegin,
        mode->hend,
        mode->htotal,
        mode->vactive,
        mode->vbegin,
        mode->vend,
        mode->vtotal,
        mode->interlace
    );

    if (ret != 0)
    {
        LogMessage("CmdSwitchres was not acknowledged; will retry.", true);
        return false;
    }

    m_current_mode = pending;
    m_modeRevision = revision;
    m_width = mode->hactive;
    m_height = mode->interlace ? mode->vactive / 2 : mode->vactive;
    m_vtotal = mode->vtotal;
    m_field = 0;
    m_line_period = (double)mode->htotal / (mode->pclock * 1000.0);
    m_period = m_line_period * mode->vtotal / (mode->interlace ? 2.0 : 1.0);
    LogMessage("[mode] revision=" + std::to_string(revision) + " width=" + std::to_string(m_width) +
        " packedRows=" + std::to_string(m_height) + " interlace=" + std::to_string(mode->interlace));
    return true;
}

//============================================================
//  renderer_nogpu::nogpu_register_frametime
//============================================================

void renderer_nogpu::nogpu_register_frametime(uint64_t frametime)
{
    const unsigned max_regs = sizeof(time_frame) / sizeof(time_frame[0]);
    uint64_t acum = 0;

    // Discard invalid values
    if (frametime <= 0 || get_ms(frametime) > m_period)
        return;

    // Register value and compute current average
    time_frame[time_frame_index] = frametime;
    time_frame_index = (time_frame_index + 1) % max_regs;
    if (time_frame_count < max_regs)
        ++time_frame_count;

    for (unsigned k = 0; k < time_frame_count; k++)
        acum += time_frame[k];

    time_frame_avg = acum / time_frame_count;

    // Compute current max deviation
    uint64_t max_diff = 0;

    const unsigned oldest = time_frame_count == max_regs ? time_frame_index : 0;
    for (unsigned k = 1; k < time_frame_count; k++)
    {
        const uint64_t current = time_frame[(oldest + k) % max_regs];
        const uint64_t previous = time_frame[(oldest + k - 1) % max_regs];
        const uint64_t diff = current > previous ? current - previous : previous - current;
        if (diff > max_diff)
            max_diff = diff;
    }

    time_frame_dm = max_diff;
}

//============================================================
//  renderer_nogpu::add_audio_to_recording
//============================================================

void renderer_nogpu::add_audio_to_recording()
{
    if (!groovyMister.fpga.audio)
    {
        audioCounters.gated += AudioWritePos / 2;
        return;
    }

    // AudioWritePos counts int16 samples and TickAudioCapture caps it at
    // AUDIO_MAX_SAMPLES, so this always fits CmdAudio's uint16 byte count and
    // is always a whole number of stereo frames.
    groovyMister.CmdAudio((uint16_t)(AudioWritePos << 1));
    audioCounters.offered += AudioWritePos / 2;
}
