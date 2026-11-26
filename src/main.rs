// ball — Windows 桌面负载监视器（Rust / 原生 Win32 GDI 重写版）
// 由 C++ 版逐功能移植：六球实时 + 六条 60s 历史带、中英双语、滚轮缩放、
// 双击 4.5x 居中放大、托盘、偏好持久化、单实例。零第三方运行时依赖。
#![allow(non_snake_case, non_camel_case_types, non_upper_case_globals, static_mut_refs, dead_code)]
#![windows_subsystem = "windows"]

use std::ffi::c_void;
use std::mem::{size_of, zeroed};
use std::ptr::{null, null_mut};

use windows_sys::core::BOOL;
use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::System::LibraryLoader::*;
use windows_sys::Win32::System::Performance::*;
use windows_sys::Win32::System::Registry::*;
use windows_sys::Win32::System::SystemInformation::*;
use windows_sys::Win32::System::Threading::*;
use windows_sys::Win32::System::Time::*;
use windows_sys::Win32::System::WindowsProgramming::*;
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::Shell::*;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;
use windows_sys::Win32::NetworkManagement::IpHelper::*;

// ===========================================================================
// 常量
// ===========================================================================
const LAMP_D_BASE: i32 = 58;
const LAMP_D_CPU: i32 = 75;
const LAMP_D_NET: i32 = 34;
const MARGIN: i32 = 3;
const GAP_X: i32 = 4;
const W: i32 = MARGIN * 2 + LAMP_D_BASE * 2 + GAP_X; // 126
const BALL_AREA_H: i32 = 182;
const HIST_N: usize = 240;
const BAND_TOP: i32 = BALL_AREA_H + 4; // 186
const BAND_H: i32 = 20;
const BAND_GAP: i32 = 3;
const BAND_LABEL_H: i32 = 0;
const BAND_COUNT: usize = 6;
const NET_BAND0: usize = 3;
const NET_BAND_COUNT: usize = 2;
const H: i32 = BAND_TOP + (BAND_COUNT as i32) * (BAND_H + BAND_GAP) + 2; // 326
const ZOOM_LEVELS: [f32; 8] = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0];
const ZOOM_DEFAULT: usize = 2;
const ZOOM_MAX: f32 = 3.0;
const CENTER_FACTOR: f32 = 1.5;
const TIMER_MS: u32 = 250;
const EMA_A: f32 = 0.35;
const TRAY_ID: u32 = 0x1C01;
const TRAY_MSG: u32 = WM_APP_CONST + 1;
const ALREADY_MSG: u32 = WM_APP_CONST + 3;
const QUERY_LANG_MSG: u32 = WM_APP_CONST + 4;
const WM_APP_CONST: u32 = 0x8000;
const ICON_EVERY: u64 = 4;
const ALPHA_BODY: u8 = 150;
const ALPHA_RING: u8 = 210;
const ALPHA_TEXT: u8 = 235;
const CLASS_NAME: &str = "CpuBallWindowClass";
const MUTEX_NAME: &str = "Local\\CpuBall_SingleInstance_v1";
const BALL_TITLE: &str = "CPU Ball";
const REG_PATH: &str = "Software\\CpuBall";
const IDI_APP: u16 = 101;

const IDM_TOGGLE_TEXT: u32 = 2001;
const IDM_TOGGLE_TOP: u32 = 2002;
const IDM_EXIT: u32 = 2003;
const IDM_TOGGLE_NETSHAPE: u32 = 2004;
const IDM_RESET_POS: u32 = 2005;
const IDM_VIEW_BOTH: u32 = 2006;
const IDM_VIEW_BALLS: u32 = 2007;
const IDM_VIEW_BANDS: u32 = 2008;
const IDM_LANG_ZH: u32 = 2009;
const IDM_LANG_EN: u32 = 2010;
const IDM_ABOUT: u32 = 2011;
const IDM_ZOOM_IN: u32 = 2012;
const IDM_ZOOM_OUT: u32 = 2013;
const IDM_ZOOM_RESET: u32 = 2014;
const IDM_RESET_ALL: u32 = 2015;

// 显示模式 / 语言
const VIEW_BOTH: i32 = 0;
const VIEW_BALLS: i32 = 1;
const VIEW_BANDS: i32 = 2;
const LANG_ZH: i32 = 0;
const LANG_EN: i32 = 1;

// ===========================================================================
// 工具：宽字符串 / 格式化 / 数学
// ===========================================================================
fn w(s: &str) -> Vec<u16> {
    let mut v: Vec<u16> = s.encode_utf16().collect();
    v.push(0);
    v
}
fn clamp01(t: f32) -> f32 { if t < 0.0 { 0.0 } else if t > 1.0 { 1.0 } else { t } }
fn clampb(v: i32) -> u8 { if v < 0 { 0 } else if v > 255 { 255 } else { v as u8 } }

#[derive(Clone, Copy)]
struct Rgb { r: i32, g: i32, b: i32 }
fn load_color(pct: f32) -> Rgb {
    let p = if pct < 0.0 { 0.0 } else if pct > 100.0 { 100.0 } else { pct };
    if p < 25.0 { Rgb { r: 46, g: 204, b: 113 } }
    else if p < 50.0 { Rgb { r: 241, g: 196, b: 15 } }
    else if p < 75.0 { Rgb { r: 230, g: 126, b: 34 } }
    else { Rgb { r: 231, g: 76, b: 60 } }
}
fn blend(a: Rgb, b: Rgb, t: f32) -> Rgb {
    let t = clamp01(t);
    Rgb {
        r: (a.r as f32 + (b.r - a.r) as f32 * t + 0.5) as i32,
        g: (a.g as f32 + (b.g - a.g) as f32 * t + 0.5) as i32,
        b: (a.b as f32 + (b.b - a.b) as f32 * t + 0.5) as i32,
    }
}
// BGRA 像素混合（DIB 32bpp，alpha 通道直写）
fn blend_pixel(px: &mut [u8], r: i32, g: i32, b: i32, a: u8) {
    if a == 0 { return; }
    if a == 255 {
        px[0] = b as u8; px[1] = g as u8; px[2] = r as u8; px[3] = 255;
        return;
    }
    let ia = 255i32 - a as i32;
    let da = px[3] as i32;
    let oa = a as i32 + (da * ia) / 255;
    if oa <= 0 { px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 0; return; }
    px[0] = clampb((b as i32 * a as i32 + px[0] as i32 * da * ia / 255) / oa);
    px[1] = clampb((g as i32 * a as i32 + px[1] as i32 * da * ia / 255) / oa);
    px[2] = clampb((r as i32 * a as i32 + px[2] as i32 * da * ia / 255) / oa);
    px[3] = clampb(oa);
}

// ===========================================================================
// 注册表
// ===========================================================================
fn reg_get(name: &str) -> Option<u32> {
    unsafe {
        let path = w(REG_PATH);
        let nm = w(name);
        let mut hk: HKEY = null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, path.as_ptr(), 0, KEY_READ, &mut hk) != ERROR_SUCCESS {
            return None;
        }
        let mut v: u32 = 0;
        let mut cb = size_of::<u32>() as u32;
        let mut ty: u32 = 0;
        let ok = RegQueryValueExW(hk, nm.as_ptr(), null(), &mut ty, &mut v as *mut _ as *mut u8, &mut cb) == ERROR_SUCCESS
            && ty == REG_DWORD && cb == size_of::<u32>() as u32;
        RegCloseKey(hk);
        if ok { Some(v) } else { None }
    }
}
fn reg_set(name: &str, v: u32) {
    unsafe {
        let path = w(REG_PATH);
        let nm = w(name);
        let mut hk: HKEY = null_mut();
        let mut disp: u32 = 0;
        if RegCreateKeyExW(HKEY_CURRENT_USER, path.as_ptr(), 0, null(), 0, KEY_WRITE, null(), &mut hk, &mut disp) != ERROR_SUCCESS {
            return;
        }
        RegSetValueExW(hk, nm.as_ptr(), 0, REG_DWORD, &v as *const _ as *const u8, size_of::<u32>() as u32);
        RegCloseKey(hk);
    }
}
fn reg_delete(name: &str) {
    unsafe {
        let path = w(REG_PATH);
        let nm = w(name);
        let mut hk: HKEY = null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, path.as_ptr(), 0, KEY_SET_VALUE, &mut hk) == ERROR_SUCCESS {
            RegDeleteValueW(hk, nm.as_ptr());
            RegCloseKey(hk);
        }
    }
}

