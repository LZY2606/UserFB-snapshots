# h261enc
A useless H.261 encoder with RDO

I do not know how the H.261 video buffer model works.

## Optional per-GOB budget rate control

In addition to the legacy fixed-quantiser (`H261_RATECTRL_MODE_QUANT`) and
whole-frame VBR (`H261_RATECTRL_MODE_VBR`) modes, the encoder can pick a base
GQUANT per GOB from a link budget. Enable it with
`h261_ratectrl_init_gob_budget()` (see `src/h261/h261_ratectrl.h` and
`examples/gob_budget_example.c`). Reinitialising with `h261_init_ratectrl()`
turns the mode off again.

### Configuration (`h261_gob_budget_config_t`)

| field          | unit            | valid range            |
|----------------|-----------------|------------------------|
| `i_bitrate`    | bits / second   | 1 .. 100000000         |
| `i_fps_num`    | frames / second numerator | 1 .. 1000000 |
| `i_fps_den`    | frames / second denominator | 1 .. 1000000 |
| `i_buffer_bits`| bits            | 1 .. 1000000000        |
| `pf_observe`   | callback        | may be NULL            |
| `p_opaque`     | user pointer    | passed to `pf_observe` |

The frame rate ratio `i_fps_num / i_fps_den` must lie in [1, 120]. A NULL
config or any out-of-range value makes the call return 0 and leaves the
control state untouched; on success it returns 1 and restarts the budget
history. The configured frame rate only drives the budget clock and does not
change the meaning of `temporal_reference`.

### Budget accounting

QCIF frames have 3 GOBs, CIF frames 12. After the K-th GOB the cumulative
budget is exactly `floor(K * bitrate * fps_den / (fps_num * G))` bits, with
the integer remainder carried forever. Each GOB's quota is the difference of
consecutive cumulative budgets. The debt `D` starts at 0, grows by
`bits - quota` after every GOB, and is never reset at frame boundaries nor
clamped to zero. The picture header is billed to the first GOB of its frame;
every other GOB is charged only its own header and macroblocks. A sub-byte
remainder left in the writer across frames is counted exactly once, and the
final padding bits are never counted.

For every GOB each base GQUANT 1..31 is trial-encoded through the full
pipeline and measured for real bits and reconstruction SSE. Among candidates
satisfying `D + bits - quota <= buffer_bits` the one with the smallest SSE is
committed (ties: fewer bits, then smaller quantiser). If nothing fits, the
fewest-bit candidate is committed anyway (ties: smaller SSE, then smaller
quantiser) and `overflow` is reported; the GOB is still emitted complete and
legal, nothing is dropped or truncated.

### Statistics record (`h261_gob_budget_stats_t`)

The observe callback fires exactly once per committed GOB. The record is only
**borrowed** for the duration of the callback — copy anything you need to
keep. Both the record and the observer pointer belong to the encoder instance.

| field            | unit  | meaning |
|------------------|-------|---------|
| `i_frame_num`    | frame | input frame number of the picture |
| `i_gob_number`   | —     | GOB number as written into the bitstream (1..12) |
| `i_gquant`       | —     | base GQUANT of the committed candidate (1..31) |
| `i_overflow`     | bool  | 1 when no candidate fit the buffer (least-bit candidate committed) |
| `i_trials`       | count | real candidate encodings run for this GOB |
| `i_bits`         | bits  | effective bits spent on this GOB |
| `i_quota`        | bits  | budget quota granted to this GOB |
| `i_coded_total`  | bits  | cumulative effective bits after this GOB |
| `i_target_total` | bits  | cumulative link budget after this GOB |
| `i_debt_after`   | bits  | settlement debt `D` after this GOB (may be negative) |
| `i_sse`          | sum of squared sample differences | Y/Cb/Cr reconstruction error of the committed GOB |

`overflow` means the buffer constraint could not be met by any of the 31
candidates; the encoder still commits the fewest-bit candidate and keeps
going, so the stream stays decodable while the debt grows.
