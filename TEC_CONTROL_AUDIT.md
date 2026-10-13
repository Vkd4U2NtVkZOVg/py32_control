# tec_control 代码审计报告

| 项 | 值 |
|---|---|
| 审计对象 | `Core/SRC/tec_control.c`（946 行）、`Core/INC/tec_control.h`（114 行） |
| 基线提交 | `577c68f` "feat(tec): hold minimum cooling with dew-point hysteresis" |
| 分支 | `codex/tec-fault-interlocks` |
| 审计时间 | 2026-10-13 |
| 审计方式 | 源码级推演 + 构建产物符号取证 + 交叉编译器语法检查 |
| 审计范围 | TEC 控制任务、参数校验、露点保护、故障联锁、PID 语义、NVRAM 交互 |

---

## 0. 审计期间的代码状态变化（重要）

审计过程中 `tec_control.c` 被改动过，本报告**以最终状态为准**：

| 时点 | `tec_control.c` blob | 形态 |
|---|---|---|
| 审计开始时 | `6be5804`（≠ HEAD） | 露点保护被改回"硬切"版本，与测试/文档矛盾 |
| 审计结束时 | `78da3c0`（＝ HEAD） | "最小输出保持 + 迟滞"版本，测试恢复可编译 |

同时 `Core/INC/tec_control.h` 带有一处本地改动：`TEC_FAN_FAULT_ENABLE` 由 `1` 改为 `0`。

**因此审计早期针对"硬切版本"提出的若干问题（露点包络校验缺失、无迟滞、无积分跟踪、风扇归属未接管、测试编译失败）在当前代码中已不存在**，本文不再列为待办，仅在 §3 中作为已确认正确的项记录。

构建产物证据链（说明当前 `TEST.elf` 与源码一致）：

```
Core/SRC/tec_control.c          2026-10-13 08:58:16
Core/INC/tec_control.h          2026-10-13 09:03:49
.obj/__/Core/SRC/tec_control.o  2026-10-13 09:03:51
build/Debug/TEST.elf            2026-10-13 09:03:52
build/Debug/TEST.bin            2026-10-13 09:03:52
```

---

## 1. 结论

结构上这份代码是健康的：故障联锁的优先级顺序、参数校验、DAC 逆映射、PID 语义都经得起推演，上一轮我确认过的符号约定和 fail-safe 链在本版本中依然成立。

**当前真正的未决问题有两类：**

1. **目标温度被编译期常量钉死**（`target_mc = 5000`），使整条 NVRAM 动态目标链失效；
2. **两处 NVRAM 取值缺乏约束**，可以通过校验却把系统推向不安全方向（DAC 零制冷端点、露点最小输出反转为满制冷）。

另有 1 处台架开关留在工作区、若干文档与实现不一致。

**已修复（本次）**：运行期通过 I2C 参数窗口改值原先不生效，现已改为下一个控制周期自动重载，完整固件编译链接验证通过 —— 见 §7。

---

## 1.1 修订记录

| 日期 | 变更 |
|---|---|
| 2026-10-13 | 首版。审计对象为露点"硬切"版本（blob `6be5804`）。 |
| 2026-10-13 | 代码回到 `577c68f`（blob `78da3c0`），§0 记录该状态变化；据此重写 §2/§3。 |
| 2026-10-13 | **修复原 §2.2（运行期改 NVRAM 不生效）**：新增缓存代数计数器 + 控制周期重载、新增回归断言、完整固件编译链接验证通过。详见 §7。 |
| 2026-10-13 | **修正 §7 首版缺陷**：PID 重初始化判定原先挂在 cache 代数上，导致任何无关参数写入都会清空积分器；改为比较增益值本身（§7.3 例外 2），并补上对应回归断言。 |
| 2026-10-13 | 代码已提交为 `216f573`（含 `target_mc = 5000` 移除、`TEC_FAN_FAULT_ENABLE` 恢复 `1u`）。§7.4 体积数字改用 EIDE 构建器与 `HEAD~1` 的同源 A/B 更正（原手工链接法偏低 288 B FLASH）。 |

