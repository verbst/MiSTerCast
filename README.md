# MiSTerCast

A general-purpose tool for streaming your Windows PC screen and loopback audio to your MiSTer through the Groovy_MiSTer core.

This is not a replacement for GroovyMAME or another emulator with integrated Groovy_MiSTer support.

Make sure you already have GroovyMAME working well with Groovy_MiSTer before using MiSTerCast. A direct Ethernet connection to your MiSTer is strongly recommended — if the MiSTer is also reachable over Wi-Fi, enter the Ethernet adapter's raw IPv4 address so a host name cannot select the slower route.
- https://github.com/lutechsource/MiSTerStuff/blob/main/GroovyMiSTer/mame_documentation.md
- https://github.com/psakhis/Groovy_MiSTer

## Origin

This is a merge of two independent forks of the original [iequalshane/MiSTerCast](https://github.com/iequalshane/MiSTerCast):

- **[fjsj/MiSTerCast](https://github.com/fjsj/MiSTerCast)** — single-window capture, Point/Bilinear/Line Blend sampling, an optional full-height framebuffer for interlaced output, safe stop/restart behavior, and non-blocking host-name lookup and network transport.
- **[verbst/MiSTerCast](https://github.com/verbst/MiSTerCast)**, paired with **[verbst/Groovy_MiSTer](https://github.com/verbst/Groovy_MiSTer)** — the NLC near-lossless codec, auto-reconnect, extended controller/rumble support, and stream diagnostics.

Both forks independently rewrote the low-level Windows RIO network transport (`Library/MiSTerCastLib/groovymister.cpp`) for different reasons — fjsj for non-blocking send/receive robustness, verbst for the NLC protocol and reconnect logic. That file (and the rest of the capture/protocol pipeline) was hand-merged to combine both: see the commit history in `Library/MiSTerCastLib/` for the reasoning behind each merge decision.

## Requirements

- A MiSTer running the Groovy_MiSTer core. The **NLC codec is optional** and needs [verbst's updated core](https://github.com/verbst/Groovy_MiSTer); everything else, including the default LZ4 codec, works on the standard [psakhis/Groovy_MiSTer](https://github.com/psakhis/Groovy_MiSTer) core.
- The Microsoft VC++ x64 Redistributable: https://aka.ms/vs/17/release/vc_redist.x64.exe (an x86 build is also available, which needs the x86 redistributable instead: https://aka.ms/vs/17/release/vc_redist.x86.exe)
- Audio enabled in the core if you want sound.
- A Windows display running near the output refresh rate, normally about 60 Hz.
- Windows 10 version 1903 or newer to cast a single application window instead of a full display.

## Features

- **Single-window capture** — cast one application window instead of a whole display. Requires Windows 10 1903+; a minimized window pauses on its last frame until restored.
- **Sampling** — Point (sharp, pixel-exact, the default), Bilinear (smooths neighboring pixels), or Line Blend (an area-weighted vertical filter that reduces CRT line shimmer while keeping horizontal sharpness).
- **Full-height framebuffer** for interlaced modes — sends one full-frame blit instead of alternating half-height fields, which can improve receiver stability at roughly double the video bandwidth.
- **NLC near-lossless codec** (opt-in, needs the NLC core) — holds 60fps at resolutions LZ4 cannot.
- **Auto-reconnect** — rebuilds the session by itself after a core restart or a dropped link.
- Diagnostic logs under `%LOCALAPPDATA%\MiSTerCast\Logs`, and `Tools\MiSTerCastCli` for repeatable frame-counter, mode-switch, stop/start, and sampling tests.

## Stream options

These ride the Groovy connection handshake, so they apply at the next **Start Stream** and are locked while streaming.

| Option | Notes |
| --- | --- |
| Codec | LZ4 (default) works on every Groovy core. NLC is near-lossless and holds 60fps where LZ4 cannot, but needs the NLC-capable core linked above. Raw is for debugging only. |
| NLC Pack | NLC only. Rice compresses best but needs a Rice-capable core (`rbf_rice_r3` or newer) — an older core misparses Rice as TILED and shows garbage with no error. |
| NLC NEAR | NLC only. 0 is lossless; 1 is visually indistinguishable on a CRT and is recommended, as lossless peaks can saturate the MiSTer's ingest and cause brief noise banding. |
| RGB Mode | RGB888 normally. RGB565 halves bandwidth for some colour banding. RGBA8888 costs a third more bandwidth for an alpha channel the MiSTer does not display. |
| MTU | 1500, or 3800 if you have turned on *Server → Jumbo frames* in the core's OSD. |
| Auto-reconnect | Rebuilds the session by itself after a core restart or a dropped link. |
| Log Level | 0 errors and handshake, 1 adds network telemetry (use this to diagnose dropped frames), 2 full trace. |
| Allow oversize modes | Permits modelines above 1024x576. The frame size limit still applies. |

## Known issues

- Frames may be dropped or doubled due to sync with video signal.
- At least 1-2 frames of latency.
- High refresh rate monitors are not supported due to frame times. Please change your monitor to ~60Hz.
- Nothing above 720x480i is currently recommended because of MiSTer-side throughput.
- Wi-Fi can overload the MiSTer-side receiver badly enough to make its menu unresponsive; use direct Ethernet.

## Notes

The current pre-defined modelines are just for testing. You can add your own in `modelines.dat`. It's best to use a refresh that matches your PC for better sync.

Modelines are validated before use. A mode is refused if its blanking does not enclose the active area, or if one frame would exceed the Groovy client's 1,245,312 byte buffer (`width * height * bytes-per-pixel`, halved for interlaced modes unless the full-height framebuffer option is on). The reason is shown in the UI and Start Stream is disabled until it is fixed — RGB565 or a non-full-height interlaced mode is usually the quickest way back inside the limit.

Find more modeline examples here: https://www.geocities.ws/podernixie/htpc/modes-en.html

## Building

Open `FrontEnd/MiSTerCast.sln` and build the `x64` (or `x86`) configuration. There are no external dependencies to fetch — LZ4 and the Groovy client are vendored in `Library/MiSTerCastLib/`.

`Tests/MiSTerCastTests.vcxproj` and `Tools/MiSTerCastCli/MiSTerCastCli.csproj` are wired into the solution. See `AGENTS.md` for the engineering notes on the merge and what to validate on real hardware.

## License

None of the three upstream repositories this project merges (iequalshane/MiSTerCast, fjsj/MiSTerCast, verbst/MiSTerCast) publishes an OSS license file. Treat this merge the same way: no license is granted here beyond what upstream implicitly allows. If you plan to distribute this publicly, check with the upstream authors first and add an explicit license.
