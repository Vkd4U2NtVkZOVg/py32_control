# TEC NVRAM / MCP4725 调试指南

本文用于验证最近几次 TEC 控制链修改：

- MCP4725 替代原占位输出，作为 TEC 电流控制 DAC。
- TEC PID、温度阈值、电流阈值、风扇策略、DAC 窗口和电流采样标定参数迁入 NVRAM。
- 上电时自动从内部 FLASH 读取参数。
- I2C 从机增加 NVRAM 参数窗口和保存/恢复命令。
- FLASH 保存改为 LL/寄存器方式，不再依赖未接入工程的 HAL flash 驱动。
- 系统时钟确认：HSE 16 MHz，PLL ×2，SYSCLK/HCLK = 32 MHz；FLASH 擦写时序按 HSI 档位选择，本工程 HSI 档位为 8 MHz。

> 建议先使用限流电源、断开或限制 TEC 功率级，确认 DAC、电源使能和保护逻辑后，再接入完整 TEC 负载。

---

## 1. 调试前的硬件确认

### 1.1 必须确认的信号

| 信号 | 固件位置 | 调试时应观察 |
|---|---|---|
| MCP4725 SDA | PB0 / Soft I2C | 空闲高电平，I2C ACK 正常 |
| MCP4725 SCL | PB1 / Soft I2C | 有时钟波形，无明显拉低 |
| MCP4725 地址 | `0x60` | 地址阶段收到 ACK |
| TEC_VEN | PA12 | 预检未通过时为关闭；允许运行后才使能 |
| TEC_VCTRL | MCP4725 输出 | DAC 命令变化时随 duty 变化 |
| TEC 热端 NTC | PA5 / ADC1_IN5 | 温度值合理、无断线极端值 |
| TEC 冷端 NTC | PA4 / ADC1_IN4 | 温度值合理 |
| TEC 电流采样 | PA6 / ADC1_IN6 | INA181A2 输出变化，电流换算合理 |
| 风扇 PWM | TIM3 CH2 | duty 与热端温度策略一致 |
| 风扇测速 | 对应 EXTI/计数输入 | 风扇工作时 RPM 非零 |
| TRES I2C 从机 | I2C1 | 主机可读状态和 NVRAM 窗口 |

### 1.2 电气安全

1. 首次调试不要直接让 TEC 进入满电流。
2. 功率级控制输入先接示波器或万用表，确认 DAC 映射后再接 TEC 功率级。
3. MCP4725 输出满量程不等于 TEC 满电流；默认控制窗口为 0~1200 mV，必须按实际功率级重新标定。
4. 不要在 TEC 正在大电流运行时反复擦写 NVRAM。FLASH 保存会擦除参数页并暂时关闭中断。
5. 首次执行 `SAVE_PARAMS` 前，先确认参数窗口中的值确实是期望值。

---

## 2. 编译和静态检查

### 2.1 编译命令

工程使用 GNU Arm Embedded Toolchain 10 2020-q4-major。可按工程实际安装路径设置 `GCC`：

```bash
cd /d/2026_project/Mix3_TRES_2/Firmware/TEST

GCC="/c/Program Files (x86)/GNU Arm Embedded Toolchain/10 2020-q4-major/bin/arm-none-eabi-gcc"

COMMON="-mcpu=cortex-m0plus -mthumb -std=gnu11 -O2 \
  -ffunction-sections -fdata-sections \
  -DPY32F030x8 -DUSE_FULL_LL_DRIVER -DHSE_VALUE=16000000U \
  -ICore/INC \
  -IDrivers/CMSIS/Include \
  -IDrivers/PY32F030_HAL_Driver/Inc \
  -IDrivers/CMSIS/Device/PY32F030/Include \
  -IDrivers/QuarkTS/include \
  -IDrivers/Embedded-PID/src"

$GCC $COMMON -Wall -Wextra -c Core/SRC/nv_params.c -o /tmp/nv_params.o
$GCC $COMMON -Wall -Wextra -c Core/SRC/tec_control.c -o /tmp/tec_control.o
$GCC $COMMON -Wall -Wextra -c Core/SRC/adc.c -o /tmp/adc.o
$GCC $COMMON -Wall -Wextra -c Core/SRC/tres_i2c_slave.c -o /tmp/tres_i2c_slave.o
$GCC $COMMON -Wall -Wextra -c Core/SRC/tres_cmd.c -o /tmp/tres_cmd.o
```