---

## 2. 待办问题

### 2.1 `target_mc` 被硬编码为 5000 —— 动态目标链失效

**位置**：`tec_control.c:501-509`

```c
501:    /*--- 2. target temperature --------------------------------------------*/
502:    target_mc = s_p.target_base_millic;                     // 读 NVRAM ✓
503:    if ( env_mc > s_p.env_comp_temp_millic ) {              // 读 NVRAM ✓
504:        target_mc += ( env_mc - s_p.env_comp_temp_millic ) / 2;
505:    }
506:    if ( sdm_mc > s_p.soc_tighten_temp_millic ) {           // 读 NVRAM ✓
507:        target_mc -= s_p.soc_tighten_delta_millic;
508:    }
509:    target_mc = 5000;                                      // ← 无条件覆盖
```

**性质**：参数**存了、读了、也校验了**（`tec_params_load()` 第 139-176、220-228 行），但 502-508 行算出的结果在 509 行被整体丢弃，最终送入 PID 的设定值恒为 `5000`（`TecPid_SetSetpoint(&s_pid, -(float)target_mc)`，第 621 行）。

**后果**：

- NVRAM 槽 `NV_TEC_TARGET_BASE_MILLIC`（默认 45000）、`NV_TEC_ENV_COMP_TEMP_MILLIC`（默认 50000）、`NV_TEC_SOC_TIGHTEN_TEMP_MILLIC`（默认 70000）、`NV_TEC_SOC_TIGHTEN_DELTA_MILLIC`（默认 5000）**均不参与控制**；
- 主机工具 `test/tres_i2c_probe.py` 将 `TARGET_BASE`、`ENV_COMP_TEMP`、`SOC_TIGHTEN_TEMP`、`SOC_TIGHTEN_DELTA` 作为可写参数暴露（第 90、110-112 行）。改值、读回、`--save`、`RELOAD_PARAMS` 全部显示成功，**控制目标不变**；
- `--params` 自检用的正是 `SOC_TIGHTEN_DELTA`（`tres_i2c_probe.py:292,464`），因此该自检恒绿，反而掩盖此问题；
- 头文件 `tec_control.h:13` 宣称 "Dynamic target: base + env compensation - SoC compensation (cold-side)"，与实现不符。

**备注**：`env_comp_temp_millic` 另有一处真实运行时用途 —— 环境温度无效时作为兜底环境值（第 445 行）。该兜底值随后同样被 509 行覆盖。`soc_tighten_*` 除这段死代码外无第二处引用。

**建议**（按优先级）：

1. 若"50 °C 目标"是调试需要，**不要用常量覆盖**，改把 `NV_TEC_TARGET_BASE_MILLIC` 设为 5000，并删除 502-508 段或明确注释其被弃用；
2. 若希望调试期能实时改目标，把 509 行换成每周期 RAM 读：
   ```c
   target_mc = (int32_t)NvParams_Get( NV_TEC_TARGET_BASE_MILLIC );
   ```
   `NvParams_Get()` 实测仅返回 `s_cache[id]`（`nv_params.c:322-328`），无 FLASH 开销，循环内读安全；这样 `--set TARGET_BASE 5000` 下一个控制周期即生效，无需重新编译或重启；
3. 无论选哪条，都应让主机工具不再暴露当前无效的参数，或在文档中标注它们暂不生效。

---

### 2.2 ~~运行期修改 NVRAM 参数不生效~~ —— **本次已修复，见 §7**

---

### 2.3 DAC 窗口未约束 `dac_max_ctrl_mv` 下限，可落入 FB 过驱区

**位置**：`tec_control.c:189-195`（校验）与 `tec_control.c:370-372`（映射）

**现状**：窗口校验只做四件事 —— 两端 ≥ 0、两端 ≤ `MCP4725_VDD_MV`(3300)、`min < max`。**没有校验 `dac_max_ctrl_mv` 高于 FB 基准点**。

