# Mix3_TRES_2 Firmware 当前工作日志（精简版）

## 2026-10-11：TEC 故障联锁修复（codex/tec-fault-interlocks）

基于 af0ce6833fe8224659bfad15c61e02d23ed9ffb5。用户确认 target_mc=5000 和露点保护注释用于调试，本次保持原样。分压还原公式 div_den/div_num 正确，未额外添加 ×2。

- 电流标定：div_num/div_den 的未加载哨兵改为 0，使首次采样加载 NVRAM 中 1/2 的分压配置；保留原有零值回退和整数换算。默认参数、ADC=4000 时应得到 3516mA。
- 风扇失速：持续失速建立独立联锁；在任何重新使能之前检查转速，低于阈值时保持 TEC 禁能并请求风扇全速。主机清报警不能解除联锁。转速恢复后重置 PID，再走强制 DAC 锚点启动。启动宽限在首次 TEC 接管风扇时只发放一次，避免死区或故障反复启停刷新宽限。
- 冷端测量：无效温度立即禁用 TEC、尝试停车 DAC、接管风扇并全速，报告 SENSOR；不再替换为正常目标温度。恢复有效测量时以真实温度重建 PID 历史。
- DAC 运行故障：写失败立即拉低 TEC_VEN、重置 PID、风扇全速并报告 DAC_FAULT；后续恢复必须经过已有强制锚点写入，持续总线失败不能再次开启 TEC。
- 回归测试：新增 test/run_tec_fault_tests.py 与 tec_fault_harness.c，使用实际 C 回调和电流换算函数，模拟外设及 PID 输出，覆盖持续失速、清报警不能绕过联锁、转速恢复、死区循环、冷端开短路及恢复、DAC 失败及恢复。控制回调内电流输入在测试中置零，以独立验证联锁；电流换算单独断言。
- 验证：已有 Xtensa GCC 对该 C 测试单元的语法检查通过，git diff --check 通过。当前环境无可运行的主机 C 编译器或完整 ARM 构建环境，测试断言尚未执行，未完成 ARM 链接或上板验证。具备主机 GCC 后运行 python test/run_tec_fault_tests.py --cc gcc；上板需补充风扇拔除、NTC 开短路和 I2C 写失败注入。


> 本文件只记录当前有效方案、关键硬件事实、已验证的故障修复和待完成事项。
> 完整历史与推翻过的中间方案保留在 `WORKLOG.md`。
>
> 更新时间：2026-10-11

---

## 1. 项目概览

定位：PY32F030K28U6 是 Mix3 平台的**服务/电源管理 MCU**，上承 SDM845（AGNOS Linux 主机，经 I2C 与 USART2 控制台），下辖 STM32H7（TRES 小系统）和 TEC/风扇等外设，用 QuarkTS 协作式调度器串起全部状态机与周期任务。

系统拓扑：

```mermaid
graph TB
    subgraph HOST[SDM845 / AGNOS Linux host]
        L[管理软件 / 控制台]
    end

    subgraph MCU[PY32F030K28U6 服务 MCU]
        I2[I2C1 从机 0x50]
        U2["USART2 + TX_SW (PF3)"]
        TEC[TEC 控制 PID/前馈/保护]
        sensor["Sensor 采集: AHT20 / 双NTC / INA181A2 / 风扇RPM"]
        fan_pwm[TIM3 CH2 PWM<br/>PA11 归属切换]
    end

    subgraph POWER[TRES/STM32H7 主系统电源]
        H7[H7 系统]
    end

    subgraph TECP[散热执行]
        DAC[MCP4725 DAC]
        DCDC[TPS565201 FB 注入]
        TECL[TEC 功率级 单向制冷]
        FAN[风扇]
    end

    L -- "I2C (slave 0x50)<br/>状态/温度/NVRAM/命令" --> I2
    L -- "USART2 控制台<br/>115200-8N1, DMA" --> U2
    U2 -- "启动登录链" --> L
    I2 --> TEC
    U2 --> sensor

    H7 -- "PWR_EN/RST/BOOT0/VDD33USB<br/>SOM_BOOTED" --> MCU
    MCU -- "PWR_EN/RST/BOOT0" --> H7
    H7 -- "SOM_BOOTED" --> MCU

    TEC -- "duty 0..100%" --> DAC
    DAC -- "DAC_VCTRL 电压" --> DCDC
    DCDC -- "FB 注入控制" --> TECL
    sensor -- "热端温度/电流" --> TEC
    TEC -- "风扇 duty" --> fan_pwm
    fan_pwm -- "PWM" --> FAN
```

主要职责：

