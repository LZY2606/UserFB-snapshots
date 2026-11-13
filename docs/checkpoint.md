# 可续采采集（RunCheckpoint）使用说明

`Spider.RunCheckpoint(ctx, dir)` 在普通 `Run` 之外提供了一条可重启的采集入口：
采集前沿（待发队列、在途请求、已完成集合）持久化在一个目录里，进程被杀或
返回错误后，用同一个目录重新启动即可接着跑剩下的请求，不需要再从入口页
重新遍历。

- 入口：`func (s *Spider) RunCheckpoint(ctx context.Context, dir string) error`
- `Run`、`Get`、自定义 `Scheduler`（含 Redis 插件）的原有用法完全不变，
  去重行为也保持原样。
- 一个目录只给一个采集进程使用。不支持多进程抢任务，也不接 Redis。

## 基本用法

```go
sp := spider.NewSpider(NewMyPageProcesser(), "task-name").
    AddPipeline(pipeline.NewPipelineConsole()).
    SetThreadnum(3)

// 种子仍然通过 AddUrl / AddRequest 加入
sp.AddUrl("https://example.invalid/index", "html")

// 第一次运行：种子被接纳并落盘；之后每次重启都从这个目录恢复
err := sp.RunCheckpoint(context.Background(), "./crawl-state")
```

再次运行同一个程序时：

1. 目录里没有快照：把本次 `AddRequest/AddUrl` 的种子作为初始前沿，新建快照；
2. 目录里已有快照：先恢复待发队列，再把在途项按当初领取顺序放到待发队列
   最前面，最后接纳本次新加入的种子；
3. 目录已经采完（快照带 `finished` 标记）：直接返回，不会再调用一次
   `PageProcesser.Finish`。

## 去重语义

- 身份就是 URL 原字符串，作用范围是“这一次采集”（一个目录的生命周期）。
- 已排队、在途、已完成的 URL 都参与去重，同一个 URL 只接纳一次。
- 同一个 URL 以**第一次接纳时**的请求参数为准；后面再推同 URL 的不同
  method/body 会被忽略。
- 循环链接因此天然安全：指回已采集页面的链接不会产生第二次下载。

## 持久化了什么

每个请求保存：`url`、响应类型（`resp_type`）、`method`、`postdata`、
`urltag`、`header`、`cookies`、`proxyHost`。恢复后仍然是原来的 POST，
不会退化成 GET。

以下两类请求无法持久化，会在进入持久状态、开始下载之前返回明确错误：

- 带自定义 redirect 回调（`checkRedirect != nil`）的请求；
- `Meta != nil` 的请求。

回调函数和运行时对象（Spider、PageProcesser、Pipeline、锁等）不会被
序列化。

## 一次页面处理的持久化时序

1. 领取请求时，先把“该请求进入在途”写入快照，落盘成功后才调用
   `Downloader`；
2. 下载沿用原有三次尝试；三次都失败时，请求重新放回待发队列**最前面**，
   返回错误——不会无限重试，也不会记成完成；
3. 下载成功后照旧执行 `PageProcesser.Process` 和各 `Pipeline`；
4. 父页面标记完成与它发现的全部子请求，在**同一次原子写入**里提交：
   不存在“父已完成、子链接还没落盘”的中间状态。

如果进程在第 4 步提交之前被杀：这个页面会被重抓，`Process` 和 Pipeline
可能重放。**输出的幂等由调用方保证**。一旦提交成功，该页面不会再被派发。

处理回调 panic、Pipeline panic、快照写盘失败都会让本次运行返回错误，
不会被当成正常结束；对应请求保持可恢复状态。

## 取消语义

`ctx` 取消后：

- 不再领取新请求；
- 已经开始的页面会收尾并保存好状态（自定义 `Downloader` 不会被强行
  打断）；
- 全部在途页面结束后返回 `ctx.Err()`。

用 `context.WithCancel` 或 `context.WithTimeout` 控制停止时机，然后用同一
目录再次调用 `RunCheckpoint` 续采。

## 快照文件与容错

目录中的 `checkpoint.json` 是唯一快照，JSON 格式并带 `version` 字段：

- 文件不存在：自动新建；
- 文件截断、不是合法 JSON、版本号无法识别、请求字段不完整：直接报错，
  不会用空状态覆盖；
- 每次写入走“临时文件 + fsync + 原子 rename”，新状态写失败时旧快照
  仍然可读，内存里的前沿也不会推进。

不要手工编辑该文件；升级库版本后若格式版本变化，需要用新目录重新采集。

## 完整示例

见 `example/checkpoint_resume`：一个带循环链接的本地站点，包含 GET 和
POST 种子。运行后在采集过程中 Ctrl-C 杀掉进程，再用相同 `-dir` 重启即可
观察续采：

```sh
go run ./example/checkpoint_resume -dir ./my-crawl
```
