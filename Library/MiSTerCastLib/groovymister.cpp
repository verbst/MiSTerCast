#include "pch.h"
#include "groovymister.h"
#include "GroovyProtocol.h"
#include "NetworkMtu.h"
#include "StreamingTiming.h"

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <cstring>

#ifndef _WIN32
 #include <netinet/udp.h>
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <fcntl.h>
 #include <sys/time.h>
 #include <sys/stat.h>
 #include <time.h>
 #include <unistd.h>
#endif

// Hosts that already link LZ4 (PCSX2, RPCS3) define GM_SYSTEM_LZ4 to use the
// system headers and skip building api/lz4/ — avoids duplicate-symbol link
// errors. Only LZ4_compress_default/LZ4_compress_HC are used; API-stable.
#ifdef GM_SYSTEM_LZ4
 #include <lz4.h>
 #include <lz4hc.h>
#else
 #include "lz4/lz4.h"
 #include "lz4/lz4hc.h"
#endif
#include "nlc_codec.h"

// CmdInit lz4Frames value selecting the NLC block-adaptive near-lossless codec (tiled, lossless base).
#define GM_CODEC_NLC_TILED 7

#define USE_RIO 1

#ifdef _WIN32
#pragma comment(lib, "Iphlpapi.lib")
#endif

// Optional log sink (gm_set_log_sink): replaces stdout as LOG()'s destination
// so GUI hosts can capture the handshake/failure trace. Verbosity is still the
// per-instance setVerbose() gate applied by the LOG macro.
static gm_log_sink_fn g_gmLogSink = 0;

void gm_set_log_sink(gm_log_sink_fn fn)
{
	g_gmLogSink = fn;
}

static void gm_log_emit(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (g_gmLogSink)
	{
		char buf[1024];
		vsnprintf(buf, sizeof(buf), fmt, ap);
		g_gmLogSink(buf);
	}
	else
	{
		vprintf(fmt, ap);
	}
	va_end(ap);
}

#define LOG(sev,fmt, ...) do {\
					if (sev <= m_verbose) {\
					gm_log_emit(fmt, ##__VA_ARGS__);\
								}\
							} while (0)

#define CMD_CLOSE 1
#define CMD_INIT 2
#define CMD_SWITCHRES 3
#define CMD_AUDIO 4
#define CMD_GET_STATUS 5
#define CMD_BLIT_VSYNC 6
#define CMD_BLIT_FIELD_VSYNC 7
#define CMD_GET_VERSION 8

#define INIT_ACK_ATTEMPTS 3
#define INIT_ACK_TIMEOUT_MS 200

typedef union
{
	struct
	{
		unsigned char bit0 : 1;
		unsigned char bit1 : 1;
		unsigned char bit2 : 1;
		unsigned char bit3 : 1;
		unsigned char bit4 : 1;
		unsigned char bit5 : 1;
		unsigned char bit6 : 1;
		unsigned char bit7 : 1;
	}u;
	uint8_t byte;
} bitByte;

#define LZ4_ADAPTATIVE_CSIZE 600000
#define K_CONGESTION_SIZE    500000
#define K_CONGESTION_TIME    110000

// DiffTimeRaster safety net: the echoed frame and the core's GPU frame track
// within a frame or two in healthy play, so a raster correction is a sub-frame
// beam-race nudge. A spread beyond this many frames means the two counters
// belong to different sessions (a reconnect where the core restarted its counter
// but the host kept its own) — the derived sleep would be tens of seconds and
// WaitSync would busy-spin on it. Skip the correction instead. Matches the fbneo
// host's field-validated GROOVY_RASTER_MAX_SPREAD.
#define RASTER_MAX_FRAME_SPREAD 8

GroovyMister::GroovyMister()
{
	m_verbose = 0;
	m_lz4Frames = 0;
	m_soundChan = 0;
	m_rgbMode = 0;

	resetSessionState(); // zero fpga.* + m_frame (mirrored at the top of every CmdInit)

	joyInputs.joyFrame = 0;
	joyInputs.joyOrder = 0;
	joyInputs.joy1 = 0;
	joyInputs.joy2 = 0;
	joyInputs.joy1LXAnalog = 0;
	joyInputs.joy1LYAnalog = 0;
	joyInputs.joy1RXAnalog = 0;
	joyInputs.joy1RYAnalog = 0;
	joyInputs.joy2LXAnalog = 0;
	joyInputs.joy2LYAnalog = 0;
	joyInputs.joy2RXAnalog = 0;
	joyInputs.joy2RYAnalog = 0;
	joyInputs.joy1LTAnalog = 0;
	joyInputs.joy1RTAnalog = 0;
	joyInputs.joy2LTAnalog = 0;
	joyInputs.joy2RTAnalog = 0;

	ps2Inputs.ps2Frame = 0;
	ps2Inputs.ps2Order = 0;
	memset(&ps2Inputs.ps2Keys, 0, sizeof(ps2Inputs.ps2Keys));
	ps2Inputs.ps2Mouse = 0;
	ps2Inputs.ps2MouseX = 0;
	ps2Inputs.ps2MouseY = 0;
	ps2Inputs.ps2MouseZ = 0;

	m_RGBSize = 0;
	m_nlcWidth = 0;
	m_nlcDispMode = 2;   // /47 default: mode 2 (autonomous engine) — the rock-solid display path
	m_nlcPack = 1;       // TILED default; setNlcPack(2) selects RICE (R0-gated: rice+near1 clears the /59 ingest ceiling)
	m_nearLevel = 0;     // lossless default; near 1 recommended for heavy 3D content
	m_inputCaps = 0;     // legacy v1 inputs; setInputCaps(GM_CAP_INPUTS_V2 | ...) to opt in
	m_preEncodedSize = 0;
	m_interlace = 0;
	m_vTotal = 0;
	m_frame = 0;
	m_frameTime = 0;
	m_streamTime = 0;
	m_emulationTime = 0;
	m_mtu = 0;
	m_doCongestionControl = 0;
	m_network_ping = 0;
	m_dumpFrames = 0;
	m_dumpCount = 0;
	m_dumpMax = 0;
	m_dumpDir[0] = '\0';
	{
		const char* env = getenv("GM_FRAME_DUMP");
		if (env && env[0]) {
			const char* envMax = getenv("GM_FRAME_DUMP_MAX");
			setFrameDump(env, envMax ? (uint32_t)atoi(envMax) : 120);
		}
	}
	m_delta_enabled[0] = 0;
	m_delta_enabled[1] = 0;
	m_isConnected = 0;
	m_haveFpgaStatus = false;
	m_droppedVideoBatches = 0;
	m_droppedAudioBatches = 0;
	m_transportErrors = 0;
	m_requestedPathMtu = 0;
	m_routeInterfaceMtu = 0;
	m_routeInterfaceIndex = 0;
	m_core_version = 0;
	m_negotiatedCaps = 0;
	m_videoTorndown = 1;   // nothing to tear down until CmdInit builds the video side
	m_autoReconnect = 0;

	memset(m_initHost, 0, sizeof(m_initHost));
	m_initPort = 0;
	m_initLz4Frames = 0;
	m_initSoundRate = 0;
	m_initSoundChan = 0;
	m_initRgbMode = 0;
	m_initMtu = 0;
	m_switchresValid = 0;
	m_initPClock = 0;
	m_initHActive = 0;
	m_initHBegin = 0;
	m_initHEnd = 0;
	m_initHTotal = 0;
	m_initVActive = 0;
	m_initVBegin = 0;
	m_initVEnd = 0;
	m_initVTotal = 0;
	m_initInterlace = 0;
	m_lastFrameEchoSeen = 0;
	m_noAckBlitCount = 0;
	m_lastReconnectAttemptMs = 0;
	m_reconnectEpoch = 0;

	m_rioSendPosted = 0;
	m_rioSendFailed = 0;
	m_rioSendDrained = 0;
	m_rioRecvRepostFailed = 0;
	m_rioAckTimeout = 0;
	m_rioLastSummaryMs = 0;

#ifdef _WIN32
	m_sockFD = INVALID_SOCKET;
	m_sockInputsFD = INVALID_SOCKET;
	memset(&m_rio, 0, sizeof(m_rio));
	m_sendQueue = RIO_INVALID_CQ;
	m_receiveQueue = RIO_INVALID_CQ;
	m_requestQueue = RIO_INVALID_RQ;
	m_hIOCP = NULL;
	memset(&m_receiveOverlapped, 0, sizeof(m_receiveOverlapped));
	m_sendRioBufferId = RIO_INVALID_BUFFERID;
	m_receiveRioBufferId = RIO_INVALID_BUFFERID;
	m_sendRioBufferAudioId = RIO_INVALID_BUFFERID;
	memset(m_sendRioBuffers, 0, sizeof(m_sendRioBuffers));
	memset(m_rioCommandBuffers, 0, sizeof(m_rioCommandBuffers));
	memset(m_commandSlotOutstanding, 0, sizeof(m_commandSlotOutstanding));
	memset(&m_receiveRioBuffer, 0, sizeof(m_receiveRioBuffer));
	memset(&m_sendRioBufferAudio, 0, sizeof(m_sendRioBufferAudio));
	m_pBufsAudio = nullptr;
	for (int field = 0; field < 2; ++field)
	{
		m_sendRioBufferBlitId[field] = RIO_INVALID_BUFFERID;
		m_pBufsBlit[field] = nullptr;
		m_outstandingBlitSends[field] = 0;
	}
	m_wsaStarted = false;
	m_rioFunctionsReady = false;
	m_receiveNotificationArmed = false;
	m_rioFailed = false;
	m_outstandingRioSends = 0;
	m_outstandingRioReceives = 0;
	m_outstandingAudioSends = 0;
#else
	m_sockFD = -1;
	m_sockInputsFD = -1;
#endif

	memset(&m_tickStart, 0, sizeof(m_tickStart));
	memset(&m_tickEnd, 0, sizeof(m_tickEnd));
	memset(&m_tickSync, 0, sizeof(m_tickSync));
	memset(&m_tickCongestion, 0, sizeof(m_tickCongestion));
#ifdef _WIN32
	LARGE_INTEGER timerFrequency;
	m_timerFrequency = QueryPerformanceFrequency(&timerFrequency)
		? static_cast<uint64_t>(timerFrequency.QuadPart)
		: mistercast::HundredNanosecondsPerSecond;
#endif

	DWORD totalBufferCount = 0;
	DWORD totalBufferSize = 0;
	m_pBufferAudio = AllocateBufferSpace(BUFFER_SIZE, 1, totalBufferSize, totalBufferCount);
	m_pBufferBlitDelta = AllocateBufferSpace(BUFFER_SIZE, 1, totalBufferSize, totalBufferCount);
	for(int i=0;i<2;i++)
	{
		m_pBufferBlit[i] = AllocateBufferSpace(BUFFER_SIZE, 1, totalBufferSize, totalBufferCount);
		m_pBufferLZ4[i] = AllocateBufferSpace(BUFFER_SIZE, 1, totalBufferSize, totalBufferCount);
	}
}


GroovyMister::~GroovyMister()
{
	CmdClose();
#ifdef _WIN32
	VirtualFree(m_pBufferAudio, 0, MEM_RELEASE);
	VirtualFree(m_pBufferBlitDelta, 0, MEM_RELEASE);
	for(int i=0;i<2;i++)
	{
		VirtualFree(m_pBufferBlit[i], 0, MEM_RELEASE);
		VirtualFree(m_pBufferLZ4[i], 0, MEM_RELEASE);
	}
#else
	free(m_pBufferAudio);
	free(m_pBufferBlitDelta);
	for(int i=0;i<2;i++)
	{
		free(m_pBufferBlit[i]);
		free(m_pBufferLZ4[i]);
	}
#endif
}

