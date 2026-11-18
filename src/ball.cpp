// cpu_ball.cpp — 竖排六球负载监视器（纯 Win32 GDI）
//
// 顶部=CPU  中排=内存/磁盘  底部小=上传/下载网速，四档颜色：绿→黄→橙→红
// 百分比默认关闭，托盘右键可打开；支持单实例、始终置顶开关
// 鼠标悬停窗口会弹出气泡提示各球含义
// 2026-11-15 修复：窗口跑出屏幕后自动找回、Explorer 重启后重建托盘图标、
//                  再次启动时把旧窗口叫回可见区域、托盘菜单增加"重置窗口位置"
// 2026-11-15 新增：底部三条历史状态条（CPU/内存/磁盘最近 60 秒），
//                  球体仍显示实时值，状态条显示时间序列（瞬时采样 + 四档颜色）
// 2026-11-15 调整：状态条改为纯图形趋势带——指标名与所有百分比都不落画面，
//                  只在鼠标悬停该条时的气泡里显示；整条高度都给时序柱
// 2026-11-15 扩充：状态条由 3 条增至 5 条，新增上传/下载网速；网速条的悬停
//                  气泡额外给出真实速率（B/s、KB/s、MB/s），柱高仍用对数繁忙度
// 2026-11-15 扩充：状态条增至 6 条，最下方新增 GPU 最近 60 秒趋势带
// 2026-11-15 新增：托盘右键可勾选"显示下方状态条"，关闭时窗口缩回球区高度（182）
// 2026-11-15 新增：显示模式三选一、中英文界面切换、关于/About 对话框
//                  （版本、开发信息、编译日期；日期取编译器 __DATE__）
// 2026-11-16 新增：球 / 状态条悬停气泡尾部附带计算机名、操作系统及版本、开机时间
//                  （气泡文本改走 lpszText 大缓冲，突破 szText 80 字符限制）
// 2026-11-16/17 新增：滚轮缩放整个面板；显示模式 / 语言 / 缩放 / 显示百分比 /
//                  置顶 / 网速形状 / 窗口位置全部写入 HKCU\Software\CpuBall，
//                  重启后沿用；窗口位置若落到屏幕外则回退默认右上角。
//
// 编译（MinGW）：
//   g++ -Os -municode -mwindows -s -fno-exceptions -fno-rtti ^
//       -o cpu_ball.exe cpu_ball.cpp ^
//       -lgdi32 -luser32 -lshell32 -lole32 -lpdh -lcomctl32 -liphlpapi

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   // 需要 GetTickCount64（Vista+）
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <pdh.h>
#include <iphlpapi.h>
#include <math.h>
#include <string.h>
#include <wchar.h>
#include <stddef.h>
#include <stdlib.h>

#pragma comment(lib, "advapi32.lib")   // 读取 HKLM 的 Windows 版本信息
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "iphlpapi.lib")

// 资源图标 ID（见 cpu_ball.rc / cpu_ball.ico，红绿蓝三球）
#define IDI_APP 101

#ifndef NIF_INFO
#define NIF_INFO 0x00000010
#endif
#ifndef NIIF_INFO
#define NIIF_INFO 0x00000001
#endif
#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef MONITOR_DEFAULTTONULL
#define MONITOR_DEFAULTTONULL 0x00000000
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// ---------------------------------------------------------------------------
// 配置（六球竖排：顶=CPU，中=内存/磁盘，底=上传/GPU/下载小球）
// CPU 灯约为中排灯的 1.3 倍，更突出；底部网速球更小
// ---------------------------------------------------------------------------
static const int  kLampDBase = 58;   // 中排灯直径（内存/磁盘）
static const int  kLampDCpu  = 75;   // 顶部 CPU 灯直径（58*1.3≈75）
static const int  kLampDNet  = 34;   // 底部小球直径（上传/GPU/下载）
static const int  kMargin    = 3;
static const int  kGapX      = 4;    // 同排两灯水平间距
// 六球竖排：顶部 CPU → 中排 内存/磁盘 → 底部 上传/GPU/下载
static const int  kW         = kMargin * 2 + kLampDBase * 2 + kGapX;
static const int  kBallAreaH = 182;      // 球区高度；隐藏状态条时窗口只保留这一段
// ---- 底部历史状态条（CPU / 内存 / 磁盘 / 上传 / 下载 / GPU，最近 60 秒）----
// kTimerMs=250 → 240 个采样点 = 60 秒；窗口宽度 126，状态条画 120 列（每列 2 个采样点）
static const int  kHistN     = 240;      // 历史采样点数（想改成 30 秒就写 120）
static const int  kBandTop   = kBallAreaH + 4;   // 球区下方留 4px
static const int  kBandH     = 20;       // 每条：整条 20px 时序柱（纯趋势带，无任何文字）
static const int  kBandGap   = 3;
static const int  kBandLabelH= 0;        // 标题行高度：0=不画文字、柱区占满 20px；改回 10 可留 10px 空白
static const int  kBandCount    = 6;     // 0 CPU / 1 内存 / 2 磁盘 / 3 上传 / 4 下载 / 5 GPU
static const int  kNetBand0     = 3;     // 网速状态条起始下标（同时是 g_histBps 的索引 0）
static const int  kNetBandCount = 2;     // 网速状态条数量（上传 + 下载）
static const int  kH         = kBandTop + kBandCount * (kBandH + kBandGap) + 2;   // = 326
// ---- 鼠标滚轮缩放：绘制仍按 kW×kH 的逻辑帧，输出时等比拉伸到目标尺寸 ----
// 档位固定，避免连续缩放把窗口拖到不可读；3.0x 是分配拉伸缓冲的上限。
static const float kZoomLevels[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f };
static const int   kZoomCount    = (int)(sizeof(kZoomLevels) / sizeof(kZoomLevels[0]));
static const int   kZoomDefault  = 2;      // 1.0x
static const float kZoomMax      = 3.0f;
static const UINT kTimerMs  = 250;
static const float kEmaA    = 0.35f;
static const UINT kTrayId   = 0x1C01;
static const UINT kTrayMsg  = WM_APP + 1;
static const UINT kAlreadyMsg = WM_APP + 3;
static const UINT kQueryLangMsg = WM_APP + 4;   // 二次启动：向旧实例询问当前界面语言
static const UINT kIconEvery = 4;
static const wchar_t kClassName[] = L"CpuBallWindowClass";
static const wchar_t kMutexName[] = L"Local\\CpuBall_SingleInstance_v1";
static const wchar_t kBallTitle[] = L"CPU Ball";
// 显示模式偏好存 HKCU\Software\CpuBall\ViewMode（REG_DWORD：0=球+条 / 1=只球 / 2=只条）。
// 不产生配置文件，exe 仍是单文件；读写失败静默回退默认值，不影响主功能。
static const wchar_t kRegPath[]  = L"Software\\CpuBall";
static const wchar_t kRegView[]  = L"ViewMode";
static const wchar_t kRegLang[]  = L"Lang";     // 0=中文 / 1=English
static const wchar_t kRegZoom[]  = L"Zoom";     // kZoomLevels 下标 0..kZoomCount-1
static const wchar_t kRegShowText[] = L"ShowText";  // 0/1 显示百分比
static const wchar_t kRegTopmost[]  = L"Topmost";   // 0/1 始终置顶
static const wchar_t kRegNetPill[]  = L"NetPill";   // 0=圆形 / 1=圆角药丸
static const wchar_t kRegWinX[]     = L"WinX";      // 窗口左上角 X（有符号坐标按 DWORD 原样存）
static const wchar_t kRegWinY[]     = L"WinY";

// 通用 DWORD 读写：读失败一律静默，保持调用方的默认值。
static bool RegGetDword(const wchar_t* name, DWORD* out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    DWORD v = 0, cb = sizeof(v), type = 0;
    const bool ok = RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
                    type == REG_DWORD && cb == sizeof(v);
    RegCloseKey(hk);
    if (ok) *out = v;
    return ok;
}
static void RegSetDword(const wchar_t* name, DWORD v) {
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, &disp) != ERROR_SUCCESS)
        return;
    RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
}

// ---------------------------------------------------------------------------
// 版本 / 构建信息（About 对话框）
// ---------------------------------------------------------------------------
static const wchar_t kVersion[] = L"1.0.0";
// __DATE__ 是编译器给的构建日期（形如 "Nov 15 2026"），统一格式化为 YYYY-MM-DD。
// 若要求跨日期重复编译也保持同一 md5，可用 -DBALL_BUILD_DATE=L"2026-11-15" 固定。
#ifdef BALL_BUILD_DATE
static const wchar_t* BuildDateIso() { return BALL_BUILD_DATE; }
#else
static const wchar_t* BuildDateIso() {
    static wchar_t iso[11] = L"";
    if (iso[0]) return iso;
    static const char* const kMon[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                          "Jul","Aug","Sep","Oct","Nov","Dec" };
    // 手工解析固定格式 "Mmm dd yyyy"（个位日期写作 "Mmm  5 yyyy"），
    // 不用 sscanf：静态链接 UCRT 的 scanf 系列会给 exe 增加约 40KB。
    const char* s = __DATE__;
    int mi = 0;
    for (int i = 0; i < 12; ++i)
        if (s[0] == kMon[i][0] && s[1] == kMon[i][1] && s[2] == kMon[i][2]) {
            mi = i + 1; break;
        }
    const int day  = (s[4] == ' ' ? 0 : (s[4] - '0') * 10) + (s[5] - '0');
    const int year = (s[7] - '0') * 1000 + (s[8] - '0') * 100
                   + (s[9] - '0') * 10  + (s[10] - '0');
    if (mi) swprintf(iso, 11, L"%04d-%02d-%02d", year, mi, day);
    else    swprintf(iso, 11, L"%ls", L"unknown");
    return iso;
}
#endif

#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif

// ---------------------------------------------------------------------------
// 界面文案：中文 / English 两套，菜单、托盘提示、悬停气泡、弹窗统一走 T()
// ---------------------------------------------------------------------------
enum Lang { kLangZh = 0, kLangEn = 1 };
struct UiText {
    const wchar_t* appTitle;        // 托盘提示前缀 / 气泡标题 / 弹窗标题
    const wchar_t* ballName[6];     // 六个球的指标名
    const wchar_t* bandName[6];     // 六条状态条的指标名
    const wchar_t* trayTipFmt;
    const wchar_t* menuPercent;
    const wchar_t* menuViewBoth;
    const wchar_t* menuViewBalls;
    const wchar_t* menuViewBands;
    const wchar_t* menuTopmost;
    const wchar_t* menuNetToRound;  // 当前是药丸 → 点击变圆形
    const wchar_t* menuNetToPill;   // 当前是圆形 → 点击变药丸
    const wchar_t* menuReset;
    const wchar_t* menuResetAll;    // 恢复所有偏好到出厂默认
    const wchar_t* menuZoomIn;      // 放大（滚轮上）
    const wchar_t* menuZoomOut;     // 缩小（滚轮下）
    const wchar_t* menuZoomReset;   // 重置为 100%
    const wchar_t* menuExit;
    const wchar_t* ballTipFmt;
    const wchar_t* bandTipFmt;
    const wchar_t* netTipFmt;
    const wchar_t* balloonInfo;     // 二次启动时托盘气泡正文
    const wchar_t* alreadyRunning;  // 二次启动时弹窗正文
    const wchar_t* menuAbout;       // 「关于 / About」菜单项
    const wchar_t* aboutTitle;      // About 对话框标题
    const wchar_t* aboutBodyFmt;    // About 正文；%ls=版本号，%ls=编译日期
};

