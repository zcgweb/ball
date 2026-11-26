# ball 设计思路（Rust 重写版）

## 1. 目标与约束

做一个常驻托盘的桌面小工具，让“当前有多忙”和“最近一段时间有多忙”同时可见，但不能占用多少屏幕、不能装一堆依赖、不能留下配置文件。

因此定下几条硬约束：

- **单文件绿色 exe**：release 构建后只有 Windows 系统 DLL / UCRT API Set 导入，双击即用。
- **无配置文件**：所有偏好存注册表 `HKCU\Software\CpuBall`。
- **画面极简**：状态条上不写文字、数字和 `%`，指标与数值只在悬停时显示。
- **小窗口可缩放**：默认只显示六条状态条，滚轮可整体放大缩小。
- **可见性优先**：窗口跑出屏幕、Explorer 重启、多屏插拔后都要能找回来。
- **等价替代**：Rust 版要对齐旧 C++ 版的全部交互与持久化行为， Including 双击 4.5× 居中放大。

## 2. 总体结构

当前正式实现是单文件 `src/main.rs`，按职责分层：

```
采样层        →  状态层        →  渲染层        →  交互层        →  持久化层
Sampler::cpu     G::lamp[]      render_panel     wnd_proc        reg_get/reg_set
Sampler::mem     G::hist[]      UpdateLayered    托盘/右键       reset_all
Sampler::disk    G::zoom_idx    StretchBlt       tooltip         load_prefs
Sampler::net     G::view_mode   DIB 像素混合      滚轮/拖动
Sampler::gpu     G::center_exp  ...              双击/菜单
```

- 定时器每 250 ms 触发一次采样，调用 `record_history()` 写入 240 点历史。
- 状态层用 `G`（`static mut GLOBAL: Option<G>`）集中保存实时 EMA、历史环形缓冲、缩放档位、显示模式、坐标和 UI 控件句柄。
- 渲染层先在 126×326 的逻辑 DIB 上画完整帧，再按 `Zoom()` 拉伸，最后 `UpdateLayeredWindow()` 贴图。
- 交互层负责托盘、右键菜单、滚轮、拖动、悬停气泡、双击放大。
- 持久化层只读写 `REG_DWORD`，失败时静默回退默认值。

## 3. 双时间尺度：球看现在，条看过去

这是整个设计的核心。

| 元素 | 数据来源 | 时间尺度 | 用途 |
|---|---|---|---|
| 球 | EMA 平滑值（`EMA_A = 0.35`） | 约秒级 | 此刻忙不忙 |
| 状态条 | 未平滑瞬时值写入 `G::hist[6][240]` | 60 秒 | 最近发生了什么 |

- 球用 EMA 避免数字跳动刺眼。
- 状态条故意不平滑，能真实反映短时峰值。
- 网速没有固定的“满载”值，因此先用对数映射到 0–100 的“繁忙度”画条，同时把真实 B/s 存在历史里，只在 tooltip 里显示。
- 网速采样细节（`GetIfTable`）：只统计 `dwOperStatus == IF_OPER_STATUS_OPERATIONAL(5)` 的已连接接口（曾误写为 6 导致网速恒为 0），排除回环（type 24）；同一块网卡可能被过滤驱动镜像成多行（`dwIndex` 不同但 `dwInOctets/dwOutOctets` 完全相同），按计数对去重只统计一次，否则网速会被放大数倍。
- GPU 用 PDH 的 `GPU Engine(*)\Utilization Percentage` 计数器汇总得到使用率。
- `fmt_rate()` 把 B/s 格式化为 B/s、KB/s 或 MB/s。

## 4. 渲染：分层窗口 + DIB + 整帧拉伸

窗口是 `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_POPUP`：无边框、不入任务栏、透明背景。绘制流程：

1. 在 `WIDTH×HEIGHT`（126×326）的 DIB 上画完整的球区和条区。
2. `render_panel()` 按当前缩放档位用 `StretchBlt(..., COLORONCOLOR)` 整帧拉伸到目标 DIB。最近邻采样不会插值破坏 32bpp 预乘 alpha。
3. `UpdateLayeredWindow()` 用 `ULW_ALPHA` 把结果贴到屏幕。
4. 显示模式只决定从拉伸帧的哪一段取样：
   - 球 + 条：源 y=0，高 `H`
   - 只球：源 y=0，高 `BALL_AREA_H`
   - 只条：源 y=`BAND_TOP`，高 `H - BAND_TOP`

