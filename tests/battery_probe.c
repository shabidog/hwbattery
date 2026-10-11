/* battery_probe.c —— 纯 Win32 电池读数探针（控制台）
 *
 * 为什么不用 WMI：本机 Win32_Battery 的 DesignCapacity / FullChargeCapacity
 * 经 pywin32 实测均为 null，拿不到容量，因此必须直接问电池驱动。
 *
 * 走的是标准三层：
 *   1) SetupDiGetClassDevs(GUID_DEVCLASS_BATTERY)  枚举电池设备接口
 *   2) IOCTL_BATTERY_QUERY_TAG                     取得 BatteryTag（会话句柄）
 *   3) IOCTL_BATTERY_QUERY_INFORMATION             取 BatteryInformation / BatteryStatus
 *
 * 编译: gcc -O2 -o battery_probe.exe battery_probe.c -lsetupapi
 */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <initguid.h>   /* 让下面各头文件里的 DEFINE_GUID 直接产生定义，免链接 uuid.lib */
#include <devguid.h>
#include <setupapi.h>
#include <batclass.h>

/* mingw 的 poclass.h 把 BATTERY_QUERY_INFORMATION_LEVEL 只列到 BatterySerialNumber，
 * 而且成员顺序与 Windows SDK 并不一致（ManufactureDate / ManufactureName 位置互换，
 * 且完全没有 CycleCount / FullChargedCapacity / BatteryStatus）。
 * 由于驱动只认 ULONG 数值，这里不依赖该枚举的名字，改用 SDK 的真实编号。 */
#define HW_LEVEL_INFORMATION     0   /* BatteryInformation         */
#define HW_LEVEL_CYCLE_COUNT     8   /* BatteryCycleCount          */
#define HW_LEVEL_FULL_CHARGED    9   /* BatteryFullChargedCapacity */
#define HW_LEVEL_STATUS         12   /* BatteryStatus              */

static int probe_one(const wchar_t *path, int index)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  [%d] 打开失败: %lu\n", index, (unsigned long)GetLastError());
        return 0;
    }

    DWORD wait = 0, bytes = 0, tag = 0;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait),
                         &tag, sizeof(tag), &bytes, NULL)) {
        printf("  [%d] IOCTL_BATTERY_QUERY_TAG 失败: %lu\n", index, (unsigned long)GetLastError());
        CloseHandle(h);
        return 0;
    }

    BATTERY_QUERY_INFORMATION bqi;
    ZeroMemory(&bqi, sizeof(bqi));
    bqi.BatteryTag = tag;

    BATTERY_INFORMATION bi;
    ZeroMemory(&bi, sizeof(bi));
    bqi.InformationLevel = (BATTERY_QUERY_INFORMATION_LEVEL)HW_LEVEL_INFORMATION;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi),
                         &bi, sizeof(bi), &bytes, NULL)) {
        printf("  [%d] QUERY_INFORMATION(BatteryInformation) 失败: %lu\n",
               index, (unsigned long)GetLastError());
        CloseHandle(h);
        return 0;
    }

    /* 状态不能走 QUERY_INFORMATION 的 level 12 —— 驱动会以
     * ERROR_INVALID_FUNCTION(1) 拒绝。它有自己的 IOCTL：
     * IOCTL_BATTERY_QUERY_STATUS，输入 BATTERY_WAIT_STATUS。 */
    BATTERY_WAIT_STATUS bws;
    ZeroMemory(&bws, sizeof(bws));
    bws.BatteryTag = tag;   /* Timeout 与各过滤条件留 0 = 立即返回当前状态 */

    BATTERY_STATUS bs;
    ZeroMemory(&bs, sizeof(bs));
    BOOL have_status = DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws),
                                       &bs, sizeof(bs), &bytes, NULL);

    printf("  [%d] 化学体系  : %c%c%c%c\n", index,
           bi.Chemistry[0], bi.Chemistry[1], bi.Chemistry[2], bi.Chemistry[3]);
    printf("      出厂设计容量: %lu mWh\n", (unsigned long)bi.DesignedCapacity);
    printf("      当前满充容量: %lu mWh\n", (unsigned long)bi.FullChargedCapacity);
    if (bi.DesignedCapacity) {
        printf("      健康度      : %.1f%%\n",
               (double)bi.FullChargedCapacity * 100.0 / (double)bi.DesignedCapacity);
    }
    printf("      循环次数    : %lu\n", (unsigned long)bi.CycleCount);
    printf("      Capabilities: 0x%08lX\n", (unsigned long)bi.Capabilities);

    if (have_status) {
        printf("      当前容量    : %lu mWh\n", (unsigned long)bs.Capacity);
        if (bi.FullChargedCapacity) {
            printf("      当前电量    : %.1f%%\n",
                   (double)bs.Capacity * 100.0 / (double)bi.FullChargedCapacity);
        }
        printf("      PowerState  : 0x%08lX (%s%s%s)\n", (unsigned long)bs.PowerState,
               (bs.PowerState & BATTERY_POWER_ON_LINE) ? "已插电 " : "",
               (bs.PowerState & BATTERY_CHARGING) ? "充电中 " : "",
               (bs.PowerState & BATTERY_DISCHARGING) ? "放电中" : "");
        printf("      电压        : %lu mV   速率: %ld mW\n",
               (unsigned long)bs.Voltage, (long)bs.Rate);
    } else {
        printf("      (BatteryStatus 查询失败: %lu)\n", (unsigned long)GetLastError());
    }

    CloseHandle(h);
    return 1;
}

int main(void)
{
    printf("=== 电池探针 (SetupAPI + IOCTL_BATTERY_QUERY_*) ===\n");

    HDEVINFO hdev = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, 0, 0,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs 失败: %lu\n", (unsigned long)GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA did;
    ZeroMemory(&did, sizeof(did));
    did.cbSize = sizeof(did);

    int found = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hdev, 0, &GUID_DEVCLASS_BATTERY, i, &did); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &need, NULL);
        if (!need) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pdid =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
        if (!pdid) break;
        pdid->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hdev, &did, pdid, need, NULL, NULL)) {
            probe_one(pdid->DevicePath, (int)i);
            found++;
        }
        free(pdid);
    }

    if (!found) printf("未找到任何电池设备 (GUID_DEVCLASS_BATTERY)\n");

    SetupDiDestroyDeviceInfoList(hdev);
    return 0;
}
