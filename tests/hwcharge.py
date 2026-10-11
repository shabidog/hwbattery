#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hwcharge —— 华为笔记本电池充电阈值小工具（独立实现，不依赖华为电脑管家）

原理
----
华为笔记本（MateBook 等）BIOS 通过 ACPI-WMI 暴露一个 OEM 方法：

    root\\WMI :  OemWMIMethod.OemWMIfun(u64Input, u32Resrved, u8Output[256])
    InstanceName = 'ACPI\\PNP0C14\\HWMI_0'   (GUID ABBC0F5B-8EA1-11D1-A000-C90629100000)

u64Input 是一个 64 位命令字，返回缓冲区 u8Output 共 256 字节，
首字节为状态（0 = 成功），其后是数据。充电阈值相关命令：

    读取: 0x00001103            -> out[1] = 起始阈值(%), out[2] = 结束阈值(%)
    设置: 0x00001003 | 起<<16 | 止<<24   （即字节 [0..1]=0x1003,[2]=起,[3]=止）

这些命令字是从 PCManager 的 WmiUtil.dll（HwSDK::BiosWmi::*）反汇编取出，
与 Linux 主线驱动 drivers/platform/x86/huawei-wmi.c 的 HWMI 协议一致。

注意：部分机型的 BIOS 需要连续调用两次才会真正执行/生效，本工具已内置该重试。

用法
----
    python hwcharge.py                  # 查看当前阈值
    python hwcharge.py 80               # 只改结束阈值（充电上限）为 80%
    python hwcharge.py 70 80            # 起始 70%、结束 80%
    python hwcharge.py 40 70            # 起始 40%、结束 70%（插电才充，更护电池）
    python hwcharge.py off              # 关闭保护：0 100
    python hwcharge.py --raw 0x1103     # 直接下发原始命令（诊断用）