**风险**：`duty = 0` 对应的端点正是 `dac_max_ctrl_mv`，而它同时是所有故障停机的归宿（热端危险、过流、传感器故障、露点硬切等路径都调用 `tec_dac_set(0u)`）。代码自身注释（第 337-340 行）已写明：

> `dac_min_ctrl_mv` is the mV at which FB sits at VFBTH (0.76 V) … **Never command below it** - with FB below the reference the D-CAP2 converter over-drives toward max duty (uncontrolled TEC voltage / UVP hiccup).

但代码未实现"不低于 VFBTH"这一约束。理论上 `dac_max_ctrl_mv = 700`（连同 `dac_min_ctrl_mv = 700` 以下）可通过校验，从而**每次停机都把 FB 压到基准以下，把转换器推向满占空比** —— 恰是过流保护要防的场景。

**可达性**：`NV_TEC_DAC_MAX_CTRL_MV` 是主机可写槽。当前默认值 760/1600 安全。

**建议**：在 `tec_params_load()` 的窗口校验中加入下限，例如强制 `dac_max_ctrl_mv >= VFBTH_MV + margin`（或至少要求 `dac_max_ctrl_mv - dac_min_ctrl_mv` 覆盖一个合理控制跨度），违反则回落默认值。

---

### 2.4 `dew_min_duty` 在高 `EN_OFF_PCT` 下反转为满制冷

**位置**：`tec_control.c:427`

```c
int32_t dew_min_duty = s_p.en_off_pct + TEC_EN_HYST_PCT;
```

露点保持期间该值同时作为**目标下限**（第 556 行）、**PID 输出**（第 614 行）、**斜坡下限**（第 791 行）和**启动锚点**（第 735 行），语义是"略高于 EN 死区的最小制冷量"。

但 `tec_params_load()` 允许 `en_off_pct` 取到 96（第 237-240 行只约束 `0 ≤ en_off_pct ≤ 100 - 4`）。当 `en_off_pct = 96` 时 `dew_min_duty = 100` —— **露点事件会把 TEC 推到满制冷，与保护意图完全相反**。中间值（如 `en_off_pct = 50` → `dew_min_duty = 54`）同样会产生明显的越界制冷。

**建议**：为 `dew_min_duty` 增加独立上限（例如不超过某个安全的"最小制冷"百分比），或在参数校验中约束 `en_off_pct` 的上界远低于 96。

---

### 2.5 `TEC_FAN_FAULT_ENABLE = 0` 留在工作区

**位置**：`Core/INC/tec_control.h:71`（工作区改动，HEAD 为 `1u`）

**影响**：关闭会**同时编译掉**测速失速检测（第 812-852 行整段）与失速联锁（第 587-602 行），即 `TEC_ALARM_FAN_FAULT` 与 `s_fan_stalled` 相关保护全部消失，风扇只剩按热端温度开环调速。

**判断**：头文件自身注释写明 "Default 1 = enabled"，且 2.3/2.4 等参数校验中有部分逻辑是为失速保护服务的。若这是无风扇台架的临时设置，**不应留在待提交状态**；若确为正式需求，应单独提交并说明理由，不要与 `tec_control.c` 的改动混在一起。

---

### 2.6 文档与实现不一致（低危，但会误导排障）

| 文档位置 | 文档值 | 代码实际值 |
|---|---|---|
| `WORKLOG_CURRENT.md` §5.5 | `PID_KP/KI/KD = 2.0 / 0.1 / 0.5` | 全为 `0.1`（`nv_params.c`） |
| `WORKLOG_CURRENT.md` §5.5 | `DAC_MAX_CTRL = 1200 mV 默认占位` | `1600 mV` |
| `WORKLOG_CURRENT.md` §5.5 | `DAC_MIN_CTRL = 0 mV 默认占位` | `760 mV` |
| `WORKLOG_CURRENT.md` §2.3 | NVRAM "34 槽"（`0x40..0xC7`） | `NVM_PARAM_COUNT` 由 sentinel 派生；`tres_i2c_slave.h:97` 注释为 `0x40..0xCF` |