// ===========================================================================
// 文案（中 / 英）：纯文本直接用 &'static str，带格式的在下面用 format! 分支
// ===========================================================================
struct Ui {
    app_title: &'static str,
    ball_names: [&'static str; 6],
    band_names: [&'static str; 6],
    menu_percent: &'static str,
    menu_view_both: &'static str,
    menu_view_balls: &'static str,
    menu_view_bands: &'static str,
    menu_topmost: &'static str,
    menu_net_to_round: &'static str,
    menu_net_to_pill: &'static str,
    menu_reset: &'static str,
    menu_reset_all: &'static str,
    menu_zoom_in: &'static str,
    menu_zoom_out: &'static str,
    menu_exit: &'static str,
    balloon_info: &'static str,
    already_running: &'static str,
    menu_about: &'static str,
    about_title: &'static str,
}
const UI_ZH: Ui = Ui {
    app_title: "负载灯",
    ball_names: ["CPU 负载", "内存占用", "磁盘繁忙", "上传网速", "下载网速", "GPU 使用率"],
    band_names: ["CPU", "内存", "磁盘", "上传网速", "下载网速", "GPU"],
    menu_percent: "显示百分比",
    menu_view_both: "显示球 + 状态条",
    menu_view_balls: "只显示六个球",
    menu_view_bands: "只显示六个状态条",
    menu_topmost: "始终置顶",
    menu_net_to_round: "网速形状→圆形",
    menu_net_to_pill: "网速形状→圆角",
    menu_reset: "重置窗口位置",
    menu_reset_all: "重置所有偏好",
    menu_zoom_in: "放大（滚轮上）",
    menu_zoom_out: "缩小（滚轮下）",
    menu_exit: "退出",
    balloon_info: "程序已在运行，窗口已恢复到屏幕可见区域。",
    already_running: "负载监视器已在运行。\n\n已把窗口恢复到屏幕右上角；\n右键托盘图标可切换显示模式、语言、百分比、置顶、网速形状、\n重置位置、重置所有偏好、滚轮缩放、查看关于信息或退出。",
    menu_about: "关于",
    about_title: "关于 负载灯",
};
const UI_EN: Ui = Ui {
    app_title: "Load Monitor",
    ball_names: ["CPU Load", "Memory Usage", "Disk Activity", "Upload", "Download", "GPU Usage"],
    band_names: ["CPU", "Memory", "Disk", "Upload", "Download", "GPU"],
    menu_percent: "Show percentage",
    menu_view_both: "Balls + status bars",
    menu_view_balls: "Balls only",
    menu_view_bands: "Status bars only",
    menu_topmost: "Always on top",
    menu_net_to_round: "Net shape → Round",
    menu_net_to_pill: "Net shape → Rounded",
    menu_reset: "Reset window position",
    menu_reset_all: "Reset all preferences",
    menu_zoom_in: "Zoom in (wheel up)",
    menu_zoom_out: "Zoom out (wheel down)",
    menu_exit: "Exit",
    balloon_info: "Already running. Window restored to the visible screen area.",
    already_running: "Load Monitor is already running.\n\nThe window has been restored to the top-right corner.\nRight-click the tray icon to switch view mode, language, percentage text, always-on-top, net shape, reset position, reset all preferences, zoom with the wheel, view About, or exit.",
    menu_about: "About",
    about_title: "About Load Monitor",
};
fn ui(lang: i32) -> &'static Ui { if lang == LANG_EN { &UI_EN } else { &UI_ZH } }

fn fmt_zoom(lang: i32, pct: f32) -> String {
    if lang == LANG_EN { format!("Zoom {:.0}% (click to reset)", pct) }
    else { format!("缩放 {:.0}%（点击重置）", pct) }
}
fn fmt_tray(lang: i32, t: &Ui, v: [f32; 6]) -> String {
    if lang == LANG_EN {
        format!("{}  CPU {:.0}%  MEM {:.0}%  DISK {:.0}%  GPU {:.0}%  UP {:.0}%  DOWN {:.0}%",
            t.app_title, v[0], v[1], v[2], v[5], v[3], v[4])
    } else {
        format!("{}  CPU {:.0}%  内存 {:.0}%  磁盘 {:.0}%  GPU {:.0}%  上 {:.0}%  下 {:.0}%",
            t.app_title, v[0], v[1], v[2], v[5], v[3], v[4])
    }
}
fn fmt_ball_tip(lang: i32, name: &str, val: f32) -> String {
    if lang == LANG_EN { format!("{}\nNow {:.0}% (live; green → red)", name, val) }
    else { format!("{}\n当前 {:.0}%（实时值，绿→黄→橙→红，越忙越红）", name, val) }
}
fn fmt_band_tip(lang: i32, name: &str, avg: f32, peak: f32, cur: f32) -> String {
    if lang == LANG_EN { format!("{} 60s\nAvg/Pk/Now {:.0}/{:.0}/{:.0}%", name, avg, peak, cur) }
    else { format!("{} 最近 60 秒\n平均 {:.0}%  峰值 {:.0}%  当前 {:.0}%", name, avg, peak, cur) }
}
fn fmt_net_tip(lang: i32, name: &str, sa: &str, sp: &str, sc: &str, avg: f32, peak: f32, cur: f32) -> String {
    if lang == LANG_EN { format!("{} 60s\nAvg {} Pk {} Now {}\nLoad {:.0}/{:.0}/{:.0}", name, sa, sp, sc, avg, peak, cur) }
    else { format!("{} 最近 60 秒\n平均 {} 峰值 {} 当前 {}\n繁忙度 {:.0}% / {:.0}% / {:.0}%", name, sa, sp, sc, avg, peak, cur) }
}
fn fmt_rate(bps: f64) -> String {
    if bps < 1024.0 { format!("{:.0} B/s", bps) }
    else if bps < 1024.0 * 1024.0 { format!("{:.1} KB/s", bps / 1024.0) }
    else { format!("{:.2} MB/s", bps / (1024.0 * 1024.0)) }
}
fn build_date() -> String { "2026-11-26".to_string() }
fn fmt_about(lang: i32) -> String {
    let v = "1.0.0";
    let d = build_date();
    if lang == LANG_EN {
        format!("Load Monitor\nVersion {}    Build date {}\n\nDeveloped by jxs using WorkBuddy\nBuilt with Rust / native Win32 GDI\nSingle-file EXE, no third-party DLLs\n\nSix live lamps: CPU / Memory / Disk / Upload / Download / GPU\nSix status bars: rolling 60-second history\nWheel or right-click menu zooms the panel (50%-300%)\n\n(c) 2026 jxs. All rights reserved", v, d)
    } else {
        format!("负载灯  Load Monitor\n版本 {}    编译日期 {}\n\n开发：jxs using WorkBuddy\n构建：Rust / 原生 Win32 GDI，静态编译\n发布：单文件 EXE，零第三方依赖（仅调用 Windows 系统 DLL）\n\n功能：六个负载球实时显示 CPU / 内存 / 磁盘 / 上传 / 下载 / GPU，\n      六条状态条记录最近 60 秒的运行趋势。\n      滚轮或右键菜单可缩放整个面板（50%~300%）。\n\n© 2026 jxs 版权所有", v, d)
    }
}

// 系统信息：计算机名 / 系统版本 / 开机时间
fn os_version_text() -> String {
    unsafe {
        let key = w("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
        let mut hk: HKEY = null_mut();
        if RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.as_ptr(), 0, KEY_READ, &mut hk) == ERROR_SUCCESS {
            let mut product = [0u16; 128];
            let mut disp = [0u16; 64];
            let mut build = [0u16; 32];
            let mut ubr: u32 = 0;
            let mut cb = (product.len() * 2) as u32;
            let mut ty = 0u32;
            RegQueryValueExW(hk, w("ProductName").as_ptr(), null(), &mut ty, product.as_mut_ptr() as *mut u8, &mut cb);
            cb = (disp.len() * 2) as u32;
            RegQueryValueExW(hk, w("DisplayVersion").as_ptr(), null(), &mut ty, disp.as_mut_ptr() as *mut u8, &mut cb);
            cb = (build.len() * 2) as u32;
            RegQueryValueExW(hk, w("CurrentBuild").as_ptr(), null(), &mut ty, build.as_mut_ptr() as *mut u8, &mut cb);
            cb = 4;
            RegQueryValueExW(hk, w("UBR").as_ptr(), null(), &mut ty, &mut ubr as *mut _ as *mut u8, &mut cb);
            RegCloseKey(hk);
            let ps = wstr(&product);
            if !ps.is_empty() {
                let ds = wstr(&disp);
                let bs = wstr(&build);
                if !ds.is_empty() && !bs.is_empty() { return format!("{} {} ({}.{})", ps, ds, bs, ubr); }
                if !ds.is_empty() { return format!("{} {}", ps, ds); }
                if !bs.is_empty() { return format!("{} ({})", ps, bs); }
                return ps;
            }
        }
        // 回退 RtlGetVersion
        let ntdll = w("ntdll.dll");
        let h = LoadLibraryW(ntdll.as_ptr());
        if h.is_null() {
            let p = GetProcAddress(h, b"RtlGetVersion\0".as_ptr());
            if let Some(f) = p {
                type Fn = unsafe extern "system" fn(*mut OsVer) -> i32;
                let mut vi: OsVer = zeroed();
                vi.size = size_of::<OsVer>() as u32;
                unsafe { std::mem::transmute::<_, Fn>(f)(&mut vi) };
                FreeLibrary(h);
                return format!("Windows {}.{} (build {})", vi.major, vi.minor, vi.build);
            }
            FreeLibrary(h);
        }
        "Windows".to_string()
    }
}
#[repr(C)]
struct OsVer { size: u32, major: u32, minor: u32, build: u32, platform: u32, csd: [u16; 128], flags: u32, suite: u16, product: u16 }

fn wstr(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..end])
}
fn sys_info_text(lang: i32) -> String {
    unsafe {
        let mut pc = [0u16; 64];
        let mut n = 64u32;
        if GetComputerNameW(pc.as_mut_ptr(), &mut n) == 0 { return "计算机：-".to_string(); }
        let pc = wstr(&pc);
        let os = os_version_text();
        let ms = GetTickCount64();
        let mut now: SYSTEMTIME = zeroed();
        GetLocalTime(&mut now);
        let mut ft: FILETIME = zeroed();
        SystemTimeToFileTime(&now, &mut ft);
        let mut u64v = ((ft.dwHighDateTime as u64) << 32) | ft.dwLowDateTime as u64;
        u64v = u64v.wrapping_sub(ms * 10000);
        let mut bt: FILETIME = zeroed();
        bt.dwLowDateTime = u64v as u32;
        bt.dwHighDateTime = (u64v >> 32) as u32;
        let mut st: SYSTEMTIME = zeroed();
        FileTimeToSystemTime(&bt, &mut st);
        let sec = ms / 1000;
        let days = sec / 86400;
        let hours = (sec % 86400) / 3600;
        let mins = (sec % 3600) / 60;
        let boot = if lang == LANG_EN {
            format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02} (up {}d {:02}h {:02}m)",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, days, hours, mins)
        } else {
            format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}（已运行 {} 天 {:02} 小时 {:02} 分）",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, days, hours, mins)
        };
        if lang == LANG_EN { format!("Computer: {}\nOS: {}\nBoot: {}", pc, os, boot) }
        else { format!("计算机：{}\n系统：{}\n开机：{}", pc, os, boot) }
    }
}

