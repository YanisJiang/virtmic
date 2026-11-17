# virtmic Design Spec

**Date:** 2026-11-17  
**Status:** Ready for user review before implementation planning  
**Location:** `D:\Workspace\AI\virtmic` (under the AI tools portal `D:\Workspace\AI`)

## Goal

virtmic is a Windows virtual microphone that relays the user’s real microphone through a local RVC voice-conversion pipeline and exposes the result as a system microphone selectable by Discord, Zoom, and similar real-time call apps.

## Confirmed requirements

| Topic | Decision |
|-------|----------|
| Primary use | Real-time calls (Discord / Zoom); low latency is a hard requirement |
| Voice AI | Local RVC on NVIDIA CUDA |
| Control UI | System tray for daily use + optional advanced settings window |
| Virtual mic | Ship / integrate our own virtual microphone driver (not “please install VB-CABLE”) |
| Placement | Subproject folder under the AI portal; project name `virtmic` |

## Architecture (Approach 2: split stack)

Four units with clear process boundaries:

```
[Real microphone]
      │ WASAPI capture
      ▼
[Audio engine]  ←──IPC──→  [RVC infer process (CUDA)]
      │ converted PCM
      ▼
[VirtMic virtual mic driver] ──→ Discord / Zoom / other apps
      ▲
[Tauri control app: tray + advanced window] ──IPC──→ engine / infer / driver status
```

### Responsibilities

- **Driver** — Expose “VirtMic” as a capture device; consume PCM from the engine; no AI, no UI.
- **Engine** — Capture, clocking, ring buffers, bypass vs block policy, feed frames to infer and write results to the driver.
- **Infer** — Load / unload / hot-switch RVC models; CUDA inference; health signals. Must not write to the driver directly.
- **Control app** — Tray: on/off, voice model, input device, latency/status, manual bypass. Advanced window: model paths, buffer/frame size, levels, logs, driver status.

### Repository layout (target)

- `driver/` — Virtual microphone driver (C++ / WDK)
- `engine/` — Low-latency audio engine (prefer C++ for shared types/tooling with the WDK driver; Rust only if it clearly wins for the WASAPI path)
- `infer/` — RVC inference service (Python and/or ONNX Runtime + CUDA)
- `app/` — Tauri tray + advanced settings
- `shared/` — PCM format, frame duration, IPC commands/events, error codes
- `docs/` — Specs and plans

## Data flow and latency budget

### Happy path (voice conversion on)

1. Engine captures PCM from the real mic via WASAPI (prefer 48 kHz to match call apps).
2. Frames enter a ring buffer; when a window is ready (about 10–20 ms), send to infer.
3. Infer returns converted PCM for that window (or agreed hop size).
4. Engine aligns/resamples lightly if needed and writes into the driver’s shared buffer.
5. Call apps read converted audio from “VirtMic”.

### Bypass

Manual bypass (user-initiated from tray) is the **only** path that may write original (unconverted) audio to VirtMic.

### Latency budget (v1 targets)

| Stage | Target |
|-------|--------|
| Capture buffer | ~10–20 ms |
| RVC inference | ~30–80 ms (model / GPU dependent) |
| Output / driver | ~10–20 ms |
| **End-to-end** | **Aim under 120 ms; acceptable upper bound ~200 ms** |

Tray shows estimated latency. Continuous overload is handled by the timeout policy below, not by silently degrading quality without notice.

### Shared PCM contract

Fixed in `shared/` (no ad hoc renegotiation):

- Sample rate: 48 kHz
- Channels: mono for the conversion path (engine may downmix mic input)
- Inter-process PCM: little-endian `float32`
- Driver edge: convert to the format Windows / the driver stack requires (typically 16-bit or float as declared by the device)
- Frame duration: chosen in implementation within 10–20 ms; one constant shared by engine and infer

## Module interfaces

### `driver/`

- **In:** PCM from shared memory or named pipe; start/stop of the render/capture path as required by the audio stack.
- **Out:** System microphone device “VirtMic”; underrun / ready status.
- **Non-goals:** Capture from real hardware, UI, model loading.

### `engine/`

- **In:** Real mic PCM; control commands (start/stop, input device, bypass on/off, timeout threshold); infer PCM responses / health.
- **Out:** PCM to driver; events to control app (`InferTimeout`, latency stats, device errors).
- **Timeout policy:** On infer timeout or non-response → **block**: write silence (or stop writing audible audio) to VirtMic; emit `InferTimeout`. Do **not** auto-fallback to original voice.
- **Non-goals:** Model file management UI, rich settings chrome.

### `infer/`

- **In:** PCM windows; load/unload/switch model commands.
- **Out:** Converted PCM windows; model info; health check.
- **Failure mode:** Crash or hang is detected by the engine as timeout/unhealthy → block + notify. Infer does not touch the driver.

### `app/`

- **In:** Engine/infer/driver status events.
- **Out:** User commands; non-blocking prompts on `InferTimeout` with retry / turn-off conversion actions.
- **Lifecycle:** Control app may start the engine; engine may start and supervise the infer process. If the engine is not running, VirtMic outputs silence.

## Error handling

Prefer explicit failure over silent original-voice leak or unexplained mute.

| Situation | Behavior |
|-----------|----------|
| Infer timeout / no response | Block VirtMic (silence); tray prompt; user can retry or disable conversion |
| Infer process crash | Same as timeout; mark unhealthy; optional single auto-restart; if still failing, stay blocked and prompt |
| Real mic busy / permission failure | Do not start the processing path; explain in UI |
| Driver missing / unsigned load failure | Detect at startup; advanced window shows install / test-signing guidance |
| GPU / CUDA unavailable | Refuse to enter conversion mode; prompt to check drivers/CUDA |
| User enables Bypass | Original voice passed through to VirtMic (only allowed original-voice path) |
| Engine exits | VirtMic stays silent; no stale buffer replay |

## Testing (v1)

- **Unit:** Ring buffer; timeout detection; bypass vs block transitions (optional short fade to avoid clicks).
- **Integration:** Engine ↔ infer IPC round-trip latency; timeout always blocks and always emits an event.
- **End-to-end:** VirtMic appears in Windows sound settings; Discord/Zoom can capture converted audio; disable / timeout / crash match the error table.
- **Performance:** Tray latency estimate matches perceived delay; GPU load acceptable for calls.

## Out of scope (v1)

- Cloud inference
- Multi-user / accounts
- Mobile
- Production EV code-signing / Store distribution (dev/test signing first)
- Shipping a dependency on third-party virtual cables as the primary path

## Success criteria

1. User installs VirtMic driver (test-signed acceptable for v1) and sees “VirtMic” as an input device.
2. With conversion on, call apps hear RVC-converted voice with end-to-end latency typically under ~200 ms on the target NVIDIA machine.
3. Infer timeout or crash results in blocked (silent) output plus a clear user prompt—not original-voice leakage.
4. Tray covers daily controls; advanced window covers model/buffer/diagnostics.
5. Manual bypass is available and is the only original-voice passthrough.