**附带影响**：实际周期为 500 ms（`NV_TEC_CONTROL_PERIOD_MS = 500`），配合 `kp = ki = kd = 0.1` 展开为

```
kp_eff = 0.1 %/°C          误差 1 °C → 0.1 % 占空比
ki_eff = 0.1 × 0.5 = 0.05 /周期
kd_eff = 0.1 / 0.5 = 0.2
i_lim  = 50 × 0.5 = ±25 /周期
d_filter_a = 0.5/(2.0+0.5) = 0.2
```

积分作用相当迟缓。文档声称的 2.0/0.1/0.5 会让调参者按错误的数量级判断，建议同步修正。

---

### 2.7 `TecPid_TrackOutput` 在保持期间每周期调用，与注释不符（低危）

**位置**：`tec_control.c:611-614`

```c
if (s_dew_hold) {
    /* Freeze accumulation while the actuator is externally constrained. */
    TecPid_TrackOutput(&s_pid, -(float)sdm_mc, (float)dew_min_duty);
    pid_out = dew_min_duty;
}
```

`TecPid_TrackOutput()` 实现为 `TecPid_Reset(pid, measure)` 再设置 `y_out = output`。每周期都对**变化的 measure** 执行 reset，因此"Freeze accumulation（冻结累积）"的说法不准确 —— 实际上是每周期重置。

**实际危害很小**：保持期间 `pid_out` 被直接赋值为 `dew_min_duty`，PID 结果被忽略；释放路径（第 616-620 行）会用真实输出重新锚定，所以不会把陈旧的内部状态带出去。属注释与实现不符 + 语义冗余。

**建议**：要么把注释改成"每周期以当前测量重新锚定、输出被忽略"，要么改为进入保持时只调用一次。

---

## 3. 已核实正确（本轮逐条推演/取证）

以下项目经本轮独立验证确认无误，不再需要关注。

### PID 语义

- **符号约定正确**。EPID 增量式控制律（`Drivers/Embedded-PID/src/pid.c`）：
  ```
  p_term = kp * (x[k-1] - x[k])      i_term = ki_eff * (SP - x[k])
  d_term = kd * (x[k-1] + p_term_unnormalized - x[k-2])      y_out += p + i + d
  ```
  代入第 621-622 行的 `SP = -target`、`x = -sdm`：
  ```
  SP - x = -target + sdm = sdm - target
  ```
  **验算**：target = 5 °C（5000）、冷端 45 °C（45000）→ 误差项 `(45000-5000)/1000 = +40 °C` → `i_term = +0.05 × 40 = +2` → `y_out` 上升 → duty 升 → 制冷加强。冷端过冷时反向。**方向正确**，无隐性正反馈。
- 输出饱和 `[out_min, out_max] = [0, 100]`、`i_term` 限幅 `±(TEC_I_SLEW_PCT_PER_S × period_s)`、D 项 LPF 启动种子为 0（避免首周期被 measure 冲爆）均正确。
- `kp/ki/kd` 必须为正的校验（第 229-233 行）与 `TecPid_Init()` 的拒绝条件一致。

### 露点保护（当前最小输出保持版本）

- **包络校验完整**：越界露点与缺失露点（`INT32_MIN`，含样本过期 25 s 后）都会落入第 550-553 行分支 → `s_dew_hold = 1` + `TEC_ALARM_SENSOR`。**湿度数据不可用时不会静默恢复无限制制冷**。
- **迟滞正确**：进入条件 `sdm_mc <= floor_mc`，释放条件 `sdm_mc >= floor_mc + 1000`（`TEC_DEW_RELEASE_MILLIC`，第 557-558 行）。在 `[floor, floor+1 °C]` 带内保持原状态，不会抖动。
- **目标下限生效**：第 556 行 `if (target_mc < floor_mc) target_mc = floor_mc;` 且位于热折返抬升**之后**，因此下限优先 —— PID 不会再要求低于露点下限的制冷，与保护层不冲突。
- **释放路径重新锚定**：第 616-620 行用 `s_tec_enabled && s_dac_percent <= 100` 判定实际输出并 `TecPid_TrackOutput()`，避免积累后突增。
- **启动锚点尊重保持**：第 735 行 `if (s_dew_hold) start_duty = dew_min_duty;`。
- **斜坡下限**：第 791 行保证保持期间输出不低于 `dew_min_duty`。

