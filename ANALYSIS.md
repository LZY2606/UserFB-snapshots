# pyais：从 NMEA 句到消息对象的组装链审计

- 审计提交：`ba9a339c8d976be83430d4c496b40a188defa149`（当前 `HEAD`）
- 链路范围：NMEA 句解析（tag block / checksum）→ 多片缓冲组装 → 六位解码与消息分派 → 属性访问
- 复现入口：`tests/test_assembly_chain_audit.py`，每个案例可单独点名运行，例如：

```bash
.venv/bin/python -m pytest \
  tests/test_assembly_chain_audit.py::TestAssemblyChainAudit::test_same_seq_id_different_sources_get_stitched_together -q
```

本审计只沿现有模块边界（`stream.py` / `queue.py` / `messages.py` / `bit_vector.py` / `decode.py`）记录现状，
不改动生产代码；测试用于把失败现场固定在当前 commit 上。所有行号均来自该 commit。

## 1. 组装链分层与关键位置

| 阶段 | 模块 / 位置 | 职责 |
| --- | --- | --- |
| Tag block 切分与解析 | `pyais/messages.py:156`（`NMEASentenceFactory._pre_process`）、`pyais/messages.py:366`（`TagBlock.init`） | 剥离 `\...\` 前缀、校验 tag block checksum、解析 `g` 分组 |
| NMEA checksum | `pyais/messages.py:561`（`NMEASentence.is_valid`）、`pyais/util.py:262`（`compute_checksum`）、`pyais/util.py:342`（`chk_to_int`） | 惰性计算并比对句校验和 |
| 句工厂 / 类型分派 | `pyais/messages.py:177`（`NMEASentenceFactory.produce`）、`pyais/messages.py:208`（未知句型抛 `UnknownMessageException`） | 区分 AIVDM/VDO 与 Gatehouse 句 |
| 流内多片缓冲 | `pyais/stream.py:154`（`AssembleMessages._assemble_messages`）、`pyais/stream.py:184`（slot 键）、`pyais/stream.py:190`（按片号写入） | 交错流中聚合多片消息 |
| Tag block 分组缓冲 | `pyais/stream.py:62`（`TagBlockQueue`）、`pyais/stream.py:93`（未知 group 静默返回） | 按 `g:X-Y-Z` 聚合句子组 |
| 队列式多片缓冲 | `pyais/queue.py:19`（`NMEAQueue.buffer`）、`pyais/queue.py:64`（slot 键）、`pyais/queue.py:70`（按片号写入） | 与流路径并行的另一条组装实现 |
| 六位（six-bit）解码 | `pyais/bit_vector.py:11`（`_PAYLOAD_ARMOR` 表，越界字符映射为 0）、`pyais/bit_vector.py:41`（`bit_vector.__init__` 重映射为 base64 解码） | AIS payload 字符 → 比特向量 |
| 消息分派 | `pyais/messages.py:753`（`AISSentence.decode`）、`pyais/messages.py:764`（`MSG_CLASS[self.ais_id]`）、`pyais/messages.py:766`（未知 AIS 型抛错）、`pyais/messages.py:3754`（`MSG_CLASS` 表，仅 0–28） | 按消息型选择 `Payload` 子类 |
| 属性访问 | `pyais/messages.py:541`（`NMEASentence.__getitem__` 退化为 `getattr`）、`pyais/messages.py:742`（`is_single` 依赖空 seq_id） | 句对象字段读取 |

## 2. 三个兼容性边界（含文件行号与证明测试）

### 边界 #1：slot 键不含来源标识 —— 不同来源串片

- 流路径的缓冲槽位是 `(seq_id, channel, talker_id, type, frag_cnt)`（`pyais/stream.py:184`），
  其中没有任何发送源标识；同槽位再次出现时直接按片号覆盖（`pyais/stream.py:190`）。
- 队列路径更弱：槽位只有 `(seq_id, channel)`（`pyais/queue.py:64`），空 seq_id 被归一化为 `-1`
  （`pyais/queue.py:59-61`，流路径对应 `pyais/stream.py:179-181`），连 talker/类型/frag_cnt 都不参与。
- NMEA 0183/AIS 句本身不带发送源字段；多路复用（TCP 汇聚、多接收机、UDP 混合源）时，
  两个来源只要序号与频道相同，就会互相覆盖片槽并拼出不属于同一条消息的载荷。
- 证明测试：
  - `test_same_seq_id_different_sources_get_stitched_together`：来源 A 的第 1 片被来源 B 覆盖，
    B 的第 2 片到达后立即吐出拼接消息，来源 A 永久丢失（`PART_2` 不再出现在输出中）。
  - `test_empty_seq_id_collides_in_nmea_queue`：两条空序号多片消息在 `NMEAQueue` 中抢占 `(-1, 'A')`，
    只有先凑齐的一条出队，另一条被静默吞掉。
  - 对照组 `test_interleaved_channels_share_buffer_slot`：频道不同（A/B）时因 slot 含 channel 而不串，
    反证串片的决定因素是“同槽位 + 无来源维度”。

### 边界 #2：诊断被静默丢弃 —— 坏 checksum、未知消息型、越界六位字符

- 组装循环只捕获解析期异常并 `continue`（`pyais/stream.py:167-169`；队列路径 `pyais/queue.py:42-44`），
  从不检查 `is_valid`。校验和只在惰性属性里计算（`pyais/messages.py:561-565`），
  因此坏 checksum 的句子会被原样 `yield`，流层面零告警。
- 消息分派发生在显式 `decode()` 时：`MSG_CLASS` 只注册了 0–28 型（`pyais/messages.py:3754` 起），
  未命中即抛 `UnknownMessageException`（`pyais/messages.py:764-766`）。流组装阶段不做解码，
  未知型句子照常流出，失败被推迟到消费端且没有上下文聚合点。
- 六位解码表把 ASCII 48–119（`'0'`–`'w'`）之外的所有字节映射为 0（`pyais/bit_vector.py:11-12`，
  注释自述 “upstream validation is expected”），破坏字符与合法 `'0'` 不可区分，无异常、无计数。
- Tag block 的 checksum 同样只记录 `_is_valid`（`pyais/messages.py:366-373`），
  `TagBlockQueue.put_sentence` 不看该标志（`pyais/stream.py:68-107`），坏 tag 校验不影响分组。
- 证明测试：
  - `test_bad_checksum_is_emitted_as_invalid_without_diagnostic`：坏 checksum 句子被吐出且 `is_valid is False`。
  - `test_unknown_message_type_streams_but_fails_at_decode`：63 型消息流过组装，仅在 `.decode()` 抛 `UnknownMessageException`。
  - `test_sixbit_out_of_alphabet_byte_is_silently_mapped_to_zero`：越界字符 `'!'` 与合法 `'0'` 都解为 0。

### 边界 #3：缓冲状态只在“成功凑齐”时释放 —— 缺片 / 流结束 / 孤儿分组错误复用或无声滞留

- 流缓冲槽只在全部片凑齐后 `del buffer[slot]`（`pyais/stream.py:194-198`）；
  生成器结束时没有任何 flush/告警路径，缺中间片（或缺尾片）时残留槽位随对象消失，无消息也无异常。
- 同槽位被新来源复用即边界 #1 的覆盖写入；而 `[None] * max(fragment_count, 0xff)`
  （`pyais/stream.py:188`，队列路径 `pyais/queue.py:68`）固定按 255 槽预分配，
  完成判定又以“最新到达句”的 `fragment_count` 切片（`pyais/stream.py:191-193`），
  使旧片可以被新计数逻辑复用。
- Tag block 分组遇到未知 `group_id`（首句缺失）直接 `return`（`pyais/stream.py:93-95`），
  `groups` 在流结束时同样不回收（构造于 `pyais/stream.py:64-66`），孤儿句无任何诊断。
- 证明测试：
  - `test_missing_fragment_is_dropped_silently_at_stream_end`：只喂第 1 片，流结束输出为空且不抛错。
  - `test_tag_block_group_orphan_fragment_is_dropped_silently`：只收到 `g:2-2-42` 的第 2 句，
    `TagBlockQueue` 队列长度保持 0。

## 3. 可单独点名运行的案例清单

| 场景 | 测试名 | 观测到的现状 |
| --- | --- | --- |
| 交错频道（A/B 同序号） | `test_interleaved_channels_share_buffer_slot` | 两条消息均正常输出，频道参与 slot |
| 相同序号不同源（串片） | `test_same_seq_id_different_sources_get_stitched_together` | 后到来源覆盖先到来源并抢先成句 |
| 缺中间片 / 缺尾片 | `test_missing_fragment_is_dropped_silently_at_stream_end` | 流结束无输出、无异常 |
| 坏 checksum | `test_bad_checksum_is_emitted_as_invalid_without_diagnostic` | 原样吐出，仅 `is_valid=False` |
| 空序号 | `test_empty_seq_id_collides_in_nmea_queue` | `NMEAQueue` 中归一化 `-1` 后撞槽丢消息 |
| 未知消息型 | `test_unknown_message_type_streams_but_fails_at_decode` | 组装放行，`.decode()` 抛 `UnknownMessageException` |
| 流结束（残留缓冲） | `test_missing_fragment_is_dropped_silently_at_stream_end` | 缓冲槽无 flush/告警路径 |
| 附：tag block 孤儿分组 | `test_tag_block_group_orphan_fragment_is_dropped_silently` | 未知 group 静默 `return` |
| 附：六位越界字符 | `test_sixbit_out_of_alphabet_byte_is_silently_mapped_to_zero` | 越界字节映射为 0，与合法字符混淆 |

## 4. 复现与验证

```bash
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/python -m pytest tests/test_assembly_chain_audit.py -q   # 8 passed
.venv/bin/python -m pytest -q                                      # 全量回归
```

最小 instrumentation 核对方式（无需改库代码）：

```bash
# 串片现场：观察输出载荷由 foreign 两片拼成
.venv/bin/python -m pytest \
  tests/test_assembly_chain_audit.py::TestAssemblyChainAudit::test_same_seq_id_different_sources_get_stitched_together -q
# 六位映射现场：bit_vector 对 '!' 与 '0' 返回同一值
.venv/bin/python -c "from pyais.bit_vector import bit_vector; print(bit_vector(b'!').get(0,6), bit_vector(b'0').get(0,6))"
```
