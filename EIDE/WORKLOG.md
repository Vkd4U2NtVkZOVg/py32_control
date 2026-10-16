
---

## 2026-10-03（第二次真机）— `0 1 2 3 9 ×2 → 11`：ResetAll 放错态，冷启动 banner 被自己擦掉

### 现象

上一轮修复烧录后：每次尝试都完整走 0(TP)→1(PON)→2(PP)→3(BC)，但 **BC 15s 等不到 `Loader Build Info`**，两轮强杀后 11(STRAP_FAIL)。重试路径不再秒过（上轮 ResetAll 生效），但换成了"第一关就过不去"。

### 根因：SoC 供电轨常开，banner 在 PP entry 之前就到

MIX3 上 SDM845 的电源轨不受 PWRKEY 控制（PWRKEY 只触发开机/关机流程），PY32 一上电 SoC 就自行开跑：

- **冷启动**：loader banner 在 THERMAL_PRECHECK/POWER_ON_WAIT 窗口内到达并锁存（stage1 匹配正常，这正是测试 1 能一路走到 STAGE4 的原因）；而我把 `ResetAll()` 放在 **PP entry**，恰恰把这个唯一一次的 banner 锁存擦掉——BC 再也等不到第二次打印。
- **重试**：强杀 15s 期间 PMIC 掉电重启，新 banner 在 **BR/PS 窗口**到达；PP entry 的清除再次把它擦掉。每轮 BC 都在等一个不会再来第二次的 banner → 全部超时。

即：清除点在时间轴上总是晚于 banner 到达点，自我拆台。

### 修复：清除点移到 `BOOT_RESET` entry（强杀开始、新 banner 未打印之前）

| 位置 | 行为 |
|---|---|
| PP entry | **不再清除**——冷启动路径 TP/PP 期间赚到的锁存合法有效，保留给 BC 用 |
| BR entry | `Sdm845_Detect_ResetAll()`——强杀刚开始时清干净，重启 SoC 的新 banner 在 BR/PS/PP 窗口重新锁存，下一轮 BC 用的是新 boot 的证据 |

S3 entry 的 `ResetStage345()` 保持不变（只清 3/4/5，不碰 stage1/2）。

### 验证

- `test/replay_boot_fsm.py` 更新复位位置后三场景复跑：A 复现故障、B 到 BOOT_SUCCESS、C 诚实重试（每轮 BC 都拿到 BR 期间的新 banner），REPLAY OK。
- 全仓重编 + 链接 0 错：RAM 4352/8 KB（53%）、FLASH 28992/60 KB（47.2%）。
- WORKLOG 12 态图 BOOT_RESET 行补 "reset detectors"。

### 遗留

- 若下一轮真机仍出现 BC 超时，则需要用主机侧在 STAGE3 前抓一段 SoC 输出确认 banner 的实际到达时刻（以及排除 DMA2_MAP 写入影响 RX 路由的可能——头文件字段定义 CH1/CH2 独立，理论无影响）。
