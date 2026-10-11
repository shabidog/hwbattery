# src —— 正式代码

纯 Win32 / 纯 C，无第三方运行库依赖，单文件实现。产品说明见仓库根 [README.md](../README.md)；
开发期验证脚本见 [tests/](../tests/README.md)。

## 文件

| 文件 | 作用 |
|---|---|
| `hwbattery.c` | 全部逻辑（约 1400 行，单文件） |
| `hwbattery.rc` | PE 资源：VERSIONINFO（中英双语）+ 图标 + 引用清单 |
| `hwbattery.manifest` | 执行级别 `requireAdministrator`、DPI 感知 `permonitorv2`、Win 版本兼容 |
| `hwbattery.ico` | 9 个尺寸的绿色电池图标（PNG 压缩） |
| `build.bat` | `windres` 编译资源 + `gcc` 链接，产物统一输出到 `../release/` |

## 代码结构

`hwbattery.c` 按下列区块顺序排列，每块前有分隔注释。括号内是行号，**只作定位参考**
（会随编辑漂移）：

### 电池读取（77–201）
- `BatteryState`(77) —— 电池数据聚合结构
- `battery_read_device`(98) —— SetupAPI 枚举电池设备接口 → `CreateFile` → IOCTL
- `battery_read`(161) —— 填充状态、算健康度

### HWMI 协议（221–439）
- `hwmi_connect`(221) —— COM/WMI 初始化 + `ConnectServer(root\WMI)`；服务指针缓存进 `g_svc`
- `hwmi_resolve`(267) —— **只跑一次**：解析实例 `__PATH` 与 `OemWMIfun` 入参类定义并缓存
- `hwmi_drop_cache`(308) —— 丢弃上述缓存，仅在后续调用失败时使用
- `hwmi_call`(320) —— 下发一个命令字，回填 256 字节返回区与状态码
- `hwmi_call_retry`(395) —— 部分机型需连发两次才执行，统一封装重试
- `hwmi_read_threshold`(407) / `hwmi_set_threshold`(426) —— 读 / 写充电阈值

### 配置文件（476–561）
- `cfg_ini_path`(476) —— 取 exe 同目录下 `hwbattery.ini` 的完整路径
- `cfg_save`(496) / `cfg_load`(520) —— 写 / 读"期望阈值"；文件不存在时生成默认 65/90
- `apply_threshold`(548) —— 下发 + 落盘 + 回读刷新，**阈值下发的唯一入口**

### 开机自启（575–647）
- `run_wait`(575) —— 起进程并等它结束、取退出码
- `drop_legacy_run_value`(602) —— 清理历史遗留的 Run 键值
- `autostart_enabled`(611) / `autostart_set`(618) —— 基于**任务计划程序**，不是 Run 键

### 托盘与菜单（651–864）
- `update_tooltip`(651) —— 刷新托盘悬浮提示
- `refresh_state`(670) —— 读电池 + 读阈值 + 更新提示；定时器每次触发都走它
- `show_menu`(687) —— 拼右键菜单（起始 / 截止子菜单、自启、退出）
- `WndProc`(761) —— 托盘窗口消息；`WM_TIMER` 里顺带刷新可见的设置窗口

### 设置窗口（875–1185）
- `S`(875) —— 以 96 DPI 为基准缩放一个逻辑像素值
- `ui_font`(877) —— 按当前 DPI 建界面字体（Microsoft YaHei UI 9pt）
- `mkctl`(893) —— 建子控件并统一挂字体
- `cfg_fill_combo` / `cfg_combo_get` / `cfg_combo_set`(925–952) —— 百分比下拉框读写
- `cfg_refresh`(955) —— 把最新状态刷进窗口控件
- `cfg_create_controls`(1006) —— 按固定逻辑坐标建全部控件（坐标经 `S()` 缩放）
- `dpi_rescale_child`(1039) —— DPI 变化时按比例重排子控件
- `CfgProc`(1059) —— 窗口过程，含 `WM_DPICHANGED` 处理与「应用」按钮
- `cfg_show`(1164) —— 创建 / 显示设置窗口，用 `AdjustWindowRectEx` 保证客户区尺寸

### 命令行与入口（1223–1453）
- `cli_log`(1223) —— 追加一行到 `%TEMP%\hwbattery-autostart.log`（UTF-8）
- `cli_autostart`(1246) —— `--autostart=on|off|query`
- `cli_query`(1287) —— `--query`，走与 GUI 完全相同的数据路径
- `app_icon`(1323) —— 按目标尺寸加载内嵌图标（托盘取 `SM_CXSMICON`）
- `WinMain`(1338) —— 见下方数据流

## 启动时的数据流

```
WinMain
 ├─ SetProcessDPIAware() + 取系统 DPI
 ├─ 命令行开关？ --query / --autostart= / --show → 处理完即退出，不进托盘
 ├─ 单实例互斥体：已有实例在跑则本进程直接退出（不弹提示）
 ├─ 注册两个窗口类（托盘窗口、设置窗口）
 ├─ cfg_load()            读 ini；不存在则生成默认 65/90
 ├─ hwmi_set_threshold()  把期望阈值下发给固件（失败只记日志，不弹窗）
 ├─ refresh_state()       先读一次
 └─ SetTimer(15 秒)       之后每次触发 refresh_state()，窗口可见时一并刷新
```

## 设计要点

其余实现细节（电池读取为何走 IOCTL、HWMI 方法签名为何要从类对象取等）都写在源码注释里。
这里只记两条容易忽略、且会影响到外部行为的：

- **签名与实例路径解析一次后缓存**：每次刷新的 WMI 往返由 5 次降到 1 次；失败即清缓存防粘死
- **ini 存的是"期望值"**而非"实际生效值"：所以每次启动都会重新下发一次，也因此清单必须
  `requireAdministrator`
