# 验证脚本

本目录是**逆向分析与验证**用的脚本，**不属于交付程序**。主程序见仓库根 [README.md](../README.md)。

这些脚本在开发期承担两件事：先把 HWMI 协议摸清楚，再逐个把主程序依赖的系统调用单独跑通，
确认可行之后才写进 C 代码。主程序里每一处看起来"奇怪"的写法，基本都能在这里找到原因。

## 脚本一览

| 脚本 | 类型 | 用途 |
|---|---|---|
| `hwmi.py` | Python | HWMI 命令集完整实现：读写充电阈值、智能充电模式、恢复充电标志 |
| `hwcharge.py` | Python | 早期只做充电阈值的精简版命令行工具 |
| `battery_probe.c` | C（控制台） | 单独验证「电池容量 / 健康度 / 状态」这条 IOCTL 路径 |
| `wmi_probe.c` | C（控制台） | 逐步打印 WMI 调用的 HRESULT，定位 HWMI 调用在哪一步失败 |

---

## Python 脚本

**环境约定：统一使用 `D:/v.python` 的解释器，不要用 PATH 上的全局 `python`。**
两个脚本都依赖 pywin32，需先装进该环境：

```
uv pip install --python "D:/v.python/Scripts/python.exe" pywin32
```

> 读操作普通权限即可；**写操作需要管理员权限，且会真实改变本机充电行为**，
> 不确定时先只用 `--raw` 读一把看看。

### hwmi.py

覆盖全部已知命令字，并区分两条返回布局不同的读取路径（详见根 README 的「原理」）。

```
"D:/v.python/Scripts/python.exe" tests/hwmi.py                    # 总览：阈值/模式/容量/健康度
"D:/v.python/Scripts/python.exe" tests/hwmi.py 80                 # 只改截止
"D:/v.python/Scripts/python.exe" tests/hwmi.py 70 80              # 起始 + 截止
"D:/v.python/Scripts/python.exe" tests/hwmi.py off                # 关闭保护 0/100
"D:/v.python/Scripts/python.exe" tests/hwmi.py --mode 2           # 按预设表设智能充电模式
"D:/v.python/Scripts/python.exe" tests/hwmi.py --resume 1         # 设置恢复充电标志
"D:/v.python/Scripts/python.exe" tests/hwmi.py --raw 0x1603       # 下发原始命令看 256 字节返回
"D:/v.python/Scripts/python.exe" tests/hwmi.py --json             # JSON 输出
```

比 `hwcharge.py` 多出：智能充电模式（`0x1503`/`0x1603`）、模式预设表、
恢复充电标志（`0x1203`/`0x1303`）、两条读路径的区分实现。

### 写阈值走 `0x1503`，不要用 `0x1003`

两个脚本的写阈值接口（`hwmi.py` 的 `set_threshold()`、`hwcharge.py` 的
`set_thresholds()`）都走 `0x1503`（SetSmartChargeMode，模式 1 + 自定义起止值）。

固件开启「智能充电模式」后，`0x1003` 的写入会被固件**丢弃**，并在 0.2~1.3 秒内
把阈值回滚到当前模式的预设值（典型表现：阈值被改回 0/100，即「充到 100%」）。
这**不是**应用抢占——停掉华为 PCManager 服务、杀掉 `HiviewService` 后回滚照旧发生，
且回滚时刻前后没有任何 WMI 写入事件。完整证据链见
[docs/充电阈值被改回100%_排查.md](../docs/充电阈值被改回100%_排查.md)。

> `--raw` 仍可下发任意原始命令字（诊断用），它绕不过固件的这条回滚规则。
> 想在脚本里复现回滚现象，可以 `--raw 0x1003` 或直接构造 8 字节载荷。

### hwcharge.py

早期版本，只做充电阈值的读写，外加电量与是否插电的展示。
保留它是因为其对外行为最直观、依赖最少，适合当最小验证样本。

```
"D:/v.python/Scripts/python.exe" tests/hwcharge.py                # 查看当前阈值
"D:/v.python/Scripts/python.exe" tests/hwcharge.py 80             # 只改结束阈值
"D:/v.python/Scripts/python.exe" tests/hwcharge.py 70 80          # 起始 70%、结束 80%
"D:/v.python/Scripts/python.exe" tests/hwcharge.py off            # 0 100
"D:/v.python/Scripts/python.exe" tests/hwcharge.py --raw 0x1103   # 诊断用
```

---

## C 探针

两个都是控制台程序，用 mingw-w64 的 gcc 编译，输出直接打到终端，无第三方依赖。
编译产物（`*.exe` / `*.o`）已被 `.gitignore` 忽略。

### battery_probe.c

```
gcc -O2 -o tests/battery_probe.exe tests/battery_probe.c -lsetupapi
./tests/battery_probe.exe
```

验证 SetupAPI + `IOCTL_BATTERY_QUERY_*` 能否读到出厂设计容量、满充容量、健康度、
循环次数、当前容量与电压。

单独写它的原因：**本机 WMI 的 `Win32_Battery` 里 `DesignCapacity` 与
`FullChargeCapacity` 都是空值**，必须先证明 IOCTL 这条路可行，才能据此写主程序。

实测输出（本机）：

```
出厂设计容量: 56316 mWh
当前满充容量: 48701 mWh
健康度      : 86.5%
循环次数    : 250
当前容量    : 43594 mWh
当前电量    : 89.5%
PowerState  : 0x00000001 (已插电)
电压        : 7770 mV
```

过程中确认了一个坑：电池状态不能走 `IOCTL_BATTERY_QUERY_INFORMATION` 的 level 12
（驱动返回 `ERROR_INVALID_FUNCTION`），必须用 `IOCTL_BATTERY_QUERY_STATUS`。

### wmi_probe.c

```
gcc -O2 -o tests/wmi_probe.exe tests/wmi_probe.c -lole32 -loleaut32 -luuid -lwbemuuid
./tests/wmi_probe.exe
```

把 HWMI 的 WMI 调用拆成每一步单独打印 `HRESULT`，用来定位失败点。
输出刻意全用 ASCII——宽字符经管道/控制台代码页转换会变成 `?`，ASCII 才可读。

它定位到的关键结论：

```
(a) GetMethod on INSTANCE    hr=0x8004101E  <<< 失败   ← 最初的写法
(b) GetObject(OemWMIMethod)  hr=0x00000000  OK
(b3) GetMethod on CLASS      hr=0x00000000  OK         ← 正确方式
(c1) SpawnInstance / Put / ExecMethod / Get(u8Output) 全部 OK
     返回区 00 55 5A ...   状态码 0x00
```

即：**方法签名必须从类对象上取**，动态 provider 不接受在 `ExecQuery` 得到的实例上
调用 `GetMethod`。

---

## 注意

- 这些脚本是开发期工具，接口可能随调试需要变动，**不保证稳定**。
- 写操作会真实改动本机充电阈值，用前请确认自己知道在做什么。
- 本目录脚本的实测环境与本机机型信息，见根 [README.md](../README.md) 的「实测环境」。
