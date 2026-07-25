# MiSTerCast
A general-purpose tool for streaming your Windows PC screen to your MiSTer through the Groovy_MiSTer core.

This is not a replacement for Groovy_Mame or other integrated emulators.	

Make sure you already have Groovy_Mame working well with Groovy_MiSTer before using MiSTerCast. A direct ethernet connection to your MiSTer is recommended.
https://github.com/lutechsource/MiSTerStuff/blob/main/GroovyMiSTer/mame_documentation.md
https://github.com/psakhis/Groovy_MiSTer

The Microsoft VC++ x64 Redistributable is required: https://aka.ms/vs/17/release/vc_redist.x64.exe
(An x86 build is also available, which needs https://aka.ms/vs/17/release/vc_redist.x86.exe instead.)

For audio, you will need to enable audio on the Groovy_MiSTer core.

## Stream options
These ride the Groovy connection handshake, so they apply at the next **Start Stream** and are locked while streaming.

| Option | Notes |
| --- | --- |
| Codec | LZ4 works on every Groovy core. NLC is near-lossless and holds 60fps where LZ4 cannot, but needs an NLC-capable core. Raw is for debugging only. |
| NLC Pack | NLC only. Rice compresses best but needs a Rice-capable core (`rbf_rice_r3` or newer) |
| NLC NEAR | NLC only. 0 is lossless; 1 is visually indistinguishable on a CRT and is recommended, as lossless peaks can saturate the MiSTer's ingest and cause brief noise banding. |
| RGB Mode | RGB888 normally. RGB565 halves bandwidth for some colour banding. RGBA8888 costs a third more bandwidth for an alpha channel the MiSTer does not display. |
| MTU | 1500, or 3800 if you have turned on *Server → Jumbo frames* in the core's OSD. |
| Auto-reconnect | Rebuilds the session by itself after a core restart or a dropped link. |
| Log Level | 0 errors and handshake, 1 adds network telemetry (use this to diagnose dropped frames), 2 full trace. |
| Allow oversize modes | Permits modelines above 1024x576. The frame size limit still applies. |

## Known issues
- Frames may be dropped or doubled due to sync with video signal.
- At least 1-2 frames of latency.
- High refresh rate monitors are not supported due to frame times. Please change your monitor to ~60hz.

## Notes
The current pre-defined modelines are just for testing. You can add your own in modelines.dat.
It's best to use a refresh that matches your PC for better sync.

Modelines are validated before use. A mode is refused if its blanking does not enclose the active area, or if one frame would exceed the Groovy client's 1,245,312 byte buffer (`width * height * bytes-per-pixel`, halved for interlaced modes). The reason is shown in the UI and Start Stream is disabled until it is fixed — RGB565 or an interlaced mode is usually the quickest way back inside the limit.

Find more modeline examples here: https://www.geocities.ws/podernixie/htpc/modes-en.html

## Building
Open `FrontEnd/MiSTerCast.sln` and build the `x64` (or `x86`) configuration. There are no external dependencies to fetch — LZ4 and the Groovy client are vendored in `Library/MiSTerCastLib/` (see its `PROVENANCE.md`).
