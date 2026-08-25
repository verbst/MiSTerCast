// Groovy_MiSTer communication adapted from the Groovy_Mame source.
// See https://github.com/antonioginer/GroovyMAME for original source

// Original license:
// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Antonio Giner, Sergi Clara

// Modification by Shane Lynch

#pragma once

#include "AudioProcessing.h"
#include "DiagnosticFaults.h"
#include "StreamingTiming.h"

#include <iomanip>
#include <sstream>

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
    bool  progressiveFramebuffer;
} nogpu_modeline;

std::atomic_bool shouldUpdateVideoMode = false;
nogpu_modeline selected_modeline = {};
std::mutex selected_modeline_mutex;

nogpu_modeline selected_modeline_snapshot()
{
    std::lock_guard<std::mutex> lock(selected_modeline_mutex);
    return selected_modeline;
}

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
    // applies to interlaced field blits only (a progressive framebuffer still
    // sends the full active height every blit). Nothing in the client clamps this.
    uint32_t bytes = (uint32_t)m.hactive * m.vactive * BytesPerPixel(rgbMode);
    if (m.interlace && !m.progressiveFramebuffer)
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
    renderer_nogpu(
        std::string targetip,
        bool audioEnabled,
        mistercast::StreamFaultOptions diagnosticFaults = {})
        : m_targetip(targetip),
          m_audio_enabled(audioEnabled),
          m_fault_schedule(diagnosticFaults)
    {
    }

    ~renderer_nogpu();
    int create();
    void draw();
    void save() {}
    void record() {}
    void toggle_fsfx() {}

private:
    // npgpu private members
    GroovyMister groovyMister;
    bool m_initialized = false;
    bool m_first_blit = true;
    uint32_t m_lastReconnectEpoch = 0;
    uint32_t m_frame = 0;
    uint8_t m_field = 0;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
    int m_vtotal = 0;
    int m_vsync_scanline = 0;
    double m_period = 16.666667;
    double m_line_period = 0.064;
    double m_frame_delay = 0.0;
    double m_fd_margin = 1.5;
    nogpu_modeline m_current_mode;

    uint64_t m_diagnostics_start = 0;
    uint64_t m_diagnostics_last_draw = 0;
    uint64_t m_diagnostics_last_capture = 0;
    uint64_t m_diagnostics_max_gap = 0;
    uint64_t m_diagnostics_max_transform = 0;
    uint64_t m_diagnostics_max_send_wait = 0;
    uint64_t m_diagnostics_frames = 0;
    uint64_t m_diagnostics_captures = 0;
    uint64_t m_diagnostics_capture_repeats = 0;
    uint32_t m_diagnostics_status_updates = 0;
    uint32_t m_diagnostics_f1_changes = 0;
    uint32_t m_diagnostics_field_repeats = 0;
    uint32_t m_diagnostics_frame_realignments = 0;
    uint32_t m_diagnostics_last_frame = 0;
    uint32_t m_diagnostics_last_fpga_frame = 0;
    uint32_t m_diagnostics_last_frame_echo = 0;
    uint16_t m_diagnostics_last_vcount = 0;
    uint16_t m_diagnostics_last_vcount_echo = 0;
    uint8_t m_diagnostics_last_field = 0;
    uint8_t m_diagnostics_last_f1 = 0;
    bool m_diagnostics_have_sample = false;
    bool m_transport_failure_logged = false;
    bool m_capture_failure_logged = false;
    bool m_audio_enabled = false;
    bool m_phase_lock_reported = false;
    mistercast::StreamFaultSchedule m_fault_schedule;
    uint32_t m_diagnostics_injected_skips = 0;
    uint32_t m_diagnostics_injected_stalls = 0;

    uint64_t time_start = 0;
    uint64_t time_entry = 0;
    uint64_t time_blit = 0;
    uint64_t time_exit = 0;
    uint64_t time_frame[16];
    uint64_t time_frame_avg = 0;
    uint64_t time_frame_dm = 0;

    int m_sockfd = -1; //INVALID_SOCKET;
    sockaddr_in m_server_addr;
    std::string m_targetip;

    bool nogpu_init();
    bool nogpu_switch_video_mode();
    void nogpu_register_frametime(uint64_t frametime);
    void nogpu_log_diagnostics(
        uint64_t draw_start,
        uint64_t transform_end,
        uint64_t send_start,
        uint64_t send_end,
        uint64_t capture_sequence);
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
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(m_period));

    LogMessage("Sending CMD_CLOSE...");
    // Issued from the thread that owns the socket, which is what lets the core
    // return to connection-search instead of freezing on the last frame.
    groovyMister.CmdClose();
}