static const UiText kZh = {
    L"负载灯",
    { L"CPU 负载", L"内存占用", L"磁盘繁忙", L"上传网速", L"下载网速", L"GPU 使用率" },
    { L"CPU", L"内存", L"磁盘", L"上传网速", L"下载网速", L"GPU" },
    L"%ls  CPU %.0f%%  内存 %.0f%%  磁盘 %.0f%%  GPU %.0f%%  上 %.0f%%  下 %.0f%%",
    L"显示百分比",
    L"显示球 + 状态条",
    L"只显示六个球",
    L"只显示六个状态条",
    L"始终置顶",
    L"网速形状→圆形",
    L"网速形状→圆角",
    L"重置窗口位置",
    L"重置所有偏好",
    L"放大（滚轮上）",
    L"缩小（滚轮下）",
    L"缩放 %.0f%%（点击重置）",
    L"退出",
    L"%ls\n当前 %.0f%%（实时值，绿→黄→橙→红，越忙越红）",
    L"%ls 最近 60 秒\n平均 %.0f%%  峰值 %.0f%%  当前 %.0f%%",
    L"%ls 最近 60 秒\n平均 %ls 峰值 %ls 当前 %ls\n繁忙度 %.0f%% / %.0f%% / %.0f%%",
    L"程序已在运行，窗口已恢复到屏幕可见区域。",
    L"负载监视器已在运行。\n\n已把窗口恢复到屏幕右上角；\n"
    L"右键托盘图标可切换显示模式、语言、百分比、置顶、网速形状、\n"
    L"重置位置、重置所有偏好、滚轮缩放、查看关于信息或退出。",
    L"关于",
    L"关于 负载灯",
    L"负载灯  Load Monitor\n"
    L"版本 %ls    编译日期 %ls\n"
    L"\n"
    L"开发：jxs using WorkBuddy\n"
    L"构建：C++ / 原生 Win32 GDI，MSYS2 UCRT64 GCC 静态编译\n"
    L"发布：单文件 EXE，零第三方依赖（仅调用 Windows 系统 DLL）\n"
    L"\n"
    L"功能：六个负载球实时显示 CPU / 内存 / 磁盘 / 上传 / 下载 / GPU，\n"
    L"      六条状态条记录最近 60 秒的运行趋势。\n"
    L"      滚轮或右键菜单可缩放整个面板（50%%~300%%）。\n"
    L"\n"
    L"© 2026 jxs 版权所有",
};

static const UiText kEn = {
    L"Load Monitor",
    { L"CPU Load", L"Memory Usage", L"Disk Activity", L"Upload", L"Download", L"GPU Usage" },
    { L"CPU", L"Memory", L"Disk", L"Upload", L"Download", L"GPU" },
    L"%ls  CPU %.0f%%  MEM %.0f%%  DISK %.0f%%  GPU %.0f%%  UP %.0f%%  DOWN %.0f%%",
    L"Show percentage",
    L"Balls + status bars",
    L"Balls only",
    L"Status bars only",
    L"Always on top",
    L"Net shape → Round",
    L"Net shape → Rounded",
    L"Reset window position",
    L"Reset all preferences",
    L"Zoom in (wheel up)",
    L"Zoom out (wheel down)",
    L"Zoom %.0f%% (click to reset)",
    L"Exit",
    L"%ls\nNow %.0f%% (live; green → red)",
    L"%ls 60s\nAvg/Pk/Now %.0f/%.0f/%.0f%%",
    L"%ls 60s\nAvg %ls Pk %ls Now %ls\nLoad %.0f/%.0f/%.0f",
    L"Already running. Window restored to the visible screen area.",
    L"Load Monitor is already running.\n\nThe window has been restored to the top-right corner.\n"
    L"Right-click the tray icon to switch view mode, language, percentage text, "
    L"always-on-top, net shape, reset position, reset all preferences, zoom with the wheel, "
    L"view About, or exit.",
    L"About",
    L"About Load Monitor",
    L"Load Monitor\n"
    L"Version %ls    Build date %ls\n"
    L"\n"
    L"Developed by jxs using WorkBuddy\n"
    L"Built with C++ / native Win32 GDI\n"
    L"MSYS2 UCRT64 GCC, static link\n"
    L"Single-file EXE, no third-party DLLs\n"
    L"\n"
    L"Six live lamps: CPU / Memory / Disk / Upload / Download / GPU\n"
    L"Six status bars: rolling 60-second history\n"
    L"Wheel or right-click menu zooms the panel (50%%-300%%)\n"
    L"\n"
    L"(c) 2026 jxs. All rights reserved",
};
static HWND g_hwndTip = nullptr;   // tooltip 控件（每球一个 tool）
static wchar_t g_tipFull[768];     // 悬停气泡全文缓冲（指标 + 系统信息）

enum {
    IDM_TOGGLE_TEXT = 2001,
    IDM_TOGGLE_TOP  = 2002,
    IDM_EXIT        = 2003,
    IDM_TOGGLE_NETSHAPE = 2004,
    IDM_RESET_POS   = 2005,
    // 显示模式（三选一，互斥）：球+状态条 / 只显示球 / 只显示状态条
    IDM_VIEW_BOTH   = 2006,
    IDM_VIEW_BALLS  = 2007,
    IDM_VIEW_BANDS  = 2008,
    // 语言（二选一，互斥）：菜单文字本身固定为「中文 / English」，两种语言下都看得懂
    IDM_LANG_ZH     = 2009,
    IDM_LANG_EN     = 2010,
    IDM_ABOUT       = 2011,
    // 缩放：滚轮为主，菜单提供无滚轮设备的入口
    IDM_ZOOM_IN     = 2012,
    IDM_ZOOM_OUT    = 2013,
    IDM_ZOOM_RESET  = 2014,
    IDM_RESET_ALL   = 2015,   // 一键清空所有持久化偏好，恢复出厂默认
};

static const BYTE kAlphaBody = 150;
static const BYTE kAlphaRing = 210;
static const BYTE kAlphaText = 235;

// ---------------------------------------------------------------------------
// 采样
// ---------------------------------------------------------------------------
static ULONGLONG g_prevIdle = 0, g_prevTotal = 0;
static bool g_cpuSeeded = false;

static float SampleCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.f;
    auto u64 = [](const FILETIME& ft) -> ULONGLONG {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const ULONGLONG idleU = u64(idle);
    const ULONGLONG total = u64(kernel) + u64(user);
    if (!g_cpuSeeded) {
        g_prevIdle = idleU;
        g_prevTotal = total;
        g_cpuSeeded = true;
        return 0.f;
    }
    const LONGLONG dIdle  = (LONGLONG)idleU - (LONGLONG)g_prevIdle;
    const LONGLONG dTotal = (LONGLONG)total - (LONGLONG)g_prevTotal;
    g_prevIdle = idleU;
    g_prevTotal = total;
    if (dTotal <= 0) return 0.f;
    LONGLONG busy = dTotal - dIdle;
    if (busy < 0) busy = 0;
    float pct = 100.f * (float)busy / (float)dTotal;
    if (pct < 0.f) pct = 0.f;
    if (pct > 100.f) pct = 100.f;
    return pct;
}

static float SampleMem() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0.f;
    float pct = (float)ms.dwMemoryLoad;
    if (pct < 0.f) pct = 0.f;
    if (pct > 100.f) pct = 100.f;
    return pct;
}

static PDH_HQUERY   g_pdhQuery = nullptr;
static PDH_HCOUNTER g_pdhDisk  = nullptr;
static bool g_diskIsBytes = false;

static void InitDiskCounter() {
    if (PdhOpenQueryW(nullptr, 0, &g_pdhQuery) != ERROR_SUCCESS) {
        g_pdhQuery = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &g_pdhDisk) == ERROR_SUCCESS) {
        g_diskIsBytes = false;
        return;
    }
    if (PdhAddEnglishCounterW(g_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Bytes/sec", 0, &g_pdhDisk) == ERROR_SUCCESS) {
        g_diskIsBytes = true;
        return;
    }
    g_pdhDisk = nullptr;
}

static float SampleDisk() {
    if (!g_pdhQuery || !g_pdhDisk) return 0.f;
    if (PdhCollectQueryData(g_pdhQuery) != ERROR_SUCCESS) return 0.f;
    PDH_FMT_COUNTERVALUE v{};
    if (PdhGetFormattedCounterValue(g_pdhDisk, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS)
        return 0.f;
    if (v.CStatus != ERROR_SUCCESS) return 0.f;
    double pct = v.doubleValue;
    if (g_diskIsBytes) {
        const double bytes = pct;
        if (bytes <= 1.0) pct = 0.0;
        else {
            // 约 0.01MB/s→0，1MB/s→25，10MB/s→50，100MB/s→75，1GB/s→100
            pct = 25.0 * (log10(bytes / (1024.0 * 1024.0)) + 2.0);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
    } else {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    return (float)pct;
}

// ---------------------------------------------------------------------------
// GPU 使用率：汇总所有 GPU Engine 实例的 Utilization Percentage（Win8+ 可用）
// 注意：通配符计数器需用 PdhGetFormattedCounterArrayW 取数组求和
// ---------------------------------------------------------------------------
static PDH_HCOUNTER g_pdhGpu = nullptr;

// MinGW pdh.h 兼容：类型名带下划线，PDH_MORE_DATA 需自行定义
#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA 0x800007D2L
#endif

static void InitGpuCounter() {
    if (!g_pdhQuery) return;
    if (PdhAddEnglishCounterW(g_pdhQuery,
            L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_pdhGpu) != ERROR_SUCCESS) {
        g_pdhGpu = nullptr;   // 无 GPU 计数器（旧系统/无驱动）→ 恒为 0
    }
}

static float SampleGpu() {
    if (!g_pdhGpu) return 0.f;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(g_pdhGpu, PDH_FMT_DOUBLE, &size, &count, nullptr)
            != PDH_MORE_DATA || size == 0) return 0.f;
    static BYTE* buf = nullptr;
    static DWORD bufSize = 0;
    if (size > bufSize) {
        free(buf);
        buf = (BYTE*)malloc(size);
        bufSize = size;
        if (!buf) { bufSize = 0; return 0.f; }
    }
    if (PdhGetFormattedCounterArrayW(g_pdhGpu, PDH_FMT_DOUBLE, &bufSize, &count,
            (PDH_FMT_COUNTERVALUE_ITEM_W*)buf) != ERROR_SUCCESS) return 0.f;
    double sum = 0.0;
    const PDH_FMT_COUNTERVALUE_ITEM_W* items = (const PDH_FMT_COUNTERVALUE_ITEM_W*)buf;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > 0.0)
            sum += items[i].FmtValue.doubleValue;
    }
    if (sum < 0.0) sum = 0.0;
    if (sum > 100.0) sum = 100.0;   // 多引擎求和可能略超 100，钳位
    return (float)sum;
}

// ---------------------------------------------------------------------------
// 网速采样：汇总所有“已连接、非回环”网卡的上/下行字节计数，换算为繁忙度
//   繁忙度映射（与磁盘字节模式类似，对数刻度）：
//     <1 KB/s → 0； 100 KB/s → 25； 1 MB/s → 50； 10 MB/s → 75； 100 MB/s → 100
// ---------------------------------------------------------------------------
static ULONGLONG g_prevIn = 0, g_prevOut = 0;
static ULONGLONG g_lastNetMs = 0;
static bool g_netSeeded = false;

