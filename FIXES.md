# 修复说明：CODE_AUDIT 六项处置

| 项 | 值 |
|---|---|
| 日期 | 2026-10-13 |
| 基线 | 分支 `codex/tec-fault-interlocks`，HEAD `300efa8` |
| 输入 | `CODE_AUDIT.md`（静态审核报告，7 条缺陷） |
| 范围 | 处置其中的 L2、H1、M2、L1、L3、M4（共 6 项） |
| 状态 | **全部完成；EIDE 构建通过，全部测试套件通过；改动尚未提交** |

本文档只记录**本轮实际落地的修复**，作为 `CODE_AUDIT.md` 的执行结果。审核报告中两处结论经复核不成立，见文末 §7。

---

## 1. 总览

| 编号 | 问题 | 严重度 | 处置 | 主要落点 |
|---|---|---|---|---|
| L2 | 控制周期下限 1 ms 无意义 | 低 | 下限改为 10 ms | `Core/SRC/tec_control.c:73` |
| H1 | ADC 校准失败被静默吞掉 | 高（重定性） | 加超时+失败检测+重试+fail-safe | `Core/SRC/adc.c` |
| M2 | 热预检失败后风扇无所有者 | 中 | `STRAP_FAIL` 强制接管并满速 | `Core/SRC/sdm845_boot.c:543` |
| L1 | SVC/PendSV 为死循环 | 低 | 补设计意图说明 | `Core/SRC/py32f030_it.c:121` |
| L3 | 文档与实现不符 | 低 | 按实现修正两处文档 | `WORKLOG_CURRENT.md`、`tres_sequencer_state_diagram.md` |
| M4 | 带符号槽未标注 | 低 | 头文件声明 + 主机工具带符号解码 | `Core/INC/nv_params.h`、`test/tres_i2c_probe.py`、`test/tres_host.py` |

**验证结论**

```
EIDE 全量构建:  build successfully
                RAM   4912 B / 8 KB  (59.96%)
                FLASH 37084 B / 60 KB (60.36%)

相对改动前(4904 / 36912):  +8 B RAM,  +172 B FLASH
```

| 测试套件 | 结果 |
|---|---|
| `run_tec_fault_tests.py` | 通过（`TEC_FAN_FAULT_ENABLE` = default/0/1 三档 + 电流溢出用例 + 非法开关值拒绝） |
| `run_boundary_tests.py` | 8 项通过 |
| `nv_dualslot_sim.py` | 232 项掉电枚举 + 元数据 CRC/回绕/v4 迁移 + 65 逐字断电 + 32 序列号位翻转全部通过 |
| `replay_boot_fsm.py` | 通过 |
| `test_probe.py` / `test_host.py` | 8 项 / 6 项通过 |
| 主机工具 | `py_compile` 通过；`NV_SIGNED_SLOTS` 导入与解码断言通过 |

---

## 2. L2 —— 控制周期下限无意义

**文件**：`Core/SRC/tec_control.c:73`

```c
- #define TEC_PERIOD_MIN_MS              ( 1 )
+ #define TEC_PERIOD_MIN_MS              ( 10 )
```

**原因**：该下限是**任务自身运行时间**的下界，不是调度偏好。`TecControl_Cb()` 每周期读 3 路 ADC（热端、冷端、电流），而 `adc1_convert()` 每路都含一个 `LL_mDelay(1)` 稳定延时（`Core/SRC/adc.c:283`）——即固定 3 ms，尚未计 EOC 忙等与 MCP4725 的软 I2C 写。周期短于任务体就无法兑现：任务会超期运行，环路实际以任务体的速率跑，而 PID 归一化（`ki·Ts`/`kd÷Ts`）、I 项斜率限幅、风扇失速计时器仍按配置周期计算，全部失配。

**取值依据**：10 ms 覆盖 3 ms 强制延时 + EOC 等待 + DAC 事务并留余量。注释中已明确标注这是**量级下限、非实测最坏值**。

**附带效果**：`tec_params_load()` 的校验走该宏，故主机写入 <10 ms 的周期会回落默认值，不需额外改动。

---

## 3. H1 —— ADC 校准失败被静默吞掉（重定性）

**文件**：`Core/SRC/adc.c`

### 3.1 原问题

