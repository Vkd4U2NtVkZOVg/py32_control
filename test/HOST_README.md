# TRES 主机工具（接口 0x03）

在 Linux / SDM845 主机运行，Python 3.8+。适配固件 `71b886d` 及兼容版本；默认总线 `/dev/i2c-3`、7 位地址 `0x50`。需要该 I2C 设备的访问权限。脚本不会自动扫描总线或修改温度源设置。

## 安装与查询

GitHub 仓库中的源码位于 `test/`，可下载 `dist/tres-host-v3.zip` 直接使用。若直接在仓库的 `test/` 目录运行，下列安装命令的依赖文件改为 `requirements-host.txt`。

将压缩包解压后，在目录内执行：

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 tres_host.py --bus 3 --addr 0x50 status
python3 tres_host.py params
```

不带子命令也是只读状态查询。全局选项 `--bus`、`--addr` 放在子命令前。

## 参数管理

```sh
# 导出当前 RAM 参数；同名文件已存在时拒绝覆盖
python3 tres_host.py backup calibration.json

# 查询 params 输出中的参数名/槽号；普通参数使用原始整数
python3 tres_host.py set PID_KP 2.0
# Q16 小数/科学计数法表示实际增益，整数/十六进制表示 raw 值
python3 tres_host.py set PID_KP 0x1e

# 保存整个 RAM 参数表，等待执行确认，再从 Flash 重载、逐槽比对
python3 tres_host.py save
# 也可以在单次写入后保存整表
python3 tres_host.py set PID_KP 2.0 --save

# 恢复备份到 RAM；加 --save 才持久化整表
python3 tres_host.py restore calibration.json --save
# 放弃 RAM 修改，从 Flash 重载
python3 tres_host.py reload
# 只把 RAM 表改为默认值，不自动保存
python3 tres_host.py defaults
```

备份反映当前 RAM 表，不保证与 Flash 一致。保存会持久化整个表，包括此前未保存的修改。参数生效时间遵循固件：多数控制参数和电流校准需 MCU 重启加载，`reload` 不会重启控制器。`reset` 重启的是 TRES 下游系统，不是 MCU。运行中的整表恢复并非原子事务，应在调试维护时进行；失败会以非零状态退出。

## 温度上报

先读取 Linux 传感器类型，确认需要的 SoC 测点，不能假设 thermal_zone0 就是 CPU：

```sh
for z in /sys/class/thermal/thermal_zone*; do echo "$z: $(cat "$z/type")"; done
```

选择正确的传感器路径后执行（下面路径只是示例）：

```sh
python3 tres_host.py publish-temp --source /sys/class/thermal/thermal_zone0/temp --once
python3 tres_host.py publish-temp --source /sys/class/thermal/thermal_zone0/temp --interval 1
```

源文件必须是毫摄氏度整数，如 `45000` 表示 45°C。每轮重新读取真实测量，完整写入 4 字节并校验。源读取或总线访问失败即退出，不用缓存值继续刷新有效期；固件在最后样本 5 秒后判定过期。Ctrl+C 停止。该模式不会自动更改 `COLD_SOURCE`，只有固件选择主机温度源时才影响对应控制输入。

单次手工上报适合调试，不能代替持续测量：

```sh
python3 tres_host.py temp 45000
python3 tres_host.py temp -5500
```

## 下游控制与诊断

```sh
python3 tres_host.py reset
python3 tres_host.py dfu
python3 tres_i2c_probe.py
```

reset / dfu 受固件状态策略限制；返回执行成功不代表下游已经启动完成，随后查询 status。probe 默认只读；`--commands` 会增加失败计数，`--params` 会写 Flash，均仅用于主动测试。低层寄存器操作工具为 `tres_i2c_getset.py`，其 `test` 模式会写温度，不能当只读探针。

主机命令通道按单写入者串行使用；同一时刻不要运行多个写参数/发命令进程。VERSION 必须为 0x03；STATUS bit5=忙，LASTERR 3=Flash 失败、4=忙拒绝，0x15=完成计数，0x16=UART 缺口计数。出现错误时脚本非零退出，不自动重发可能已经执行的命令。

这些工具已做离线模拟验证，尚未在实际 I2C 硬件上运行。
