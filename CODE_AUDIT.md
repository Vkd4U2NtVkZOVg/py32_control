# 固件用户代码审核报告

| 项 | 值 |
|---|---|
| 审核对象 | `Core/` 下全部用户编写的固件源码（.c/.h） |
| 排除范围 | `Drivers/Embedded-PID`、`Drivers/QuarkTS`（第三方库，不在审核范围） |
| 审核基线 | 分支 `codex/tec-fault-interlocks`，HEAD `300efa8` |
| 审核时间 | 2026-10-13 |
| 审核方式 | 逐函数源码级通读 + 逻辑推演 |

---

## 0. 结论摘要

固件整体结构健康：故障联锁分层、参数校验、双槽 NVRAM 断电安全、DMA 环形缓冲、PID 语义、软 I2C 时序均经得起推演，注释与实现的一致性在同类工程中属于上乘。

**当前真正需要关注的是 1 处高风险 + 3 处中风险**，其中 M2（热预检失败后风扇停摆）与 M3（STAGE5 无重发）直接影响实际硬件行为且有明确的责任缺口，建议优先处理。其余为低危/清晰度问题。

---

## 1. 高风险

### H1. `adc.c` 校准函数：无意义且可能丢位的左移位移

**位置**：`Core/SRC/adc.c:146-151`

```c
ADC_CALRR1_Buff[i]= *((__IO uint32_t *)(0x40012448));
ADC_CALRR1_BuffInt[i]=(int32_t)(ADC_CALRR1_Buff[i]<<9);   // ← 左移 9 位
...
*((__IO uint32_t *)(0x40012450)) = (ADC_CALRR1_BuffInt[2]>>9);
```

**问题**：先 `<<9` 排序再 `>>9` 写回。若校准值有效位超过 23 位，`<<9` 会截断高位，排序结果失真；而中值校准理论上低十几位才有效，位移既无必要又有丢位风险。同时 `0x40012448/50/54/44` 是裸地址魔法数，项目其它代码走 LL 宏，此处维护性差且无校验。

**建议**：直接对 `ADC_CALRR1_Buff[i]` 排序取中值，去掉 `<<9/>>9`；用 LL 定义的寄存器位域宏替换裸地址。

---

## 2. 中风险

### M2. 热预检失败（STRAP_FAIL）后，风扇没有任何所有者

**位置**：`Core/SRC/sdm845_boot.c:543-556`（STRAP_FAIL）+ `Core/SRC/tec_control.c:678-683`（THERMAL_WAIT）

**现状**：
- `sdm845_boot` 的 `THERMAL_PRECHECK` entry 强制 `Fan_SetEnabled(1)` 接管风扇冷却（[sdm845_boot.c:201](Core/SRC/sdm845_boot.c:201)）。
- 一旦 precheck 因超时/传感器问题进入 `STRAP_FAIL`，entry 释放按键但不做任何风扇处理（[sdm845_boot.c:543](Core/SRC/sdm845_boot.c:543)）。
- 与此同时 `tec_control` 的 `THERMAL_WAIT` 分支（`Sdm845_Boot_IsThermalReady()==0`）**不驱动风扇**，注释称"precheck 拥有它"（[tec_control.c:678](Core/SRC/tec_control.c:678)）。

**后果**：若热预检失败并卡在 STRAP_FAIL，风扇既不被 sdm845_boot 驱动、也不被 TEC 驱动，散热完全停摆；若机箱持续发热，没有任何软件路径会主动开风扇。

**建议**：在 `STRAP_FAIL` entry 显式 `Fan_SetDutyPercent(100)`（或至少保持 `Fan_SetEnabled(1)`），或在 `tec_control` 的 THERMAL_WAIT 分支补充风扇驱动逻辑。需要确认设计意图：预检失败是否应视为"需要人工干预的故障态"并保持散热。

---

### M3. STAGE5 命令无重发，10s settle 与 15s 超时构成竞态

**位置**：`Core/SRC/sdm845_boot.c:449-481`

**现状**：
- STAGE3/4 的 poll 分支都调用了 `sdm845_stage_send_retry()`（[sdm845_boot.c:402](Core/SRC/sdm845_boot.c:402)、[sdm845_boot.c:437](Core/SRC/sdm845_boot.c:437)），命令被 `USART2_DMA_TxBusy()` 拒绝时会重发。
- **STAGE5 的 poll 分支没有调用 `sdm845_stage_send_retry()`**（[sdm845_boot.c:473](Core/SRC/sdm845_boot.c:473) 只有 IsStage5Seen 检查）。
- STAGE5 entry 的 amixer 命令带 `SDM845_T_STAGE5_SETTLE_MS = 10s` settle（[sdm845_boot.c:77](Core/SRC/sdm845_boot.c:77)、[sdm845_boot.c:454](Core/SRC/sdm845_boot.c:454)），而 STAGE5 超时是 `SDM845_T_STAGE345_S = 15s`。

**后果**：若 amixer 命令因 TX 忙被拒一次，STAGE5 永远不会重发，15s 超时后误入 STRAP_FAIL（即便 SoC 已经准备好）。同时 10s settle + 剩余 5s 响应窗口偏紧。

