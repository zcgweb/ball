# ball — Windows 桌面负载监视器（Rust 版）

一个零依赖、单文件、可直接运行的 Windows 桌面小工具：六个实时负载球显示当前 CPU / 内存 / 磁盘 / 上传 / 下载 / GPU；下方六条历史状态条显示最近 60 秒趋势。球是实时值，条是历史值。当前正式版由 **Rust / 原生 Win32 GDI** 重写实现，功能与旧 C++ 版对齐。

## 功能

- **六个实时负载球**：CPU、内存、磁盘、上传、下载、GPU，平滑后按绿 → 黄 → 橙 → 红映射忙碌程度。
- **六条 60 秒历史条**：250 ms 采样、240 点环形历史；纯趋势显示，不在画面上写标签、数字或 `%`。
- **悬停信息**：球和状态条悬停时显示指标名、实时值 / 60 秒均值 / 峰值，以及计算机名、操作系统版本、开机时间与已运行时长。
- **右键菜单**：球 + 条 / 只球 / 只条；显示百分比；始终置顶；网速球圆形 / 圆角；重置窗口位置；**重置所有偏好**；中英文切换；关于；退出。
- **滚轮缩放**：整面板 50% – 300%，以鼠标位置为锚点；菜单也提供放大 / 缩小 / 重置入口。
- **双击放大**：在球或状态条上双击，面板立即放大到 **4.5×**（滚轮最大档 3.0× 再放大 50%）并居中到当前屏幕；再双击一次，精确还原到双击前的位置和大小。该状态是临时的，不写入偏好。
- **偏好持久化**：显示模式、语言、缩放、百分比、置顶、网速形状、窗口位置全部写入注册表，重启后沿用。
- **单实例**：重复启动会拉回旧窗口并给出提示，不再出现“进程在跑但桌面找不到窗口”。
- **零第三方依赖**：Rust release 静态/剥离构建，导入表只有 Windows 系统 DLL 与 UCRT API Set，无第三方运行时 DLL。

## 快速开始

1. 下载 [`bin/ball.exe`](bin/ball.exe)，直接双击运行，无需安装。
2. 鼠标滚轮：缩放面板；拖动窗口：改变位置；双击面板：居中放大 / 再双击还原。
3. 右键托盘图标或窗口：切换显示模式、语言、缩放、开关项，或重置所有偏好。
4. 想恢复出厂状态：右键 → **重置所有偏好 / Reset all preferences**。

## 下载后提示“无法验证发布者 / 未知发布者”怎么办？

从网页下载 `ball.exe` 时，Windows 可能弹出“打开文件 – 安全警告：无法验证发布者”。这是因为本程序是**未签名的单文件 exe**，且从网络下载的文件会被系统标记来源（MOTW），并不代表有病毒。任选一种方式即可运行：

- **直接运行**：在弹出的对话框点 **运行(R)**。
- **永久解除**：右键 `ball.exe` → 属性 → 常规 → 若有“解除锁定 / Unblock”则勾选 → 应用。
- **命令行**：在 PowerShell 执行 `Unblock-File .\ball.exe`。

首次运行后 Windows 一般不再提示。若你所在环境由管理员强制拦截未签名程序，请联系管理员放行，或使用下方源码自行编译。

> 发布者若要彻底消除该提示，需用代码签名证书（OV/EV）对 `ball.exe` 签名并向 Microsoft SmartScreen 建立信誉；本项目默认不附带签名。

> 当前发布版本：`1.0.0`（Rust 重写版，内嵌图标）。编译日期：`2026-11-26`。二进制 md5：`74fbb1d3e649a989889704f177d7425f`（314,368 B）。

## 目录结构

```
.
├── README.md             本说明
├── DESIGN.md             设计思路与实现要点（Rust 版）
├── LICENSE               MIT 开源许可证
├── Cargo.toml            Rust 工程配置
├── Cargo.lock            依赖锁定（windows-sys 0.61）
├── build_rust.bat        Rust 发布构建脚本（当前版本）
├── bin/
│   └── ball.exe          发布用单文件可执行程序（Rust 版）
├── src/
│   ├── main.rs           当前正式源码：Rust / Win32 GDI
│   ├── ball.cpp          旧 C++ 实现，保留作对照
│   ├── ball.rc           图标资源脚本（Rust 版复用，IDI_APP=101）
│   └── ball.ico          程序图标，Rust exe 内嵌资源
├── legacy/
│   └── ball.py           早期 Python 原型，仅作历史参考
└── build_ball.bat        旧 C++ 版构建脚本，保留作历史参考
```

当前 `bin/ball.exe` 由 `src/main.rs` + `Cargo.toml` + `src/ball.rc` / `src/ball.ico` 构建，不是由旧 C++ 源码生成。旧 `build_ball.bat` 和 `src/ball.cpp` 仅用于对照早期实现，不再用于发布构建。

## 从源码构建（Rust）

需要 Rust stable（已在 `rustc 1.100.0 / x86_64-pc-windows-gnu` 上验证）。在仓库根目录执行：

```bat
build_rust.bat
```

脚本会先用 MSYS2 UCRT64 的 `windres` 把 `src/ball.rc` 编译成 COFF 资源对象，再通过 `RUSTFLAGS=-C link-arg=...` 链接进 release exe，最后复制到 `bin/ball.exe`：

```bat
windres -i ball.rc -o ..\target\ball_res.o   rem 工作目录为 src\
set RUSTFLAGS=%RUSTFLAGS% -C link-arg=<repo>/target/ball_res.o
cargo build --release
copy /Y target\release\ball.exe bin\ball.exe
```

`Cargo.toml` 的 release profile 使用 `opt-level = "z"`、`lto = true`、`panic = "abort"`、`strip = true`、`codegen-units = 1`，产物为单个 GUI 子系统 exe。本机的 WorkBuddy 工具进程里直接跑 cargo 可能触发子进程创建异常，构建请在独立终端中执行。

> 图标：`src/main.rs` 用 `IDI_APP = 101` 从自身资源加载图标，作为窗口类图标和托盘初始图标；托盘运行时仍会按负载动态绘制状态图标。

## 偏好存储位置

全部在 `HKCU\Software\CpuBall`，均为 `REG_DWORD`：

| 名称 | 含义 |
|---|---|
| `ViewMode` | 0=球+条，1=只球，2=只条 |
| `Lang` | 0=中文，1=English |
| `Zoom` | `ZOOM_LEVELS` 下标，0..7 |
| `ShowText` | 是否显示百分比 |
| `Topmost` | 是否始终置顶 |
| `NetPill` | 网速球 0=圆形，1=圆角药丸 |
| `WinX` / `WinY` | 窗口左上角坐标，按 DWORD 原样存有符号值 |

“重置所有偏好”会删除以上全部值并立即把运行中的界面恢复默认。

## 兼容性与说明

- 目标平台：Windows 10 / 11，x64。
- 无安装器、无配置文件、无后台服务；删除 `ball.exe` 即完成卸载（偏好注册表项会保留，可用菜单里的重置功能清除）。
- 进程使用命名互斥量 `Local\CpuBall_SingleInstance_v1` 保证单实例。
- About 中显示版本 `1.0.0`、开发信息 `jxs using WorkBuddy`、编译日期 `2026-11-26`。
- 本项目采用 [MIT 许可证](LICENSE)，允许自由使用、复制、修改、合并、发布、分发、再许可和/或销售，只需保留版权与许可声明。

更多实现细节见 [DESIGN.md](DESIGN.md)。
