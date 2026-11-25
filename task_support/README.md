离线入口：`bash task_support/run.sh baseline`，新增实现后运行 `bash task_support/run.sh target`。
所有 Cargo 命令用 harness/Cargo.lock、vendor 和离线源替换，不调用根包的网络示例/dev-dependencies。
新测试在 harness/tests/mirror_transport.rs：
```rust
#[path = "../../task_support/check.rs"] mod taskcheck;
#[test] fn mirror_geometry() {
    let g = taskcheck::Group::new("mirror_geometry");
    // 这里添加真实的独立参照和内容断言：g.check(condition, "description");
    g.finish();
}
```
另两组同样调用 Group；此片段不构成目标测试，也没有预置目标行为。
示例接收一个输出文件路径，实际生成 PNG；每次入口先删除自己的旧图片再检查本次输出。
API binding 是编译约束，必须公开 rpt::mirrors::MirrorTransport 和 MirrorPanel。
