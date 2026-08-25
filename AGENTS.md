# Contributor and Agent Guide

This file contains the implementation, build, and hardware-validation details intentionally omitted from the user-facing README. Keep `README.md` and `FrontEnd/README.txt` concise and task-oriented. Put protocol rationale, performance trade-offs, diagnostic interpretation, and repeatable engineering procedures here.

## Project priorities

MiSTerCast is a low-latency Windows desktop sender for Groovy_MiSTer. Correct field delivery, bounded resource ownership, responsive stop/restart, and gaming latency take priority over smoothing short network stalls.

Do not assume that behavior from another platform is automatically appropriate for Windows. This sender uses Desktop Duplication, WASAPI, Windows Registered I/O (RIO), and the FPGA acknowledgement clock. Any pacing or buffering change needs deterministic tests and a direct-Ethernet frame-counter test before it becomes the default.

The supported build is `Release|x86`. The x64 project configuration is not a supported deliverable.

## Current streaming behavior

### Capture and transform

- Desktop Duplication captures BGRA frames into a three-entry CPU bitmap ring.
- GUI display sources use Desktop Duplication. GUI window sources use `Windows.Graphics.Capture` through the `IGraphicsCaptureItemInterop` HWND path, so overlapping windows are not composited into the selected source. A minimized source pauses and the renderer repeats the newest complete frame until capture resumes.
- Window size changes recreate the two-entry WinRT frame pool after the outstanding frame is closed. Display/window switches stop and join the capture worker before replacing D3D resources.
- A CPU-readable D3D11 staging texture is reused until the capture dimensions or DXGI format changes. `[capture] Created reusable staging texture ...` should appear only at capture startup, source-size/format change, or capture reinitialization.
- Capture loss keeps the worker alive and reinitializes Desktop Duplication instead of terminating the GUI.
- The render worker always transforms the newest complete capture. Static desktops may produce a low `capture=` rate because Desktop Duplication reports only changes; `rate=` is the relevant output cadence.
- Output buffers are owned by the Groovy_MiSTer transport. A field is not overwritten while an outstanding RIO send still references it.

### Sampling modes

`SamplingMode` and the transform algorithms live in `Library/MiSTerCastLib/FrameTransform.h`. They are based on the deterministic implementation in [MiSTerCast-Linux](https://github.com/fjsj/MiSTerCast-Linux).

- **Point** builds centered nearest-neighbor row and column tables. It is the compatibility default.
- **Bilinear** uses centered texel coordinates and eight-bit fixed-point fractions. It blends the four adjacent source pixels and explicitly handles a fraction that rounds to a whole texel.
- **Line Blend** uses point sampling horizontally and exact area weights vertically. Each destination row's integer weights sum to the source-axis extent, including fractional reduction and enlargement. A reciprocal-based rounded divide preserves a constant image exactly.
- Rotation is represented as an axis mapping. A quarter turn swaps axes, so Line Blend filters source columns without a separate special case.
- Interlaced field buffers select full output row `row * 2 + !(field & 1)`, preserving the Groovy_MiSTer protocol's field/display parity. Filtering is computed for the logical full-height output before the requested field rows are selected.
- Tables and scratch vectors are `thread_local` and retain capacity after warm-up. The filters add transform work but do not queue or buffer an additional frame.

The legacy native `SetSource` export remains ABI-compatible and selects Point. GUI and CLI callers use `SetSourceEx`, which adds the sampling value. `SetCaptureWindow` accepts a pointer-sized HWND and keeps both source exports ABI-compatible. GUI save files are version 4; versions 1 and 2 load with Point sampling, and versions 1-3 load in display mode.

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

Download LZ4 1.9.4 from https://github.com/lz4/lz4/releases/download/v1.9.4/lz4_win32_v1_9_4.zip and extract it to `External/lz4`. The Win32 projects link `External/lz4/dll/liblz4.dll.a`; post-build steps copy `msys-lz4-1.dll` beside the GUI and CLI.

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

## Documentation and commit discipline

- Keep root and packaged READMEs user-friendly. Move internal rationale and exhaustive procedures here.
- When another platform implementation is relevant, link its repository; otherwise describe this sender's behavior directly without platform comparisons.
- Document new dependencies and minimum versions in this file and mention only user-installed runtime requirements in the README.
- Keep logically separate behavior in separate commits, and build/test each commit.
- Reference upstream issues in the specific commit that fixes them.
- Preserve unrelated worktree changes and do not rewrite published history unless explicitly requested.
