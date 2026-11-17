# virtmic Phase 2：可插拔推理（假变调）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 打通 engine ↔ Python 推理的双 SHM + 控制管，假变调可听、半音热改、推理挂则静音；驱动/feeder 不动。

**Architecture:** engine 采麦后写入 `Local\VirtMicInferInV1`；Python `virtmic-infer` 读入、时域变调、写入 `Local\VirtMicInferOutV1`；engine 在默认 80 ms 内读到新 out 帧则写入既有 `Local\VirtMicPcmV1`，否则静音并 `InferTimeout`。对外仍只暴露 `\\.\pipe\VirtMicControl`；`SetPitchSemitones` 由 engine 转发到 `\\.\pipe\VirtMicInferControl`。

**Tech Stack:** 既有 CMake/C++20/GTest/WASAPI；Python 3.10+（标准库 + 可选 numpy；假变调优先纯 Python 浮点）；Windows 命名共享内存与命名管道。

## Global Constraints

- 平台：Windows 10/11 x64
- PCM：48 kHz / mono / float32 / 10 ms / 480 samples / 1920 bytes（与 Phase 1 相同）
- 推理不可用或超时 → VirtMic **静音**；**不得**自动泄原声
- 唯一原声出口：用户显式 **Bypass**
- 假变调额外延迟目标约 +20–40 ms；超时默认 **80 ms**
- 半音运行中可热改；建议钳制 **-12…+12**
- 本阶段不做：真 RVC/CUDA、Tauri、改驱动/feeder
- 推理进程不得写 `Local\VirtMicPcmV1`

---

## 文件结构（本阶段）

```
virtmic/
  shared/include/virtmic/
    protocol.hpp              # 改：SetPitchSemitones + PitchPayload
    shm_layout.hpp            # 改：导出通用 ShmHeader；infer 环常量可同文件或新头
    infer_shm.hpp             # 新建：InferIn/Out 环名与 magic
  shared/tests/
    test_protocol.cpp         # 改：半音编解码
    test_infer_shm.cpp        # 新建：infer 环常量
  engine/include/virtmic/
    engine_policy.hpp         # 改：OutputMode::Processed
    infer_client.hpp          # 新建：写 in / 读 out / 超时判定
    infer_control_client.hpp  # 新建：转发半音到 InferControl 管
    engine.hpp                # 改：set_pitch + InferClient
    control_server.hpp        # 改：处理 SetPitchSemitones
  engine/src/
    engine_policy.cpp
    infer_client.cpp
    infer_control_client.cpp
    engine.cpp
    control_server.cpp
    shm_writer.cpp            # 可选小改：若抽出通用 create_named 辅助则用
  engine/tests/
    test_engine_policy.cpp    # 改：Processed 路径
    test_infer_client.cpp     # 新建
  tools/virtmic_ctl.cpp       # 改：pitch <n>
  infer/
    README.md
    requirements.txt          # 可空或仅注明 py>=3.10
    virtmic_infer/
      __init__.py
      shm.py                  # OpenFileMapping / 读写帧
      pitch.py                # 假变调
      control.py              # InferControl 管服务
      __main__.py             # 入口：python -m virtmic_infer
  scripts/start-virtmic.ps1   # 改：拉起 Python infer
  README.md                   # 改：Phase 2 用法
```

---

### Task 1: 协议扩展 `SetPitchSemitones`

**Files:**
- Modify: `shared/include/virtmic/protocol.hpp`
- Modify: `shared/tests/test_protocol.cpp`

**Interfaces:**
- Consumes: 现有 `encode_command` / `decode_command` 模式
- Produces:
  - `enum class Command { …, SetPitchSemitones = 6 }`
  - `struct PitchPayload { int32_t semitones; }`
  - `DecodedCommand` 增加 `PitchPayload pitch{}`
  - `encode_command(Command::SetPitchSemitones, PitchPayload)`
  - `decode_command` 解析 `nbytes == 4` 的 int32

- [ ] **Step 1: 写失败测试**

在 `shared/tests/test_protocol.cpp` 追加：