### fail-safe 链

- **NTC 开短路**：`NTC3950_AdcToResistance()` 对 `adc_code == 0` 返回 `0xFFFFFFFF`、对 `adc_code >= 满量程` 返回 `0`，两者经查表分别钳到 `-40 °C` / `+200 °C`（`ntc3950.c:257-258, 274-275`），**均落在 `temp_valid` 窗口（-20000..120000 m°C）之外** → 走传感器故障路径。
- **ADC 超时**：`adc1_convert()` 超时返回 0（`adc.c:246-252`）→ 同上落入窗口外 → 故障路径。冷端走 SDM845 源时未就绪返回 `INT32_MIN`（`adc.c:369-381`）→ 同样被窗口拒绝。
- **过流路径与 EN 死区分离**：第 659-674 行 `tec_set_enabled(0)` + `DAC 0` + `TecPid_Reset` + 风扇 100%，不从死区路径穿过。
- **报警语义**：第 435 行 `s_alarm = TEC_ALARM_NONE` 每周期清零，早退路径不会携带上一周期残位；露点保护**不再早退**，因此 `OVERTEMP`/`OVERCURRENT` 与 `DEWPOINT` 可同时上报，不存在掩盖。
- **停机端点方向正确**：`tec_dac_set(0u)` 在逆映射下等于"零制冷"，注释与实现一致。

### 低层与参数校验

- `tec_dac_set_forced()` 的 `span` 与 `mv` 计算不溢出，`duty = 0/100` 两个端点正确；`s_dac_percent` 仅在写成功后推进，失败不伪装已执行。
- `tec_params_load()` 的包络校验先于关系校验执行，顺序正确（避免越界值干扰后续比较）；`dac_min < dac_max`、`fan_base < fan_full`、`over < danger`、`valid_max > danger`、`ramp_step > 0`、`pid > 0` 等不变量均有回落。
- 首次使能"强制写锚点 → 再拉 `TEC_VEN`"的顺序正确，锚点写失败则不使能并置 `DAC_FAULT`。
- EN 反抖死区（`en_off_pct` = 8，`+TEC_EN_HYST_PCT` = 12）状态保持逻辑正确。
- 报警位图与头文件定义逐位一致（`NONE/OVERTEMP/HOT_WARNING/OVERCURRENT/FAN_FAULT/THERMAL_WAIT/DEWPOINT/DAC_FAULT/SENSOR`）。

### 回归测试状态

- **`test/run_tec_fault_tests.py` 对当前树三档全部可编译**（`TEC_FAN_FAULT_ENABLE` = default / 0 / 1）。审计早期"测试编不过"的问题已随 `tec_control.c` 回到 HEAD 而消失。
- `Core/INC/tec_control.h` 中 `TEC_FAN_FAULT_ENABLE` 的取值校验（仅接受 0/1，否则 `#error`）保持有效。

---

## 4. 需要说明的既有事实（非本次改动引入）

以下不属缺陷，但会影响实机排查，一并记录：

