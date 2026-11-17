# virtmic Phase 1：通路 MVP（直通 / 阻断 / 虚拟麦）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 上打通「真实麦 → 引擎 → VirtMic 虚拟麦」，支持手动 bypass（原声）与阻断（静音），尚不接入 RVC。

**Architecture:** C++ 共享库定义 PCM/IPC 约定；引擎用 WASAPI 采集，经环形缓冲与状态机写入与驱动共享的 PCM 区；WDK 虚拟麦驱动把该 PCM 暴露为系统麦克风「VirtMic」；本阶段用简易 CLI 控制启停 / bypass / 模拟超时阻断。

**Tech Stack:** CMake 3.20+、C++20、GoogleTest、WASAPI（`mmdeviceapi`）、Windows Driver Kit (WDK) + 基于 Microsoft Virtual Audio Device / SYSVAD 思路的虚拟捕获设备、命名共享内存、控制用命名管道 + 定长二进制消息。

## Global Constraints

- 平台：Windows 10/11 x64
- 变声路径采样率：48 kHz；声道：mono；进程间 PCM：小端 `float32`
- 帧时长：固定 **10 ms** → 每帧 **480** 样本 → **1920** 字节
- 推理超时（本阶段用 CLI「模拟超时」验证）：向 VirtMic **阻断为静音**，发出 `InferTimeout`；**不得**自动泄原声
- 唯一允许原声出口：用户显式 **Bypass**
- 引擎优先 C++；驱动 C++/WDK；第一版驱动可用测试签名
- 本阶段不做：RVC、Tauri UI、云端、EV 正式签名

## 后续计划（本文件不实现）

| 计划 | 内容 |
|------|------|
| Phase 2 | `infer/` RVC + CUDA，引擎接真推理与超时 |
| Phase 3 | `app/` Tauri 托盘 + 高级设置窗 |

---

## 文件结构（本阶段将创建）

```
virtmic/
  CMakeLists.txt
  shared/
    include/virtmic/pcm.hpp
    include/virtmic/ring_buffer.hpp
    include/virtmic/protocol.hpp
    include/virtmic/shm_layout.hpp
    src/ring_buffer.cpp          # 若头文件全 inline 可省略
    tests/test_ring_buffer.cpp
    tests/test_protocol.cpp
    tests/test_state_machine.cpp
  engine/
    include/virtmic/engine.hpp
    include/virtmic/wasapi_capture.hpp
    include/virtmic/shm_writer.hpp
    include/virtmic/control_server.hpp
    src/engine.cpp
    src/wasapi_capture.cpp
    src/shm_writer.cpp
    src/control_server.cpp
    src/main_cli.cpp
    tests/test_engine_policy.cpp
  driver/
    README.md                    # 构建 / 测试签名 / 安装步骤
    virtmic_adapter/             # 用户态适配或与引擎约定的接口说明
    # 内核驱动工程由 WDK 生成；计划中给出必须实现的 IO 与设备名约定
  tools/
    dump_shm_pcm.cpp             # 可选：从 SHM 拉 PCM 验通路
  docs/superpowers/plans/        # 本文件
```

---

### Task 1: 仓库骨架 + PCM 常量

**Files:**
- Create: `CMakeLists.txt`
- Create: `shared/include/virtmic/pcm.hpp`
- Create: `shared/CMakeLists.txt`
- Create: `shared/tests/test_pcm_constants.cpp`
- Create: `.gitignore`

**Interfaces:**
- Consumes: 无
- Produces: `virtmic::pcm` 命名空间内编译期常量（见下方代码）

- [ ] **Step 1: 写失败测试**