```cpp
TEST(Protocol, SetPitchSemitonesRoundTrip) {
  virtmic::PitchPayload p{-4};
  auto bytes = virtmic::encode_command(virtmic::Command::SetPitchSemitones, p);
  auto decoded = virtmic::decode_command(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->cmd, virtmic::Command::SetPitchSemitones);
  EXPECT_EQ(decoded->pitch.semitones, -4);
}

TEST(Protocol, SetPitchRejectsBadPayloadSize) {
  // 手工组一个 nbytes=0 的 SetPitchSemitones
  std::vector<uint8_t> bad;
  auto append = [&](uint32_t v) {
    const auto off = bad.size();
    bad.resize(off + 4);
    std::memcpy(bad.data() + off, &v, 4);
  };
  append(virtmic::kProtocolMagic);
  append(static_cast<uint32_t>(virtmic::Command::SetPitchSemitones));
  append(0);
  EXPECT_FALSE(virtmic::decode_command(bad).has_value());
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake -S . -B build -DVIRTMIC_BUILD_TESTS=ON
cmake --build build --config Debug --target shared_tests
ctest --test-dir build -C Debug -R Protocol.SetPitch --output-on-failure
```

Expected: 编译失败或测试失败（缺 `SetPitchSemitones` / `PitchPayload`）。

- [ ] **Step 3: 最小实现**

在 `protocol.hpp`：

1. `Command` 增加 `SetPitchSemitones = 6`
2. 增加：

```cpp
struct PitchPayload {
  int32_t semitones;
};
```

3. `DecodedCommand` 增加 `PitchPayload pitch{}`
4. 增加 `encode_command(Command cmd, PitchPayload payload)`：`nbytes = 4`，追加 `semitones` 的小端 int32（可用 `append_u32` 对 bit 模式，或新增 `append_i32`）
5. `decode_command`：若 `cmd == SetPitchSemitones`，要求 `nbytes == 4`，读 int32 到 `decoded.pitch.semitones`；其它带 payload 命令保持原逻辑；无 payload 命令仍要求 `nbytes == 0`

`append_i32` 示例：

```cpp
inline void append_i32(std::vector<uint8_t>& out, int32_t v) {
  const std::size_t off = out.size();
  out.resize(off + sizeof(int32_t));
  std::memcpy(out.data() + off, &v, sizeof(int32_t));
}
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build --config Debug --target shared_tests
ctest --test-dir build -C Debug -R Protocol --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add shared/include/virtmic/protocol.hpp shared/tests/test_protocol.cpp
git commit -m "feat(shared): add SetPitchSemitones control protocol"
```

---

### Task 2: Infer SHM 常量

**Files:**
- Create: `shared/include/virtmic/infer_shm.hpp`
- Create: `shared/tests/test_infer_shm.cpp`
- Modify: `shared/CMakeLists.txt`（把 `test_infer_shm.cpp` 加入 `shared_tests`）

**Interfaces:**
- Consumes: `virtmic/shm_layout.hpp` 的 `ShmHeader` / `mapping_bytes`
- Produces: `virtmic::infer_shm` 命名空间常量

- [ ] **Step 1: 写失败测试**

```cpp
// shared/tests/test_infer_shm.cpp
#include <gtest/gtest.h>
#include <cstring>
#include "virtmic/infer_shm.hpp"
#include "virtmic/shm_layout.hpp"

TEST(InferShm, NamesAndGeometry) {
  EXPECT_TRUE(std::wcscmp(virtmic::infer_shm::kInName, L"Local\\VirtMicInferInV1") == 0);
  EXPECT_TRUE(std::wcscmp(virtmic::infer_shm::kOutName, L"Local\\VirtMicInferOutV1") == 0);
  EXPECT_NE(virtmic::infer_shm::kInMagic, virtmic::shm::kMagic);
  EXPECT_NE(virtmic::infer_shm::kOutMagic, virtmic::shm::kMagic);
  EXPECT_NE(virtmic::infer_shm::kInMagic, virtmic::infer_shm::kOutMagic);
  EXPECT_EQ(virtmic::infer_shm::kVersion, 1u);
  EXPECT_EQ(virtmic::shm::mapping_bytes(32),
            sizeof(virtmic::shm::ShmHeader) + 32 * virtmic::pcm::kBytesPerFrame);
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --config Debug --target shared_tests
```

