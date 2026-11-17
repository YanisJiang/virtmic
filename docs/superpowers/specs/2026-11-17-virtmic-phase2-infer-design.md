# virtmic Phase 2：可插拔推理（假变调先行）设计说明

**日期：** 2026-11-17  
**状态：** 待用户审阅后进入实现计划  
**前置：** `docs/superpowers/specs/2026-11-17-virtmic-design.md`（总设计）、Phase 1 直通已落地  
**路径：** `D:\Workspace\AI\virtmic`

## 目标

在不动 VirtMic 驱动 / feeder 的前提下，打通 **engine ↔ 推理进程** 的可插拔 IPC：先用 Python **假变调**验证低延迟链路、热参数与超时静音；以后将处理函数换成真 RVC，协议与环布局保持不变。

## 已确认决策

| 主题 | 决策 |
|------|------|
| 接入形态 | 方案 1：双 SHM 环（in/out）+ 控制管 |
| 推理运行时 | Python（假变调与日后 RVC 同语言栈） |
| 占位效果 | 可听出的实时变调（半音），音质可糙 |
| 半音调节 | 运行中热更新，不重启推理进程 |
| 推理不可用 | **静音**，不得自动泄原声 |
| 延迟取向（假变调） | 优先低延迟，目标额外约 +20–40 ms |
| 本阶段不做 | 真 RVC / CUDA、托盘 UI、多音色管理 |

## 架构与数据流

```
真实麦 ──WASAPI──► engine
                    │
                    ├─写─► Local\VirtMicInferInV1    (SHM_in)
                    │              │
                    │              ▼
                    │         virtmic-infer (Python)
                    │              │
                    │              ▼
                    ├─读─◄ Local\VirtMicInferOutV1   (SHM_out)
                    │
                    ├─策略：超时 / 无心跳 → 向 VirtMic 写静音
                    │
                    └─写─► Local\VirtMicPcmV1 ──feeder──► VirtMic 驱动

外部 ctl ──\\.\pipe\VirtMicControl──► engine ──转发──► \\.\pipe\VirtMicInferControl
```

### 职责

| 单元 | 做什么 | 不做什么 |
|------|--------|----------|
| **engine** | 采麦；写 SHM_in；读 SHM_out；超时/不健康时写静音；写 VirtMic SHM；转发半音等推理命令 | 变调算法、加载模型、写驱动 IOCTL |
| **infer (Python)** | 读 in、处理、写 out；听 infer 控制管；心跳/健康 | 写 `Local\VirtMicPcmV1`、接触驱动、直连用户 ctl |
| **feeder / 驱动** | 与 Phase 1 相同 | 不知推理存在 |

### PCM 约定（与 Phase 1 对齐）

- 采样率 48 kHz，单声道，小端 float32  
- 帧长 10 ms（480 samples / 帧）  
- infer 两环与现有 `ShmHeader` **同构**；以映射名 + magic/version 区分，避免与 `Local\VirtMicPcmV1` 混淆  

建议命名（实现时可微调，但须写入 `shared/` 常量）：

| 环 | 映射名 | 用途 |
|----|--------|------|
| VirtMic 出口 | `Local\VirtMicPcmV1` | engine → feeder（不变） |
| 推理输入 | `Local\VirtMicInferInV1` | engine → infer |
| 推理输出 | `Local\VirtMicInferOutV1` | infer → engine |

## 控制协议

### 对外（用户 / 脚本 / 日后托盘）

- 仍只连：`\\.\pipe\VirtMicControl`  
- engine 为唯一对外控制入口；推理相关命令由 engine **转发**到 infer。

### 对内（engine ↔ infer）

- 本地命名管：`\\.\pipe\VirtMicInferControl`（仅本机、仅 engine 连接）  
- infer 不接受外部第二入口，避免状态分叉。

### 命令与事件扩展

在现有 `shared` 协议上增加（名称以实现代码枚举为准，语义如下）：

