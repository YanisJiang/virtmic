# Task 3 Report: 控制协议（命令 / 事件）

## Status

**DONE**

## What was implemented

Shared header-only command/event wire protocol with GoogleTest coverage wired into `shared_tests`.

| File | Role |
|------|------|
| `shared/include/virtmic/protocol.hpp` | `Command` / `EventType` / `MessageHeader` / `BypassPayload` + encode/decode |
| `shared/tests/test_protocol.cpp` | `Protocol.RoundTripBypass`, `Protocol.InferTimeoutEvent` (brief verbatim) |
| `shared/CMakeLists.txt` | Added `tests/test_protocol.cpp` to `shared_tests` |

### API

- `magic = 0x564D4943` (`VMIC`)
- `Command`: Start=1, Stop=2, SetBypass=3, SimulateInferTimeout=4
- `EventType`: Started=1, Stopped=2, InferTimeout=3, Error=4, Latency=5
- `encode_command(Command)` — no payload (`Start` / `Stop` / `SimulateInferTimeout`)
- `encode_command(Command, BypassPayload)` — 4-byte payload (`SetBypass`)
- `encode_event(EventType, EmptyPayload = {})` — no payload for current events
- `decode_command` / `decode_event` → `std::nullopt` on bad magic or short payload

## TDD evidence

### RED — failing test first

Created only:

- `shared/tests/test_protocol.cpp` (exact brief contents)
- Updated `shared/CMakeLists.txt` to list the new test source

No `protocol.hpp` yet.

Commands:

```text
cmake --build build --config Debug
ctest --test-dir build -C Debug -R Protocol --output-on-failure
```

Observed (expected failure — missing header):

```text
  test_protocol.cpp
D:\Workspace\AI\virtmic\shared\tests\test_protocol.cpp(3,10): error C1083: 无法打开包括文件: "virtmic/protocol.hpp": No such file or directory [D:\Workspace\AI\virtmic\build\shared\shared_tests.vcxproj]
```

Note: `cmake`/`ctest` not on PATH; used VS 2022 bundled CMake:

`D:\Soft\Microsoft\Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`

### GREEN — minimal implementation then re-run

After adding `shared/include/virtmic/protocol.hpp`:

```text
cmake --build build --config Debug
ctest --test-dir build -C Debug -R Protocol --output-on-failure
```

Result:

```text
Test project D:/Workspace/AI/virtmic/build
    Start 4: Protocol.RoundTripBypass
1/2 Test #4: Protocol.RoundTripBypass .........   Passed    0.04 sec
    Start 5: Protocol.InferTimeoutEvent
2/2 Test #5: Protocol.InferTimeoutEvent .......   Passed    0.05 sec

100% tests passed, 0 tests failed out of 2

Total Test time (real) =   0.13 sec
```

Full suite (Tasks 1–3):

```text
1/5 Test #1: PcmConstants.FrameGeometry .......   Passed
2/5 Test #2: RingBuffer.PushPopOneFrame .......   Passed
3/5 Test #3: RingBuffer.RejectWhenFull ........   Passed
4/5 Test #4: Protocol.RoundTripBypass .........   Passed
5/5 Test #5: Protocol.InferTimeoutEvent .......   Passed
100% tests passed, 0 tests failed out of 5
```

## Commit

```text
feat: 添加引擎控制命令与事件协议
```

Branch: `feat/phase1-passthrough` (not pushed).

## Concerns

1. **Command vs event discrimination** — both use the same header shape and overlapping numeric `type` values; callers must pick `decode_command` vs `decode_event` (no wire discriminator beyond usage context).
2. **Brief tests omit negative paths** — bad magic / short `SetBypass` payload / no-payload commands are implemented to return `nullopt` but not covered by the two brief cases.
3. **Endianness** — host-endian `memcpy` of `uint32_t`; fine for same-machine IPC; not portable across architectures.
4. **Event payloads** — `Error` / `Latency` have no payload schema yet; `EmptyPayload` placeholder matches the InferTimeout test.
5. **Report/path tooling** — CMake still requires the VS-bundled path on this machine (same as Tasks 1–2).

## Review fix (Task 3 finding)

### Change

- `decode_command` for `SetBypass`: require exact payload size (`hdr.nbytes != sizeof(uint32_t)` → `nullopt`), not `nbytes < 4`.
- `decode_event`: require empty payload (`hdr.nbytes != 0` → `nullopt`).
- Added `Protocol.OversizedSetBypassPayloadRejected` (8-byte claimed payload → `nullopt`).

### Re-run

```text
cmake --build build --config Debug
ctest --test-dir build -C Debug -R Protocol --output-on-failure
```

Result:

```text
Test project D:/Workspace/AI/virtmic/build
    Start 4: Protocol.RoundTripBypass
1/3 Test #4: Protocol.RoundTripBypass .....................   Passed    0.08 sec
    Start 5: Protocol.InferTimeoutEvent
2/3 Test #5: Protocol.InferTimeoutEvent ...................   Passed    0.04 sec
    Start 6: Protocol.OversizedSetBypassPayloadRejected
3/3 Test #6: Protocol.OversizedSetBypassPayloadRejected ...   Passed    0.08 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   0.27 sec
```
