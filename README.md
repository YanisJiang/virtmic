# virtmic

Windows 虚拟麦克风：捕获真实麦克风，经本机变声管线处理后，以系统麦克风「VirtMic」供 Discord / Zoom 等通话软件选用。

## 架构（四块）

```
[真实麦克风]
      │ WASAPI
      ▼
[音频引擎]  ←──IPC──→  [RVC 推理进程 (CUDA)]   ← Phase 2
      │ PCM
      ▼
[VirtMic 虚拟麦克风驱动] ──→ Discord / Zoom / …
      ▲
[Tauri 控制端：托盘 + 设置] ──IPC──→ 引擎 / 推理 / 驱动     ← Phase 3
```

| 单元 | 职责 |
|------|------|
| **引擎** (`engine/`) | 采集、时钟、环形缓冲、bypass / 超时阻断；向驱动侧 SHM 写 PCM |
| **驱动** (`driver/`) | 将 VirtMic 暴露为系统采集设备；消费引擎 PCM |
| **推理** (`infer/`) | Phase 2：Python 假变调（可插拔 IPC）；真 RVC/CUDA 后续替换 |
| **控制端** (`app/`) | 托盘与高级设置（Phase 3）；Phase 1 用 CLI 代替 |

共享约定见 `shared/`（48 kHz、mono float32、10 ms / 480 样本 / 帧）。设计说明：[docs/superpowers/specs/2026-11-17-virtmic-design.md](docs/superpowers/specs/2026-11-17-virtmic-design.md)。

## Phase 1 范围

本阶段目标是打通通路 MVP：

- **直通（passthrough）**：引擎 Start 后向 SHM 写可听帧
- **阻断（block）**：模拟推理超时 → 写静音并发出 `InferTimeout`，**不得泄原声**
- **VirtMic**：WDK 虚拟捕获设备（测试签名安装）

**不做（Phase 1）：** RVC 推理、Tauri UI、正式代码签名。

实现计划：[docs/superpowers/plans/2026-11-17-virtmic-phase1-passthrough.md](docs/superpowers/plans/2026-11-17-virtmic-phase1-passthrough.md)。

## Phase 2：可插拔推理（假变调）

设计 / 计划：

- [docs/superpowers/specs/2026-11-17-virtmic-phase2-infer-design.md](docs/superpowers/specs/2026-11-17-virtmic-phase2-infer-design.md)
- [docs/superpowers/plans/2026-11-17-virtmic-phase2-infer.md](docs/superpowers/plans/2026-11-17-virtmic-phase2-infer.md)

要点：

- 双 SHM：`Local\VirtMicInferInV1` / `Local\VirtMicInferOutV1`
- Python：`infer/` → `python -m virtmic_infer`（需 Python 3.10+，无强制 pip 依赖）
- 热改半音：`virtmic_ctl pitch <n>`（经 engine 转发）
- 推理挂掉 → VirtMic **静音**（不泄原声）；`bypass 1` 仍可出原声

一键脚本会按顺序拉起 engine → infer → `ctl start` → feeder。

手工验收：

1. 一键后 VirtMic 可听出变调  
2. `virtmic_ctl pitch 7` 运行中变化  
3. 杀 infer → 静音；重启 infer 后 `virtmic_ctl retry` 恢复  
4. `virtmic_ctl bypass 1` 可出原声  
5. `ctest --test-dir build -C Debug` 全绿

## 构建与引擎测试

依赖：CMake 3.20+、C++20（Visual Studio 2022）、Windows 10/11 x64。

