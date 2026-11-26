// 编译期生成 BUILD_DATE 环境变量（等价于 C++ 的 __DATE__）。
// 纯 std 实现，不引入任何依赖。build.rs 每次构建都会执行；
// cargo 只在源码变化时重编 main.rs，所以日期以最后一次实际重编为准。
use std::time::{SystemTime, UNIX_EPOCH};

fn main() {
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let days = (secs / 86400) as i64;
    // Howard Hinnant 的 civil_from_days 算法
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    println!("cargo:rustc-env=BUILD_DATE={:04}-{:02}-{:02}", y, m, d);
    println!("cargo:rerun-if-changed=build.rs");
}
