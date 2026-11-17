# VirtMic 虚拟麦克风驱动（WDK）

## 选型

基于 Microsoft **Simple Audio Sample**（[Windows-driver-samples/audio/simpleaudiosample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample)），PortCls + WaveRT + KMDF。

裁剪范围：

- **只保留捕获（microphone）端点**；render/speaker 不再注册
- 设备友好名 / DeviceDesc：**VirtMic**（硬件 ID：`ROOT\VirtMic`）
- 捕获格式：48 kHz / 32-bit PCM / **stereo**（mono float32 在 feeder 侧复制为左右声道）
- 默认正弦音源改为 **IOCTL 环形缓冲**：用户态读引擎 SHM，经 `\\.\VirtMicFeed` 推帧

数据路径（文档化选择）：

```
virtmic-engine  --写-->  Local\VirtMicPcmV1 (float32 mono, 480/帧)
        ^
virtmic-feeder  --读 SHM-->  IOCTL_VIRTMIC_PUSH_FRAME  -->  驱动 ring  -->  VirtMic 捕获端点
欠载：feeder 推静音并置 shm flags.underrun；驱动 ring 空时输出静音
```

内核无法直接打开会话内 `Local\` 映射，因此采用 **SHM + 用户态 feeder + IOCTL**（见 `engine/src/driver_client.cpp`、`driver/feeder/main.cpp`）。PCM 几何与 `shared/include/virtmic/pcm.hpp` 一致。

## WDK / 工具链

| 项 | 版本 / 路径 |
|----|-------------|
| Windows SDK / WDK | **10.0.26100.0** |
| KMDF | 1.15（stampinf） |
| VS | 2022 Community（`WindowsKernelModeDriver10.0` toolset） |
| Spectre | 工程已设 `SpectreMitigation=false`（本机未装 Spectre 库时） |

## 构建

```powershell
# 驱动 .sys / .inf / .cat
powershell -ExecutionPolicy Bypass -File driver\scripts\build-driver.ps1 -Configuration Release

# 产物目录
# driver\simpleaudiosample\x64\Release\package\
#   SimpleAudioSample.sys
#   SimpleAudioSample.inf
#   simpleaudiosample.cat

# 用户态 feeder（与引擎一起）
cmake --build build --config Debug --target virtmic-feeder
```

## 测试签名与安装（需管理员）

1. **开启测试签名**（首次需重启）：

```powershell
# 管理员
bcdedit /set testsigning on
# 重启后确认：bcdedit | findstr testsigning  ->  Yes
```

2. **签名并安装**（脚本封装）：

```powershell
# 管理员
powershell -ExecutionPolicy Bypass -File driver\scripts\sign-and-install.ps1 `
  -PackageDir "D:\Workspace\AI\virtmic\driver\simpleaudiosample\x64\Release\package"
```

手动等价步骤（须同时签名 **.sys** 与 **.cat**；无内核调试器时缺任一则安装/加载会失败）：

```powershell
$kit = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
$kit86 = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86"
$pkg = "D:\Workspace\AI\virtmic\driver\simpleaudiosample\x64\Release\package"

# 自签证书（一次性）
& "$kit\MakeCert.exe" -r -pe -ss PrivateCertStore -n "CN=VirtMic Test" -eku 1.3.6.1.5.5.7.3.3 VirtMicTest.cer
& "$kit\certmgr.exe" /add VirtMicTest.cer /s /r localMachine root
& "$kit\certmgr.exe" /add VirtMicTest.cer /s /r localMachine trustedpublisher

# 1) 先签 .sys  2) Inf2Cat 按已签名二进制重生 .cat  3) 再签 .cat
& "$kit\signtool.exe" sign /fd SHA256 /s PrivateCertStore /n "VirtMic Test" "$pkg\SimpleAudioSample.sys"
& "$kit86\Inf2Cat.exe" /driver:$pkg /os:10_X64,10_NI_X64,10_GE_X64 /verbose
& "$kit\signtool.exe" sign /fd SHA256 /s PrivateCertStore /n "VirtMic Test" "$pkg\simpleaudiosample.cat"

pnputil /add-driver "$pkg\SimpleAudioSample.inf" /install

# ROOT 枚举设备（需 WDK Tools 中的 devcon）
devcon install "$pkg\SimpleAudioSample.inf" ROOT\VirtMic
# 若无 devcon：设备管理器 -> 操作 -> 添加过时硬件 -> 从磁盘安装上述 INF，硬件 ID = ROOT\VirtMic
```

3. **卸载**：

```powershell
# 设备管理器卸载「VirtMic」，勾选删除驱动软件
# 或：
pnputil /enum-drivers
pnputil /delete-driver oemXX.inf /uninstall /force
devcon remove ROOT\VirtMic
```

关闭测试签名（恢复生产机器时）：`bcdedit /set testsigning off` 后重启。

## 验证 VirtMic 出现

1. **设置 → 系统 → 声音 → 输入** 应出现 **VirtMic**
2. 设备管理器 →「声音、视频和游戏控制器」→ **VirtMic**
3. 端到端：

```powershell
# 终端 A
.\build\engine\Debug\virtmic-engine.exe
# 另开：virtmic_ctl start

# 终端 B（驱动已安装后）
.\build\engine\Debug\virtmic-feeder.exe

# 声音设置中选 VirtMic，或「聆听此设备」
```

## 最终文件清单

### 仓库内（源码 / 脚本）

| 路径 | 说明 |
|------|------|
| `driver/README.md` | 本文档 |
| `driver/include/virtmic_ioctl.h` | 用户态 IOCTL 契约 |
| `driver/feeder/main.cpp` | SHM→IOCTL feeder |
| `driver/scripts/build-driver.ps1` | MSBuild 驱动 |
| `driver/scripts/sign-and-install.ps1` | 测试签名 + 安装 |
| `driver/simpleaudiosample/` | 裁剪后的 SimpleAudioSample 工程 |
| `.../Source/Utilities/virtmic_pcm_feed.{h,cpp}` | 内核 ring + 控制设备 |
| `.../Source/Inc/virtmic_ioctl.h` | 内核侧 IOCTL 头 |
| `.../Source/Main/SimpleAudioSample.inx` | VirtMic INF 源 |
| `engine/include/virtmic/driver_client.hpp` | IOCTL 客户端 |
| `engine/src/driver_client.cpp` | IOCTL 客户端实现 |

### 构建产物（不入库，本地生成）

| 路径 | 说明 |
|------|------|
| `driver/simpleaudiosample/x64/Release/package/SimpleAudioSample.sys` | 驱动二进制 |
| `driver/simpleaudiosample/x64/Release/package/SimpleAudioSample.inf` | 安装 INF |
| `driver/simpleaudiosample/x64/Release/package/simpleaudiosample.cat` | 目录文件（由 Inf2Cat 生成；安装前与 .sys 一并签名） |
| `build/engine/Debug/virtmic-feeder.exe` | feeder |

### 第三方归属

`driver/simpleaudiosample/` 源自 Microsoft Windows-driver-samples（Microsoft 版权声明保留于各源文件头）。VirtMic 修改包括：捕获-only、友好名、PCM feed/IOCTL。

## 已知限制

- 安装 / `bcdedit testsigning` / `devcon` 需要**管理员**；未提权时只能完成构建
- 端点显示名依赖 INF `FriendlyName=VirtMic`；部分 UI 可能附加设备后缀
- 驱动暴露 stereo PCM；与引擎 mono float32 的桥接在 feeder 完成
