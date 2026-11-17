# Task 7 Report: 引擎主循环 + 控制管道 + CLI

## Status

**DONE**

## What was implemented

| File | Role |
|------|------|
| `engine/include/virtmic/engine.hpp` | `IFrameSource` + `Engine::create` / start / stop / bypass / timeout / retry |
| `engine/src/engine.cpp` | ~10 ms worker：`decide()` → Passthrough 写源帧 / Silence 写零 |
| `engine/include/virtmic/control_server.hpp` | 命名管道 `\\.\pipe\VirtMicControl` |
| `engine/src/control_server.cpp` | 解码 `Command` 并驱动 `Engine`；可选回写 event |
| `engine/src/main_cli.cpp` | `virtmic-engine.exe`：WASAPI（失败则 SineSource）+ SHM + 控制管道 |
| `tools/virtmic_ctl.cpp` | `start` / `stop` / `bypass` / `timeout` / `retry` |
| `engine/tests/test_engine_loop.cpp` | SineSource 集成：Start 非静音 → timeout 静音 → bypass 仍静音 → retry 恢复 |
| `shared/include/virtmic/protocol.hpp` | 新增 `Command::Retry = 5`（无 payload） |
| `engine/CMakeLists.txt` | 库源、`virtmic-engine`、`virtmic_ctl`、测试接线 |

### Behavior

- Start 后 worker 每 ~10 ms 按 `EnginePolicy::decide()` 写 SHM
- **InferTimeout 阻断优先于 bypass**：Silence 路径只调用 `write_silence_frame()`，不写源 PCM
- `retry` 清阻断后 + bypass/直通恢复非零帧
- CLI：`virtmic-engine` 前台服务；`virtmic_ctl` 发协议命令

## TDD evidence

### RED

CMake 引用尚未存在的源文件：

```text
CMake Error at engine/CMakeLists.txt:1 (add_library):
  Cannot find source file:
    src/engine.cpp
```

### GREEN

```text
ctest --test-dir build -C Debug -R "EngineLoop|EnginePolicy|ShmWriter|Protocol" --output-on-failure
100% tests passed, 0 tests failed out of 7
```

Including `EngineLoop.StartPassthroughTimeoutSilenceThenRetryRestore`.

CMake via VS 2022 bundled path (not on PATH).

## Commit

```text
feat: 引擎主循环、控制管道与 CLI
```

Branch: `feat/phase1-passthrough` (not pushed).

## Concerns

1. **`Command::Retry` 扩展协议** — Task 3 原无 Retry；ctl 手测步骤需要，已加 `Retry=5`。
2. **Start 前也会写 Silence 帧** — worker 在 `create` 后即跑，`decide()` 未 Start 时写零；浪费少量 SHM 带宽，行为安全。
3. **控制管道单实例 `nMaxInstances=1`** — 停服时靠客户端连一下唤醒 `ConnectNamedPipe`；高并发控制端未做。
4. **CLI stdout 重定向可能块缓冲** — 交互控制台正常；管道/重定向时事件日志可能延迟。

## Review follow-up (Important fixes)

### 1) Event ownership — ControlServer must not steal `poll_event`

**Root cause:** `serve_client_` called `engine_.poll_event()` after commands and wrote the event to the pipe. `InferTimeout` is single-shot; this raced the CLI loop in `main_cli.cpp` and often left stdout without `event: InferTimeout` after `virtmic_ctl timeout`.

**Fix:** Control pipe is command-only — removed `poll_event` / event reply from `ControlServer`. CLI retains sole ownership of engine events via `poll_event`.

**Test:** `ControlServer.TimeoutLeavesInferTimeoutForCliPoll` — pipe `SimulateInferTimeout`, then `engine->poll_event()` still returns `InferTimeout`.

### 2) Race — Silence vs `write_frame` after timeout

**Root cause:** `process_one_` did `decide()` under lock, unlocked, then `write_frame`. Timeout between unlock and write could leak one source frame after block.

**Fix:** Hold `mu_` across `decide()` and the subsequent `write_silence_frame()` / `write_frame()` so Silence cannot lose to a raced Passthrough write.

### Verification

```text
ctest --test-dir build -C Debug -R "EngineLoop|EnginePolicy|ShmWriter|ControlServer|Protocol" --output-on-failure
100% tests passed, 0 tests failed out of 8
```