// ===========================================================================
// 采样
// ===========================================================================
struct Sampler {
    prev_idle: u64, prev_total: u64, cpu_seeded: bool,
    pdh_query: PDH_HQUERY, pdh_disk: PDH_HCOUNTER, pdh_gpu: PDH_HCOUNTER,
    disk_is_bytes: bool, gpu_buf: Vec<u8>,
    prev_in: u64, prev_out: u64, last_net_ms: u64, net_seeded: bool,
}
impl Sampler {
    fn new() -> Self {
        Self { prev_idle: 0, prev_total: 0, cpu_seeded: false,
            pdh_query: null_mut(), pdh_disk: null_mut(), pdh_gpu: null_mut(), disk_is_bytes: false,
            gpu_buf: Vec::new(), prev_in: 0, prev_out: 0, last_net_ms: 0, net_seeded: false }
    }
    fn init_counters(&mut self) {
        unsafe {
            if PdhOpenQueryW(null(), 0, &mut self.pdh_query) != ERROR_SUCCESS {
                self.pdh_query = null_mut(); return;
            }
            if PdhAddEnglishCounterW(self.pdh_query, w("\\PhysicalDisk(_Total)\\% Disk Time").as_ptr(), 0, &mut self.pdh_disk) == ERROR_SUCCESS {
                self.disk_is_bytes = false;
            } else if PdhAddEnglishCounterW(self.pdh_query, w("\\PhysicalDisk(_Total)\\Disk Bytes/sec").as_ptr(), 0, &mut self.pdh_disk) == ERROR_SUCCESS {
                self.disk_is_bytes = true;
            } else { self.pdh_disk = null_mut(); }
            if PdhAddEnglishCounterW(self.pdh_query, w("\\GPU Engine(*)\\Utilization Percentage").as_ptr(), 0, &mut self.pdh_gpu) != ERROR_SUCCESS {
                self.pdh_gpu = null_mut();
            }
            // 预热一次
            if self.pdh_query.is_null() { PdhCollectQueryData(self.pdh_query); }
        }
    }
    fn cpu(&mut self) -> f32 {
        unsafe {
            let mut idle: FILETIME = zeroed();
            let mut kernel: FILETIME = zeroed();
            let mut user: FILETIME = zeroed();
            if GetSystemTimes(&mut idle, &mut kernel, &mut user) == 0 { return 0.0; }
            let u64v = |ft: &FILETIME| ((ft.dwHighDateTime as u64) << 32) | ft.dwLowDateTime as u64;
            let iu = u64v(&idle);
            let total = u64v(&kernel).wrapping_add(u64v(&user));
            if !self.cpu_seeded {
                self.prev_idle = iu; self.prev_total = total; self.cpu_seeded = true;
                return 0.0;
            }
            let di = iu as i64 - self.prev_idle as i64;
            let dt = total as i64 - self.prev_total as i64;
            self.prev_idle = iu; self.prev_total = total;
            if dt <= 0 { return 0.0; }
            let busy = (dt - di).max(0);
            let pct = 100.0 * busy as f32 / dt as f32;
            pct.clamp(0.0, 100.0)
        }
    }
    fn mem(&self) -> f32 {
        unsafe {
            let mut ms: MEMORYSTATUSEX = zeroed();
            ms.dwLength = size_of::<MEMORYSTATUSEX>() as u32;
            if GlobalMemoryStatusEx(&mut ms) == 0 { return 0.0; }
            (ms.dwMemoryLoad as f32).clamp(0.0, 100.0)
        }
    }
    fn disk(&mut self) -> f32 {
        unsafe {
            if self.pdh_query.is_null() || self.pdh_disk.is_null() { return 0.0; }
            if PdhCollectQueryData(self.pdh_query) != ERROR_SUCCESS { return 0.0; }
            let mut v: PDH_FMT_COUNTERVALUE = zeroed();
            if PdhGetFormattedCounterValue(self.pdh_disk, PDH_FMT_DOUBLE, null_mut(), &mut v) != ERROR_SUCCESS { return 0.0; }
            if v.CStatus != ERROR_SUCCESS { return 0.0; }
            let mut pct = v.Anonymous.doubleValue;
            if self.disk_is_bytes {
                let bytes = pct;
                pct = if bytes <= 1.0 { 0.0 } else { 25.0 * ((bytes / (1024.0 * 1024.0)).log10() + 2.0) };
            }
            (pct as f32).clamp(0.0, 100.0)
        }
    }
    fn gpu(&mut self) -> f32 {
        unsafe {
            if self.pdh_gpu.is_null() { return 0.0; }
            let mut size = 0u32; let mut count = 0u32;
            if PdhGetFormattedCounterArrayW(self.pdh_gpu, PDH_FMT_DOUBLE, &mut size, &mut count, null_mut()) != PDH_MORE_DATA || size == 0 { return 0.0; }
            if self.gpu_buf.len() < size as usize { self.gpu_buf.resize(size as usize, 0); }
            let mut sz = size;
            if PdhGetFormattedCounterArrayW(self.pdh_gpu, PDH_FMT_DOUBLE, &mut sz, &mut count, self.gpu_buf.as_mut_ptr() as *mut PDH_FMT_COUNTERVALUE_ITEM_W) != ERROR_SUCCESS { return 0.0; }
            let items = self.gpu_buf.as_ptr() as *const PDH_FMT_COUNTERVALUE_ITEM_W;
            let mut sum = 0.0f64;
            for i in 0..count as usize {
                let it = &*items.add(i);
                if it.FmtValue.CStatus == ERROR_SUCCESS && it.FmtValue.Anonymous.doubleValue > 0.0 {
                    sum += it.FmtValue.Anonymous.doubleValue;
                }
            }
            (sum as f32).clamp(0.0, 100.0)
        }
    }
    fn net(&mut self) -> (f32, f32, f64, f64) {
        unsafe {
            let mut sz = 0u32;
            if GetIfTable(null_mut(), &mut sz, FALSE) != ERROR_INSUFFICIENT_BUFFER || sz == 0 { return (0.0, 0.0, 0.0, 0.0); }
            let mut buf = vec![0u8; sz as usize];
            let table = buf.as_mut_ptr() as *mut MIB_IFTABLE;
            if GetIfTable(table, &mut sz, FALSE) != ERROR_SUCCESS { return (0.0, 0.0, 0.0, 0.0); }
            let mut inb = 0u64; let mut outb = 0u64;
            let n = (*table).dwNumEntries as usize;
            for i in 0..n {
                let r = &*(*table).table.as_ptr().add(i);
                if r.dwOperStatus != 6 { continue; } // IF_OPER_STATUS_OPERATIONAL = 6
                if r.dwType == 24 { continue; }       // LOOPBACK
                inb = inb.wrapping_add(r.dwInOctets as u64);
                outb = outb.wrapping_add(r.dwOutOctets as u64);
            }
            let now = GetTickCount64();
            if !self.net_seeded {
                self.prev_in = inb; self.prev_out = outb; self.last_net_ms = now;
                self.net_seeded = true;
                return (0.0, 0.0, 0.0, 0.0);
            }
            let dms = (now - self.last_net_ms).max(1);
            self.last_net_ms = now;
            let dt = dms as f64 / 1000.0;
            let bin = (inb.wrapping_sub(self.prev_in) as f64 / dt).max(0.0);
            let bout = (outb.wrapping_sub(self.prev_out) as f64 / dt).max(0.0);
            self.prev_in = inb; self.prev_out = outb;
            (net_busy(bin), net_busy(bout), bin, bout) // (down, up, inBps, outBps)
        }
    }
}
const ERROR_INSUFFICIENT_BUFFER: u32 = 122;
fn net_busy(bps: f64) -> f32 {
    if bps < 1024.0 { return 0.0; }
    let kb = bps / 1024.0;
    let pct = 25.0 * (kb.log10() - 1.0);
    (pct as f32).clamp(0.0, 100.0)
}

// ===========================================================================
// 全局状态
// ===========================================================================
struct G {
    hwnd: HWND, hwnd_tip: HWND, tip_full: Vec<u16>,
    mem_dc: HDC, text_dc: HDC, scale_dc: HDC,
    dib: HBITMAP, text_dib: HBITMAP, scale_dib: HBITMAP,
    old_bmp: HGDIOBJ, text_old: HGDIOBJ, scale_old: HGDIOBJ,
    bits: *mut u8, text_bits: *mut u8,
    font: HFONT, font_small: HFONT,
    hicon: HICON, nid: NOTIFYICONDATAW, tray_added: bool,
    topmost: bool, show_text: bool, net_pill: bool,
    view_mode: i32, lang: i32, zoom_idx: usize,
    win_x: i32, win_y: i32, dragging: bool, drag_x: i32, drag_y: i32,
    running: bool, tick: u64, wheel_acc: i32,
    center_expanded: bool, center_saved_zoom: usize, center_saved_x: i32, center_saved_y: i32,
    msg_taskbar: u32, mutex: HANDLE,
    sampler: Sampler,
    cpu: f32, mem: f32, disk: f32, up: f32, down: f32, gpu: f32,
    cpu_s: f32, mem_s: f32, disk_s: f32, up_s: f32, down_s: f32, gpu_s: f32,
    hist: [[f32; HIST_N]; BAND_COUNT], hist_bps: [[f64; HIST_N]; NET_BAND_COUNT], hist_count: usize,
    lamp_x: [f32; 6], lamp_y: [f32; 6], lamp_r: [f32; 6], lamp_hw: [f32; 6], lamp_hh: [f32; 6],
    about_hook: HHOOK,
}
impl G {
    fn new() -> Self {
        Self {
            hwnd: null_mut(), hwnd_tip: null_mut(), tip_full: vec![0u16; 768],
            mem_dc: null_mut(), text_dc: null_mut(), scale_dc: null_mut(),
            dib: null_mut(), text_dib: null_mut(), scale_dib: null_mut(),
            old_bmp: null_mut(), text_old: null_mut(), scale_old: null_mut(),
            bits: null_mut(), text_bits: null_mut(), font: null_mut(), font_small: null_mut(),
            hicon: null_mut(), nid: unsafe { zeroed() }, tray_added: false,
            topmost: true, show_text: false, net_pill: false,
            view_mode: VIEW_BANDS, lang: LANG_ZH, zoom_idx: ZOOM_DEFAULT,
            win_x: 80, win_y: 80, dragging: false, drag_x: 0, drag_y: 0,
            running: true, tick: 0, wheel_acc: 0,
            center_expanded: false, center_saved_zoom: ZOOM_DEFAULT, center_saved_x: 0, center_saved_y: 0,
            msg_taskbar: 0, mutex: null_mut(), sampler: Sampler::new(),
            cpu: 0.0, mem: 0.0, disk: 0.0, up: 0.0, down: 0.0, gpu: 0.0,
            cpu_s: 0.0, mem_s: 0.0, disk_s: 0.0, up_s: 0.0, down_s: 0.0, gpu_s: 0.0,
            hist: [[0.0; HIST_N]; BAND_COUNT], hist_bps: [[0.0; HIST_N]; NET_BAND_COUNT], hist_count: 0,
            lamp_x: [0.0; 6], lamp_y: [0.0; 6], lamp_r: [0.0; 6], lamp_hw: [0.0; 6], lamp_hh: [0.0; 6],
            about_hook: null_mut(),
        }
    }
    fn zoom(&self) -> f32 {
        ZOOM_LEVELS[self.zoom_idx] * if self.center_expanded { CENTER_FACTOR } else { 1.0 }
    }
    fn out_w(&self) -> i32 { (W as f32 * self.zoom() + 0.5) as i32 }
    fn zs(&self, v: f32) -> i32 { (v * self.zoom() + 0.5) as i32 }
    fn balls_visible(&self) -> bool { self.view_mode != VIEW_BANDS }
    fn bands_visible(&self) -> bool { self.view_mode != VIEW_BALLS }
    fn src_top(&self) -> i32 { if self.view_mode == VIEW_BANDS { BAND_TOP } else { 0 } }
    fn win_height(&self) -> i32 {
        if self.view_mode == VIEW_BALLS { BALL_AREA_H }
        else if self.view_mode == VIEW_BANDS { H - BAND_TOP }
        else { H }
    }
    fn out_view_h(&self) -> i32 {
        let s = self.zoom();
        ((self.src_top() + self.win_height()) as f32 * s + 0.5) as i32 - (self.src_top() as f32 * s + 0.5) as i32
    }
    fn px(&mut self, x: i32, y: i32) -> &mut [u8] {
        let off = ((y as usize) * W as usize + x as usize) * 4;
        unsafe { std::slice::from_raw_parts_mut(self.bits.add(off), 4) }
    }
    fn save_pos(&self) { reg_set("WinX", self.win_x as u32); reg_set("WinY", self.win_y as u32); }
    fn save_zoom(&self) { reg_set("Zoom", self.zoom_idx as u32); }
    fn save_bool(&self) {
        reg_set("ShowText", if self.show_text { 1 } else { 0 });
        reg_set("Topmost", if self.topmost { 1 } else { 0 });
        reg_set("NetPill", if self.net_pill { 1 } else { 0 });
    }
}
static mut GLOBAL: Option<G> = None;
unsafe fn g() -> &'static mut G { GLOBAL.as_mut().unwrap() }

