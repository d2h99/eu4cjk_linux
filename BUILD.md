# 从源码构建（推荐）

推荐所有用户自行编译：可以审计源码，并保证编译器环境与系统一致。
整个流程两条命令。

## 依赖

| 工具 | 版本 |
|---|---|
| gcc 或 clang | ≥ 9（需 C++17） |
| CMake | ≥ 3.16 |

Ubuntu/Debian：`sudo apt install build-essential cmake`

> 产物 `libeu4cjk.so` 仅依赖 libc（libstdc++ 静态链接），
> 在任意 glibc x86-64 发行版可用。

## 构建步骤

推荐命令（与 Releases 发布件同配置）：

```bash
git clone https://github.com/d2h99/eu4cjk_linux.git
cd eu4cjk_linux
cmake -B build
cmake --build build
cmake --build build --target eu4cjk_release
```

- `build/libeu4cjk.so`：含符号表产物（开发/调试用）
- `release/libeu4cjk.so`：**发布件**（约 380 KB）。`eu4cjk_release` 目标用
  `objcopy --strip-debug` 仅剥离调试/符号段，运行行为与剥离前完全一致；
  符号保留在 `build/libeu4cjk.so.debug`，供离线 backtrace 符号化。

### 优化等级与产物体积（实测）

| `CMAKE_BUILD_TYPE` | 优化flag | 含符号产物 | strip 后发布件 |
|---|---|---|---|
| 不填（默认，**推荐**） | `-O0` | ~1.16 MB | ~380 KB |
| `Release` | `-O3` | ~385 KB | ~384 KB |
| `RelWithDebInfo` | `-O2 -g` | ~1.16 MB | ~380 KB |
| `MinSizeRel` | `-Os` | ~369 KB | ~368 KB |

用法示例：`cmake -B build -DCMAKE_BUILD_TYPE=Release`（其余步骤相同）。

**为什么教程/自构建产物约 1.2 MB，而 Releases 只有 ~380 KB？**
差异来自**调试/符号段**而非功能或优化等级：含符号表的构建（以及
`-g` 的调试信息）占大头，`eu4cjk_release` 用 objcopy 剥离后即发布件体积，
两者运行行为逐位一致。

**为什么发布件用默认（-O0）而非 -O3？** 本补丁的每帧热路径（取字/测量/
折行/曲线 stub）全部是**手写汇编**，不受 `-O` 影响；C++ 只出现在安装期与
低频路径（字体表查表、日志），-O0 与 -O3 的稳态差异不可测。同时 -O3 构建
在冒烟中观察到一次加载期偶发崩溃（复测未复现、根因未定位，见仓库外开发
记录），而默认构建有多轮零崩溃记录与用户验收。故发布件暂用默认优化等级——
**发布件的小体积来自 strip，与优化等级无关**。待 -O3 问题定位后可切换。

## 运行自测

仓库内置 198 项离线自测（覆盖转义协议、stub 生成、测量、折行等
核心逻辑，不需要游戏本体）：

```bash
ctest --test-dir build --output-on-failure
```

全部 PASS 即可放心使用。

## 生成发布件（可选）

日常使用直接拿 `build/libeu4cjk.so` 即可。若想要与官方发布一致的
精简版（剥离调试段，运行时行为完全相同）：

```bash
cmake --build build --target eu4cjk_release   # 见 CMakeLists 的 release 目标
```

或手动：

```bash
objcopy --only-keep-debug build/libeu4cjk.so libeu4cjk.so.debug
objcopy --strip-debug build/libeu4cjk.so libeu4cjk.so
```

## 后续安装

按 [INSTALL.md](INSTALL.md) 第 3 步起操作。