| 方向 | 名称 | 语义 |
|------|------|------|
| 命令 | `SetPitchSemitones` | 有符号半音（建议钳制 -12…+12）；校验失败则拒绝并保持旧值 |
| 命令 | `GetInferStatus` | 可选；可推迟到下一小步 |
| 事件 | `InferTimeout` | 已有；推理无进展 / 不健康 |
| 事件 | `InferOnline` / `InferOffline` | 可选；第一版可仅依赖超时 + 日志 |

保留语义不变：

- `Start` / `Stop` / `Retry`  
- `SetBypass`：**唯一**允许原声进入 VirtMic 的开关（须用户显式开启）  
- 推理挂死 **不等于** bypass  

### 半音热更新

1. ctl → engine：`SetPitchSemitones`  
2. engine 校验范围后转发 infer  
3. infer 自**下一输出帧**起使用新半音  
4. 转发失败（管不通等）→ 判推理不健康 → 静音 + 超时/离线类事件  

## 超时与静音策略

与总设计一致：**宁可静音，不可无提示泄原声**。

1. engine 向 SHM_in 写入后，在超时窗内（**默认 80 ms**，可配置）未读到足够新的 SHM_out 进展 → 向 `VirtMicPcmV1` **写静音**，并发 `InferTimeout`。  
2. infer 未启动、崩溃、心跳断开 → 同上。  
3. `Retry`：清除超时锁定，重新等待 infer；恢复出声前继续静音。  
4. 假变调自身引入的小延迟（目标 +20–40 ms）**不算**超时；超时只针对「推理侧无进展」。  
5. 手动 Bypass 开启时，按总设计走原声路径（本阶段不改变该产品语义）。

## 组件落点

| 路径 | 职责 |
|------|------|
| `shared/` | infer 环名与布局常量；`SetPitchSemitones` 等协议编解码 |
| `engine/` | InferClient（写 in / 读 out / 心跳与超时）；接入 `EnginePolicy`；控制转发 |
| `infer/` | Python：SHM 读写、假变调、infer 控制管、心跳 |
| `scripts/start-virtmic.ps1` | 一键拉起 infer；失败时明确提示（引擎侧将静音） |
| `tools/` | 可选：半音相关 ctl 封装，便于手工验收 |
| `driver/` / feeder | **本阶段不改** |

### 假变调约束

- 算法：简单时域变调即可，优先延迟而非音质。  
- 帧长与 Phase 1 一致，避免 engine 侧复杂重切窗。  
- 依赖尽量少（实现计划中钉死；倾向标准库 + 轻量数值库，避免重框架）。

## 错误处理摘要

| 情况 | 行为 |
|------|------|
| infer 未起 / 崩溃 | VirtMic 静音 + `InferTimeout`（及可选 Offline） |
| SHM 创建/映射失败 | 拒绝进入可用变声态或立即按不健康处理；日志明确 |
| 半音越界 | 拒绝命令，保持旧参数 |
| 用户手动 Bypass | 唯一原声出口 |
| 日后真 RVC | 只替换 `infer/` 内处理实现；SHM + 控制协议不变 |

## 非目标（本 Phase）

- 真 RVC 模型加载、CUDA、音色切换 UI  
- Tauri 托盘 / 高级窗  
- 修改 VirtMic 驱动或 feeder 协议  
- 自动「失败则直通原声」模式  

## 验收标准

1. 一键启动后，应用选择 VirtMic，说话可听出变调。  
2. 运行中修改半音，短时间内听感变化，无需重启进程。  
3. 杀掉 infer → VirtMic 静音且不出现原声；拉起 infer 并 `Retry` 后恢复变调。  
4. 手动 Bypass 仍可出原声。  
5. 单测覆盖：协议编解码、超时→静音策略、InferClient 环读写（可用假帧，不依赖真麦）。

## 与总设计的关系

- 落实总设计中「引擎 ↔ 推理 IPC」「超时阻断」「推理不写驱动」。  
- 将总设计中的 RVC 进程具体化为：先假变调 Python worker，真 RVC 为同 IPC 上的后续替换。  
- Phase 3（Tauri）仍通过 `VirtMicControl` 操作；本 Phase 为其预留半音与推理状态语义。