**建议**：在 STAGE5 的 `default`（poll）分支补上 `sdm845_stage_send_retry()`，与 STAGE3/4 一致；必要时放宽 STAGE5 超时。

---

### M4. 负值参数以无符号补码暴露给主机，协议未标注符号

**位置**：`Core/SRC/nv_params.c:159`（默认值 `(uint32_t)(-20000)`）+ `Core/SRC/tres_i2c_slave.c:110-115`（大端无符号窗口）

**现状**：`NV_TEC_TEMP_VALID_MIN_MILLIC`、`NV_TEC_TARGET_BASE_MILLIC`（可能为负）等以 `uint32_t` 存储、I2C 窗口以 4 字节无符号暴露。主机工具需自行知道哪些槽带符号（m°C），协议文档未明确。

**后果**：主机写错符号或对带符号槽做无符号比较，参数会静默错值（多数会被 `tec_params_load` 的包络/关系校验兜底，但依赖校验而非协议清晰度）。

**建议**：在 `nv_params.h` 每个槽注释或主机工具中标注带符号槽；考虑协议加符号位/专用类型。

---

## 3. 低风险

### L1. SVC / PendSV 中断处理函数为死循环

**位置**：`Core/SRC/py32f030_it.c:114-140`

QuarkTS 使用协作式调度（`qOS_Run` 循环 + SysTick），不触发 SVC/PendSV，因此当前不执行。但一旦任何代码执行 `svc` 指令或未来切换抢占式调度，固件会死锁。建议注释说明原因或改为安全默认。

### L2. TEC 周期可被调到 1ms，而 ADC 每通道等待 1ms

**位置**：`Core/SRC/tec_control.c:258-260`（周期校验允许 1ms）+ `Core/SRC/adc.c:243`（每通道 `LL_mDelay(1)`）

一次 TEC 周期读 3 个 ADC 通道共 ~3ms 等待。对默认 500ms 周期无影响；但若主机把 `NV_TEC_CONTROL_PERIOD_MS` 设为 1ms，TEC 周期将被 ADC 等待吞噬 3 倍，控制失效。建议周期下限放宽到能覆盖 ADC+软 I2C 的最差执行时间，或在文档标注。

### L3. 文档与实现不一致（低危，会误导排障）

- `tres_sequencer_state_diagram.md` 的 `BOOT_WAIT_DFU`/`PULSE_RESET_DFU` 细节落后于 `tres_seq.c` 实现。
- `WORKLOG_CURRENT.md` 部分参数默认值与 `nv_params.c` 实际不符（此前审计已指出 PID 增益 0.1/0.1/0.1、DAC 窗口 760/1600 等）。

---

## 4. 已确认正确的关键点（顺带复核，非缺陷）

| 模块 | 结论 |
|---|---|
| `nv_params.c` 双槽 NVRAM | CRC(seq+params) v5、seq 回绕（`delta<0x80000000`）、写非活跃槽+读回校验、每退出路径重锁 FLASH，逻辑正确。缓存快照已在临界区内（上轮提交 `300efa8`）。 |
| `NvParams_Set` 从 ISR 写 | 单槽 4 字节对齐写原子 + cache generation，TEC 按代数重载，正确。 |
| `tec_pid.c` | ki·Ts / kd÷Ts 归一化、D 项 LPF 从 0 seed、条件积分抗饱和，全部正确。 |
| `sdm845_detect.c` | 5 路独立 KMP 匹配器、字节流连续匹配 + idle 窗口判定、`ResetAll` 在失速/重试时复位，正确。 |
| `usart2_dma.c` | 读前读后双重检查写位置 + gap 检测 + overrun 计数，环形消费逻辑正确；慢 IRQ 时丢弃而非拼接，安全取向正确。 |
| `soft_i2c.c` | 事务级 IRQ 屏蔽、SDA 开漏回读、NACK/超时处理齐全；`len` 上限 64 匹配所有调用者。 |
| `aht20.c` | CRC8 校验、20/24 位解包、露点 Magnus + 查表 ln(RH)，正确。 |
| `aes.c` | S-box/ShiftRows/MixColumns 符合 FIPS-197，仅编译于 `TRES_AES_ENABLE=1`。 |
| `main.c` 初始化顺序 | 与各模块依赖匹配，QuarkTS 协作调度下 SysTick 为唯一时基，正确。 |

---

## 5. 处理优先级建议

1. **M2（热预检失败后风扇停摆）** — 硬件安全相关，优先确认设计意图并补风扇保持逻辑。
2. **M3（STAGE5 无重发）** — 一行补齐 `sdm845_stage_send_retry()`，与 STAGE3/4 对齐。
3. **H1（ADC 校准位移）** — 去掉 `<<9/>>9`，用 LL 寄存器访问。
4. M4/L1/L2/L3 可按版本迭代清理。

---

*本报告为静态源码审核结论，未经实机验证。涉及硬件行为（风扇、复位时序、DAC 端点）的建议在实施后仍需台架确认。*