static float NetBusy(float bytesPerSec) {
    if (bytesPerSec < 1024.f) return 0.f;          // < 1 KB/s 视为空闲
    double kb = bytesPerSec / 1024.0;
    double pct = 25.0 * (log10(kb) - 1.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (float)pct;
}

static void SampleNet(float* dl, float* ul, double* inBps, double* outBps) {
    // 用 GetIfTable（MIB_IFTABLE，XP 起可用）：部分 MinGW 头文件不导出 IfTable2
    // inBps/outBps 额外返回真实字节速率，供网速状态条的悬停气泡显示
    *inBps = *outBps = 0.0;
    MIB_IFTABLE* pTable = nullptr;
    ULONG sz = 0;
    if (GetIfTable(nullptr, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER || sz == 0) {
        *dl = *ul = 0.f; return;
    }
    pTable = (MIB_IFTABLE*)malloc(sz);
    if (!pTable || GetIfTable(pTable, &sz, FALSE) != NO_ERROR) {
        free(pTable); *dl = *ul = 0.f; return;
    }
    ULONGLONG inB = 0, outB = 0;
    for (DWORD i = 0; i < pTable->dwNumEntries; ++i) {
        const MIB_IFROW& r = pTable->table[i];   // MinGW 头文件成员为小写 table
        if (r.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;   // 已连接
        if (r.dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;          // 排除回环
        inB  += r.dwInOctets;
        outB += r.dwOutOctets;
    }
    free(pTable);

    const ULONGLONG now = GetTickCount64();
    if (!g_netSeeded) {
        g_prevIn = inB; g_prevOut = outB; g_lastNetMs = now;
        g_netSeeded = true; *dl = *ul = 0.f; return;
    }
    ULONGLONG dMs = now - g_lastNetMs;
    g_lastNetMs = now;
    if (dMs < 1) dMs = 1;
    const double dt = (double)dMs / 1000.0;
    double bpsIn  = (double)(inB  - g_prevIn)  / dt;
    double bpsOut = (double)(outB - g_prevOut) / dt;
    g_prevIn = inB; g_prevOut = outB;
    if (bpsIn  < 0) bpsIn  = 0;
    if (bpsOut < 0) bpsOut = 0;
    *inBps  = bpsIn;
    *outBps = bpsOut;
    *dl = NetBusy((float)bpsIn);    // 下载 = 入站
    *ul = NetBusy((float)bpsOut);   // 上传 = 出站
}

// ---------------------------------------------------------------------------
// 四档颜色：绿→黄→橙→红
// ---------------------------------------------------------------------------
struct RGB24 { int r, g, b; };

static RGB24 LoadColor(float pct) {
    if (pct < 0.f) pct = 0.f;
    if (pct > 100.f) pct = 100.f;
    if (pct < 25.f) return RGB24{46, 204, 113};
    if (pct < 50.f) return RGB24{241, 196, 15};
    if (pct < 75.f) return RGB24{230, 126, 34};
    return RGB24{231, 76, 60};
}

static RGB24 Blend(const RGB24& a, const RGB24& b, float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    RGB24 c;
    c.r = (int)(a.r + (b.r - a.r) * t + 0.5f);
    c.g = (int)(a.g + (b.g - a.g) * t + 0.5f);
    c.b = (int)(a.b + (b.b - a.b) * t + 0.5f);
    return c;
}

static BYTE ClampB(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

static void BlendPixel(BYTE* p, int r, int g, int b, BYTE a) {
    if (a == 0) return;
    if (a == 255) {
        p[0] = (BYTE)b; p[1] = (BYTE)g; p[2] = (BYTE)r; p[3] = 255;
        return;
    }
    const int ia = 255 - a;
    const int da = p[3];
    const int oa = a + (da * ia) / 255;
    if (oa <= 0) {
        p[0] = p[1] = p[2] = p[3] = 0;
        return;
    }
    p[0] = ClampB((b * a + p[0] * da * ia / 255) / oa);
    p[1] = ClampB((g * a + p[1] * da * ia / 255) / oa);
    p[2] = ClampB((r * a + p[2] * da * ia / 255) / oa);
    p[3] = ClampB(oa);
}

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static HWND  g_hwnd = nullptr;
static HDC   g_memDc = nullptr, g_textDc = nullptr;
static HBITMAP g_dib = nullptr, g_textDib = nullptr;
static HGDIOBJ g_oldBmp = nullptr, g_textOldBmp = nullptr;
static BYTE* g_bits = nullptr;
static BYTE* g_textBits = nullptr;
static HICON g_hIcon = nullptr;
static NOTIFYICONDATAW g_nid = {};
static bool  g_trayAdded = false;
static bool  g_topmost = true;
static bool  g_showText = false;
// 显示模式：0=球+状态条（默认） / 1=只显示六个球 / 2=只显示六条状态条
enum ViewMode { kViewBoth = 0, kViewBallsOnly = 1, kViewBandsOnly = 2 };
static int   g_viewMode = kViewBandsOnly;   // 启动默认只显示六条状态条；启动时被 LoadViewMode 覆盖

// 显示模式持久化：仅保存一个 REG_DWORD，注册表不可用时保持默认值。
static void LoadViewMode() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    DWORD v = 0, cb = sizeof(v), type = 0;
    if (RegQueryValueExW(hk, kRegView, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD && cb == sizeof(v) && v <= (DWORD)kViewBandsOnly)
        g_viewMode = (int)v;
    RegCloseKey(hk);
}
static void SaveViewMode() {
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, &disp) != ERROR_SUCCESS)
        return;
    const DWORD v = (DWORD)g_viewMode;
    RegSetValueExW(hk, kRegView, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
}

static int   g_lang = kLangZh;    // 启动默认中文；启动时被 LoadLang 覆盖

// 界面语言持久化：HKCU\Software\CpuBall\Lang（REG_DWORD 0=中文 / 1=English）。
static void LoadLang() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    DWORD v = 0, cb = sizeof(v), type = 0;
    if (RegQueryValueExW(hk, kRegLang, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD && cb == sizeof(v) && v <= (DWORD)kLangEn)
        g_lang = (int)v;
    RegCloseKey(hk);
}
static void SaveLang() {
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, &disp) != ERROR_SUCCESS)
        return;
    const DWORD v = (DWORD)g_lang;
    RegSetValueExW(hk, kRegLang, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
}

static bool  g_netPill = false;   // false=圆形小球，true=圆角药丸形

// 三个布尔开关的加载 / 保存：显示百分比、始终置顶、网速形状。
static void LoadBoolPrefs() {
    DWORD v = 0;
    if (RegGetDword(kRegShowText, &v)) g_showText = (v != 0);
    if (RegGetDword(kRegTopmost,  &v)) g_topmost  = (v != 0);
    if (RegGetDword(kRegNetPill,  &v)) g_netPill  = (v != 0);
}
static void SaveShowText() { RegSetDword(kRegShowText, g_showText ? 1u : 0u); }
static void SaveTopmost()  { RegSetDword(kRegTopmost,  g_topmost  ? 1u : 0u); }
static void SaveNetPill()  { RegSetDword(kRegNetPill,  g_netPill  ? 1u : 0u); }

// 滚轮缩放：绘制始终在 kW×kH 的逻辑帧上进行，输出时整帧拉伸到目标尺寸。
// 拉伸用的 DIB 按最大档位一次性分配，切档位不需重建，也不会留下上一档残影。
static int     g_zoomIdx = kZoomDefault;   // kZoomLevels 下标；启动时被 LoadZoom 覆盖

// 双击面板：临时放大到当前显示器工作区中央，再双击还原。
// 这是纯临时状态，不写注册表；g_centerSaved* 保存进入前的缩放和窗口位置。
static bool g_centerExpanded = false;
static int  g_centerSavedZoom = kZoomDefault;
static int  g_centerSavedX = 0, g_centerSavedY = 0;

// 缩放档位持久化：HKCU\Software\CpuBall\Zoom（REG_DWORD，kZoomLevels 下标）。
static void LoadZoom() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    DWORD v = 0, cb = sizeof(v), type = 0;
    if (RegQueryValueExW(hk, kRegZoom, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD && cb == sizeof(v) && v < (DWORD)kZoomCount)
        g_zoomIdx = (int)v;
    RegCloseKey(hk);
}
static void SaveZoom() {
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, &disp) != ERROR_SUCCESS)
        return;
    const DWORD v = (DWORD)g_zoomIdx;
    RegSetValueExW(hk, kRegZoom, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
}

static HDC     g_scaleDc = nullptr;
static HBITMAP g_scaleDib = nullptr;
static HGDIOBJ g_scaleOldBmp = nullptr;

static float Zoom() { return kZoomLevels[g_zoomIdx]; }
static int   OutW() { return (int)(kW * Zoom() + 0.5f); }
static int   OutH(int logicalH) { return (int)(logicalH * Zoom() + 0.5f); }
static LONG  Zs(float v) { return (LONG)(v * Zoom() + 0.5f); }

// 当前语言对应的一套文案；所有用户可见文字都从这里取
static const UiText& T() { return g_lang == kLangEn ? kEn : kZh; }

// ---------------------------------------------------------------------------
// 系统信息：悬停气泡尾部附带 计算机名 / 操作系统及版本 / 开机时间
// ---------------------------------------------------------------------------
static void GetOsVersionText(wchar_t* out, size_t cap) {
    out[0] = 0;
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t product[128] = {}, disp[64] = {}, build[32] = {};
        DWORD ubr = 0, cb = 0, type = 0;
        cb = sizeof(product); RegQueryValueExW(hk, L"ProductName",     nullptr, &type, (LPBYTE)product, &cb);
        cb = sizeof(disp);    RegQueryValueExW(hk, L"DisplayVersion",  nullptr, &type, (LPBYTE)disp,    &cb);
        cb = sizeof(build);   RegQueryValueExW(hk, L"CurrentBuild",    nullptr, &type, (LPBYTE)build,   &cb);
        cb = sizeof(ubr);     RegQueryValueExW(hk, L"UBR",             nullptr, &type, (LPBYTE)&ubr,    &cb);
        RegCloseKey(hk);
        if (product[0]) {
            if (disp[0] && build[0])
                swprintf(out, cap, L"%ls %ls (%ls.%u)", product, disp, build, (unsigned)ubr);
            else if (disp[0])
                swprintf(out, cap, L"%ls %ls", product, disp);
            else if (build[0])
                swprintf(out, cap, L"%ls (%ls)", product, build);
            else
                wcsncpy(out, product, cap - 1);
            return;
        }
    }
    // 注册表读不到时回退到 ntdll!RtlGetVersion（GetVersionEx 在 Win10+ 会被清单层降级）
    typedef void (WINAPI *PFN_RTLGETVERSION)(OSVERSIONINFOEXW*);
    PFN_RTLGETVERSION p = (PFN_RTLGETVERSION)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (p) {
        OSVERSIONINFOEXW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        p(&vi);
        swprintf(out, cap, L"Windows %u.%u (build %u)",
                 (unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion,
                 (unsigned)vi.dwBuildNumber);
    } else {
        wcscpy(out, L"Windows");
    }
}

// 把当前本地时间减去开机至今的毫秒数，得到开机时刻；同时给出运行时长
static void FormatBootInfo(wchar_t* out, size_t cap) {
    const ULONGLONG ms = GetTickCount64();
    SYSTEMTIME now;
    GetLocalTime(&now);
    FILETIME ft;
    SystemTimeToFileTime(&now, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= ms * 10000ULL;
    FILETIME bt;
    bt.dwLowDateTime = u.LowPart; bt.dwHighDateTime = u.HighPart;
    SYSTEMTIME st;
    FileTimeToSystemTime(&bt, &st);

    const ULONGLONG sec = ms / 1000;
    const int days  = (int)(sec / 86400);
    const int hours = (int)((sec % 86400) / 3600);
    const int mins  = (int)((sec % 3600) / 60);
    if (g_lang == kLangEn)
        swprintf(out, cap, L"%04d-%02d-%02d %02d:%02d:%02d (up %dd %02dh %02dm)",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 days, hours, mins);
    else
        swprintf(out, cap, L"%04d-%02d-%02d %02d:%02d:%02d（已运行 %d 天 %02d 小时 %02d 分）",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 days, hours, mins);
}

// 组装三行系统信息（计算机名 / 系统版本 / 开机时间）
static void FormatSysInfo(wchar_t* out, size_t cap) {
    wchar_t pc[64] = {}, os[192] = {}, boot[128] = {};
    DWORD n = 64;
    if (!GetComputerNameW(pc, &n)) wcscpy(pc, L"-");
    GetOsVersionText(os, 192);
    FormatBootInfo(boot, 128);
    if (g_lang == kLangEn)
        swprintf(out, cap, L"Computer: %ls\nOS: %ls\nBoot: %ls", pc, os, boot);
    else
        swprintf(out, cap, L"计算机：%ls\n系统：%ls\n开机：%ls", pc, os, boot);
}

static bool  g_running = true;
static int   g_winX = 80, g_winY = 80;
static bool  g_dragging = false;

// 窗口位置保存：坐标按 DWORD 原样存（负值多显示器场景也能正确往返）。
static void SaveWindowPos() {
    RegSetDword(kRegWinX, (DWORD)g_winX);
    RegSetDword(kRegWinY, (DWORD)g_winY);
}
// 读取上次的左上角；返回是否读到。可见性校验由调用方在启动定位处完成。
static bool LoadWindowPos(int* x, int* y) {
    DWORD vx = 0, vy = 0;
    if (!RegGetDword(kRegWinX, &vx) || !RegGetDword(kRegWinY, &vy)) return false;
    *x = (int)vx; *y = (int)vy;
    return true;
}
static POINT g_dragOff{0, 0};
static UINT  g_tick = 0;
static HFONT g_font = nullptr;
static HFONT g_fontSmall = nullptr;
static HANDLE g_hMutex = nullptr;
static UINT  g_msgTaskbarCreated = 0;   // Explorer 重启后广播的 "TaskbarCreated"

static float g_cpu = 0.f, g_mem = 0.f, g_disk = 0.f;
static float g_cpuS = 0.f, g_memS = 0.f, g_diskS = 0.f;
static float g_up = 0.f, g_down = 0.f;     // 上传 / 下载（瞬时，0~100 繁忙度）
static float g_upS = 0.f, g_downS = 0.f;   // 上传 / 下载（平滑后）
static float g_gpu = 0.f, g_gpuS = 0.f;    // GPU 使用率（瞬时 / 平滑后）

// 历史环形缓冲（线性左移版）：g_hist[i][0] 最旧、[kHistN-1] 最新
//   i=0 CPU / 1 内存 / 2 磁盘 / 3 上传 / 4 下载 / 5 GPU（3、4 存 NetBusy 对数繁忙度 0~100）
// 球体用 EMA 平滑值显示"此刻"，状态条用未平滑的瞬时值记录"这一段时间发生了什么"
// g_histBps[0]=上传、[1]=下载，保存真实字节/秒，仅用于两条网速条的悬停气泡
static float  g_hist[kBandCount][kHistN] = {};
static double g_histBps[kNetBandCount][kHistN] = {};
static int    g_histCount = 0;             // 已采集的点数（≤ kHistN）

// 帧缓冲始终按 kH 分配（球在上、状态条在下），显示模式只决定截取哪一段：
//   球+状态条 : 源 y=0，     高 kH
//   只显示球   : 源 y=0，     高 kBallAreaH
//   只显示状态条: 源 y=kBandTop，高 kH-kBandTop（条区整体提到窗口顶部）
// 因此切换模式无需重建 DIB、不会留残影，历史曲线也保持连续。
static bool BallsVisible() { return g_viewMode != kViewBandsOnly; }
static bool BandsVisible() { return g_viewMode != kViewBallsOnly; }
static int  SrcTop() {
    return g_viewMode == kViewBandsOnly ? kBandTop : 0;
}
static int WinHeight() {
    if (g_viewMode == kViewBallsOnly) return kBallAreaH;
    if (g_viewMode == kViewBandsOnly) return kH - kBandTop;   // 140
    return kH;
}
// 当前显示模式下窗口的输出高度；起止都取整再相减，与 RenderPanel 的 DIB 源区严格一致
static int OutViewH() {
    const float s = Zoom();
    return (int)((SrcTop() + WinHeight()) * s + 0.5f) - (int)(SrcTop() * s + 0.5f);
}

static void RequestExit();
static void ApplyTopmost();
static void RenderPanel();
static void RelayoutNet();
static void DrawPill(int index, float pct);
static void TrayAdd();
static void ResetWindowPosition(bool save = true);
static void ResetAllPreferences();
static void ToggleCenterZoom();
static void ClearCenterZoomState();
static void EnsureWindowVisible();
static RECT BandRect(int index);
static void RecordHistory(float cpu, float mem, float disk, float up, float down,
                          float gpu, double upBps, double downBps);
static void HistStats(int band, float* avg, float* peak, float* cur);
static void HistBpsStats(int band, double* avg, double* peak, double* cur);
static void FormatRate(double bps, wchar_t* out, size_t cap);
static void DrawHistoryBands();

// ---------------------------------------------------------------------------
// 六球几何（竖排）
//   [0] CPU   顶（大灯）
//   [1] 内存  中排左
//   [2] 磁盘  中排右
//   [3] 上传  底部左（小球）
//   [4] 下载  底部右（小球）
//   [5] GPU   底部中（小球）
// ---------------------------------------------------------------------------
static float g_lampX[6];
static float g_lampY[6];
static float g_lampR[6];  // 半径（圆形用）
static float g_lampHW[6]; // 半宽（药丸形用）
static float g_lampHH[6]; // 半高（药丸形用）

static void InitLamps() {
    const float rCpu  = kLampDCpu  * 0.5f;
    const float rBase = kLampDBase * 0.5f;
    const float rNet  = kLampDNet  * 0.5f;

    g_lampR[0] = rCpu;
    g_lampR[1] = rBase;
    g_lampR[2] = rBase;
    g_lampR[3] = rNet;
    g_lampR[4] = rNet;
    g_lampR[5] = rNet;
    // 非网速球用半径作为半宽/半高（药丸绘制仅用于 3/4/5，这里仅占位）
    for (int i = 0; i < 6; ++i) { g_lampHW[i] = g_lampR[i]; g_lampHH[i] = g_lampR[i]; }

    // 顶：CPU 大球
    g_lampX[0] = kW * 0.5f;
    g_lampY[0] = kMargin + rCpu;

    // 中排：内存 / 磁盘（紧贴 CPU 下缘）
    const float yBase = kMargin + 2.f * rCpu + 2.f + rBase;
    g_lampY[1] = yBase;
    g_lampY[2] = yBase;
    g_lampX[1] = kMargin + rBase;
    g_lampX[2] = kW - kMargin - rBase;

    // 底部：上传 / GPU / 下载 三球（Y 固定，X / 半宽半高 由形状决定）
    const float yNet = yBase + rBase + 4.f + rNet;
    g_lampY[3] = yNet;
    g_lampY[4] = yNet;
    g_lampY[5] = yNet;
    RelayoutNet();
}

// 根据 g_netPill 重新摆放底部三个灯（上传/GPU/下载）的位置与尺寸
static void RelayoutNet() {
    const float rNet = kLampDNet * 0.5f;
    const float xMid = kW * 0.5f;
    if (g_netPill) {
        // 三个药丸并排：宽度自动收窄以适配窗口（两球间隙 kGapX）
        const float hw = (kW - 2.f * kMargin - 2.f * kGapX) / 6.f;
        const float hh = rNet;
        g_lampHW[3] = g_lampHW[4] = g_lampHW[5] = hw;
        g_lampHH[3] = g_lampHH[4] = g_lampHH[5] = hh;
        g_lampX[3] = kMargin + hw;          // 左：上传
        g_lampX[4] = kW - kMargin - hw;     // 右：下载
        g_lampX[5] = xMid;                  // 中：GPU
    } else {
        // 三个圆球均分宽度
        const float gap = (kW - 2.f * kMargin - 6.f * rNet) / 2.f;  // 相邻边缘间距
        g_lampHW[3] = g_lampHW[4] = g_lampHW[5] = rNet;
        g_lampHH[3] = g_lampHH[4] = g_lampHH[5] = rNet;
        g_lampX[3] = xMid - 2.f * rNet - gap;   // 左：上传
        g_lampX[4] = xMid + 2.f * rNet + gap;   // 右：下载
        g_lampX[5] = xMid;                      // 中：GPU
    }
    g_lampR[3] = g_lampR[4] = g_lampR[5] = rNet;
}

// 为 6 个球 + 6 条历史状态条各注册一个 tooltip 工具（矩形取包围盒，稍加容差）
// 文字走 LPSTR_TEXTCALLBACK，在 WndProc 的 TTN_GETDISPINFO 里填指标名与数值
// 注意：程序无 v6 清单 → 运行的是 comctl32 v5，必须用 v1 结构体尺寸（不含 lpReserved），
//       否则 TTM_ADDTOOLW 静默失败，tooltip 完全不显示
#ifndef TTTOOLINFOW_V1_SIZE
#define TTTOOLINFOW_V1_SIZE (offsetof(TOOLINFOW, lParam) + sizeof(LPARAM))
#endif
static void AddBallTools() {
    if (!g_hwndTip || !g_hwnd) return;
    for (int i = 0; i < 6 + kBandCount; ++i) {   // 0..5 = 六个球，6..11 = 六条历史状态条
        TOOLINFOW del{};
        del.cbSize = TTTOOLINFOW_V1_SIZE;
        del.hwnd = g_hwnd;
        del.uId = (UINT_PTR)i;
        SendMessageW(g_hwndTip, TTM_DELTOOLW, 0, (LPARAM)&del);  // 先删（不存在则无害）
        // 当前模式没显示的那一组不再注册悬停区域
        if (i <  6 && !BallsVisible()) continue;
        if (i >= 6 && !BandsVisible()) continue;

        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_hwnd;
        ti.uId = (UINT_PTR)i;
        ti.hinst = nullptr;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        // 命中区按当前缩放同步放大/缩小，否则缩小时气泡范围会大于画面
        if (i < 6) {
            ti.rect.left   = Zs(g_lampX[i] - g_lampHW[i]) - 3;
            ti.rect.right  = Zs(g_lampX[i] + g_lampHW[i]) + 3;
            ti.rect.top    = Zs(g_lampY[i] - g_lampHH[i]) - 3;
            ti.rect.bottom = Zs(g_lampY[i] + g_lampHH[i]) + 3;
        } else {
            const RECT r = BandRect(i - 6);       // 逻辑帧坐标
            const int  off = SrcTop();            // 只显示状态条时整组上移
            ti.rect.left   = Zs((float)r.left) - 1;
            ti.rect.right  = Zs((float)r.right) + 1;
            ti.rect.top    = Zs((float)(r.top - off)) - 1;
            ti.rect.bottom = Zs((float)(r.bottom - off)) + 1;
        }
        SendMessageW(g_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

// 创建 tooltip 控件并注册所有悬停区域。切换语言时销毁重建整个控件：
// comctl32 v5 会缓存每个 tool 首次显示的文本，而单纯 TTM_DELTOOL/ADDTOOL
// 重注册又容易弄丢 TTF_SUBCLASS（气泡直接不弹），重建控件最干净。
static void InitTooltip() {
    InitCommonControls();
    g_hwndTip = CreateWindowExW(
        0, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | TTS_BALLOON,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hwndTip) return;
    SendMessageW(g_hwndTip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)560);   // 容纳指标 + 三行系统信息
    SendMessageW(g_hwndTip, TTM_SETDELAYTIME, (WPARAM)TTDT_INITIAL, (LPARAM)200);
    SendMessageW(g_hwndTip, TTM_SETDELAYTIME, (WPARAM)TTDT_AUTOPOP, (LPARAM)8000);
    SendMessageW(g_hwndTip, TTM_SETTITLEW, 0, (LPARAM)T().appTitle);
    AddBallTools();
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
// clipHw/clipHh：文字裁剪半宽/半高（默认用 radius 作圆裁剪，药丸形传入自身尺寸）
static void DrawTextCentered(const wchar_t* text, float cx, float cy, const RGB24& col,
                             float radius, HFONT font, float clipHw = 0.f, float clipHh = 0.f) {
    if (!g_showText || !g_textBits || !g_textDc) return;

    const float chw = (clipHw > 0.f ? clipHw : radius) * 0.92f;
    const float chh = (clipHh > 0.f ? clipHh : radius) * 0.85f;

    memset(g_textBits, 0, (size_t)kW * kH * 4);
    SetBkMode(g_textDc, TRANSPARENT);
    SetTextColor(g_textDc, RGB(255, 255, 255));
    if (font) SelectObject(g_textDc, font);
    RECT rc;
    rc.left = (LONG)(cx - chw);
    rc.right = (LONG)(cx + chw);
    rc.top = (LONG)(cy - chh);
    rc.bottom = (LONG)(cy + chh);
    DrawTextW(g_textDc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    for (int y = (int)rc.top; y < (int)rc.bottom; ++y) {
        if (y < 0 || y >= kH) continue;
        for (int x = (int)rc.left; x < (int)rc.right; ++x) {
            if (x < 0 || x >= kW) continue;
            const float dx = x + 0.5f - cx;
            const float dy = y + 0.5f - cy;
            if (fabsf(dx) > chw || fabsf(dy) > chh) continue;
            const BYTE* src = g_textBits + ((size_t)y * kW + x) * 4;
            const int lum = (src[2] * 30 + src[1] * 59 + src[0] * 11) / 100;
            if (lum < 40) continue;
            BYTE a = (BYTE)lum;
            if (a > 220) a = 255;
            a = (BYTE)((a * kAlphaText) / 255);
            BlendPixel(g_bits + ((size_t)y * kW + x) * 4, col.r, col.g, col.b, a);
        }
    }
}

static void DrawLamp(int index, float pct) {
    if (index >= 3 && g_netPill) { DrawPill(index, pct); return; }
    const RGB24 base  = LoadColor(pct);
    const RGB24 outer = Blend(base, RGB24{0, 0, 0}, 0.18f);
    const RGB24 inner = Blend(base, RGB24{255, 255, 255}, 0.22f);
    const RGB24 ring  = Blend(base, RGB24{255, 255, 255}, 0.15f + (pct / 100.f) * 0.55f);

    const float cX = g_lampX[index];
    const float cY = g_lampY[index];
    const float rOut = g_lampR[index];
    const float rIn  = rOut * 0.76f;
    const float rRing0 = rOut - 2.2f, rRing1 = rOut - 0.2f;

    const int y0 = (int)(cY - rOut - 1);
    const int y1 = (int)(cY + rOut + 1);
    const int x0 = (int)(cX - rOut - 1);
    const int x1 = (int)(cX + rOut + 1);

    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= kH) continue;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= kW) continue;
            const float dx = x + 0.5f - cX;
            const float dy = y + 0.5f - cY;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > rOut + 0.5f) continue;

            float edge = 1.f;
            if (d > rOut - 1.4f) {
                edge = (rOut - d) / 1.4f;
                if (edge <= 0.f) continue;
                if (edge > 1.f) edge = 1.f;
            }

            RGB24 col = outer;
            BYTE baseA = kAlphaBody;
            if (d <= rIn) {
                col = inner;
                baseA = (BYTE)(kAlphaBody - 20);
            }
            const float hx = dx + rOut * 0.15f, hy = dy + rOut * 0.18f;
            const float hd = (hx * hx) / (rOut * rOut * 0.18f) + (hy * hy) / (rOut * rOut * 0.12f);
            if (hd < 1.f && d < rIn) {
                const float t = 0.38f * (1.f - hd);
                col = Blend(col, RGB24{255, 255, 255}, t);
                baseA = ClampB(baseA + (int)(35 * t));
            }
            if (d >= rRing0 && d <= rRing1) {
                col = ring;
                baseA = kAlphaRing;
            }

            const BYTE alpha = ClampB((int)(baseA * edge + 0.5f));
            if (alpha) BlendPixel(g_bits + ((size_t)y * kW + x) * 4, col.r, col.g, col.b, alpha);
        }
    }

    wchar_t t[16];
    swprintf(t, 16, L"%.0f%%", pct);
    const RGB24 tc = (pct < 50.f) ? RGB24{20, 20, 20} : RGB24{255, 255, 255};
    const bool isNet = (index == 3 || index == 4);
    DrawTextCentered(t, cX, cY, tc, rOut, isNet ? g_fontSmall : g_font);
}

// 圆角药丸形（仅用于底部网速灯 3/4）：横向拉长、两端全圆角，沿用球体的配色与高光
static void DrawPill(int index, float pct) {
    const RGB24 base  = LoadColor(pct);
    const RGB24 outer = Blend(base, RGB24{0, 0, 0}, 0.18f);
    const RGB24 inner = Blend(base, RGB24{255, 255, 255}, 0.22f);
    const RGB24 ring  = Blend(base, RGB24{255, 255, 255}, 0.15f + (pct / 100.f) * 0.55f);

    const float cX = g_lampX[index];
    const float cY = g_lampY[index];
    const float hw = g_lampHW[index];
    const float hh = g_lampHH[index];
    const float cr = hh;            // 全圆角 → 药丸
    const float rOut = hh;

    const int x0 = (int)(cX - hw - 1), x1 = (int)(cX + hw + 1);
    const int y0 = (int)(cY - hh - 1), y1 = (int)(cY + hh + 1);

    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= kH) continue;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= kW) continue;
            const float dx = x + 0.5f - cX;
            const float dy = y + 0.5f - cY;

            // 圆角矩形有符号距离（sdRoundBox）
            const float qx = fabsf(dx) - hw + cr;
            const float qy = fabsf(dy) - hh + cr;
            const float mx = qx > 0.f ? qx : 0.f;
            const float my = qy > 0.f ? qy : 0.f;
            const float sdf = ((qx > qy ? qx : qy) < 0.f ? (qx > qy ? qx : qy) : 0.f)
                            + sqrtf(mx * mx + my * my) - cr;

            float cov = 0.5f - sdf;     // 1px 抗锯齿
            if (cov <= 0.f) continue;
            if (cov > 1.f) cov = 1.f;

            // 内区（更亮）：按比例缩小的同形
            const float qx2 = fabsf(dx) - hw * 0.76f + cr * 0.76f;
            const float qy2 = fabsf(dy) - hh * 0.76f + cr * 0.76f;
            const float mx2 = qx2 > 0.f ? qx2 : 0.f;
            const float my2 = qy2 > 0.f ? qy2 : 0.f;
            const float sdfIn = ((qx2 > qy2 ? qx2 : qy2) < 0.f ? (qx2 > qy2 ? qx2 : qy2) : 0.f)
                             + sqrtf(mx2 * mx2 + my2 * my2) - cr * 0.76f;

            RGB24 col = outer;
            BYTE baseA = kAlphaBody;
            if (sdfIn <= 0.f) { col = inner; baseA = (BYTE)(kAlphaBody - 20); }

            // 顶部高光
            const float hx = dx + hw * 0.10f, hy = dy + hh * 0.18f;
            const float hd = (hx * hx) / (hw * hw * 0.55f) + (hy * hy) / (hh * hh * 0.30f);
            if (hd < 1.f && sdf <= 0.f) {
                const float t = 0.30f * (1.f - hd);
                col = Blend(col, RGB24{255, 255, 255}, t);
                baseA = ClampB(baseA + (int)(30 * t));
            }

            // 边缘亮环
            if (sdf > -2.2f && sdf <= 0.f) { col = ring; baseA = kAlphaRing; }

            const BYTE alpha = ClampB((int)(baseA * cov + 0.5f));
            if (alpha) BlendPixel(g_bits + ((size_t)y * kW + x) * 4, col.r, col.g, col.b, alpha);
        }
    }

    wchar_t t[16];
    swprintf(t, 16, L"%.0f%%", pct);
    const RGB24 tc = (pct < 50.f) ? RGB24{20, 20, 20} : RGB24{255, 255, 255};
    DrawTextCentered(t, cX, cY, tc, rOut, g_fontSmall, hw, hh);
}