```cpp
// shared/tests/test_pcm_constants.cpp
#include <gtest/gtest.h>
#include "virtmic/pcm.hpp"

TEST(PcmConstants, FrameGeometry) {
  EXPECT_EQ(virtmic::pcm::kSampleRate, 48000);
  EXPECT_EQ(virtmic::pcm::kChannels, 1);
  EXPECT_EQ(virtmic::pcm::kFrameMs, 10);
  EXPECT_EQ(virtmic::pcm::kSamplesPerFrame, 480);
  EXPECT_EQ(virtmic::pcm::kBytesPerFrame, 480 * sizeof(float));
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake -S . -B build -DVIRTMIC_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 配置或链接失败（缺少 `virtmic/pcm.hpp`）或编译失败。

- [ ] **Step 3: 最小实现**

`.gitignore`:

```
build/
out/
.vs/
*.user
*.sdf
*.opendb
```

`shared/include/virtmic/pcm.hpp`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace virtmic::pcm {

inline constexpr int kSampleRate = 48000;
inline constexpr int kChannels = 1;
inline constexpr int kFrameMs = 10;
inline constexpr int kSamplesPerFrame = kSampleRate * kFrameMs / 1000; // 480
inline constexpr std::size_t kBytesPerFrame =
    static_cast<std::size_t>(kSamplesPerFrame) * sizeof(float);

}  // namespace virtmic::pcm
```

