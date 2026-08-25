<Bold>MiSTerCast</Bold>

MiSTerCast streams your Windows PC screen and loopback audio to the Groovy_MiSTer core. It is not a replacement for GroovyMAME or another emulator with integrated Groovy_MiSTer support.

A direct Ethernet connection is strongly recommended. If the MiSTer is also on Wi-Fi, enter the raw IPv4 address of its Ethernet adapter.
<Hyperlink>https://github.com/lutechsource/MiSTerStuff/blob/main/GroovyMiSTer/mame_documentation.md</Hyperlink>
<Hyperlink>https://github.com/psakhis/Groovy_MiSTer</Hyperlink>

The standard build is x64 and needs the Microsoft VC++ x64 Redistributable. An x86 build is also available.

<Bold>Features</Bold>
- Point is the sharp, pixel-exact sampling default. Bilinear smooths neighboring pixels. Line Blend reduces vertical CRT shimmer while preserving horizontal sharpness; with 90-degree rotation it blends source columns instead.
- Full-height framebuffer mode is available for interlaced output, but roughly doubles video bandwidth.
- The GUI can cast a complete display or isolate one application window. Single-window capture requires Windows 10 version 1903 or newer. A minimized single-window source pauses on its last frame until restored.
- NLC near-lossless compression holds 60fps at resolutions LZ4 cannot, but needs a MiSTer running the NLC-capable Groovy_MiSTer core build. LZ4 is the default and works on every standard Groovy_MiSTer core.
- Auto-reconnect rebuilds the session by itself if the core restarts or the link drops, instead of leaving the stream dead.
- Diagnostic logs are stored under %LOCALAPPDATA%\MiSTerCast\Logs.
- MiSTerCastCli.exe provides repeatable frame-counter, mode-switch, stop/start, and sampling tests.

<Bold>Stream options</Bold>
These are all part of the connection handshake, so they are fixed for the life of a session: set them before you press Start Stream. They are greyed out while streaming.

Codec - how each frame is compressed before it goes on the wire.
  LZ4 works on every Groovy core and is the default, safe choice.
  NLC is a near-lossless codec that holds 60fps at resolutions LZ4 cannot, but needs a Groovy core built with NLC support: <Hyperlink>https://github.com/verbst/Groovy_MiSTer</Hyperlink>
  Raw is for debugging only; it will exceed the MiSTer's ingest bandwidth at anything above 240p.

NLC Pack - only applies when the codec is NLC.
  Rice gives the best compression, but it needs a Rice-capable core. An older core will misparse Rice data as TILED and show a garbled picture with no error message at all. If NLC looks like scrambled noise, switch this to TILED.

NLC NEAR - only applies when the codec is NLC. 0 is fully lossless. 1 is visually indistinguishable on a CRT and is recommended; it keeps peak frame sizes well under the MiSTer's ingest limit, whereas 0 can saturate it and cause brief horizontal noise banding on busy scenes.

RGB Mode - RGB888 is the normal choice. RGB565 halves the bandwidth at the cost of some colour banding. RGBA8888 carries an alpha channel that the MiSTer does not display, and uses a third more bandwidth than RGB888 for no benefit.

MTU - leave at 1500 unless you have switched Jumbo frames on in the Groovy core's OSD, under Server.

Auto-reconnect - rebuilds the session by itself if the core restarts or the link drops, instead of leaving the stream dead.

Log Level - 0 shows errors and the connection handshake. 1 adds periodic network telemetry, which is what to use when diagnosing dropped frames. 2 is a full trace and is very noisy.

Allow oversize modes - permits modelines above 1024x576. The frame size limit still applies regardless; this only relaxes the check on what a fixed-frequency CRT can reasonably sync.

<Bold>Audio</Bold>
You will need to enable audio on the Groovy_MiSTer core, under Audio in its OSD.

Groovy accepts 22050, 44100 and 48000 Hz only. MiSTerCast streams whatever rate Windows is mixing at, so if your playback device is set to anything else audio will be disabled and a message will say so. Set the device to 48000 Hz in Windows Sound settings. Surround setups are downmixed to front left and right.

<Bold>Healthy direct-Ethernet test</Bold>
- Output stays near 60 fields per second for 480i.
- Phase changes from local to locked just after start or a mode switch.
- dropped_video, dropped_audio, and transport_errors stay at zero.
- The moving frame counter matches HDMI or is no more than one frame behind.

<Bold>Known limitations</Bold>
- Wi-Fi can overload the MiSTer-side receiver. Use direct Ethernet for stable, low-latency streaming.
- Nothing above 720x480i is currently recommended because of MiSTer-side throughput.
- High-refresh Windows desktops are not supported well. Match the desktop refresh to the output when possible.
- At least 1-2 frames of latency.

<Bold>Notes</Bold>
The bundled modelines are starting points. You can add your own in modelines.dat. It's best to use a refresh that matches your PC for better sync.

Modelines are checked before they are used. A mode is refused if the blanking does not enclose the active area, or if a single frame would be larger than the Groovy client can hold - 1,245,312 bytes, which is width x height x bytes-per-pixel, halved for interlaced modes (unless the full-height framebuffer option is on). If a mode is refused the reason is shown under the stream options and Start Stream is disabled until you fix it. Picking RGB565 or a (non full-height) interlaced mode is usually the quickest way to bring a large mode back inside the limit.

Find more modeline examples here: <Hyperlink>https://www.geocities.ws/podernixie/htpc/modes-en.html</Hyperlink>

<Bold>Contact</Bold>
You can find the MiSTerCast community on the official MiSTer Discord.
