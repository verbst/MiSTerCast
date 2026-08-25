# Contributor and Agent Guide

This file contains the implementation, build, and hardware-validation details intentionally omitted from the user-facing README. Keep `README.md` and `FrontEnd/README.txt` concise and task-oriented. Put protocol rationale, performance trade-offs, diagnostic interpretation, and repeatable engineering procedures here.

## Project priorities

MiSTerCast is a low-latency Windows desktop sender for Groovy_MiSTer. Correct field delivery, bounded resource ownership, responsive stop/restart, and gaming latency take priority over smoothing short network stalls.

Do not assume that behavior from another platform is automatically appropriate for Windows. This sender uses Desktop Duplication, WASAPI, Windows Registered I/O (RIO), and the FPGA acknowledgement clock. Any pacing or buffering change needs deterministic tests and a direct-Ethernet frame-counter test before it becomes the default.

Both `Release|x86` and `Release|x64` are supported deliverables. x64 exists because the NLC codec (see below) needs more headroom than x86 comfortably gives on slower CPUs; x86 remains buildable and is what most of the capture-side validation below was originally run against.

## Current streaming behavior

### Capture and transform

- Desktop Duplication captures BGRA frames into a three-entry CPU bitmap ring.
- GUI display sources use Desktop Duplication. GUI window sources use `Windows.Graphics.Capture` through the `IGraphicsCaptureItemInterop` HWND path, so overlapping windows are not composited into the selected source. A minimized source pauses and the renderer repeats the newest complete frame until capture resumes.
- Window size changes recreate the two-entry WinRT frame pool after the outstanding frame is closed. Display/window switches stop and join the capture worker before replacing D3D resources.
- A CPU-readable D3D11 staging texture is reused until the capture dimensions or DXGI format changes. `[capture] Created reusable staging texture ...` should appear only at capture startup, source-size/format change, or capture reinitialization.
- Capture loss keeps the worker alive and reinitializes Desktop Duplication instead of terminating the GUI.
- The render worker always transforms the newest complete capture. Static desktops may produce a low `capture=` rate because Desktop Duplication reports only changes; `rate=` is the relevant output cadence.
- Output buffers are owned by the Groovy_MiSTer transport. A field is not overwritten while an outstanding RIO send still references it.
- For any non-delta codec (raw, plain LZ4, LZ4HC, NLC), `CmdBlit`'s compressed output buffer selection (`buffer_blit`) follows `field`, so the two fields pipeline across `m_pBufferLZ4[0]`/`[1]` instead of both serializing through slot 0. `CanWriteBlitBuffer` matches: it only requires *both* slots idle for the even-valued LZ4+delta codecs (the only ones where a payload can land in slot 1 regardless of field), and checks just `field`'s slot otherwise. Before this fix, every non-delta codec always targeted slot 0, so a session had no real double-buffering at all - field-tested on plain LZ4 and NLC, both accumulated `dropped_video` continuously (hundreds of drops per minute at 60fps/256x240) purely from this serialization, with no actual network or transport problem.

### Sampling modes