这样切换模式不需要重建 DIB，历史曲线也不会因为模式切换而中断。

### 缩放

逻辑帧尺寸恒定，所有几何（球半径、条高度、tooltip 命中区）都先按逻辑坐标算，再乘 `Zoom()`。滚轮缩放以鼠标下的窗内点为锚点：

```
内容坐标 = (鼠标屏幕坐标 - 窗口原点) / 旧缩放
新窗口原点 = 鼠标屏幕坐标 - 内容坐标 * 新缩放
```

否则连续缩小时鼠标会滑出已经变小的透明窗口，后续滚轮事件收不到。菜单缩放则用左上角锚点，避免鼠标在菜单上把窗口甩走。

**双击居中放大**是缩放之上的一层临时视图状态，由 `toggle_center()` 实现：

- 进入时记下当前 `zoom_idx` 与 `win_x/win_y`，把档位切到最大并把窗口放到当前显示器工作区正中央。
- 倍率不是直接用最大档的 3.0×，而是让 `Zoom()` 在 `center_expanded` 时再乘 `CENTER_FACTOR = 1.5`，得到 **4.5×**。
- 滚轮档位仍止于 3.0×，所以这个额外 50% 只作用于双击、不会进入持久化档位。
- 拉伸缓冲按 `ZOOM_MAX × CENTER_FACTOR = 4.5×` 一次性分配，避免放大后缓冲不足。
- 窗口类注册 `CS_DBLCLKS` 才会收到 `WM_LBUTTONDBLCLK`。双击处理开头主动清掉拖动标记并释放鼠标捕获，否则双击序列末尾的 `WM_LBUTTONUP` 会把临时放大的坐标写进注册表。
- 临时状态下滚轮和拖动也不写注册表，保证退出后回到原位、重启后的偏好一致。

## 5. 交互与 tooltip

- 托盘图标承担主要入口；`TaskbarCreated` 广播后重新 `NIM_ADD`，Explorer 重启也不会丢图标。
- `WM_DISPLAYCHANGE` / `WM_DPICHANGED` 触发 `ensure_visible()`；窗口与所有显示器都无交集时自动拉回工作区右上角。
- tooltip 使用 comctl32 v5，必须把 `TTTOOLINFOW.cbSize` 设成 `TTTOOLINFOW_V1_SIZE`，否则 `TTM_ADDTOOLW` 静默失败。
- 文本走回调（`lpszText` 为 `-1isize as *mut u16`），在 `TTN_GETDISPINFO` 里由 `fill_tip()` 动态拼指标名、数值和系统信息。
- v5 会缓存每个 tool 首次显示的文本；切换语言时销毁并重建整个 tooltip 控件，只删/加工具会导致气泡不再弹出。

## 6. 窗口可见性与单实例

- 命名互斥量 `Local\CpuBall_SingleInstance_v1` 保证只有一个实例。
- 二次启动用 `QUERY_LANG_MSG` 询问旧实例语言，再 `ALREADY_MSG` 让旧实例拉回窗口，弹窗文案因此跟随旧实例语言。
- `ensure_visible()` 用 `MonitorFromRect(..., MONITOR_DEFAULTTONULL)` 判断窗口是否完全在屏幕外；是则调用 `reset_pos()`。
- 读取偏好坐标时同样做可见性校验；非法或屏幕外坐标回退默认右上角并写回，避免每次启动都重复校验。

## 7. 持久化

所有偏好都在 `HKCU\Software\CpuBall`：

| 键 | 类型 | 取值 |
|---|---|---|
| `ViewMode` | REG_DWORD | 0=球+条，1=只球，2=只条 |
| `Lang` | REG_DWORD | 0=中文，1=English |
| `Zoom` | REG_DWORD | `ZOOM_LEVELS` 下标 0..7 |
| `ShowText` | REG_DWORD | 0/1 |
| `Topmost` | REG_DWORD | 0/1 |
| `NetPill` | REG_DWORD | 0=圆形，1=圆角 |
| `WinX` / `WinY` | REG_DWORD | 有符号坐标按 DWORD 原样存 |