Expected: 找不到 `infer_shm.hpp`。

- [ ] **Step 3: 最小实现**

```cpp
// shared/include/virtmic/infer_shm.hpp
#pragma once
#include <cstdint>
#include "virtmic/shm_layout.hpp"

namespace virtmic::infer_shm {

inline constexpr wchar_t kInName[] = L"Local\\VirtMicInferInV1";
inline constexpr wchar_t kOutName[] = L"Local\\VirtMicInferOutV1";
inline constexpr uint32_t kInMagic = 0x564D494E;   // 'VMIN'
inline constexpr uint32_t kOutMagic = 0x564D4F55;  // 'VMOU'
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kDefaultFramesCapacity = virtmic::shm::kDefaultFramesCapacity;

}  // namespace virtmic::infer_shm
```

并在 `shared/CMakeLists.txt` 的 `shared_tests` 源列表加入 `tests/test_infer_shm.cpp`。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build --config Debug --target shared_tests
ctest --test-dir build -C Debug -R InferShm --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add shared/include/virtmic/infer_shm.hpp shared/tests/test_infer_shm.cpp shared/CMakeLists.txt
git commit -m "feat(shared): add infer in/out SHM name and magic constants"
```

---

### Task 3: `EnginePolicy` 增加 `Processed`

**Files:**
- Modify: `engine/include/virtmic/engine_policy.hpp`
- Modify: `engine/src/engine_policy.cpp`
- Modify: `engine/tests/test_engine_policy.cpp`

**Interfaces:**
- Consumes: 现有 `start/stop/set_bypass/notify_infer_timeout/retry`
- Produces: `enum class OutputMode { Silence, Passthrough, Processed };`
  - 运行中、未阻断、未 bypass → `Processed`
  - 运行中、未阻断、bypass → `Passthrough`
  - 阻断或未运行 → `Silence`（阻断仍优先于 bypass）

- [ ] **Step 1: 写失败测试**

替换/追加 `test_engine_policy.cpp` 核心断言：

```cpp
TEST(EnginePolicy, StartDefaultsToProcessed) {
  virtmic::EnginePolicy p;
  p.start();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Processed);
}

TEST(EnginePolicy, BypassPassthroughUnlessBlocked) {
  virtmic::EnginePolicy p;
  p.start();
  p.set_bypass(true);
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
  p.notify_infer_timeout();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  p.retry();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
}

TEST(EnginePolicy, TimeoutBlocksProcessed) {
  virtmic::EnginePolicy p;
  p.start();
  p.notify_infer_timeout();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  auto ev = p.poll_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(*ev, virtmic::EventType::InferTimeout);
}
```

删除或改写旧的 `StartPassthroughThenTimeoutBlocks`（Phase 1 默认 Passthrough 已过时）。

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --config Debug --target engine_tests
ctest --test-dir build -C Debug -R EnginePolicy --output-on-failure
```

Expected: FAIL（无 `Processed` 或 Start 仍为 Passthrough）。

- [ ] **Step 3: 最小实现**

`engine_policy.hpp`：

```cpp
enum class OutputMode { Silence, Passthrough, Processed };
```

`engine_policy.cpp` 中 `decide()`：

```cpp
OutputMode EnginePolicy::decide() const {
  if (!running_ || blocked_) return OutputMode::Silence;
  if (bypass_) return OutputMode::Passthrough;
  return OutputMode::Processed;
}
```

其余方法不变。

- [ ] **Step 4: 跑测试确认通过**

```bash
ctest --test-dir build -C Debug -R EnginePolicy --output-on-failure
```

Expected: PASS。若 `test_engine_loop` 依赖旧 Passthrough 语义，同步改期望为：无 bypass 时走 Processed（本 Task 若失败先改 loop 测试里对 `decide()` 的断言；真正接 InferClient 在 Task 5）。

- [ ] **Step 5: Commit**

```bash
git add engine/include/virtmic/engine_policy.hpp engine/src/engine_policy.cpp engine/tests/test_engine_policy.cpp
git commit -m "feat(engine): default output mode Processed for infer path"
```

---

### Task 4: `InferClient`（双环写读 + 超时）

