// Groovy_MiSTer communication adapted from the Groovy_Mame source.
// See https://github.com/antonioginer/GroovyMAME for original source

// Original license:
// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Antonio Giner, Sergi Clara

// Modification by Shane Lynch

#pragma once

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

std::atomic_bool shouldUpdateVideoMode = false;
nogpu_modeline selected_modeline = {};

#pragma pack()

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
    if (m.pclock <= 0.0 || m.hactive == 0 || m.vactive == 0)
        return ModelineMalformed;
    if (m.hbegin < m.hactive || m.hend < m.hbegin || m.htotal <= m.hend)
        return ModelineMalformed;
    if (m.vbegin < m.vactive || m.vend < m.vbegin || m.vtotal <= m.vend)
        return ModelineMalformed;

    // Byte budget. Mirrors the client's own m_RGBSize arithmetic: the halving
    // applies to interlaced field blits only. Nothing in the client clamps this.
    uint32_t bytes = (uint32_t)m.hactive * m.vactive * BytesPerPixel(rgbMode);
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
        return "Modeline rejected: blanking must enclose the active area and the pixel clock must be positive.";
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
    renderer_nogpu(std::string targetip)
        : m_targetip(targetip)
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
    nogpu_modeline m_current_mode;

    uint64_t time_start = 0;
    uint64_t time_entry = 0;
    uint64_t time_blit = 0;
    uint64_t time_exit = 0;
    uint64_t time_frame[16];
    uint64_t time_frame_avg = 0;
    uint64_t time_frame_dm = 0;
    uint64_t time_sleep = uint64_t(TicksPerSecond() / 1000.0); // 1 ms

    int m_sockfd = -1; //INVALID_SOCKET;
    sockaddr_in m_server_addr;
    std::string m_targetip;

    bool nogpu_init();
    bool nogpu_switch_video_mode();
    void nogpu_register_frametime(uint64_t frametime);
    void nogpu_pack_frame(char* fb);
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
//  Scale the captured desktop into the modeline's active area and write it to
//  the client's registered blit buffer.
//
//  Both the rotation and the pixel-format decisions are resolved once per frame
//  rather than once per pixel: rotation becomes a pair of integer coefficients,
//  and the format selects the loop. The capture is DXGI BGRA, and the wire wants
//  B,G,R - so for RGB888/RGBA the first three source bytes copy straight across.
//============================================================

