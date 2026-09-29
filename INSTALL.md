# 安装教程

适用于 EU4 Linux 原生版 **v1.37.5**（Steam buildid 15918133）。
其他版本请先看[兼容性说明](#兼容性说明)。

整个安装约 5 分钟，**不修改任何游戏文件**，随时可完全卸载。

---

## 前提条件

- Steam（Flatpak 版或原生版均可）+ 正版 EU4 Linux 原生版 v1.37.5
- 磁盘上任意位置（不需要写权限到游戏目录）

> 下文以 Flatpak 版 Steam 为主要示例（最常见），原生版差异单独标注。
> 判断方法：`flatpak list | grep -i steam` 有输出 = Flatpak 版。

---

## 第 1 步：获取 libeu4cjk.so

两种方式任选：

**方式 A（推荐）：自行编译** —— 见 [BUILD.md](BUILD.md)，
需要 gcc/clang + cmake，一条命令完成，编译产物在 `build/libeu4cjk.so`。

**方式 B：下载预编译件** —— GitHub 仓库 `release/` 目录或
Releases 页面的 `libeu4cjk.so`（约 380 KB）。

> 推荐自行编译：预编译件无法保证与你环境完全一致。两种方式的产物完全等效。

---

## 第 2 步：订阅中文 mod（必需）

本补丁只负责让引擎"能显示"双字节文字；中文文本与扩容字体由中文多字节
mod 提供（mod 名以 `eu4_chinese` / `eu4_chinese_sup` 为准，版本需与
游戏 v1.37.5 配套）。获取方式二选一：

**方式 A（推荐）：Steam 创意工坊订阅**（自动更新，链接如下）

| mod | 创意工坊链接 |
|---|---|
| eu4_chinese（主汉化） | https://steamcommunity.com/sharedfiles/filedetails/?id=2976470733 |
| eu4_chinese_sup（补充） | https://steamcommunity.com/sharedfiles/filedetails/?id=1999055990 |

**方式 B：第三方网站下载**（如 52pcgame 等汉化站的 mod 下载区）

从第三方网站下载的中文 mod 文件同样可用，手动放入 Paradox 用户目录
的 `mod/` 文件夹：

```bash
# Flatpak 版 Steam：
~/.var/app/com.valvesoftware.Steam/.local/share/Paradox Interactive/Europa Universalis IV/mod/
# 原生 Steam：
~/.local/share/Paradox Interactive/Europa Universalis IV/mod/
```

放入后的目录结构应类似（目录 + 同名 `.mod` 描述文件成对出现）：

```
mod/
├── eu4_chinese/
├── eu4_chinese.mod
├── eu4_chinese_sup/
└── eu4_chinese_sup.mod
```

> 第三方下载请自行确认 mod 版本与游戏 v1.37.5 配套。

订阅或放置后，首次启动 EU4 时在 Paradox Launcher 中启用：

1. 在 launcher 中打开 **Playsets（游戏配置）** 页
2. 新建一个 playset（如 `chinese`）
3. 勾选启用 **eu4_chinese** 和 **eu4_chinese_sup** 两个 mod
4. 选中该 playset，点击 Play

> 本补丁不包含、也不分发 mod 内容——mod 是其作者的成果，请通过
> 创意工坊获取。mod 与补丁的分工：mod 提供预转义文本 + 扩容位图字体
> （跨平台数据），补丁让 Linux 引擎正确渲染它们。

---

## 第 3 步：放置 .so 文件

**路径硬性要求：整个路径不能含空格**（`LD_PRELOAD` 用空格分隔多个库）。

### A. Flatpak 版 Steam（Ubuntu 软件商店装的 Steam 多为此种）

把 `libeu4cjk.so` 放到 Flatpak 数据目录：

```bash
mkdir -p ~/.var/app/com.valvesoftware.Steam/.local/share/eu4cjk
cp libeu4cjk.so ~/.var/app/com.valvesoftware.Steam/.local/share/eu4cjk/
```

（这个目录是 Flatpak 沙盒内可见的位置——放游戏目录之外的其他
地方大概率会被沙盒挡住，这是 Flatpak 机制决定的。）

### B. 原生（deb/rpm 包）Steam

任意无空格路径均可，例如：

```bash
mkdir -p ~/.local/share/eu4cjk
cp libeu4cjk.so ~/.local/share/eu4cjk/
```

---

## 第 4 步：配置 Steam 启动选项

Steam → 库 → 右键 Europa Universalis IV → **属性** → **通用/常规** →
**启动选项**，填入（注意替换 `<你的用户名>` 为实际用户名，即
终端里 `echo $HOME` 显示的 `/home/` 之后部分）：

**Flatpak 版 Steam：**

```
LD_PRELOAD=/home/<你的用户名>/.var/app/com.valvesoftware.Steam/.local/share/eu4cjk/libeu4cjk.so %command%
```

**原生 Steam：**

```
LD_PRELOAD=/home/<你的用户名>/.local/share/eu4cjk/libeu4cjk.so %command%
```

> ⚠️ 三个常见坑：
> 1. **必须写完整的字面路径**。网上/文档中 `$XXX` 形式的变量若未在
>    你的环境定义，shell 会展开成空，导致 preload 静默失效。
> 2. 路径里**不能有空格**（`LD_PRELOAD` 的空格是库分隔符）。
> 3. `%command%` 必须保留，Steam 用它替换实际的游戏启动命令。

---

## 第 5 步：验证安装

正常启动游戏（用你启用了中文 mod 的 playset）：

**游戏内验证**：主菜单出现中文 = 成功。进入游戏后地图名、事件、
tooltip 均应正常；保存游戏后存档列表里的中文名正常显示。

**日志验证**（可选，出问题时排查用）：

```bash
# Flatpak 版 Steam：
cat ~/.var/app/com.valvesoftware.Steam/.local/share/eu4cjk/eu4cjk.log | grep -a "loaded pid\|sha256\|render:"
# 原生 Steam：
cat ~/.local/share/eu4cjk/eu4cjk.log 2>/dev/null || cat /tmp/eu4cjk.log | grep -a "loaded pid\|sha256\|render:"
```

正常应看到三行关键输出：

```
[eu4cjk 1.1.0-M6 | build ...] loaded pid=...
[eu4cjk] sha256 self-check: MATCH (expect Steam buildid 15918133)
[eu4cjk] render: mode=full installed
```

游戏正常退出时日志会追加统计行（`render final stats` / `savefix final`）。

---

## 故障排查

| 症状 | 原因与处理 |
|---|---|
| 游戏正常但完全没有中文 | ① 启动选项没生效：检查是否完整字面路径、无空格、`%command%` 保留；② 中文 mod 的 playset 没选中；③ 看日志有没有 `loaded pid` 行——没有就是 .so 根本没加载（路径问题） |
| 日志有 `sha256 self-check: MISMATCH` | 游戏版本不是 v1.37.5。等本补丁跟进或退回游戏版本 |
| 日志有 `loaded pid` 但游戏内仍乱码/方块 | 中文 mod 未启用（playset 没选），或 mod 版本与游戏版本不匹配 |
| 游戏崩溃 | 本补丁内置崩溃处理器会写日志；请携带日志（路径见第 5 步）到 GitHub Issues 报告 |
| 存档列表中文名乱码但游戏内正常 | 应该不会发生——存档名是补丁的一部分功能；若出现请带日志报 Issue |
| 看不到日志文件 | Flatpak 版在 `~/.var/app/com.valvesoftware.Steam/.local/share/eu4cjk/`；原生版优先 `~/.local/share/eu4cjk/`，兜底 `/tmp/eu4cjk.log` |

## 卸载

1. Steam 启动选项清空（唯一的"安装"痕迹）
2. （可选）删除 .so：Flatpak 版 `rm -rf ~/.var/app/com.valvesoftware.Steam/.local/share/eu4cjk`
3. 游戏、mod、存档完全不受影响

## 兼容性说明

- 补丁按**字节特征**逐点校验引擎代码后才会挂钩；游戏版本不符时
  全部跳过，游戏按原版运行，**不会崩溃**
- 仅支持 x86-64 Linux（EU4 原生版本身只有 x86-64）
- 与 Vulkan/Proton 无关：本补丁用于 Linux **原生**客户端，
  Windows 客户端请使用上游 [EU4dll](https://github.com/matanki-saito/EU4dll)

## 日韩语用户

补丁核心（转义协议、字形门）与上游 EU4dll 完全一致且语言无关。
日语用户订阅日语多字节 mod（上游项目的配套 mod）理论上即可使用，
未实测；韩语取决于社区是否存在对应多字节 mod。欢迎实测反馈。
