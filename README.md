# HwVRGlassLite

华为 VR Glass（CV10）的轻量级 Windows 客户端：替代官方 PC 助手做日常管理，并提供震动补丁、驱动模式切换和手柄姿态修正。

> 纯 AI 构建项目（DeepSeek V4 Flash + DeepSeek Harness）。

## 功能

- 原厂驱动 ⇄ 补丁驱动一键切换（切换前自动询问并关闭 SteamVR / Steam）
- 震动模式：透传 / 翻译 / 静音（补丁驱动生效）
- 手柄姿态微调（旋转）
- 与官方助手共存：切补丁时临时改名屏蔽，切回原厂自动恢复
- SteamVR 原生透视
- SteamVR 原生亮度调节

## 构建

只依赖 MinGW-w64（g++ / windres）和 Windows SDK 头文件，静态链接：

```powershell
.\build.ps1
```

产物：`HwVRGlassLite.exe`（单文件、无第三方运行库依赖）。

## 说明

- 本仓库**不包含**华为官方驱动、资源文件或安装包；使用前请自备官方 PC 助手安装包，并确认你拥有对应设备。
- 驱动切换逻辑默认安装目录为 `C:\Program\HwVRGlassLite`，可通过 `--register` 重新登记。
- CLI：`--status`、`--start-vr`、`--stop-vr`、`--to-patch`、`--to-factory`、`--register`、`--unregister`、`--verify`。

## 更新日志

- 1.4.5 优化与官方助手的共存；优化客户端 UI
- 1.4.1 适配 SteamVR 亮度控制
- 1.4.0 增加 SteamVR 原生透视功能
- 1.3.0 修复手柄震动 BUG
- 1.2.0 修复 SteamVR 关掉后自动重启的 BUG，修复手柄偏移
- 1.1.0 修复每次都拉取房间设置的 bug
- 1.0 重构客户端

## English

A lightweight Windows client for Huawei VR Glass (CV10). It replaces the official PC assistant for day-to-day control and adds a haptic patch, driver mode switching and controller pose correction.

Features: one-click factory/patch driver switching (asks before closing SteamVR/Steam), haptic modes (passthrough / translate / mute), controller pose tuning, coexistence with the official assistant, SteamVR native passthrough and native brightness control.

Build with MinGW-w64 via `build.ps1`. The repository does not ship Huawei proprietary drivers or resources; bring your own official installation package and only use it with hardware you own.

Pure-AI-built project (DeepSeek V4 Flash + DeepSeek Harness).
