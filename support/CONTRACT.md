# Rust 驱动接口

新增 `tests/segmented_contract.rs` 是普通 Rust 模块，不使用 Rust 的 test harness。它导出 `pub fn register(cases: &mut crate::support::Cases)`，通过 `cases.add("名称", fn(&mut crate::support::Check))` 注册五组，恰好一次：aranges_alignment、line_selector、sequence_identity、flat_compatibility、bank_attribution。

实际比较调用 `check.equal(actual, expected)` 或 `check.require(condition, reason)`，每组至少有一次实际比较。支持文件 `support/contract_main.rs` 在每组正常返回、全部完成后才写临时完成记录。缺文件、少组、重复组、零比较、跳过函数体、编译错误、比较失败、process::exit(0) 提前退出都不能成功。不要改这些支持文件来绕过检查。

`bash support/run.sh baseline` 用 rustc 编译上游真实 read/read-core/std 路径及一个独立平坦 aranges 正例。`target` 先复跑此基线，再编译新测试、运行五组，然后编译新增工具并实际读取固定查询。工具报告的格式见 REPORT.md。驱动的报告检查只保证结构、查询覆盖与有正常输出，语义由独立测试及人工逐字节核对负责，不能用输出成功标记替代正确归属。

不构建 Cargo workspace、write feature 或外部调试器。不需要下载 crate。rustc 至少支持 edition 2024 和此上游版本的 MSRV（1.88）。目标实现提供的库模块须被 src/lib.rs/read 的真实生产导出路径包含，CLI 与测试调用同一生产 API。