根 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(virtmic LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(VIRTMIC_BUILD_TESTS "Build unit tests" ON)
add_subdirectory(shared)
if(VIRTMIC_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
```

`shared/CMakeLists.txt`：

```cmake
add_library(virtmic_shared INTERFACE)
target_include_directories(virtmic_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(VIRTMIC_BUILD_TESTS)
  add_executable(shared_tests tests/test_pcm_constants.cpp)
  target_link_libraries(shared_tests PRIVATE virtmic_shared GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(shared_tests)
endif()
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake -S . -B build -DVIRTMIC_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `PcmConstants.FrameGeometry` PASS

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt shared .gitignore
git commit -m "feat: 添加 shared PCM 常量与 CMake 骨架"
```

---

### Task 2: 环形缓冲

**Files:**
- Create: `shared/include/virtmic/ring_buffer.hpp`
- Create: `shared/tests/test_ring_buffer.cpp`
- Modify: `shared/CMakeLists.txt`（把新测试加入 `shared_tests` 或单独 target）

**Interfaces:**
- Consumes: `virtmic::pcm::kSamplesPerFrame`
- Produces: `virtmic::RingBuffer` — `bool push(const float* frame)`、`bool pop(float* frame)`、`size_t size_frames() const`、容量以帧计

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "virtmic/ring_buffer.hpp"
#include "virtmic/pcm.hpp"

TEST(RingBuffer, PushPopOneFrame) {
  virtmic::RingBuffer rb(/*capacity_frames=*/4);
  std::vector<float> in(virtmic::pcm::kSamplesPerFrame, 0.5f);
  std::vector<float> out(virtmic::pcm::kSamplesPerFrame, 0.f);
  ASSERT_TRUE(rb.push(in.data()));
  ASSERT_TRUE(rb.pop(out.data()));
  EXPECT_FLOAT_EQ(out[0], 0.5f);
  EXPECT_EQ(rb.size_frames(), 0u);
}

TEST(RingBuffer, RejectWhenFull) {
  virtmic::RingBuffer rb(2);
  std::vector<float> in(virtmic::pcm::kSamplesPerFrame, 1.f);
  EXPECT_TRUE(rb.push(in.data()));
  EXPECT_TRUE(rb.push(in.data()));
  EXPECT_FALSE(rb.push(in.data()));
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug -R RingBuffer --output-on-failure
```

Expected: 编译失败（无 `RingBuffer`）

- [ ] **Step 3: 最小实现**

```cpp
#pragma once
#include "virtmic/pcm.hpp"
#include <atomic>
#include <cstring>
#include <vector>

namespace virtmic {

class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity_frames)
      : cap_(capacity_frames),
        data_(capacity_frames * pcm::kSamplesPerFrame) {}

  bool push(const float* frame) {
    if (size_frames_ >= cap_) return false;
    const std::size_t off = (head_ % cap_) * pcm::kSamplesPerFrame;
    std::memcpy(data_.data() + off, frame,
                pcm::kSamplesPerFrame * sizeof(float));
    head_ = (head_ + 1) % (cap_ * 2);  // 简单单线程版；引擎音频线程单写单读即可
    ++size_frames_;
    return true;
  }

  bool pop(float* frame) {
    if (size_frames_ == 0) return false;
    const std::size_t tail = (head_ + cap_ - size_frames_) % cap_;
    const std::size_t off = tail * pcm::kSamplesPerFrame;
    std::memcpy(frame, data_.data() + off,
                pcm::kSamplesPerFrame * sizeof(float));
    --size_frames_;
    return true;
  }

  std::size_t size_frames() const { return size_frames_; }

 private:
  std::size_t cap_;
  std::vector<float> data_;
  std::size_t head_ = 0;
  std::size_t size_frames_ = 0;
};

}  // namespace virtmic
```

> 实现时若单测暴露索引错误，修正 `tail` 计算；保持「满则 push 失败、空则 pop 失败」。音频回调与处理若分线程，Task 后续改为带 mutex 或无锁 SPSC；本任务先保证单线程语义正确。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug -R RingBuffer --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add shared/include/virtmic/ring_buffer.hpp shared/tests/test_ring_buffer.cpp shared/CMakeLists.txt
git commit -m "feat: 添加按帧环形缓冲"
```

---

### Task 3: 控制协议（命令 / 事件）

**Files:**
- Create: `shared/include/virtmic/protocol.hpp`
- Create: `shared/tests/test_protocol.cpp`

**Interfaces:**
- Consumes: 无
- Produces:
  - `enum class Command : uint32_t { Start=1, Stop=2, SetBypass=3, SimulateInferTimeout=4 }`
  - `enum class EventType : uint32_t { Started=1, Stopped=2, InferTimeout=3, Error=4, Latency=5 }`
  - `struct MessageHeader { uint32_t magic; uint32_t type; uint32_t nbytes; }` — `magic = 0x564D4943`（`VMIC`）
  - `struct BypassPayload { uint32_t enabled; }` — 0/1
  - `Serialize/Deserialize` 辅助函数（定长或 header+payload）

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "virtmic/protocol.hpp"

TEST(Protocol, RoundTripBypass) {
  virtmic::BypassPayload p{1};
  auto bytes = virtmic::encode_command(virtmic::Command::SetBypass, p);
  auto decoded = virtmic::decode_command(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->cmd, virtmic::Command::SetBypass);
  EXPECT_EQ(decoded->bypass.enabled, 1u);
}

TEST(Protocol, InferTimeoutEvent) {
  auto bytes = virtmic::encode_event(virtmic::EventType::InferTimeout, {});
  auto ev = virtmic::decode_event(bytes);
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->type, virtmic::EventType::InferTimeout);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败

- [ ] **Step 3: 最小实现**

在 `protocol.hpp` 中实现上述类型与编解码（payload 不足时返回 `std::nullopt`；`magic` 不匹配返回空）。`SetBypass` 必须带 4 字节 payload；`SimulateInferTimeout` / `Start` / `Stop` 无 payload。

- [ ] **Step 4: 跑测试确认通过**

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add shared/include/virtmic/protocol.hpp shared/tests/test_protocol.cpp shared/CMakeLists.txt
git commit -m "feat: 添加引擎控制命令与事件协议"
```

---

### Task 4: 引擎策略状态机（bypass / 阻断 / 直通）

**Files:**
- Create: `engine/include/virtmic/engine_policy.hpp`
- Create: `engine/src/engine_policy.cpp`
- Create: `engine/tests/test_engine_policy.cpp`
- Create: `engine/CMakeLists.txt`
- Modify: 根 `CMakeLists.txt` — `add_subdirectory(engine)`

**Interfaces:**
- Consumes: `pcm.hpp`、`protocol` 语义
- Produces: `class EnginePolicy`
  - `void set_bypass(bool)`
  - `void notify_infer_timeout()` → 进入 blocked，且 `consume_events()` 能取出 `InferTimeout`
  - `void clear_block_for_test()` 或 `retry()` → 清除阻断（模拟用户重试）
  - `enum class OutputMode { Silence, Passthrough }` `OutputMode decide(bool conversion_wanted)`  
    - bypass=true → Passthrough（原声）
    - blocked=true → Silence
    - 本阶段无真推理：conversion_wanted=false 且未 bypass → Silence（未启动变声时也静音；若 CLI Start 且未 bypass，Phase1 定义为 **Passthrough 原声直通** 以便验收通路）
  - **Phase 1 明确规则：**
    1. `Stop` 或未 `Start` → 输出 Silence
    2. `Start` + bypass → Passthrough
    3. `Start` + 非 bypass → Passthrough（本阶段用原声占位，为 Phase2 留 `process_frame` 钩子）
    4. 任意时刻 `notify_infer_timeout()` → Silence + 事件；需 `retry()` 或重新 `Start` 才恢复

- [ ] **Step 1: 写失败测试**

```cpp
TEST(EnginePolicy, StartPassthroughThenTimeoutBlocks) {
  virtmic::EnginePolicy p;
  p.start();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
  p.notify_infer_timeout();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  auto ev = p.poll_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(*ev, virtmic::EventType::InferTimeout);
}

TEST(EnginePolicy, BypassIsOnlyOriginalPathWhileRunning) {
  virtmic::EnginePolicy p;
  p.start();
  p.notify_infer_timeout();
  p.set_bypass(true);
  // 超时阻断优先于 bypass，避免「超时却泄原声」
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  p.retry();
  p.set_bypass(true);
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败

- [ ] **Step 3: 最小实现**

实现 `EnginePolicy`，严格满足：**InferTimeout 阻断优先于 bypass**（与规格「超时不泄原声」一致；bypass 仅在未阻断时生效）。

- [ ] **Step 4: 跑测试确认通过**

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add engine CMakeLists.txt
git commit -m "feat: 引擎输出策略（直通/阻断，超时优先于 bypass）"
```

---

### Task 5: 共享内存 PCM 布局 + 写入端

**Files:**
- Create: `shared/include/virtmic/shm_layout.hpp`
- Create: `engine/include/virtmic/shm_writer.hpp`
- Create: `engine/src/shm_writer.cpp`
- Create: `engine/tests/test_shm_writer.cpp`

**Interfaces:**
- Consumes: `pcm::kBytesPerFrame`
- Produces:
  - 共享内存名：`Local\VirtMicPcmV1`
  - 布局：
    ```cpp
    struct ShmHeader {
      uint32_t magic;          // 0x564D4943
      uint32_t version;        // 1
      uint32_t write_index;    // 单调递增帧序号
      uint32_t frames_capacity;// 例如 32
      uint32_t sample_rate;    // 48000
      uint32_t channels;       // 1
      uint32_t samples_frame;  // 480
      uint32_t flags;          // bit0=underrun
    };
    // 随后 frames_capacity * kBytesPerFrame 的 float PCM
    ```
  - `ShmWriter::create()` / `write_frame(const float*)` / `write_silence_frame()`

- [ ] **Step 1: 写失败测试**

用同一进程 `CreateFileMapping` + `ShmWriter` 写一帧非零，再 map 只读端读回校验；再 `write_silence_frame` 校验全 0。

- [ ] **Step 2: 跑测试确认失败**

- [ ] **Step 3: 最小实现**

Windows API：`CreateFileMappingW` / `MapViewOfFile`。容量默认 32 帧。

- [ ] **Step 4: 跑测试确认通过**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat: VirtMic PCM 共享内存写入端"
```

---

### Task 6: WASAPI 采集

**Files:**
- Create: `engine/include/virtmic/wasapi_capture.hpp`
- Create: `engine/src/wasapi_capture.cpp`
- Create: `engine/tests/test_wasapi_capture_smoke.cpp`（标记为可选 / 手动，无麦环境可 SKIP）

**Interfaces:**
- Consumes: `pcm` 常量
- Produces: `WasapiCapture`
  - `bool start(std::string* err)`
  - `void stop()`
  - `bool read_frame(float* mono_out /*480*/)` — 内部重采样/downmix 到 48 kHz mono；不足一帧返回 false

- [ ] **Step 1: 写失败测试（有麦）**

```cpp
TEST(WasapiCapture, ReadSomeFramesOrSkip) {
  virtmic::WasapiCapture cap;
  std::string err;
  if (!cap.start(&err)) {
    GTEST_SKIP() << err;
  }
  std::vector<float> frame(480);
  int got = 0;
  for (int i = 0; i < 200 && got < 5; ++i) {
    if (cap.read_frame(frame.data())) ++got;
    Sleep(5);
  }
  cap.stop();
  EXPECT_GE(got, 1);
}
```

- [ ] **Step 2: 跑测试（允许 SKIP）**

- [ ] **Step 3: 实现 WASAPI shared-mode capture**

使用 `IMMDeviceEnumerator`、`IAudioClient`、`IAudioCaptureClient`；若设备不是 48 kHz，用简单线性重采样到 48 kHz；立体声取均值变 mono。

- [ ] **Step 4: 有麦机器上确认能读到帧**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat: WASAPI 麦克风采集并规范为 48k mono 帧"
```

---

### Task 7: 引擎主循环 + 控制管道 + CLI

**Files:**
- Create: `engine/include/virtmic/engine.hpp`
- Create: `engine/src/engine.cpp`
- Create: `engine/src/control_server.cpp`
- Create: `engine/include/virtmic/control_server.hpp`
- Create: `engine/src/main_cli.cpp`

**Interfaces:**
- Consumes: `EnginePolicy`、`WasapiCapture`、`ShmWriter`、`protocol`
- Produces:
  - 命名管道：`\\.\pipe\VirtMicControl`
  - CLI 子命令：`virtmic-engine serve`（前台跑引擎）、或同进程内建管道服务
  - 行为：`Start` 后按 `decide()` 每 10ms 写 SHM（Passthrough=麦帧，Silence=零）

- [ ] **Step 1: 写集成测试（可同进程）**

启动 `Engine`（可用假 capture：正弦波注入接口 `IFrameSource`，便于无麦 CI）：

```cpp
struct SineSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < 480; ++i) out[i] = 0.1f;
    return true;
  }
};
```

断言：Start 后 SHM 非静音；`SimulateInferTimeout` 后若干帧为 0；`retry`+`SetBypass` 后恢复非 0。

- [ ] **Step 2: 跑测试确认失败**

- [ ] **Step 3: 实现 Engine + ControlServer + CLI**

`main_cli.cpp` 最小用法：

```text
virtmic-engine.exe
  # 启动采集 + SHM + 控制管道；日志打印事件
```

另附 `tools/virtmic_ctl.cpp`（可同 Task）：发送 `Start` / `Stop` / `SetBypass` / `SimulateInferTimeout`。

- [ ] **Step 4: 本地手动验证**

1. 运行 `virtmic-engine.exe`
2. `virtmic_ctl start` → dump 工具见非静音
3. `virtmic_ctl timeout` → 变为静音，引擎日志 `InferTimeout`
4. `virtmic_ctl bypass 1` 在未 retry 前仍应静音
5. `virtmic_ctl retry` + `bypass 1` → 原声/正弦恢复

- [ ] **Step 5: Commit**

```bash
git commit -m "feat: 引擎主循环、控制管道与 CLI"
```

---

### Task 8: VirtMic 虚拟麦克风驱动（WDK）

**Files:**
- Create: `driver/README.md`
- Create: `driver/` 下 WDK 工程（具体文件名随选用的 Microsoft Virtual Audio Device Sample / SYSVAD 裁剪结果而定；实现者须在 README 列出最终文件清单）

**Interfaces:**
- Consumes: `ShmLayout`（`Local\VirtMicPcmV1`）
- Produces:
  - 系统声音设置中可见的捕获设备，友好名 **`VirtMic`**
  - 设备向 OS 提供 48 kHz / mono（或 stereo 复制同一 mono）PCM
  - 从 SHM 最新帧取数；欠载时输出静音并置 `flags.underrun`

**实现约束：**

1. 基于官方虚拟音频驱动样例裁剪，**只保留捕获（microphone）路径**，去掉无关拓扑。
2. 用户态与内核态之间：优先「引擎写 SHM、驱动（或伴随的用户态辅助服务）读 SHM」。若样例更适合 IOCTL 推流，则增加 `engine/src/driver_client.cpp` 用 IOCTL 推帧，但 **PCM 几何仍必须与 `pcm.hpp` 一致**，并更新 README。
3. 安装：测试签名（`bcdedit` 测试模式或 CI 外文档说明）；`driver/README.md` 写清：
   - 所需 WDK 版本
   - 生成 / 签名 / `devcon` 或 `pnputil` 安装命令
   - 卸载命令
   - 验证：声音设置出现「VirtMic」

- [ ] **Step 1: 文档化选型并搭空工程**

在 `driver/README.md` 写明采用的样例来源与裁剪范围；空工程能编译出 `.sys` / `.inf`（即使尚无 SHM）。

- [ ] **Step 2: 实现从 SHM（或 IOCTL）取帧并填满捕获缓冲**

- [ ] **Step 3: 安装到开发机（测试签名）**

Expected: 声音设置 → 输入 → **VirtMic**

- [ ] **Step 4: 端到端手测**

1. 安装驱动  
2. 启动 `virtmic-engine` + `ctl start`  
3. Windows「聆听此设备」或 Discord 输入设备选 VirtMic  
4. bypass / timeout 行为与 Task 7 一致  

- [ ] **Step 5: Commit**

```bash
git add driver
git commit -m "feat: VirtMic 虚拟麦克风驱动（测试签名通路）"
```

---

### Task 9: Phase 1 验收清单与 README

**Files:**
- Create: `README.md`
- Modify: `docs/superpowers/specs/2026-11-17-virtmic-design.md`（状态改为 Phase1 计划已就绪 / 或保持不动，仅 README 指向计划）

- [ ] **Step 1: 写根 README（中文）**

内容必须包括：项目目标一句话、架构四块、Phase1 范围、构建引擎测试步骤、驱动安装链接到 `driver/README.md`、如何用 CLI 验证 timeout 阻断、指向 Phase2/3 计划占位。

- [ ] **Step 2: 按验收清单自测并勾选**

- [ ] 单元测试：`ctest` 全绿  
- [ ] SHM 在 Start 时有能量、Timeout 后近零  
- [ ] 系统中出现 VirtMic  
- [ ] 超时不泄原声（阻断优先于 bypass）  
- [ ] 手动 bypass 在 retry 后可原声直通  

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: Phase1 验收说明与使用入口"
```

---

## Plan Self-Review

1. **Spec coverage（对本阶段）：**  
   - 虚拟麦自带驱动 → Task 8  
   - 真实麦采集 → Task 6  
   - bypass 唯一原声出口、超时阻断不泄原声 → Task 4/7  
   - 48k mono float32、10ms 帧 → Task 1/5  
   - 托盘/RVC → 明确留给 Phase 2/3，不在本计划  

2. **Placeholder scan：** 驱动目录具体文件名依赖 WDK 样例裁剪，已强制要求写入 `driver/README.md` 最终清单，避免实现时无锚点。  

3. **Type consistency：** `Command` / `EventType` / `OutputMode` / SHM 名 `Local\VirtMicPcmV1` / 管道 `\\.\pipe\VirtMicControl` 全文统一；超时优先于 bypass 在 Task 4 测试中锁定。
