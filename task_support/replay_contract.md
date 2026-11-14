在 src/script_replay.h 声明以下 C 接口，由 src/script_replay.c 与原生产解析链共同实现：

```c
#include "gumbo.h"
typedef GumboStringPiece (*GumboScriptReplay)(const GumboOutput *partial,
                                             const GumboNode *script,
                                             void *userdata);
typedef struct {
  GumboOutput *tree;
  GumboStringPiece source;
} GumboReplayOutput;
GumboReplayOutput *gumbo_parse_with_script_replay(const GumboOptions *options,
    const char *input, size_t length, GumboScriptReplay replay, void *userdata);
void gumbo_destroy_replay_output(const GumboOptions *options,
                                GumboReplayOutput *output);
```

这是已知脚本输出的回放协议，不解释 JavaScript、不取网络资源，也不模拟 async/defer。非空 replay 在完整页面中每个被真正 HTML script 结束 token 关闭的 HTML script 上调用一次，不按 src/type/async/defer 筛选。仅 EOF 隐式闭合的不回调。处于任意 HTML template 内容内、非 HTML namespace 的 script，及整个 fragment 解析过程均不回调；HTML integration point 内重新生成的 HTML script 可以回调，前提是没有 template 祖先。

回调发生在脚本文本和 original_end_tag 完整、脚本已从开放栈弹出、原插入模式已恢复之后，且下一 token 尚未消费。partial 只读，script 属于这棵活树。不可在回调里修改树、销毁树或递归进入同一解析会话。回调可调用独立的普通解析入口；各次解析的状态必须隔离。回调返回值是待写入字节，空值不写入；非空 data 的内容应至少保持到下一次回调或外层 parse 返回，库须在恢复解析前复制。调用方负责保证回放会终止。

返回字节插在刚结束 script 后、尚未读取的输入之前，直接进入当下的 tokenizer 和 HTML 构树状态，不作为独立片段解析。输出内部再次闭合的 script 会先触发回调，再继续其后面的字节。插入串可以不完整，例如 `<div class=` 后紧接原尾部 `card>...`；实体、CRLF 和 UTF-8 字符也可以跨这个连接处。与插入内容相邻的既有 token、开放栈、格式元素、form 和表格寄养状态继续有效，不重置、不重放已处理 token。

source 是最终逻辑字节序列：原正文中每个触发脚本的完整结束标签后插入其输出，再递归处理输出内脚本。返回结果自己拥有 source，即使 callback 为 NULL 也复制原输入。source 不含额外结尾 NUL 的计入长度，可自行增加终止符。所有来自输入的非空 original_tag/original_end_tag/original_text/属性 original_name/original_value，以及错误原文指针，均定位在这份 source 内；源位置的 offset/line/column 按这份展开后的 UTF-8 输入计数，沿用原库的 CRLF、制表符规则。source 在 destroy 前有效。回调期间已有原文切片有效，但不得把切片指针跨回调保存；仍在最终树中的既有节点地址不能改变。

语法容错与原选项保持。合法回放最终树与对完整 source 一次性解析的树、parse flags、位置和错误序列一致；树形相同不能替代独立的关键节点及顺序断言。原 gumbo_parse/gumbo_parse_with_options 的借用输入约定和结果不变。所有新增内存使用传入的 allocator/deallocator，单独释放 replay wrapper、source 与树且不重复释放。

新增 tests/script_replay_contract.c，不写 main；导出 task_cases 和 task_case_count，用 task_support/check.h 的 TASK_CHECK 登记 INSERTION、CONTEXT、SOURCE 三组。分别覆盖嵌套回放与部分标签/实体连接、真实script闭合和惰性上下文/表格现场、展开位置和节点身份/分配释放。至少包含正常不回放对照，不能用标记打印代替断言。runner 直接编译 src/*.c（包括新增的 script_replay.c）及此回归；缺文件、零用例或任一组没有断言均失败。
