# 变异检出证据（mutation detection evidence）

本目录保存两个针对 `lib/decode.c` 的可编译退化版本，以及它们被
`tests/residual_reference.c` 回归套件拒绝的原始日志。两个变异都不会导致
崩溃或超时，而是被具体的内容断言拒绝，证明测试真正校验了逐 MCU 的
DC 预测历史与 EOB 游程消费，而不是只检查程序能否运行。

复现方式（在仓库根目录，需要一个可写的 TMPDIR）：

    bash .task-evidence/run_variant.sh \
      .task-evidence/variant-pred-clear \
      .task-evidence/variant-pred-clear/run.log
    bash .task-evidence/run_variant.sh \
      .task-evidence/variant-eob-no-decay \
      .task-evidence/variant-eob-no-decay/run.log

## 变异 1：variant-pred-clear（每次 MCU 丢弃 pred_last 历史）

改动位置：`variant-pred-clear/decode.c` 的 `th_decode_packetin()` 主
MCU 循环内、`oc_dec_dc_unpredict_mcu_plane()` 之前。当进入第二及之后的
MCU（`stripe_fragy>0`）时，对 `_dec->pipe.pred_last` 清零，模拟“逐块重建
不把上一 MCU 的按参考预测历史带到下一 MCU”。

被拒绝位置：`run.log` 中
`FAIL tests/residual_reference.c ... got->pred==ex[i].pred`，
由 `stripe_state/mcu_history_carries` 用例触发。该用例让第二 MCU 首行的
GOLDEN 片段没有同参考的上方/左方邻居，生产实现必须回退到第一 MCU 累积的
`pred_last[GOLDEN]`；变异把它清零后预测值改变，独立模型的逐块预测断言
失败。正常库下该用例通过。

## 变异 2：variant-eob-no-decay（EOB 游程不递减）

改动位置：`variant-eob-no-decay/decode.c` 的
`oc_dec_frags_recon_mcu_plane_c()` 内，把 `eob_runs[zzi]--;` 去掉，使一个
EOB 游程在被一个块消费后不衰减、被后续块重复消费。

被拒绝位置：`run.log` 中
`FAIL tests/residual_reference.c ... r->coeffs[k]==(ogg_int16_t)(...)`，
由 `token_runs` / `coefficients` 组用例触发。EOB 游程横跨多个块，不递减
会让本应写入实际系数的块被错误地当成 EOB 块，预变换自然坐标系数与独立
模型按系数矩阵推出的期望值不一致，断言失败。正常库下这些用例通过。