- 读取发生在互斥量检查之后、窗口尺寸计算之前，由 `load_prefs()` 完成。
- 写入发生在菜单切换、`apply_zoom()`、拖动结束、重置位置等时刻；`WM_MOUSEMOVE` 里不写，避免高频 I/O。
- `reset_all()` 删除全部 8 个值并把运行时状态恢复默认；调用 `reset_pos(gp, false)` 避免重定位时把刚删掉的坐标又写回去。

## 8. 中英文与 About

- 所有用户可见文案集中在 `Ui` 结构体的 `UI_ZH` / `UI_EN` 常量，通过 `ui(lang)` 取当前语言的一套。
- 托盘提示、tooltip 标题/正文、右键菜单、About、二次启动弹窗都走同一个语言开关。
- MessageBox 的按钮文字受系统 UI 语言控制，额外用同线程 CBT hook（`about_cbt()`）在 `HCBT_ACTIVATE` 时改成“确定 / OK”。
- `fmt_about()` 显示版本 `1.0.0`、开发信息 `jxs using WorkBuddy`、构建说明 `Rust / 原生 Win32 GDI`、版权 `© 2026 jxs`；`build_date()` 固定返回 `2026-11-26`，便于跨日期复现。

## 9. 构建与可复现

`build_rust.bat`：