// ---------------------------------------------------------------------------
// 底部六条历史状态条：CPU / 内存 / 磁盘 / 上传 / 下载 / GPU，最近 kHistN * 250ms = 60 秒
// 每条 = 整条时序柱（每列 2 个采样点的均值；柱高=负载百分比或网速对数繁忙度，
//        颜色沿用球体的绿→黄→橙→红四档），画面上不含任何文字
// 指标名 + 平均/峰值/当前数值都由该条的悬停气泡给出；网速两条另给真实速率
// ---------------------------------------------------------------------------
static RECT BandRect(int index) {
    RECT r;
    r.left   = kMargin;
    r.right  = kW - kMargin;
    r.top    = kBandTop + index * (kBandH + kBandGap);
    r.bottom = r.top + kBandH;
    return r;
}

static void RecordHistory(float cpu, float mem, float disk, float up, float down,
                          float gpu, double upBps, double downBps) {
    const float  v[kBandCount]   = { cpu, mem, disk, up, down, gpu };
    const double bps[kBandCount] = { 0.0, 0.0, 0.0, upBps, downBps, 0.0 };
    // 线性左移：每 250ms 搬 6×239 个 float + 2×239 个 double（约 9.5KB），开销可忽略
    for (int i = 0; i < kBandCount; ++i) {
        memmove(g_hist[i], g_hist[i] + 1, (size_t)(kHistN - 1) * sizeof(float));
        g_hist[i][kHistN - 1] = v[i];
        // 只有两条网速条另存真实字节速率，GPU 条是普通百分比
        if (i >= kNetBand0 && i < kNetBand0 + kNetBandCount) {
            double* row = g_histBps[i - kNetBand0];
            memmove(row, row + 1, (size_t)(kHistN - 1) * sizeof(double));
            row[kHistN - 1] = bps[i];
        }
    }
    if (g_histCount < kHistN) ++g_histCount;
}