需要管理员权限（写操作必需），依赖 pywin32。
"""

import argparse
import json
import sys

try:
    import win32com.client as win32
except ImportError:  # pragma: no cover
    sys.stderr.write("[!] 缺少依赖 pywin32，请先执行:  python -m pip install pywin32\n")
    sys.exit(2)

WMI_NAMESPACE = "root\\WMI"
WMI_CLASS = "OemWMIMethod"
WMI_METHOD = "OemWMIfun"

CMD_GET_THRESHOLD = 0x00001103  # \GBTT  Get Battery Threshold
CMD_SET_THRESHOLD = 0x00001003  # \SBTT  Set Battery Threshold

STATUS_OK = 0
MAX_TRIES = 2  # 部分机型 BIOS 需要调用两次才执行


class HwChargeError(RuntimeError):
    pass


def _connect():
    """连接 root\\WMI 命名空间，返回 OemWMIMethod 实例。"""
    try:
        locator = win32.Dispatch("WbemScripting.SWbemLocator")
        svc = locator.ConnectServer(".", WMI_NAMESPACE)
    except Exception as exc:  # noqa: BLE001
        raise HwChargeError(f"无法连接 WMI 命名空间 {WMI_NAMESPACE}: {exc}") from exc

    try:
        items = svc.ExecQuery(f"SELECT * FROM {WMI_CLASS}")
    except Exception as exc:  # noqa: BLE001
        raise HwChargeError(
            f"未找到 WMI 类 {WMI_CLASS}（本机 BIOS 可能不支持 HWMI OEM 接口）: {exc}"
        ) from exc

    if items.Count == 0:
        raise HwChargeError(
            f"{WMI_CLASS} 无实例：该机型未暴露华为 HWMI OEM 接口，无法用此法控充"
        )
    return items.ItemIndex(0)


def hwmi_call(instance, cmd):
    """下发一个 HWMI 命令，返回 (状态码, 256 字节返回区)。"""
    try:
        in_params = instance.Methods_(WMI_METHOD).InParameters.SpawnInstance_()
        in_params.u64Input = int(cmd) & 0xFFFFFFFFFFFFFFFF
        out = instance.ExecMethod_(WMI_METHOD, in_params)
    except Exception as exc:  # noqa: BLE001
        raise HwChargeError(f"调用 {WMI_METHOD} 失败（需管理员权限？）: {exc}") from exc

    raw = out.u8Output
    if raw is None:
        raise HwChargeError("BIOS 未返回 u8Output 数据")
    data = bytes(bytearray(int(b) & 0xFF for b in raw))
    return data[0], data


def read_thresholds(instance):
    """读取起止阈值。

    完全照搬 huawei-wmi 的解析方式：在整块 256 字节返回区（含首字节状态）中，
    自末尾向前找到最后一个非零字节 data[i]（i >= 2），则
        start = data[i-1], end = data[i]
    """
    status, data = hwmi_call(instance, CMD_GET_THRESHOLD)
    if status != STATUS_OK:
        # 部分机型首次调用只返回状态，重试一次
        status, data = hwmi_call(instance, CMD_GET_THRESHOLD)

    start = end = 0
    idx = len(data) - 1
    while idx >= 2:
        if data[idx]:
            start, end = data[idx - 1], data[idx]
            break
        idx -= 1
    return start, end


def set_thresholds(instance, start, end, verify=True):
    """设置起止阈值。"""
    if not (0 <= start <= 100 and 0 <= end <= 100):
        raise HwChargeError(f"阈值必须是 0-100，收到 start={start} end={end}")
    if start > end:
        raise HwChargeError(f"起始阈值不能大于结束阈值（{start} > {end}）")

    # 字节布局（来自 WmiUtil.dll @ 0x180003110 反汇编）：
    #   bytes[0..1] = 0x1003 (LE), bytes[2] = start, bytes[3] = end
    cmd = CMD_SET_THRESHOLD | (start << 16) | (end << 24)

    status, data = hwmi_call(instance, cmd)
    if status != STATUS_OK:  # 二次调用
        status, data = hwmi_call(instance, cmd)
    if status != STATUS_OK:
        raise HwChargeError(f"设置阈值失败，BIOS 返回状态 0x{status:02X}")

    if verify:
        cur_start, cur_end = read_thresholds(instance)
        if (cur_start, cur_end) != (start, end):
            raise HwChargeError(
                f"设置后回读不一致：期望 {start}%/{end}%，实际 {cur_start}%/{cur_end}%"
            )
        return cur_start, cur_end
    return start, end


def battery_state():
    """当前电量与是否插电（仅用于展示）。"""
    try:
        locator = win32.Dispatch("WbemScripting.SWbemLocator")
        svc = locator.ConnectServer(".", "root\\cimv2")
        items = svc.ExecQuery(
            "SELECT EstimatedChargeRemaining, BatteryStatus FROM Win32_Battery"
        )
        if items.Count:
            b = items.ItemIndex(0)
            remaining = int(b.EstimatedChargeRemaining)
            plugged = int(b.BatteryStatus) != 1  # 1 = 放电中
            return remaining, plugged
    except Exception:  # noqa: BLE001
        pass
    return None, None


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


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="hwcharge",
        description="华为笔记本电池充电阈值控制（直接走 BIOS ACPI-WMI，不依赖电脑管家）",
        epilog="示例: hwcharge 80 | hwcharge 70 80 | hwcharge off | hwcharge --raw 0x1103",
    )
    parser.add_argument("values", nargs="*", help="[起始] 结束 阈值百分比，或 off")
    parser.add_argument("--raw", help="直接下发原始 64 位命令，如 0x1103", default=None)
    parser.add_argument("--json", action="store_true", help="以 JSON 输出")
    parser.add_argument("--no-verify", action="store_true", help="设置后不回读校验")
    args = parser.parse_args(argv)

    try:
        instance = _connect()

        if args.raw is not None:
            cmd = int(args.raw, 0)
            status, data = hwmi_call(instance, cmd)
            print(f"命令 0x{cmd:016X} -> 状态 0x{status:02X}")
            print("返回区:", " ".join(f"{b:02X}" for b in data[:16]), "...")
            return 0 if status == STATUS_OK else 1

        values = [v.lower() for v in args.values]

        if not values:
            start, end = read_thresholds(instance)
            remaining, plugged = battery_state()
            if args.json:
                print(json.dumps({
                    "start": start, "end": end,
                    "mode": describe(start, end),
                    "charge_percent": remaining,
                    "plugged": plugged,
                }, ensure_ascii=False))
            else:
                extra = ""
                if remaining is not None:
                    extra = f"（当前电量 {remaining}%，{'已插电' if plugged else '未插电'}）"
                print(f"充电阈值: 起始 {start}%  结束 {end}%{extra}")
                print(f"当前模式: {describe(start, end)}")
            return 0

        if values[0] == "off":
            start, end = 0, 100
        elif len(values) == 1:
            start, end = read_thresholds(instance)[0], parse_percent(values[0])
        elif len(values) == 2:
            start, end = parse_percent(values[0]), parse_percent(values[1])
        else:
            parser.error("最多两个阈值参数")

        cur_start, cur_end = set_thresholds(instance, start, end,
                                            verify=not args.no_verify)
        if args.json:
            print(json.dumps({"start": cur_start, "end": cur_end,
                              "mode": describe(cur_start, cur_end)}, ensure_ascii=False))
        else:
            print(f"已设置充电阈值: 起始 {cur_start}%  结束 {cur_end}%")
            print(f"当前模式: {describe(cur_start, cur_end)}")
            if not args.no_verify:
                print("已回读校验通过 ✔")
        return 0

    except HwChargeError as exc:
        sys.stderr.write(f"[!] {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