**Files:**
- Create: `engine/include/virtmic/infer_client.hpp`
- Create: `engine/src/infer_client.cpp`
- Create: `engine/tests/test_infer_client.cpp`
- Modify: `engine/CMakeLists.txt`（库源 + `engine_tests`）

**Interfaces:**
- Consumes: `infer_shm.hpp`、`shm_layout.hpp`、`pcm.hpp`
- Produces:

```cpp
namespace virtmic {
class InferClient {
 public:
  static constexpr int kDefaultTimeoutMs = 80;

  InferClient(const InferClient&) = delete;
  InferClient& operator=(const InferClient&) = delete;
  InferClient(InferClient&&) noexcept;
  InferClient& operator=(InferClient&&) noexcept;
  ~InferClient();

  // Creates both InferIn (engine writer) and InferOut (engine reader) mappings.
  static std::optional<InferClient> create(
      uint32_t frames_capacity = infer_shm::kDefaultFramesCapacity);

  // Writes one capture frame into InferIn; advances in write_index.
  bool push_input(const float* samples);

  // Tries to copy the latest InferOut frame newer than last_seen into out[480].
  // Returns true if a fresh frame was read within timeout_ms since push (caller
  // tracks time) OR if out_index advanced since last successful read.
  // For unit tests: use try_pop_latest(out, &seq) without wall clock.
  bool try_pop_latest(float* out, uint32_t* out_seq);

  uint32_t in_write_index() const;
  uint32_t out_write_index() const;

 private:
  InferClient() = default;
  void* in_mapping_ = nullptr;
  void* in_view_ = nullptr;
  void* out_mapping_ = nullptr;
  void* out_view_ = nullptr;
  uint32_t frames_capacity_ = 0;
  uint32_t last_out_seq_ = 0;
};
}
```

超时墙钟逻辑放在 `Engine::process_one_`（Task 5）：`push_input` 后若 `try_pop_latest` 在 80 ms 内一直失败则 `notify_infer_timeout`。本 Task 单测只验证环读写与 seq。

- [ ] **Step 1: 写失败测试**

```cpp
// engine/tests/test_infer_client.cpp
#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include "virtmic/infer_client.hpp"
#include "virtmic/pcm.hpp"

TEST(InferClient, PushThenManualOutPop) {
  auto client = virtmic::InferClient::create(8);
  ASSERT_TRUE(client.has_value());

  std::vector<float> in(virtmic::pcm::kSamplesPerFrame, 0.25f);
  ASSERT_TRUE(client->push_input(in.data()));
  EXPECT_GE(client->in_write_index(), 1u);

  // Simulate infer: open out mapping as writer is the same process — use test
  // helper or second InferClient view. Prefer InferClient::debug_write_out_for_test
  // if needed for same-process loopback.
  ASSERT_TRUE(client->debug_write_out_for_test(in.data()));
  std::vector<float> out(virtmic::pcm::kSamplesPerFrame, 0.f);
  uint32_t seq = 0;
  ASSERT_TRUE(client->try_pop_latest(out.data(), &seq));
  EXPECT_EQ(seq, 1u);
  EXPECT_FLOAT_EQ(out[0], 0.25f);
}

TEST(InferClient, TryPopFailsUntilOutAdvances) {
  auto client = virtmic::InferClient::create(8);
  ASSERT_TRUE(client.has_value());
  std::vector<float> out(virtmic::pcm::kSamplesPerFrame, 0.f);
  uint32_t seq = 0;
  EXPECT_FALSE(client->try_pop_latest(out.data(), &seq));
}
```

在头文件增加测试辅助（仅测试或 `#ifdef` 不必要；直接作为 public test hook 可接受于本阶段）：

```cpp
bool debug_write_out_for_test(const float* samples);
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --config Debug --target engine_tests
```

Expected: 缺 `infer_client.hpp`。

- [ ] **Step 3: 最小实现**

`infer_client.cpp` 要点：