void GroovyMister::resetSessionState(void)
{
	// Per-session raster state. Zeroed here (constructor) AND at the top of every
	// CmdInit so a reconnect can never carry a dead session's frame counters into
	// DiffTimeRaster/WaitSync (a stale counter vs the core's fresh one produced a
	// multi-second busy-spin), and so getACK's monotonic gate
	// (frameUDP > fpga.frameEcho) starts permissive for the core's fresh,
	// low-numbered session instead of rejecting all of its ACKs (which would leave
	// the watchdog reconnect-looping forever). The full struct is cleared so no
	// stale status bit (e.g. fpga.audio, which gates CmdAudio) leaks past the
	// first fresh ACK.
	fpga.frame = 0;
	fpga.frameEcho = 0;
	fpga.vCount = 0;
	fpga.vCountEcho = 0;
	fpga.vramEndFrame = 0;
	fpga.vramReady = 0;
	fpga.vramSynced = 0;
	fpga.vgaFrameskip = 0;
	fpga.vgaVblank = 0;
	fpga.vgaF1 = 0;
	fpga.audio = 0;
	fpga.vramQueue = 0;
	m_frame = 0;
	m_haveFpgaStatus = false;
	m_interlacePhase.Reset(m_interlace != 0);
}

char* GroovyMister::getPBufferBlit(uint8_t field)
{
	return m_pBufferBlit[field];
}

char* GroovyMister::getPBufferPreEncoded(void)
{
	return m_pBufferLZ4[0];
}

void GroovyMister::setPreEncodedSize(uint32_t cSize)
{
	m_preEncodedSize = cSize;
}

void GroovyMister::buildNlcParams(void* pnp)
{
	nlc_params* np = (nlc_params*)pnp;
	memset(np, 0, sizeof(*np));
	int bpp = (m_rgbMode == 1) ? 4 : (m_rgbMode == 2) ? 2 : 3;
	np->width  = m_nlcWidth;
	np->height = (m_nlcWidth > 0) ? (int)(m_RGBSize / ((uint32_t)m_nlcWidth * bpp)) : 0;
	np->rgb    = (m_rgbMode == 1) ? NLC_RGBA : (m_rgbMode == 2) ? NLC_RGB565 : NLC_RGB888;
	np->color  = NLC_COLOR_YCOCG; np->near_lvl = m_nearLevel;
	np->pack   = (m_nlcPack == 2) ? NLC_PACK_RICE : NLC_PACK_TILED;
	np->tile = 16; np->width_bits = 4; np->rice_k = -1;
}

uint32_t GroovyMister::EncodeNLC(const char* rgbFrame, char* out)
{
	nlc_params np;
	buildNlcParams(&np);
	int r = nlc_encode((const uint8_t*)rgbFrame, (uint8_t*)out, BUFFER_SIZE, &np);
	return (r > 0) ? (uint32_t)r : 0;
}

char* GroovyMister::getPBufferBlitDelta(void)
{
	return m_pBufferBlitDelta;
}

char* GroovyMister::getPBufferAudio(void)
{
	return m_pBufferAudio;
}

bool GroovyMister::CanWriteBlitBuffer(uint8_t field)
{
	if (field > 1)
		return false;
#ifdef _WIN32
	if (USE_RIO && m_rioFunctionsReady)
	{
		DrainRioSendCompletions();
		if (m_rioFailed)
		{
			++m_droppedVideoBatches;
			return false;
		}
		bool available = false;
		if (m_lz4Frames)
			available = m_outstandingBlitSends[0] == 0 && m_outstandingBlitSends[1] == 0;
		else
			available = m_outstandingBlitSends[field] == 0;
		if (!available)
			++m_droppedVideoBatches;
		return available;
	}
#endif
	return true;
}

bool GroovyMister::CanWriteAudioBuffer(void)
{
#ifdef _WIN32
	if (USE_RIO && m_rioFunctionsReady)
	{
		DrainRioSendCompletions();
		if (m_rioFailed)
		{
			++m_droppedAudioBatches;
			return false;
		}
		bool available = m_outstandingAudioSends == 0;
		if (!available)
			++m_droppedAudioBatches;
		return available;
	}
#endif
	return true;
}

void GroovyMister::CmdClose(void)
{
	if (m_isConnected)
	{
		m_bufferSend[0] = CMD_CLOSE;
		Send(&m_bufferSend[0], 1);
	}
	m_isConnected = 0;
	// a deliberate close is never auto-undone by the reconnect watchdog
	m_initHost[0] = '\0';
	m_switchresValid = 0;
	m_noAckBlitCount = 0;
	teardownVideo();
	if (inputsBound())
	{
#ifdef _WIN32
		::closesocket(m_sockInputsFD);
		m_sockInputsFD = INVALID_SOCKET;
#else
		close(m_sockInputsFD);
		m_sockInputsFD = -1;
#endif
	}
#ifdef _WIN32
	// teardownVideo() already closed m_hIOCP; this pairs whichever of
	// CmdInit/BindInputs called WSAStartup first (both guard on m_wsaStarted,
	// so it only ever ran once).
	if (m_wsaStarted)
	{
		::WSACleanup();
		m_wsaStarted = false;
	}
#endif
}

// Tear down the video-side resources (RIO queues/buffers + video socket)
// exactly once per CmdInit — idempotent, so a failed CmdInit's internal
// cleanup followed by the host's own CmdClose() is safe. The inputs socket is
// deliberately NOT touched here: CmdInit never created it, and the
// auto-reconnect path must keep it (and its local port) alive so the core's
// stored subscribe address stays valid across the reconnect.
void GroovyMister::teardownVideo(void)
{
	if (m_videoTorndown)
	{
		return;
	}
	m_videoTorndown = 1;
#ifdef _WIN32
	if (USE_RIO)
	{
		DrainRioSendCompletions();
		if (m_rioFunctionsReady)
		{
			const ULONGLONG deadline = GetTickCount64() + 250;
			while ((m_outstandingRioSends != 0 || m_outstandingRioReceives != 0) &&
				GetTickCount64() < deadline)
			{
				DrainRioSendCompletions();
				DrainRioReceiveCompletions();
				if (m_outstandingRioSends != 0 || m_outstandingRioReceives != 0)
					Sleep(1);
			}

			if (m_sendQueue != RIO_INVALID_CQ)
			{
				m_rio.RIOCloseCompletionQueue(m_sendQueue);
				m_sendQueue = RIO_INVALID_CQ;
			}
			if (m_receiveQueue != RIO_INVALID_CQ)
			{
				m_rio.RIOCloseCompletionQueue(m_receiveQueue);
				m_receiveQueue = RIO_INVALID_CQ;
			}
			m_requestQueue = RIO_INVALID_RQ;

			if (m_sendRioBufferId != RIO_INVALID_BUFFERID)
				m_rio.RIODeregisterBuffer(m_sendRioBufferId);
			if (m_receiveRioBufferId != RIO_INVALID_BUFFERID)
				m_rio.RIODeregisterBuffer(m_receiveRioBufferId);
			if (m_sendRioBufferAudioId != RIO_INVALID_BUFFERID)
				m_rio.RIODeregisterBuffer(m_sendRioBufferAudioId);
			for (int field = 0; field < 2; ++field)
			{
				if (m_sendRioBufferBlitId[field] != RIO_INVALID_BUFFERID)
					m_rio.RIODeregisterBuffer(m_sendRioBufferBlitId[field]);
			}
		}

		m_sendRioBufferId = RIO_INVALID_BUFFERID;
		m_receiveRioBufferId = RIO_INVALID_BUFFERID;
		m_sendRioBufferAudioId = RIO_INVALID_BUFFERID;
		delete[] m_pBufsAudio;
		m_pBufsAudio = nullptr;
		for (int field = 0; field < 2; ++field)
		{
			m_sendRioBufferBlitId[field] = RIO_INVALID_BUFFERID;
			delete[] m_pBufsBlit[field];
			m_pBufsBlit[field] = nullptr;
			m_outstandingBlitSends[field] = 0;
		}
		memset(m_commandSlotOutstanding, 0, sizeof(m_commandSlotOutstanding));
		m_outstandingRioSends = 0;
		m_outstandingRioReceives = 0;
		m_outstandingAudioSends = 0;
		m_receiveNotificationArmed = false;
		m_rioFunctionsReady = false;
		m_rioFailed = false;
	}
	if (m_sockFD != INVALID_SOCKET)
	{
		::closesocket(m_sockFD);
		m_sockFD = INVALID_SOCKET;
	}
	// Owned by CmdInit, scoped to the video side only (inputs use plain
	// recvfrom, not IOCP) — must be closed here, not just in CmdClose, or a
	// reconnect's fresh CreateIoCompletionPort leaks the old handle.
	if (m_hIOCP != NULL)
	{
		CloseHandle(m_hIOCP);
		m_hIOCP = NULL;
	}
#else
	if (m_sockFD >= 0)
	{
		close(m_sockFD);
	}
	m_sockFD = -1;
#endif
}

uint8_t GroovyMister::inputsBound(void)
{
#ifdef _WIN32
	return (m_sockInputsFD != INVALID_SOCKET) ? 1 : 0;
#else
	return (m_sockInputsFD >= 0) ? 1 : 0;
#endif
}

uint64_t GroovyMister::monotonicMs(void)
{
#ifdef _WIN32
	return (uint64_t)GetTickCount64();
#else
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000ULL + (uint64_t)(t.tv_nsec / 1000000);
#endif
}

void GroovyMister::CmdSendClose(void)
{
	if (m_isConnected)
	{
		m_bufferSend[0] = CMD_CLOSE;
		sendto(m_sockFD, (char *) &m_bufferSend[0], 1, 0, (struct sockaddr *)&m_serverAddr, sizeof(m_serverAddr));
	}
}

void GroovyMister::CmdSendKeepAlive(void)
{
	// Hold an idle session open across pauses / long loads so the core's idle
	// timeout does not drop it. CMD_GET_STATUS is a 1-byte, no-side-effect request;
	// any datagram on the video socket resets the core's activity timer. Uses the
	// normal send path (RIO on Windows) like CmdClose, so keep your usual ACK
	// draining cadence during long pauses. No-op when disconnected.
	if (m_isConnected)
	{
		m_bufferSend[0] = CMD_GET_STATUS;
		Send(&m_bufferSend[0], 1);
	}
}

void GroovyMister::ResendInputSubscribe(void)
{
	if (!inputsBound())
	{
		return;
	}
	// mirrors the sendto in BindInputs: the core only checks len==1
	sendto(m_sockInputsFD, m_bufferSend, 1, 0, (struct sockaddr *)&m_serverAddrInputs, sizeof(m_serverAddrInputs));
}

uint8_t GroovyMister::isConnected(void)
{
	return m_isConnected;
}

uint8_t GroovyMister::getInputCaps(void)
{
	return m_isConnected ? m_negotiatedCaps : 0;
}

void GroovyMister::setAutoReconnect(uint8_t on)
{
	m_autoReconnect = on ? 1 : 0;
}

uint32_t GroovyMister::reconnectEpoch(void)
{
	return m_reconnectEpoch;
}

void GroovyMister::setVerbose(uint8_t sev)
{
	m_verbose = sev;
}

const char* GroovyMister::getVersion()
{
	return &GROOVYMISTER_VERSION[0];
}

const std::string& GroovyMister::getLastError() const noexcept
{
	return m_lastError;
}