- 管理 TRES/STM32H7 的启动、复位和 DFU 时序；
- 管理 SDM845/AGNOS 的启动、登录和初始化命令链（5 路 KMP 流式匹配）；
- 采集 AHT20、双 NTC（热端/冷端）、TEC 电流（INA181A2）、风扇 RPM；
- 通过 PID + 电流前馈控制 TEC，经 MCP4725 DAC 注入 TPS565201 FB；
- 风扇按热端温度独立调速，归属由 `Fan_SetSource()` 显式管理；
- 通过 I2C 从机向 SDM845 提供状态寄存器、机器状态、NVRAM 参数窗口和命令通道。

核心模块：

| 模块 | 文件 | 职责 |
|---|---|---|
| TRES 时序 FSM | `tres_seq.c` | STM32H7 上电/复位/DFU 状态机 |
| 命令策略 | `tres_cmd.c` | I2C 命令合法性矩阵 + SAVE/DEFAULT 参数 |
| I2C 从机 | `tres_i2c_slave.c` | 寄存器文件 + 状态块 + NVRAM 窗口 |
| SDM845 启动 | `sdm845_boot.c` | 热预检 + 5 阶段登录链 + KMP 匹配 |
| 控制台/检测 | `usart2_dma.c` / `sdm845_detect.c` | USART2 DMA 收发 + 启动成功检测 |
| TEC 控制 | `tec_control.c` | 冷端 PID/前馈/保护/风扇解耦 |
| ADC/NTC | `adc.c` | CHSELR 单通道读取（EOC 带超时）、温度/电流换算 |
| AHT20 | `aht20_task.c` | 非阻塞测量状态机 |
| 风扇 | `fan.c` / `fan_task.c` | PWM 输出 + RPM 采样 |
| NVRAM | `nv_params.c` | FLASH 参数存储（双槽 A/B、seq 交替写、CRC） |

工程与构建：

- 目标：PY32F030K28U6（Cortex-M0+，8 KB RAM / 64 KB FLASH，最后 4 KB 为 NVRAM 双槽参数区）；
- 时钟：16 MHz HSE → PLL ×2 → SYSCLK 32 MHz；ADC 8 MHz；FLASH 擦写时序按 HSI 档位选择；
- 工具链：GNU Arm Embedded GCC 10-2020-q4，EIDE 工程，纯 LL 驱动（不依赖 HAL）；
- 调试工具：`test/tres_i2c_probe.py`（实机 I2C 探针）、`test/replay_boot_fsm.py`（离线重放）、`test/nv_dualslot_sim.py`（NVRAM 双槽掉电模拟）。

原则：

- 周期任务和状态机均不得忙等阻塞；
- 状态机回调统一使用 `switch (h->Signal)`；
- 固定延时使用 QuarkTS timeout；
- 不在 `QSM_SIGNAL_ENTRY` 中设置 `NextState`，迁移放在 NONE/timeout 分支；
- 可调参数优先放入 NVRAM，避免重新编译固件。

---

## 2. 硬件和通信接口

### 2.1 时钟

- 外部晶振：16 MHz；
- PLL 后 SYSCLK：32 MHz；
- 工程全局定义 `HSE_VALUE=16000000U`，避免厂商 LL 默认 24 MHz 导致 USART/I2C 时序错误；
- ADC 时钟使用 PCLK/4 = 8 MHz；
- FLASH 时序根据 `RCC->ICSCR.HSI_FS` 的 HSI 档位选择，不根据 SYSCLK 选择。

### 2.2 USART2 / SDM845 控制台

- USART2：PA0/PA1，115200-8N1；
- DMA1 CH1：USART2_RX，映射 `0x08`；
- DMA1 CH2：USART2_TX，映射 `0x07`；
- PF3：`UART2_TX_SW`，当前硬件用于断开/接回 CH343_TX；
- `USART2_SetTxSource()` 是唯一线路切换 API；
- 当前 `USART2_TXSW_LINEMODE=1` 时还会重配 PA0：
  - PY32 发送：AF9 推挽、TX_RX；
  - CH343 控制台：PA0 高阻、RX-only；
- 将来换成真正 SPDT 后可编译 `USART2_TXSW_LINEMODE=0`，只切换 PF3。

### 2.3 I2C 从机

- PY32 I2C1 从机地址：7-bit `0x50`（写 `0xA0`，读 `0xA1`）；
- 寄存器窗口：

| 地址 | 名称 | 说明 |
|---|---|---|
| `0x00` | CMD | `[0xA5][cmd][param]` |
| `0x10` | STATUS | TRES 状态、host_ready、错误标志 |
| `0x11` | RETRIES | TRES 重试计数 |
| `0x12` | PINS | 控制脚快照 |
| `0x13` | LASTERR | 最近错误 |
| `0x14` | VERSION | 接口版本 |
| `0x20..0x23` | SDM845_TEMP | 4 字节大端、milli-°C |
| `0x30` | BOOTSTATE | SDM845 FSM 状态 |
| `0x31` | BOOTRETRY | SDM845 重试次数 |
| `0x32` | TECFLAGS | TEC 报警位图 |
| `0x33` | AHTSTAT | AHT20 最近状态 |
| `0x34` | FANDUTY | 当前风扇占空比 |
| `0x35` | TECDUTY | TEC 当前控制 duty |
| `0x40..0xC7` | NVRAM | 34 槽 × 4 字节大端 |
| `0xD0..0xDF` | AES challenge | `TRES_AES_ENABLE` 时启用 |
| `0xE0..0xEF` | AES response | AES-128-ECB 实时响应 |