1. 用 MSYS2 UCRT64 的 `windres` 在 `src\` 目录编译 `ball.rc`，输出 `target\ball_res.o`。资源脚本把 `ball.ico` 声明为 `IDI_APP ICON "ball.ico"`（`IDI_APP = 101`）。
2. 设置 `RUSTFLAGS=-C link-arg=<repo>/target/ball_res.o`，把这个 COFF 资源对象交给 rustc 链接器。
3. `cargo build --release`。
4. 把 `target\release\ball.exe` 复制到 `bin\ball.exe`。

`src/main.rs` 在注册窗口类和初始化托盘时都用 `LoadIconW(hinstance, IDI_APP)` 加载该资源；资源缺失时才回退到系统默认图标或动态绘制图标。

`Cargo.toml` 的 release profile：

```toml
opt-level = "z"
lto = true
panic = "abort"
strip = true
codegen-units = 1
```

依赖只有 `windows-sys 0.61`（启用所需 Win32 feature）；`Cargo.lock` 锁定具体版本。已在 `rustc 1.100.0 (x86_64-pc-windows-gnu)` 上验证，产物是 GUI 子系统 PE32+ exe，导入表 20 项，全部为 `KERNEL32.dll`、`user32.dll`、`gdi32.dll`、`advapi32.dll`、`shell32.dll`、`comctl32.dll`、`pdh.dll`、`iphlpapi.dll`、`ntdll.dll` 和 `api-ms-win-*` UCRT API Set，没有第三方 DLL。

当前发布版：318,464 B，md5 `1888a594d361e5b2d53c6a49475cc394`（已内嵌 `ball.ico`）。本机 WorkBuddy 工具进程里直接跑 cargo 可能触发子进程创建异常，构建应放在独立终端执行。

### 单实例与句柄管理注意事项（2026-11-26 审计修复）

移植 C++ 到 Rust 时出现过一批「条件写反」错误，集中在空句柄判断上（`is_null()` 少取反），已全部修复：

- 单实例：`CreateMutexW` 在互斥体已存在时**仍返回有效句柄**，必须用 `GetLastError()==ERROR_ALREADY_EXISTS` 判断，不能看句柄是否为空；对旧窗口的通知（询问语言 + `ALREADY_MSG` 拉回）也因 `FindWindowW` 结果判断写反而从未执行。
- 句柄泄漏：托盘图标每秒更新，旧 `HICON` 因条件写反从未销毁（实测 20 秒泄漏 20 个句柄，修复后零增长）；退出路径的 `DestroyIcon`、CBT hook 的 `UnhookWindowsHookEx` 同理。
- 失效路径：`LoadLibraryW` 成功才应 `GetProcAddress`（版本号回退、DPI awareness 两处因条件写反变成对空模块取地址，永远失败）；`IDM_EXIT` 的 `PostMessageW(WM_CLOSE)` 因 `hwnd.is_null()` 写反从未发出；About 对话框 OK 按钮改文字、PDH 预热采集同理。
- 防御性调整：磁盘计数器添加失败时仍执行 `PdhCollectQueryData`，保证同查询下的 GPU 计数器有数据。

### 第二轮独立审计（2026-11-26，非 `is_null()` 类别）

换类别复查后又修复 3 处（均为移植时丢失的安全属性或潜在 UB）：

- tooltip 缓冲截断安全：`fill_tip` 的 `tip_full[768]` 缓冲复制后没有保证 NUL 结尾，超长截断时 tooltip 会读到缓冲区外脏内存；C++ 版用 `swprintf(…,768,…)` 天然安全。已改为复制后强制补 NUL。
- PDH 缓冲对齐：`gpu_buf` 原为 `Vec<u8>`，被强转成要求 8 字节对齐的 `PDH_FMT_COUNTERVALUE_ITEM_W` 数组（内含指针/double），属对齐 UB（Windows 堆实践 16 对齐所以未崩）。已改用 `Vec<u64>` 保证对齐。
- 编译日期：C++ 用 `__DATE__` 编译期生成，Rust 版曾硬编码日期、重建后会过期。新增 `build.rs`（纯 std）编译期生成 `BUILD_DATE` 环境变量供 About 显示。

核对过并确认为刻意设计（非缺陷）：`ApplyZoom` 无条件保存窗口位置（C++ 相同，注释写明「锚点缩放会移动窗口，记下最终位置」）、拖动/定时器/看门狗顺序、`WM_DPICHANGED` 仅拉回可见区、退出路径不还原 GDI 选定对象（进程退出由系统回收）。

### 第三轮修复：网速"超光速"假值（2026-11-26，两阶段）

现象一：tooltip 平均/峰值出现 `17592186044421.20 MB/s`（≈2^44 MB/s = 2^64 B/s）级的天文数字，而"当前"值正常。
根因一：`net()` 把所有「已连接、非回环」行的 32 位字节计数器求和进 u64，再用 `wrapping_sub(prev)` 求增量。当**行集合缩小**（网卡下线、镜像行消失）时，无符号减法回绕出接近 2^64 的值，转 f64 后是约 1.8×10^19 的巨大正数，`.max(0.0)` 拦不住，被写入 60 秒历史污染平均/峰值。C++ 原版同病：`(double)(inB - g_prevIn)` 回绕后 `if (bpsIn < 0)` 是死代码。

现象二（第一轮修复后的复测）：平均 816 MB/s、峰值 14419.57 MB/s（≈14 GB/s）、当前 0 B/s。
根因二：**行集合扩大**（网卡上线、镜像行出现、去重集合成员变化）时，增量一次性混入新加入行的**累计字节计数**——单个 32 位计数器最多 ~4.29 GB，除以 0.25 s 采样间隔即 ~17 GB/s 的假峰值，能通过按"每采样间隔增量 < 2^40"设的闸门。

最终修复：采样器记录**参与求和的行身份集合**（排序后的 `dwIndex` 列表 `prev_ids`），与上次不同即重新播种（更新 prev 与 last_net_ms，本秒记 0），从源头同时消除"行移除回绕"与"行加入累计计数跳变"两类假值；行集合不变时 32 位计数器正常回绕仍由 `wrapping_sub` 正确处理，原 2^40 增量闸门保留作兜底。播种/播种失败路径统一更新 `last_net_ms`，避免下个采样 dt 跨两个间隔。

### 第四轮重构：改用 GetIfTable2（2026-11-26，网速不准的最终解法）

实测（diag_net.py）发现第三轮的"行集合抖动"根源是**按计数对去重本身不可靠**：本机 MediaTek Wi-Fi 物理网卡（idx 15）与其 5 个 NDIS 过滤镜像行（idx 31-35，WFP/QoS/Virtual WiFi Filter）报告**完全相同的 32 位计数**，绝大多数采样时刻相同→去重只留一行；偶尔镜像行先更新 1 个包→计数出现微小差异→两行都保留→求和翻倍（39 亿→78 亿字节）→15GB/s 假峰值和下一刻的回绕 GATE/0。稳定时真实速率其实只有 0.03~4 MB/s。前两轮的钳位/集合播种只是给错误设计打补丁。

最终修复：彻底弃用 `GetIfTable`/`MIB_IFROW`（32 位计数 + 无过滤标志），改用 **`GetIfTable2`/`MIB_IF_TABLE2`**：
- `MIB_IF_ROW2.InOctets/OutOctets` 是 **64 位**计数，不会 32 位回绕；
- `InterfaceAndOperStatusFlags._bitfield & 0x02`（FilterInterface）直接剔除 NDIS 过滤驱动镜像行，不再按计数猜测去重；
- `OperStatus` 用新枚举 `IfOperStatusUp=1`（旧 MIB_IFROW 是 `OPERATIONAL=5`，切勿混用）；
- 用 `FreeMibTable` 释放 API 分配的表；
- 保留行集合（`InterfaceIndex` 排序）变化重新播种与 2^40 增量闸门作兜底。
- `Cargo.toml` 增加 `Win32_NetworkManagement_Ndis` feature。

实测验证（diag_net2.py 连续 40 次 × 250ms）：行集合恒为同一组 9 行真实网卡，速率平稳 0.03~0.10 MB/s，无假峰值。

## 10. Rust FFI 踩过的坑（供维护者参考）

- `windows-sys 0.61` 中 `HWND/HDC/HKEY/HICON/HFONT/PDH_HQUERY` 等句柄都是 `*mut c_void`，不能再和整数 `0` 比较或赋值，统一用 `null_mut()` / `.is_null()`。
- `TOOLINFOW` 不存在，要用 `TTTOOLINFOW`；回调文本常量 `LPSTR_TEXTCALLBACKW` 也不存在，改用 `-1isize as *mut u16`。
- `RegCreateKeyExW` 需要 `Win32_Security` feature；`GetComputerNameW` / `SystemTimeToFileTime` 需要 `Win32_System_WindowsProgramming` / `Win32_System_Time`；`SetCapture` / `ReleaseCapture` 需要 `Win32_UI_Input_KeyboardAndMouse`。
- `MIB_IFTABLE.table` 在 Rust 中声明为 `[MIB_IFROW; 1]` 柔性数组，不能用边界检查下标访问第 1 项以后的元素，必须用 `(*table).table.as_ptr().add(i)` 指针偏移。曾因此启动即 panic：`index out of bounds: the len is 1 but the index is 1`。
- `GetMonitorInfoW` 接受 `*mut MONITORINFO`，不要用 `MONITORINFOEXW` 的字段布局。
- `GetWindowLongPtrW` 返回 `isize`，`WS_EX_*` 常量要 `as isize` 后再参与位运算。
- 分层窗口抓屏用 `PrintWindow` 也可能得到白图；用同样方法抓旧 C++ 版也是白图，说明是抓图方法限制，不应据此判定渲染失败。

## 11. 验证记录

- 启动稳定，无 stderr 输出。
- 双击放大：窗口 `101×112 → 454×504 → 101×112`，比例 `4.50 / 4.50`，位置精确还原；`Zoom/WinX/WinY/ViewMode` 注册表值前后不变。
- 偏好写入：`Lang=1`、`Zoom=3`、`ShowText=1`、`WinX=1770`、`WinY=24`；重启后窗口尺寸 `126×140`（Zoom=3 档），语言查询返回 `1`。
- 菜单命令冒烟测试：命令 `2001..2015`（排除会弹模态框的 About 和 Exit）逐个发送后窗口仍存活，输出 `ALL_MENU_OK`，stderr 为空。
- 导入表与子系统：PE32+，Subsystem=2（GUI），20 个导入项均为系统 DLL / UCRT API Set。
- 图标资源：PE 资源目录含 `RT_ICON` ids `[1..7]` 与 `RT_GROUP_ICON` id `101`；`ExtractIconExW` 可提取图标；运行时窗口类 `HICON/HICONSM` 均非空。

## 12. 文件对应关系

- `src/main.rs`：当前正式版 Rust 源码。
- `Cargo.toml` / `Cargo.lock`：Rust 工程与依赖锁定。
- `build.rs`：构建脚本，编译期生成 `BUILD_DATE` 环境变量（等价 C++ 的 `__DATE__`），供 About 窗口显示真实编译日期。
- `build_rust.bat`：当前发布构建脚本。
- `bin/ball.exe`：与 Rust 源码对应的已编译发布版。
- `src/ball.cpp`、`build_ball.bat`：旧 C++ 实现与构建脚本，保留作历史对照，不参与当前发布构建。
- `src/ball.rc`、`src/ball.ico`：当前 Rust 版复用的图标资源脚本与图标文件，由 `build_rust.bat` 编译并链入 exe。
- `legacy/ball.py`：早期 Python 原型，保留作思路演进参考，不参与构建。
