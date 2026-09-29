# eu4cjk

**English intro below.**

 Europa Universalis IV（Linux 原生版）的中日韩文字显示补丁。
由 Windows 平台的 [EU4dll](https://github.com/matanki-saito/EU4dll) 移植而来。

[安装教程](INSTALL.md) · [从源码构建](BUILD.md)（推荐） · [故障排查](INSTALL.md#故障排查)

## 这是什么

EU4 的 Linux 原生版无法显示中文等双字节文字：中文 mod 的文本会变成乱码或白色方块。
本补丁通过 `LD_PRELOAD` 注入 `libeu4cjk.so`，在运行时修补引擎的文字渲染、
测量、折行与存档名处理，使预转义的中日韩文本与扩容位图字体正确显示。

- **不修改任何游戏文件**（纯运行时注入，卸载即恢复原版）
- 不触碰 Steam API / DRM
- 稳态零日志、零分配、零系统调用开销（发布版已剥离全部诊断探测）
- 版本不符时优雅降级为原版行为（不崩溃）

## 语言支持

| 语言 | 状态 |
|---|---|
| 简体中文 | ✅ 已实测（配合中文多字节 mod） |
| 日语 | 🔶 协议与上游 EU4dll 完全一致，理论可用（未实测） |
| 韩语 | ❓ 取决于是否存在对应多字节 mod（未实测） |

补丁本身（转义协议 `0x10~0x13`、字形门、存档名处理）是语言无关的；
语言相关的文本与字体由对应语言的创意工坊多字节 mod 提供，见
[安装教程](INSTALL.md)。

## 兼容性

| 项 | 要求 |
|---|---|
| 游戏 | EU4 **v1.37.5** Linux 原生版（Steam buildid 15918133） |
| 系统 | 任意 glibc x86-64 Linux（.so 仅依赖 libc） |
| Steam | Flatpak 版与原生版均可 |
| 中文 mod | 创意工坊订阅（见安装教程） |

其他游戏版本：钩子逐点字节校验失败则自动跳过，游戏按原版运行，不会崩溃。

## 快速开始

1. [订阅中文 mod](INSTALL.md#第-2-步订阅中文-mod必需)（工坊两个条目）
2. [放置 .so 并配置启动选项](INSTALL.md#第-3-步放置-so-文件)
3. 启动游戏，主菜单出现中文即成功

**推荐自行编译**（见 [BUILD.md](BUILD.md)，一条 cmake 命令即可）；
仓库 `release/` 与 GitHub Releases 也提供预编译产物作为便利。

## 致谢

- [EU4dll](https://github.com/matanki-saito/EU4dll)（☆ (ゝω・)v，MIT）——
  Windows 平台的原项目：转义协议、字体扩容方案与引擎逆向成果是本移植的基础
- 中文多字节 mod 的作者们（文本预转义 + 扩容位图字体）
- Paradox Interactive 的 Europa Universalis IV

## 许可

[MIT](LICENSE)。本项目与 Paradox Interactive 无关联，非官方，仅为爱好者作品。

---

# English

**eu4cjk** is a CJK text-display patch for the **Linux native** version of
Europa Universalis IV (v1.37.5), ported from the Windows project
[EU4dll](https://github.com/matanki-saito/EU4dll).

The Linux client renders CJK mods as mojibake / tofu blocks. eu4cjk injects a
small shared library via `LD_PRELOAD` and patches the engine at runtime —
text rendering, measurement, word-wrap and save-name handling — so the
pre-escaped localization text and extended bitmap fonts shipped by community
"multibyte" mods display correctly.

- No game files are modified; removing the launch option restores vanilla
- No Steam API / DRM involvement
- Release build has zero steady-state overhead (diagnostic probes stripped)
- Graceful degradation on game-version mismatch (never crashes)
- Language-agnostic core (escape protocol identical to upstream EU4dll):
  Chinese verified, Japanese expected to work with the JP multibyte mod

**Quick start**: subscribe to the Chinese multibyte mods (Workshop links in
the [install guide](INSTALL.md)), drop the `.so` in place, set the Steam
launch option, play. Building from source is recommended and takes one cmake
command — see [BUILD.md](BUILD.md).

License: MIT — see [LICENSE](LICENSE). Unofficial fan project, not
affiliated with Paradox Interactive.