NVRAM 写入只更新 RAM；通过 `SAVE_PARAMS` 命令才写 FLASH：

```text
写 CMD: [0xA5][0x04][0x00]  -> 保存
写 CMD: [0xA5][0x05][0x00]  -> 恢复默认到 RAM
```

---

## 3. SDM845 启动状态机

状态链：

```text
THERMAL_PRECHECK
  -> POWER_ON_WAIT
  -> PWR_PRESS
  -> BOOT_CHECK       (stage1: Loader Build Info)
  -> STAGE2_WAIT      (stage2: login)
  -> STAGE3_WAIT      (发送账号，等待 Password:)
  -> STAGE4_WAIT      (发送密码，等待 Welcome to AGNOS)
  -> STAGE5_WAIT      (等待 10 s 后发送 amixer，匹配 Front Left: Playback)
  -> BOOT_SUCCESS
```

异常状态：`BOOT_RESET`、`PMIC_SETTLE`、`STRAP_FAIL`。

### 当前有效行为

- `THERMAL_PRECHECK`：AHT20 环境温度有效且不超过 70°C 才放行；若超过 70°C，风扇全速，降到 60°C 才放行；预检 timeout 为周期性（`QSM_TSOPT_PERIODIC`，每秒持续轮询），300 s 后 fail-open；
- `BOOT_CHECK` 超时允许有限次数 PMIC 强杀重试；
- stage2~stage5 超时按当前代码/状态图进入 `STRAP_FAIL`，不再盲目复位已进入 userspace 的系统；
- STAGE3 发送 `comma\r\n`，必须等 `Password:`，不能用 `comma` 回显判定；
- STAGE4 在 `Password:` 后立即发送密码 `comma\r\n`；
- STAGE5 entry 后静默 10 s，再发送：

```text
\r\namixer set PCM 100%\r\n\r\n\r\n
```

- STAGE5 匹配 `Front Left: Playback` 后进入 `BOOT_SUCCESS`；
- `BOOT_SUCCESS`、`BOOT_RESET`、`STRAP_FAIL` 将控制台线路交还 CH343；
- 启动检测采用 5 个独立 KMP 流式匹配器。

### 最终状态图（Mermaid，与 `sdm845_boot.c` 一致）

```mermaid
stateDiagram-v2
    direction TB

    [*] --> THERMAL_PRECHECK

    state "THERMAL\nPRECHECK" as THERMAL_PRECHECK
    THERMAL_PRECHECK : entry / keys released, cooling=0
    THERMAL_PRECHECK : do / poll AHT20 every 1s
    THERMAL_PRECHECK --> THERMAL_PRECHECK : [ambient > 70°C] / fan 100%, cooling=1
    THERMAL_PRECHECK --> POWER_ON_WAIT : [ambient <= 70°C && !cooling]
    THERMAL_PRECHECK --> POWER_ON_WAIT : [cooling && ambient <= 60°C]
    THERMAL_PRECHECK --> POWER_ON_WAIT : [polls ≥ 300] / fail open

    POWER_ON_WAIT : entry / keys released, retry=0
    POWER_ON_WAIT --> BOOT_SUCCESS : [SOM_BOOTED == HIGH]
    POWER_ON_WAIT --> PWR_PRESS : [100 ms]

    PWR_PRESS : entry / PWRKEY low, RST released
    PWR_PRESS --> BOOT_CHECK : [hold 3 s]

    state "BOOT_CHECK\n(STAGE1)" as BOOT_CHECK
    BOOT_CHECK : entry / keys released
    BOOT_CHECK : do / match "Loader Build Info"
    BOOT_CHECK --> STAGE2_WAIT : [stage1 matched]
    BOOT_CHECK --> BOOT_RESET : [15 s timeout && retry < 3]
    BOOT_CHECK --> STRAP_FAIL : [15 s timeout && retry ≥ 3]

    STAGE2_WAIT : do / match "login" && idle ≥ 2 s
    STAGE2_WAIT --> STAGE3_WAIT : [stage2 matched && idle ≥ 2 s]
    STAGE2_WAIT --> STRAP_FAIL : [60 s timeout]  (不复位: 复位仅保留于 BOOT_CHECK)

    STAGE3_WAIT : entry / TX→PY32 (PF3 TX-SW), reset matchers, TX DMA "comma\r\n"
    STAGE3_WAIT : do / match "Password:"
    STAGE3_WAIT --> STAGE4_WAIT : [stage3 matched]
    STAGE3_WAIT --> STRAP_FAIL : [15 s timeout]  (不复位)

    STAGE4_WAIT : entry / TX DMA "comma\r\n" (password, prompt-gated)
    STAGE4_WAIT : do / match "Welcome to AGNOS"
    STAGE4_WAIT --> STAGE5_WAIT : [stage4 matched]
    STAGE4_WAIT --> STRAP_FAIL : [15 s timeout]  (不复位)

    STAGE5_WAIT : entry / settle 10s, TX DMA "\r\n\r\namixer set PCM 100%\r\n\r\n\r\n"
    STAGE5_WAIT : do / match "Front Left: Playback"
    STAGE5_WAIT --> BOOT_SUCCESS : [stage5 matched]
    STAGE5_WAIT --> STRAP_FAIL : [15 s timeout]  (不复位: 已到 userspace, 硬复位无益)

    BOOT_SUCCESS : entry / TX→CH343 (PF3 TX-SW), booted=1
    BOOT_SUCCESS --> [*]
    state "BOOT_RESET" as BOOT_RESET
    BOOT_RESET : entry / retry++, TX→CH343 (PF3 TX-SW), reset detectors, PWRKEY+RST low
    BOOT_RESET --> PMIC_SETTLE : [hold 15 s]
    
    state "PMIC_SETTLE" as PMIC_SETTLE
    PMIC_SETTLE : entry / keys released
    PMIC_SETTLE --> PWR_PRESS : [settle 3 s]

    STRAP_FAIL : entry / keys released, halt
```