已知可以忽略的警告噪音：

- `py32f0xx_ll_rcc.h` 的 unused parameter。
- `py32f0xx_ll_adc.h` 的 unused parameter。
- CMSIS 的 `__CORTEX_SC` 预处理警告。

项目自身代码不应出现新的未使用变量、隐式声明、类型不匹配或除零相关警告。

### 2.2 检查旧宏是否残留

```bash
grep -rn \
  "TEC_PID_KP\\|TEC_TARGET_BASE_MILLIC\\|TEC_HOT_OVERTEMP\\|TEC_HOT_DANGER\\|TEC_CURRENT_FEEDFORWARD\\|TEC_CURRENT_DANGER\\|FAN_MIN_RPM\\|TEC_CONTROL_PERIOD_S\\|TEC_DAC_VREF\\|TEC_DAC_MAX_CTRL\\|TEC_FAN_TEMP\\|TEC_FAN_DUTY\\|TEC_CURRENT_SHUNT\\|TEC_CURRENT_AMP\\|TEC_CURRENT_DIVIDER\\|TEC_CURRENT_ADC_VREF" \
  Core/INC Core/SRC --include='*.h' --include='*.c'
```

预期：不应出现旧的可调宏定义。允许出现对应的 `NV_TEC_*` NVRAM 槽位名。

### 2.3 链接检查

链接必须使用工程的 `EIDE/PY32F030K28U6-E.ld`，并保留：

```text
--gc-sections
--specs=nosys.specs
--specs=nano.specs
```

预期资源占用约为当前验证值：

- RAM：约 54.6%，上限 8 KB。
- FLASH：约 49.6%，链接区为 60 KB；最后参数页从 `0x0800F000` 开始。

如果出现 `undefined reference to HAL_FLASH_*`，说明仍然链接了旧版 `nv_params.c` 或没有使用当前 LL 版本。当前实现不应依赖 `py32f0xx_hal_flash.c` 和 `py32f0xx_hal_conf.h`。

---

## 3. 上电启动调试

### 3.1 推荐初始化顺序

在 `main.c` 中确认：

1. `Studio_RCC_Init()`：确认 SYSCLK/HCLK 为 32 MHz。
2. `NvParams_Init()`：在 `TecControl_Init()` 之前执行。
3. `SoftI2C_Init()` 后执行 `MCP4725_Init()`，上电 DAC 归零。
4. 初始化 ADC、风扇、TRES I2C 从机和 QuarkTS。
5. 最后执行 `TecControl_Init()`。

### 3.2 时钟确认

在调试器中观察：

```c
SystemCoreClock == 32000000u
```

同时确认 `rcc.c`：

```c
LL_RCC_HSI_SetCalibFreq(LL_RCC_HSICALIBRATION_8MHz);
LL_Init1msTick(32000000);
LL_SetSystemCoreClock(32000000);
```

这意味着：

- HSE = 16 MHz。
- PLL = ×2。
- SYSCLK = 32 MHz。
- HSI 频率档位 = 8 MHz。
- NVRAM FLASH 擦写时序应选择 HSI 8 MHz 对应的 option-byte 行 `0x1FFF0F30` 组。

不要把 `LL_FLASH_TIMMING_SEQUENCE_CONFIG_24M()` 理解为 SYSCLK 配置；当前实现已改成运行时读取 `RCC->ICSCR.HSI_FS` 选择时序。

### 3.3 上电后的第一组观察点

复位后观察：

- MCP4725 输出应为 0 V。
- `TEC_VEN` 应保持关闭，直到 SDM845 thermal precheck 通过。
- NVRAM 参数应为有效非零默认值，而不是全 0。
- `TecPid_Init()` 使用 NVRAM 中的 PID 增益和目标温度。
- AHT20/SDM845 温度尚未有效时，应使用 NVRAM 的 `TARGET_BASE` 作为回退值。

如果 TEC 上电即有电流，优先检查：

1. `MCP4725_Init()` 是否在 `SoftI2C_Init()` 后执行。
2. `TEC_VEN` GPIO 默认电平是否为关闭。
3. `NvParams_Init()` 是否在 `TecControl_Init()` 前执行。
4. 是否有旧版本固件仍在运行。

