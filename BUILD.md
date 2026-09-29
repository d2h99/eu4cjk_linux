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

```bash
git clone https://github.com/d2h99/eu4cjk.git
cd eu4cjk
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

产物：`build/libeu4cjk.so`（约 1.2 MB，含调试符号）。

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