### 关键调试修复

1. TX DMA 必须配置 DMA2_MAP=`0x07`；
2. DMA 普通模式传输结束后 EN 不会自动清零；新发送前必须软件 Disable，再重写地址和 CNDTR；
3. `TxBusy` 判断为 `EN && CNDTR != 0`；
4. 每次真正强杀开始时调用 `Sdm845_Detect_ResetAll()`；冷启动路径不能过早清除已经收到的 loader banner；
5. STAGE3 不能匹配 `comma`，因为主机名 `comma-... login:` 本身包含该字符串；
6. stage3/4/5 发送失败使用 pending + 轮询重试，不静默丢命令。

---

## 4. TRES/STM32H7 时序器

TRES 控制信号：

- `TRES_PWR_EN`：PB6；
- `TRES_RST`：PA8；
- `TRES_BOOT0`：PB5；
- `TRES_VDD33USB_CTRL`：PA15；
- `SOM_BOOTED`：PB4。

主要状态：

```text
PWR_UP
 -> BOOT_WAIT_NORMAL
 -> RUNNING
 -> PULSE_RESET_NORMAL / PULSE_RESET_DFU
 -> BOOT_WAIT_DFU
 -> DFU_ACTIVE
```

- 冷启动无条件进入 NORMAL；
- DFU 通过主机命令触发复位进入，不掉电；
- `REPORT_FAIL` 只计数，不自动迁移；
- I2C 状态寄存器 `0x10..0x14` 对外提供诊断信息。

### 最终状态图（Mermaid，与 `tres_seq.c` 一致）

```mermaid
stateDiagram-v2
    direction TB
    
    [*] --> STATE_PWR_UP : 上电启动

    STATE_PWR_UP : entry / PWR_EN=1, BOOT0=0, VDD33USB=0\nRST 保持断言 (GPIO 初始化置低)
    STATE_PWR_UP --> STATE_BOOT_WAIT_NORMAL : [Timeout t_rail 50ms]

    STATE_BOOT_WAIT_NORMAL : entry / 释放 RST, BOOT0=0 已采样\n记录当前 SOM_BOOTED
    STATE_BOOT_WAIT_NORMAL : do / 采样 SOM_BOOTED
    STATE_BOOT_WAIT_NORMAL --> STATE_RUNNING : [SOM_BOOTED 上升沿 (启动链确认)]
    STATE_BOOT_WAIT_NORMAL --> STATE_RUNNING : [SOM_BOOTED 已高 / t_appboot 宽限]

    STATE_RUNNING : entry / host_ready = SOM_BOOTED
    STATE_RUNNING : do / 采样 SOM_BOOTED 保持 host_ready
    STATE_RUNNING --> STATE_PULSE_RESET_NORMAL : [CMD_RESET]
    STATE_RUNNING --> STATE_PULSE_RESET_DFU : [CMD_ENTER_DFU]

    STATE_PULSE_RESET_NORMAL : entry / BOOT0=0, VDD33USB=0\nRST 断言
    STATE_PULSE_RESET_NORMAL --> STATE_BOOT_WAIT_NORMAL : [Timeout t_rst 20ms]

    STATE_PULSE_RESET_DFU : entry / BOOT0=1, VDD33USB=1\nRST 断言
    STATE_PULSE_RESET_DFU --> STATE_BOOT_WAIT_DFU : [Timeout t_rst 20ms]

    STATE_BOOT_WAIT_DFU : entry / 释放 RST, BOOT0=1 已采样\nH7 进入系统 DFU
    STATE_BOOT_WAIT_DFU --> STATE_DFU_ACTIVE : [Timeout t_enum 3s]

    STATE_DFU_ACTIVE : entry / 保持 VDD33USB=1, 等 dfu-util
    STATE_DFU_ACTIVE --> STATE_PULSE_RESET_NORMAL : [CMD_RESET (BOOT0=0, VDD33USB=0)]
```

