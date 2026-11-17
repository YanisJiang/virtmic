# Task 8 Report: VirtMic 虚拟麦克风驱动（WDK）

## Status

**DONE_WITH_CONCERNS**

## Summary

基于 Microsoft Simple Audio Sample 裁剪为 **capture-only VirtMic**，接入引擎 SHM（`Local\VirtMicPcmV1`）经用户态 feeder + IOCTL 推帧；Release x64 已成功产出 `.sys` / `.inf` / `.cat`。本会话**非管理员**，未能完成 testsigning 开启、驱动签名安装与「声音设置中可见 VirtMic」的实机验证。

## Architecture choice

| 项 | 选择 |
|----|------|
| 样例 | SimpleAudioSample（非完整 SYSVAD） |
| 拓扑 | 仅 Mic 捕获；render 数组清空 |
| 用户↔内核 | **IOCTL**（`\\.\VirtMicFeed` / `IOCTL_VIRTMIC_PUSH_FRAME`），因内核无法打开会话 `Local\` SHM |
| PCM | 引擎 SHM：48 kHz mono float32 × 480；驱动端点：48 kHz stereo 32-bit PCM（feeder 复制声道） |
| 欠载 | feeder 推静音并置 `flags.underrun`；驱动 ring 空 → 静音 |

## What was built

- `driver/README.md` — 选型、WDK 10.0.26100、构建/签名/安装/卸载、文件清单
- `driver/simpleaudiosample/` — 裁剪样例 + `virtmic_pcm_feed` + VirtMic INF
- `driver/feeder/main.cpp` + `engine/.../driver_client.*`
- 构建产物：`driver/simpleaudiosample/x64/Release/package/{SimpleAudioSample.sys,.inf,simpleaudiosample.cat}`

## Verification

| 检查 | 结果 |
|------|------|
| MSBuild Release x64 驱动 | **通过**（0 warning / 0 error） |
| `ctest` Engine/SHM/Protocol 等 | **12/12 通过** |
| `virtmic-feeder` 链接 | **通过** |
| `bcdedit testsigning` / 签名安装 | **未执行**（当前进程非 Admin） |
| 声音设置出现 VirtMic | **未验证**（依赖上一步） |
| 端到端聆听 | **未验证** |

## Manual steps remaining

1. 管理员 PowerShell：`bcdedit /set testsigning on` → **重启**
2. `driver\scripts\sign-and-install.ps1 -PackageDir ...\package`
3. 确认设置 → 声音 → 输入 → **VirtMic**
4. 启动 `virtmic-engine` + `ctl start` + `virtmic-feeder`，做聆听/Discord 输入抽检

详见 `driver/README.md`。

## Commits

1. `6601e5a` — `feat: VirtMic 虚拟麦克风驱动（测试签名通路）`
2. `a13be98` — `chore: drop accidental WDK samples zip from driver tree`

## Review follow-up (Important finding)

**Issue:** testsigning installs require both `.sys` and `.cat` signed; script/README previously only signed `.sys`.

**Fix (docs/scripts only; no admin install in this session):**

- `driver/scripts/sign-and-install.ps1`: after signing `.sys`, run Inf2Cat to regenerate `.cat`, then `signtool sign` the `.cat`
- `driver/README.md`: manual steps aligned (MakeCert → sign sys → Inf2Cat → sign cat → pnputil/devcon)

Order matters: Inf2Cat must hash the Authenticode-signed `.sys`.