static void HistStats(int band, float* avg, float* peak, float* cur) {
    const int first = kHistN - g_histCount;
    float sum = 0.f, mx = 0.f;
    int n = 0;
    for (int s = first; s < kHistN; ++s) {
        const float v = g_hist[band][s];
        sum += v; ++n;
        if (v > mx) mx = v;
    }
    *avg  = n ? sum / n : 0.f;
    *peak = mx;
    *cur  = g_histCount ? g_hist[band][kHistN - 1] : 0.f;
}

static void HistBpsStats(int band, double* avg, double* peak, double* cur) {
    const double* row = g_histBps[band - kNetBand0];
    const int first = kHistN - g_histCount;
    double sum = 0.0, mx = 0.0;
    int n = 0;
    for (int s = first; s < kHistN; ++s) {
        const double v = row[s];
        sum += v; ++n;
        if (v > mx) mx = v;
    }
    *avg  = n ? sum / n : 0.0;
    *peak = mx;
    *cur  = g_histCount ? row[kHistN - 1] : 0.0;
}

static void FormatRate(double bps, wchar_t* out, size_t cap) {
    if (bps < 1024.0)
        swprintf(out, cap, L"%.0f B/s", bps);
    else if (bps < 1024.0 * 1024.0)
        swprintf(out, cap, L"%.1f KB/s", bps / 1024.0);
    else
        swprintf(out, cap, L"%.2f MB/s", bps / (1024.0 * 1024.0));
}