---

## 5. TEC 控制最终方案

### 5.1 控制链

```text
温度/电流采集
  -> 冷端 PID（反馈 = sdm_mc，多源可选）+ COMMON-MODE 前馈 + 电流前馈
     （热端只做保护/风扇输入，不进 PID 误差；温度融合已废除）
  -> TEC duty (0..100%)
  -> MCP4725 DAC（反向映射到 FB 注入窗口）
  -> TPS565201 FB 注入控制
  -> TEC 功率级（单向制冷）
```

### 5.1.1 控制任务状态图（Mermaid，与 `tec_control.c` 一致）

```mermaid
stateDiagram-v2
    [*] --> READ_SENSORS : 周期触发 (NVRAM control_period_ms)

    READ_SENSORS : 读 hot / sdm(冷端多源) / env / cur
    READ_SENSORS --> HOT_INVALID : 热端读数超出可信窗
    READ_SENSORS --> DYNAMIC_TARGET : 冷端/环境无效回退目标值

    HOT_INVALID : 关 TEC_VEN + DAC0, 风扇 100%
    HOT_INVALID --> [*] : TEC_ALARM_SENSOR(0x80) 提前退出

    DYNAMIC_TARGET : base + env补偿 − 冷端收紧<br/>热折返: 热端>80°C 每超1°C 目标+2°C(封顶+30°C)
    DYNAMIC_TARGET --> DEWPOINT_GUARD

    DEWPOINT_GUARD : 冷端 < 露点+margin → 硬切
    DEWPOINT_GUARD --> DEWPOINT_EXIT : [低于凝露下限]
    DEWPOINT_GUARD --> DANGER_CHECK : [正常]
    DEWPOINT_EXIT --> [*] : 关 TEC + DAC0 + 风扇100% (0x20)

    DANGER_CHECK : 热端 > danger (默认 70°C)
    DANGER_CHECK --> DANGER_EXIT : [超危险]
    DANGER_CHECK --> THERMAL_GATE : [正常]
    DANGER_EXIT --> [*] : 关 TEC_VEN + DAC0, 风扇 100% (0x01)

    THERMAL_GATE --> HOLD_OFF : 热预检未通过
    THERMAL_GATE --> FIRST_ENABLE : 通过
    HOLD_OFF --> [*] : TEC 关 + DAC0, 不抢风扇 (0x10)

    FIRST_ENABLE : DAC 写启动锚点(60%) → TEC_VEN=1 → 接管风扇
    FIRST_ENABLE --> PID_CALC
    FIRST_ENABLE --> [*] : DAC 写失败 → DAC_FAULT(0x40)

    PID_CALC : 反馈 = 冷端 sdm_mc (无融合)<br/>setpoint/measure 取负 → 制冷误差
    PID_CALC --> FEEDFORWARD : COMMON-MODE FF + 电流 FF

    FEEDFORWARD --> OVERCURRENT : 电流 > danger
    FEEDFORWARD --> OUTPUT : [正常]
    OVERCURRENT : 关 TEC enable + DAC0 + 风扇100%
    OVERCURRENT --> [*] : OVERCURRENT(0x04) 提前退出

    OUTPUT : 合并限幅 + ramp 限步<br/>(折返经目标生效, duty 平滑滑落)
    OUTPUT --> SET_ACTUATORS : 正常合并输出
    OUTPUT --> [*] : DAC 写失败 → DAC_FAULT(0x40)

    SET_ACTUATORS : 风扇 = 热端斜坡, DAC 写入
    SET_ACTUATORS --> [*] : 周期结束
```

保护报警位图（`TECFLAGS` 0x32）：`0x01 OVERTEMP / 0x02 HOT_WARNING / 0x04 OVERCURRENT / 0x08 FAN_FAULT / 0x10 THERMAL_WAIT / 0x20 DEWPOINT / 0x40 DAC_FAULT / 0x80 SENSOR`。

风扇与 TEC 解耦：

- 风扇依据热端温度线性调速；
- `PA11=LOW`：风扇由 SDM845 控制；
- `PA11=HIGH`：风扇由 PY32/TIM3 CH2 控制；
- 使用 `Fan_SetSource(FAN_SOURCE_PY32/SDM845)`，不要再用 `Fan_SetEnabled()` 隐含表达归属。

