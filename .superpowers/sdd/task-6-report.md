# Task 6 Report: WASAPI 采集

## Status

**DONE**

## What was implemented

| File | Role |
|------|------|
| `engine/include/virtmic/wasapi_capture.hpp` | `WasapiCapture`：`start` / `stop` / `read_frame` |
| `engine/src/wasapi_capture.cpp` | Shared-mode WASAPI capture → 48 kHz mono float frames |
| `engine/tests/test_wasapi_capture_smoke.cpp` | Optional smoke：无麦 `GTEST_SKIP` |
| `engine/CMakeLists.txt` | 加入源/测试；链接 `ole32` + `uuid` |

### Behavior

- Default capture endpoint via `IMMDeviceEnumerator` / `IAudioClient` / `IAudioCaptureClient`
- Mix format：float32 或 PCM 16/24/32；多声道均值 downmix 为 mono
- 非 48 kHz 时线性重采样到 `pcm::kSampleRate`；不足 480 样点时 `read_frame` 返回 false

## TDD evidence

### RED

CMake 引用尚未存在的 `src/wasapi_capture.cpp`：

```text
CMake Error at engine/CMakeLists.txt:1 (add_library):
  Cannot find source file:
    src/wasapi_capture.cpp
```

### GREEN

```text
ctest --test-dir build -C Debug -R "WasapiCapture|ShmWriter|EnginePolicy" --output-on-failure
100% tests passed, 0 tests failed out of 4
```

`WasapiCapture.ReadSomeFramesOrSkip` **Passed**（本机有默认麦，未 SKIP）。

CMake via VS 2022 bundled path (not on PATH).

## Commit

```text
feat: WASAPI 麦克风采集并规范为 48k mono 帧
```

Branch: `feat/phase1-passthrough` (not pushed).

## Concerns

1. **线性重采样** — 质量足够直通原型；非整数倍采样率可能有轻微失真。
2. **COM 初始化** — `RPC_E_CHANGED_MODE` 时沿用线程已有模式且不 `CoUninitialize`；多线程/宿主集成需注意。
3. **无独立 mmdevapi.lib** — 通过 COM + `ole32`/`uuid` 链接即可；未再链 `mmdevapi`。

## Review fix (Task 6 finding)

### Change

- Bug: when `drop >= src_mono_.size()` during downsample, code kept the last sample and reset `resample_pos_ = 0.0`, losing fractional phase and duplicating the boundary sample across packets.
- Fix: extract `virtmic::resample::AdvanceSlidingWindow` / `LinearResampleToRate` in `engine/include/virtmic/resample_window.hpp`; on full consume, **clear** the buffer and `pos -= (double)size` (preserve fractional phase).
- `wasapi_capture.cpp` now calls the helper instead of inline window logic.
- Added `engine/tests/test_resample_window.cpp`: fractional-phase boundary, partial drop, and chunked **96k→48k** synthetic mono proving no duplicate at packet boundary.

### Re-run

```text
cmake --build build --config Debug
ctest --test-dir build -C Debug -R "ResampleWindow|WasapiCapture|ShmWriter|EnginePolicy" --output-on-failure
```

Result:

```text
Test project D:/Workspace/AI/virtmic/build
    Start  7: EnginePolicy.StartPassthroughThenTimeoutBlocks
1/7 Test  #7: EnginePolicy.StartPassthroughThenTimeoutBlocks .........   Passed
    Start  8: EnginePolicy.BypassIsOnlyOriginalPathWhileRunning
2/7 Test  #8: EnginePolicy.BypassIsOnlyOriginalPathWhileRunning ......   Passed
    Start  9: ShmWriter.WriteFrameThenSilenceRoundTrip
3/7 Test  #9: ShmWriter.WriteFrameThenSilenceRoundTrip ...............   Passed
    Start 10: WasapiCapture.ReadSomeFramesOrSkip
4/7 Test #10: WasapiCapture.ReadSomeFramesOrSkip .....................   Passed
    Start 11: ResampleWindow.FullyConsumedPreservesFractionalPhase
5/7 Test #11: ResampleWindow.FullyConsumedPreservesFractionalPhase ...   Passed
    Start 12: ResampleWindow.PartialDropKeepsTail
6/7 Test #12: ResampleWindow.PartialDropKeepsTail ....................   Passed
    Start 13: ResampleWindow.Chunked96kTo48kNoDuplicateAtBoundary
7/7 Test #13: ResampleWindow.Chunked96kTo48kNoDuplicateAtBoundary ....   Passed

100% tests passed, 0 tests failed out of 7
```