---

## 4. NVRAM 参数窗口调试

### 4.1 参数窗口布局

I2C 从机参数窗口从 `0x40` 开始，每个槽 4 字节，大端序：

```text
槽 n 地址 = 0x40 + 4*n
槽 0      = 0x40..0x43
槽 1      = 0x44..0x47
...
槽 26     = 0xA8..0xAB
```

27 个槽位的顺序定义在：

```text
Core/INC/nv_params.h
```

### 4.2 默认值检查

默认值应为：

| 槽 | 参数 | 默认值 |
|---:|---|---:|
| 0 | PID_KP_Q16 | 131072 = 2.0 |
| 1 | PID_KI_Q16 | 6554 ≈ 0.1 |
| 2 | PID_KD_Q16 | 32768 = 0.5 |
| 3 | TARGET_BASE | 45000 m°C |
| 4 | HOT_OVERTEMP | 60000 m°C |
| 5 | HOT_DANGER | 70000 m°C |
| 6 | CURRENT_FEEDFORWARD | 2000 mA |
| 7 | CURRENT_DANGER | 3000 mA |
| 8 | FAN_MIN_RPM | 500 RPM |
| 9 | CONTROL_PERIOD | 1000 ms |
| 10 | DAC_VREF | 3300 mV |
| 11 | DAC_MAX_CTRL | 1200 mV |
| 12 | FAN_TEMP_BASE | 40000 m°C |
| 13 | FAN_TEMP_FULL | 60000 m°C |
| 14 | FAN_DUTY_BASE | 30 % |
| 15 | CURRENT_SHUNT | 20000 µOhm |
| 16 | CURRENT_AMP_GAIN | 50 V/V |
| 17 | CURRENT_DIVIDER_NUM | 1 |
| 18 | CURRENT_DIVIDER_DEN | 2 |
| 19 | CURRENT_ADC_VREF | 1800 mV |
| 20 | TEMP_VALID_MIN | -20000 m°C |
| 21 | TEMP_VALID_MAX | 120000 m°C |
| 22 | CURRENT_VALID_MAX | 5000 mA |
| 23 | FAN_FAULT_MIN_DUTY | 30 % |
| 24 | ENV_COMP_TEMP | 35000 m°C |
| 25 | SOC_TIGHTEN_TEMP | 70000 m°C |
| 26 | SOC_TIGHTEN_DELTA | 5000 m°C |

PID 增益解码：

```text
gain = raw_uint32 / 65536.0
```

### 4.3 使用现有 Python 探针

```bash
cd /d/2026_project/Mix3_TRES_2/Firmware/TEST
python3 test/tres_i2c_probe.py --bus 3 --addr 0x50
```

预期输出包括：

1. `0x10..0x14` 状态寄存器。
2. `0x30..0x35` SDM845、TEC alarm、风扇 duty、TEC DAC duty。
3. `0x40..0xAB` 全部 27 个 NVRAM 参数。

如果读到全 `0xFF`：检查 I2C 地址、上拉、电源、寄存器指针实现和固件是否包含新窗口。

如果读到全 0：检查：

- `NvParams_Init()` 是否执行。
- FLASH 参数页是否为空或 CRC/version 校验失败。
- 默认表是否被错误清零。
- 当前运行的固件是否是旧版本。

---

## 5. 修改、保存和重启验证

### 5.1 推荐测试顺序

不要一开始修改 PID。先修改一个无危险的诊断参数，例如 `CONTROL_PERIOD_MS`，确认读写和持久化链路。

流程：

1. 读出整个 `0x40..0xAB`，保存原始值。
2. 修改槽 9，例如从 `1000` 改为 `2000`。
3. 再读槽 9，确认 RAM cache 已改变。
4. 发送保存命令：

```text
寄存器 0x00 写入：A5 04 00
```

5. 复位/重新上电。
6. 再读槽 9，应仍为 `2000`。
7. 观察 TEC 控制任务周期是否变成 2 秒。

### 5.2 恢复默认值

发送：

```text
寄存器 0x00 写入：A5 05 00
```

这只恢复 RAM cache，不立即写 FLASH。要持久化恢复默认值，还要再发送：