### 5.2 DAC 电压窗口

这是当前最容易误读的部分：

- MCP4725 电气参考/满量程：VDD ≈ **3300 mV**（编译期宏 `MCP4725_VDD_MV`，DAC 换算按此固定值）；
- TEC 功率级有效控制窗口**不是固定 0~1200 mV**；
- `1200 mV` 是早期 TPS63802/VSET 假设留下的默认占位值，不能直接作为量产满量程；
- 当前使用 FB 注入拓扑：DAC 电压越高，FB 越高，TEC 输出电压越低，映射为反向：

```text
duty 0%   -> DAC_MAX_CTRL_MV -> VOUT 最低 / TEC 关闭端
duty 100% -> DAC_MIN_CTRL_MV -> VOUT 分压自然点 / 满制冷端
```

按 R1≈68k、R2=10k、R3≈20k、VFB≈760mV 的理论估算：

- `DAC_MIN_CTRL_MV`：约 **740 mV**，FB=0.76V、TEC 电流接近 0 的端点；
- `DAC_MAX_CTRL_MV`：约 **2360 mV**，把 VOUT 压到低端的端点；
- 上述数值只是理论起点，必须用示波器/万用表在实机标定后写入 NVRAM。

### 5.3 启动策略

- 启动不从 0V 端全范围扫描；
- `NV_TEC_DAC_STARTUP_PCT` 默认 **60%**，表示从满制冷量的 60% 附近起调；
- TPS565201 内部约 1 ms soft-start 负责 0 到锚点的建立；
- 固件每个控制周期最多改变 `RAMP_STEP_PERCENT`，默认 5%；
- 这样避免 D-CAP2 在极低占空比/极低输出电压区工作不稳定。

### 5.4 保护逻辑

- 热端传感器失效（开路 -40°C / 短路 +200°C / ADC 超时）：关 TEC、DAC 置安全端、风扇 100%，置 `SENSOR(0x80)`；冷端/环境无效则回退目标值（不失效保护）；
- 冷端低于 `露点 + DEWPOINT_MARGIN`：硬切（关 TEC + DAC0 + 风扇 100%，`DEWPOINT 0x20`）——独立于 PID 的硬保护层，与 PID 共用同一冷端变量；
- 热端超过 danger 阈值（80°C，NVRAM 可设）：关闭 `TEC_VEN`、DAC 置安全端、风扇全速（`OVERTEMP 0x01`）——最后防线，正常折返到不了；
- 热端超过 overtemp 阈值（80°C）：`HOT_WARNING(0x02)`，**热折返**——冷端目标按增益 2.0/°C 线性抬升（封顶 +30°C），PID 自动减小制冷、热端负荷随之下降，回落后自动恢复；不压 duty=0、不清积分器（避免 bang-bang 振荡）；
- 过流（> `CURRENT_DANGER`）：`OVERCURRENT(0x04)`，**关 TEC enable 线** + DAC0 + 风扇 100%，电流回落经正常启动路径恢复；
- 热预检未通过：`THERMAL_WAIT(0x10)`，TEC 保持关闭，不抢占预检阶段的风扇控制；
- DAC 写失败：`DAC_FAULT(0x40)`，下周期重试，`s_dac_percent` 保持 last-good（不假装已执行）；
- 风扇 duty 较高但 RPM 过低：`FAN_FAULT(0x08)`；
- NVRAM 参数加载时做全套关系/范围校验（DAC 窗口 min<max、风扇斜坡 base<full、警告<危险阈值顺序、PID 增益为正、ramp 步长等），违反回落出厂默认——防主机写坏 NVRAM 或 FLASH 位翻转导致失控。

### 5.5 当前 TEC NVRAM 关键参数

| 参数 | 当前默认/理论值 | 说明 |
|---|---:|---|
| `PID_KP/KI/KD` | 2.0 / 0.1 / 0.5 | Q16 存储 |
| `TARGET_BASE` | 45°C | 冷端（被控对象）基准目标 |
| `HOT_OVERTEMP` | 80°C | 热警告（折返起点） |
| `HOT_DANGER` | 90°C | 危险关断（最后防线，NVRAM 可设） |
| `HOT_FB_GAIN` | 2.0 | 折返增益：热端每超 1°C 冷端目标抬 2°C |
| `HOT_FB_MAX` | +30°C | 折返抬升上限 |
| `DAC_MIN_CTRL` | 0 mV 默认占位 | 需实机标定，理论约 740 mV |
| `DAC_MAX_CTRL` | 1200 mV 默认占位 | 需实机标定，理论约 2360 mV |
| `DAC_STARTUP_PCT` | 60% | 启动锚点 |
| `RAMP_STEP_PERCENT` | 5% | 每控制周期最大变化 |
| `DEWPOINT_MARGIN` | 2000 m°C | 凝露保护下限余量 |
| `FF_GAIN` | 1.0 %/°C | COMMON-MODE 前馈（环境-目标梯度） |
| `CONTROL_PERIOD` | 1000 ms | 控制周期 |
| `CURRENT_DANGER` | 3000 mA | 电流危险阈值 |
| `COLD_SOURCE` | 0 | 0=TEC_COLD NTC，1=SDM845 温度 |