//============================================================
//  renderer_nogpu::draw
//============================================================
void renderer_nogpu::draw()
{
    const SourceOptions source = source_config.snapshot();

    // initialize nogpu right before first blit
    if (m_first_blit && !m_initialized)
    {
        m_initialized = nogpu_init();
        if (m_initialized)
        {
            LogMessage("Done.");
            shouldUpdateVideoMode.store(false, std::memory_order_release);
            nogpu_switch_video_mode();
        }
        else
        {
            m_first_blit = false;
        }
    }

    // only send frame if nogpu is initialized
    if (!m_initialized)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    // Apply the receiver mode before selecting a field and transforming the
    // next payload. Applying it later mixed the new active size with the old
    // height/interlace layout for one frame during a live switch.
    if (shouldUpdateVideoMode.exchange(false, std::memory_order_acq_rel))
        nogpu_switch_video_mode();

    const groovyMisterDiagnostics transportState = groovyMister.getDiagnostics();
    if (!transportState.connected)
    {
        // An armed auto-reconnect watchdog can still bring the session back on
        // its own (see GroovyMister::CmdBlit) - only give up on it here if the
        // feature is off, so a transient drop isn't reported as fatal.
        if (!stream_config.autoReconnect)
        {
            if (!m_transport_failure_logged)
            {
                LogMessage("The streaming transport stopped after a network error. Stop and restart the stream to retry.", true);
                m_transport_failure_logged = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }
    m_transport_failure_logged = false;

    // A reconnect restarts the core's own frame counter (resetSessionState()
    // zeroes fpga.* + the client's m_frame at the top of every CmdInit) - realign
    // ours to match, or the raster-sync spread guard in DiffTimeRaster would see
    // a permanently huge gap between our climbing counter and the core's fresh
    // one and never resume correcting.
    const uint32_t reconnectEpoch = transportState.reconnectEpoch;
    if (reconnectEpoch != m_lastReconnectEpoch)
    {
        m_lastReconnectEpoch = reconnectEpoch;
        m_frame = 0;
        m_phase_lock_reported = !m_current_mode.interlace;
    }

    const uint64_t diagnostics_draw_start = CurrentTicks();

    unsigned int drawIndex = lastVideoCaptureIndex;
    const uint64_t capture_sequence = videoCaptureSequence.load(std::memory_order_relaxed);
    int screenwidth = videoCaptures[drawIndex].width;
    int screenheight = videoCaptures[drawIndex].height;
    if (screenwidth <= 0 || screenheight <= 0 || videoCaptures[drawIndex].buffer.empty())
    {
        if (!m_capture_failure_logged)
        {
            LogMessage("Waiting for the first captured frame.");
            m_capture_failure_logged = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }

    m_frame++;

    groovyMister.AlignFrame(m_frame, m_field);
    if (m_current_mode.progressiveFramebuffer)
        m_field = 0;

    char* fb = groovyMister.getPBufferBlit(m_field);
    const size_t outputSize = Rgb24FrameSize(
        m_width,
        m_current_mode.vactive,
        m_current_mode.interlace,
        m_current_mode.progressiveFramebuffer);
    if (!TransformBgraToRgb24(
        videoCaptures[drawIndex].buffer.data(),
        screenwidth,
        screenheight,
        screenwidth * 4,
        m_width,
        m_current_mode.vactive,
        m_current_mode.interlace,
        m_current_mode.progressiveFramebuffer,
        static_cast<uint8_t>(m_field),
        videoCaptures[drawIndex].rotation,
        source.sampling,
        reinterpret_cast<uint8_t*>(fb),
        outputSize))
    {
        if (!m_capture_failure_logged)
        {
            LogMessage("Unable to transform the captured frame.", true);
            m_capture_failure_logged = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    m_capture_failure_logged = false;

    time_entry = CurrentTicks();

    if (source.syncrefresh && m_first_blit)
    {
        time_start = time_entry;
        time_blit = time_entry;
        time_exit = time_entry;

        m_first_blit = false;
        m_frame = 0;

        // Skip blitting first frame, so we avoid glitches while MAME loads roms
        return;
    }

    const mistercast::StreamFaultAction fault = m_fault_schedule.Next();
    if (fault.stallMilliseconds != 0)
    {
        ++m_diagnostics_injected_stalls;
        LogMessage("[fault] Stalling before blit attempt " + std::to_string(fault.attempt) +
            " for " + std::to_string(fault.stallMilliseconds) + " ms.");
        std::this_thread::sleep_for(std::chrono::milliseconds(fault.stallMilliseconds));
    }
    if (fault.skip)
    {
        ++m_diagnostics_injected_skips;
        LogMessage("[fault] Skipping blit attempt " + std::to_string(fault.attempt) +
            " at logical frame " + std::to_string(m_frame) + ".");
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(m_period));
        return;
    }

    int vsync_offset = 0;

    if (source.framedelay == 0)
        // automatic
        m_frame_delay = std::max((double)(m_period - std::max(m_fd_margin, get_ms(time_frame_dm))) / m_period, 0.0);
    else
    {
        // user defined
        m_frame_delay = (double)(source.framedelay) / 10.0;
        vsync_offset = 0;// window().machine().video().vsync_offset();
    }

    // Capture and send audio
    if (m_audio_enabled)
    {
        const bool audioBufferAvailable = groovyMister.CanWriteAudioBuffer();
        TickAudioCapture(audioBufferAvailable);
        if (audioBufferAvailable && groovyMister.fpga.audio && AudioWritePos > 0)
            groovyMister.CmdAudio(static_cast<uint16_t>(AudioWritePos * sizeof(int16_t)));
    }

    // Update vsync scanline
    m_vsync_scanline = mistercast::RequestedSyncLine(
        m_current_mode.vtotal,
        m_frame_delay + (m_current_mode.vtotal == 0 ? 0.0 : (double)vsync_offset / m_current_mode.vtotal),
        m_current_mode.interlace,
        source.framedelay == 0);

    // Blit now
    const uint64_t diagnostics_send_start = CurrentTicks();
    groovyMister.CmdBlit(m_frame, m_field, static_cast<uint16_t>(m_vsync_scanline), 15000, 0);
    groovyMister.WaitSync();
    const groovyMisterDiagnostics completedBlit = groovyMister.getDiagnostics();
    if (m_current_mode.interlace && !m_phase_lock_reported && completedBlit.interlacePhaseValid)
    {
        LogMessage("[phase] Locked to FPGA feedback at logical frame " + std::to_string(m_frame) + ".");
        m_phase_lock_reported = true;
    }
    const uint64_t diagnostics_send_end = CurrentTicks();

    time_blit = diagnostics_send_end;
    nogpu_register_frametime(time_entry - time_exit);
    time_exit = CurrentTicks();
    nogpu_log_diagnostics(
        diagnostics_draw_start,
        time_entry,
        diagnostics_send_start,
        diagnostics_send_end,
        capture_sequence);

    return;
}

//============================================================
//  renderer_nogpu::nogpu_init
//============================================================

bool renderer_nogpu::nogpu_init()
{
    const uint32_t negotiatedAudioRate = m_audio_enabled ? audioSampleRate.load() : 0;
    switch (negotiatedAudioRate)
    {
    case 0:
        LogMessage("Audio disabled");
        break;
    case 22050:
        LogMessage("Audio Freq 22.05KHz");
        break;
    case 44100:
        LogMessage("Audio Freq 44.1KHz");
        break;
    case 48000:
        LogMessage("Audio Freq 48KHz");
        break;
    default:
        LogMessage("Unsupported audio sample rate. Only 48kHz, 44.1kHz and 22.05kHz are supported.", true);
        return false;
    }

    // Reset current mode
    m_current_mode = {};

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
        negotiatedAudioRate,
        m_audio_enabled ? 2 : 0,
        stream_config.rgbMode,
        stream_config.mtu);

    if (ret == 0)
    {
        audioBuffer = reinterpret_cast<int16_t*>(groovyMister.getPBufferAudio());
        return true;
    }
    else
    {
        const std::string& detail = groovyMister.getLastError();
        LogMessage(detail.empty()
            ? "Groovy MiSTer API failed to initialize."
            : "Groovy MiSTer API failed to initialize: " + detail, true);
        return false;
    }
}

//============================================================
//  renderer_nogpu::nogpu_switch_video_mode()
//============================================================

bool renderer_nogpu::nogpu_switch_video_mode()
{
    const nogpu_modeline mode = selected_modeline_snapshot();

    m_current_mode = mode;

    // Send new modeline to nogpu
    LogMessage("Sending CMD_SWITCHRES...");

    m_width = mode.hactive;
    m_height = mode.vactive;
    m_vtotal = mode.vtotal;
    m_field = 0;

    const double linePeriod = mistercast::LinePeriodMilliseconds(mode.pclock, mode.htotal);
    const double fieldPeriod = mistercast::FieldPeriodMilliseconds(
        mode.pclock, mode.htotal, mode.vtotal, mode.interlace);
    if (linePeriod > 0.0 && fieldPeriod > 0.0)
    {
        m_line_period = linePeriod;
        m_period = fieldPeriod;
    }

    int ret = groovyMister.CmdSwitchres(
        mode.pclock,
        mode.hactive,
        mode.hbegin,
        mode.hend,
        mode.htotal,
        mode.vactive,
        mode.vbegin,
        mode.vend,
        mode.vtotal,
        mistercast::ProtocolInterlaceMode(mode.interlace, mode.progressiveFramebuffer)
    );

    m_phase_lock_reported = !mode.interlace;
    if (mode.interlace)
    {
        LogMessage(mode.progressiveFramebuffer
            ? "[phase] Full-frame interlace uses field 0; waiting for a post-switch acknowledgement for frame alignment."
            : "[phase] Using local field sequence until a post-switch blit is acknowledged.");
    }

    if (ret != 0)
    {
        // Unconfirmed: the core has no modeline and will silently discard every
        // subsequent video packet until one lands (no self-recovery on its own).
        // Set shouldUpdateVideoMode so draw() retries this on the next frame,
        // regardless of which call site (initial connect, a user-triggered mode
        // change, or the auto-reconnect watchdog replaying the stashed mode) got
        // us here.
        LogMessage("CmdSwitchres was not acknowledged; will retry.", true);
        shouldUpdateVideoMode.store(true, std::memory_order_release);
        return false;
    }

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

    if (i >= max_regs)
        i = 0;

    if (regs < max_regs)
        regs++;

    for (int k = 0; k < regs; k++)
        acum += time_frame[k];

    time_frame_avg = acum / regs;

    // Compute current max deviation
    uint64_t max_diff = 0;

    for (int k = 1; k < regs; k++)
    {
        diff = time_frame[k] >= time_frame[k - 1]
            ? time_frame[k] - time_frame[k - 1]
            : time_frame[k - 1] - time_frame[k];

        if (diff > 0 && diff > max_diff)
            max_diff = diff;
    }

    time_frame_dm = max_diff;
}

//============================================================
//  renderer_nogpu::nogpu_log_diagnostics
//============================================================

void renderer_nogpu::nogpu_log_diagnostics(
    uint64_t draw_start,
    uint64_t transform_end,
    uint64_t send_start,
    uint64_t send_end,
    uint64_t capture_sequence)
{
    const groovyMisterDiagnostics transport = groovyMister.getDiagnostics();

    if (!m_diagnostics_have_sample)
    {
        m_diagnostics_start = draw_start;
        m_diagnostics_last_capture = capture_sequence;
        m_diagnostics_have_sample = true;
    }
    else
    {
        const uint64_t draw_gap = draw_start - m_diagnostics_last_draw;
        m_diagnostics_max_gap = std::max(m_diagnostics_max_gap, draw_gap);

        const uint64_t captured = capture_sequence - m_diagnostics_last_capture;
        m_diagnostics_captures += captured;
        if (captured == 0)
            m_diagnostics_capture_repeats++;

        if (UsesInterlacedFieldBuffer(
                m_current_mode.interlace,
                m_current_mode.progressiveFramebuffer) &&
            m_diagnostics_last_field == static_cast<uint8_t>(m_field))
            m_diagnostics_field_repeats++;

        if (m_diagnostics_last_frame + 1 != static_cast<uint32_t>(m_frame))
            m_diagnostics_frame_realignments++;

        if (m_diagnostics_last_fpga_frame != groovyMister.fpga.frame ||
            m_diagnostics_last_frame_echo != groovyMister.fpga.frameEcho ||
            m_diagnostics_last_vcount != groovyMister.fpga.vCount ||
            m_diagnostics_last_vcount_echo != groovyMister.fpga.vCountEcho ||
            m_diagnostics_last_f1 != groovyMister.fpga.vgaF1)
        {
            m_diagnostics_status_updates++;
        }

        if (m_diagnostics_last_f1 != groovyMister.fpga.vgaF1)
            m_diagnostics_f1_changes++;
    }

    m_diagnostics_frames++;
    m_diagnostics_max_transform = std::max(m_diagnostics_max_transform, transform_end - draw_start);
    m_diagnostics_max_send_wait = std::max(m_diagnostics_max_send_wait, send_end - send_start);
    m_diagnostics_last_draw = draw_start;
    m_diagnostics_last_capture = capture_sequence;
    m_diagnostics_last_frame = static_cast<uint32_t>(m_frame);
    m_diagnostics_last_field = static_cast<uint8_t>(m_field);
    m_diagnostics_last_fpga_frame = groovyMister.fpga.frame;
    m_diagnostics_last_frame_echo = groovyMister.fpga.frameEcho;
    m_diagnostics_last_vcount = groovyMister.fpga.vCount;
    m_diagnostics_last_vcount_echo = groovyMister.fpga.vCountEcho;
    m_diagnostics_last_f1 = groovyMister.fpga.vgaF1;

    const uint64_t elapsed_ticks = send_end - m_diagnostics_start;
    if (elapsed_ticks < TicksPerSecond())
        return;

    const double elapsed_seconds = static_cast<double>(elapsed_ticks) / TicksPerSecond();
    const double ticks_to_ms = 1000.0 / TicksPerSecond();
    std::ostringstream message;
    message << std::fixed << std::setprecision(2)
        << "[stream] mode=" << m_current_mode.hactive << 'x' << m_current_mode.vactive
        << (m_current_mode.interlace ? 'i' : 'p')
        << " framebuffer=" << (m_current_mode.progressiveFramebuffer ? "progressive" : "field")
        << " codec=" << (unsigned int)stream_config.codec
        << " sampling=" << SamplingModeName(source_config.snapshot().sampling)
        << " rate=" << (m_diagnostics_frames / elapsed_seconds)
        << " capture=" << (m_diagnostics_captures / elapsed_seconds)
        << " capture_repeat=" << m_diagnostics_capture_repeats
        << " max_gap_ms=" << (m_diagnostics_max_gap * ticks_to_ms)
        << " max_transform_ms=" << (m_diagnostics_max_transform * ticks_to_ms)
        << " max_send_wait_ms=" << (m_diagnostics_max_send_wait * ticks_to_ms)
        << " cmd=" << transport.commandFrame << ':' << static_cast<unsigned int>(m_field)
        << " fpga=" << groovyMister.fpga.frame << ':' << static_cast<unsigned int>(groovyMister.fpga.vgaF1)
        << " echo=" << groovyMister.fpga.frameEcho
        << " phase=" << (transport.interlacePhaseValid ? "locked" : "local")
        << " vcount=" << groovyMister.fpga.vCount << '/' << groovyMister.fpga.vCountEcho
        << " status_updates=" << m_diagnostics_status_updates
        << " f1_changes=" << m_diagnostics_f1_changes
        << " field_repeats=" << m_diagnostics_field_repeats
        << " frame_realign=" << m_diagnostics_frame_realignments
        << " injected_skip=" << m_diagnostics_injected_skips
        << " injected_stall=" << m_diagnostics_injected_stalls
        << " flags="
        << static_cast<unsigned int>(groovyMister.fpga.vramEndFrame)
        << static_cast<unsigned int>(groovyMister.fpga.vramReady)
        << static_cast<unsigned int>(groovyMister.fpga.vramSynced)
        << static_cast<unsigned int>(groovyMister.fpga.vgaFrameskip)
        << static_cast<unsigned int>(groovyMister.fpga.vgaVblank)
        << static_cast<unsigned int>(groovyMister.fpga.vramQueue)
        << " stream_ms=" << (transport.streamTime / 10000.0)
        << " emulation_ms=" << (transport.emulationTime / 10000.0)
        << " target_ms=" << (transport.frameTime / 10000.0)
        << " sync_line=" << m_vsync_scanline
        << " path_mtu=" << transport.requestedPathMtu
        << " route_mtu=" << transport.routeInterfaceMtu
        << " route_if=" << transport.routeInterfaceIndex
        << " rio_outstanding=" << transport.outstandingSends
        << " dropped_video=" << transport.droppedVideoBatches
        << " dropped_audio=" << transport.droppedAudioBatches
        << " transport_errors=" << transport.transportErrors
        << " reconnect_epoch=" << transport.reconnectEpoch
        << " no_ack_blits=" << transport.noAckBlitCount
        << " rio_send_posted=" << transport.rioSendPosted
        << " rio_send_failed=" << transport.rioSendFailed
        << " rio_ack_timeout=" << transport.rioAckTimeout;
    LogMessage(message.str());

    m_diagnostics_start = send_end;
    m_diagnostics_max_gap = 0;
    m_diagnostics_max_transform = 0;
    m_diagnostics_max_send_wait = 0;
    m_diagnostics_frames = 0;
    m_diagnostics_captures = 0;
    m_diagnostics_capture_repeats = 0;
    m_diagnostics_status_updates = 0;
    m_diagnostics_f1_changes = 0;
    m_diagnostics_field_repeats = 0;
    m_diagnostics_frame_realignments = 0;
}