static void DrawHistoryBands() {
    const int cols  = kW - 2 * kMargin;      // 120 列 = 60 秒
    const int first = kHistN - g_histCount;  // 未采集到的时间槽在左侧留空
    for (int i = 0; i < kBandCount; ++i) {
        const RECT r = BandRect(i);

        // 半透明底板 + 上下细边
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x)
                BlendPixel(g_bits + ((size_t)y * kW + x) * 4, 16, 18, 22, 105);
        for (int x = r.left; x < r.right; ++x) {
            BlendPixel(g_bits + ((size_t)r.top * kW + x) * 4, 255, 255, 255, 28);
            BlendPixel(g_bits + ((size_t)(r.bottom - 1) * kW + x) * 4, 255, 255, 255, 28);
        }

        // 状态条上不画任何文字：指标名与平均/峰值/当前百分比都在悬停气泡里
        // 时序柱：左旧右新；柱高=负载，颜色=档位
        const int by0  = r.top + kBandLabelH;
        const int bh   = r.bottom - by0;
        const int midY = by0 + bh / 2;
        for (int x = r.left + 1; x < r.right - 1; ++x)
            BlendPixel(g_bits + ((size_t)midY * kW + x) * 4, 255, 255, 255, 22);  // 50% 参考线

        for (int c = 0; c < cols; ++c) {
            int a = c * kHistN / cols;
            int b = (c + 1) * kHistN / cols;
            if (b <= a) b = a + 1;
            if (b <= first) continue;                 // 这段 60 秒窗口还没数据
            if (a < first) a = first;
            float sum = 0.f; int n = 0;
            for (int s = a; s < b && s < kHistN; ++s) { sum += g_hist[i][s]; ++n; }
            if (!n) continue;
            const float v = sum / n;
            int hh = (int)(v * bh / 100.f + 0.5f);
            if (v > 0.f && hh < 1) hh = 1;
            if (hh > bh) hh = bh;
            const int x = r.left + c;
            if (x >= r.right) break;
            const RGB24 col = LoadColor(v);
            if (hh == 0) {
                // 网速空闲时繁忙度为 0：留 1px 暗底线，区分"已采集但空闲"与"左侧尚无数据"
                if (i >= kNetBand0 && i < kNetBand0 + kNetBandCount)
                    BlendPixel(g_bits + ((size_t)(r.bottom - 1) * kW + x) * 4,
                               col.r, col.g, col.b, 70);
                continue;
            }
            for (int k = 0; k < hh; ++k) {
                const int y = r.bottom - 1 - k;
                BlendPixel(g_bits + ((size_t)y * kW + x) * 4, col.r, col.g, col.b, 205);
            }
            BlendPixel(g_bits + ((size_t)(r.bottom - hh) * kW + x) * 4,
                       255, 255, 255, 80);   // 柱顶亮点
        }
    }
}

static void RenderPanel() {
    if (!g_hwnd || !g_bits) return;

    memset(g_bits, 0, (size_t)kW * kH * 4);
    // 六球：顶=CPU，中排=内存/磁盘，底部=上传/GPU/下载
    if (BallsVisible()) {
        DrawLamp(0, g_cpuS);
        DrawLamp(1, g_memS);
        DrawLamp(2, g_diskS);
        DrawLamp(3, g_upS);
        DrawLamp(4, g_downS);
        DrawLamp(5, g_gpuS);
    }
    if (BandsVisible())
        DrawHistoryBands();          // 最近 60 秒的 CPU/内存/磁盘/上传/下载/GPU

    // 逻辑帧 → 拉伸缓冲：COLORONCOLOR 是最近邻缩放，32bpp 的 alpha 通道原样复制，
    // 不会被插值成半透明杂色。缓冲按最大档位分配，切档位不重建、不留残影。
    const float s = Zoom();
    const int fullW = (int)(kW * s + 0.5f);
    const int fullH = (int)(kH * s + 0.5f);
    if (g_scaleDc) {
        GdiFlush();   // g_bits 是 CPU 直写的 DIB section，确保 GDI 看到最新像素
        SetStretchBltMode(g_scaleDc, COLORONCOLOR);
        StretchBlt(g_scaleDc, 0, 0, fullW, fullH, g_memDc, 0, 0, kW, kH, SRCCOPY);
    }

    // size 决定窗口高度，pptSrc 决定从拉伸帧哪一行开始取：
    // 只显示状态条时把条区整体提到顶部，球区不进入窗口。
    // 起止都按缩放后取整再相减，避免底部出现 1px 缝。
    const int srcTop = (int)(SrcTop() * s + 0.5f);
    SIZE size{fullW, OutViewH()};
    POINT src{0, srcTop};
    POINT dst{g_winX, g_winY};
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_hwnd, screen, &dst, &size,
                        g_scaleDc ? g_scaleDc : g_memDc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------
static HICON CreateTrayIcon(float pct) {
    const int size = 16;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBmp || !bits) return nullptr;
    auto* px = (BYTE*)bits;
    const RGB24 base = LoadColor(pct);
    const float c = size * 0.5f, r = size * 0.46f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x + 0.5f - c, dy = y + 0.5f - c;
            const float dist = sqrtf(dx * dx + dy * dy);
            BYTE* p = px + (y * size + x) * 4;
            if (dist > r) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
            BYTE a = 255;
            if (dist > r - 1.f) a = ClampB((int)((r - dist) * 255.f));
            RGB24 col = base;
            if (dist > r - 2.f) col = Blend(base, RGB24{20, 20, 20}, 0.35f);
            p[0] = ClampB(col.b); p[1] = ClampB(col.g); p[2] = ClampB(col.r); p[3] = a;
        }
    }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hBmp;
    ii.hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    DeleteObject(hBmp);
    return hIcon;
}