MCP4725 的 VDD 参考**不再是参数**：固定编译期宏 `MCP4725_VDD_MV (3300U)`（`NV_TEC_DAC_VREF_MV` 槽已删除）。

---

## 6. 采集和基础模块

### AHT20

- QuarkTS 8 状态非阻塞测量流程；
- 上电等待 100 ms；
- 未校准时执行一次校准；
- 转换等待约 80 ms；
- 默认 10 s 周期；
- 提供环境温度、露点和状态 getter；
- 未就绪返回 `INT32_MIN`。

### 最终状态图（Mermaid，与 `aht20_task.c` 一致）

```mermaid
stateDiagram-v2
    direction TB
    
    [*] --> STATE_POWER_ON_WAIT : 上电启动

    STATE_POWER_ON_WAIT : entry / 启动 100ms 定时器
    STATE_POWER_ON_WAIT : do / 等待上电稳定
    STATE_POWER_ON_WAIT --> STATE_CHECK_STATUS : [Timeout 100ms]

    STATE_CHECK_STATUS : entry / 读取 1 字节状态字 (0x71)
    STATE_CHECK_STATUS --> STATE_PRE_TRIGGER : [(状态字 & 0x18) == 0x18 (已校准)]
    STATE_CHECK_STATUS --> STATE_SPECIAL_INIT : [未校准或读失败]

    STATE_SPECIAL_INIT : entry / 下发 0xBE 校准命令\n启动 30ms 定时器
    STATE_SPECIAL_INIT --> STATE_PRE_TRIGGER : [Timeout 30ms]

    STATE_PRE_TRIGGER --> STATE_TRIGGER : [Timeout 10ms] 

    STATE_TRIGGER : entry / 发送 0xAC 及其参数
    STATE_TRIGGER --> STATE_WAIT_MEASURE : [无条件 (Next Poll)]

    STATE_WAIT_MEASURE : do / 等待转换完成 80ms
    STATE_WAIT_MEASURE --> STATE_READ_DATA : [Timeout 80ms]
    
    STATE_READ_DATA : entry / 读取 7 字节 + CRC + 换算\n成功后计算露点并更新缓存
    STATE_READ_DATA --> STATE_SAMPLE_WAIT : [读取完成 (成功/失败均回)]
    
    STATE_SAMPLE_WAIT --> STATE_TRIGGER : [Timeout t_cycle 10s]
```

### ADC / NTC / TEC 电流

- ADC 不允许每次读取时 Disable/Enable；
- 读取采用 CHSELR 单通道切换流程；
- ADC 时钟 8 MHz；
- 采样时间 239.5 cycles；
- 此修复解决了后续采样固定在 2048 的问题；
- EOC 等待带超时（超时返回 0，上游按传感器故障处理，不再永久阻塞控制任务）；
- 电流链参数（分流电阻、放大倍数、分压、ADC VREF）由 NVRAM 提供，零值守卫回落默认（含 shunt=0 除零防护）。

### 风扇 RPM

- EXTI 实时累计 tach 脉冲；
- 普通周期任务每 2 s 计算 RPM；
- 缓存 getter 供控制层读取；
- 风扇控制归属通过 `Fan_SetSource()` 显式管理。

---

## 7. NVRAM / FLASH（v7 双槽）

- FLASH 顶部 4 KB 预留参数区，基址 `0x0800F000`；**双槽 A/B** 各 256B（A=`0x0800F000`，B=`0x0800F100`）；
- 槽布局 `[magic][version][seq][crc32][params[]]`（当前 148B，`NVM_PAGES` 由块大小自动派生 = 2 页）；`NVM_VERSION=7`；
- **掉电安全**：`Save()` 交替写非 active 槽 → 读回校验 → 翻转 active；`Init()` 双槽各自校验后取 `seq` 大者；任何掉电点至少留一个 CRC 有效块（跨页撕裂块 CRC 拒绝），`test/nv_dualslot_sim.py` 232 项掉电枚举断言通过；
- 首次上电/双槽全损：回默认值并立即写入 slot A；v6→v7 切换后首次上电参数回默认一次（版本不符，预期）；
- 参数枚举使用末尾 sentinel 自动计数（当前 33 槽）；新增参数 = enum sentinel 前加一项 + defaults 加一项；
- 布局变化必须递增 `NVM_VERSION`；双槽总占用有 static_assert 兜底（≤4KB，参数上限 252 个）；
- **加载时全套关系/范围校验**（`tec_params_load()`）：违反不变量回落出厂默认（见 5.4）；
- 擦写时序按 HSI 档位选择；所有 BUSY 等待带超时；失败路径 `LL_FLASH_Lock()` + 恢复 PRIMASK；
- I2C 参数窗口（0x40..0xC7，34 槽）写 RAM，显式 `SAVE_PARAMS` 才落 FLASH；
- 遗留：CRC 只覆盖 `params[]`（magic/version/seq 靠独立比较兜底，翻转走安全降级）。

