#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hwmi.py —— 华为笔记本 HWMI（ACPI-WMI）完整命令集

本文件是 PCManager 反汇编结果的落地实现，包含充电控制与读取的**全部**已知
HWMI 命令字，以及两个必须区分的读取路径。

接口
----
    root\\WMI :  OemWMIMethod.OemWMIfun(u64Input    : uint64        [in],
                                        u32Resrved  : uint32        [out],
                                        u8Output    : uint8[256]    [out])
    InstanceName = 'ACPI\\PNP0C14\\HWMI_0'
    GUID         = ABBC0F5B-8EA1-11D1-A000-C90629100000
    u8Output[0]  = 状态码（0 = 成功），其后为数据

命令字总表（格式统一为 0xNN03，NN 为功能号，0x0e~0x18 连号）
-------------------------------------------------------------
    0x0e03  SOC 电源控制条件          WmiUtil!GetSocPowerCtrlCondition
    0x1003  设置充电阈值              WmiUtil!SetBatteryChargeThreshlod（固件模式下会被回滚，勿用）
    0x1103  读取充电阈值              ← 不在任何 DLL 内（见下）
    0x1203  设置恢复充电标志          WmiUtil!SetResumeChargingFlag
    0x1303  读取恢复充电标志          WmiUtil!GetResumeChargingFlag
    0x1403  AC 供电时长               WmiUtil!GetAcDurationTime
    0x1503  设置智能充电模式          WmiUtil!SetSmartChargeMode（写阈值实际走这条）
    0x1603  读取智能充电模式/容量     WmiUtil!GetSmartChargeMode
    0x1703  Prochot 原因              WmiUtil!GetProchotReason
    0x1803  温度控制策略              WmiUtil!SetTemperControlPolicy

写阈值为什么用 0x1503 而不是 0x1003
-----------------------------------
固件开启「智能充电模式」后，0x1003 的写入会被固件丢弃，并在 0.2~1.3 秒内
把阈值回滚到当前模式的预设值（典型表现：阈值被改回 0/100，即"充到 100%"）。

这**不是**应用抢占，证据：
  1. ETW 抓 Microsoft-Windows-WMI-Activity，回滚时刻前后没有任何 WMI 写入事件
  2. 停掉华为 PCManager 服务、杀掉 HiviewService，回滚照旧发生
  3. 回滚目标严格等于当前智能充电模式的预设值

详见 docs/充电阈值被改回100%_排查.md。

关于 0x1103 的重要更正
----------------------
`WmiUtil.dll` 与 `SmartChargePlugin.dll` 中**都不存在** 0x1103：

  * WmiUtil.dll：全文件 124296 字节穷举字节对 `03 11`，零命中
  * SmartChargePlugin.dll：唯一的 `03 11` 位于 VA 0x18002e049，实为
    `ff 15 03 11 04 00`（call qword ptr [rip+0x41103]）的跳转位移；
    其余候选也全是 call rel32 位移或 add 指令的巧合字节

0x1103 来自 Linux 主线驱动 drivers/platform/x86/huawei-wmi.c 的
BATTERY_THRESH_GET。它由**固件**响应而非 DLL，所以直接下发依然可用。

两条读取路径的返回布局**完全不同，不可混用**
--------------------------------------------
  0x1103  自末尾向前找最后一个非零字节 out[i]（i >= 2）：
              start = out[i-1], end = out[i]
  0x1603  固定偏移：out[1] = 模式（须 < 7），out[3] / out[4] = 容量对

把 0x1103 的样本（如 `00 55 5A 00`）套到 0x1603 上会解析错，反之亦然。