void renderer_nogpu::nogpu_pack_frame(char* fb)
{
    const unsigned int drawIndex = lastVideoCaptureIndex;
    const Bitmap& capture = videoCaptures[drawIndex];
    const uint8_t* src = capture.buffer.data();
    const int screenwidth = capture.width;
    const int screenheight = capture.height;

    if (screenwidth <= 0 || screenheight <= 0 || src == nullptr)
        return;

    // Which of the two interlaced fields this blit samples.
    bool drawInt = (m_field == 0);
    if (source_config.rotation != Rotation::None)
        drawInt = !drawInt;

    // The 90 degree rotations transpose, so destination x walks the source's
    // vertical axis and the modeline's active area is sampled height-first.
    const bool transposed = (source_config.rotation == Rotation::CW90 ||
                             source_config.rotation == Rotation::CCW90);

    const float stepx = transposed ? ((float)screenwidth / (float)m_height)
                                   : ((float)screenwidth / (float)m_width);
    const float stepy = transposed ? ((float)screenheight / (float)m_width)
                                   : ((float)screenheight / (float)m_height);

    int interlaceStepX = 0;
    int interlaceStepY = 0;
    if (m_current_mode.interlace && drawInt)
    {
        if (transposed)
            interlaceStepX = int(stepx / 2.0f);
        else
            interlaceStepY = int(stepy / 2.0f);
    }

    // Source coordinate as an affine function of the destination coordinate,
    // so the rotation costs no per-pixel branch:
    //   sx = (kxx*x + kxy*y + kx0) * stepx,  sy = (kyx*x + kyy*y + ky0) * stepy
    int kxx = 1, kxy = 0, kx0 = 0;
    int kyx = 0, kyy = 1, ky0 = 0;
    switch (source_config.rotation)
    {
    case Rotation::CW90:
        kxx = 0; kxy = -1; kx0 = (int)m_height - 1;
        kyx = 1; kyy = 0;  ky0 = 0;
        break;
    case Rotation::CCW90:
        kxx = 0; kxy = 1;  kx0 = 0;
        kyx = -1; kyy = 0; ky0 = (int)m_width - 1;
        break;
    case Rotation::Flip180:
        kxx = -1; kxy = 0; kx0 = (int)m_width - 1;
        kyx = 0; kyy = -1; ky0 = (int)m_height - 1;
        break;
    default:
        break;
    }

    auto sourceOffset = [&](unsigned int x, unsigned int y) -> int
    {
        int sx = int((kxx * (int)x + kxy * (int)y + kx0) * stepx) + interlaceStepX;
        int sy = int((kyx * (int)x + kyy * (int)y + ky0) * stepy) + interlaceStepY;

        // Clamp rather than skip: skipping would shift every later pixel.
        if (sx < 0) sx = 0; else if (sx >= screenwidth) sx = screenwidth - 1;
        if (sy < 0) sy = 0; else if (sy >= screenheight) sy = screenheight - 1;

        return (sy * screenwidth + sx) * 4;
    };

    switch (stream_config.rgbMode)
    {
    case RgbMode::Rgba8888:
        for (unsigned int y = 0; y < m_height; y++)
        {
            char* dst = fb + (size_t)y * m_width * 4;
            for (unsigned int x = 0; x < m_width; x++, dst += 4)
            {
                const uint8_t* s = src + sourceOffset(x, y);
                dst[0] = (char)s[0]; // B
                dst[1] = (char)s[1]; // G
                dst[2] = (char)s[2]; // R
                dst[3] = (char)s[3]; // A - carried but not displayed
            }
        }
        break;

    case RgbMode::Rgb565:
        for (unsigned int y = 0; y < m_height; y++)
        {
            uint16_t* dst = (uint16_t*)(fb + (size_t)y * m_width * 2);
            for (unsigned int x = 0; x < m_width; x++, dst++)
            {
                const uint8_t* s = src + sourceOffset(x, y);
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
            char* dst = fb + (size_t)y * m_width * 3;
            for (unsigned int x = 0; x < m_width; x++, dst += 3)
            {
                const uint8_t* s = src + sourceOffset(x, y);
                dst[0] = (char)s[0]; // B
                dst[1] = (char)s[1]; // G
                dst[2] = (char)s[2]; // R
            }
        }
        break;
    }
}

//============================================================
//  renderer_nogpu::draw
//============================================================
void renderer_nogpu::draw()
{
    // Hack because these aren't intiailized...
    m_width = selected_modeline.hactive;
    m_height = selected_modeline.interlace ? selected_modeline.vactive / 2 : selected_modeline.vactive;

    // initialize nogpu right before first blit; retry with a backoff on failure
    // instead of giving up forever (a wrong IP / core not yet running must not
    // pin this thread at 100% CPU doing nothing).
    if (!m_initialized)
    {
        m_initialized = nogpu_init();
        if (m_initialized)
        {
            LogMessage("Done.");
            nogpu_switch_video_mode();
        }
        else
        {
            SleepTicks(TicksPerSecond()); // back off before the next connection attempt
            return;
        }
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

    nogpu_pack_frame(groovyMister.getPBufferBlit(m_field));

    // change video mode right before the blit
    if (shouldUpdateVideoMode)
        nogpu_switch_video_mode();

    time_entry = CurrentTicks();

    if (source_config.syncrefresh && m_first_blit)
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

    if (source_config.framedelay == 0)
        // automatic
        m_frame_delay = std::max((double)(m_period - std::max(m_fd_margin, get_ms(time_frame_dm))) / m_period, 0.0);
    else
    {
        // user defined
        m_frame_delay = (double)(source_config.framedelay) / 10.0;
        vsync_offset = 0;// window().machine().video().vsync_offset();
    }

    // Capture and send audio
    if (source_config.audio)
    {
        TickAudioCapture();
        if (AudioWritePos > 0)
            add_audio_to_recording();
    }

    // Update vsync scanline
    m_vsync_scanline = std::min<int>(int((m_current_mode.vtotal) * m_frame_delay + vsync_offset + 1), m_current_mode.vtotal);

    // Blit now
    groovyMister.CmdBlit(m_frame, m_field, 0/*m_vsync_scanline*/, 15000, 0);
    groovyMister.WaitSync();

    time_blit = CurrentTicks();
    nogpu_register_frametime(time_entry - time_exit);
    time_exit = CurrentTicks();

    return;
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
    if (source_config.audio)
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

    groovyMister.setVerbose(stream_config.verbose);
    groovyMister.setAutoReconnect(stream_config.autoReconnect ? 1 : 0);

    // Pre-init only: these ride CMD_INIT byte[1] and must precede it.
    if (stream_config.codec == CodecNLC)
    {
        groovyMister.setNlcPack(stream_config.nlcPack);
        groovyMister.setNearLevel(stream_config.nearLevel);
    }

    std::string summary = "Sending CMD_INIT... codec " + std::to_string(stream_config.codec);
    if (stream_config.codec == CodecNLC)
    {
        summary += (stream_config.nlcPack == NlcPackRice) ? " (NLC, Rice pack" : " (NLC, TILED pack";
        summary += ", NEAR " + std::to_string(stream_config.nearLevel) + ")";
    }
    summary += ", rgb mode " + std::to_string(stream_config.rgbMode) +
               ", mtu " + std::to_string(stream_config.mtu) +
               ", auto-reconnect " + (stream_config.autoReconnect ? "on" : "off");
    LogMessage(summary);

    int ret = groovyMister.CmdInit(
        m_targetip.c_str(),
        UDP_PORT,
        stream_config.codec,
        soundRate,
        soundChan,
        stream_config.rgbMode,
        stream_config.mtu);

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
    nogpu_modeline *mode = &selected_modeline;
    if (mode == nullptr)
        return false;

    m_current_mode = *mode;

    // Send new modeline to nogpu
    LogMessage("Sending CMD_SWITCHRES...");

    m_width = mode->hactive;
    m_height = mode->vactive;
    m_vtotal = mode->vtotal;
    m_field = 0;

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
        // Unconfirmed: the core has no modeline and will silently discard every
        // subsequent video packet until one lands (no self-recovery on its own).
        // Set shouldUpdateVideoMode so draw() retries this on the next frame,
        // regardless of which call site (initial connect or a user-triggered
        // mode change) got us here.
        LogMessage("CmdSwitchres was not acknowledged; will retry.", true);
        shouldUpdateVideoMode = true;
        return false;
    }

    shouldUpdateVideoMode = false;
    return true;
}

//============================================================
//  renderer_nogpu::nogpu_register_frametime
//============================================================

void renderer_nogpu::nogpu_register_frametime(uint64_t frametime)
{
    static int i = 0;
    static int regs = 0;
    const int max_regs = sizeof(time_frame) / sizeof(time_frame[0]);
    uint64_t acum = 0;
    uint64_t diff = 0;

    // Discard invalid values
    if (frametime <= 0 || get_ms(frametime) > m_period)
        return;

    // Register value and compute current average
    time_frame[i] = frametime;
    i++;

    if (i > max_regs)
        i = 0;

    if (regs < max_regs)
        regs++;

    for (int k = 0; k < regs; k++)
        acum += time_frame[k];

    time_frame_avg = acum / regs;

    // Compute current max deviation
    uint64_t max_diff = 0;

    for (int k = 1; k <= regs; k++)
    {
        diff = time_frame[k] - time_frame[k - 1];

        if (diff > 0 && diff > max_diff)
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
        return;

    // AudioWritePos counts int16 samples and TickAudioCapture caps it at
    // AUDIO_MAX_SAMPLES, so this always fits CmdAudio's uint16 byte count and
    // is always a whole number of stereo frames.
    groovyMister.CmdAudio((uint16_t)(AudioWritePos << 1));
}