---

## 8. 已知硬件/软件陷阱

1. QuarkTS `ENTRY` 回调内设置 `NextState` 不会立即迁移；
2. 状态迁移成功条件必须先于 timeout 判断；
3. **QuarkTS 一次性 timeout（无 `QSM_TSOPT_PERIODIC`）在状态自停留时（如 proceed==0 不迁移）触发后即 disarm，状态机永久卡死**——`THERMAL_PRECHECK` 的实际教训；
4. DMA 通道 EN 由软件清除，普通传输结束不会自动清零；
5. DMA CNDTR 在 EN=1 时不能重写；
6. DMA1 RX 和 DMA2 TX 映射字段独立，不能漏配 TX 的 `DMA2_MAP`；
7. `HSE_VALUE` 默认值会污染 USART/I2C 所有时序计算；
8. PY32 ADC 不支持运行时 Disable 后再 Enable 的读取模式；
9. TEC FB 注入拓扑中 DAC 极性与直觉相反：DAC 高，TEC 输出低；
10. `DAC_MAX_CTRL_MV=1200` 只是当前默认占位值，不能当作最终满量程；
11. MCP4725 fast-write 首字节低 4 位就是 12 位数据的高 4 位（D11..D8），PD 位在 bit[5:4]——不要按"命令占低 3 位"理解；
12. SoftI2C 整个事务期间关全局中断（单次 ≤64B 上限强制约束），NVRAM Save（4 页擦写）同理——长临界区会推迟调度器/从机 ISR/tach EXTI。

---

## 9. 当前必须完成的实机验证

1. 用示波器/万用表标定 `DAC_MIN_CTRL_MV` 和 `DAC_MAX_CTRL_MV`；
2. 验证 60% 启动点对应的实际 TEC 电压、电流和热响应；
3. 按热惯性调整 `RAMP_STEP_PERCENT`；
4. 标定 PID 增益、电流前馈与 COMMON-MODE 前馈（`FF_GAIN`）；
5. 标定热折返参数（`HOT_FB_GAIN`/`HOT_FB_MAX`）：验证过热时冷端目标抬升、duty 滑落、热端回落的动态过程，确认不与 danger 硬关断振荡；
6. 验证 TPS565201 在 DAC 两端点和启动锚点的 FB 电压，确保不触发 UVP hiccup；
7. 验证 `Welcome to AGNOS` 确实是登录成功后的可靠标志；
8. 验证 STAGE5 10 s 静默期后 `amixer` 回显是否能在剩余超时窗口内到达；
9. 真机验证 NVRAM 双槽擦写、掉电保持和 SAVE_PARAMS 流程（v8 首次上电参数回默认一次属预期，重写标定值即可）；
10. 确认当前硬件是否最终升级为真正 SPDT，并决定是否切换 `USART2_TXSW_LINEMODE=0`；
11. AES 软件挑战响应仅提供基础防复制能力，密钥可被 JTAG 读取；需要硬件级安全时使用 ATECC608 类安全器件。

已知遗留（安全审查确认、未修复）：

- NVRAM CRC 只覆盖 `params[]`，magic/version/seq 位翻转走安全降级但无告警（S3）；
- AHT20 初始化失败无重试上限，传感器故障时每 10 s 无限重发且无降级标志（H3）；
- 露点 Magnus 公式在 RH≈100% 时结果翻转，可能错误触发/漏触发露点保护（H5）；
- 跨 ISR/任务共享的 `s_alarm`/`s_dac_percent` 等缺 `volatile`（M0+ 单字节读写实际原子，属标准符合性问题）；
- HardFault/NMI 处理为 while(1) 挂死，无 `NVIC_SystemReset()`；全固件无看门狗。

---

## 10. 验证工具

- `test/tres_i2c_probe.py`：I2C 状态、参数读写（33 槽）、SAVE/DEFAULT、可选 AES challenge-response；
- `test/replay_boot_fsm.py`：离线重放 SDM845 启动状态机和故障路径；
- `test/nv_dualslot_sim.py`：NVRAM 双槽掉电点枚举模拟（232 项断言）；
- 当前修改应使用 EIDE 等价 GCC 参数全量编译和链接验证。