```text
寄存器 0x00 写入：A5 04 00
```

然后重新上电，确认所有槽恢复默认值。

### 5.3 保存失败时的检查

`NvParams_Save()` 运行在命令任务中，不在 I2C ISR 中执行。若保存失败，检查：

1. FLASH 是否已解锁。
2. 参数页地址 `0x0800F000` 是否与链接脚本保留区一致。
3. `RCC->ICSCR.HSI_FS` 是否为有效档位。
4. `FLASH->SR` 是否出现 `WRPERR` 或其他错误。
5. FLASH 写保护是否打开。
6. 是否在保存期间发生掉电或复位。
7. 页擦除后是否确实写入 32 个 32-bit word。

建议用调试器在 `NvParams_Save()` 内设置断点，观察：

```text
FLASH->CR：LOCK 清除 → PER → PER 清除 → PG → PGSTRT → PG 清除 → LOCK 置位
FLASH->SR：BUSY 置位后清除
RCC->ICSCR.HSI_FS：本工程预期为 1
```

> FLASH 擦写序列尚未经过目标板实测；第一次必须先用低风险参数验证读回和断电保持。

---

## 6. MCP4725 DAC 输出验证

### 6.1 不带 TEC 功率级的 DAC 测试

建议先让控制逻辑处于 `THERMAL_WAIT`，或临时限制参数，使用万用表/示波器测 MCP4725 输出。

默认映射：

```text
dac_mV = duty * DAC_MAX_CTRL_MV / 100
DAC_MAX_CTRL_MV = 1200 mV
```

| duty | 预期 DAC 输出 |
|---:|---:|
| 0 % | 0 mV |
| 25 % | 300 mV |
| 50 % | 600 mV |
| 75 % | 900 mV |
| 100 % | 1200 mV |

检查点：

- duty 不变时，控制循环不会重复发送相同 DAC 值。
- `THERMAL_WAIT` 时 DAC 应为 0 V。
- 热端危险温度时 DAC 应为 0 V，`TEC_VEN` 关闭。
- 正常运行时 DAC 电压随 TEC duty 增加。
- MCP4725 地址为 `0x60`，每次写入应收到 ACK。

### 6.2 MCP4725 无 ACK

依次排查：

1. PB0/PB1 是否与硬件 SDA/SCL 对应。
2. SDA/SCL 是否有外部上拉。
3. 地址脚配置是否确实为 `0x60`。
4. `SoftI2C_Init()` 是否早于 `MCP4725_Init()`。
5. I2C GPIO 是否被其他初始化重新配置。
6. 是否把 `0x60` 当成了 8-bit 地址 `0xC0` 使用；驱动当前使用的是 7-bit 地址语义。

---

## 7. TEC 控制闭环验证

建议按以下顺序逐步放开：

### 阶段 A：传感器只读

不使能 TEC，只观察：

- 热端温度。
- 冷端温度。
- 环境温度。
- SDM845 温度。
- INA181A2 电流。
- 风扇 RPM。

确认温度单位是 milli-degree Celsius，电流单位是 mA。

### 阶段 B：风扇解耦

手动改变热端温度或使用热源，验证：

```text
热端 <= 40°C      → 风扇约 30%
热端 = 50°C       → 风扇约 65%
热端 >= 60°C      → 风扇 100%
```

风扇 duty 不应跟 PID 输出直接绑定，而应跟热端温度绑定。

### 阶段 C：小电流 TEC

1. 设置保守的 `DAC_MAX_CTRL_MV`。
2. 确认 thermal precheck 已通过。
3. 确认 `TEC_VEN` 才打开。
4. 观察电流从低值开始增加。
5. 比较 ADC 计算电流和外部电流表。
6. 逐步校准 `CURRENT_SHUNT`、`CURRENT_AMP_GAIN`、divider 和 ADC VREF。

### 阶段 D：PID 调试

推荐顺序：

1. 先设置 `KI=0`、`KD=0`，只调 `KP`。
2. 确认方向正确：温度高于目标时，制冷输出应该增加。
3. 再小幅增加 `KI`，观察稳态误差。
4. 最后增加 `KD`，观察过冲和噪声。
5. 每次只修改一个参数，保存后重启。