1. `CreateFileMappingW` 创建 `kInName` / `kOutName`，初始化 `ShmHeader`（分别用 `kInMagic` / `kOutMagic`，`version=1`，geometry 与 Phase 1 相同）。
2. `push_input`：与 `ShmWriter::write_frame` 相同算法写 in 环（slot = write_index % capacity，memcpy 样本，然后 `write_index++` release）。
3. `debug_write_out_for_test`：同样算法写 out 环。
4. `try_pop_latest`：读 out 的 `write_index`；若 `== last_out_seq_` 返回 false；否则读最新已完成帧 `(write_index - 1) % capacity`，更新 `last_out_seq_ = write_index`，拷贝到 `out`。

复用 `ShmWriter` 的帧写入逻辑时可抽私有自由函数到 `infer_client.cpp` 匿名命名空间，**不要**改 feeder。

CMake：`virtmic_engine` 增加 `src/infer_client.cpp`；`engine_tests` 增加 `tests/test_infer_client.cpp`。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build --config Debug --target engine_tests
ctest --test-dir build -C Debug -R InferClient --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add engine/include/virtmic/infer_client.hpp engine/src/infer_client.cpp engine/tests/test_infer_client.cpp engine/CMakeLists.txt
git commit -m "feat(engine): add InferClient dual SHM ring IO"
```

---

### Task 5: Engine 热路径接入 InferClient

**Files:**
- Modify: `engine/include/virtmic/engine.hpp`
- Modify: `engine/src/engine.cpp`
- Modify: `engine/tests/test_engine_loop.cpp`（若断言输出模式/内容）
- Create 或扩展: `engine/tests/test_engine_infer_path.cpp`（推荐新建，假 source + 同进程 debug out）

**Interfaces:**
- Consumes: `InferClient`、`EnginePolicy::Processed`
- Produces:
  - `Engine::create` 同时创建 VirtMic `ShmWriter` + `InferClient`
  - `process_one_` 行为：
    - `Silence` → `writer_.write_silence_frame()`
    - `Passthrough` → 有源帧则 `writer_.write_frame(source)`，否则静音
    - `Processed` → `push_input(source)`；在最多 `InferClient::kDefaultTimeoutMs` 内自旋/`try_pop_latest`；成功则写 VirtMic；失败则 `policy_.notify_infer_timeout()` 并写静音
  - `void set_pitch(int32_t semitones);` 声明先放着，Task 6 实现转发

- [ ] **Step 1: 写失败测试**

```cpp
// engine/tests/test_engine_infer_path.cpp
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include "virtmic/engine.hpp"
#include "virtmic/pcm.hpp"

namespace {
struct ToneSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) out[i] = 0.5f;
    return true;
  }
};
}  // namespace