```powershell
cmake -S . -B build -DVIRTMIC_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

主要产物（Debug）：

- `build\engine\Debug\virtmic-engine.exe` — 引擎前台服务（WASAPI；不可用时退回 SineSource）
- `build\engine\Debug\virtmic_ctl.exe` — 控制命令客户端
- `build\engine\Debug\virtmic-feeder.exe` — 读 SHM，经 IOCTL 推入驱动

## 驱动安装

测试签名、打包与安装步骤见 **[driver/README.md](driver/README.md)**。

注意：

- 开启 testsigning 与驱动安装需要**管理员权限**，首次开启 testsigning 后通常需重启
- 安装成功后，请在 **设置 → 系统 → 声音 → 输入**（或设备管理器）中**手动确认**是否出现「VirtMic」；本仓库自动化无法代替该人工验收

## 通用用法（采集源 ≠ 虚拟麦）

virtmic 把两件事拆开，才能同时服务微信（只认系统默认麦）和 Discord 等软件：

| 角色 | 选谁 |
|------|------|
| **引擎采集** | 自动选一台**物理麦克风**（永远跳过 VirtMic） |
| **微信 / 其它 App 的输入** | 选 **VirtMic**（微信：把系统**默认输入**设为 VirtMic） |

推荐测法：

1. 系统默认输入设为 **VirtMic**（方便微信等）
2. **一键启动**（推荐）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start-virtmic.ps1
```

   或手动：启动 `virtmic-engine` → `virtmic_ctl start` → `virtmic-feeder`
3. 看 engine 日志：`capture from "你的真麦名字"`（不是 VirtMic）
4. 用微信语音 / Discord（选 VirtMic）验证有声

采集优先级：控制台默认真麦 → 通讯默认真麦 → 枚举第一台非 VirtMic。引擎启动时会打印实际设备名。

## 用 CLI 验证超时阻断

策略：**InferTimeout → 静音**；阻断优先于 bypass，超时期间即使开了 bypass 也不写原声。

```powershell
# 终端 1：启动引擎
.\build\engine\Debug\virtmic-engine.exe

# 终端 2：启停 / 超时 / bypass / 恢复
.\build\engine\Debug\virtmic_ctl.exe start
.\build\engine\Debug\virtmic_ctl.exe timeout   # 引擎 stdout 应出现 event: InferTimeout
.\build\engine\Debug\virtmic_ctl.exe bypass 1  # 超时未清时仍应保持静音（不泄原声）
.\build\engine\Debug\virtmic_ctl.exe retry     # 清除阻断后，配合 bypass 可原声/直通
.\build\engine\Debug\virtmic_ctl.exe stop
```

单元与集成测试（`EnginePolicy` / `EngineLoop`）覆盖「超时优先于 bypass」与「retry 后恢复」；SHM 能量可用引擎循环测试或后续工具观察。

完整 VirtMic 听感验证还需：按 [driver/README.md](driver/README.md) 安装驱动并运行 `virtmic-feeder`。

## Phase 1 验收清单

- [x] 单元测试：`ctest` 全绿（本机 Debug：15/15）
- [x] SHM 在 Start 时有能量、Timeout 后近零（`EngineLoop.StartPassthroughTimeoutSilenceThenRetryRestore` / `ShmWriter`）
- [ ] 系统中出现 VirtMic（**需管理员开启 testsigning、安装驱动，并在声音设置中手动确认**）
- [x] 超时不泄原声（阻断优先于 bypass；`EnginePolicy` + `EngineLoop`；CLI：`timeout` → `InferTimeout` / 静音）
- [x] 手动 bypass 在 retry 后可原声直通（`retry` 后 + `bypass 1`；同上测试覆盖）

## 后续阶段（占位）

| 阶段 | 计划 | 内容 |
|------|------|------|
| **Phase 2** | （待写）`docs/superpowers/plans/…-phase2-rvc.md` | `infer/` RVC + CUDA；引擎接真推理与超时 |
| **Phase 3** | （待写）`docs/superpowers/plans/…-phase3-tauri.md` | `app/` Tauri 托盘 + 高级设置窗 |

---

许可证与第三方：驱动树基于 Microsoft Windows-driver-samples（见各源文件头与 `driver/README.md`）。