fn init_lamps(gp: &mut G) {
    let rc = LAMP_D_CPU as f32 * 0.5;
    let rb = LAMP_D_BASE as f32 * 0.5;
    let rn = LAMP_D_NET as f32 * 0.5;
    gp.lamp_r = [rc, rb, rb, rn, rn, rn];
    for i in 0..6 { gp.lamp_hw[i] = gp.lamp_r[i]; gp.lamp_hh[i] = gp.lamp_r[i]; }
    gp.lamp_x[0] = W as f32 * 0.5;
    gp.lamp_y[0] = MARGIN as f32 + rc;
    let yb = MARGIN as f32 + 2.0 * rc + 2.0 + rb;
    gp.lamp_y[1] = yb; gp.lamp_y[2] = yb;
    gp.lamp_x[1] = MARGIN as f32 + rb;
    gp.lamp_x[2] = W as f32 - MARGIN as f32 - rb;
    let yn = yb + rb + 4.0 + rn;
    gp.lamp_y[3] = yn; gp.lamp_y[4] = yn; gp.lamp_y[5] = yn;
    relayout_net(gp);
}
fn relayout_net(gp: &mut G) {
    let rn = LAMP_D_NET as f32 * 0.5;
    let xmid = W as f32 * 0.5;
    if gp.net_pill {
        let hw = (W as f32 - 2.0 * MARGIN as f32 - 2.0 * GAP_X as f32) / 6.0;
        let hh = rn;
        for i in 3..6 { gp.lamp_hw[i] = hw; gp.lamp_hh[i] = hh; }
        gp.lamp_x[3] = MARGIN as f32 + hw;
        gp.lamp_x[4] = W as f32 - MARGIN as f32 - hw;
        gp.lamp_x[5] = xmid;
    } else {
        let gap = (W as f32 - 2.0 * MARGIN as f32 - 6.0 * rn) / 2.0;
        for i in 3..6 { gp.lamp_hw[i] = rn; gp.lamp_hh[i] = rn; }
        gp.lamp_x[3] = xmid - 2.0 * rn - gap;
        gp.lamp_x[4] = xmid + 2.0 * rn + gap;
        gp.lamp_x[5] = xmid;
    }
    for i in 3..6 { gp.lamp_r[i] = rn; }
}

// 拉伸缓冲 / 逻辑 DIB 初始化
fn init_frame_buffer(gp: &mut G) -> bool {
    unsafe {
        let mut bi: BITMAPINFO = zeroed();
        bi.bmiHeader.biSize = size_of::<BITMAPINFOHEADER>() as u32;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        let out_max = ZOOM_MAX * CENTER_FACTOR;
        let mut si: BITMAPINFO = zeroed();
        si.bmiHeader.biSize = size_of::<BITMAPINFOHEADER>() as u32;
        si.bmiHeader.biWidth = (W as f32 * out_max + 0.5) as i32;
        si.bmiHeader.biHeight = -((H as f32 * out_max + 0.5) as i32);
        si.bmiHeader.biPlanes = 1;
        si.bmiHeader.biBitCount = 32;
        si.bmiHeader.biCompression = BI_RGB;
        let screen = GetDC(null_mut());
        gp.mem_dc = CreateCompatibleDC(screen);
        gp.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &mut gp.bits as *mut _ as *mut *mut c_void, null_mut(), 0);
        gp.text_dc = CreateCompatibleDC(screen);
        gp.text_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &mut gp.text_bits as *mut _ as *mut *mut c_void, null_mut(), 0);
        gp.scale_dc = CreateCompatibleDC(screen);
        let mut scale_bits: *mut c_void = null_mut();
        gp.scale_dib = CreateDIBSection(screen, &si, DIB_RGB_COLORS, &mut scale_bits, null_mut(), 0);
        ReleaseDC(null_mut(), screen);
        if gp.mem_dc.is_null() || gp.dib.is_null() || gp.bits.is_null() || gp.text_dc.is_null() ||
           gp.text_dib.is_null() || gp.text_bits.is_null() || gp.scale_dc.is_null() || gp.scale_dib.is_null() {
            return false;
        }
        gp.old_bmp = SelectObject(gp.mem_dc, gp.dib);
        gp.text_old = SelectObject(gp.text_dc, gp.text_dib);
        gp.scale_old = SelectObject(gp.scale_dc, gp.scale_dib);
        init_lamps(gp);
        true
    }
}