`CODE_AUDIT.md` 认为 `<<9/>>9` 位移有丢位风险。经复核该论述不成立（见 §7），但排查过程中发现了**更严重的真实缺陷**：

```c
#if (USE_TIMEOUT == 1)          /* 全仓从未定义 USE_TIMEOUT */
    ...
    if (LL_SYSTICK_IsActiveCounterFlag()) {
        if (Timeout-- == 0) {
            /* ← 空语句，超时被静默忽略 */
        }
    }
#endif
```

由于 `USE_TIMEOUT` 在工程中从未定义，整块被预处理掉，导致：

1. `while (LL_ADC_IsCalibrationOnGoing(ADC1))` 是**无界忙等**（校准从不完成则永久卡在启动路径）；
2. 硬件校准失败标志 `CALFAIL` **从未被检查**；
3. 无论校准是否成功，中值结果都被无条件写回并投入使用。

后果：转换器未校准时，TEC 的超温/过流/露点保护会拿**错误的温度与电流**去比较，却仍显示"健康"，属静默失效。

### 3.2 修复

拆分为两个函数：

| 函数 | 行 | 职责 |
|---|---|---|
| `Studio_ADC1_CalibrateOnce()` | `adc.c:126` | 一次校准（保留厂商中值 5 次采样序列）+ 有界自旋 + `CALFAIL` 检测 |
| `Studio_ADC1_Calibrate()` | `adc.c:169` | 最多 `ADC_CAL_ATTEMPTS`(=3) 次尝试；全失败则置 `s_calib_failed` |

新增检测（`adc.c:145` 附近）：

```c
while (LL_ADC_IsCalibrationOnGoing(ADC1) != 0) {
    if (spin-- == 0u) { return 0u; }        /* 校准从未完成 */
}
if (LL_ADC_GetCalibrationStatus(ADC1) == LL_ADC_CAL_STATUS_FAIL) {
    LL_ADC_ClearCalibrationStatus(ADC1);
    return 0u;                              /* 硬件拒绝本次校准 */
}
```

新增常量：`ADC_CAL_ATTEMPTS`、`ADC_CAL_SPIN_LIMIT`（`adc.c` 的 Private define 区）。

### 3.3 fail-safe 语义

`s_calib_failed`（`adc.c:80`）为真时，所有**依赖该校准**的读数报告"无数据"，而不是可疑的合法值：

| 接口 | 未校准时返回 | 行 |
|---|---|---|
| `Studio_ADC1_ReadHotMilliCelsius()` | `INT32_MIN` | `adc.c:414` |
| `Studio_ADC1_ReadColdMilliCelsius()` | `INT32_MIN`（仅 ADC 路径；SDM845 源不依赖 ADC，照常返回） | `adc.c:434` |
| `Studio_ADC1_ReadCurrentMilliamp()` | 返回 0（无效样本），`*out_ma = 0` | `adc.c:337` |

`INT32_MIN` 会被 `tec_control` 的 `is_temp_valid()` 窗口拒绝，从而进入**既有的 SENSOR 失效路径**（关 TEC、DAC 停靠、风扇全速、置 `TEC_ALARM_SENSOR`），主机可经 I2C 状态块看到。电流路径返回"无效样本"而非 0 mA，避免与"TEC 关闭时的真实 0 mA"混淆、让过流保护盲跑。

### 3.4 测试覆盖与缺口

- **已覆盖**：电流路径被 `test/tec_fault_harness.c` 真实编译，新增断言校验"未校准 → 返回无效样本 → 环路进入 SENSOR 失效路径"。
- **未覆盖**：两个温度 getter 被 `test/run_tec_fault_tests.py` 的桩替换（`return hot;` / `return cold;`），其校准门**无法从该框架执行**，目前只有源码级保证。
- 因 `s_calib_failed` 定义在被截取的代码片段之外，测试桩中补充了同名定义（`test/run_tec_fault_tests.py`）。

---

## 4. M2 —— 热预检失败后风扇无所有者

**文件**：`Core/SRC/sdm845_boot.c:543`（`st_StrapFail_Cb` 的 `QSM_SIGNAL_ENTRY`）

```c
+ Fan_SetEnabled( 1u );
+ Fan_SetDutyPercent( 100u );
```