groovyMisterDiagnostics GroovyMister::getDiagnostics() const noexcept
{
	groovyMisterDiagnostics diagnostics = {};
	diagnostics.commandFrame = m_frame;
	diagnostics.frameTime = m_frameTime;
	diagnostics.streamTime = m_streamTime;
	diagnostics.emulationTime = m_emulationTime;
	diagnostics.outstandingSends = 0;
#ifdef _WIN32
	diagnostics.outstandingSends = m_outstandingRioSends;
#endif
	diagnostics.droppedVideoBatches = m_droppedVideoBatches;
	diagnostics.droppedAudioBatches = m_droppedAudioBatches;
	diagnostics.transportErrors = m_transportErrors;
	diagnostics.requestedPathMtu = m_requestedPathMtu;
	diagnostics.routeInterfaceMtu = m_routeInterfaceMtu;
	diagnostics.routeInterfaceIndex = m_routeInterfaceIndex;
	diagnostics.rioSendPosted = m_rioSendPosted;
	diagnostics.rioSendFailed = m_rioSendFailed;
	diagnostics.rioSendDrained = m_rioSendDrained;
	diagnostics.rioRecvRepostFailed = m_rioRecvRepostFailed;
	diagnostics.rioAckTimeout = m_rioAckTimeout;
	diagnostics.reconnectEpoch = m_reconnectEpoch;
	diagnostics.noAckBlitCount = m_noAckBlitCount;
	diagnostics.connected = m_isConnected;
	diagnostics.haveFpgaStatus = m_haveFpgaStatus ? 1 : 0;
	diagnostics.interlacePhaseValid = m_interlacePhase.IsValid() ? 1 : 0;
	return diagnostics;
}

void GroovyMister::setFrameDump(const char* dir, uint32_t maxFrames)
{
	if (!dir || !dir[0]) { m_dumpFrames = 0; return; }
	strncpy(m_dumpDir, dir, sizeof(m_dumpDir) - 1);
	m_dumpDir[sizeof(m_dumpDir) - 1] = '\0';
	m_dumpMax = maxFrames ? maxFrames : 120;
	m_dumpCount = 0;
	m_dumpFrames = 1;
	LOG(0, "[MiSTer] Frame dump enabled -> %s (max %u frames)\n", m_dumpDir, m_dumpMax);
}

void GroovyMister::DumpFrame(uint8_t field)
{
	// Write the raw, pre-compression frame so tools/nlc_bench can measure real
	// content. Dimensions are encoded in the filename; rows derive from the actual
	// buffer size so interlaced fields are captured correctly.
	if (!m_dumpFrames || m_dumpCount >= m_dumpMax) return;
	int W = m_nlcWidth;
	uint8_t bpp = (m_rgbMode == 1) ? 4 : (m_rgbMode == 2) ? 2 : 3;
	if (W <= 0 || m_RGBSize == 0) return;
	int rows = m_RGBSize / (W * bpp);
	const char* fmt = (m_rgbMode == 1) ? "rgba" : (m_rgbMode == 2) ? "565" : "888";
	char path[320];
	snprintf(path, sizeof(path), "%s/frame_%dx%d_%s_%06u.raw", m_dumpDir, W, rows, fmt, m_dumpCount);
	FILE* fp = fopen(path, "wb");
	if (!fp) { LOG(0, "[MiSTer] Frame dump: cannot open %s\n", path); m_dumpFrames = 0; return; }
	fwrite(m_pBufferBlit[field], 1, m_RGBSize, fp);
	fclose(fp);
	m_dumpCount++;
	if (m_dumpCount >= m_dumpMax) LOG(0, "[MiSTer] Frame dump complete: %u frames in %s\n", m_dumpCount, m_dumpDir);
}

void GroovyMister::setNlcDispMode(uint8_t mode)
{
	m_nlcDispMode = (mode <= 3) ? mode : 2;   // sent in CMD_INIT byte[1] bits [6:5]
}

void GroovyMister::setNlcPack(uint8_t pack)
{
	m_nlcPack = (pack == 2) ? 2 : 1;          // sent in CMD_INIT byte[1] bit 7 (1 = RICE)
}

void GroovyMister::setNearLevel(uint8_t lvl)
{
	m_nearLevel = (lvl <= 3) ? lvl : 0;       // sent in CMD_INIT byte[1] bits [3:2]
}

void GroovyMister::setInputCaps(uint8_t caps)
{
	m_inputCaps = caps;                       // sent as CMD_INIT byte[5] (len-6 init; 0 = len-5, legacy)
}