1. **`TecControl_ClearAlarm()` 在固件中无调用者**。全仓搜索仅有定义（`tec_control.c:938`）、声明（`tec_control.h:102`）与测试调用（`test/tec_fault_harness.c:55`）。`tres_cmd.c` 无清报警命令。正因如此，"主机清报警不能解除失速联锁"的威胁模型天然成立；但头文件承诺的"Clear alarm flags"接口实际未接线，露点持续报警也没有主机侧消音手段。
2. **`s_p` 快照机制** 是 2.2 的根因，但也是有意设计（避免控制循环每周期读 FLASH 支持的存储），本身正确。
3. **`adc1_convert()` 含忙等 EOC 轮询**（超时上限 1,000,000 次迭代，另有 `LL_mDelay(1)`）。单次最坏可达毫秒级，而 TEC 回调每周期最多调用 3 次 ADC 读。这违反工程"周期任务不得忙等阻塞"的原则，会推迟低优先级任务与调度器（`adc.c:232-252`）。属既有设计，若实机出现异常可优先查此处。
4. **SoftI2C 事务期间关全局中断**（`sdm845_boot.c` 相关注释已记录），DAC 写入与 NVRAM 保存均属长临界区。当前斜坡阶段每周期都会写 DAC，实机需关注对从机 ISR/测速 EXTI 的推迟。

---

## 5. 验证边界（明确未做的事）

本报告的结论强度分级如下，请勿把未执行项当作已通过：

| 验证手段 | 状态 |
|---|---|
| 源码级逐行推演 | ✅ 已完成（本报告主要依据） |
| 构建产物符号取证（确认 ELF 与源码一致） | ✅ 已完成（`nm` / 时间戳链） |
| `arm-none-eabi-gcc -fsyntax-only` 语法检查（测试三档） | ✅ 已执行，无错误（含修复后复检） |
| EPID 符号约定验算 | ✅ 手算完成（见 §3），**未运行** |
| `test_probe.py`（8 项）、`nv_dualslot_sim.py`（232+65+32 项） | ⚠️ 审计早期执行过并全部通过，**与本文件无耦合，本轮未重跑** |
| 完整固件编译 + 链接 | ✅ 已用 **EIDE 自带构建器**（`unify_builder.exe` + 工程 `builder.params`）实测通过；另用官方 `TEST.lnp` 独立复现，符号表逐条一致（见 §7.4） |
| `run_tec_fault_tests.py` 的**运行时断言** | ❌ **未执行** —— 工程内无主机 C 编译器（`gcc`/`clang`/`zig`/`cc`/`tcc` 均不存在，仅有 MSVC `cl.exe`，与测试套件的 gcc C99 写法不兼容），且交叉编译产物是 ARM ELF，无法在 Windows 主机运行 |
| 上板验证 | ❌ 未做（无硬件操作） |

**未覆盖的风险**：§2.1、2.3、2.4 均为静态推演结论；`dew_min_duty` 反转（2.4）与 DAC 窗口（2.3）都依赖 NVRAM 被写入特定值，**未在实机或仿真中复现**。若要确证 2.4，可在主机端用 `--set EN_OFF_PCT 96` + `--save` + 重启后观察 `TECDUTY`（寄存器 `0x35`）在露点事件中的取值。

**§7 修复的验证强度**：编译/链接已实测，新增回归断言已写好但**未执行**（原因同上）——详见 §7.4。

---

## 6. 修复优先级汇总

| # | 问题 | 位置 | 严重度 | 建议动作 |
|---|---|---|---|---|
| 1 | `target_mc = 5000` 覆盖动态目标 | `tec_control.c:509` | 高 | 改为 NVRAM 值或明确注释弃用；同步主机工具 |
| ~~2~~ | ~~运行期改 NVRAM 参数不生效~~ | — | — | ✅ **已修复，见 §7** |
| 3 | DAC 窗口未约束 `dac_max_ctrl_mv` 下限 | `tec_control.c:189-195` | 中 | 增加 ≥ VFBTH 的下限校验 |
| 4 | `dew_min_duty` 高 `EN_OFF_PCT` 下反转为满制冷 | `tec_control.c:427` | 中 | 为 `dew_min_duty` 设独立上限 |
| 5 | `TEC_FAN_FAULT_ENABLE = 0` 留在工作区 | `tec_control.h:71` | 中 | 恢复为 1 或单独提交并说明 |
| 6 | 文档与实现不一致（PID 增益、DAC 默认、槽数） | `WORKLOG_CURRENT.md` §2.3/§5.5 | 低 | 按代码实际值修正 |
| 7 | `TecPid_TrackOutput` 每周期调用，注释不符 | `tec_control.c:611-614` | 低 | 改注释或改为进入时调用一次 |