`SamplingMode` and the transform algorithms live in `Library/MiSTerCastLib/FrameTransform.h`. They are based on the deterministic implementation in [MiSTerCast-Linux](https://github.com/fjsj/MiSTerCast-Linux).

- **Point** builds centered nearest-neighbor row and column tables. It is the compatibility default.
- **Bilinear** uses centered texel coordinates and eight-bit fixed-point fractions. It blends the four adjacent source pixels and explicitly handles a fraction that rounds to a whole texel.
- **Line Blend** uses point sampling horizontally and exact area weights vertically. Each destination row's integer weights sum to the source-axis extent, including fractional reduction and enlargement. A reciprocal-based rounded divide preserves a constant image exactly.
- Rotation is represented as an axis mapping. A quarter turn swaps axes, so Line Blend filters source columns without a separate special case.
- Interlaced field buffers select full output row `row * 2 + !(field & 1)`, preserving the Groovy_MiSTer protocol's field/display parity. Filtering is computed for the logical full-height output before the requested field rows are selected.
- Tables and scratch vectors are `thread_local` and retain capacity after warm-up. The filters add transform work but do not queue or buffer an additional frame.

The legacy native `SetSource` export remains ABI-compatible and selects Point. GUI and CLI callers use `SetSourceEx`, which adds the sampling value. `SetCaptureWindow` accepts a pointer-sized HWND and keeps both source exports ABI-compatible. GUI save files are version 5. Version 1 (the format both pre-merge forks shared) loads fully. Versions 2-4 are pre-merge, single-fork formats that used the same version numbers for mutually incompatible field layouts past the version-1 prefix (fjsj inserted the progressive-framebuffer field before the capture block; verbst appended codec/RGB/MTU fields after it) — the loader restores the modeline from these but stops there rather than guess-parse the divergent tail; see `MainWindow.xaml.cs`.

### NLC codec, auto-reconnect, and controller v2 (from verbst/Groovy_MiSTer)

- `Codec` defaults to `CodecLZ4` (works on any stock Groovy_MiSTer core). `CodecNLC` is opt-in from the Stream Options panel / `SetStreamOptions`, and only works against verbst's NLC-capable core build.
- NLC encoding lives in `nlc_codec.cpp/h` (vendored, protocol-frozen: it must match the FPGA decoder bit-for-bit). `GroovyMister::EncodeNLC`/`buildNlcParams` drive it from `m_rgbMode`/`m_nlcPack`/`m_nearLevel`/`m_nlcWidth`. There is a pre-encode fast path (`getPBufferPreEncoded`/`setPreEncodedSize`) for callers whose CPU can't keep up with per-blit software encoding at the frame rate.
- `setAutoReconnect(1)` arms a watchdog in `CmdBlit`: after `WATCHDOG_RECONNECT_MS` (1.5s wall-clock, not a blit count — see below) with no `frameEcho` advance it tears down the video side only (the inputs socket and its local port survive), rebuilds the session, and replays the last `CmdSwitchres` modeline. `reconnectEpoch()` increments on each successful reconnect; `renderer_nogpu::draw()` watches it to realign its own frame counter, since `resetSessionState()` zeroes the core's counters on every `CmdInit` but the host's counter would otherwise keep climbing from its old value.
  - The gate was originally a raw consecutive-blit count (verbst's original: 10 blits, ~167ms at 60Hz). Field-tested against a real NLC core, that fired a disruptive reconnect every 1-2 seconds even on an otherwise healthy link: `frameEcho` advanced in bursts, with observed 300-800ms stretches of zero advance in between, on a session with no other traffic on the wire besides video. The same core, same content, same codec, stayed rock solid (`reconnect_epoch` never advancing) once *any* other traffic was flowing on the connection (audio enabled with real audio playing) - toggling only whether audio packets were actually being sent was enough to flip the behavior, which points at the core's own receive/ACK cadence rather than anything transport-side. That is outside what this Windows sender can fix directly; switching the gate to wall-clock time (`WATCHDOG_WARN_MS`/`WATCHDOG_RECONNECT_MS` in `groovymister.cpp`) absorbs the observed burst pattern regardless of its root cause, while still recovering a genuinely dead link within about 1.5s. Re-tune these constants if a future core/link combination shows longer legitimate stalls than that.
- Input capability negotiation (`GM_CAP_INPUTS_V2`, `GM_CAP_RUMBLE`) probes the core with `CMD_GET_VERSION` before sending a longer `CMD_INIT`, because a core older than `GROOVY_VERSION` 2 silently discards a `CMD_INIT` longer than 5 bytes (no ACK at all) instead of rejecting the extra byte gracefully.
- RIO completion-path telemetry (`m_rioSendPosted/Failed/Drained`, `m_rioRecvRepostFailed`, `m_rioAckTimeout`) exists because an undrained send completion queue was field-diagnosed as the root cause of an audio-load stall that looked like a dead connection. Watch these alongside `dropped_video`/`dropped_audio` when debugging a stall.
- Opt-in raw-frame dump (`setFrameDump`, or env `GM_FRAME_DUMP`) captures pre-compression frames for building an NLC test corpus.

### Bugs found while merging the two forks' transport rewrites

`groovymister.cpp`/`.h` were independently rewritten by both forks (fjsj for non-blocking multi-slot RIO sends, verbst for the NLC protocol and reconnect state machine) and had to be hand-merged. Two real bugs surfaced in verbst's version during that merge and were fixed rather than carried forward:

- The ACK receive RIO buffer was registered at 17 bytes (`RIORegisterBuffer(m_bufferReceive, 17)`) against a 13-byte `m_bufferReceive` array — an out-of-bounds write on any receive that filled the registered length. The wire status packet is 13 bytes (`mistercast::protocol::FpgaStatusSize`); the registration now matches `sizeof(m_bufferReceive)`.
- `DiffTime()` computed `m_tickEnd.QuadPart - m_tickStart.QuadPart` and treated the result as already being in 100ns units, which is only true if `QueryPerformanceFrequency()` happens to return exactly 10 MHz. It now scales by the measured frequency (`mistercast::CounterTicksTo100ns`), matching fjsj's original implementation.

### Interlace phase and framebuffer modes

- After start or `CMD_SWITCHRES`, an alternating local field sequence begins at protocol field zero.
- Stale status from the previous raster cannot choose the new mode's field.
- The sender locks to the FPGA-relative Windows field formula only after an acknowledgement matches a post-switch blit.
- This recovery addresses [upstream issue #9](https://github.com/iequalshane/MiSTerCast/issues/9).
- Default interlaced delivery sends alternating half-height field buffers using protocol interlace mode `1`.
- The opt-in full-height framebuffer uses protocol mode `2`, always sends command field zero, and lets the receiver derive both display fields. It roughly doubles transform work and payload.
- Automatic frame delay caps interlaced sync requests to the first half of the raster so a field upload cannot race the field being displayed. Manual frame-delay values remain explicit.

The sender deliberately has no UDP socket-rate shaper, adaptive interlaced reserve, or audio prebuffer. Direct-Ethernet frame-counter tests measured HDMI parity or one frame of lag with the existing RIO/FPGA-acknowledgement pacing. Revisit those choices only with gaming-latency evidence.

### Transport and route validation

- RIO completion draining is non-blocking in the render path.
- If a slow link still owns a complete video or audio buffer, the next batch is dropped instead of reusing registered memory or growing an unbounded queue.
- `dropped_video`, `dropped_audio`, `rio_outstanding`, and `transport_errors` expose this state.
- Stream startup resolves the Windows-selected local route with the IP Helper API and checks that its interface supports the configured 1500-byte IP packet.
- This is a local-interface check, not end-to-end path-MTU discovery. The UDP socket also sets `IP_DONTFRAGMENT` so downstream failures become transport errors.
- `[stream]` includes `path_mtu`, `route_mtu`, and `route_if`.
- Host-name lookup is supported by the GUI, runs off the UI thread, and logs the selected IPv4 address. The CLI intentionally requires a raw IPv4 address for reproducible route selection.

Wi-Fi can overwhelm the receiver badly enough to make the MiSTer-side menu unresponsive. The same behavior has been observed with the original sender, so the practical recovery is a good direct-Ethernet stream or a core restart, not more sender buffering.

### Video payload pacing (link-speed mismatch)

`SendStream`'s RIO path commits a whole field's packets to the NIC in one batch once it has more than `mistercast::PacingBurstPackets` (32) packets queued (`StreamingTiming.h`). A source NIC faster than the MiSTer's gigabit link (e.g. 2.5G through a switch) can otherwise overrun the switch's egress buffer toward the MiSTer; the tail of the burst is silently dropped, the FPGA sees an incomplete field, and interlaced modes lock into the fallback framebuffer — visible as the picture periodically freezing or jumping, with the sender reporting no drops or errors of its own. Above that threshold, sends are released in 32-packet bursts gated on an absolute `QueryPerformanceCounter` schedule (`GroovyMister::SleepUntilQpc`) targeting `PacingBitsPerSecond` (950 Mb/s, just under gigabit). A payload at or under the threshold (audio, a small or highly-compressed field) is still sent unpaced in one commit.

This is a direct port of the equivalent fix in [MiSTerCast-Linux](https://github.com/ElFDA/mistercast-linux) (`StreamTimingPolicy`/`sendPayload`), which used `sendmmsg()` + `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` for the same burst/pace split; verified there on a 2.5G → switch → 1G bench at 720x576i (0 fallback frames across 68k+ frames, versus failing within ~4100 unpaced). Confirm the Windows port the same way: a direct-Ethernet or matched-speed baseline should show no regression in `rate`/timing, and a 2.5G(or faster)-NIC-through-a-switch-to-a-1G-MiSTer bench at a large interlaced mode (720x480i/576i) should show no fallback lock and no `dropped_video` growth over a long run where the unpaced build reliably fails within a few thousand frames.

### Audio

- WASAPI loopback captures the default render endpoint's 32-bit floating-point mix.
- Mono is duplicated to stereo. For multichannel endpoints, front-left and front-right are selected.
- Samples are converted to signed 16-bit PCM.
- The newest accumulated audio is sent with each render cycle. A long-stall backlog is capped to the newest protocol-safe block.
- There is no prebuffer, preserving the low-latency gaming path.

### Protocol and timing correctness

- The 13-byte status packet is decoded explicitly as little-endian data.
- Status ordering uses wrapping 32-bit frame counters so acknowledgements remain valid across rollover.
- The calculated frame-delay sync line is passed to `CmdBlit`; do not replace it with a literal zero.
- Progressive-framebuffer modelines and RGB sizes are validated against the fixed transport buffer before streaming.
- Invalid timing order, zero dimensions, odd-height field buffers, and oversized RGB payloads must fail validation rather than corrupt transport memory.

## Diagnostics

The GUI and CLI write timestamped logs under `%LOCALAPPDATA%\MiSTerCast\Logs`; CLI `--log-directory` overrides the location.

One `[stream]` line per second includes:

- `mode`, `framebuffer`, and `sampling`;
- output `rate`, changed-frame `capture` rate, and `capture_repeat`;
- `max_gap_ms`, `max_transform_ms`, and `max_send_wait_ms`;
- command, FPGA, echo, phase, F1, vcount, and protocol-ready flags;
- field repeats, frame realignments, and injected fault totals;
- stream/emulation/target timing and chosen sync line;
- route MTU and RIO ownership/drop/error counters.

The GUI retains only the latest telemetry line in its log panel so long sessions do not grow the visual tree indefinitely.

CLI fault controls are opt-in and must not affect normal GUI streams:

- `--skip-every N` advances the logical frame but omits each Nth blit while preserving one field period.
- `--stall-every N --stall-ms M` pauses before each Nth blit.
- `--cycles N` repeats start/stop in one process.
- `--switch-modeline NAME` switches halfway through each cycle.

## Build dependencies

Install Visual Studio 2022 or Visual Studio 2022 Build Tools with:

- Desktop development with C++
- MSVC v143
- a Windows 10 or Windows 11 SDK
- MSBuild
- .NET Framework 4.7.2 targeting pack

No particular newer SDK build is required. MTU validation links `Iphlpapi.lib`, which is part of the Windows SDK and adds no separate package or runtime dependency.

Single-window capture uses the Windows SDK C++/WinRT headers and `WindowsApp.lib`, requires C++17, and has no additional redistributable dependency. The runtime feature requires Windows 10 version 1903 or newer.

LZ4 is vendored source (not a prebuilt DLL) in `Library/MiSTerCastLib/lz4/` and compiles directly into `MISTERCASTLIB.dll` — there is nothing to download and no `msys-lz4-1.dll` to copy. `MiSTerCast.csproj`'s post-build step only copies `MISTERCASTLIB.dll`/`.pdb` from the matching `x86`/`x64` output directory.

## Build and deterministic tests

From a Visual Studio developer shell:

```powershell
msbuild FrontEnd\MiSTerCast.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x86
Tests\Release\MiSTerCastTests.exe
```

Some agent-hosted PowerShell sessions inherit both `Path` and `PATH`. If MSBuild fails before compilation with `MSB6001` and reports those duplicate environment keys, start a child shell with PowerShell's `Start-Process -UseNewEnvironment`, set `TEMP` and `TMP` inside that child to a writable task-specific directory, and run MSBuild with `/m:1 /nr:false`. Disabling parallelism and node reuse prevents an earlier MSBuild worker from retaining the duplicate environment. A directly launched native child can preserve both case variants even when PowerShell displays only one. This is a session-launcher issue; do not modify persistent user or machine environment variables to work around it.

The tests compile with `/W4 /WX`. Keep deterministic coverage for:

- point, bilinear, and line-blend pixels, rotations, reductions, enlargements, flat images, large ratios, and invalid modes;
- interlaced parity and progressive framebuffer output;
- phase recovery before/after matching acknowledgements and frame-counter rollover;
- stream timing/modeline/buffer validation;
- RIO lifecycle, completion ownership, raw fallback for incompressible fields, repeated close, and failed init;
- audio conversion/backlog bounds, diagnostic fault schedules, source-state snapshots, staging-resource keys, and MTU arithmetic.

Build and run the complete suite before every implementation commit.

## Direct-Ethernet hardware tests

The CLI binary is `FrontEnd\bin\Release\MiSTerCastCli.exe`. Desktop Duplication requires an interactive user desktop. When running through automation, launch the CLI as a visible process in the signed-in session; a detached shell can fail `DuplicateOutput` with `E_ACCESSDENIED`.

Set the direct-Ethernet address for the current bench:

```powershell
$MisterIp = "192.168.200.2"
```

Baseline 480i frame-counter test:

```powershell
FrontEnd\bin\Release\MiSTerCastCli.exe --target $MisterIp --modeline "720x480i NTSC (60Hz)" --duration 15 --test-pattern --no-audio
```

Repeated phase-recovery stress test:

```powershell
FrontEnd\bin\Release\MiSTerCastCli.exe --target $MisterIp --modeline "720x480i NTSC (60Hz)" --switch-modeline "640x480i NTSC (60Hz)" --duration 8 --cycles 3 --capture-width 720 --capture-height 480 --no-audio --skip-every 17 --stall-every 29 --stall-ms 40
```

Line Blend performance test with a high-resolution source crop:

```powershell
FrontEnd\bin\Release\MiSTerCastCli.exe --target $MisterIp --modeline "720x480i NTSC (60Hz)" --duration 10 --capture-width 1920 --capture-height 1080 --sampling line-blend --no-audio
```

Also test `--sampling point`, `--sampling bilinear`, and `--progressive-framebuffer` when their code paths change.

For 480i, verify:

1. `rate` remains close to 60 fields/s.
2. Every start and switch reports `phase=local`, then locks on the first matching post-switch acknowledgement.
3. Field order is visually stable; a moving counter must not alternate or jump backward.
4. `dropped_video=0`, `dropped_audio=0`, and `transport_errors=0` on direct Ethernet.
5. `rio_outstanding` drains rather than increasing without bound.
6. `max_transform_ms` leaves enough of the roughly 16.7 ms field interval for send/wait work.
7. `path_mtu=1500` and `route_mtu>=1500` use the intended Ethernet interface.
8. HDMI comparison is the same frame or no more than one frame behind.
9. Repeated stop/start leaves both GUI and MiSTer responsive.

Sampling changes normally need deterministic pixel tests plus one direct-Ethernet Line Blend run. They do not require visual A/B approval unless the exact filter appearance or gaming latency is in question.

Reference direct-Ethernet results from the current implementation:

- Phase-recovery stress: three 720x480i/640x480i cycles with every 17th blit skipped and every 29th stalled for 40 ms produced 81 skips and 48 stalls with no RIO drops or transport errors.
- Progressive interlace framebuffer: 720x480i/640x480i held about 59.9-60.3 updates/s with no drops or errors.
- Line Blend: two 1920x1080-to-480i cycles with live 720-to-640 switches held 59.92-60.32 fields/s; maximum transform time was 3.72 ms with no drops or transport errors.

## Known gap: MiSTerCastCli does not expose stream options yet

`Tools/MiSTerCastCli/Program.cs` is unchanged from fjsj's original and only drives the capture/sampling/modeline surface (`SetSourceEx`, `SetModelineEx`, the fault-injection flags). It never calls `SetStreamOptions`, so a CLI-run session always uses whatever `stream_config` defaults `MiSTerCastLib.cpp::Initialize` set (LZ4, RGB888, MTU 1500, auto-reconnect on) and cannot be pointed at the NLC codec or a different RGB mode/MTU. Add `--codec`/`--rgb-mode`/`--mtu`/`--nlc-pack`/`--near-level`/`--auto-reconnect` flags calling `SetStreamOptions` before extending the hardware test matrix below to cover NLC.

## Documentation and commit discipline

- Keep root and packaged READMEs user-friendly. Move internal rationale and exhaustive procedures here.
- When another platform implementation is relevant, link its repository; otherwise describe this sender's behavior directly without platform comparisons.
- Document new dependencies and minimum versions in this file and mention only user-installed runtime requirements in the README.
- Keep logically separate behavior in separate commits, and build/test each commit.
- Reference upstream issues in the specific commit that fixes them.
- Preserve unrelated worktree changes and do not rewrite published history unless explicitly requested.