int GroovyMister::CmdInit(const char* misterHost, uint16_t misterPort, int lz4Frames, uint32_t soundRate, uint8_t soundChan, uint8_t rgbMode, uint16_t mtu)
{
	// Idempotent: safe to call again on a live session without an explicit
	// CmdClose() first. Tears down only the video side, so a caller mid
	// auto-reconnect (which deliberately keeps the inputs socket alive) is
	// unaffected.
	m_isConnected = 0;
	teardownVideo();
	// Clear stale per-session raster state before the version-probe/init ACKs, so
	// a reconnect starts the raster servo and getACK's ACK gate from a clean slate
	// (see resetSessionState). On the initial connect this repeats the constructor.
	resetSessionState();
	m_droppedVideoBatches = 0;
	m_droppedAudioBatches = 0;
	m_transportErrors = 0;
	m_requestedPathMtu = mistercast::ResolvePathMtu(mtu);
	m_routeInterfaceMtu = 0;
	m_routeInterfaceIndex = 0;
	m_lastError.clear();
	if (!mistercast::IsSupportedPathMtu(mtu))
	{
		m_lastError = "Unsupported path MTU " + std::to_string(m_requestedPathMtu) +
			"; the Windows RIO transport requires at least 1500 bytes.";
		LOG(0, "[MiSTer] Unsupported MTU %u; this transport requires at least 1500 bytes\n", mtu);
		return -1;
	}
	m_mtu = static_cast<uint16_t>(mistercast::UdpPayloadBytes(mtu));

	// Set server
	m_serverAddr.sin_family = AF_INET;
	m_serverAddr.sin_port = htons(misterPort);
	m_serverAddr.sin_addr.s_addr = inet_addr(misterHost);

	// Set socket
#ifdef _WIN32
	if (!m_wsaStarted)
	{
		WSADATA wsd;
		int rc = ::WSAStartup(MAKEWORD(2, 2), &wsd);
		if (rc != 0)
		{
			m_lastError = "Unable to load Winsock (error " + std::to_string(rc) + ").";
			LOG(0, "[MiSTer] Unable to load Winsock: %d\n", rc);
			return -1;
		}
		m_wsaStarted = true;
	}

	DWORD interfaceIndex = 0;
	DWORD routeResult = GetBestInterfaceEx(
		reinterpret_cast<sockaddr*>(&m_serverAddr), &interfaceIndex);
	if (routeResult != NO_ERROR)
	{
		m_lastError = "Unable to determine the route MTU for " +
			std::string(misterHost) + " (Windows error " + std::to_string(routeResult) + ").";
		LOG(0, "[MiSTer] Unable to determine the target route MTU: %lu\n", routeResult);
		return -1;
	}

	MIB_IF_ROW2 interfaceRow = {};
	interfaceRow.InterfaceIndex = interfaceIndex;
	routeResult = GetIfEntry2(&interfaceRow);
	if (routeResult != NO_ERROR)
	{
		m_lastError = "Unable to read MTU for route interface " +
			std::to_string(interfaceIndex) + " (Windows error " + std::to_string(routeResult) + ").";
		LOG(0, "[MiSTer] Unable to read interface %lu MTU: %lu\n", interfaceIndex, routeResult);
		return -1;
	}

	m_routeInterfaceIndex = interfaceIndex;
	m_routeInterfaceMtu = interfaceRow.Mtu;
	if (!mistercast::InterfaceSupportsPathMtu(mtu, interfaceRow.Mtu))
	{
		m_lastError = "Route interface " + std::to_string(interfaceIndex) +
			" has MTU " + std::to_string(interfaceRow.Mtu) +
			", smaller than the requested " + std::to_string(m_requestedPathMtu) + ".";
		LOG(0, "[MiSTer] Route interface %lu MTU %lu is smaller than requested MTU %u\n",
			interfaceIndex, interfaceRow.Mtu, m_requestedPathMtu);
		return -1;
	}
	LOG(0, "[MiSTer] Route interface %lu MTU %lu supports requested MTU %u\n",
		interfaceIndex, interfaceRow.Mtu, m_requestedPathMtu);

	int rc;
	m_sockFD = INVALID_SOCKET;

	if (USE_RIO)
	{
		LOG(0, "[MiSTer] Initialising socket registered io %s...\n","");
		m_sockFD = ::WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
		if (m_sockFD == INVALID_SOCKET)
		{
			LOG(0,"[MiSTer] Could not create socket : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}

		DWORD val = 1;
		rc = setsockopt(m_sockFD, IPPROTO_IP, IP_DONTFRAGMENT, (char *)&val, sizeof(val));
		if (rc != 0)
		{
		        LOG(0,"[MiSTer] Could not create IP_DONTFRAGMENT : %lu", ::GetLastError());
		        teardownVideo();
		        return -1;
		}

		LOG(0,"[MiSTer] Setting WSAIoctl %s...\n","");
		GUID functionTableId = WSAID_MULTIPLE_RIO;
		DWORD dwBytes = 0;
		if ( 0 != WSAIoctl(m_sockFD, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &functionTableId, sizeof(GUID), (void**)&m_rio, sizeof(m_rio), &dwBytes, NULL, NULL) )
		{
			LOG(0,"[MiSTer] Could not create WSAIoctl : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}
		m_rioFunctionsReady = true;

		m_hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0) ;
		if (NULL == m_hIOCP)
		{
			LOG(0,"[MiSTer] Could not create m_hIOCP IoCompletionPort : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}

		ZeroMemory(&m_receiveOverlapped, sizeof(m_receiveOverlapped));

		RIO_NOTIFICATION_COMPLETION completionType ;

		completionType.Type = RIO_IOCP_COMPLETION;
		completionType.Iocp.IocpHandle = m_hIOCP;
		completionType.Iocp.CompletionKey = (void*)1;
		completionType.Iocp.Overlapped = &m_receiveOverlapped;

		LOG(0,"[MiSTer] Register Buffers %s...\n","");
		m_sendRioBufferId = m_rio.RIORegisterBuffer(
			reinterpret_cast<char*>(m_rioCommandBuffers), sizeof(m_rioCommandBuffers));
		if (m_sendRioBufferId == RIO_INVALID_BUFFERID)
		{
			LOG(0,"[MiSTer] RIORegisterBuffer m_BufferSend Error: %lu\n", ::GetLastError());
			teardownVideo();
			return -1;
		}
		for (DWORD slot = 0; slot < RIO_COMMAND_SLOTS; ++slot)
		{
			m_sendRioBuffers[slot].BufferId = m_sendRioBufferId;
			m_sendRioBuffers[slot].Offset = slot * sizeof(m_rioCommandBuffers[slot]);
			m_sendRioBuffers[slot].Length = sizeof(m_rioCommandBuffers[slot]);
			m_commandSlotOutstanding[slot] = false;
		}

		m_receiveRioBufferId = m_rio.RIORegisterBuffer(m_bufferReceive, sizeof(m_bufferReceive));
		if (m_receiveRioBufferId == RIO_INVALID_BUFFERID)
		{
			LOG(0,"[MiSTer] RIORegisterBuffer m_BufferReceive Error: %lu\n", ::GetLastError());
			teardownVideo();
			return -1;
		}
		m_receiveRioBuffer.BufferId = m_receiveRioBufferId;
		m_receiveRioBuffer.Offset = 0;
		m_receiveRioBuffer.Length = sizeof(m_bufferReceive);

		DWORD offset = 0;
		for (int field = 0; field < 2; field++)
		{
			if (lz4Frames)
			{
				m_sendRioBufferBlitId[field] = m_rio.RIORegisterBuffer(m_pBufferLZ4[field], BUFFER_SIZE);
			}
			else
			{
				m_sendRioBufferBlitId[field] = m_rio.RIORegisterBuffer(m_pBufferBlit[field], BUFFER_SIZE);
			}
			if (m_sendRioBufferBlitId[field] == RIO_INVALID_BUFFERID)
			{
				LOG(0,"[MiSTer] RIORegisterBuffer pBufferBlit[%d] Error: %lu\n", field, ::GetLastError());
				teardownVideo();
				return -1;
			}

			offset = 0;
			m_pBufsBlit[field] = new RIO_BUF[BUFFER_SLICES];
			for (DWORD i = 0; i < BUFFER_SLICES; ++i)
			{
				RIO_BUF *pBuffer = m_pBufsBlit[field] + i;

				pBuffer->BufferId = m_sendRioBufferBlitId[field];
				pBuffer->Offset = offset;
				pBuffer->Length = m_mtu;

				offset += m_mtu;
			}
		}
		m_sendRioBufferAudioId = m_rio.RIORegisterBuffer(m_pBufferAudio, BUFFER_SIZE);
		if (m_sendRioBufferAudioId == RIO_INVALID_BUFFERID)
		{
			LOG(0,"[MiSTer] RIORegisterBuffer pBufferAudio Error: %lu\n", ::GetLastError());
			teardownVideo();
			return -1;
		}
		offset = 0;
		m_pBufsAudio = new RIO_BUF[BUFFER_SLICES];
		for (DWORD i = 0; i < BUFFER_SLICES; ++i)
		{
			RIO_BUF *pBuffer = m_pBufsAudio + i;

			pBuffer->BufferId = m_sendRioBufferAudioId;
			pBuffer->Offset = offset;
			pBuffer->Length = m_mtu;

			offset += m_mtu;
		}

		LOG(0,"[MiSTer] Create queues %s...\n","");
		m_sendQueue = m_rio.RIOCreateCompletionQueue(RIO_SEND_QUEUE_SIZE, &completionType);
		if (m_sendQueue == RIO_INVALID_CQ)
		{
			LOG(0,"[MiSTer]Could not create m_sendQueue : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}

		m_receiveQueue = m_rio.RIOCreateCompletionQueue(8, &completionType);
		if (m_receiveQueue == RIO_INVALID_CQ)
		{
			LOG(0,"[MiSTer]Could not create m_receiveQueue : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}

		m_requestQueue = m_rio.RIOCreateRequestQueue(
			m_sockFD, 8, 1, RIO_SEND_QUEUE_SIZE, 1, m_receiveQueue, m_sendQueue, NULL);
		if (m_requestQueue == RIO_INVALID_RQ)
		{
			LOG(0,"[MiSTer]Could not create m_requestQueue : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}

		LOG(0,"[MiSTer] Connect %s...\n","");
		if (SOCKET_ERROR == ::connect(m_sockFD, reinterpret_cast<sockaddr *>(&m_serverAddr), sizeof(m_serverAddr)))
		{
			LOG(0,"[MiSTer] Could not connect : %lu", ::GetLastError());
			teardownVideo();
			return -1;
		}
	}
	else
	{
		LOG(0, "[MiSTer] Initialising socket %s...\n","");
		m_sockFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (m_sockFD == INVALID_SOCKET)
		{
			LOG(0,"[MiSTer] Could not create socket : %lu", ::GetLastError());
			return -1;
		}

		LOG(0,"[MiSTer] Setting socket async %s...\n","");
		u_long iMode=1;
		rc = ioctlsocket(m_sockFD, FIONBIO, &iMode);
		if (rc < 0)
		{
			LOG(0,"[MiSTer] set nonblock fail %d\n", rc);
			return -1;
		}

		LOG(0,"[MiSTer] Setting send buffer to %d bytes...\n", 2097152);
		int optVal = 2097152;
		int optLen = sizeof(int);
		rc = setsockopt(m_sockFD, SOL_SOCKET, SO_SNDBUF, (char*)&optVal, optLen);
		if (rc != 0)
		{
			LOG(0,"[MiSTer] Unable to set send buffer: %d\n", rc);
			return -1;
		}
	}

#else
	printf("[DEBUG] Initialising socket...\n");
	m_sockFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_sockFD < 0)
	{
		LOG(0,"[MiSTer] Could not create socket : %d", m_sockFD);
		return -1;
	}

	LOG(0,"[MiSTer] Setting socket async %s...\n","");
	// Non blocking socket
	int flags;
	flags = fcntl(m_sockFD, F_GETFD, 0);
	if (flags < 0)
	{
		LOG(0,"[MiSTer] get falg error %d\n", flags);
		return -1;
	}
	flags |= O_NONBLOCK;
	if (fcntl(m_sockFD, F_SETFL, flags) < 0)
	{
		LOG(0,"[MiSTer] set nonblock fail %d\n", flags);
		return -1;
	}

	printf("[DEBUG] Setting send buffer to 2097152 bytes...\n");
	int size = 2 * 1024 * 1024;
	if (setsockopt(m_sockFD, SOL_SOCKET, SO_SNDBUF, (void*)&size, sizeof(size)) < 0)
	{
		LOG(0,"[MiSTer] Unable to set send buffer: %d\n", 2097152);
		return -1;
	}
#endif

	m_videoTorndown = 0; // video-side resources live from here; re-arm teardownVideo()
	m_rioSendPosted = 0;
	m_rioSendFailed = 0;
	m_rioSendDrained = 0;
	m_rioRecvRepostFailed = 0;
	m_rioAckTimeout = 0;

#ifdef _WIN32
	if (USE_RIO)
	{
		if (!QueueRioReceive() || !ArmRioReceiveNotification())
		{
			teardownVideo();
			return -1;
		}
	}
#endif

	// Caps negotiation: a len-6 CMD_INIT is silently DISCARDED by cores older
	// than GROOVY_VERSION 2 (their length check rejects it, no ACK), so probe
	// the version first and drop to a len-5 init (v1 inputs) when the core
	// can't take the caps byte. getInputCaps() exposes the outcome.
	m_negotiatedCaps = m_inputCaps;
	if (m_inputCaps)
	{
		m_core_version = 0;
		m_bufferSend[0] = CMD_GET_VERSION;
		Send(&m_bufferSend[0], 1);
		getACK(60);
		if (m_core_version < 2)
		{
			LOG(0,"[MiSTer] Core version %d < 2: no caps support, falling back to v1 inputs\n", m_core_version);
			m_negotiatedCaps = 0;
		}
	}

	LOG(0,"[MiSTer] Sending CMD_INIT...lz4 %d sound_rate %d sound_chan %d rgb_mode %d mtu %d\n", lz4Frames, soundRate, soundChan, rgbMode, mtu);

	m_lz4Frames = static_cast<uint8_t>(lz4Frames);
	m_soundChan = soundChan;
	m_rgbMode = rgbMode;

	m_bufferSend[0] = CMD_INIT;
	// codec byte: RAW=0, LZ4=1 (bare); NLC packs codec=2 + near + colour + pack
	// ([1:0]=codec [3:2]=near [4]=colour [6:5]=dispMode [7]=RICE — R5 negotiation bit).
	// The HPS reads codec via &3; old cores see >1 and fall back to raw as intended.
	m_bufferSend[1] = (lz4Frames == GM_CODEC_NLC_TILED)
	                ? (char)(2 | ((m_nearLevel & 0x3) << 2) | (1 << 4) | ((m_nlcDispMode & 0x3) << 5) | ((m_nlcPack == 2 ? 1 : 0) << 7))
	                : (lz4Frames) ? 1 : 0;
	m_bufferSend[2] = (soundRate == 22050) ? 1 : (soundRate == 44100) ? 2 : (soundRate == 48000) ? 3 : 0;
	m_bufferSend[3] = soundChan;
	m_bufferSend[4] = rgbMode;
	m_bufferSend[5] = m_negotiatedCaps;

	// len-5 init stays byte-identical for older cores; caps ride an extra byte
	if (!Send(&m_bufferSend[0], m_negotiatedCaps ? 6 : 5))
	{
		teardownVideo();
		return -1;
	}

	uint32_t ackTime = 0;
	for (int attempt = 0; attempt < INIT_ACK_ATTEMPTS && !ackTime; ++attempt)
	{
		if (attempt > 0 && !Send(&m_bufferSend[0], m_negotiatedCaps ? 6 : 5))
			break;
		ackTime = getACK(INIT_ACK_TIMEOUT_MS);
	}
	if (!ackTime)
	{
		m_lastError = "No ACK from " + std::string(misterHost) + " after " +
			std::to_string(INIT_ACK_ATTEMPTS) + " attempts.";
		LOG(0,"[MiSTer] ACK failed after %d attempts of %d ms\n", INIT_ACK_ATTEMPTS, INIT_ACK_TIMEOUT_MS);
		teardownVideo(); // inputs socket (if bound) stays alive; full cleanup is the host's CmdClose()
		return -1;
	}
	else
	{
		LOG(0,"[MiSTer] ACK received with %f ms\n", (double) ackTime / 10000);
		m_network_ping = 0;
		m_isConnected = 1;
		// stash the params for the setAutoReconnect watchdog
		strncpy(m_initHost, misterHost, sizeof(m_initHost) - 1);
		m_initHost[sizeof(m_initHost) - 1] = '\0';
		m_initPort = misterPort;
		m_initLz4Frames = lz4Frames;
		m_initSoundRate = soundRate;
		m_initSoundChan = soundChan;
		m_initRgbMode = rgbMode;
		m_initMtu = mtu;
		m_lastFrameEchoSeen = 0;
		m_noAckBlitCount = 0;
		LOG(0,"[MiSTer] Connected: core ver %d, CMD_INIT byte1=0x%02x caps=0x%02x\n", m_core_version, (uint8_t) m_bufferSend[1], m_negotiatedCaps);
		return 0;
	}
}

int GroovyMister::CmdSwitchres(double pClock, uint16_t hActive, uint16_t hBegin, uint16_t hEnd, uint16_t hTotal, uint16_t vActive, uint16_t vBegin, uint16_t vEnd, uint16_t vTotal, uint8_t interlace)
{
	if (!m_isConnected)
	  return -1;

	uint8_t interlace_modeline = (interlace != 2) ? interlace : 1;

	m_RGBSize = (m_rgbMode == 1) ? (hActive * vActive) << 2 : (m_rgbMode == 2) ? (hActive * vActive) << 1 : hActive * vActive * 3;
	m_nlcWidth = hActive;   // for nlc_encode / DumpFrame

	if (interlace == 1)
	{
		m_RGBSize = m_RGBSize >> 1;
	}

	m_widthTime = static_cast<uint32_t>(10 * round((double) hTotal * (1 / pClock))); //in 100 ns units, time to raster 1 line
	m_frameTime = (m_widthTime * vTotal) >> interlace_modeline;

	m_interlace = interlace_modeline;
	m_vTotal    = vTotal;
	m_interlacePhase.Reset(interlace_modeline != 0);
	m_delta_enabled[0] = 0;
	m_delta_enabled[1] = 0;

	m_bufferSend[0] = CMD_SWITCHRES;
	memcpy(&m_bufferSend[1],&pClock,sizeof(pClock));
	memcpy(&m_bufferSend[9],&hActive,sizeof(hActive));
	memcpy(&m_bufferSend[11],&hBegin,sizeof(hBegin));
	memcpy(&m_bufferSend[13],&hEnd,sizeof(hEnd));
	memcpy(&m_bufferSend[15],&hTotal,sizeof(hTotal));
	memcpy(&m_bufferSend[17],&vActive,sizeof(vActive));
	memcpy(&m_bufferSend[19],&vBegin,sizeof(vBegin));
	memcpy(&m_bufferSend[21],&vEnd,sizeof(vEnd));
	memcpy(&m_bufferSend[23],&vTotal,sizeof(vTotal));
	memcpy(&m_bufferSend[25],&interlace,sizeof(interlace));

	// Unlike every other state-critical command (CmdInit, CmdBlit/ACK, CmdGetStatus), this
	// send used to be pure fire-and-forget: no ACK, no retry, no return value. On the
	// reconnect path (setAutoReconnect's watchdog) this lands immediately after CmdInit's own
	// send with no natural gap between them, and was found to be lost 100% of the time on
	// reconnect (never on the initial connect, where unrelated app startup work happens to
	// separate the two sends by a few hundred ms). A lost switchres left the core's
	// PoC_bytes_len at 0 forever — setInit()'s calloc zeroes it on every CmdInit, and only a
	// successfully-processed CmdSwitchres restores it — silently discarding all incoming video
	// data for the rest of the session with no way to recover short of a full reconnect.
	// Retry like CmdInit already does (same getACK(60) pattern, same ACK the core already
	// sends back for CMD_INIT/CMD_GET_STATUS).
	const int SWITCHRES_ATTEMPTS = 3;
	uint32_t ackTime = 0;
	for (int attempt = 0; attempt < SWITCHRES_ATTEMPTS && !ackTime; attempt++)
	{
		if (!Send(&m_bufferSend[0], 26))
			continue;
		ackTime = getACK(60);
	}

	if (!ackTime)
	{
		LOG(0, "[MiSTer] CmdSwitchres ACK failed after %d attempts\n", SWITCHRES_ATTEMPTS);
		return -1;
	}

	// stash the modeline so the setAutoReconnect watchdog can replay it after
	// an internal reconnect (the caller never has to detect the reconnect)
	m_switchresValid = 1;
	m_initPClock    = pClock;
	m_initHActive   = hActive;
	m_initHBegin    = hBegin;
	m_initHEnd      = hEnd;
	m_initHTotal    = hTotal;
	m_initVActive   = vActive;
	m_initVBegin    = vBegin;
	m_initVEnd      = vEnd;
	m_initVTotal    = vTotal;
	m_initInterlace = interlace;
	return 0;
}

void GroovyMister::CmdBlit(uint32_t frame, uint8_t field, uint16_t vCountSync, uint32_t margin, uint32_t matchDeltaBytes)
{
	if (!m_isConnected)
	{
		// after a FAILED auto-reconnect the session is down but the watchdog
		// stays armed: fall through so the rate-limited retry below can run
		// (otherwise "retry in 1s" could never fire — this early-out would
		// block it forever)
		if (!(m_autoReconnect && m_noAckBlitCount >= 10 && m_initHost[0] != '\0'))
		{
			return;
		}
	}

	// Opt-in ACK watchdog (setAutoReconnect): fpga.frameEcho is refreshed by
	// getACK() from WaitSync/DiffTimeRaster; if it stops advancing across
	// blits the core has gone silent. Warn at 5 misses, reconnect at 10
	// (~167ms at 60Hz), rate-limited to one attempt per second. The reconnect
	// tears down ONLY the video side — the inputs socket and its local port
	// survive, so the subscribe re-sent around the inner CmdInit restores the
	// pad stream (the pre-init send lands in the core's one-shot CMD_INIT
	// read; the post-init send is UDP-loss insurance for address-aware cores).
	if (m_autoReconnect)
	{
		if (fpga.frameEcho > m_lastFrameEchoSeen)
		{
			m_lastFrameEchoSeen = fpga.frameEcho;
			m_noAckBlitCount = 0;
		}
		else if (m_frame > 0)
		{
			m_noAckBlitCount++;
			if (m_noAckBlitCount == 5)
			{
				LOG(0,"[MiSTer] WARNING: no ACK advance for 5 blits (lastEcho=%u, sending=%u)\n", fpga.frameEcho, frame);
			}
			if (m_noAckBlitCount >= 10 && m_initHost[0] != '\0')
			{
				uint64_t nowMs = monotonicMs();
				if (m_lastReconnectAttemptMs != 0 && (nowMs - m_lastReconnectAttemptMs) < 1000)
				{
					// inside the back-off window; stay primed to retry the
					// moment it elapses instead of needing 10 fresh misses
					m_noAckBlitCount = 10;
					return;
				}
				m_lastReconnectAttemptMs = nowMs;

				// CmdInit re-stashes into m_initHost — snapshot it first
				char savedHost[sizeof(m_initHost)];
				memcpy(savedHost, m_initHost, sizeof(savedHost));

				LOG(0,"[MiSTer] No ACK advance for %u blits: reconnecting to %s:%u\n", m_noAckBlitCount, savedHost, m_initPort);
				CmdSendClose();  // plain sendto: delivers even if the RIO queues are wedged
				m_isConnected = 0;
				teardownVideo(); // inputs socket deliberately untouched
				ResendInputSubscribe(); // queue a fresh subscribe for the core's CMD_INIT one-shot read
				int rc = CmdInit(savedHost, m_initPort, m_initLz4Frames, m_initSoundRate, m_initSoundChan, m_initRgbMode, m_initMtu);
				if (rc == 0)
				{
					ResendInputSubscribe();
					bool switchresReplayed = false;
					if (m_switchresValid)
					{
						int switchresRc = CmdSwitchres(m_initPClock, m_initHActive, m_initHBegin, m_initHEnd, m_initHTotal, m_initVActive, m_initVBegin, m_initVEnd, m_initVTotal, m_initInterlace);
						switchresReplayed = (switchresRc == 0);
						if (!switchresReplayed)
						{
							// CmdSwitchres already retried internally and logged the ACK failure;
							// PoC_bytes_len stays 0 on the core until a future reconnect gets it through
							LOG(0,"[MiSTer] WARNING: modeline replay failed on reconnect — video will stay blank/corrupt until it succeeds\n");
						}
					}
					m_reconnectEpoch++;
					LOG(0,"[MiSTer] Reconnect OK (epoch %u)%s\n", m_reconnectEpoch, switchresReplayed ? ", modeline replayed" : "");
				}
				else
				{
					LOG(0,"[MiSTer] Reconnect failed (rc=%d); retrying in 1s\n", rc);
					m_noAckBlitCount = 10;
				}
				// either way this blit is skipped: the send buffers/queues
				// were just rebuilt (or are down)
				return;
			}
		}
	}

	if (!m_isConnected)
		return;
	if (!CanWriteBlitBuffer(field))
		return;

	m_frame = frame;
	uint16_t vSync = vCountSync;

	if (!vSync)
	{
		if (m_frame <= 10)
		{
			vSync = m_vTotal >> 1;
		}
		else
		{
			uint32_t timeCalc = (m_network_ping + margin + m_emulationTime >= m_frameTime) ? 0 : m_network_ping + margin + m_emulationTime - m_streamTime;
			vSync = (timeCalc == 0)
				? 1
				: static_cast<uint16_t>(m_vTotal - round(m_vTotal * timeCalc) / m_frameTime);
		}
	}

	if (m_dumpFrames) DumpFrame(field);   // corpus capture: raw pre-compression frame

	uint32_t cSize = 0;
	uint32_t cSizeDelta = 0;
	uint32_t bytesToSend = 0;
	double ratio_delta = 1.0;
	if (m_lz4Frames == GM_CODEC_NLC_TILED)
	{
		// NLC tiled (block-adaptive near-lossless): encodes the RAW frame (its own colour transform +
		// predictor). Output replaces the LZ4 buffer; reuses the 12-byte cSize blit header (no delta).
		if (m_preEncodedSize)
		{
			// pre-encode fast path: the caller already wrote an EncodeNLC frame into getPBufferPreEncoded()
			// (the per-blit software encode dominates the frame period on slow CPUs, e.g. ~40ms on the
			// MiSTer's Cortex-A9 at 240p — pre-encoding each unique frame once restores full send cadence)
			cSize = m_preEncodedSize;
			m_preEncodedSize = 0;
		}
		else
		{
			nlc_params np;
			buildNlcParams(&np);
			int r = nlc_encode((const uint8_t*)&m_pBufferBlit[field][0], (uint8_t*)m_pBufferLZ4[0], BUFFER_SIZE, &np);
			cSize = (r > 0) ? (uint32_t)r : 0;   // 0 -> raw fallback
		}
		cSizeDelta = cSize;
	}
	else if (m_lz4Frames)
	{
		double ratio_match = (double) matchDeltaBytes / m_RGBSize;
		if (!(m_lz4Frames % 2 == 0) || ratio_match < 1 || !m_delta_enabled[field]) // duplicated frame, compress only delta
		{
			switch (m_lz4Frames)
			{
				case(6):
				case(5):
				case(2):
				case(1): cSize = LZ4_compress_default((char *)&m_pBufferBlit[field][0], m_pBufferLZ4[0], m_RGBSize, m_RGBSize);
						 break;
				case(4):
				case(3): cSize = LZ4_compress_HC((char *)&m_pBufferBlit[field][0], m_pBufferLZ4[0], m_RGBSize, m_RGBSize, LZ4HC_CLEVEL_DEFAULT);
						 break;
			}
		}
		else
		{
			cSize = m_RGBSize;
		}
		cSizeDelta = cSize;
		double ratio_lz4 = (double) cSize / m_RGBSize;
		if ((m_lz4Frames % 2 == 0) && m_delta_enabled[field] && ratio_lz4 > 0.05 && ratio_match > 0.20 && ratio_match > 0.9 - ratio_lz4) // try_delta size
		{
			switch (m_lz4Frames)
			{
				case(6):
				case(5):
				case(2):
				case(1): cSizeDelta = LZ4_compress_default((char *)&m_pBufferBlitDelta[0], m_pBufferLZ4[1], m_RGBSize, m_RGBSize);
						 break;
				case(4):
				case(3): cSizeDelta = LZ4_compress_HC((char *)&m_pBufferBlitDelta[0], m_pBufferLZ4[1], m_RGBSize, m_RGBSize, LZ4HC_CLEVEL_DEFAULT);
						 break;
			}
			ratio_delta = (double) cSizeDelta / cSize;
		}

		if ((m_lz4Frames == 5 || m_lz4Frames == 6) && cSizeDelta > LZ4_ADAPTATIVE_CSIZE)
		{
			if (cSize <= cSizeDelta || m_lz4Frames == 5)
			{
				cSize = LZ4_compress_HC((char *)&m_pBufferBlit[field][0], m_pBufferLZ4[0], m_RGBSize, m_RGBSize, LZ4HC_CLEVEL_DEFAULT);
			}
			else
			{
				cSizeDelta = LZ4_compress_HC((char *)&m_pBufferBlitDelta[0], m_pBufferLZ4[1], m_RGBSize, m_RGBSize, LZ4HC_CLEVEL_DEFAULT);
				ratio_delta = (double) cSizeDelta / cSize;
			}
			m_lz4Frames = m_lz4Frames - 2;
			LOG(0,"[MiSTer] LZ4 Adaptative apply LZ4HC on frame %d\n", frame);
		}
	}

	mistercast::protocol::EncodeBlitHeader(
		reinterpret_cast<uint8_t*>(m_bufferSend), sizeof(m_bufferSend), frame, field, vSync);
	int headerSize = 0;
	if (cSize > 0)
	{
		if (ratio_delta < 0.95)
		{
			memcpy(&m_bufferSend[8], &cSizeDelta, sizeof(cSizeDelta));
			m_bufferSend[12] = 0x01; //frame_delta
			bytesToSend = cSizeDelta;
			headerSize = 13;
		}
		else
		{
			memcpy(&m_bufferSend[8], &cSize, sizeof(cSize));
			bytesToSend = cSize;
			headerSize = 12;
		}
	}
	else
	{
		if (m_delta_enabled[field] && matchDeltaBytes == m_RGBSize)
		{
			m_bufferSend[8] = 0x01; //frame_dup
			headerSize = 9;
		}
		else
		{
			bytesToSend = m_RGBSize;
			headerSize = 8;
		}
		if (m_lz4Frames && cSize == 0 && bytesToSend != 0)
		{
			// RIO registers the LZ4 buffers whenever compression is enabled. If
			// incompressible input does not fit in the raw-sized destination,
			// the protocol falls back to a raw header, so stage that raw field in
			// the registered buffer selected by the field number.
			memcpy(m_pBufferLZ4[field], m_pBufferBlit[field], m_RGBSize);
		}
	}

#ifdef _WIN32
	if (USE_RIO)
	{
		const ULONG payloadRequests = bytesToSend == 0
			? 0
			: static_cast<ULONG>((bytesToSend + m_mtu - 1) / m_mtu);
		if (!CanQueueRioSends(1 + payloadRequests))
		{
			++m_droppedVideoBatches;
			return;
		}
	}
#endif

	if (!Send(&m_bufferSend[0], headerSize))
	{
		++m_droppedVideoBatches;
		return;
	}
	if (bytesToSend == 0)
		return;

	if (m_doCongestionControl)
	{
		m_tickStart = m_tickCongestion;
		setTimeEnd();
		m_streamTime = DiffTime();
		while (m_streamTime < K_CONGESTION_TIME)
		{
			setTimeEnd();
			m_streamTime = DiffTime();
		}
	}

	setTimeStart();
	uint8_t buffer_blit = (cSize > 0) ? (ratio_delta < 0.95) ? 1 : 0 : field;
	if (!SendStream(0, buffer_blit, bytesToSend, (ratio_delta < 0.95) ? cSizeDelta : cSize))
	{
		++m_droppedVideoBatches;
		return;
	}
	setTimeEnd();
	m_streamTime = DiffTime();
	m_tickCongestion = m_tickEnd;
	m_doCongestionControl = (bytesToSend >= K_CONGESTION_SIZE) ? 1 : 0;
	m_delta_enabled[field] = 1;
}

void GroovyMister::CmdAudio(uint16_t soundSize)
{
	if (!fpga.audio || !m_isConnected)
	{
		return;
	}
	if (!CanWriteAudioBuffer())
		return;
#ifdef _WIN32
	if (USE_RIO)
	{
		const ULONG payloadRequests = soundSize == 0
			? 0
			: static_cast<ULONG>((soundSize + m_mtu - 1) / m_mtu);
		if (!CanQueueRioSends(1 + payloadRequests))
		{
			++m_droppedAudioBatches;
			return;
		}
	}
#endif

	m_bufferSend[0] = CMD_AUDIO;
	memcpy(&m_bufferSend[1], &soundSize, sizeof(soundSize));
	if (!Send(&m_bufferSend[0], 3))
	{
		++m_droppedAudioBatches;
		return;
	}

	if (!SendStream(1, 0, soundSize, 0))
		++m_droppedAudioBatches;
}

void GroovyMister::AlignFrame(uint32_t& frame, uint8_t& field)
{
	m_interlacePhase.Align(frame, field, fpga.frame, fpga.vgaF1);
}

uint32_t GroovyMister::getACK(DWORD dwMilliseconds)
{
	uint32_t getACKresult = 0;
	if (dwMilliseconds > 0)
	{
		setTimeStart();
	}
#ifdef _WIN32
	if (USE_RIO)
	{
		DrainRioSendCompletions();
		if (m_rioFailed || !m_rioFunctionsReady || m_hIOCP == NULL)
			return 0;

		static const DWORD RIO_MAX_RESULTS = 8;
		DWORD numberOfBytes = 0;
		ULONG_PTR completionKey = 0;
		OVERLAPPED* pOverlapped = 0;
		if (::GetQueuedCompletionStatus(m_hIOCP, &numberOfBytes, &completionKey, &pOverlapped, dwMilliseconds))
		{
			m_receiveNotificationArmed = false;
			RIORESULT results[RIO_MAX_RESULTS];
			ULONG numResults = m_rio.RIODequeueCompletion(m_receiveQueue, results, RIO_MAX_RESULTS);
			while (numResults != 0 && numResults != RIO_CORRUPT_CQ)
			{
				m_outstandingRioReceives = numResults >= m_outstandingRioReceives
					? 0
					: m_outstandingRioReceives - numResults;
				for (ULONG idx = 0; idx < numResults; ++idx)
				{
					if (results[idx].Status != ERROR_SUCCESS)
						continue;
					if (results[idx].BytesTransferred == mistercast::protocol::FpgaStatusSize) //blit ACK
					{
						if (dwMilliseconds > 0)
						{
							setTimeEnd();
							getACKresult = DiffTime();
						}
						else
						{
							getACKresult = 1;
						}
						setFpgaStatus();
					}
					if (results[idx].BytesTransferred == 1) //getVersion
					{
						if (dwMilliseconds > 0)
						{
							setTimeEnd();
							getACKresult = DiffTime();
						}
						else
						{
							getACKresult = 1;
						}
						memcpy(&m_core_version, &m_bufferReceive[0], 1);
					}
				}
				numResults = m_rio.RIODequeueCompletion(m_receiveQueue, results, RIO_MAX_RESULTS);
			}
			if (numResults == RIO_CORRUPT_CQ)
			{
				RecordRioError("RIO receive completion queue");
				return getACKresult;
			}
			if (!QueueRioReceive())
			{
				m_rioRecvRepostFailed++;
				return getACKresult;
			}
			ArmRioReceiveNotification();
		}
		else if (::GetLastError() == WAIT_TIMEOUT)
		{
			if (dwMilliseconds > 0)
				m_rioAckTimeout++;
		}
		else
		{
			m_receiveNotificationArmed = false;
			RecordRioError("GetQueuedCompletionStatus");
		}
		return getACKresult;
	}
#endif
	socklen_t sServerAddr = sizeof(struct sockaddr);
	int len = 0;
	uint32_t diff = 1;
	uint32_t dwNanoseconds = dwMilliseconds * 10000;
	do
	{
		len = recvfrom(m_sockFD, m_bufferReceive, sizeof(m_bufferReceive), 0, (struct sockaddr *)&m_serverAddr, &sServerAddr);
		if (dwMilliseconds > 0)
		{
			setTimeEnd();
			diff = DiffTime();
		}
		if (len == mistercast::protocol::FpgaStatusSize) //blit ACK
		{
			getACKresult = diff;
			setFpgaStatus();
		}
		if (len == 1) //get version
		{
			getACKresult = diff;
			memcpy(&m_core_version, &m_bufferReceive[0], 1);
		}
	} while ((len > 0) || (!getACKresult && dwNanoseconds > diff));
	return getACKresult;
}

void GroovyMister::WaitSync(void)
{
	if (!m_isConnected)
	  return;

	m_tickStart = m_tickSync;
	setTimeEnd();
	m_emulationTime = DiffTime();
	int sleepTime = (m_emulationTime >= m_frameTime) ? 0 : m_frameTime - m_emulationTime;
	int prevSleepTime = sleepTime;
	uint32_t realTime = 0;
	setTimeStart();
	do
	{
		int diffRaster = DiffTimeRaster();
		sleepTime = (diffRaster < 0 && abs(diffRaster) > sleepTime) ? 0 : sleepTime + diffRaster;
		setTimeEnd();
		realTime = DiffTime();

	} while (realTime <= (uint32_t) sleepTime);

	m_tickSync = m_tickEnd;

	if (((uint32_t) sleepTime + 10000 < realTime)) //sleep?
	{
		LOG(1,"[MiSTer] Frame %d Sleep prev=%d/final=%d/real=%d (frameTime=%d blitTime=%d emulationTime=%d) (vcount_vsync=%d/%d vcount_gpu=%d/%d)\n", m_frame, prevSleepTime, sleepTime, realTime, m_frameTime, m_streamTime, m_emulationTime, fpga.frameEcho, fpga.vCountEcho, fpga.frame, fpga.vCount);
	}

	// keep the send CQ empty + emit the telemetry summary every ~2s at
	// verbose level 1 (watch sendFailed/recvRepostFailed climbing alongside
	// ackTimeout — that is the send-CQ-full fingerprint)
#ifdef _WIN32
	if (USE_RIO)
	{
		DrainRioSendCompletions();
		uint64_t nowMs = monotonicMs();
		if (m_rioLastSummaryMs == 0 || (nowMs - m_rioLastSummaryMs) >= 2000)
		{
			m_rioLastSummaryMs = nowMs;
			LOG(1,"[MiSTer][RIO] frame=%u sendPosted=%llu sendFailed=%llu sendDrained=%llu recvRepostFailed=%llu ackTimeout=%llu noAck=%u\n",
				m_frame,
				(unsigned long long)m_rioSendPosted, (unsigned long long)m_rioSendFailed,
				(unsigned long long)m_rioSendDrained, (unsigned long long)m_rioRecvRepostFailed,
				(unsigned long long)m_rioAckTimeout, m_noAckBlitCount);
		}
	}
#endif
}

int GroovyMister::DiffTimeRaster(void)
{
	uint32_t frameEcho = fpga.frameEcho;
	int diffTime = 0;
	if (m_frame != fpga.frameEcho)
	{
		getACK(0);
	}
	if (fpga.frameEcho != frameEcho)
	{
		LOG(2,"[MiSTer] echo %d %d / %d %d \n", fpga.frameEcho, fpga.vCountEcho, fpga.frame, fpga.vCount);

		// Reconnect desync guard: bail before the multiply (also avoids the int
		// overflow a huge spread would hit in m_widthTime * dif). Returning 0
		// skips this frame's raster nudge — WaitSync then falls back to its
		// coarse frameTime pace, which is one un-aligned frame instead of a
		// multi-second hang.
		int64_t spread = (int64_t) fpga.frameEcho - (int64_t) fpga.frame;
		if (spread < 0) spread = -spread;
		if (spread > RASTER_MAX_FRAME_SPREAD)
		{
			LOG(2,"[MiSTer] raster spread %lld frames (echo %u vs gpu %u) - skipping sync (reconnect desync?)\n", (long long) spread, fpga.frameEcho, fpga.frame);
			return 0;
		}

		uint32_t vCount1 = ((fpga.frameEcho - 1) * m_vTotal + fpga.vCountEcho) >> m_interlace;
		uint32_t vCount2 = (fpga.frame * m_vTotal + fpga.vCount) >> m_interlace;
		int dif = (int) (vCount1 - vCount2) / 2; //dicotomic

		diffTime = (int) (m_widthTime * dif);
	}
	return diffTime;
}

void GroovyMister::BindInputs(const char* misterHost, uint16_t misterPort)
{
	// Set server
	m_serverAddrInputs.sin_family = AF_INET;
	m_serverAddrInputs.sin_port = htons(misterPort);
	m_serverAddrInputs.sin_addr.s_addr = inet_addr(misterHost);
	// Set socket
#ifdef _WIN32
	if (!m_wsaStarted)
	{
		WSADATA wsd;
		int rc = ::WSAStartup(MAKEWORD(2, 2), &wsd);
		if (rc != 0)
		{
			LOG(0, "[MiSTer][Inputs] Unable to load Winsock: %d\n", rc);
		}
		else
		{
			m_wsaStarted = true;
		}
	}
	m_sockInputsFD = INVALID_SOCKET;
	LOG(0, "[MiSTer][Inputs] Initialising socket %s...\n","");
	m_sockInputsFD = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_sockInputsFD == INVALID_SOCKET)
	{
		LOG(0,"[MiSTer][Inputs] Could not create socket : %lu", ::GetLastError());
	}
	LOG(0,"[MiSTer][Inputs] Setting socket async %s...\n","");
	u_long iMode=1;
	int rc2 = ioctlsocket(m_sockInputsFD, FIONBIO, &iMode);
	if (rc2 < 0)
	{
		LOG(0,"[MiSTer][Inputs] set nonblock fail %d\n", rc2);
	}
	LOG(0,"[MiSTer][Inputs] Binding port %s...\n","");
		sendto(m_sockInputsFD, m_bufferSend, 1, 0, (struct sockaddr *)&m_serverAddrInputs, sizeof(m_serverAddrInputs));

#else
	printf("[MiSTer][Inputs] Initialising socket...\n");
	m_sockInputsFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_sockInputsFD < 0)
	{
		LOG(0,"[MiSTer][Inputs] Could not create socket : %d", m_sockInputsFD);
	}
	LOG(0,"[MiSTer][Input] Setting socket async %s...\n","");
	// Non blocking socket
	int flags;
	flags = fcntl(m_sockInputsFD, F_GETFD, 0);
	if (flags < 0)
	{
		LOG(0,"[MiSTer][Inputs] get falg error %d\n", flags);
	}
	flags |= O_NONBLOCK;
	if (fcntl(m_sockInputsFD, F_SETFL, flags) < 0)
	{
		LOG(0,"[MiSTer] set nonblock fail %d\n", flags);
	}
	LOG(0,"[MiSTer][Inputs] Binding port %s...\n","");
	sendto(m_sockInputsFD, m_bufferSend, 1, 0, (struct sockaddr *)&m_serverAddrInputs, sizeof(m_serverAddrInputs));
#endif
}

void GroovyMister::PollInputs(void)
{
	if (!inputsBound())
	{
		return;
	}
	uint32_t joyFrame = joyInputs.joyFrame;
	uint8_t  joyOrder = joyInputs.joyOrder;
	uint32_t ps2Frame = ps2Inputs.ps2Frame;
	uint8_t  ps2Order = ps2Inputs.ps2Order;
	socklen_t sServerAddr = sizeof(struct sockaddr);
	int len = 0;
	do
	{
		len = recvfrom(m_sockInputsFD, m_bufferInputsReceive, sizeof(m_bufferInputsReceive), 0, (struct sockaddr *)&m_serverAddrInputs, &sServerAddr);
		if (len == 9 || len == 17 || len == 13 || len == 25) //blit joystick digital or analog (v1/v2)
		{
			memcpy(&joyFrame, &m_bufferInputsReceive[0], 4);
			memcpy(&joyOrder, &m_bufferInputsReceive[4], 1);
			if (joyFrame > joyInputs.joyFrame || (joyFrame == joyInputs.joyFrame && joyOrder > joyInputs.joyOrder))
			{
				setFpgaJoystick(len);
			}
		}
		if (len == 37 || len == 41) //blit ps2 keyboard and mouse
		{
			memcpy(&ps2Frame, &m_bufferInputsReceive[0], 4);
			memcpy(&ps2Order, &m_bufferInputsReceive[4], 1);
			if (ps2Frame > ps2Inputs.ps2Frame || (ps2Frame == ps2Inputs.ps2Frame && ps2Order > ps2Inputs.ps2Order))
			{
				setFpgaPS2(len);
			}
		}
	} while (len > 0);
}

void GroovyMister::SendRumble(uint8_t player, uint8_t strong, uint8_t weak)
{
	// rides the inputs socket (same one BindInputs registered); the core drops it
	// unless CMD_INIT advertised GM_CAP_RUMBLE and the OSD Rumble option is On.
	// Guarded: sendto'ing an unbound/uninitialized socket, or a v1-fallback
	// session (which must not emit rumble at all), is a silent no-op.
	if (!m_isConnected || !inputsBound() || !(m_negotiatedCaps & GM_CAP_RUMBLE))
	{
		return;
	}
	char msg[4];
	msg[0] = (char) player;
	msg[1] = (char) strong;
	msg[2] = (char) weak;
	msg[3] = 0;
	sendto(m_sockInputsFD, msg, 4, 0, (struct sockaddr *)&m_serverAddrInputs, sizeof(m_serverAddrInputs));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// PRIVATE
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
template <typename TV, typename TM>
inline TV RoundDown(TV Value, TM Multiple)
{
	return((Value / Multiple) * Multiple);
}

template <typename TV, typename TM>
inline TV RoundUp(TV Value, TM Multiple)
{
	return(RoundDown(Value, Multiple) + (((Value % Multiple) > 0) ? Multiple : 0));
}
#endif

//get aligned memory
char *GroovyMister::AllocateBufferSpace(const DWORD bufSize, const DWORD bufCount, DWORD& totalBufferSize, DWORD& totalBufferCount)
{
#ifdef _WIN32
	SYSTEM_INFO systemInfo;
	::GetSystemInfo(&systemInfo);

	const unsigned __int64 granularity = systemInfo.dwAllocationGranularity;
	const unsigned __int64 desiredSize = bufSize * bufCount;
	unsigned __int64 actualSize = RoundUp(desiredSize, granularity);

	if (actualSize > std::numeric_limits<DWORD>::max())
	{
		actualSize = (std::numeric_limits<DWORD>::max() / granularity) * granularity;
	}

	totalBufferCount = std::min<DWORD>(bufCount, static_cast<DWORD>(actualSize / bufSize));
	totalBufferSize = static_cast<DWORD>(actualSize) ;
	char *lBuffer = reinterpret_cast<char *>(VirtualAllocEx(GetCurrentProcess(), 0, totalBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

	if (lBuffer == 0)
	{
		LOG(0,"[MiSTer] VirtualAllocEx Error %lu\n", ::GetLastError());
	}

	return lBuffer;
#else
	totalBufferSize = bufSize;
	char *lBuffer = (char*)malloc((size_t)bufSize);
	return lBuffer;
#endif
}

#ifdef _WIN32
void GroovyMister::RecordRioError(const char* operation)
{
	++m_transportErrors;
	if (!m_rioFailed)
		LOG(0, "[MiSTer] %s failed (Winsock error %d)\n", operation, WSAGetLastError());
	m_rioFailed = true;
	m_isConnected = 0;
}

void GroovyMister::ReleaseRioSendContext(void* requestContext)
{
	for (ULONG slot = 0; slot < RIO_COMMAND_SLOTS; ++slot)
	{
		if (requestContext == &m_sendRioBuffers[slot])
		{
			m_commandSlotOutstanding[slot] = false;
			return;
		}
	}

	const uintptr_t context = reinterpret_cast<uintptr_t>(requestContext);
	for (int field = 0; field < 2; ++field)
	{
		if (m_pBufsBlit[field] == nullptr)
			continue;
		const uintptr_t begin = reinterpret_cast<uintptr_t>(m_pBufsBlit[field]);
		const uintptr_t end = begin + sizeof(RIO_BUF) * BUFFER_SLICES;
		if (context >= begin && context < end)
		{
			if (m_outstandingBlitSends[field] > 0)
				--m_outstandingBlitSends[field];
			return;
		}
	}

	if (m_pBufsAudio != nullptr)
	{
		const uintptr_t begin = reinterpret_cast<uintptr_t>(m_pBufsAudio);
		const uintptr_t end = begin + sizeof(RIO_BUF) * BUFFER_SLICES;
		if (context >= begin && context < end && m_outstandingAudioSends > 0)
			--m_outstandingAudioSends;
	}
}

void GroovyMister::DrainRioSendCompletions(void)
{
	if (!m_rioFunctionsReady || m_sendQueue == RIO_INVALID_CQ || m_outstandingRioSends == 0)
		return;

	RIORESULT results[128];
	for (;;)
	{
		const ULONG count = m_rio.RIODequeueCompletion(m_sendQueue, results, ARRAYSIZE(results));
		if (count == RIO_CORRUPT_CQ)
		{
			RecordRioError("RIO send completion queue");
			return;
		}
		if (count == 0)
			return;

		m_rioSendDrained += count;
		for (ULONG index = 0; index < count; ++index)
		{
			ReleaseRioSendContext(reinterpret_cast<void*>(
				static_cast<uintptr_t>(results[index].RequestContext)));
			if (m_outstandingRioSends > 0)
				--m_outstandingRioSends;
			if (results[index].Status != ERROR_SUCCESS && m_sockFD != INVALID_SOCKET)
				RecordRioError("RIO send completion");
		}
		if (count < ARRAYSIZE(results))
			return;
	}
}

void GroovyMister::DrainRioReceiveCompletions(void)
{
	if (!m_rioFunctionsReady || m_receiveQueue == RIO_INVALID_CQ || m_outstandingRioReceives == 0)
		return;

	RIORESULT results[8];
	for (;;)
	{
		const ULONG count = m_rio.RIODequeueCompletion(m_receiveQueue, results, ARRAYSIZE(results));
		if (count == RIO_CORRUPT_CQ || count == 0)
			return;
		m_outstandingRioReceives = count >= m_outstandingRioReceives
			? 0
			: m_outstandingRioReceives - count;
		if (count < ARRAYSIZE(results))
			return;
	}
}

bool GroovyMister::CanQueueRioSends(ULONG requestCount)
{
	DrainRioSendCompletions();
	return !m_rioFailed && m_rioFunctionsReady && m_requestQueue != RIO_INVALID_RQ &&
		m_outstandingRioSends <= RIO_SEND_QUEUE_SIZE &&
		requestCount <= RIO_SEND_QUEUE_SIZE - m_outstandingRioSends;
}

bool GroovyMister::QueueRioReceive(void)
{
	if (m_rioFailed || !m_rioFunctionsReady || m_requestQueue == RIO_INVALID_RQ ||
		m_outstandingRioReceives != 0)
	{
		return m_outstandingRioReceives != 0;
	}
	if (!m_rio.RIOReceive(m_requestQueue, &m_receiveRioBuffer, 1, 0, &m_receiveRioBuffer))
	{
		RecordRioError("RIOReceive");
		return false;
	}
	++m_outstandingRioReceives;
	return true;
}

bool GroovyMister::ArmRioReceiveNotification(void)
{
	if (m_receiveNotificationArmed)
		return true;
	if (m_rioFailed || !m_rioFunctionsReady || m_receiveQueue == RIO_INVALID_CQ)
		return false;
	ZeroMemory(&m_receiveOverlapped, sizeof(m_receiveOverlapped));
	if (m_rio.RIONotify(m_receiveQueue) != ERROR_SUCCESS)
	{
		RecordRioError("RIONotify");
		return false;
	}
	m_receiveNotificationArmed = true;
	return true;
}
#endif

bool GroovyMister::Send(void *cmd, int cmdSize)
{
#ifdef _WIN32
	if (USE_RIO)
	{
		if (cmd == nullptr || cmdSize <= 0 || cmdSize > static_cast<int>(sizeof(m_rioCommandBuffers[0])) ||
			!CanQueueRioSends(1))
		{
			return false;
		}

		for (ULONG slot = 0; slot < RIO_COMMAND_SLOTS; ++slot)
		{
			if (m_commandSlotOutstanding[slot])
				continue;
			memcpy(m_rioCommandBuffers[slot], cmd, cmdSize);
			m_sendRioBuffers[slot].Length = cmdSize;
			m_rioSendPosted++;
			if (!m_rio.RIOSend(
				m_requestQueue, &m_sendRioBuffers[slot], 1, RIO_MSG_DONT_NOTIFY, &m_sendRioBuffers[slot]))
			{
				m_rioSendFailed++;
				RecordRioError("RIOSend command");
				return false;
			}
			m_commandSlotOutstanding[slot] = true;
			++m_outstandingRioSends;
			return true;
		}
		return false;
	}
#endif
	return sendto(m_sockFD, static_cast<char*>(cmd), cmdSize, 0,
		reinterpret_cast<struct sockaddr*>(&m_serverAddr), sizeof(m_serverAddr)) == cmdSize;
}

bool GroovyMister::SendStream(uint8_t whichBuffer, uint8_t field, uint32_t bytesToSend, uint32_t cSize)
{
	uint32_t bytesSended = 0;
#ifdef _WIN32
	if (USE_RIO)
	{
		if (bytesToSend == 0)
			return true;
		const ULONG requestCount = static_cast<ULONG>((bytesToSend + m_mtu - 1) / m_mtu);
		if (requestCount > BUFFER_SLICES || !CanQueueRioSends(requestCount) ||
			(whichBuffer == 0 && (field > 1 || m_outstandingBlitSends[field] != 0)) ||
			(whichBuffer != 0 && m_outstandingAudioSends != 0))
		{
			return false;
		}

		DWORD flags = RIO_MSG_DONT_NOTIFY | RIO_MSG_DEFER;
		ULONG queued = 0;
		while (bytesSended < bytesToSend)
		{
			const ULONG chunkSize = (bytesToSend - bytesSended >= m_mtu)
				? m_mtu
				: bytesToSend - bytesSended;
			RIO_BUF* buffer = whichBuffer == 0
				? &m_pBufsBlit[field][queued]
				: &m_pBufsAudio[queued];
			buffer->Length = chunkSize;
			m_rioSendPosted++;
			if (!m_rio.RIOSend(m_requestQueue, buffer, 1, flags, buffer))
			{
				m_rioSendFailed++;
				if (queued != 0)
					m_rio.RIOSend(m_requestQueue, NULL, 0, RIO_MSG_COMMIT_ONLY, NULL);
				RecordRioError("RIOSend stream");
				return false;
			}
			++queued;
			++m_outstandingRioSends;
			if (whichBuffer == 0)
				++m_outstandingBlitSends[field];
			else
				++m_outstandingAudioSends;
			bytesSended += chunkSize;
		}
		if (!m_rio.RIOSend(m_requestQueue, NULL, 0, RIO_MSG_COMMIT_ONLY, NULL))
		{
			RecordRioError("RIO stream commit");
			return false;
		}
		return true;
	}
#endif
	while (bytesSended < bytesToSend)
	{
		const uint32_t chunkSize = (bytesToSend - bytesSended >= m_mtu) ? m_mtu : bytesToSend - bytesSended;
		bool sent = false;
		if (whichBuffer == 0)
		{
			sent = cSize > 0
				? Send(&m_pBufferLZ4[field][bytesSended], chunkSize)
				: Send(&m_pBufferBlit[field][bytesSended], chunkSize);
		}
		else
		{
			sent = Send(&m_pBufferAudio[bytesSended], chunkSize);
		}
		if (!sent)
			return false;
		bytesSended += chunkSize;
	}
	return true;
}

inline void GroovyMister::setTimeStart(void)
{
#ifdef _WIN32
	QueryPerformanceCounter(&m_tickStart);
#else
	clock_gettime(CLOCK_MONOTONIC, &m_tickStart);
#endif
}

inline void GroovyMister::setTimeEnd(void)
{
#ifdef _WIN32
	QueryPerformanceCounter(&m_tickEnd);
#else
	clock_gettime(CLOCK_MONOTONIC, &m_tickEnd);
#endif
}

uint32_t GroovyMister::DiffTime(void)
{
#ifdef _WIN32
	if (m_tickEnd.QuadPart <= m_tickStart.QuadPart)
		return 0;
	return mistercast::CounterTicksTo100ns(
		static_cast<uint64_t>(m_tickEnd.QuadPart - m_tickStart.QuadPart),
		m_timerFrequency);
#else
	uint32_t diffTime = 0;
	timespec temp;
	if ((m_tickEnd.tv_nsec - m_tickStart.tv_nsec) < 0)
	{
		temp.tv_sec = m_tickEnd.tv_sec - m_tickStart.tv_sec - 1;
		temp.tv_nsec = 1000000000 + m_tickEnd.tv_nsec - m_tickStart.tv_nsec;
	}
	else
	{
		temp.tv_sec = m_tickEnd.tv_sec - m_tickStart.tv_sec;
		temp.tv_nsec = m_tickEnd.tv_nsec - m_tickStart.tv_nsec;
	}
	diffTime = (temp.tv_sec * 1000000000) + temp.tv_nsec;
	return diffTime / 100;
#endif
}

bool GroovyMister::setFpgaStatus(void)
{
	mistercast::protocol::DecodedFpgaStatus status = {};
	if (!mistercast::protocol::DecodeFpgaStatus(
		reinterpret_cast<const uint8_t*>(m_bufferReceive), sizeof(m_bufferReceive), status) ||
		!mistercast::protocol::ShouldAcceptStatus(
			m_haveFpgaStatus, fpga.frameEcho, status.frameEcho))
	{
		return false;
	}

	fpga.frameEcho = status.frameEcho;
	fpga.vCountEcho = status.vCountEcho;
	fpga.frame = status.frame;
	fpga.vCount = status.vCount;
	fpga.vramReady     = (status.bits & 0x01) != 0;
	fpga.vramEndFrame  = (status.bits & 0x02) != 0;
	fpga.vramSynced    = (status.bits & 0x04) != 0;
	fpga.vgaFrameskip  = (status.bits & 0x08) != 0;
	fpga.vgaVblank     = (status.bits & 0x10) != 0;
	fpga.vgaF1         = (status.bits & 0x20) != 0;
	fpga.audio         = (status.bits & 0x40) != 0;
	fpga.vramQueue     = (status.bits & 0x80) != 0;
	m_haveFpgaStatus = true;
	m_interlacePhase.Acknowledge(m_frame, status.frameEcho);

	LOG(2,"[MiSTer] ACK %d %d / %d %d / bits(%d%d%d%d%d%d%d%d)\n", fpga.frameEcho, fpga.vCountEcho, fpga.frame, fpga.vCount, fpga.vramReady, fpga.vramEndFrame, fpga.vramSynced, fpga.vgaFrameskip, fpga.vgaVblank, fpga.vgaF1, fpga.audio, fpga.vramQueue);
	return true;
}

void GroovyMister::setFpgaJoystick(int len)
{
	memcpy(&joyInputs.joyFrame, &m_bufferInputsReceive[0], 4);
	memcpy(&joyInputs.joyOrder, &m_bufferInputsReceive[4], 1);
	int analogOfs = 0;
	if (len == 9 || len == 17) // v1: 16-bit button masks
	{
		joyInputs.joy1 = 0;
		joyInputs.joy2 = 0;
		memcpy(&joyInputs.joy1, &m_bufferInputsReceive[5], 2);
		memcpy(&joyInputs.joy2, &m_bufferInputsReceive[7], 2);
		if (len == 17) analogOfs = 9;
	}
	else // v2 (13/25): 32-bit button masks
	{
		memcpy(&joyInputs.joy1, &m_bufferInputsReceive[5], 4);
		memcpy(&joyInputs.joy2, &m_bufferInputsReceive[9], 4);
		if (len == 25) analogOfs = 13;
	}
	LOG(2,"[MiSTer] JOY %d %d / %d %d\n", joyInputs.joyFrame, joyInputs.joyOrder, joyInputs.joy1, joyInputs.joy2);

	if (analogOfs)
	{
		memcpy(&joyInputs.joy1LXAnalog, &m_bufferInputsReceive[analogOfs + 0], 1);
		memcpy(&joyInputs.joy1LYAnalog, &m_bufferInputsReceive[analogOfs + 1], 1);
		memcpy(&joyInputs.joy1RXAnalog, &m_bufferInputsReceive[analogOfs + 2], 1);
		memcpy(&joyInputs.joy1RYAnalog, &m_bufferInputsReceive[analogOfs + 3], 1);
		memcpy(&joyInputs.joy2LXAnalog, &m_bufferInputsReceive[analogOfs + 4], 1);
		memcpy(&joyInputs.joy2LYAnalog, &m_bufferInputsReceive[analogOfs + 5], 1);
		memcpy(&joyInputs.joy2RXAnalog, &m_bufferInputsReceive[analogOfs + 6], 1);
		memcpy(&joyInputs.joy2RYAnalog, &m_bufferInputsReceive[analogOfs + 7], 1);
		LOG(2,"[MiSTer] JOY A1(LX=%d,LY=%d,RX=%d,RY=%d) A2(LX=%d,LY=%d,RX=%d,RY=%d)\n", joyInputs.joy1LXAnalog, joyInputs.joy1LYAnalog, joyInputs.joy1RXAnalog, joyInputs.joy1RYAnalog, joyInputs.joy2LXAnalog, joyInputs.joy2LYAnalog, joyInputs.joy2RXAnalog, joyInputs.joy2RYAnalog);
	}
	if (len == 25) // v2 analog carries the triggers too
	{
		memcpy(&joyInputs.joy1LTAnalog, &m_bufferInputsReceive[21], 1);
		memcpy(&joyInputs.joy1RTAnalog, &m_bufferInputsReceive[22], 1);
		memcpy(&joyInputs.joy2LTAnalog, &m_bufferInputsReceive[23], 1);
		memcpy(&joyInputs.joy2RTAnalog, &m_bufferInputsReceive[24], 1);
		LOG(2,"[MiSTer] JOY T1(L=%u,R=%u) T2(L=%u,R=%u)\n", joyInputs.joy1LTAnalog, joyInputs.joy1RTAnalog, joyInputs.joy2LTAnalog, joyInputs.joy2RTAnalog);
	}
}

void GroovyMister::setFpgaPS2(int len)
{
	memcpy(&ps2Inputs.ps2Frame, &m_bufferInputsReceive[0], 4);
	memcpy(&ps2Inputs.ps2Order, &m_bufferInputsReceive[4], 1);

	if (m_verbose == 2)
	{
		LOG(2,"[MiSTer] KBD %d %d ", ps2Inputs.ps2Frame, ps2Inputs.ps2Order);
		for (int i=0; i<256; i++)
		{
			int bit_pre = 1 & (ps2Inputs.ps2Keys[i / 8] >> (i % 8));
			char *pos = &m_bufferInputsReceive[5];
			int bit_pos = 1 & (pos[i / 8] >> (i % 8));
			if (bit_pre != bit_pos)
			{
				LOG(2,"[%d=%d->%d]", i, bit_pre, bit_pos);
			}
		}
		LOG(2,"%s\n", "");
	}
	memcpy(&ps2Inputs.ps2Keys, &m_bufferInputsReceive[5], 32);

	if (len == 41)
	{
		memcpy(&ps2Inputs.ps2Mouse, &m_bufferInputsReceive[37], 1);
		memcpy(&ps2Inputs.ps2MouseX, &m_bufferInputsReceive[38], 1);
		memcpy(&ps2Inputs.ps2MouseY, &m_bufferInputsReceive[39], 1);
		memcpy(&ps2Inputs.ps2MouseZ, &m_bufferInputsReceive[40], 1);
		bitByte bits;
		bits.byte = ps2Inputs.ps2Mouse;
		LOG(2, "[MiSTer] MOUSE [yo=%d,xo=%d,ys=%d,xs=%d,1=%d,bm=%d,br=%d,bl=%d][x=%d,y=%d,z=%d]\n", bits.u.bit7, bits.u.bit6, bits.u.bit5, bits.u.bit4, bits.u.bit3, bits.u.bit2, bits.u.bit1, bits.u.bit0, ps2Inputs.ps2MouseX, ps2Inputs.ps2MouseY, ps2Inputs.ps2MouseZ);
	}
}
