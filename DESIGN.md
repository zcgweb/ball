# ball 设计思路

## 1. 目标与约束

做一个常驻托盘的桌面小工具，让“当前有多忙”和“最近一段时间有多忙”同时可见，但不能占用多少屏幕、不能装一堆依赖、不能留下配置文件。

因此定下几条硬约束：

- **单文件绿色 exe**：静态链接，只有 Windows 系统 DLL，双击即用。
- **无配置文件**：所有偏好存注册表 `HKCU\Software\CpuBall`。
- **画面极简**：状态条上不写文字、数字和 `%`，指标与数值只在悬停时显示。
- **小窗口可缩放**：默认只显示六条状态条，滚轮可整体放大缩小。
- **可见性优先**：窗口跑出屏幕、Explorer 重启、多屏插拔后都要能找回来。

## 2. 总体结构

代码在 `src/ball.cpp` 里按职责分成几层：

```
采样层  →  状态层  →  渲染层  →  交互层  →  持久化层
 CPU        g_cpu      RenderPanel   托盘/右键   注册表读写
 内存       g_hist[]   UpdateLayered 滚轮/拖动   Load/Save...
 磁盘/网络  g_zoomIdx  StretchBlt    tooltip    ResetAllPreferences
 GPU        g_viewMode
```

- 采样层每 250 ms 更新一次。
- 状态层保存实时 EMA 值和 240 点历史。
- 渲染层把逻辑帧画到 DIB，再按缩放拉伸，最后交给分层窗口。
- 交互层负责托盘、右键菜单、滚轮、拖动、悬停气泡。
- 持久化层只读写 `REG_DWORD`，失败时静默回退默认值。

## 3. 双时间尺度：球看现在，条看过去

这是整个设计的核心。

| 元素 | 数据来源 | 时间尺度 | 用途 |
|---|---|---|---|
| 球 | EMA 平滑值 `g_cpuS/g_memS/...` | 约秒级 | 此刻忙不忙 |
| 状态条 | 未平滑瞬时值写入 `g_hist[6][240]` | 60 秒 | 最近发生了什么 |

- 球用 EMA（`kEmaA=0.35`）避免数字跳动刺眼。
- 状态条故意不平滑，能真实反映短时峰值。
- 网速没有固定的“满载”值，因此先用对数映射到 0–100 的“繁忙度”画条，同时把真实 B/s 存在 `g_histBps` 里，只在 tooltip 里显示。
- GPU 用 PDH 的 GPU Engine 计数器汇总得到使用率。

## 4. 渲染：分层窗口 + DIB + 整帧拉伸

窗口是 `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_POPUP`：无边框、不入任务栏、透明背景。绘制流程：

1. 在 `kW×kH`（126×326）的 DIB 上画完整的球区和条区。
2. 按当前缩放档位用 `StretchBlt(..., COLORONCOLOR)` 整帧拉伸到目标 DIB。
   - `COLORONCOLOR` 是最近邻，不会插值破坏 32bpp 预乘 alpha。
3. `UpdateLayeredWindow()` 用 `ULW_ALPHA` 把结果贴到屏幕。
4. 显示模式只决定从拉伸帧的哪一段取样：
   - 球 + 条：源 y=0，高 `kH`
   - 只球：源 y=0，高 `kBallAreaH`
   - 只条：源 y=`kBandTop`，高 `kH-kBandTop`

这样切换模式不需要重建 DIB，历史曲线也不会因为模式切换而中断。

### 缩放

逻辑帧尺寸恒定，所有几何（球半径、条高度、tooltip 命中区）都先按逻辑坐标算，再乘 `Zoom()`。滚轮缩放以鼠标下的窗内点为锚点：

```
内容坐标 = (鼠标屏幕坐标 - 窗口原点) / 旧缩放
新窗口原点 = 鼠标屏幕坐标 - 内容坐标 * 新缩放
```

否则连续缩小时鼠标会滑出已经变小的透明窗口，后续滚轮事件收不到。菜单缩放则用左上角锚点，避免鼠标在菜单上把窗口甩走。

**双击居中放大**是缩放之上的一层临时视图状态：进入时记下当前 `g_zoomIdx` 与 `g_winX/g_winY`，把档位切到最大并把窗口放到当前显示器 `rcWork` 的正中央；再双击时原样还原。放大倍率不是直接用最大档的 3.0×，而是让 `Zoom()` 在 `g_centerExpanded` 时再乘 `kCenterFactor = 1.5`，得到 **4.5×**；滚轮档位仍止于 3.0×，所以这个额外 50% 只作用于双击、不会进入持久化档位。拉伸缓冲按 `kZoomMax × kCenterFactor = 4.5×` 一次性分配，避免放大后缓冲不足导致拉伸失真。窗口类注册了 `CS_DBLCLKS` 才会收到 `WM_LBUTTONDBLCLK`。双击处理的开头要主动清掉拖动标记并 `ReleaseCapture()`，否则双击序列末尾的 `WM_LBUTTONUP` 会把临时放大的坐标写进注册表。临时状态下滚轮和拖动也不写注册表，保证“退出后回到原位”和重启后的偏好一致。

## 5. 交互与 tooltip