**原因**：`STRAP_FAIL` 是终态，此后没有任何代码再驱动风扇。而 `THERMAL_PRECHECK` **仅在 `precheckCooling != 0` 时才命令满速**（`sdm845_boot.c:235`）：若预检 fail-open（300 s 超时，`sdm845_boot.c:225`）或环境读数无效而从未进入降温分支，最后一次风扇命令就是 `Fan_SetDutyPercent(0)`（`sdm845_boot.c:230`）。此时进入 `STRAP_FAIL` 会让散热**永久停到断电**，而机箱仍可能持续升温。

**两个调用都必要**：`Fan_SetEnabled()` 才是把 `FAN_CH_SW` 切给 PY32 的动作；若 SoC 仍持有 PWM 通道，单独写 duty 是空操作。`Sdm845_Boot_IsThermalReady()` 保持为 0，故 `tec_control` 的 `THERMAL_WAIT` 分支仍不介入，不会互相打架。

**影响**：预检失败从"静默失去散热"变为"持续满速散热直至人工干预"。

---

## 5. L1 / L3 / M4

### 5.1 L1 —— SVC / PendSV 死循环

**文件**：`Core/SRC/py32f030_it.c:121`、`py32f030_it.c` 的 `PendSV_Handler`

**处置**：保留 `while(1)`，补充设计意图说明。内核为 QuarkTS 协作式（`qOS_Run` + 1 kHz SysTick），不执行 SVC、不挂起 PendSV；该处理函数是**有意的停机陷阱**，而非静默返回——将来若启用抢占式内核或误执行 `svc`，会停在此处而非返回未定义栈帧。

### 5.2 L3 —— 文档与实现不符

**① `WORKLOG_CURRENT.md:532` 起的 TEC NVRAM 参数表**，按 `Core/SRC/nv_params.c` 实际默认值修正：

| 参数 | 原文档 | 实际值 |
|---|---|---|
| `PID_KP/KI/KD` | 2.0 / 0.1 / 0.5 | **0.1 / 0.1 / 0.1**（并标注 500 ms 下 `ki_eff=0.05`/周期、`kd_eff=0.2`） |
| `DAC_MIN_CTRL` | 0 mV 默认占位 | **760 mV** |
| `DAC_MAX_CTRL` | 1200 mV 默认占位 | **1600 mV** |
| `DAC_STARTUP_PCT` | 60% | **10%** |
| `CONTROL_PERIOD` | 1000 ms | **500 ms**（并注明下限 10 ms 的成因） |

**② `tres_sequencer_state_diagram.md:38` 起**新增状态名对应表（图用 `STATE_POWER_ON`，代码用 `stPwrUp` / `PWR_UP`），并**纠正引脚时序描述**：`BOOT0` 与 `VDD33USB` 在 `PULSE_RESET_*` 的 **entry** 就设好（此时 RST 仍被断言），`BOOT_WAIT_*` 仅负责释放 RST 并等待 `t_enum`。原图内 `Rail_Up` / `Assert` / `Check_SOM` 等子步骤只是示意，代码中无对应子状态，已在文档中说明。

### 5.3 M4 —— 带符号槽未标注

**问题**：协议窗口为 4 字节无符号，但毫摄氏度槽在固件内是 `int32`，以补码上线。`TEMP_VALID_MIN` 默认 `-20000`，按无符号读会显示 `4294947296`。协议与工具均未标注哪些槽带符号。

**处置**：

| 位置 | 改动 |
|---|---|
| `Core/INC/nv_params.h`（枚举前注释） | 新增 SIGN CONVENTION 段，点名带符号槽并给出误解示例 |
| `test/tres_i2c_probe.py:135` | 新增 `NV_SIGNED_SLOTS` = {`TARGET_BASE`, `TEMP_VALID_MIN`, `TEMP_VALID_MAX`} |
| `test/tres_i2c_probe.py:142` | 新增 `nv_raw_to_signed()`；参数表显示按带符号解码 |
| `test/tres_i2c_probe.py` 帮助文本 | 写明这三个槽可传负值 |
| `test/tres_host.py` 的 `params` 动作 | `--dump` 同步按带符号显示 |