TEST(EngineInferPath, ProcessedTimesOutToSilenceWithoutInfer) {
  ToneSource src;
  auto engine = virtmic::Engine::create(src);
  ASSERT_NE(engine, nullptr);
  engine->start();
  // No Python infer → within ~100ms should block
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  auto ev = engine->poll_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(*ev, virtmic::EventType::InferTimeout);
  engine->stop();
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --config Debug --target engine_tests
ctest --test-dir build -C Debug -R EngineInferPath --output-on-failure
```

Expected: FAIL（仍直通或无超时事件）。

- [ ] **Step 3: 最小实现**

`engine.hpp` 增加成员 `std::optional<InferClient> infer_`（或必选，create 失败则 `Engine::create` 返回 nullptr）、`set_pitch` 声明。

`process_one_` 伪代码：

```cpp
void Engine::process_one_() {
  std::vector<float> frame(pcm::kSamplesPerFrame, 0.f);
  const bool got = source_ && source_->read_frame(frame.data());

  std::lock_guard<std::mutex> lock(mu_);
  const OutputMode mode = policy_.decide();
  if (mode == OutputMode::Silence) {
    writer_.write_silence_frame();
    return;
  }
  if (mode == OutputMode::Passthrough) {
    if (got) writer_.write_frame(frame.data());
    else writer_.write_silence_frame();
    return;
  }
  // Processed
  if (!got || !infer_) {
    writer_.write_silence_frame();
    return;
  }
  if (!infer_->push_input(frame.data())) {
    policy_.notify_infer_timeout();
    writer_.write_silence_frame();
    return;
  }
  std::vector<float> out(pcm::kSamplesPerFrame, 0.f);
  uint32_t seq = 0;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(InferClient::kDefaultTimeoutMs);
  bool ok = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (infer_->try_pop_latest(out.data(), &seq)) {
      ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!ok) {
    policy_.notify_infer_timeout();
    writer_.write_silence_frame();
    return;
  }
  writer_.write_frame(out.data());
}
```

注意：持锁自旋 80 ms 会卡住控制命令。**必须**改为：

- 短临界区只读 `mode` / 调 policy；或
- 超时等待在锁外，写入 VirtMic / `notify_infer_timeout` 再进锁。

推荐实现：拷贝 `mode` 与是否 bypass 后解锁再等 infer，再锁上写 policy/writer（writer 也需与其它线程互斥——当前 writer 只在 worker 用，可只对 policy 加锁）。以现有 `mu_` 覆盖 policy；`InferClient`/`ShmWriter` 仅 worker 线程访问则等 infer 时可释放 `mu_`。

`set_pitch` 本 Task 可先空实现或只存 `pitch_`，Task 6 接转发。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build --config Debug --target engine_tests
ctest --test-dir build -C Debug -R "EngineInferPath|EnginePolicy|EngineLoop" --output-on-failure
```

Expected: PASS。修复因默认 Processed 导致的旧 loop 测试。

- [ ] **Step 5: Commit**

```bash
git add engine/include/virtmic/engine.hpp engine/src/engine.cpp engine/tests/test_engine_infer_path.cpp engine/tests/test_engine_loop.cpp engine/CMakeLists.txt
git commit -m "feat(engine): route Processed frames through InferClient with timeout silence"
```

---

### Task 6: 半音控制转发（engine ↔ infer 管）

**Files:**
- Create: `engine/include/virtmic/infer_control_client.hpp`
- Create: `engine/src/infer_control_client.cpp`
- Modify: `engine/include/virtmic/control_server.hpp`（若需导出 infer pipe 名则放 shared/protocol 旁）
- Modify: `engine/src/control_server.cpp`
- Modify: `engine/src/engine.cpp`（`set_pitch`）
- Modify: `tools/virtmic_ctl.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Consumes: `SetPitchSemitones` 协议
- Produces:
  - `inline constexpr wchar_t kInferControlPipeName[] = L"\\\\.\\pipe\\VirtMicInferControl";`（放 `infer_control_client.hpp` 或 `protocol` 旁）
  - `class InferControlClient { bool send_pitch(int32_t semitones); };` — 打开 infer 管，写入与对外相同的 `encode_command(SetPitchSemitones, …)` 字节
  - `Engine::set_pitch(int32_t)`：钳制到 [-12,12]，调用 `InferControlClient::send_pitch`；失败则 `notify_infer_timeout()`
  - `ControlServer` 识别 `SetPitchSemitones` → `engine_.set_pitch`
  - `virtmic_ctl pitch <n>`

- [ ] **Step 1: 写失败测试**

扩展协议已测。引擎侧增加轻量测试（可 mock 管）：至少保证 `decode`+钳制函数。

```cpp
// engine/tests/test_pitch_clamp.cpp
#include <gtest/gtest.h>
#include "virtmic/engine.hpp"

TEST(PitchClamp, SaturatesToRange) {
  EXPECT_EQ(virtmic::clamp_pitch_semitones(100), 12);
  EXPECT_EQ(virtmic::clamp_pitch_semitones(-100), -12);
  EXPECT_EQ(virtmic::clamp_pitch_semitones(3), 3);
}
```

在 `engine.hpp` 或 `infer_control_client.hpp` 提供：

```cpp
inline int32_t clamp_pitch_semitones(int32_t v) {
  if (v > 12) return 12;
  if (v < -12) return -12;
  return v;
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --config Debug --target engine_tests
```

Expected: 缺 `clamp_pitch_semitones`。

- [ ] **Step 3: 最小实现**

1. 实现 `InferControlClient::send_pitch`（`CreateFileW` + `WriteFile` 整包命令；打不开管返回 false）。
2. `Engine::set_pitch`：钳制 → `send_pitch`；失败 → `policy_.notify_infer_timeout()`。
3. `control_server.cpp` `serve_client_`：`decode_command` 后 `case SetPitchSemitones: engine_.set_pitch(decoded->pitch.semitones);`。
4. `virtmic_ctl.cpp`：

```cpp
} else if (std::strcmp(argv[1], "pitch") == 0 && argc >= 3) {
  const int32_t st = static_cast<int32_t>(std::atoi(argv[2]));
  ok = send_bytes(virtmic::encode_command(
      virtmic::Command::SetPitchSemitones, virtmic::PitchPayload{st}));
}
```

更新 usage 字符串。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build --config Debug --target engine_tests virtmic_ctl
ctest --test-dir build -C Debug -R PitchClamp --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add engine/include/virtmic/infer_control_client.hpp engine/src/infer_control_client.cpp engine/include/virtmic/engine.hpp engine/src/engine.cpp engine/src/control_server.cpp engine/tests/test_pitch_clamp.cpp tools/virtmic_ctl.cpp engine/CMakeLists.txt
git commit -m "feat(engine): forward SetPitchSemitones to infer control pipe"
```

---

### Task 7: Python `virtmic-infer`（SHM + 假变调 + 控制管）

**Files:**
- Create: `infer/README.md`
- Create: `infer/requirements.txt`（内容可仅注释：`# Python 3.10+; stdlib only for fake pitch`）
- Create: `infer/virtmic_infer/__init__.py`
- Create: `infer/virtmic_infer/shm.py`
- Create: `infer/virtmic_infer/pitch.py`
- Create: `infer/virtmic_infer/control.py`
- Create: `infer/virtmic_infer/__main__.py`
- Create: `infer/tests/test_pitch.py`（纯函数单测，pytest 可选；若不想加 pytest，用 `python -c` 断言）

**Interfaces:**
- Consumes: 与 C++ 相同的 header 布局（用 `ctypes`/`struct` 定义 `ShmHeader`）
- Produces: `python -m virtmic_infer` 从仓库 `infer/` 为 cwd 或 `PYTHONPATH=infer` 可运行
  - 打开 `Local\VirtMicInferInV1` / `Out`
  - 循环：等 in `write_index` 前进 → 读帧 → `pitch_shift(frame, semitones)` → 写 out
  - 后台线程听 `\\.\pipe\VirtMicInferControl`，解析与 C++ 相同的二进制头，更新 `semitones`
  - 半音默认 `0`（听感近直通）；启动参数 `--semitones N` 可选

- [ ] **Step 1: 写失败测试（纯 Python 变调）**

```python
# infer/tests/test_pitch.py
from virtmic_infer.pitch import shift_semitones
import array

def test_zero_semitones_preserves_length():
    frame = array.array('f', [0.1] * 480)
    out = shift_semitones(frame, 0)
    assert len(out) == 480
    assert abs(out[0] - 0.1) < 1e-6

def test_plus_twelve_changes_content():
    frame = array.array('f', [0.0] * 480)
    for i in range(480):
        frame[i] = 0.2 if (i // 40) % 2 == 0 else -0.2
    out = shift_semitones(frame, 12)
    assert len(out) == 480
    assert list(out) != list(frame)
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cd infer
python -m pytest tests/test_pitch.py -v
```

若环境无 pytest：

```bash
cd infer
set PYTHONPATH=.
python -c "from virtmic_infer.pitch import shift_semitones"
```

Expected: ImportError。

- [ ] **Step 3: 最小实现**

`pitch.py`：用「按速率重采样」假变调（糙、低延迟）：

```python
import array
import math

def shift_semitones(frame: array.array, semitones: int) -> array.array:
    n = len(frame)
    if semitones == 0 or n == 0:
        return array.array('f', frame)
    ratio = 2.0 ** (semitones / 12.0)
    out = array.array('f', [0.0] * n)
    for i in range(n):
        src = i * ratio
        j = int(src)
        frac = src - j
        if j >= n - 1:
            out[i] = frame[n - 1]
        else:
            out[i] = frame[j] * (1.0 - frac) + frame[j + 1] * frac
    return out
```

`shm.py`：`ctypes.windll.kernel32` → `OpenFileMappingW` / `MapViewOfFile`；定义 header；`read_latest` / `write_frame`。

`control.py`：`CreateNamedPipeW` 或 `win32` 等价；读 12+4 字节；magic/`SetPitchSemitones`；更新共享 `semitones`（`threading` 锁）。

`__main__.py`：主循环 + 控制线程；打印 `virtmic-infer: listening … pitch=N`。

`README.md`：说明

```text
cd infer
set PYTHONPATH=.
python -m virtmic_infer --semitones 4
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cd infer
set PYTHONPATH=.
python -m pytest tests/test_pitch.py -v
```

Expected: PASS

手工烟测（engine 已按 Task 5 可超时静音）：

1. 启动 engine + `virtmic_ctl start`
2. 启动 `python -m virtmic_infer --semitones 4`
3. 启动 feeder；对 VirtMic 监听应变调
4. `virtmic_ctl pitch -4` 听感变化
5. 杀 Python → 应静音

- [ ] **Step 5: Commit**

```bash
git add infer
git commit -m "feat(infer): add Python fake pitch worker over infer SHM rings"
```

---

### Task 8: 一键脚本 + README 验收说明

**Files:**
- Modify: `scripts/start-virtmic.ps1`
- Modify: `README.md`

**Interfaces:**
- Consumes: Task 7 入口
- Produces: 一键顺序：engine → ctl start → **infer** → feeder；infer 失败打印红字警告（引擎将静音），不假装成功变声

- [ ] **Step 1: 更新脚本**

在 `ctl start` 成功后、feeder 前：

```powershell
$InferDir = Join-Path $Root "infer"
$Py = Get-Command python -ErrorAction SilentlyContinue
if (-not $Py) {
  Write-Host "警告: 未找到 python。变声推理未启动，VirtMic 将静音（可用 bypass 出原声）。" -ForegroundColor Yellow
} else {
  Write-Host "启动 virtmic-infer (Python) ..."
  Start-Process -FilePath $Py.Source `
    -ArgumentList @("-m", "virtmic_infer", "--semitones", "4") `
    -WorkingDirectory $InferDir `
    -PassThru | Out-Null
  Start-Sleep -Milliseconds 400
}
```

并在成功提示中加：`virtmic_ctl pitch <n>` 热改半音；杀 python 应静音。

- [ ] **Step 2: 更新 README**

增加 Phase 2 小节：依赖 Python 3.10+、一键脚本、手动 `pitch`、验收 5 条（与 spec 一致）。

- [ ] **Step 3: 手工验收清单（执行者勾选）**

1. 一键后 VirtMic 可听出变调  
2. `virtmic_ctl pitch 7` 运行中变化  
3. 杀 infer → 静音不泄原声；重启 infer + `virtmic_ctl retry` 恢复  
4. `virtmic_ctl bypass 1` 可出原声  
5. `ctest` Debug 全绿  

- [ ] **Step 4: Commit**

```bash
git add scripts/start-virtmic.ps1 README.md
git commit -m "feat: launch Python infer from one-click start script"
```

---

## Self-Review（对照 spec）

| Spec 要求 | 对应 Task |
|-----------|-----------|
| 双 SHM in/out + 名/magic | Task 2, 4, 7 |
| Python 假变调 | Task 7 |
| 半音热更新经对外管转发 | Task 1, 6, 7 |
| 超时默认 80 ms → 静音 | Task 4 常量 + Task 5 |
| 挂了不泄原声；Bypass 唯一原声 | Task 3, 5, 8 |
| 驱动/feeder 不改 | 无 Task 改 driver |
| 一键拉起 infer | Task 8 |
| 单测协议/超时/环 | Task 1–5 |
| 真 RVC 非本阶段 | 无 |

**Placeholder scan：** 无 TBD；测试 hook `debug_write_out_for_test` 为有意的同进程环回。

**Type consistency：** `SetPitchSemitones = 6`、`PitchPayload.semitones: int32_t`、`OutputMode::Processed`、`InferClient::kDefaultTimeoutMs = 80`、环名 `Local\VirtMicInferInV1` / `OutV1`、管 `\\.\pipe\VirtMicInferControl` 全文统一。

---

## 执行交接

Plan 完成后由用户选择：

1. **Subagent-Driven（推荐）** — 每 Task 新子代理 + 任务间审查  
2. **Inline Execution** — 本会话按 executing-plans 连续做并设检查点
