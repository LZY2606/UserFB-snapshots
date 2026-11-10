fn main() {
    // macOS 与常见 Linux 发行版均自带 libsqlite3；
    // 直接链接系统库，避免任何 crate 下载，保证离线构建。
    println!("cargo:rustc-link-lib=sqlite3");
    println!("cargo:rerun-if-changed=build.rs");
}