// ===========================================================================
// 绘制
// ===========================================================================
fn draw_text_centered(gp: &mut G, text: &str, cx: f32, cy: f32, col: Rgb,
                      radius: f32, font: HFONT, clip_hw: f32, clip_hh: f32) {
    if !gp.show_text || gp.text_bits.is_null() || gp.text_dc.is_null() { return; }
    let chw = (if clip_hw > 0.0 { clip_hw } else { radius }) * 0.92;
    let chh = (if clip_hh > 0.0 { clip_hh } else { radius }) * 0.85;
    unsafe {
        std::ptr::write_bytes(gp.text_bits, 0, (W as usize) * (H as usize) * 4);
        SetBkMode(gp.text_dc, TRANSPARENT as i32);
        SetTextColor(gp.text_dc, 0xFFFFFF);
        if !font.is_null() { SelectObject(gp.text_dc, font); }
        let left = (cx - chw) as i32;
        let right = (cx + chw) as i32;
        let top = (cy - chh) as i32;
        let bottom = (cy + chh) as i32;
        let mut rc = RECT { left, top, right, bottom };
        let tw = w(text);
        DrawTextW(gp.text_dc, tw.as_ptr(), -1, &mut rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for y in top..bottom {
            if y < 0 || y >= H { continue; }
            for x in left..right {
                if x < 0 || x >= W { continue; }
                let dx = x as f32 + 0.5 - cx;
                let dy = y as f32 + 0.5 - cy;
                if dx.abs() > chw || dy.abs() > chh { continue; }
                let off = ((y as usize) * W as usize + x as usize) * 4;
                let src = std::slice::from_raw_parts(gp.text_bits.add(off), 4);
                let lum = (src[2] as i32 * 30 + src[1] as i32 * 59 + src[0] as i32 * 11) / 100;
                if lum < 40 { continue; }
                let mut a = lum as u8;
                if a > 220 { a = 255; }
                a = ((a as i32 * ALPHA_TEXT as i32) / 255) as u8;
                blend_pixel(gp.px(x, y), col.r, col.g, col.b, a);
            }
        }
    }
}

fn draw_lamp(gp: &mut G, index: usize, pct: f32) {
    if index >= 3 && gp.net_pill { draw_pill(gp, index, pct); return; }
    let base = load_color(pct);
    let outer = blend(base, Rgb { r: 0, g: 0, b: 0 }, 0.18);
    let inner = blend(base, Rgb { r: 255, g: 255, b: 255 }, 0.22);
    let ring = blend(base, Rgb { r: 255, g: 255, b: 255 }, 0.15 + (pct / 100.0) * 0.55);
    let cx = gp.lamp_x[index]; let cy = gp.lamp_y[index];
    let ro = gp.lamp_r[index]; let ri = ro * 0.76;
    let r0 = ro - 2.2; let r1 = ro - 0.2;
    let y0 = (cy - ro - 1.0) as i32; let y1 = (cy + ro + 1.0) as i32;
    let x0 = (cx - ro - 1.0) as i32; let x1 = (cx + ro + 1.0) as i32;
    for y in y0..y1 {
        if y < 0 || y >= H { continue; }
        for x in x0..x1 {
            if x < 0 || x >= W { continue; }
            let dx = x as f32 + 0.5 - cx; let dy = y as f32 + 0.5 - cy;
            let d = (dx * dx + dy * dy).sqrt();
            if d > ro + 0.5 { continue; }
            let mut edge = 1.0f32;
            if d > ro - 1.4 {
                edge = (ro - d) / 1.4;
                if edge <= 0.0 { continue; }
                if edge > 1.0 { edge = 1.0; }
            }
            let mut col = outer; let mut base_a = ALPHA_BODY;
            if d <= ri { col = inner; base_a = ALPHA_BODY - 20; }
            let hx = dx + ro * 0.15; let hy = dy + ro * 0.18;
            let hd = (hx * hx) / (ro * ro * 0.18) + (hy * hy) / (ro * ro * 0.12);
            if hd < 1.0 && d < ri {
                let t = 0.38 * (1.0 - hd);
                col = blend(col, Rgb { r: 255, g: 255, b: 255 }, t);
                base_a = clampb(base_a as i32 + (35.0 * t) as i32);
            }
            if d >= r0 && d <= r1 { col = ring; base_a = ALPHA_RING; }
            let alpha = clampb((base_a as f32 * edge + 0.5) as i32);
            if alpha != 0 { blend_pixel(gp.px(x, y), col.r, col.g, col.b, alpha); }
        }
    }
    let txt = format!("{:.0}%", pct);
    let tc = if pct < 50.0 { Rgb { r: 20, g: 20, b: 20 } } else { Rgb { r: 255, g: 255, b: 255 } };
    let is_net = index == 3 || index == 4;
    draw_text_centered(gp, &txt, cx, cy, tc, ro, if is_net { gp.font_small } else { gp.font }, 0.0, 0.0);
}

fn draw_pill(gp: &mut G, index: usize, pct: f32) {
    let base = load_color(pct);
    let outer = blend(base, Rgb { r: 0, g: 0, b: 0 }, 0.18);
    let inner = blend(base, Rgb { r: 255, g: 255, b: 255 }, 0.22);
    let ring = blend(base, Rgb { r: 255, g: 255, b: 255 }, 0.15 + (pct / 100.0) * 0.55);
    let cx = gp.lamp_x[index]; let cy = gp.lamp_y[index];
    let hw = gp.lamp_hw[index]; let hh = gp.lamp_hh[index];
    let cr = hh;
    let x0 = (cx - hw - 1.0) as i32; let x1 = (cx + hw + 1.0) as i32;
    let y0 = (cy - hh - 1.0) as i32; let y1 = (cy + hh + 1.0) as i32;
    for y in y0..y1 {
        if y < 0 || y >= H { continue; }
        for x in x0..x1 {
            if x < 0 || x >= W { continue; }
            let dx = x as f32 + 0.5 - cx; let dy = y as f32 + 0.5 - cy;
            let qx = dx.abs() - hw + cr; let qy = dy.abs() - hh + cr;
            let mx = qx.max(0.0); let my = qy.max(0.0);
            let sdf = qx.max(qy).min(0.0) + (mx * mx + my * my).sqrt() - cr;
            let mut cov = 0.5 - sdf;
            if cov <= 0.0 { continue; }
            if cov > 1.0 { cov = 1.0; }
            let qx2 = dx.abs() - hw * 0.76 + cr * 0.76;
            let qy2 = dy.abs() - hh * 0.76 + cr * 0.76;
            let mx2 = qx2.max(0.0); let my2 = qy2.max(0.0);
            let sdf_in = qx2.max(qy2).min(0.0) + (mx2 * mx2 + my2 * my2).sqrt() - cr * 0.76;
            let mut col = outer; let mut base_a = ALPHA_BODY;
            if sdf_in <= 0.0 { col = inner; base_a = ALPHA_BODY - 20; }
            let hx = dx + hw * 0.10; let hy = dy + hh * 0.18;
            let hd = (hx * hx) / (hw * hw * 0.55) + (hy * hy) / (hh * hh * 0.30);
            if hd < 1.0 && sdf <= 0.0 {
                let t = 0.30 * (1.0 - hd);
                col = blend(col, Rgb { r: 255, g: 255, b: 255 }, t);
                base_a = clampb(base_a as i32 + (30.0 * t) as i32);
            }
            if sdf > -2.2 && sdf <= 0.0 { col = ring; base_a = ALPHA_RING; }
            let alpha = clampb((base_a as f32 * cov + 0.5) as i32);
            if alpha != 0 { blend_pixel(gp.px(x, y), col.r, col.g, col.b, alpha); }
        }
    }
    let txt = format!("{:.0}%", pct);
    let tc = if pct < 50.0 { Rgb { r: 20, g: 20, b: 20 } } else { Rgb { r: 255, g: 255, b: 255 } };
    draw_text_centered(gp, &txt, cx, cy, tc, hh, gp.font_small, hw, hh);
}

fn band_rect(index: usize) -> RECT {
    RECT {
        left: MARGIN, right: W - MARGIN,
        top: BAND_TOP + (index as i32) * (BAND_H + BAND_GAP),
        bottom: BAND_TOP + (index as i32) * (BAND_H + BAND_GAP) + BAND_H,
    }
}
fn record_history(gp: &mut G, vals: [f32; 6], up_bps: f64, down_bps: f64) {
    let bps = [0.0f64, 0.0, 0.0, up_bps, down_bps, 0.0];
    for i in 0..BAND_COUNT {
        gp.hist[i].copy_within(1..HIST_N, 0);
        gp.hist[i][HIST_N - 1] = vals[i];
        if i >= NET_BAND0 && i < NET_BAND0 + NET_BAND_COUNT {
            let row = &mut gp.hist_bps[i - NET_BAND0];
            row.copy_within(1..HIST_N, 0);
            row[HIST_N - 1] = bps[i];
        }
    }
    if gp.hist_count < HIST_N { gp.hist_count += 1; }
}
fn hist_stats(gp: &G, band: usize) -> (f32, f32, f32) {
    let first = HIST_N - gp.hist_count;
    let mut sum = 0.0f32; let mut mx = 0.0f32; let mut n = 0;
    for s in first..HIST_N {
        let v = gp.hist[band][s];
        sum += v; n += 1; if v > mx { mx = v; }
    }
    let avg = if n > 0 { sum / n as f32 } else { 0.0 };
    let cur = if gp.hist_count > 0 { gp.hist[band][HIST_N - 1] } else { 0.0 };
    (avg, mx, cur)
}
fn hist_bps_stats(gp: &G, band: usize) -> (f64, f64, f64) {
    let row = &gp.hist_bps[band - NET_BAND0];
    let first = HIST_N - gp.hist_count;
    let mut sum = 0.0f64; let mut mx = 0.0f64; let mut n = 0;
    for s in first..HIST_N {
        let v = row[s]; sum += v; n += 1; if v > mx { mx = v; }
    }
    let avg = if n > 0 { sum / n as f64 } else { 0.0 };
    let cur = if gp.hist_count > 0 { row[HIST_N - 1] } else { 0.0 };
    (avg, mx, cur)
}

fn draw_history_bands(gp: &mut G) {
    let cols = W - 2 * MARGIN;
    let first = HIST_N - gp.hist_count;
    for i in 0..BAND_COUNT {
        let r = band_rect(i);
        for y in r.top..r.bottom {
            for x in r.left..r.right {
                blend_pixel(gp.px(x, y), 16, 18, 22, 105);
            }
        }
        for x in r.left..r.right {
            blend_pixel(gp.px(x, r.top), 255, 255, 255, 28);
            blend_pixel(gp.px(x, r.bottom - 1), 255, 255, 255, 28);
        }
        let by0 = r.top + BAND_LABEL_H;
        let bh = r.bottom - by0;
        let mid = by0 + bh / 2;
        for x in (r.left + 1)..(r.right - 1) { blend_pixel(gp.px(x, mid), 255, 255, 255, 22); }
        for c in 0..cols {
            let mut a = (c as usize) * HIST_N / cols as usize;
            let mut b = ((c + 1) as usize) * HIST_N / cols as usize;
            if b <= a { b = a + 1; }
            if b <= first { continue; }
            if a < first { a = first; }
            let mut sum = 0.0f32; let mut n = 0;
            for s in a..b.min(HIST_N) { sum += gp.hist[i][s]; n += 1; }
            if n == 0 { continue; }
            let v = sum / n as f32;
            let mut hh = (v * bh as f32 / 100.0 + 0.5) as i32;
            if v > 0.0 && hh < 1 { hh = 1; }
            if hh > bh { hh = bh; }
            let x = r.left + c;
            if x >= r.right { break; }
            let col = load_color(v);
            if hh == 0 {
                if i >= NET_BAND0 && i < NET_BAND0 + NET_BAND_COUNT {
                    blend_pixel(gp.px(x, r.bottom - 1), col.r, col.g, col.b, 70);
                }
                continue;
            }
            for k in 0..hh {
                let y = r.bottom - 1 - k;
                blend_pixel(gp.px(x, y), col.r, col.g, col.b, 205);
            }
            blend_pixel(gp.px(x, r.bottom - hh), 255, 255, 255, 80);
        }
    }
}

fn render_panel(gp: &mut G) {
    if gp.hwnd.is_null() || gp.bits.is_null() { return; }
    unsafe {
        std::ptr::write_bytes(gp.bits, 0, (W as usize) * (H as usize) * 4);
        if gp.balls_visible() {
            draw_lamp(gp, 0, gp.cpu_s); draw_lamp(gp, 1, gp.mem_s);
            draw_lamp(gp, 2, gp.disk_s); draw_lamp(gp, 3, gp.up_s);
            draw_lamp(gp, 4, gp.down_s); draw_lamp(gp, 5, gp.gpu_s);
        }
        if gp.bands_visible() { draw_history_bands(gp); }
        let s = gp.zoom();
        let full_w = (W as f32 * s + 0.5) as i32;
        let full_h = (H as f32 * s + 0.5) as i32;
        if !gp.scale_dc.is_null() {
            GdiFlush();
            SetStretchBltMode(gp.scale_dc, COLORONCOLOR as i32);
            StretchBlt(gp.scale_dc, 0, 0, full_w, full_h, gp.mem_dc, 0, 0, W, H, SRCCOPY);
        }
        let src_top = (gp.src_top() as f32 * s + 0.5) as i32;
        let size = SIZE { cx: full_w, cy: gp.out_view_h() };
        let src = POINT { x: 0, y: src_top };
        let dst = POINT { x: gp.win_x, y: gp.win_y };
        let mut bf: BLENDFUNCTION = zeroed();
        bf.BlendOp = AC_SRC_OVER as u8;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA as u8;
        let screen = GetDC(null_mut());
        UpdateLayeredWindow(gp.hwnd, screen, &dst, &size,
            if !gp.scale_dc.is_null() { gp.scale_dc } else { gp.mem_dc }, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(null_mut(), screen);
    }
}

// ===========================================================================
// 托盘
// ===========================================================================
fn create_tray_icon(pct: f32) -> HICON {
    unsafe {
        let size = 16i32;
        let mut bi: BITMAPINFO = zeroed();
        bi.bmiHeader.biSize = size_of::<BITMAPINFOHEADER>() as u32;
        bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        let hdc = GetDC(null_mut());
        let mut bits: *mut c_void = null_mut();
        let hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &mut bits, null_mut(), 0);
        ReleaseDC(null_mut(), hdc);
        if hbmp.is_null() || bits.is_null() { return null_mut(); }
        let px = std::slice::from_raw_parts_mut(bits as *mut u8, (size * size * 4) as usize);
        let base = load_color(pct);
        let c = size as f32 * 0.5; let r = size as f32 * 0.46;
        for y in 0..size as usize {
            for x in 0..size as usize {
                let dx = x as f32 + 0.5 - c; let dy = y as f32 + 0.5 - c;
                let dist = (dx * dx + dy * dy).sqrt();
                let p = &mut px[(y * size as usize + x) * 4..][..4];
                if dist > r { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; continue; }
                let mut a = 255u8;
                if dist > r - 1.0 { a = clampb(((r - dist) * 255.0) as i32); }
                let col = if dist > r - 2.0 { blend(base, Rgb { r: 20, g: 20, b: 20 }, 0.35) } else { base };
                p[0] = clampb(col.b); p[1] = clampb(col.g); p[2] = clampb(col.r); p[3] = a;
            }
        }
        let mut ii: ICONINFO = zeroed();
        ii.fIcon = TRUE; ii.hbmColor = hbmp;
        ii.hbmMask = CreateBitmap(size, size, 1, 1, null());
        let icon = CreateIconIndirect(&ii);
        DeleteObject(ii.hbmMask); DeleteObject(hbmp);
        icon
    }
}
fn tray_tip(gp: &mut G) {
    if !gp.tray_added { return; }
    let t = ui(gp.lang);
    let v = [gp.cpu_s, gp.mem_s, gp.disk_s, gp.up_s, gp.down_s, gp.gpu_s];
    let tip = fmt_tray(gp.lang, t, v);
    let mut tipw = w(&tip);
    tipw.truncate(127); tipw.push(0);
    unsafe {
        let dst = gp.nid.szTip.as_mut_ptr();
        for (i, &c) in tipw.iter().enumerate() { if i < 127 { *dst.add(i) = c; } }
        *dst.add(127) = 0;
        gp_nid_modify(gp, NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP);
    }
}
fn gp_nid_modify(gp: &G, flags: u32) {
    unsafe {
        let mut nid = gp.nid;
        nid.uFlags = flags;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}
fn tray_icon(gp: &mut G) {
    if !gp.tray_added { return; }
    let mut mx = gp.cpu_s;
    if gp.mem_s > mx { mx = gp.mem_s; }
    if gp.disk_s > mx { mx = gp.disk_s; }
    let ni = create_tray_icon(mx);
    if ni.is_null() { return; }
    let old = gp.hicon;
    gp.hicon = ni;
    gp.nid.hIcon = ni;
    gp_nid_modify(gp, NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP);
    if old.is_null() { unsafe { windows_sys::Win32::UI::WindowsAndMessaging::DestroyIcon(old); } }
}
fn tray_remove(gp: &mut G) {
    unsafe {
        if gp.tray_added { Shell_NotifyIconW(NIM_DELETE, &gp.nid); gp.tray_added = false; }
        if gp.hicon.is_null() { windows_sys::Win32::UI::WindowsAndMessaging::DestroyIcon(gp.hicon); gp.hicon = null_mut(); }
    }
}
fn tray_add(gp: &mut G) {
    if gp.hwnd.is_null() { return; }
    unsafe {
        if gp.tray_added { Shell_NotifyIconW(NIM_DELETE, &gp.nid); gp.tray_added = false; }
        if gp.hicon.is_null() { gp.hicon = create_tray_icon(0.0); }
        if gp.hicon.is_null() { gp.hicon = LoadIconW(null_mut(), IDI_APPLICATION as *const u16); }
        let mut nid: NOTIFYICONDATAW = zeroed();
        nid.cbSize = size_of::<NOTIFYICONDATAW>() as u32;
        nid.hWnd = gp.hwnd; nid.uID = TRAY_ID;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = TRAY_MSG; nid.hIcon = gp.hicon;
        let title = w(ui(gp.lang).app_title);
        for i in 0..127 { nid.szTip[i] = if i < title.len() { title[i] } else { 0 }; }
        nid.szTip[127] = 0;
        gp.nid = nid;
        gp.tray_added = Shell_NotifyIconW(NIM_ADD, &gp.nid) != FALSE;
    }
}

// ===========================================================================
// Tooltip
// ===========================================================================
const TTTOOLINFO_V1_SIZE: usize = std::mem::offset_of!(TTTOOLINFOW, lParam) + size_of::<isize>();
fn add_ball_tools(gp: &mut G) {
    if gp.hwnd_tip.is_null() || gp.hwnd.is_null() { return; }
    unsafe {
        for i in 0..(6 + BAND_COUNT) {
            let mut del: TTTOOLINFOW = zeroed();
            del.cbSize = TTTOOLINFO_V1_SIZE as u32;
            del.hwnd = gp.hwnd; del.uId = i;
            SendMessageW(gp.hwnd_tip, TTM_DELTOOLW, 0, &del as *const _ as isize);
            if i < 6 && !gp.balls_visible() { continue; }
            if i >= 6 && !gp.bands_visible() { continue; }
            let mut ti: TTTOOLINFOW = zeroed();
            ti.cbSize = TTTOOLINFO_V1_SIZE as u32;
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = gp.hwnd; ti.uId = i; ti.hinst = null_mut();
            ti.lpszText = -1isize as *mut u16;
            if i < 6 {
                ti.rect.left = gp.zs(gp.lamp_x[i] - gp.lamp_hw[i]) - 3;
                ti.rect.right = gp.zs(gp.lamp_x[i] + gp.lamp_hw[i]) + 3;
                ti.rect.top = gp.zs(gp.lamp_y[i] - gp.lamp_hh[i]) - 3;
                ti.rect.bottom = gp.zs(gp.lamp_y[i] + gp.lamp_hh[i]) + 3;
            } else {
                let r = band_rect(i - 6);
                let off = gp.src_top();
                ti.rect.left = gp.zs(r.left as f32) - 1;
                ti.rect.right = gp.zs(r.right as f32) + 1;
                ti.rect.top = gp.zs((r.top - off) as f32) - 1;
                ti.rect.bottom = gp.zs((r.bottom - off) as f32) + 1;
            }
            SendMessageW(gp.hwnd_tip, TTM_ADDTOOLW, 0, &ti as *const _ as isize);
        }
    }
}
fn init_tooltip(gp: &mut G) {
    unsafe {
        InitCommonControls();
        let cls = TOOLTIPS_CLASSW;
        gp.hwnd_tip = CreateWindowExW(0, cls, null(),
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | TTS_BALLOON,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            gp.hwnd, null_mut(), GetModuleHandleW(null()), null());
        if gp.hwnd_tip.is_null() { return; }
        SendMessageW(gp.hwnd_tip, TTM_SETMAXTIPWIDTH, 0, 560);
        SendMessageW(gp.hwnd_tip, TTM_SETDELAYTIME, TTDT_INITIAL as usize, 200);
        SendMessageW(gp.hwnd_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP as usize, 8000);
        let title = w(ui(gp.lang).app_title);
        SendMessageW(gp.hwnd_tip, TTM_SETTITLEW, 0, title.as_ptr() as isize);
        add_ball_tools(gp);
    }
}

// TTN_GETDISPINFO 填文本
fn fill_tip(gp: &mut G, id: usize, di: *mut NMTTDISPINFOW) {
    let t = ui(gp.lang);
    let metric = if id < 6 {
        let v = [gp.cpu_s, gp.mem_s, gp.disk_s, gp.up_s, gp.down_s, gp.gpu_s][id];
        fmt_ball_tip(gp.lang, t.ball_names[id], v)
    } else {
        let b = id - 6;
        let (avg, peak, cur) = hist_stats(gp, b);
        if b < NET_BAND0 || b >= NET_BAND0 + NET_BAND_COUNT {
            fmt_band_tip(gp.lang, t.band_names[b], avg, peak, cur)
        } else {
            let (ra, rp, rc) = hist_bps_stats(gp, b);
            fmt_net_tip(gp.lang, t.band_names[b], &fmt_rate(ra), &fmt_rate(rp), &fmt_rate(rc), avg, peak, cur)
        }
    };
    let sys = sys_info_text(gp.lang);
    let full = format!("{}\n{}", metric, sys);
    let fw = w(&full);
    let n = fw.len().min(gp.tip_full.len());
    gp.tip_full[..n].copy_from_slice(&fw[..n]);
    unsafe { (*di).lpszText = gp.tip_full.as_mut_ptr(); }
}

// ===========================================================================
// 菜单 / 缩放 / 重置 / About
// ===========================================================================
fn apply_zoom(gp: &mut G, idx: i32, anchor: bool) {
    if idx < 0 || idx >= ZOOM_LEVELS.len() as i32 || idx as usize == gp.zoom_idx { return; }
    let old = gp.zoom();
    let mut pt = POINT { x: 0, y: 0 };
    let (mut ax, mut ay) = (0.0f32, 0.0f32);
    if anchor && unsafe { GetCursorPos(&mut pt) != 0 } {
        ax = (pt.x - gp.win_x) as f32 / old;
        ay = (pt.y - gp.win_y) as f32 / old;
    }
    gp.zoom_idx = idx as usize;
    if !gp.center_expanded { gp.save_zoom(); }
    let new = gp.zoom();
    if anchor && (ax != 0.0 || ay != 0.0) {
        gp.win_x = pt.x - (ax * new + 0.5) as i32;
        gp.win_y = pt.y - (ay * new + 0.5) as i32;
    }
    unsafe {
        SetWindowPos(gp.hwnd, null_mut(), 0, 0, gp.out_w(), gp.out_view_h(),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }
    add_ball_tools(gp);
    ensure_visible(gp);
    gp.save_pos();
    render_panel(gp);
}
fn toggle_center(gp: &mut G) {
    if gp.hwnd.is_null() { return; }
    if gp.center_expanded {
        gp.center_expanded = false;
        gp.zoom_idx = gp.center_saved_zoom;
        gp.win_x = gp.center_saved_x;
        gp.win_y = gp.center_saved_y;
        unsafe {
            SetWindowPos(gp.hwnd, null_mut(), gp.win_x, gp.win_y, gp.out_w(), gp.out_view_h(),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
        add_ball_tools(gp); render_panel(gp); apply_topmost(gp);
        return;
    }
    unsafe {
        let old = RECT { left: gp.win_x, top: gp.win_y, right: gp.win_x + gp.out_w(), bottom: gp.win_y + gp.out_view_h() };
        let mon = MonitorFromRect(&old, MONITOR_DEFAULTTONEAREST);
        let mut mi: MONITORINFO = zeroed();
        mi.cbSize = size_of::<MONITORINFO>() as u32;
        let info_ok = if !mon.is_null() { GetMonitorInfoW(mon, &mut mi) != 0 } else { false };
        if !info_ok {
            mi.rcWork = RECT { left: 0, top: 0, right: GetSystemMetrics(SM_CXSCREEN), bottom: GetSystemMetrics(SM_CYSCREEN) };
        }
        gp.center_saved_zoom = gp.zoom_idx;
        gp.center_saved_x = gp.win_x; gp.center_saved_y = gp.win_y;
        gp.center_expanded = true;
        gp.zoom_idx = ZOOM_LEVELS.len() - 1;
        let w = gp.out_w(); let h = gp.out_view_h();
        let waw = mi.rcWork.right - mi.rcWork.left;
        let wah = mi.rcWork.bottom - mi.rcWork.top;
        gp.win_x = if waw >= w { mi.rcWork.left + (waw - w) / 2 } else { mi.rcWork.left };
        gp.win_y = if wah >= h { mi.rcWork.top + (wah - h) / 2 } else { mi.rcWork.top };
        SetWindowPos(gp.hwnd, null_mut(), gp.win_x, gp.win_y, w, h,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        add_ball_tools(gp); render_panel(gp);
        ShowWindow(gp.hwnd, SW_SHOWNOACTIVATE);
        apply_topmost(gp);
    }
}
fn apply_topmost(gp: &G) {
    unsafe {
        if gp.hwnd.is_null() { return; }
        let ex = GetWindowLongPtrW(gp.hwnd, GWL_EXSTYLE);
        if gp.topmost {
            if (ex & (WS_EX_TOPMOST as isize)) == 0 {
                SetWindowLongPtrW(gp.hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST as isize | WS_EX_LAYERED as isize | WS_EX_TOOLWINDOW as isize);
            }
            SetWindowPos(gp.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            if (ex & (WS_EX_TOPMOST as isize)) != 0 {
                SetWindowLongPtrW(gp.hwnd, GWL_EXSTYLE, (ex | WS_EX_LAYERED as isize | WS_EX_TOOLWINDOW as isize) & !(WS_EX_TOPMOST as isize));
            }
            SetWindowPos(gp.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
}
fn reset_pos(gp: &mut G, save: bool) {
    if gp.hwnd.is_null() { return; }
    gp.center_expanded = false;
    unsafe {
        let mut wa: RECT = zeroed();
        if SystemParametersInfoW(SPI_GETWORKAREA, 0, &mut wa as *mut _ as *mut c_void, 0) == 0 {
            wa = RECT { left: 0, top: 0, right: GetSystemMetrics(SM_CXSCREEN), bottom: GetSystemMetrics(SM_CYSCREEN) };
        }
        gp.win_x = wa.right - gp.out_w() - 24;
        gp.win_y = wa.top + 24;
        if gp.win_x < wa.left + 8 { gp.win_x = wa.left + 8; }
        if gp.win_y < wa.top + 8 { gp.win_y = wa.top + 8; }
        if gp.win_x + gp.out_w() > wa.right - 8 { gp.win_x = wa.right - gp.out_w() - 8; }
        if gp.win_y + gp.out_view_h() > wa.bottom - 8 { gp.win_y = wa.bottom - gp.out_view_h() - 8; }
    }
    if save { gp.save_pos(); }
    render_panel(gp);
    unsafe { ShowWindow(gp.hwnd, SW_SHOWNOACTIVATE); }
    apply_topmost(gp);
}
fn ensure_visible(gp: &mut G) {
    if gp.hwnd.is_null() { return; }
    unsafe {
        let wr = RECT { left: gp.win_x, top: gp.win_y, right: gp.win_x + gp.out_w(), bottom: gp.win_y + gp.out_view_h() };
        if !MonitorFromRect(&wr, MONITOR_DEFAULTTONULL).is_null() { return; }
    }
    reset_pos(gp, true);
}
fn reset_all(gp: &mut G) {
    for n in ["ViewMode", "Lang", "Zoom", "ShowText", "Topmost", "NetPill", "WinX", "WinY"] { reg_delete(n); }
    gp.view_mode = VIEW_BANDS; gp.lang = LANG_ZH; gp.zoom_idx = ZOOM_DEFAULT;
    gp.show_text = false; gp.topmost = true; gp.net_pill = false;
    relayout_net(gp);
    unsafe {
        SetWindowPos(gp.hwnd, null_mut(), 0, 0, gp.out_w(), gp.out_view_h(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        if !gp.hwnd_tip.is_null() { DestroyWindow(gp.hwnd_tip); gp.hwnd_tip = null_mut(); }
    }
    init_tooltip(gp);
    tray_tip(gp);
    reset_pos(gp, false);
}
unsafe extern "system" fn about_cbt(code: i32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    if code == HCBT_ACTIVATE as i32 {
        let btn = GetDlgItem(wp as HWND, IDOK);
        if btn.is_null() {
            let txt = if g().lang == LANG_EN { w("OK") } else { w("确定") };
            SetWindowTextW(btn, txt.as_ptr());
        }
        if g().about_hook.is_null() { UnhookWindowsHookEx(g().about_hook); g().about_hook = null_mut(); }
    }
    CallNextHookEx(null_mut(), code, wp, lp)
}
fn show_about(gp: &mut G) {
    unsafe {
        let body = w(&fmt_about(gp.lang));
        let title = w(ui(gp.lang).about_title);
        gp.about_hook = SetWindowsHookExW(WH_CBT, Some(about_cbt), null_mut(), GetCurrentThreadId());
        MessageBoxW(gp.hwnd, body.as_ptr(), title.as_ptr(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        if gp.about_hook.is_null() { UnhookWindowsHookEx(gp.about_hook); gp.about_hook = null_mut(); }
    }
}

fn handle_menu(gp: &mut G, id: u32) {
    match id {
        IDM_EXIT => request_exit(gp),
        IDM_TOGGLE_TEXT => { gp.show_text = !gp.show_text; reg_set("ShowText", if gp.show_text {1} else {0}); render_panel(gp); }
        IDM_VIEW_BOTH | IDM_VIEW_BALLS | IDM_VIEW_BANDS => {
            let next = if id == IDM_VIEW_BOTH { VIEW_BOTH } else if id == IDM_VIEW_BALLS { VIEW_BALLS } else { VIEW_BANDS };
            if next == gp.view_mode { return; }
            gp.view_mode = next;
            reg_set("ViewMode", next as u32);
            unsafe { SetWindowPos(gp.hwnd, null_mut(), 0, 0, gp.out_w(), gp.out_view_h(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS); }
            add_ball_tools(gp); ensure_visible(gp); render_panel(gp);
        }
        IDM_TOGGLE_TOP => { gp.topmost = !gp.topmost; reg_set("Topmost", if gp.topmost {1} else {0}); apply_topmost(gp); }
        IDM_TOGGLE_NETSHAPE => { gp.net_pill = !gp.net_pill; reg_set("NetPill", if gp.net_pill {1} else {0}); relayout_net(gp); render_panel(gp); add_ball_tools(gp); }
        IDM_LANG_ZH | IDM_LANG_EN => {
            let next = if id == IDM_LANG_ZH { LANG_ZH } else { LANG_EN };
            if next == gp.lang { return; }
            gp.lang = next; reg_set("Lang", next as u32);
            tray_tip(gp);
            unsafe { if !gp.hwnd_tip.is_null() { DestroyWindow(gp.hwnd_tip); gp.hwnd_tip = null_mut(); } }
            init_tooltip(gp);
        }
        IDM_RESET_POS => reset_pos(gp, true),
        IDM_RESET_ALL => reset_all(gp),
        IDM_ABOUT => show_about(gp),
        IDM_ZOOM_IN => apply_zoom(gp, gp.zoom_idx as i32 + 1, false),
        IDM_ZOOM_OUT => apply_zoom(gp, gp.zoom_idx as i32 - 1, false),
        IDM_ZOOM_RESET => apply_zoom(gp, ZOOM_DEFAULT as i32, false),
        _ => {}
    }
}
fn show_menu(gp: &mut G, hwnd: HWND) {
    unsafe {
        let menu = CreatePopupMenu();
        if menu.is_null() { return; }
        let t = ui(gp.lang);
        let add = |flags: u32, id: u32, text: &str| { let tw = w(text); AppendMenuW(menu, flags, id as usize, tw.as_ptr()); };
        add(MF_STRING | if gp.show_text {MF_CHECKED} else {MF_UNCHECKED}, IDM_TOGGLE_TEXT, t.menu_percent);
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        add(MF_STRING | if gp.view_mode==VIEW_BOTH {MF_CHECKED} else {MF_UNCHECKED}, IDM_VIEW_BOTH, t.menu_view_both);
        add(MF_STRING | if gp.view_mode==VIEW_BALLS {MF_CHECKED} else {MF_UNCHECKED}, IDM_VIEW_BALLS, t.menu_view_balls);
        add(MF_STRING | if gp.view_mode==VIEW_BANDS {MF_CHECKED} else {MF_UNCHECKED}, IDM_VIEW_BANDS, t.menu_view_bands);
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        add(MF_STRING | if gp.topmost {MF_CHECKED} else {MF_UNCHECKED}, IDM_TOGGLE_TOP, t.menu_topmost);
        add(MF_STRING, IDM_TOGGLE_NETSHAPE, if gp.net_pill {t.menu_net_to_round} else {t.menu_net_to_pill});
        add(MF_STRING, IDM_RESET_POS, t.menu_reset);
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        add(MF_STRING, IDM_ZOOM_IN, t.menu_zoom_in);
        add(MF_STRING, IDM_ZOOM_OUT, t.menu_zoom_out);
        let zlabel = fmt_zoom(gp.lang, gp.zoom() * 100.0);
        add(MF_STRING, IDM_ZOOM_RESET, &zlabel);
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        add(MF_STRING | if gp.lang==LANG_ZH {MF_CHECKED} else {MF_UNCHECKED}, IDM_LANG_ZH, "中文");
        add(MF_STRING | if gp.lang==LANG_EN {MF_CHECKED} else {MF_UNCHECKED}, IDM_LANG_EN, "English");
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        add(MF_STRING, IDM_RESET_ALL, t.menu_reset_all);
        add(MF_STRING, IDM_ABOUT, t.menu_about);
        add(MF_STRING, IDM_EXIT, t.menu_exit);
        let mut pt = POINT { x: 0, y: 0 };
        GetCursorPos(&mut pt);
        SetForegroundWindow(hwnd);
        let cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, null());
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if cmd != 0 { handle_menu(gp, cmd as u32); }
    }
}
fn request_exit(gp: &mut G) {
    gp.running = false;
    unsafe {
        if gp.hwnd.is_null() { PostMessageW(gp.hwnd, WM_CLOSE, 0, 0); }
        PostQuitMessage(0);
    }
}

// ===========================================================================
// 窗口过程
// ===========================================================================
unsafe extern "system" fn wnd_proc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    if g().msg_taskbar != 0 && msg == g().msg_taskbar {
        tray_add(g()); tray_tip(g()); ensure_visible(g());
        return 0;
    }
    match msg {
        WM_CREATE => { SetTimer(hwnd, 1, TIMER_MS, None); return 0; }
        WM_TIMER => {
            if wp != 1 { /* fall through to default */ } else {
                let gp = g();
                if gp.topmost { apply_topmost(gp); }
                gp.cpu = gp.sampler.cpu();
                gp.mem = gp.sampler.mem();
                gp.disk = gp.sampler.disk();
                gp.gpu = gp.sampler.gpu();
                let (down, up, in_bps, out_bps) = gp.sampler.net();
                gp.down = down; gp.up = up;
                record_history(gp, [gp.cpu, gp.mem, gp.disk, gp.up, gp.down, gp.gpu], out_bps, in_bps);
                if gp.tick == 0 {
                    gp.cpu_s = gp.cpu; gp.mem_s = gp.mem; gp.disk_s = gp.disk;
                    gp.up_s = gp.up; gp.down_s = gp.down; gp.gpu_s = gp.gpu;
                } else {
                    gp.cpu_s = EMA_A * gp.cpu + (1.0 - EMA_A) * gp.cpu_s;
                    gp.mem_s = EMA_A * gp.mem + (1.0 - EMA_A) * gp.mem_s;
                    gp.disk_s = EMA_A * gp.disk + (1.0 - EMA_A) * gp.disk_s;
                    gp.up_s = EMA_A * gp.up + (1.0 - EMA_A) * gp.up_s;
                    gp.down_s = EMA_A * gp.down + (1.0 - EMA_A) * gp.down_s;
                    gp.gpu_s = EMA_A * gp.gpu + (1.0 - EMA_A) * gp.gpu_s;
                }
                render_panel(gp); tray_tip(gp);
                gp.tick += 1;
                if gp.tick % ICON_EVERY == 0 { tray_icon(gp); }
                if gp.tick % 16 == 0 { ensure_visible(gp); }
                return 0;
            }
        }
        WM_DISPLAYCHANGE | WM_DPICHANGED => { ensure_visible(g()); return 0; }
        QUERY_LANG_MSG => { return g().lang as LRESULT; }
        ALREADY_MSG => {
            let gp = g();
            ensure_visible(gp); ShowWindow(gp.hwnd, SW_SHOWNOACTIVATE); apply_topmost(gp); tray_tip(gp);
            let t = ui(gp.lang);
            let title = w(t.app_title); let info = w(t.balloon_info);
            let mut nid = gp.nid;
            nid.uFlags = NIF_ICON | NIF_INFO | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
            for i in 0..63 { nid.szInfoTitle[i] = if i < title.len() { title[i] } else { 0 }; }
            nid.szInfoTitle[63] = 0;
            for i in 0..255 { nid.szInfo[i] = if i < info.len() { info[i] } else { 0 }; }
            nid.szInfo[255] = 0;
            nid.dwInfoFlags = NIIF_INFO;
            if gp.tray_added { Shell_NotifyIconW(NIM_MODIFY, &nid); }
            return 0;
        }
        TRAY_MSG => {
            if lp == WM_RBUTTONUP as isize || lp == WM_CONTEXTMENU as isize ||
               lp == WM_LBUTTONUP as isize || lp == WM_RBUTTONDOWN as isize {
                show_menu(g(), hwnd);
            } else if lp == WM_LBUTTONDBLCLK as isize {
                let gp = g(); gp.show_text = !gp.show_text;
                reg_set("ShowText", if gp.show_text {1} else {0}); render_panel(gp);
            }
            return 0;
        }
        WM_COMMAND => { handle_menu(g(), (wp as u32) & 0xFFFF); return 0; }
        WM_NOTIFY => {
            let nm = lp as *const NMHDR;
            if (*nm).code == TTN_GETDISPINFOW as u32 && (*nm).hwndFrom == g().hwnd_tip {
                let id = (*nm).idFrom;
                if id < (6 + BAND_COUNT) as usize && (if id < 6 { g().balls_visible() } else { g().bands_visible() }) {
                    fill_tip(g(), id, lp as *mut NMTTDISPINFOW);
                    return 0;
                }
            }
        }
        WM_MOUSEWHEEL => {
            let delta = ((wp >> 16) & 0xFFFF) as i16 as i32;
            let gp = g(); gp.wheel_acc += delta;
            if gp.wheel_acc >= 120 { gp.wheel_acc -= 120; apply_zoom(gp, gp.zoom_idx as i32 + 1, true); }
            else if gp.wheel_acc <= -120 { gp.wheel_acc += 120; apply_zoom(gp, gp.zoom_idx as i32 - 1, true); }
            return 0;
        }
        WM_LBUTTONDBLCLK => {
            let gp = g(); gp.dragging = false; ReleaseCapture(); toggle_center(gp);
            return 0;
        }
        WM_LBUTTONDOWN => {
            let gp = g(); gp.dragging = true; SetCapture(hwnd);
            let mut pt = POINT { x: 0, y: 0 }; GetCursorPos(&mut pt);
            gp.drag_x = pt.x - gp.win_x; gp.drag_y = pt.y - gp.win_y;
            return 0;
        }
        WM_MOUSEMOVE => {
            if !g().dragging { /* fall through */ } else {
                let gp = g();
                let mut pt = POINT { x: 0, y: 0 }; GetCursorPos(&mut pt);
                gp.win_x = pt.x - gp.drag_x; gp.win_y = pt.y - gp.drag_y;
                if gp.topmost { apply_topmost(gp); }
                render_panel(gp);
                return 0;
            }
        }
        WM_LBUTTONUP => {
            let gp = g();
            if gp.dragging {
                gp.dragging = false; ReleaseCapture();
                if !gp.center_expanded { gp.save_pos(); }
            }
            return 0;
        }
        WM_RBUTTONUP => { show_menu(g(), hwnd); return 0; }
        WM_DESTROY => {
            KillTimer(hwnd, 1);
            tray_remove(g());
            if g().mutex.is_null() { ReleaseMutex(g().mutex); CloseHandle(g().mutex); g().mutex = null_mut(); }
            PostQuitMessage(0);
            return 0;
        }
        _ => {}
    }
    DefWindowProcW(hwnd, msg, wp, lp)
}

// ===========================================================================
// 入口
// ===========================================================================
fn load_prefs(gp: &mut G) {
    if let Some(v) = reg_get("ViewMode") { if v <= VIEW_BANDS as u32 { gp.view_mode = v as i32; } }
    if let Some(v) = reg_get("Lang") { if v <= LANG_EN as u32 { gp.lang = v as i32; } }
    if let Some(v) = reg_get("Zoom") { if (v as usize) < ZOOM_LEVELS.len() { gp.zoom_idx = v as usize; } }
    if let Some(v) = reg_get("ShowText") { gp.show_text = v != 0; }
    if let Some(v) = reg_get("Topmost") { gp.topmost = v != 0; }
    if let Some(v) = reg_get("NetPill") { gp.net_pill = v != 0; }
}
fn set_dpi_aware() {
    unsafe {
        let sc = w("shcore.dll");
        let h = LoadLibraryW(sc.as_ptr());
        if h.is_null() {
            let p = GetProcAddress(h, b"SetProcessDpiAwareness\0".as_ptr());
            if let Some(f) = p {
                type Fn = unsafe extern "system" fn(i32) -> i32;
                std::mem::transmute::<_, Fn>(f)(2);
            }
            FreeLibrary(h);
        } else {
            windows_sys::Win32::UI::WindowsAndMessaging::SetProcessDPIAware();
        }
    }
}
fn work_area() -> RECT {
    unsafe {
        let mut wa: RECT = zeroed();
        if SystemParametersInfoW(SPI_GETWORKAREA, 0, &mut wa as *mut _ as *mut c_void, 0) == 0 {
            wa = RECT { left: 0, top: 0, right: GetSystemMetrics(SM_CXSCREEN), bottom: GetSystemMetrics(SM_CYSCREEN) };
        }
        wa
    }
}

fn real_main() -> i32 {
    unsafe {
        GLOBAL = Some(G::new());
        g().msg_taskbar = RegisterWindowMessageW(w("TaskbarCreated").as_ptr());
        set_dpi_aware();

        g().mutex = CreateMutexW(null(), TRUE, w(MUTEX_NAME).as_ptr());
        let err = windows_sys::Win32::Foundation::GetLastError();
        if g().mutex.is_null() && err == ERROR_ALREADY_EXISTS {
            let cls = w(CLASS_NAME); let title = w(BALL_TITLE);
            let exist = FindWindowW(cls.as_ptr(), title.as_ptr());
            if exist.is_null() {
                let mut lang: usize = LANG_ZH as usize;
                SendMessageTimeoutW(exist, QUERY_LANG_MSG, 0, 0, SMTO_ABORTIFHUNG as u32, 300, &mut lang);
                if lang == LANG_EN as usize { g().lang = LANG_EN; }
                PostMessageW(exist, ALREADY_MSG, 0, 0);
            }
            let t = ui(g().lang);
            let body = w(t.already_running); let titlew = w(t.app_title);
            MessageBoxW(null_mut(), body.as_ptr(), titlew.as_ptr(), MB_OK | MB_ICONINFORMATION);
            CloseHandle(g().mutex); g().mutex = null_mut();
            return 0;
        }
        if g().mutex.is_null() { return 1; }

        load_prefs(g());
        if !init_frame_buffer(g()) { CloseHandle(g().mutex); g().mutex = null_mut(); return 1; }
        g().sampler.init_counters();

        let fp = (LAMP_D_CPU as f32 * 0.26) as i32;
        let fsp = (LAMP_D_NET as f32 * 0.32) as i32;
        let face = w("Segoe UI");
        g().font = CreateFontW(-fp, 0, 0, 0, FW_BOLD as i32, 0, 0, 0,
            DEFAULT_CHARSET as u32, OUT_DEFAULT_PRECIS as u32, CLIP_DEFAULT_PRECIS as u32,
            CLEARTYPE_QUALITY as u32, (DEFAULT_PITCH | FF_DONTCARE) as u32, face.as_ptr());
        g().font_small = CreateFontW(-fsp, 0, 0, 0, FW_BOLD as i32, 0, 0, 0,
            DEFAULT_CHARSET as u32, OUT_DEFAULT_PRECIS as u32, CLIP_DEFAULT_PRECIS as u32,
            CLEARTYPE_QUALITY as u32, (DEFAULT_PITCH | FF_DONTCARE) as u32, face.as_ptr());

        let wa = work_area();
        let mut restored = false;
        if let (Some(sx), Some(sy)) = (reg_get("WinX"), reg_get("WinY")) {
            let x = sx as i32; let y = sy as i32;
            let wr = RECT { left: x, top: y, right: x + g().out_w(), bottom: y + g().out_view_h() };
            if !MonitorFromRect(&wr, MONITOR_DEFAULTTONULL).is_null() {
                g().win_x = x; g().win_y = y; restored = true;
            }
        }
        if !restored {
            g().win_x = wa.right - g().out_w() - 24;
            g().win_y = wa.top + 24;
            if g().win_x < wa.left + 8 { g().win_x = wa.left + 8; }
            if g().win_y + g().out_view_h() > wa.bottom - 8 { g().win_y = wa.bottom - g().out_view_h() - 8; }
            g().save_pos();
        }

        let hinstance = GetModuleHandleW(null());
        let cls = w(CLASS_NAME);
        let mut wc: WNDCLASSEXW = zeroed();
        wc.cbSize = size_of::<WNDCLASSEXW>() as u32;
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Some(wnd_proc);
        wc.hInstance = hinstance;
        wc.hCursor = LoadCursorW(null_mut(), IDC_ARROW as *const u16);
        wc.lpszClassName = cls.as_ptr();
        wc.hIcon = LoadIconW(hinstance, IDI_APP as *const u16);
        if wc.hIcon.is_null() { wc.hIcon = LoadIconW(null_mut(), IDI_APPLICATION as *const u16); }
        wc.hIconSm = wc.hIcon;
        if RegisterClassExW(&wc) == 0 { return 1; }

        let title = w(BALL_TITLE);
        g().hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | if g().topmost { WS_EX_TOPMOST } else { 0 },
            cls.as_ptr(), title.as_ptr(), WS_POPUP,
            g().win_x, g().win_y, g().out_w(), g().out_view_h(),
            null_mut(), null_mut(), hinstance, null());
        if g().hwnd.is_null() { if g().mutex.is_null() { CloseHandle(g().mutex); g().mutex = null_mut(); } return 1; }

        g().hicon = LoadIconW(hinstance, IDI_APP as *const u16);
        if g().hicon.is_null() { g().hicon = create_tray_icon(0.0); }
        if g().hicon.is_null() { g().hicon = LoadIconW(null_mut(), IDI_APPLICATION as *const u16); }
        tray_add(g());
        init_tooltip(g());
        ShowWindow(g().hwnd, SW_SHOWNOACTIVATE);
        g().sampler.disk();
        render_panel(g());
        apply_topmost(g());

        let mut msg: windows_sys::Win32::UI::WindowsAndMessaging::MSG = zeroed();
        while g().running && GetMessageW(&mut msg, null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        tray_remove(g());
        if !g().font.is_null() { DeleteObject(g().font); }
        if !g().font_small.is_null() { DeleteObject(g().font_small); }
        if g().mutex.is_null() { ReleaseMutex(g().mutex); CloseHandle(g().mutex); g().mutex = null_mut(); }
        0
    }
}

fn main() { std::process::exit(real_main()); }
