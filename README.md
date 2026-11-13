go_spider
=========

[![Build Status](https://travis-ci.org/hu17889/go_spider.svg)](https://travis-ci.org/hu17889/go_spider)


A crawler of vertical communities achieved by GOLANG. 

![image](https://raw.githubusercontent.com/hu17889/doc/master/go_spider/img/logo.png)


Latest stable Release: [Version 1.2 (Sep 23, 2014)](https://github.com/hu17889/go_spider/releases).


* [![go_spider讨论群](http://pub.idqqimg.com/wpa/images/group.png)](http://shang.qq.com/wpa/qunwpa?idkey=29f4d06e7fa2b401bc231274d08ada879db777bbf955a44c0e598aaf3d574963) QQ群号：337344607


## Features

* Concurrent 
* Fit for vertical communities
* Flexible, Modular
* Native Go implementation
* Can be expanded to an individualized crawler easily


## Requirements

* Go 1.2 or higher

## Documentation

[中文文档](https://github.com/hu17889/go_spider/wiki/%E4%B8%AD%E6%96%87%E6%96%87%E6%A1%A3) && [常见问题](https://github.com/hu17889/go_spider/wiki/%E5%B8%B8%E8%A7%81%E9%97%AE%E9%A2%98%E4%B8%8E%E5%8A%9F%E8%83%BD%E8%AF%B4%E6%98%8E).


## Installation

```
go get github.com/hu17889/go_spider
go get github.com/PuerkitoBio/goquery
go get github.com/bitly/go-simplejson
go get golang.org/x/net/html/charset
```

This project is based on [simplejson](https://github.com/bitly/go-simplejson/blob/master/simplejson.go), [goquery](https://github.com/PuerkitoBio/goquery).

You can download packages from [http://gopm.io/](http://gopm.io/) in China.

## Use example

Here is an example for crawling github content. You can have a try of the crawl process.
* `go install github.com/hu17889/go_spider/example/github_repo_page_processor`
* `./bin/github_repo_page_processor`

More examples here: [examples](https://github.com/hu17889/go_spider/tree/master/example).


## Make your spider

``` Go
    // Spider input:
    //  PageProcesser ;
    //  Task name used in Pipeline for record;
    spider.NewSpider(NewMyPageProcesser(), "TaskName").
        AddUrl("https://github.com/hu17889?tab=repositories", "html"). // Start url, html is the responce type ("html" or "json")
        AddPipeline(pipeline.NewPipelineConsole()).                    // Print result on screen
        SetThreadnum(3).                                               // Crawl request by three Coroutines
        Run()
```

- Use default modules 

 - Downloader：HttpDownloader
 - Scheduler：QueueScheduler
 - Pipeline：PipelineConsole，PipelineFile

- Use your modules

Just copy the default modules and modify it!

If you make a Downloader module, you can use it by `Spider.SetDownloader(your_downloader)`.

If you make a Pipeline module, you can use it by `Spider.AddPipeline(your_pipeline)`.

If you make a Scheduler module, you can use it by `Spider.SetScheduler(your_scheduler)`.


## Extensions

Extensions folder include modulers or other tools someone sharing. You can push your code without bugs.

## Resumable crawling with RunCheckpoint

`Spider.RunCheckpoint(ctx, dir)` runs the same pipeline as `Run` but keeps the
crawl frontier (queued requests, in-flight requests and completed urls) in a
checkpoint directory. A process killed mid-crawl can be restarted with the
same directory and resumes the remaining requests instead of traversing the
entry pages again. One directory belongs to a single crawl process; Redis and
multi-process task stealing are not involved.

``` Go
sp := spider.NewSpider(NewMyPageProcesser(), "TaskName").
    AddPipeline(pipeline.NewPipelineConsole()).
    SetThreadnum(3)
sp.AddUrl("https://example.invalid/index", "html")

// Re-run the same program after a crash: it resumes from this directory.
if err := sp.RunCheckpoint(context.Background(), "./crawl-state"); err != nil {
    // Fix the problem and rerun; the frontier is still on disk.
}
```

Key semantics:

- De-duplication uses the raw url string and is scoped to one crawl
  (one directory): queued, in-flight and completed urls all count, and a url
  keeps the parameters of its first admission, so link cycles are safe.
- Requests persist url, response type, method, postdata, urltag, header,
  cookies and proxyHost: a POST is still a POST after restart. Requests with a
  custom redirect callback or non-nil Meta are rejected with an error before
  being persisted or downloaded.
- The claim is persisted before the Downloader runs; the parent completion and
  every child request it discovers commit in one atomic write. A page killed
  before that commit is replayed, so pipeline outputs must be idempotent.
- The three download attempts are kept; after they are exhausted the request
  stays recoverable and `RunCheckpoint` returns an error instead of retrying
  forever. Processor/pipeline panics and disk write failures are also errors,
  never a clean finish.
- Cancelling `ctx` stops new claims while in-flight pages finish and save,
  then `ctx.Err()` is returned. `Finish` runs only when the frontier is fully
  empty, at most once per checkpoint directory.

See [`docs/checkpoint.md`](docs/checkpoint.md) for the full guide and
[`example/checkpoint_resume`](example/checkpoint_resume) for a runnable
cyclic-site example.

## Modulers

### Spider

**Summary:** Crawler initialization, concurrent management, default moduler, moduler management, config setting.

**Functions:** 

- Clawler startup functions: Get, GetAll, Run
- Add request: AddUrl, AddUrls, AddRequest, AddRequests
- Set main moduler: AddPipeline(could have several pipeline modulers), SetScheduler, SetDownloader
- Set config: SetExitWhenComplete, SetThreadnum(concurrent number), SetSleepTime(sleep time after one crawl)
- Monitor: OpenFileLog, OpenFileLogDefault(open file log function, logged by **mlog** package), CloseFileLog, OpenStrace(open tracing info printed on screen by stderr), CloseStrace

### Downloader

**Summary:** Spider gets a Request in Scheduler that has url to be crawled. Then Downloader downloads the result(html, json, jsonp, text) of the Request. The result is saved in Page for parsing in PageProcesser.
Html parsing is based on **goquery** package. Json parsing is based on **simplejson** package. Jsonp will be conversed to json. Text form represents plain text content without parser. 

**Functions:**

- Download: download content of the crawl objective. Result contains data body, header, cookies and request info.

### PageProcesser

**Summary:** The PageProcesser moduler only parse results. The moduler gets results(key-value pairs) and urls to be crawled next step. 
These key-value pairs will be saved in PageItems and urls will be pushed in Scheduler.

**Functions:**

- Process: parse the objective crawled.

### Page

**Summary:** save information of request.

**Functions:** 

- Get result: GetJson, GetHtmlParser, GetBodyStr(plain text)
- Get information of objective: GetRequest, GetCookies, GetHeader
- Get Status of crawl process: IsSucc(Download success or not), Errormsg(Get error info in Downloader)
- Set config:SetSkip, GetSkip(if skip is true, do not output result in Pipeline), AddTargetRequest, AddTargetRequests(Save urls to be crawled next stage), AddTargetRequestWithParams, AddTargetRequestsWithParams, AddField(Save key-value pairs after parsing)


### Scheduler

**Summary:** The Scheduler moduler is a Request queue. Urls parsed in PageProcesser will be pushed in the queue.

**Functions:**

- Push
- Poll
- Count

### Pipeline

**Summary:** The Pipeline moduler will output the result and save wherever you want. Default moduler is PipelineConsole(Output to stdout) and PipelineFile(Output to file)

**Functions:**

- Process


### Request

**Summary:** The Request moduler has config for http request like url, header and cookies.

**Functions:**

- Process



## License
go_spider is licensed under the [Mozilla Public License Version 2.0](https://github.com/hu17889/go_spider/blob/master/LICENSE)

Mozilla summarizes the license scope as follows:
> MPL: The copyleft applies to any files containing MPLed code.


That means:
  * You can **use** the **unchanged** source code both in private as also commercial
  * You **needn't publish** the source code of your library as long the files licensed under the MPL 2.0 are **unchanged**
  * You **must publish** the source code of any **changed files** licensed under the MPL 2.0 under a) the MPL 2.0 itself or b) a compatible license (e.g. GPL 3.0 or Apache License 2.0)

Please read the [MPL 2.0 FAQ](http://www.mozilla.org/MPL/2.0/FAQ.html) if you have further questions regarding the license.

You can read the full terms here: [LICENSE](https://raw.github.com/go-sql-driver/mysql/master/LICENSE).
