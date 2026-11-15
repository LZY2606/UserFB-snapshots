# HTML 字段增量采集 API

在 source/lexbor/html/collection.h、collection.c 实现以下公开接口，类型沿用仓库的 lxb_status_t、lxb_dom_element_t、lxb_dom_node_t、lexbor_str_t。无需实现旧草稿的通用 DOM observer。

- lxb_html_collection_t *lxb_html_collection_create(lxb_dom_element_t *root, const lxb_html_collection_options_t *options, lxb_status_t *status)
- lxb_status_t lxb_html_collection_sync(lxb_html_collection_t *, lxb_html_collection_batch_t **out)
- lxb_html_collection_batch_t *lxb_html_collection_batch_destroy(lxb_html_collection_batch_t *)（返回 NULL，允许 NULL）
- lxb_html_collection_t *lxb_html_collection_destroy(lxb_html_collection_t *)（返回 NULL，允许 NULL）

## 配置与全量结果的定义

lxb_html_collection_field_t 的字段是 const char *match_attribute、*match_value、*value_attribute。lxb_html_collection_options_t 的字段是 const char *row_attribute、*key_attribute，const lxb_html_collection_field_t *fields，size_t field_length。字符串均为 NUL 结尾；名称限小写 ASCII [a-z][a-z0-9-]*，match_value 允许空串。value_attribute=NULL 表示取文本，否则取指定属性。root、options、status、fields 不得为 NULL，field_length 至少为 1；无效配置返回 NULL 并设置非 OK 状态，成功设 OK。create 复制整个配置及字符串，调用方随后可以修改或释放它们。root 必须属于已完成解析的 HTML document，保持在该 document 内且不被移动。

范围含 root 自身，按实际 DOM 前序遍历 HTML 元素和文本，不进入非 HTML 命名空间子树或 template.content。带 row_attribute 的元素是记录边界，不论标记的值是什么；key_attribute 存在且非空的边界才是有效行。主键按已解析的原字节比较，不裁空白、不折叠大小写。缺失/空键的边界仍截断外层字段归属。嵌套有效行各自输出。

每条字段规则在行的严格后代中寻找 match_attribute 的值与 match_value 逐字节相等的第一个元素；跳过嵌套记录边界及其全部子树，边界元素自己也不作为外层字段。不同规则可命中同一个元素。取属性时，首个命中元素缺少 value_attribute 就返回缺失，不能继续找第二个；存在的空属性是 present=true、长度 0。取文本时拼接该元素后代的 TEXT 字节，忽略注释，跳过嵌套记录边界、非 HTML 子树和 template.content，不裁剪或压缩空白；即使拼接为空也 present=true。无命中则 present=false、bytes 清零。属性和文本均使用 DOM 中已经解码的字节，不再次解码、不补绝对 URL。

所有有效行都要缓存字段，包括暂未胜出的重复主键候选。同主键由当前 DOM 序最早的行胜出，完整结果按胜出行的 DOM 序排列。不同节点内容相同也不能混同身份。

## 批次与净变化

公开结构如下（数组均有对应长度；不相关/不存在的结构清零）：

- lxb_html_collection_value_t：bool present；lexbor_str_t bytes。
- lxb_html_collection_row_t：lxb_dom_node_t *source；lexbor_str_t key；lxb_html_collection_value_t *values；size_t value_length。values 顺序对应配置。
- lxb_html_collection_change_t：kind（LXB_HTML_COLLECTION_UPSERT / LXB_HTML_COLLECTION_DELETE）；bool before_present、after_present；lxb_html_collection_row_t before、after。
- lxb_html_collection_batch_t：lxb_html_collection_row_t *rows；size_t row_length；lxb_html_collection_change_t *changes；size_t change_length；size_t rows_extracted。

sync 首次扫描范围，提交相对于空表的 UPSERT；后续返回当前完整去重表 rows 和相对于该采集器上次成功 sync 的净变化 changes。只在主键、胜出 source 或字段的 present/字节发生变化时产生变化；单纯胜出行调序只改变 rows 的顺序，不产生变化。主键消失是 DELETE（仅 before 存在），新增或仍存在但改变是 UPSERT（after 存在；旧键存在时 before 也存在）。先按旧表顺序列出 DELETE，再按新表顺序列出 UPSERT，每个主键最多一次。一次同步前的多次编辑合并，只比较两次提交状态；改回原值、摘下后原样放回都不得泄露中间操作。键改名按旧键删除、新键加入处理。

成功时 *out 为新分配的批次，空表/空变化也成功，相应数组 NULL/0。内存失败返回非 OK、*out=NULL，不推进已提交比较基准，允许重试。返回的所有数组和字节均属于该批次，后续编辑、sync 和采集器销毁不能改变旧批次；batch_destroy 不销毁 DOM。source 借用真实节点身份，调用方保证相关节点一直存活至采集器和全部批次销毁；先销毁采集器，再销毁文档。

## 增量维护

必须接入真实 DOM 编辑，无需调用方手动标脏或传变更列表。覆盖带事件的普通插入/摘除、element_set_attribute、attr_set_value、属性移除和 character_data_replace 的现有整段替换用法；append_child、insert_before_spec、remove_child、replace_child、replace_all_spec 的 fragment/移动/替换结果也须正确。摘除子树在下次 sync 不再输出，脱离期间的修改不单独输出；重新接回时按当前字节抽取。标记/主键变化、嵌套边界变化、字段子树跨行移动均须维护行归属与重复键候选。

首次 sync 可以全扫；之后更新与 sync 只允许下探被编辑的子树，以及编辑前后最近记录边界所属的行。边界增删时可扫描该边界子树并重抽外层所属行。可以遍历缓存的行/主键元数据、沿祖先比较顺序和复制缓存结果；不得重新遍历 root 全树、重查干净行的字段或借旧的全量属性查询重建。rows_extracted 如实统计本次成功 sync 重新计算整组字段的有效行数：首次含全部有效候选，之后每个受影响行至多一次，重复主键未胜出行也计入；没有编辑时为 0。移动中涉及的行允许重抽，无关行不得重抽。若无关分支本身就是发生编辑的子树，则属于允许扫描范围。

多个采集器可共用文档、重叠 root 以及不同配置；各自缓存、脏状态、提交基准独立。不得覆盖 document->user；普通和 style HTML document 的既有 mutation/attribute 回调继续原样执行，不能额外重复调用。允许新增文档私有状态与内部失效通知，不要求对外提供通用变更历史队列。仅支持已解析文档的单线程同文档编辑；解析期间启用、直接结构体写入、_wo_events、跨文档 adopt、销毁仍被引用的节点、文档 clean/重新解析及并发不在范围内。
