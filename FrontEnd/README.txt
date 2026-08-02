<Bold>MiSTerCast 1.04</Bold>

Updated reconnection code to be more reliable and to remove post-connect graphical issue.

This version requires an updated set of Groovy_MiSTer NLC files are required to be deployed on your MiSTer.
<Hyperlink>https://github.com/verbst/Groovy_MiSTer</Hyperlink>


<Bold>MiSTerCast 1.03</Bold>

MiSTerCast is a general-purpose tool for streaming your PC screen to your MiSTer through the Groovy_MiSTer core. 
This is not a replacement for Groovy_Mame or other integrated emulators.

An ethernet connection to your MiSTer is recommended.
<Hyperlink>https://github.com/verbst/Groovy_MiSTer</Hyperlink>

The Microsoft VC++ x64 Redistributable is required for the standard build.

<Bold>Stream options</Bold>
These are all part of the connection handshake, so they are fixed for the life of a session: set them before you press Start Stream. They are greyed out while streaming.

Codec - how each frame is compressed before it goes on the wire.
  LZ4 works on every Groovy core and is the safe choice if you are unsure.
  NLC is a near-lossless codec that holds 60fps at resolutions LZ4 cannot, but needs a Groovy core built with NLC support.
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

<Bold>Known issues</Bold>
- Frames may be dropped or doubled due to sync with video signal.
- At least 1-2 frames of latency.
- High refresh rate monitors are not supported due to frame times. Please change your monitor to ~60hz.

<Bold>Notes</Bold>
The current pre-defined modelines are just for testing. You can add your own in modelines.dat.
It's best to use a refresh that matches your PC for better sync.

Modelines are checked before they are used. A mode is refused if the blanking does not enclose the active area, or if a single frame would be larger than the Groovy client can hold - 1,245,312 bytes, which is width x height x bytes-per-pixel, halved for interlaced modes. If a mode is refused the reason is shown under the stream options and Start Stream is disabled until you fix it. Picking RGB565 or an interlaced mode is usually the quickest way to bring a large mode back inside the limit.

Find more modeline examples here: <Hyperlink>https://www.geocities.ws/podernixie/htpc/modes-en.html</Hyperlink>

<Bold>Contact</Bold>
You can find me on the official MiSTer Discord