**验证**：`nv_raw_to_signed(0xFFFFB1E0) == -20000`、`nv_raw_to_signed(0x0000AFC8) == 45000`；三个槽名均存在于参数表（表共 36 槽）。

**说明**：写入路径本就以位运算掩码（`(value >> 24) & 0xFF` 等）处理，负值能正确落成补码，该路径无需改动。`NV_SIGNED_SLOTS` 与 `nv_params.h` 的注释互为契约，新增带符号槽时两处都要更新。

---

## 6. 附带修复

`test/run_tec_fault_tests.py` 的 `--cc` 默认值为 `['cc']`，而未指定时该可执行文件并不存在（Debian 仅有 `gcc`），直接运行会失败。已改为 `['gcc']`。

---

## 7. 审核报告中经复核不成立的条目

以下两条**未按其建议实施**，理由如下（详细核对见对应审计记录）：

### 7.1 M3 —— STAGE5 缺少命令重发：不成立

审核报告称"STAGE5 的 poll 分支没有调用 `sdm845_stage_send_retry()`"。实际代码 `Core/SRC/sdm845_boot.c:472-478`：

```c
default:
    sdm845_stage_send_retry();
    if ( Sdm845_Detect_IsStage5Seen() != 0u ) {
        h->NextState = &stBootSuccess;
```

调用就位于报告自己引用的第 473 行，与 STAGE3（402）、STAGE4（437）完全一致。**若按该建议"补一行重发"，会向正确代码中插入重复调用。**

### 7.2 H1 的位宽论述 —— 不成立

审核报告称"若校准值有效位超过 23 位，`<<9` 会截断高位"。实际该字段位于寄存器 bits[20:9]，为 **12 位**（厂商代码按 `& 0x1FFE00` 处理）。12 位最大值 `0xFFF << 9 = 0x1FFE00`，远在 `int32` 范围内，**`<<9`/`>>9` 不丢任何位，是纯粹的噪声**。真实缺陷是校准失败被静默忽略（§3），而非位移。

> 另需说明：该段代码在本仓库语境下属项目代码（工程为纯 LL 驱动，不编译 HAL 源），但其序列与厂商 `HAL_ADC_Calibration_Start` 逐字一致。讨论时应按本仓库标准评价，避免以"HAL 用法"归纳。

---

## 8. 改动文件清单（未提交）

| 文件 | 增/删 | 说明 |
|---|---|---|
| `Core/SRC/adc.c` | +95 / -41 | H1 |
| `Core/SRC/sdm845_boot.c` | +11 / -0 | M2 |
| `Core/SRC/tec_control.c` | +15 / -3 | L2 |
| `Core/SRC/py32f030_it.c` | +10 / -0 | L1 |
| `Core/INC/nv_params.h` | +10 / -0 | M4 |
| `test/tres_i2c_probe.py` | +29 / -0 | M4 |
| `test/tres_host.py` | +6 / -2 | M4 |
| `test/tec_fault_harness.c` | +19 / -1 | H1 测试覆盖 |
| `test/run_tec_fault_tests.py` | +5 / -1 | H1 桩 + `--cc` 默认值 |
| `WORKLOG_CURRENT.md` | +5 / -5 | L3 |
| `tres_sequencer_state_diagram.md` | +17 / -0 | L3 |

合计 11 个文件，+222 / -53。

---

## 9. 验证边界

| 项目 | 状态 |
|---|---|
| EIDE 全量构建（工程自带 `unify_builder` + `builder.params`） | ✅ 通过，无错误/警告 |
| 上述 6 套测试（WSL Debian，gcc 14.2.0） | ✅ 全部通过 |
| H1 电流路径断言（真实编译 `adc.c` 片段） | ✅ 已执行 |
| **H1 温度校准门断言** | ❌ **未执行** —— 两个温度 getter 被测试桩替换，无法从该框架触达 |
| 上板验证 | ❌ **未做** —— 无硬件操作 |

**未做台架确认的风险点**：

1. ADC 校准失败路径（`CALFAIL` / 超时）在真机上的实际触发条件与表现未验证；
2. `STRAP_FAIL` 强制满速风扇的时序与噪声表现未验证；
3. 控制周期 10 ms 下限是量级估计，非实测最坏执行时间；真机若要跑更短周期，应先测量 `TecControl_Cb()` 的最坏执行时间。