> 行号按修复后的文件；修复只增删了 `tec_control.c` 中新增代码块所在区域上方的行，`target_mc = 5000` 仍在第 509 行（新增静态变量位于其后的 §Local state 区）。

---

## 7. 本次修复：运行期参数生效（原 §2.2）

### 7.1 根因

`tec_params_load(&s_p)` 原先只在 `TecControl_Init()` 调用一次，`s_p` 从此不再刷新；而主机的三条写入路径（`tres_i2c_slave.c` 的 `NvParams_Set`、`tres_cmd.c` 的 `NvParams_Reload` / `NvParams_RestoreDefaults`）都只更新 `s_cache`。控制环读的是 `s_p`，于是"改值 — 读回 — 保存 — 重载"全链路成功但控制行为不变。

### 7.2 方案：缓存代数计数器 + 任务侧拉取

不能简单在 `NvParams_Set()` 里直接调 `tec_params_load()`：**该函数在 I2C 从机 ISR 中执行**，而 `tec_params_load()` 会跑完整的包络/关系校验（几十次比较与 `NvParams_GetDefault` 调用），不该在中断上下文做。因此采用"写入方只记录、读取方自己拉取"：

| 改动 | 文件 | 说明 |
|---|---|---|
| 新增 `s_cache_gen` + `nv_cache_bump()` | `nv_params.c` | 唯一的缓存变更登记点；`NvParams_Set`、`NvParams_Reload` 成功后、`nv_load_defaults`（含 `NvParams_Init` 默认值回填与 `NvParams_RestoreDefaults`）全部经过它 |
| 新增 `NvParams_GetCacheGeneration()` | `nv_params.c` / `nv_params.h` | 只读访问器，任务上下文安全 |
| 新增 `tec_params_reload_if_needed()` | `tec_control.c` | 代数变化时重载 `s_p` |
| 新增 `tec_apply_pid_params()` | `tec_control.c` | PID 参数的唯一施加点（Init 与重载共用，避免两处表达式漂移） |
| `TecControl_Cb` 开头调用 | `tec_control.c` | 置于读传感器之前，保证整周期只用一套自洽参数 |

**并发安全性**：`s_cache_gen` 为 `volatile`，最坏情况是 ISR 的 `++` 与任务的读发生竞态而丢失一次递增 —— 但两者都会把计数器推进到"与任务已记录的值不同"，因此**最多把重载推迟一个控制周期，不会永久漏掉变更**。这一点已写入 `nv_params.h` 的接口注释。

### 7.3 两个刻意的例外

1. **控制周期 `NV_TEC_CONTROL_PERIOD_MS`**：它决定 `TecControl_Init()` 里创建的 QuarkTS 调度周期，并用于归一化 PID 的 `ki/kd`（`ki_eff = ki*Ts`、`kd_eff = kd/Ts`）。运行期改它无法改变已在跑的调度，若照单全收反而会**静默地把所有物理增益按错误的比例缩放**。因此重载后显式恢复启动值（`s_p.control_period_ms = s_loop_period_ms;`），并把原先两处用 `s_p.control_period_ms` 的失速计时器改为用 `s_loop_period_ms`。**改控制周期仍需重启**，已在头文件注明。
2. **PID 增益**：只在**增益值本身**变化时才重新 `TecPid_Init()` —— 该调用会清空积分器与历史。

   > **首版实现的缺陷（已修正）**：首版用 `s_pid_gen`（上次初始化时的**整个 cache 代数**）做判定，即 `if ((force == 0) && (s_pid_gen == s_nv_gen)) return;`。但任何槽位的写入都会推进代数，所以每次重载后 `s_pid_gen != s_nv_gen` 恒成立 —— **改 `EN_OFF_PCT`、`HOT_DANGER` 等任何无关参数都会清空 PID 积分器**，与注释承诺的行为正好相反。
   >
   > 现改为比较三个已施加的增益值本身（`s_pid_kp/ki/kd`，以 `NaN` 播种以保证首次判定必然不等），与代数彻底解耦：
   >
   > ```c
   > if ( ( s_p.pid_kp == s_pid_kp ) &&
   >      ( s_p.pid_ki == s_pid_ki ) &&
   >      ( s_p.pid_kd == s_pid_kd ) ) {
   >     return;                       /* 无关写入：不动正在运行的控制器 */
   > }
   > ```
   >
   > 这同时修掉了反方向的漏判：真正改增益时必须让新增益生效，否则会拿旧整定值静默运行。`force` 形参也随之删除（NaN 播种已覆盖首次初始化）。