- 托盘图标承担主要入口；`TaskbarCreated` 广播后重新 `NIM_ADD`，Explorer 重启也不会丢图标。
- `WM_DISPLAYCHANGE` / `WM_DPICHANGED` 触发可见性检查；窗口与所有显示器都无交集时自动拉回工作区右上角。
- tooltip 使用 comctl32 v5，必须把 `TOOLINFOW.cbSize` 设成 `TTTOOLINFOW_V1_SIZE`，否则 `TTM_ADDTOOLW` 静默失败。
- 文本走 `LPSTR_TEXTCALLBACK`，在 `TTN_GETDISPINFO` 里动态拼指标名、数值和系统信息（`g_tipFull`，768 宽字符）。
- v5 会缓存每个 tool 首次显示的文本；切换语言时销毁并重建整个 tooltip 控件，只删/加工具会导致气泡不再弹出。

## 6. 窗口可见性与单实例

- 命名互斥量 `Local\CpuBall_SingleInstance_v1` 保证只有一个实例。
- 二次启动用 `SendMessageTimeout(kQueryLangMsg)` 询问旧实例语言，再 `PostMessage(kAlreadyMsg)` 让旧实例拉回窗口，弹窗文案因此跟随旧实例语言。
- `EnsureWindowVisible()` 用 `MonitorFromRect(..., MONITOR_DEFAULTTONULL)` 判断窗口是否完全在屏幕外；是则调用 `ResetWindowPosition()`。
- 读取偏好坐标时同样做可见性校验；非法或屏幕外坐标回退默认右上角并写回，避免每次启动都重复校验。

## 7. 持久化

所有偏好都在 `HKCU\Software\CpuBall`：

| 键 | 类型 | 取值 |
|---|---|---|
| `ViewMode` | REG_DWORD | 0=球+条，1=只球，2=只条 |
| `Lang` | REG_DWORD | 0=中文，1=English |
| `Zoom` | REG_DWORD | `kZoomLevels` 下标 0..7 |
| `ShowText` | REG_DWORD | 0/1 |
| `Topmost` | REG_DWORD | 0/1 |
| `NetPill` | REG_DWORD | 0=圆形，1=圆角 |
| `WinX` / `WinY` | REG_DWORD | 有符号坐标按 DWORD 原样存 |

- 读取发生在互斥量检查之后、窗口尺寸计算之前。
- 写入发生在菜单切换、`ApplyZoom()`、拖动结束、重置位置等时刻；`WM_MOUSEMOVE` 里不写，避免高频 I/O。
- “重置所有偏好”会 `RegDeleteValueW` 清掉全部 8 个值，并把运行时状态恢复默认；调用 `ResetWindowPosition(false)` 避免重定位时把刚删掉的坐标又写回去。

## 8. 中英文与 About

- 所有用户可见文案集中在 `UiText kZh / kEn`，通过 `T()` 取当前语言的一套。
- 托盘提示、tooltip 标题/正文、右键菜单、About、二次启动弹窗都走同一个语言开关。
- MessageBox 的按钮文字受系统 UI 语言控制，额外用同线程 CBT 钩子在 `HCBT_ACTIVATE` 时改成“确定 / OK”。
- About 显示版本 `1.0.0`、开发信息 `jxs using WorkBuddy`、编译日期；编译日期由 `__DATE__` 手工解析成 `YYYY-MM-DD`，避免静态链接 `sscanf` 带来的体积膨胀。

## 9. 构建与可复现

`build_ball.bat`：

1. `windres` 把 `ball.ico` 编进资源。
2. `g++ -pipe -Os -municode -mwindows -fno-exceptions -fno-rtti -c` 编译源码。
   - `-pipe` 是必须的：本机 MSYS2 binutils 2.47 的 `as.exe` 走临时文件时会段错误。
3. `g++ -s -static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp` 链接。
   - 去掉 PE 时间戳后，同一天同一源码重复编译得到相同 md5。
   - 若要跨日期一致，可传 `-DBALL_BUILD_DATE=L"YYYY-MM-DD"`。

发布版导入表共 17 项，全部为系统 DLL / UCRT API Set，没有第三方运行时。

## 10. 踩过的坑（供维护者参考）

- 分层窗口抓屏要用 `PrintWindow(hwnd, mem, 2)`，普通截图拿到的可能是旧帧。
- tooltip 文本缓冲最初只有 80 字符，英文加系统信息会被截断；已改为 `lpszText` 指向 768 字符静态缓冲，`TTM_SETMAXTIPWIDTH` 设 560。
- 跨进程直接读写 tooltip 指针会崩；文本必须由真实鼠标触发的 `TTN_GETDISPINFO` 填充。
- 右键在透明分层窗口上有时候弹不出菜单；诊断时给窗口 `PostMessage(WM_RBUTTONUP)` 更稳定。
- 通过 PowerShell 调用脚本时，双引号里的 `$VAR` 会被本地展开；涉及远端命令要用单引号或绝对路径（此条来自同项目的北超服务器操作经验，与本地构建无关）。

## 11. 文件对应关系

- `src/ball.cpp`：当前正式版源码。
- `src/ball.rc` / `src/ball.ico`：图标资源。
- `bin/ball.exe`：与源码对应的已编译发布版。
- `legacy/ball.py`：早期 Python 原型，保留作思路演进参考，不参与构建。
- `build_ball.bat`：从源码重建 `bin/ball.exe` 的脚本。