PID 方向验证特别重要。当前代码通过对 setpoint 和 measurement 同时取负，把 EPID 的 heating semantics 转成 cooling semantics。如果温度越高反而输出越小，先不要调增益，先检查方向。

---

## 8. 故障保护验证

每项测试都使用限流电源，并在测试完成后恢复默认参数。

| 场景 | 预期行为 |
|---|---|
| thermal precheck 未完成 | TEC_VEN 关闭，DAC 0 V，TEC alarm 包含 THERMAL_WAIT |
| 热端超过 HOT_OVERTEMP | HOT_WARNING，风扇进入全速策略，TEC 输出至少被拉到高保护 duty |
| 热端超过 HOT_DANGER | OVERTEMP + HOT_WARNING，TEC_VEN 关闭，DAC 0 V，风扇 100% |
| 电流超过 CURRENT_DANGER | OVERCURRENT，feedforward 增加，输出最终受 100% 限制 |
| 风扇 duty 高于 FAN_FAULT_MIN_DUTY 且 RPM 低 | FAN_FAULT |
| 温度超出 TEMP_VALID_MIN/MAX | 环境或 SDM845 温度回退到 TARGET_BASE |
| 电流超过 CURRENT_VALID_MAX | 不使用该电流值做正常 feedforward |

故障验证时同时读取：

```text
0x32 TECFLAGS
0x34 FANDUTY
0x35 TECDUTY
```

并用示波器确认：

```text
TEC_VEN
MCP4725 输出
风扇 PWM
```

---

## 9. 参数版本和旧 FLASH 数据

当前 `NVM_VERSION = 2`。启动时只有同时满足以下条件才会加载 FLASH 参数：

```text
magic 正确
version == 2
CRC32 正确
```

否则自动加载 `s_defaults[]`，但不会自动写回 FLASH。首次需要保存时再执行 `SAVE_PARAMS`。

如果以后修改以下任一项：

- 槽位顺序。
- 槽位数量。
- 参数编码方式。
- 参数单位。
- FLASH block 布局。

必须递增 `NVM_VERSION`，否则旧参数可能被错误解释。

---

## 10. 最终验收清单

### 软件

- [ ] `NvParams_Init()` 在 `TecControl_Init()` 之前执行。
- [ ] `MCP4725_Init()` 在 `SoftI2C_Init()` 之后执行。
- [ ] 旧 TEC 可调宏已删除。
- [ ] NVRAM 27 槽位默认值正确。
- [ ] Q16 PID 增益解码正确。
- [ ] `NVM_VERSION=2`。
- [ ] `NvParams_Save()` 不再依赖 HAL flash 符号。
- [ ] SYSCLK=32 MHz，HSI 档位=8 MHz。
- [ ] 全量编译链接通过。

### NVRAM

- [ ] 0x40..0xAB 可以完整读出。
- [ ] 修改一个槽后 RAM 值立即可读回。
- [ ] `A5 04 00` 保存命令执行成功。
- [ ] 断电重启后修改值仍存在。
- [ ] `A5 05 00` 恢复默认值。
- [ ] 恢复默认值后再次 SAVE，重启后仍为默认值。

### TEC

- [ ] 上电 DAC 为 0 V。
- [ ] thermal precheck 未通过时 TEC 关闭。
- [ ] TEC_VEN 只在允许运行时打开。
- [ ] DAC duty→mV 映射正确。
- [ ] 风扇按热端温度独立调节。
- [ ] 电流 ADC 换算与外部电流表一致。
- [ ] 过温、过流、风扇故障保护均可触发。
- [ ] PID 方向正确，温度升高时制冷输出不会反向。

---

## 11. 推荐的最小调试顺序

如果只做一轮快速验证，按这个顺序执行：

```text
1. 编译链接，确认无旧宏和 HAL_FLASH 未定义符号。
2. 上电，确认 MCP4725=0V、TEC_VEN=OFF。
3. 通过 I2C 读 0x40..0xAB，确认 27 个默认参数。
4. 修改 CONTROL_PERIOD，SAVE，断电重启，确认持久化。
5. 不接 TEC 功率级，验证 DAC 0/25/50/75/100% 映射。
6. 只接风扇，验证 40/50/60°C 风扇斜坡。
7. 接入低电流 TEC，验证电流 ADC 换算。
8. 最后才启用 PID 闭环并逐步调参。
```