### 7.4 验证

| 项目 | 结果 |
|---|---|
| **EIDE 全量构建**（工程自带 `unify_builder` + `build/Debug/builder.params`，54 个 C + 1 个 ASM） | ✅ `build successfully`，exit 0，2 秒 |
| 资源占用（EIDE 报告） | RAM **4904 B / 8 KB（59.86%）**，FLASH **36912 B / 60 KB（60.08%）** |
| 相对基线 `HEAD~1`（577c68f）的净增 —— **同一工具链、同一参数、同一次 A/B** | **+24 B RAM、+288 B FLASH**（基线 4880 / 36624） |
| 产物 | `TEST.elf` 521152 B、`TEST.bin` 36912 B、`TEST.hex` 103905 B |
| 复现性 | ✅ 用 EIDE 官方 `TEST.lnp` + `compile_commands.json` 独立复现，产物大小一致、**符号表 570/570 逐条相同** |
| `run_tec_fault_tests.py --syntax-only` 三档 | ✅ 通过（过程中两次编译期失败已修正：函数定义顺序早于其依赖的静态变量；`pid_inits` 计数断言写错） |
| 新增回归断言 | ✅ 已写入 `test/tec_fault_harness.c`，覆盖四项：① 运行期改 `EN_OFF_PCT` 后一个周期内 TEC 被切除；② **无关槽位写入不得重新初始化 PID**（`pid_inits` 不变，即上述缺陷的回归护栏）；③ 增益真变化时**必须**重新初始化；④ 运行期改控制周期不得污染 `s_p.control_period_ms` |
| 运行时断言实际执行 | ❌ **未执行** —— 见 §7.5 |

> **§7.4 数字更正**：本节早先版本写的是「+24 B RAM、+576 B FLASH」，那是用**手工复刻的链接**得出的，偏低约 288 B（FLASH）。经与 EIDE 官方构建做同源 A/B 后更正为上表的 **+288 B FLASH**。手工链接法不足以作为体积依据，后续一律以 EIDE 构建报告为准。

### 7.5 未做的事

- 未做上板验证；未验证实机 I2C 写入 → 下一周期生效的端到端时序。
- `test_probe.py` / `nv_dualslot_sim.py` 未重跑（与本次改动无耦合）。
- 主机侧文档（`TEC_NVRAM_DEBUG_GUIDE.md`、`HOST_README.md`）尚未补充"参数自下一个控制周期生效、控制周期本身除外"的说明，建议随本次改动一并更新。


---

## 附：复现命令

```bash
# 当前状态确认
git status --short --branch
git hash-object Core/SRC/tec_control.c
git rev-parse HEAD:Core/SRC/tec_control.c      # 两者应相同

# 测试套件语法检查（需交叉编译器；工程内无主机 C 编译器）
GCC="/c/Program Files (x86)/GNU Arm Embedded Toolchain/10 2020-q4-major/bin/arm-none-eabi-gcc"
python test/run_tec_fault_tests.py --cc "$GCC" --syntax-only

# 可完整运行的离线测试（无需 C 编译器）
python -B test/test_probe.py
python -B test/nv_dualslot_sim.py

# 构建产物与源码一致性取证
arm-none-eabi-nm -a EIDE/build/Debug/TEST.elf | grep -i dew
```
