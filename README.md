# Myopic Defocus

**实验性的 Windows 色差离焦滤镜。不是医疗器械，也不是经验证的近视治疗工具。**

红通道保持清晰，绿、蓝通道分别做全分辨率高斯低通，参考 Refractify
开源扩展的公开计算思路。支持 Direct3D 11 GPU 处理，可调整屏幕尺寸、
观看距离、瞳孔直径和效果强度。独立实现，非 Refractify 官方产品。

## 下载与使用

在右侧 **Releases** 下载 Windows x64 便携版。首个发布标注为实验预发布，
不是完成实机兼容性验收的稳定版。退出旧版后运行程序；托盘右键打开 Settings。

- `Ctrl+Alt+F8`：开启/关闭。
- `Ctrl+Alt+↑ / ↓`：强度增减 5 个百分点。
- [中文参数说明](docs/参数调整说明.md)：单位、参数含义、配置和异常处理。
- [检查记录与未解决问题](BUG-CHECK.md)。

要求 Windows 10 2004+ / Windows 11 x64、D3D11 硬件显卡及主显示器横屏。
仅处理主屏；HDR、受保护内容、竖屏和独占全屏不受支持或未验证。
如果覆盖层异常且快捷键无响应，使用 Ctrl+Alt+Delete → 任务管理器退出程序。

## 已知边界

- 已完成交叉编译和部分源码测试，尚未完成充分的 Windows/GPU 实机验证。
- 输入框 I-Beam 闪烁尚未确认彻底解决；睡眠/驱动恢复仍需不同设备测试。
- 没有与浏览器滤镜完成逐像素等效验证，更不代表精确复刻论文实验条件。
- 小样本短期论文不证明本程序能防控近视、缩轴或长期安全。
- Effect strength 是图像混合比例，不是疗效百分比；不适时停用。

## 许可证

GPL-3.0-or-later。参考项目与论文署名见 [NOTICE.md](NOTICE.md)。

---

## English / build and technical notes


Version **1.3.5** — renamed and source-audited experimental build.

An independent Windows desktop chromatic filter inspired by Refractify's
GPL-3.0 browser extension. Not a Refractify product, medical device, or proven
myopia treatment. Algorithm similarity does not establish clinical equivalence.

## Run

Exit all previous versions, then run `MyopicDefocus.exe`. No administrator
privileges are required. Windows 10 2004+ or Windows 11 x64, a D3D11 hardware
adapter owning the primary output, and a landscape primary display are required.
Only the primary SDR display is handled; HDR, protected content, exclusive
fullscreen and rotated displays are not validated/supported.

- Tray → Settings: four optical/mixing parameters; click Save to apply.
- Ctrl+Alt+F8: enable/disable the overlay.
- Ctrl+Alt+Up/Down: change strength by five percentage points.
- Tray → Open config file: locate the active configuration.
- Tray → Exit: quit.

If the overlay blocks the desktop and the hotkey does not respond, use
Ctrl+Alt+Delete → Task Manager and end `MyopicDefocus.exe`.

## Configuration and migration

Normal path: `%LOCALAPPDATA%\MyopicDefocus\settings.ini`.
An existing `MyopicDefocus.ini` beside the EXE takes priority.
Legacy portable/AppData configurations are copied to the new name, without
changing optical parameters or deleting the original. If the copy fails, the
old path is retained where possible. Existing new-name files take precedence.
Internal window-class IDs intentionally retain legacy names to detect a
still-running older version; this is not the display/product name.

`FrameLatencyWait=1` and `DirtyRects=1` remain the default requests.
Unsupported low-latency flags fall back to a basic sequential flip chain.
No DirectComposition renderer option is provided.

See **docs/参数调整说明.pdf** or its Markdown/text alternatives for parameter
units, input ranges, strength semantics, configuration, recovery and limitations.
See **BUG-CHECK.md** for the precise audit scope and outstanding risks.

## Build (MinGW-w64)

```bash
cmake -S source -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/source/toolchain-mingw64.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The embedded shaders compile at runtime on Windows using D3DCompiler; successful
C++ cross-compilation does not validate the runtime GPU path.

## Source-level regression checks (Linux)

```bash
python3 source/tests/audit_checks.py
```

Requires native g++, AddressSanitizer and UndefinedBehaviorSanitizer runtimes.
This checks extracted production parser/math and source lifecycle guards,
not Windows mouse handling, DXGI/DWM behavior, recovery or medical safety.

## Filtering and limits

Untouched red plus full-resolution green/blue Gaussian filtering, 0.47/1.33 D
relative LCA, sigma = blur-circle pixel diameter × 0.32. Dynamic discrete ±3σ
kernels, packed bilinear taps, clamp edges, sigma capped at 32 px. Strength is
linear image blending, not a clinical dose. No pixel-identical browser comparison,
monitor spectral calibration, or research PSF/MTF validation has been completed.

GPL-3.0-or-later; attribution and research links are in NOTICE.md.