static void TrayUpdateTip() {
    if (!g_trayAdded) return;
    wchar_t tip[160];
    // %ls 而非 %s：MinGW 的宽版 swprintf 按 C99 语义把 %s 当窄字符串，会输出乱码
    swprintf(tip, 160, T().trayTipFmt,
             T().appTitle, g_cpuS, g_memS, g_diskS, g_gpuS, g_upS, g_downS);
    tip[159] = 0;
    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayUpdateIcon() {
    if (!g_trayAdded) return;
    float mx = g_cpuS;
    if (g_memS > mx) mx = g_memS;
    if (g_diskS > mx) mx = g_diskS;
    HICON old = g_hIcon;
    HICON ni = CreateTrayIcon(mx);
    if (!ni) return;
    g_hIcon = ni;
    g_nid.hIcon = ni;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

static void TrayRemove() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
    if (g_hIcon) {
        DestroyIcon(g_hIcon);
        g_hIcon = nullptr;
    }
}

// 首次启动和 Explorer 重启（"TaskbarCreated"）时都会调用。
// 不重新 NIM_ADD 的话，Explorer 一重启托盘图标就消失，进程却还在后台跑。
static void TrayAdd() {
    if (!g_hwnd) return;
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
    if (!g_hIcon) g_hIcon = CreateTrayIcon(0.f);
    if (!g_hIcon) g_hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = kTrayMsg;
    g_nid.hIcon = g_hIcon;
    wcsncpy(g_nid.szTip, T().appTitle, 127);
    g_nid.szTip[127] = 0;
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

// MessageBox 的按钮文字由系统 UI 语言决定（中文 Windows 固定是「确定」），
// 不受本程序语言开关影响。用同线程 CBT 钩子在对话框激活前改掉按钮文字。
static HHOOK g_aboutHook = nullptr;
static LRESULT CALLBACK AboutCbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND btn = GetDlgItem((HWND)wp, IDOK);
        if (btn) SetWindowTextW(btn, g_lang == kLangEn ? L"OK" : L"确定");
        if (g_aboutHook) { UnhookWindowsHookEx(g_aboutHook); g_aboutHook = nullptr; }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// 关于对话框：版本、开发/构建信息、编译日期；文案随界面语言切换。
// MB_TOPMOST 保证在「始终置顶」的悬浮窗之上可见，MB_SETFOREGROUND 让它直接获得焦点。
static void ShowAbout() {
    wchar_t body[1024];
    swprintf(body, 1024, T().aboutBodyFmt, kVersion, BuildDateIso());
    g_aboutHook = SetWindowsHookExW(WH_CBT, AboutCbtProc, nullptr, GetCurrentThreadId());
    MessageBoxW(g_hwnd, body, T().aboutTitle,
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    if (g_aboutHook) { UnhookWindowsHookEx(g_aboutHook); g_aboutHook = nullptr; }
}

// 缩放档位变化：同步窗口矩形与 tooltip 命中区，再重绘。
// 滚轮缩放以鼠标下的窗内点为锚点（类似地图缩放），这样连续滚动时鼠标始终停在
// 同一块内容上，不会因为窗口缩小后落到透明缝隙而收不到后续滚轮消息；
// 菜单缩放则以左上角为锚点，避免鼠标远在菜单上时把窗口甩走。
static void ApplyZoom(int idx, bool anchorAtCursor) {
    if (idx < 0 || idx >= kZoomCount || idx == g_zoomIdx) return;
    const float oldS = Zoom();
    POINT pt{};
    float ax = 0.f, ay = 0.f;
    if (anchorAtCursor && GetCursorPos(&pt)) {
        ax = (float)(pt.x - g_winX) / oldS;
        ay = (float)(pt.y - g_winY) / oldS;
    }
    g_zoomIdx = idx;
    // 临时居中放大时不写注册表，退出后仍还原进双击前的档位，
    // 避免“界面是 A 档、重启变成 B 档”的不一致。
    if (!g_centerExpanded) SaveZoom();
    const float newS = Zoom();
    if (anchorAtCursor && (ax != 0.f || ay != 0.f)) {
        g_winX = pt.x - (LONG)(ax * newS + 0.5f);
        g_winY = pt.y - (LONG)(ay * newS + 0.5f);
    }
    SetWindowPos(g_hwnd, nullptr, 0, 0, OutW(), OutViewH(),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    AddBallTools();          // 命中区跟着缩放
    EnsureWindowVisible();   // 缩放后若超出屏幕则拉回
    SaveWindowPos();         // 锚点缩放会移动窗口，记下最终位置
    RenderPanel();
}

// 双击放大：切到最大缩放档并把窗口放到当前显示器工作区正中央。
// 进入时记下原缩放/坐标；这里刻意不调用 SaveZoom/SaveWindowPos，
// 所以临时放大状态不会被持久化，退出时精确还原到双击之前。
static void ToggleCenterZoom() {
    if (!g_hwnd) return;

    if (g_centerExpanded) {
        g_centerExpanded = false;
        g_zoomIdx = g_centerSavedZoom;
        g_winX = g_centerSavedX;
        g_winY = g_centerSavedY;
        SetWindowPos(g_hwnd, nullptr, g_winX, g_winY, OutW(), OutViewH(),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        AddBallTools();
        RenderPanel();
        ApplyTopmost();
        return;
    }

    // 先按进入前的窗口判断在哪块屏幕，再按放大后的尺寸居中。
    RECT oldRect{ g_winX, g_winY, g_winX + OutW(), g_winY + OutViewH() };
    HMONITOR mon = MonitorFromRect(&oldRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        mi.rcWork.left = 0; mi.rcWork.top = 0;
        mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    g_centerSavedZoom = g_zoomIdx;
    g_centerSavedX = g_winX;
    g_centerSavedY = g_winY;
    g_centerExpanded = true;
    g_zoomIdx = kZoomCount - 1;

    const int w = OutW();
    const int h = OutViewH();
    const int waW = mi.rcWork.right - mi.rcWork.left;
    const int waH = mi.rcWork.bottom - mi.rcWork.top;
    g_winX = waW >= w ? mi.rcWork.left + (waW - w) / 2 : mi.rcWork.left;
    g_winY = waH >= h ? mi.rcWork.top + (waH - h) / 2 : mi.rcWork.top;

    SetWindowPos(g_hwnd, nullptr, g_winX, g_winY, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    AddBallTools();
    RenderPanel();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    ApplyTopmost();
}

// 菜单里的“重置窗口位置”/“重置所有偏好”会接管窗口定位，
// 这时丢弃临时放大标记，避免下一次双击把旧坐标翻回来。
static void ClearCenterZoomState() {
    g_centerExpanded = false;
}

static void HandleMenuCmd(int id) {
    if (id == IDM_EXIT) {
        RequestExit();
    } else if (id == IDM_TOGGLE_TEXT) {
        g_showText = !g_showText;
        SaveShowText();
        RenderPanel();
    } else if (id == IDM_VIEW_BOTH || id == IDM_VIEW_BALLS || id == IDM_VIEW_BANDS) {
        const int next = (id == IDM_VIEW_BOTH)  ? kViewBoth :
                         (id == IDM_VIEW_BALLS) ? kViewBallsOnly : kViewBandsOnly;
        if (next == g_viewMode) return;
        g_viewMode = next;
        SaveViewMode();          // 记住本次选择，下次启动沿用
        // 同步窗口矩形（鼠标命中区域、tooltip 子类区域），再重绘更新分层窗口尺寸
        SetWindowPos(g_hwnd, nullptr, 0, 0, OutW(), OutViewH(),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        AddBallTools();          // 按新模式注册/注销球与状态条的悬停工具
        EnsureWindowVisible();   // 尺寸变化后若底部超出屏幕则拉回
        RenderPanel();
    } else if (id == IDM_TOGGLE_TOP) {
        g_topmost = !g_topmost;
        SaveTopmost();
        ApplyTopmost();
    } else if (id == IDM_TOGGLE_NETSHAPE) {
        g_netPill = !g_netPill;
        SaveNetPill();
        RelayoutNet();
        RenderPanel();
        AddBallTools();   // 球的位置/尺寸变了，同步刷新悬停区域
    } else if (id == IDM_LANG_ZH || id == IDM_LANG_EN) {
        const int next = (id == IDM_LANG_ZH) ? kLangZh : kLangEn;
        if (next == g_lang) return;
        g_lang = next;
        SaveLang();          // 记住语言选择，下次启动沿用
        TrayUpdateTip();     // 托盘悬停提示立即换语言
        // tooltip 会缓存每个 tool 首次显示的文本，且只删/加工具会弄丢
        // TTF_SUBCLASS 让气泡不再弹出；销毁重建整个控件最干净，标题与正文一起换。
        if (g_hwndTip) { DestroyWindow(g_hwndTip); g_hwndTip = nullptr; }
        InitTooltip();
        // 菜单下次弹出时按 g_lang 重新构建，无需重建当前菜单
    } else if (id == IDM_RESET_POS) {
        ResetWindowPosition();
    } else if (id == IDM_RESET_ALL) {
        ResetAllPreferences();
    } else if (id == IDM_ABOUT) {
        ShowAbout();
    } else if (id == IDM_ZOOM_IN || id == IDM_ZOOM_OUT || id == IDM_ZOOM_RESET) {
        if (id == IDM_ZOOM_RESET) ApplyZoom(kZoomDefault, false);
        else ApplyZoom(g_zoomIdx + (id == IDM_ZOOM_IN ? 1 : -1), false);
    }
}

static void ShowContextMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu,
                MF_STRING | (g_showText ? MF_CHECKED : MF_UNCHECKED),
                IDM_TOGGLE_TEXT, T().menuPercent);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | (g_viewMode == kViewBoth ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_BOTH, T().menuViewBoth);
    AppendMenuW(menu,
                MF_STRING | (g_viewMode == kViewBallsOnly ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_BALLS, T().menuViewBalls);
    AppendMenuW(menu,
                MF_STRING | (g_viewMode == kViewBandsOnly ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_BANDS, T().menuViewBands);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | (g_topmost ? MF_CHECKED : MF_UNCHECKED),
                IDM_TOGGLE_TOP, T().menuTopmost);
    AppendMenuW(menu,
                MF_STRING, IDM_TOGGLE_NETSHAPE,
                g_netPill ? T().menuNetToRound : T().menuNetToPill);
    AppendMenuW(menu, MF_STRING, IDM_RESET_POS, T().menuReset);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ZOOM_IN,  T().menuZoomIn);
    AppendMenuW(menu, MF_STRING, IDM_ZOOM_OUT, T().menuZoomOut);
    wchar_t zoomLabel[80];
    swprintf(zoomLabel, 80, T().menuZoomReset, Zoom() * 100.f);
    AppendMenuW(menu, MF_STRING, IDM_ZOOM_RESET, zoomLabel);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // 语言项自身不翻译：两种语言下都显示「中文 / English」，避免找不到切换入口
    AppendMenuW(menu,
                MF_STRING | (g_lang == kLangZh ? MF_CHECKED : MF_UNCHECKED),
                IDM_LANG_ZH, L"中文");
    AppendMenuW(menu,
                MF_STRING | (g_lang == kLangEn ? MF_CHECKED : MF_UNCHECKED),
                IDM_LANG_EN, L"English");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_RESET_ALL, T().menuResetAll);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, T().menuAbout);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, T().menuExit);

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd != 0) HandleMenuCmd((int)cmd);
}

static void ApplyTopmost() {
    if (!g_hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (g_topmost) {
        if ((ex & WS_EX_TOPMOST) == 0) {
            SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE,
                              ex | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
        }
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        if ((ex & WS_EX_TOPMOST) != 0) {
            SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE,
                              (ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW) & ~WS_EX_TOPMOST);
        }
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// 把悬浮窗放回主工作区右上角。
// 本窗口是 WS_EX_LAYERED，位置由 RenderPanel 里 UpdateLayeredWindow 的 pptDst 决定，
// 因此只改 g_winX/g_winY 再重绘即可，不需要 SetWindowPos。
static void ResetWindowPosition(bool save) {
    if (!g_hwnd) return;
    ClearCenterZoomState();
    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right  = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    g_winX = wa.right - OutW() - 24;
    g_winY = wa.top + 24;
    if (g_winX < wa.left + 8) g_winX = wa.left + 8;
    if (g_winY < wa.top + 8)  g_winY = wa.top + 8;
    if (g_winX + OutW() > wa.right - 8)  g_winX = wa.right  - OutW() - 8;
    if (g_winY + OutViewH() > wa.bottom - 8) g_winY = wa.bottom - OutViewH() - 8;
    if (save) SaveWindowPos();
    RenderPanel();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    ApplyTopmost();
}

// 「重置所有偏好」：清空注册表里本程序的全部设置，再把运行中的界面恢复为
// 与首次启动完全一致的默认状态（中文 / 只显示状态条 / 100% / 不显示百分比 /
// 置顶 / 圆形网速球 / 窗口回到右上角）。save=false 避免重定位时把刚删掉的偏好写回去。
static void ResetAllPreferences() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        static const wchar_t* const kPrefNames[] = {
            kRegView, kRegLang, kRegZoom, kRegShowText,
            kRegTopmost, kRegNetPill, kRegWinX, kRegWinY
        };
        for (int i = 0; i < (int)(sizeof(kPrefNames) / sizeof(kPrefNames[0])); ++i)
            RegDeleteValueW(hk, kPrefNames[i]);
        RegCloseKey(hk);
    }

    g_viewMode = kViewBandsOnly;
    g_lang     = kLangZh;
    g_zoomIdx  = kZoomDefault;
    g_showText = false;
    g_topmost  = true;
    g_netPill  = false;

    RelayoutNet();   // 网速球形状改回圆形，先重算命中几何
    SetWindowPos(g_hwnd, nullptr, 0, 0, OutW(), OutViewH(),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    // 语言会改变 tooltip 标题/正文缓存，和切换语言时一样销毁重建最干净。
    if (g_hwndTip) { DestroyWindow(g_hwndTip); g_hwndTip = nullptr; }
    InitTooltip();   // 内部按新模式/新缩放重新注册所有悬停区
    TrayUpdateTip();
    ResetWindowPosition(false);
}

// 拔掉外接屏 / 改分辨率或缩放后，窗口坐标不会自动跟着变，可能整个落在屏幕外。
// 仅当窗口矩形与任何显示器都无交集时才拉回，不影响用户主动拖到屏幕边缘。
static void EnsureWindowVisible() {
    if (!g_hwnd) return;
    RECT wr = { g_winX, g_winY, g_winX + OutW(), g_winY + OutViewH() };
    if (MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != nullptr) return;
    ResetWindowPosition();
}

static void RequestExit() {
    g_running = false;
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// 资源
// ---------------------------------------------------------------------------
static bool InitFrameBuffer() {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // 拉伸缓冲：按最大缩放档位一次性分配，之后切档位只改 StretchBlt 的目标矩形
    BITMAPINFO si{};
    si.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    si.bmiHeader.biWidth = (int)(kW * kZoomMax + 0.5f);
    si.bmiHeader.biHeight = -(int)(kH * kZoomMax + 0.5f);
    si.bmiHeader.biPlanes = 1;
    si.bmiHeader.biBitCount = 32;
    si.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    g_memDc = CreateCompatibleDC(screen);
    g_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&g_bits, nullptr, 0);
    g_textDc = CreateCompatibleDC(screen);
    g_textDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&g_textBits, nullptr, 0);
    g_scaleDc = CreateCompatibleDC(screen);
    void* scaleBits = nullptr;
    g_scaleDib = CreateDIBSection(screen, &si, DIB_RGB_COLORS, &scaleBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!g_memDc || !g_dib || !g_bits || !g_textDc || !g_textDib || !g_textBits ||
        !g_scaleDc || !g_scaleDib)
        return false;
    g_oldBmp = SelectObject(g_memDc, g_dib);
    g_textOldBmp = SelectObject(g_textDc, g_textDib);
    g_scaleOldBmp = SelectObject(g_scaleDc, g_scaleDib);
    memset(g_bits, 0, (size_t)kW * kH * 4);
    memset(g_textBits, 0, (size_t)kW * kH * 4);
    InitLamps();
    return true;
}

static void FreeFrameBuffer() {
    if (g_memDc && g_oldBmp) SelectObject(g_memDc, g_oldBmp);
    if (g_textDc && g_textOldBmp) SelectObject(g_textDc, g_textOldBmp);
    if (g_scaleDc && g_scaleOldBmp) SelectObject(g_scaleDc, g_scaleOldBmp);
    if (g_dib) DeleteObject(g_dib);
    if (g_textDib) DeleteObject(g_textDib);
    if (g_scaleDib) DeleteObject(g_scaleDib);
    if (g_memDc) DeleteDC(g_memDc);
    if (g_textDc) DeleteDC(g_textDc);
    if (g_scaleDc) DeleteDC(g_scaleDc);
    g_dib = g_textDib = g_scaleDib = nullptr;
    g_memDc = g_textDc = g_scaleDc = nullptr;
    g_bits = g_textBits = nullptr;
    g_oldBmp = g_textOldBmp = g_scaleOldBmp = nullptr;
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Explorer 重启后会广播 "TaskbarCreated"，此时必须重新 NIM_ADD，否则托盘图标丢失
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        TrayAdd();
        TrayUpdateTip();
        EnsureWindowVisible();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, kTimerMs, nullptr);
        return 0;

    case WM_TIMER: {
        if (wParam != 1) break;
        if (g_topmost) ApplyTopmost();

        g_cpu = SampleCpu();
        g_mem = SampleMem();
        g_disk = SampleDisk();
        g_gpu = SampleGpu();
        double bpsIn = 0.0, bpsOut = 0.0;
        SampleNet(&g_down, &g_up, &bpsIn, &bpsOut);   // 下载=入站，上传=出站
        // 记录未平滑的瞬时值，供六条状态条画趋势；bpsOut=上传，bpsIn=下载
        RecordHistory(g_cpu, g_mem, g_disk, g_up, g_down, g_gpu, bpsOut, bpsIn);
        if (g_tick == 0) {
            g_cpuS = g_cpu; g_memS = g_mem; g_diskS = g_disk;
            g_upS = g_up; g_downS = g_down; g_gpuS = g_gpu;
        } else {
            g_cpuS  = kEmaA * g_cpu  + (1 - kEmaA) * g_cpuS;
            g_memS  = kEmaA * g_mem  + (1 - kEmaA) * g_memS;
            g_diskS = kEmaA * g_disk + (1 - kEmaA) * g_diskS;
            g_upS   = kEmaA * g_up   + (1 - kEmaA) * g_upS;
            g_downS = kEmaA * g_down + (1 - kEmaA) * g_downS;
            g_gpuS  = kEmaA * g_gpu  + (1 - kEmaA) * g_gpuS;
        }
        RenderPanel();
        TrayUpdateTip();
        ++g_tick;
        if ((g_tick % kIconEvery) == 0) TrayUpdateIcon();
        if ((g_tick % 16) == 0) EnsureWindowVisible();   // 每 4 秒兜底：窗口跑出屏幕就拉回
        return 0;
    }

    case WM_DISPLAYCHANGE:        // 改分辨率 / 拔外接屏
    case WM_DPICHANGED:           // 改缩放
        EnsureWindowVisible();
        return 0;

    case kQueryLangMsg:          // 二次启动的新进程来问：现在用的是哪套语言？
        return (LRESULT)g_lang;

    case kAlreadyMsg:
        EnsureWindowVisible();   // 第二次启动 = 把旧窗口叫回桌面，而不只是弹提示
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        ApplyTopmost();
        TrayUpdateTip();
        {
            g_nid.uFlags = NIF_ICON | NIF_INFO | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
            wcsncpy(g_nid.szInfoTitle, T().appTitle, 63);
            g_nid.szInfoTitle[63] = 0;
            wcsncpy(g_nid.szInfo, T().balloonInfo, 255);
            g_nid.szInfo[255] = 0;
            g_nid.dwInfoFlags = NIIF_INFO;
            if (g_trayAdded) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
        return 0;

    case kTrayMsg:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU ||
            lParam == WM_LBUTTONUP || lParam == WM_RBUTTONDOWN) {
            ShowContextMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            g_showText = !g_showText;
            SaveShowText();
            RenderPanel();
        }
        return 0;

    case WM_COMMAND:
        HandleMenuCmd((int)LOWORD(wParam));
        return 0;

    case WM_NOTIFY: {
        const NMHDR* nm = (const NMHDR*)lParam;
        if (nm->code == TTN_GETDISPINFOW && nm->hwndFrom == g_hwndTip &&
            nm->idFrom < 6 + kBandCount &&
            (nm->idFrom < 6 ? BallsVisible() : BandsVisible())) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lParam;
            // 正文 = 指标数值 + 三行系统信息，远超 szText 的 80 wchar，
            // 改由 lpszText 指向静态大缓冲，交给 tooltip 控件复制显示。
            wchar_t metric[240] = {}, sys[288] = {};
            if (nm->idFrom < 6) {
                static const float* const kVals[6] = { &g_cpuS, &g_memS, &g_diskS, &g_upS, &g_downS, &g_gpuS };
                swprintf(metric, 240, T().ballTipFmt,
                         T().ballName[nm->idFrom], *kVals[nm->idFrom]);
            } else {
                const int b = (int)nm->idFrom - 6;
                float avg = 0.f, peak = 0.f, cur = 0.f;
                HistStats(b, &avg, &peak, &cur);
                // 只有两条网速条显示真实速率，其余（含 GPU）显示百分比
                if (b < kNetBand0 || b >= kNetBand0 + kNetBandCount) {
                    swprintf(metric, 240, T().bandTipFmt,
                             T().bandName[b], avg, peak, cur);
                } else {
                    double ra = 0.0, rp = 0.0, rc = 0.0;
                    HistBpsStats(b, &ra, &rp, &rc);
                    wchar_t sa[24], sp[24], sc[24];
                    FormatRate(ra, sa, 24); FormatRate(rp, sp, 24); FormatRate(rc, sc, 24);
                    swprintf(metric, 240, T().netTipFmt,
                             T().bandName[b], sa, sp, sc, avg, peak, cur);
                }
            }
            FormatSysInfo(sys, 288);
            swprintf(g_tipFull, 768, L"%ls\n%ls", metric, sys);
            di->lpszText = g_tipFull;
            return 0;
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        // 在球或状态条上滚轮：向上放大、向下缩小，一次切一档。
        // 高精度滚动条一次不足 120，先累计到一格再切，避免抖动。
        static int wheelAcc = 0;
        wheelAcc += GET_WHEEL_DELTA_WPARAM(wParam);
        if (wheelAcc >= WHEEL_DELTA) {
            wheelAcc -= WHEEL_DELTA;
            ApplyZoom(g_zoomIdx + 1, true);
        } else if (wheelAcc <= -WHEEL_DELTA) {
            wheelAcc += WHEEL_DELTA;
            ApplyZoom(g_zoomIdx - 1, true);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        // 双击序列的第二次 DOWN 已经 SetCapture；这里先退出拖动,
        // 免得随后的 WM_LBUTTONUP 把临时放大后的坐标写进注册表。
        g_dragging = false;
        ReleaseCapture();
        ToggleCenterZoom();
        return 0;

    case WM_LBUTTONDOWN:
        g_dragging = true;
        SetCapture(hwnd);
        GetCursorPos(&g_dragOff);
        g_dragOff.x -= g_winX;
        g_dragOff.y -= g_winY;
        return 0;

    case WM_MOUSEMOVE:
        if (!g_dragging) break;
        {
            POINT pt{};
            GetCursorPos(&pt);
            g_winX = pt.x - g_dragOff.x;
            g_winY = pt.y - g_dragOff.y;
            if (g_topmost) ApplyTopmost();
            RenderPanel();
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false; ReleaseCapture();
            // 临时放大时拖动不改变持久位置，退出双击后回到原位。
            if (!g_centerExpanded)
                SaveWindowPos();   // 拖动结束记一次，避免 MOUSEMOVE 高频写注册表
        }
        return 0;

    case WM_RBUTTONUP:
        ShowContextMenu(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        TrayRemove();
        if (g_hMutex) {
            ReleaseMutex(g_hMutex);
            CloseHandle(g_hMutex);
            g_hMutex = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT(WINAPI* Fn)(int);
        auto fn = (Fn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2);
        FreeLibrary(shcore);
    } else {
        SetProcessDPIAware();
    }

    g_hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const DWORD mutexErr = g_hMutex ? GetLastError() : GetLastError();
    if (g_hMutex && mutexErr == ERROR_ALREADY_EXISTS) {
        HWND exist = FindWindowW(kClassName, kBallTitle);
        if (exist) {
            // 弹窗文字跟随已运行实例的当前语言，避免英文界面却弹中文提示
            DWORD_PTR lang = kLangZh;
            if (SendMessageTimeoutW(exist, kQueryLangMsg, 0, 0,
                                    SMTO_ABORTIFHUNG, 300, &lang) && lang == kLangEn)
                g_lang = kLangEn;
            PostMessageW(exist, kAlreadyMsg, 0, 0);
        }
        MessageBoxW(nullptr, T().alreadyRunning, T().appTitle,
                    MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
        return 0;
    }
    if (!g_hMutex) return 1;
    // 在窗口尺寸 / 悬停区计算之前恢复上次的选择。二次启动走上面的早返回分支，
    // 「已在运行」弹窗语言由旧实例经 kQueryLangMsg 告知，不受这里的持久化影响。
    LoadViewMode();
    LoadLang();
    LoadZoom();
    LoadBoolPrefs();   // 显示百分比 / 始终置顶 / 网速形状

    if (!InitFrameBuffer()) {
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
        return 1;
    }
    InitDiskCounter();
    InitGpuCounter();

    // 统一字号：按 CPU 大球取值，中排灯也能容纳 "100%"
    const int fontPx = (int)(kLampDCpu * 0.26f);
    g_font = CreateFontW(-fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    // 底部网速小球用的小号字
    const int fontPxSmall = (int)(kLampDNet * 0.32f);
    g_fontSmall = CreateFontW(-fontPxSmall, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    // 启动位置：优先恢复上次拖动 / 缩放后的位置；读不到或已经落到屏幕外
    // （拔了外接屏、改分辨率）才回退到默认的屏幕右上角。
    bool posRestored = false;
    int savedX = 0, savedY = 0;
    if (LoadWindowPos(&savedX, &savedY)) {
        RECT wr = { savedX, savedY, savedX + OutW(), savedY + OutViewH() };
        if (MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != nullptr) {
            g_winX = savedX; g_winY = savedY;
            posRestored = true;
        }
    }
    if (!posRestored) {
        g_winX = wa.right - OutW() - 24;
        g_winY = wa.top + 24;
        if (g_winX < wa.left + 8) g_winX = wa.left + 8;
        if (g_winY + OutViewH() > wa.bottom - 8) g_winY = wa.bottom - OutViewH() - 8;
        SaveWindowPos();   // 回退后写回，避免下次启动重复走屏幕外校验
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;   // 面板双击需要 WM_LBUTTONDBLCLK
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | (g_topmost ? WS_EX_TOPMOST : 0),
        kClassName, kBallTitle, WS_POPUP,
        g_winX, g_winY, OutW(), OutViewH(),
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) {
        if (g_hMutex) { CloseHandle(g_hMutex); g_hMutex = nullptr; }
        return 1;
    }

    // 优先使用嵌入的红绿蓝三球图标；失败则动态绘制
    g_hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    if (!g_hIcon) g_hIcon = CreateTrayIcon(0.f);
    if (!g_hIcon) g_hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    TrayAdd();

    // 鼠标悬停提示：每个球 / 每条状态条独立气泡，TTN_GETDISPINFO 回调填数值
    InitTooltip();

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SampleDisk();
    RenderPanel();
    ApplyTopmost();

    MSG msg;
    while (g_running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    TrayRemove();
    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    FreeFrameBuffer();
    if (g_pdhQuery) {
        PdhCloseQuery(g_pdhQuery);
        g_pdhQuery = nullptr;
        g_pdhDisk = nullptr;
    }
    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
    }
    return 0;
}

#ifdef __MINGW32__
int main() {
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOW);
}
#endif