写入链路（反汇编确认）
----------------------
SmartChargePlugin 只导入 WmiUtil 的 Get/SetSmartChargeMode 等函数，**不**导入
SetBatteryChargeThreshlod；阈值写入实际经中间层包装：

    SmartChargePlugin
      → HardwareHal!Battery::SetChargeThreshold(const StBatteryChangeThresholdLimit*)
        → WmiUtil!HwSDK::BiosWmi::SetBatteryChargeThreshlod(start, end)   [0x1003]
          → ACPI-WMI OemWMIMethod

    struct StBatteryChangeThresholdLimit {   // 2 字节打包
        BYTE start;   // 低字节
        BYTE end;     // 高字节
    };

注意：该中间层**不做任何范围校验**——0..255 任意字节都会被下发到固件，
也没有「起始不得高于当前电量」之类的保护。所有安全约束都只是约定。

依赖 pywin32；写操作需要管理员权限。
"""

import argparse
import json
import sys
import time

try:
    import win32com.client as win32
except ImportError:  # pragma: no cover
    sys.stderr.write("[!] 缺少依赖 pywin32，请先执行:  python -m pip install pywin32\n")
    sys.exit(2)

# ---------------------------------------------------------------- 常量

WMI_NAMESPACE = "root\\WMI"
WMI_CLASS = "OemWMIMethod"
WMI_METHOD = "OemWMIfun"

# 命令字（0xNN03：NN 为功能号）
CMD_SOC_POWER_CTRL_CONDITION = 0x0E03
CMD_SET_THRESHOLD = 0x1003   # \SBTT  Set Battery Threshold
CMD_GET_THRESHOLD = 0x1103   # \GBTT  Get Battery Threshold（固件提供，非 DLL）
CMD_SET_RESUME_CHARGING = 0x1203
CMD_GET_RESUME_CHARGING = 0x1303
CMD_GET_AC_DURATION = 0x1403
CMD_SET_SMART_CHARGE_MODE = 0x1503
CMD_GET_SMART_CHARGE_MODE = 0x1603
CMD_GET_PROCHOT_REASON = 0x1703
CMD_SET_TEMPER_CTRL_POLICY = 0x1803

STATUS_OK = 0
MAX_TRIES = 2       # 部分机型 BIOS 需连续调用两次才执行
SMART_MODE_MAX = 6  # 预设模式有效范围 1..6（7 是阈值表哨兵）

# 写阈值时使用的智能充电模式号。
# 固件的 1/2/3/5/6 号模式都能忠实应用「自定义」阈值对；4 号是固件固定档，
# 忽略传入值、始终按 40/70 生效，所以不能用它。
SMART_MODE_CUSTOM = 1

# SmartChargeMode 预设阈值表（WmiUtil.dll @ dword_1800137A0，实测读出）
# 布局: dword[2*mode-2] 低字节 = 起始%, dword[2*mode-1] 低字节 = 结束%
MODE_PRESETS = {
    1: (65, 70),
    2: (85, 90),
    3: (95, 100),
    4: (65, 70),
    5: (65, 70),
    6: (65, 70),
}

# 设模式时 BYTE3 的固定填充值（a2 != 0 走预设分支时）
MODE_PRESET_FILLER = 0x48


class HwmiError(RuntimeError):
    """HWMI 调用或解析失败。"""


# ---------------------------------------------------------------- 连接

def connect():
    """连接 root\\WMI，返回 OemWMIMethod 实例。"""
    try:
        locator = win32.Dispatch("WbemScripting.SWbemLocator")
        svc = locator.ConnectServer(".", WMI_NAMESPACE)
    except Exception as exc:  # noqa: BLE001
        raise HwmiError(f"无法连接 WMI 命名空间 {WMI_NAMESPACE}: {exc}") from exc

    try:
        items = svc.ExecQuery(f"SELECT * FROM {WMI_CLASS}")
    except Exception as exc:  # noqa: BLE001
        raise HwmiError(
            f"未找到 WMI 类 {WMI_CLASS}（本机 BIOS 可能不支持 HWMI OEM 接口）: {exc}"
        ) from exc

    if items.Count == 0:
        raise HwmiError(
            f"{WMI_CLASS} 无实例：该机型未暴露华为 HWMI OEM 接口，无法用此法控充"
        )
    return items.ItemIndex(0)


def hwmi_call(instance, cmd):
    """下发一个 HWMI 命令，返回 (状态码, 256 字节返回区)。

    cmd 既可以是完整 64 位命令字，也可以是 8 字节 bytes 载荷。
    """
    if isinstance(cmd, (bytes, bytearray)):
        payload = bytes(cmd)
        if len(payload) != 8:
            raise HwmiError(f"命令载荷必须是 8 字节，收到 {len(payload)} 字节")
        value = int.from_bytes(payload, "little")
    else:
        value = int(cmd) & 0xFFFFFFFFFFFFFFFF

    try:
        in_params = instance.Methods_(WMI_METHOD).InParameters.SpawnInstance_()
        in_params.u64Input = value
        out = instance.ExecMethod_(WMI_METHOD, in_params)
    except Exception as exc:  # noqa: BLE001
        raise HwmiError(f"调用 {WMI_METHOD} 失败（需管理员权限？）: {exc}") from exc

    raw = out.u8Output
    if raw is None:
        raise HwmiError("BIOS 未返回 u8Output 数据")
    data = bytes(bytearray(int(b) & 0xFF for b in raw))
    return data[0], data


def hwmi_exec(instance, cmd):
    """下发命令并在状态非 0 时重试一次（部分机型 BIOS 需要连调两次）。"""
    status, data = hwmi_call(instance, cmd)
    for _ in range(MAX_TRIES - 1):
        if status == STATUS_OK:
            break
        status, data = hwmi_call(instance, cmd)
    return status, data


# ---------------------------------------------------------------- 充电阈值 (0x1003 / 0x1103)

def read_threshold(instance):
    """用 0x1103 读取起止阈值（固件路径）。

    解析规则照搬 huawei-wmi：在整块 256 字节返回区中自末尾向前找到最后一个
    非零字节 out[i]（i >= 2），则 start = out[i-1]、end = out[i]。

    该规则只适用于 0x1103，不要拿去解析 0x1603（布局不同，见下）。
    """
    status, data = hwmi_exec(instance, CMD_GET_THRESHOLD)
    if status != STATUS_OK:
        raise HwmiError(f"0x1103 读取阈值失败，状态 0x{status:02X}")

    start = end = 0
    idx = len(data) - 1
    while idx >= 2:
        if data[idx]:
            start, end = data[idx - 1], data[idx]
            break
        idx -= 1
    return start, end


def set_threshold(instance, start, end, verify=True, mode=SMART_MODE_CUSTOM):
    """用 0x1503（SetSmartChargeMode）设置起止阈值。

    载荷（来自 WmiUtil.dll @ 0x180005430 反汇编）::

        bytes[0..1] = 0x1503 (LE)
        bytes[2]    = mode
        bytes[3]    = 0        <- 置 0 表示"用我传入的起止值"；置非 0 走固件预设表
        bytes[4]    = start
        bytes[5]    = end

    为什么不用 0x1003（CMD_SET_THRESHOLD）：
        固件开启「智能充电模式」后，0x1003 的写入会被固件丢弃，并在 0.2~1.3 秒内
        把阈值回滚到当前模式的预设值（典型表现：阈值被改回 0/100，即"充到 100%"）。
        这不是应用抢占——停掉华为 PCManager 服务、杀掉 HiviewService 后回滚照旧发生，
        且回滚时刻前后没有任何 WMI 写入事件。
        详见 docs/充电阈值被改回100%_排查.md。

    mode 取 1：固件的 1/2/3/5/6 号模式都能忠实应用自定义阈值；
    4 号是固件固定档，无论传什么都固定成 40/70。
    """
    if not (0 <= start <= 100 and 0 <= end <= 100):
        raise HwmiError(f"阈值必须是 0-100，收到 start={start} end={end}")
    if start > end:
        raise HwmiError(f"起始阈值不能大于结束阈值（{start} > {end}）")
    if not (1 <= mode <= SMART_MODE_MAX):
        raise HwmiError(f"模式号必须在 1..{SMART_MODE_MAX}，收到 {mode}")

    payload = (
        CMD_SET_SMART_CHARGE_MODE.to_bytes(2, "little")
        + bytes([mode & 0xFF, 0, start & 0xFF, end & 0xFF])
        + bytes(2)
    )
    status, _ = hwmi_exec(instance, payload)
    if status != STATUS_OK:
        raise HwmiError(f"设置阈值失败，BIOS 返回状态 0x{status:02X}")

    if verify:
        # 固件落盘不是瞬时的，稍等再回读
        time.sleep(0.3)
        cur_start, cur_end = read_threshold(instance)
        if (cur_start, cur_end) != (start, end):
            raise HwmiError(
                f"设置后回读不一致：期望 {start}%/{end}%，实际 {cur_start}%/{cur_end}%"
            )
        return cur_start, cur_end
    return start, end


# ------------------------------------------------------- 智能充电模式 (0x1503 / 0x1603)

def get_smart_charge_mode(instance):
    """用 0x1603 读取智能充电模式与容量对。

    返回布局（来自 WmiUtil.dll @ 0x180005600 反汇编，**固定偏移**）：

        out[1] = 模式，有效范围 1..6（>= 7 视为无效）
        out[3] = 容量[0]
        out[4] = 容量[1]

    返回 (mode, cap0, cap1)；模式号越界时整体返回 (None, None, None)。
    """
    status, data = hwmi_exec(instance, CMD_GET_SMART_CHARGE_MODE)
    if status != STATUS_OK:
        raise HwmiError(f"0x1603 读取智能充电模式失败，状态 0x{status:02X}")

    mode = data[1]
    if mode >= 7:
        return None, None, None
    return mode, data[3], data[4]


def set_smart_charge_mode(instance, mode, start=None, end=None, preset=True, a3=0):
    """用 0x1503 设置智能充电模式。

    载荷（来自 WmiUtil.dll @ 0x180005430 反汇编）：

        bytes[0..1] = 0x1503 (LE)
        bytes[2]    = mode
        bytes[3]    = preset ? 0x48 : a3
        bytes[4]    = start
        bytes[5]    = end

    对应反汇编签名 SetSmartChargeMode(mode, a2, a3, a4, a5)：
        a2 != 0（preset=True）  -> 阈值查 MODE_PRESETS 表，BYTE3 固定 0x48
        a2 == 0（preset=False） -> 用传入的 start/end，BYTE3 = a3，不查表
    """
    if not (1 <= mode <= SMART_MODE_MAX):
        raise HwmiError(f"模式号必须在 1..{SMART_MODE_MAX}，收到 {mode}")

    if preset:
        if mode not in MODE_PRESETS:
            raise HwmiError(f"模式 {mode} 没有预设阈值")
        start, end = MODE_PRESETS[mode]
        b3 = MODE_PRESET_FILLER
    else:
        if start is None or end is None:
            raise HwmiError("preset=False 时必须提供 start/end")
        if not (0 <= start <= 100 and 0 <= end <= 100):
            raise HwmiError(f"阈值必须是 0-100，收到 start={start} end={end}")
        b3 = a3

    payload = (
        CMD_SET_SMART_CHARGE_MODE.to_bytes(2, "little")
        + bytes([mode & 0xFF, b3 & 0xFF, start & 0xFF, end & 0xFF])
        + bytes(2)
    )
    status, _ = hwmi_exec(instance, payload)
    if status != STATUS_OK:
        raise HwmiError(f"设置智能充电模式失败，BIOS 返回状态 0x{status:02X}")
    return mode, start, end


# ---------------------------------------------------- 恢复充电标志 (0x1203 / 0x1303)

def get_resume_charging_flag(instance):
    """用 0x1303 读取恢复充电标志。

    布局（来自 WmiUtil.dll @ 0x180004ea0 反汇编）：out[1] == 1 -> True
    """
    status, data = hwmi_exec(instance, CMD_GET_RESUME_CHARGING)
    if status != STATUS_OK:
        raise HwmiError(f"0x1303 读取恢复充电标志失败，状态 0x{status:02X}")
    return data[1] == 1


def set_resume_charging_flag(instance, flag):
    """用 0x1203 设置恢复充电标志。

    载荷（来自 WmiUtil.dll @ 0x180005010 反汇编）：

        bytes[0..1] = 0x1203 (LE)
        bytes[2]    = flag
    """
    payload = CMD_SET_RESUME_CHARGING.to_bytes(2, "little") + bytes(
        [1 if flag else 0]
    ) + bytes(5)
    status, _ = hwmi_exec(instance, payload)
    if status != STATUS_OK:
        raise HwmiError(f"设置恢复充电标志失败，BIOS 返回状态 0x{status:02X}")
    return bool(flag)


# ---------------------------------------------------------------- 电池信息

def battery_info():
    """当前电量、是否插电、出厂容量与满充容量（root\\cimv2 Win32_Battery）。

    设计容量(DesignCapacity) 与满充容量(FullChargeCapacity) 用于估算健康度。
    注意根命名空间是 root\\cimv2，和 HWMI 的 root\\WMI 不是同一个。
    """
    info = {
        "charge_percent": None, "plugged": None,
        "design_capacity": None, "full_charge_capacity": None,
        "health_percent": None,
    }
    try:
        locator = win32.Dispatch("WbemScripting.SWbemLocator")
        svc = locator.ConnectServer(".", "root\\cimv2")
        items = svc.ExecQuery(
            "SELECT EstimatedChargeRemaining, BatteryStatus, "
            "DesignCapacity, FullChargeCapacity FROM Win32_Battery"
        )
        if items.Count:
            b = items.ItemIndex(0)
            info["charge_percent"] = int(b.EstimatedChargeRemaining)
            info["plugged"] = int(b.BatteryStatus) != 1  # 1 = 放电中
            dc = getattr(b, "DesignCapacity", None)
            fc = getattr(b, "FullChargeCapacity", None)
            if dc:
                info["design_capacity"] = int(dc)
            if fc:
                info["full_charge_capacity"] = int(fc)
            if info["design_capacity"] and info["full_charge_capacity"]:
                info["health_percent"] = round(
                    info["full_charge_capacity"] * 100.0 / info["design_capacity"], 1
                )
    except Exception:  # noqa: BLE001
        pass
    return info


def describe(start, end):
    if start == 0 and end == 100:
        return "未启用（充满 100%）"
    if start == 0:
        return f"充电上限 {end}%（低电量才充，最护电池）"
    if end == 100:
        return f"起始充电 {start}%"
    return f"电量 {start}%-{end}% 区间充电"


def parse_percent(text):
    value = str(text).strip().rstrip("%")
    try:
        return int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"不是合法的百分比: {text!r}") from None


# ---------------------------------------------------------------- 命令行

def _print_status(instance, as_json):
    start, end = read_threshold(instance)

    mode = cap0 = cap1 = None
    try:
        mode, cap0, cap1 = get_smart_charge_mode(instance)
    except HwmiError:
        pass

    try:
        resume = get_resume_charging_flag(instance)
    except HwmiError:
        resume = None

    info = battery_info()

    if as_json:
        payload = {
            "threshold_start": start,
            "threshold_end": end,
            "threshold_desc": describe(start, end),
            "smart_mode": mode,
            "capacity_pair": [cap0, cap1],
            "mode_preset": list(MODE_PRESETS.get(mode)) if mode in MODE_PRESETS else None,
            "resume_charging_flag": resume,
        }
        payload.update(info)
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0

    print(f"充电阈值    : 起始 {start}%  结束 {end}%")
    print(f"当前模式    : {describe(start, end)}")
    if mode is not None:
        preset = MODE_PRESETS.get(mode)
        extra = f"预设 {preset[0]}%-{preset[1]}%" if preset else "无预设"
        print(f"智能充电模式: {mode}（{extra}）  out[3]/out[4] = {cap0}/{cap1}")
    if resume is not None:
        print(f"恢复充电标志: {'已置位' if resume else '未置位'}")
    if info["charge_percent"] is not None:
        plug = "已插电" if info["plugged"] else "未插电"
        print(f"当前电量    : {info['charge_percent']}%（{plug}）")
    if info["design_capacity"] and info["full_charge_capacity"]:
        print(f"电池容量    : 出厂 {info['design_capacity']} mWh  "
              f"满充 {info['full_charge_capacity']} mWh  "
              f"健康度 {info['health_percent']}%")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="hwmi",
        description="华为 HWMI 完整命令集：充电阈值、智能充电模式、恢复充电标志",
        epilog=("示例: hwmi | hwmi 80 | hwmi 70 80 | hwmi off | "
                "hwmi --mode 2 | hwmi --resume 1 | hwmi --raw 0x1103"),
    )
    parser.add_argument("values", nargs="*", help="[起始] 结束 阈值百分比，或 off")
    parser.add_argument("--mode", type=int, help="设置智能充电模式（1..6，使用预设阈值）")
    parser.add_argument("--resume", type=int, choices=(0, 1), help="设置恢复充电标志")
    parser.add_argument("--raw", help="直接下发 64 位原始命令（十六进制），如 0x1103")
    parser.add_argument("--json", action="store_true", help="以 JSON 输出")
    parser.add_argument("--no-verify", action="store_true", help="设置后不回读校验")
    args = parser.parse_args(argv)

    try:
        instance = connect()

        if args.raw is not None:
            cmd = int(args.raw, 0)
            status, data = hwmi_call(instance, cmd)
            print(f"命令 0x{cmd:016X} -> 状态 0x{status:02X}")
            print("返回区:", " ".join(f"{b:02X}" for b in data[:16]), "...")
            return 0 if status == STATUS_OK else 1

        if args.resume is not None:
            value = set_resume_charging_flag(instance, bool(args.resume))
            print(f"恢复充电标志已设为 {'1' if value else '0'}")
            return 0

        if args.mode is not None:
            mode, start, end = set_smart_charge_mode(instance, args.mode)
            if not args.no_verify:
                cur_start, cur_end = read_threshold(instance)
                if (cur_start, cur_end) != (start, end):
                    sys.stderr.write(
                        f"[!] 回读不一致：模式 {mode} 预设 {start}/{end}，"
                        f"实际 {cur_start}/{cur_end}\n"
                    )
            print(f"已设置智能充电模式 {mode}：预设阈值 {start}%-{end}%")
            return 0

        values = [v.lower() for v in args.values]
        if not values:
            return _print_status(instance, args.json)

        if values[0] == "off":
            start, end = 0, 100
        elif len(values) == 1:
            start, end = read_threshold(instance)[0], parse_percent(values[0])
        elif len(values) == 2:
            start, end = parse_percent(values[0]), parse_percent(values[1])
        else:
            parser.error("最多两个阈值参数")

        cur_start, cur_end = set_threshold(instance, start, end,
                                           verify=not args.no_verify)
        if args.json:
            print(json.dumps({"start": cur_start, "end": cur_end,
                              "mode": describe(cur_start, cur_end)},
                             ensure_ascii=False))
        else:
            print(f"已设置充电阈值: 起始 {cur_start}%  结束 {cur_end}%")
            print(f"当前模式: {describe(cur_start, cur_end)}")
            if not args.no_verify:
                print("已回读校验通过 ✔")
        return 0

    except HwmiError as exc:
        sys.stderr.write(f"[!] {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
