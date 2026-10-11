/* hwbattery.c —— 华为笔记本电池托盘工具（纯 Win32 / 纯 C，无第三方依赖）
 *
 * 功能
 *   1. 托盘常驻，右键菜单查看电量/容量/健康度，并可直接设定充电截止百分比
 *   2. 托盘图标悬浮提示实时显示关键数值
 *   3. 可一键写入开机自启（HKCU\\...\\Run）
 *
 * 数据来源（两条完全独立的通道）
 *   电池容量/健康度 : SetupAPI + IOCTL_BATTERY_QUERY_INFORMATION
 *                     本机 WMI 的 Win32_Battery 返回 DesignCapacity /
 *                     FullChargeCapacity 均为空，所以必须直接问电池驱动。
 *   充电阈值        : ACPI-WMI  root\\WMI\\OemWMIMethod::OemWMIfun
 *                     命令字 0x1003，字节布局 [0..1]=0x1003 [2]=起始% [3]=结束%
 *                     （由 PCManager 的 WmiUtil.dll!SetBatteryChargeThreshlod
 *                       与 HardwareHal.dll!Battery::SetChargeThreshold 反汇编得到）
 *
 * 编译（mingw-w64 gcc，见同目录 build.bat）
 *   gcc -O2 -o hwbattery.exe hwbattery.c \
 *       -lsetupapi -lole32 -loleaut32 -luuid -lwbemuuid \
 *       -lshell32 -luser32 -ladvapi32 -lgdi32 -mwindows
 *
 * 清单声明 requireAdministrator：启动即申请管理员权限（下发阈值必需）。
 * 副作用：走 HKCU\\...\\Run 的开机自启每次登录会弹 UAC 确认框。
 */

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>   /* 让 devguid.h / batclass.h 里的 DEFINE_GUID 直接产生定义 */
#include <devguid.h>
#include <setupapi.h>
#include <batclass.h>
#include <wbemidl.h>

/* ---------------------------------------------------------------- 常量 */

#define APP_TITLE      L"华为笔记本充电保护"
#define APP_CLASS      L"HwBatteryTrayWnd"
#define APP_RUNKEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_RUNVALUE   L"HwBatteryTray"           /* 旧实现的 Run 键值名，仅用于清理 */
#define APP_TASKNAME   L"HwBatteryTray-Autostart" /* 任务计划程序中的任务名 */

#define WM_TRAYMSG     (WM_APP + 1)
#define ID_TRAY        1
#define ID_TIMER       1001
#define REFRESH_MS     15000    /* 刷新间隔：启动时先读一次，之后按此周期更新 */

/* 菜单命令 ID */
#define IDM_REFRESH    100
#define IDM_AUTOSTART  101
#define IDM_EXIT       102
#define IDM_OPENCFG    103      /* 打开设置窗口 */
#define IDM_SET_BASE   200      /* 200..210 依次对应 50%..100%（充电截止） */
#define IDM_START_BASE 300      /* 300..309 依次对应 0%..90%（起始充电） */

/* 充电截止档位：50..100，步长 5。
 * 原先只给 80..100，连 README 自己推荐的 70 都够不着，故按脚本允许的范围放宽。 */
#define SET_MIN_PCT    50
#define SET_MAX_PCT    100

/* 起始充电档位：0..90，步长 10。0 = 不限制。 */
#define START_MIN_PCT  0
#define START_MAX_PCT  90
#define START_STEP     10
#define START_COUNT    ((START_MAX_PCT - START_MIN_PCT) / START_STEP + 1)

/* mingw 的 poclass.h 把 BATTERY_QUERY_INFORMATION_LEVEL 只列到 BatterySerialNumber，
 * 成员顺序与 Windows SDK 也不一致，因此不依赖该枚举的名字，按 SDK 的真实编号传入。 */
#define HW_LEVEL_INFORMATION     0   /* BatteryInformation */

/* ---------------------------------------------------------------- 电池信息 */

typedef struct {
    BOOL  ok;                 /* 是否成功读取 */
    char  chemistry[5];       /* "LION" 等 */
    DWORD designed;           /* 出厂设计容量 mWh */
    DWORD full_charged;       /* 当前满充容量 mWh */
    DWORD cycle_count;        /* 循环次数 */
    DWORD current;            /* 当前容量 mWh */
    DWORD voltage_mv;
    LONG  rate_mw;
    DWORD power_state;
    int   percent;            /* 当前电量 % */
    int   health;             /* 健康度 % = 满充/设计 */
    BOOL  online;             /* 是否已插电 */

    /* 充电阈值（来自 HWMI，读失败时 valid=FALSE） */
    BOOL  threshold_valid;
    int   th_start;
    int   th_end;
} BatteryState;

/* 读一个电池设备接口路径，填充 state。返回是否成功。 */
static BOOL battery_read_device(const wchar_t *path, BatteryState *st)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    DWORD wait = 0, bytes = 0, tag = 0;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait),
                         &tag, sizeof(tag), &bytes, NULL)) {
        CloseHandle(h);
        return FALSE;
    }

    BATTERY_QUERY_INFORMATION bqi;
    ZeroMemory(&bqi, sizeof(bqi));
    bqi.BatteryTag = tag;

    BATTERY_INFORMATION bi;
    ZeroMemory(&bi, sizeof(bi));
    bqi.InformationLevel = (BATTERY_QUERY_INFORMATION_LEVEL)HW_LEVEL_INFORMATION;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi),
                         &bi, sizeof(bi), &bytes, NULL)) {
        CloseHandle(h);
        return FALSE;
    }

    /* 状态必须用专用 IOCTL：QUERY_INFORMATION 的 level 12 会被驱动以
     * ERROR_INVALID_FUNCTION(1) 拒绝。 */
    BATTERY_WAIT_STATUS bws;
    ZeroMemory(&bws, sizeof(bws));
    bws.BatteryTag = tag;

    BATTERY_STATUS bs;
    ZeroMemory(&bs, sizeof(bs));
    BOOL have_status = DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws),
                                       &bs, sizeof(bs), &bytes, NULL);
    CloseHandle(h);

    st->ok = TRUE;
    memcpy(st->chemistry, bi.Chemistry, 4);
    st->chemistry[4] = '\0';
    st->designed = bi.DesignedCapacity;
    st->full_charged = bi.FullChargedCapacity;
    st->cycle_count = bi.CycleCount;

    if (have_status) {
        st->current = bs.Capacity;
        st->voltage_mv = bs.Voltage;
        st->rate_mw = bs.Rate;
        st->power_state = bs.PowerState;
        st->online = (bs.PowerState & BATTERY_POWER_ON_LINE) != 0;
        if (st->full_charged)
            st->percent = (int)((double)bs.Capacity * 100.0 / (double)st->full_charged + 0.5);
    }
    if (st->designed)
        st->health = (int)((double)st->full_charged * 100.0 / (double)st->designed + 0.5);

    return TRUE;
}

/* 枚举电源设备类，找第一个可用的电池。 */
static BOOL battery_read(BatteryState *st)
{
    ZeroMemory(st, sizeof(*st));

    HDEVINFO hdev = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, 0, 0,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE)
        return FALSE;

    SP_DEVICE_INTERFACE_DATA did;
    ZeroMemory(&did, sizeof(did));
    did.cbSize = sizeof(did);

    BOOL found = FALSE;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hdev, 0, &GUID_DEVCLASS_BATTERY, i, &did); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &need, NULL);
        if (!need)
            continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pdid =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
        if (!pdid)
            break;
        pdid->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hdev, &did, pdid, need, NULL, NULL)) {
            if (battery_read_device(pdid->DevicePath, st)) {
                found = TRUE;
                free(pdid);
                break;
            }
        }
        free(pdid);
    }

    SetupDiDestroyDeviceInfoList(hdev);
    if (!found)
        st->ok = FALSE;
    return found;
}

/* ---------------------------------------------------------------- HWMI (ACPI-WMI)

 * root\WMI 的 OemWMIMethod.OemWMIfun(u64Input IN, u32Resrved OUT, u8Output OUT)
 * u8Output 共 256 字节，首字节为状态码（0 = 成功）。
 */

#define CMD_GET_THRESHOLD   0x1103ULL   /* \GBTT 读取阈值（固件提供，不在 DLL 内） */
#define CMD_SET_THRESHOLD   0x1003ULL   /* \SBTT 设置阈值                         */

static BOOL g_com_ready = FALSE;
static IWbemServices *g_svc = NULL;

/* 实例路径与方法签名在整个进程生命周期内恒定，解析一次后缓存复用。
 * 未缓存时每次调用要走 5 次 WMI 往返（查实例 / 取 __PATH / 取类 /
 * 取方法 / 构造入参），缓存后每次只剩 ExecMethod 这一次真正的远程调用。 */
static BSTR             g_inst_path = NULL;   /* OemWMIMethod 实例的 __PATH */
static IWbemClassObject *g_in_cls    = NULL;   /* OemWMIfun 的入参类定义 */

static BOOL hwmi_connect(void)
{
    if (g_svc)
        return TRUE;

    HRESULT hr;
    if (!g_com_ready) {
        hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        /* RPC_E_CHANGED_MODE：本线程已以 STA 初始化过，沿用即可，不算失败 */
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            return FALSE;
        CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
        g_com_ready = TRUE;
    }

    IWbemLocator *loc = NULL;
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr) || !loc)
        return FALSE;

    hr = loc->lpVtbl->ConnectServer(loc, L"root\\WMI",
                                    NULL,      /* strUser        */
                                    NULL,      /* strPassword    */
                                    NULL,      /* strLocale      */
                                    0,         /* lSecurityFlags */
                                    NULL,      /* strAuthority   */
                                    NULL,      /* pCtx           */
                                    &g_svc);   /* ppNamespace    */
    loc->lpVtbl->Release(loc);
    if (FAILED(hr) || !g_svc) {
        g_svc = NULL;
        return FALSE;
    }

    CoSetProxyBlanket((IUnknown *)g_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return TRUE;
}

/* 下发一个 64 位 HWMI 命令，回填 256 字节返回区与状态码。 */
/* 解析实例路径与 OemWMIfun 的入参类定义，结果交给调用方缓存。
 * 只在首次调用时执行。方法签名必须从**类对象**上取——实测在 ExecQuery
 * 得到的实例上调用 GetMethod 会被这个动态 provider 以
 * WBEM_E_ILLEGAL_OPERATION (0x8004101E) 拒绝，而类对象上调用则成功。 */
static BOOL hwmi_resolve(BSTR *out_path, IWbemClassObject **out_cls)
{
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *inst = NULL, *cls = NULL;
    ULONG got = 0;
    BOOL ok = FALSE;
    VARIANT v;
    VariantInit(&v);

    HRESULT hr = g_svc->lpVtbl->ExecQuery(g_svc, L"WQL",
                                          L"SELECT * FROM OemWMIMethod",
                                          WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                          NULL, &en);
    if (FAILED(hr) || !en)
        goto done;
    if (FAILED(en->lpVtbl->Next(en, WBEM_INFINITE, 1, &inst, &got)) || got == 0)
        goto done;
    if (FAILED(inst->lpVtbl->Get(inst, L"__PATH", 0, &v, NULL, NULL)) || v.vt != VT_BSTR)
        goto done;
    if (FAILED(g_svc->lpVtbl->GetObject(g_svc, L"OemWMIMethod", 0, NULL, &cls, NULL)))
        goto done;
    if (FAILED(cls->lpVtbl->GetMethod(cls, L"OemWMIfun", 0, out_cls, NULL)))
        goto done;

    *out_path = SysAllocString(v.bstrVal);   /* 复制一份，免得和 v 的所有权纠缠 */
    ok = (*out_path != NULL);

done:
    if (!ok && *out_cls) {
        (*out_cls)->lpVtbl->Release(*out_cls);
        *out_cls = NULL;
    }
    VariantClear(&v);
    if (inst) inst->lpVtbl->Release(inst);
    if (cls)  cls->lpVtbl->Release(cls);
    if (en)   en->lpVtbl->Release(en);
    return ok;
}

/* 清掉缓存的实例路径与方法签名，下次调用会重新解析。
 * 缓存只应在"确实失效"时丢弃，所以仅在后续调用失败时使用它。 */
static void hwmi_drop_cache(void)
{
    if (g_in_cls) {
        g_in_cls->lpVtbl->Release(g_in_cls);
        g_in_cls = NULL;
    }
    if (g_inst_path) {
        SysFreeString(g_inst_path);
        g_inst_path = NULL;
    }
}

static BOOL hwmi_call(unsigned long long cmd, BYTE *out256, BYTE *status)
{
    if (!hwmi_connect())
        return FALSE;

    /* 首次解析后一直复用，不再每 5 秒重查一遍 */
    if (!g_in_cls || !g_inst_path) {
        if (!hwmi_resolve(&g_inst_path, &g_in_cls))
            return FALSE;
    }

    IWbemClassObject *inInst = NULL, *outInst = NULL;
    BOOL ok = FALSE;

    if (FAILED(g_in_cls->lpVtbl->SpawnInstance(g_in_cls, 0, &inInst)) || !inInst) {
        hwmi_drop_cache();       /* 缓存的签名可能已失效，清掉待下次重解析 */
        return FALSE;
    }

    /* CIM uint64 传十进制字符串最稳（WMI 自行转换） */
    {
        wchar_t buf[32];
        VARIANT in;
        VariantInit(&in);
        swprintf(buf, 32, L"%llu", cmd);
        in.vt = VT_BSTR;
        in.bstrVal = SysAllocString(buf);
        HRESULT hp = inInst->lpVtbl->Put(inInst, L"u64Input", 0, &in, 0);
        SysFreeString(in.bstrVal);
        in.bstrVal = NULL;
        VariantClear(&in);
        if (FAILED(hp)) {
            inInst->lpVtbl->Release(inInst);
            hwmi_drop_cache();
            return FALSE;
        }
    }

    HRESULT hr = g_svc->lpVtbl->ExecMethod(g_svc, g_inst_path, L"OemWMIfun", 0, NULL,
                                           inInst, &outInst, NULL);
    inInst->lpVtbl->Release(inInst);

    if (FAILED(hr) || !outInst) {
        /* 失败有可能是 provider 重注册让缓存失效。清掉缓存让下次重新解析，
         * 免得一次失败把后续所有读取都粘死。 */
        hwmi_drop_cache();
        if (outInst) outInst->lpVtbl->Release(outInst);
        return FALSE;
    }

    VARIANT o;
    VariantInit(&o);
    hr = outInst->lpVtbl->Get(outInst, L"u8Output", 0, &o, NULL, NULL);
    if (SUCCEEDED(hr) && (o.vt & VT_ARRAY) && o.parray) {
        LONG lb = 0, ub = 0;
        SafeArrayGetLBound(o.parray, 1, &lb);
        SafeArrayGetUBound(o.parray, 1, &ub);
        DWORD cnt = (DWORD)(ub - lb + 1);
        BYTE *data = NULL;
        if (SUCCEEDED(SafeArrayAccessData(o.parray, (void **)&data)) && data) {
            DWORD n = cnt < 256 ? cnt : 256;
            memcpy(out256, data, n);
            if (n < 256)
                ZeroMemory(out256 + n, 256 - n);
            *status = out256[0];
            ok = TRUE;
            SafeArrayUnaccessData(o.parray);
        }
    }
    VariantClear(&o);
    outInst->lpVtbl->Release(outInst);
    return ok;
}

/* 部分机型 BIOS 需要连续两次调用才执行，这里统一封装重试。 */
static BOOL hwmi_call_retry(unsigned long long cmd, BYTE *out256, BYTE *status)
{
    if (!hwmi_call(cmd, out256, status))
        return FALSE;
    if (*status == 0)
        return TRUE;
    return hwmi_call(cmd, out256, status);
}

/* 读取起止阈值（0x1103）。解析规则：自末尾向前找最后一个非零字节 out[i]
 * （i >= 2），则 start = out[i-1]、end = out[i]。
 * 注意这条规则只适用于 0x1103；0x1603(GetSmartChargeMode) 是固定偏移布局。 */
static BOOL hwmi_read_threshold(int *start, int *end)
{
    BYTE out[256] = {0}, status = 0;
    if (!hwmi_call_retry(CMD_GET_THRESHOLD, out, &status) || status != 0)
        return FALSE;

    for (int i = 255; i >= 2; i--) {
        if (out[i]) {
            *start = out[i - 1];
            *end = out[i];
            return TRUE;
        }
    }
    *start = 0;
    *end = 100;
    return TRUE;
}

/* 设置起止阈值（0x1003）。字节布局 [0..1]=0x1003 [2]=起始 [3]=结束 */
static BOOL hwmi_set_threshold(int start, int end)
{
    if (start < 0 || start > 100 || end < 0 || end > 100 || start > end)
        return FALSE;

    unsigned long long cmd = CMD_SET_THRESHOLD
                           | ((unsigned long long)(start & 0xFF) << 16)
                           | ((unsigned long long)(end & 0xFF) << 24);

    BYTE out[256] = {0}, status = 0;
    if (!hwmi_call_retry(cmd, out, &status))
        return FALSE;
    return status == 0;
}

/* ---------------------------------------------------------------- 全局状态 */

#define SET_STEP   5
#define SET_COUNT  ((SET_MAX_PCT - SET_MIN_PCT) / SET_STEP + 1)   /* 80..100 共 5 档 */

static BatteryState    g_state;
static NOTIFYICONDATAW g_nid;
static BOOL            g_autostart = FALSE;

static void refresh_state(void);
static void cfg_show(void);      /* 设置窗口，定义见后文 */
static void cfg_refresh(HWND h); /* 设置窗口内的数值刷新，定义见后文 */
static HWND g_cfg = NULL;        /* 设置窗口句柄，由 cfg_show 创建 */

/* ---------------------------------------------------------------- 配置文件
 *
 * hwbattery.ini 放在 exe 同目录，保存"期望的"充电阈值：
 * 程序每次启动都会把它向固件下发一次；文件不存在则按默认值生成。
 *
 * 放 exe 同目录的理由：配置跟着程序走，且 release/ 已被 .gitignore 忽略，
 * 不会误入版本库。
 *
 * 文件内容刻意保持纯 ASCII：WritePrivateProfileStringW 在简中系统上按 GBK 写，
 * 中文字面量会带来跨编辑器的编码麻烦。含义在 README 里用中文说明。
 */

#define CFG_INI_NAME   L"hwbattery.ini"
#define CFG_DEF_START  65
#define CFG_DEF_END    90

static int  g_cfg_start   = CFG_DEF_START;   /* 配置里的期望值 */
static int  g_cfg_end     = CFG_DEF_END;
static BOOL g_cfg_applied = FALSE;           /* 最近一次启动下发是否成功 */

/* 拼出 exe 同目录下 ini 的完整路径 */
static BOOL cfg_ini_path(wchar_t *out, size_t cap)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return FALSE;

    wchar_t *slash = wcsrchr(exe, L'\\');
    if (!slash)
        return FALSE;
    slash[1] = L'\0';                       /* 砍掉文件名，留下目录 */

    if (wcslen(exe) + wcslen(CFG_INI_NAME) + 1 > cap)
        return FALSE;
    wcscpy(out, exe);
    wcscat(out, CFG_INI_NAME);
    return TRUE;
}

/* 把期望阈值写入 ini（附一段 ASCII 说明，让文件自身可读） */
static BOOL cfg_save(int start, int end)
{
    wchar_t path[MAX_PATH], buf[16];
    if (!cfg_ini_path(path, MAX_PATH))
        return FALSE;

    swprintf(buf, 16, L"%d", start);
    if (!WritePrivateProfileStringW(L"charge", L"start", buf, path))
        return FALSE;

    swprintf(buf, 16, L"%d", end);
    if (!WritePrivateProfileStringW(L"charge", L"end", buf, path))
        return FALSE;

    WritePrivateProfileStringW(L"help", L"start",
        L"0-100; charging only starts below this percent", path);
    WritePrivateProfileStringW(L"help", L"end",
        L"0-100; charging stops at this percent", path);
    WritePrivateProfileStringW(L"help", L"note",
        L"Written by hwbattery.exe; applied to the firmware at every start", path);
    return TRUE;
}

/* 读取配置；文件不存在则按默认值生成一份 */
static BOOL cfg_load(int *start, int *end)
{
    wchar_t path[MAX_PATH];
    if (!cfg_ini_path(path, MAX_PATH))
        return FALSE;

    BOOL fresh = (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);

    *start = GetPrivateProfileIntW(L"charge", L"start", CFG_DEF_START, path);
    *end   = GetPrivateProfileIntW(L"charge", L"end",   CFG_DEF_END,   path);

    /* 收敛到合法范围：手改 ini 写出越界值时也不至于下发坏数据 */
    if (*start < 0 || *start > 100)
        *start = CFG_DEF_START;
    if (*end < 0 || *end > 100)
        *end = CFG_DEF_END;
    if (*start > *end)
        *start = 0;                          /* 起始高于截止则取消下限 */

    if (fresh)
        cfg_save(*start, *end);              /* 首次运行：落一份默认配置 */

    return TRUE;
}

/* 下发一组阈值，同时把"期望值"落盘。
 * 即便本次下发失败也照样记录：非管理员时用户设的值不会丢，
 * 下次以管理员启动会自动补上。 */
static BOOL apply_threshold(int start, int end)
{
    BOOL ok = hwmi_set_threshold(start, end);

    cfg_save(start, end);
    g_cfg_start = start;
    g_cfg_end   = end;

    if (ok) {
        Sleep(300);                          /* 给固件一点时间落盘再回读 */
        refresh_state();
    }
    return ok;
}

/* ---------------------------------------------------------------- 开机自启
 *
 * 用**任务计划程序**而不是 HKCU 的 Run 键：
 * Run 键拉起的进程无法静默提权，每次登录必弹 UAC；本程序清单是
 * requireAdministrator，用 Run 键自启就必然弹框。任务以 /RL HIGHEST 创建，
 * 由计划程序以最高权限拉起，登录时才不弹 UAC。
 *
 * 取消自启 = 删除该任务；两种情况下都顺带清掉旧的 Run 键值，避免新旧两条
 * 各启一次（旧键值本身也属于"自启"，关掉时必须一并移除）。
 */

/* 运行命令并等它结束，返回退出码；起不来返回 -1 */
static int run_wait(const wchar_t *cmdline)
{
    wchar_t buf[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;

    wcsncpy(buf, cmdline, 1023);
    buf[1023] = L'\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return -1;

    WaitForSingleObject(pi.hProcess, 20000);
    if (!GetExitCodeProcess(pi.hProcess, &code))
        code = (DWORD)-1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* 清掉旧实现留下的 Run 键值（存在才删，本来没有也算成功） */
static void drop_legacy_run_value(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_RUNKEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, APP_RUNVALUE);
        RegCloseKey(k);
    }
}

static BOOL autostart_enabled(void)
{
    wchar_t cmd[256];
    swprintf(cmd, 256, L"schtasks /Query /TN \"%s\"", APP_TASKNAME);
    return run_wait(cmd) == 0;
}

static BOOL autostart_set(BOOL on)
{
    wchar_t exe[MAX_PATH], cmd[1024];
    int rc;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return FALSE;

    drop_legacy_run_value();

    if (on) {
        /* /SC ONLOGON 登录触发；/RL HIGHEST 最高权限运行（免 UAC 弹框）；
         * /IT 仅交互式，保证托盘图标出现在用户会话；/F 覆盖同名任务。
         * /TR 的内层引号写成 \" 转义，防路径含空格。 */
        swprintf(cmd, 1024,
                 L"schtasks /Create /F /TN \"%s\" /SC ONLOGON /RL HIGHEST /IT "
                 L"/TR \"\\\"%s\\\"\"",
                 APP_TASKNAME, exe);
        rc = run_wait(cmd);
        return rc == 0;
    }

    /* 取消：任务本就不存在时目标状态已达成，按成功处理 */
    if (!autostart_enabled())
        return TRUE;

    swprintf(cmd, 1024, L"schtasks /Delete /F /TN \"%s\"", APP_TASKNAME);
    rc = run_wait(cmd);
    return rc == 0;
}

/* ---------------------------------------------------------------- 托盘显示 */

static void update_tooltip(void)
{
    wchar_t tip[128];

    if (!g_state.ok) {
        wcscpy(tip, L"未检测到电池");
    } else if (g_state.threshold_valid) {
        swprintf(tip, 128, L"电量 %d%%  健康 %d%%  阈值 %d-%d%%",
                 g_state.percent, g_state.health, g_state.th_start, g_state.th_end);
    } else {
        swprintf(tip, 128, L"电量 %d%%  健康 %d%%  阈值不可用",
                 g_state.percent, g_state.health);
    }

    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.szTip[127] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void refresh_state(void)
{
    battery_read(&g_state);

    int s = 0, e = 0;
    if (hwmi_read_threshold(&s, &e)) {
        g_state.th_start = s;
        g_state.th_end = e;
        g_state.threshold_valid = TRUE;
    } else {
        g_state.threshold_valid = FALSE;
    }

    update_tooltip();
}

/* 弹出右键菜单 */
static void show_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    HMENU sub;
    wchar_t line[160];

    if (g_state.ok) {
        swprintf(line, 160, L"电量 %d%%（%s）", g_state.percent,
                 g_state.online ? L"已插电" : L"电池供电");
        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, line);

        swprintf(line, 160, L"健康度 %d%%   循环 %lu 次", g_state.health,
                 (unsigned long)g_state.cycle_count);
        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, line);

        swprintf(line, 160, L"设计 %lu / 满充 %lu mWh",
                 (unsigned long)g_state.designed, (unsigned long)g_state.full_charged);
        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, line);
    } else {
        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"未检测到电池");
    }

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    sub = CreatePopupMenu();
    for (int i = 0; i < SET_COUNT; i++) {
        int pct = SET_MIN_PCT + i * SET_STEP;
        UINT flags = MF_STRING;
        if (g_state.threshold_valid && g_state.th_end == pct)
            flags |= MF_CHECKED;
        swprintf(line, 160, L"充电到 %d%% 停止", pct);
        AppendMenuW(sub, flags, IDM_SET_BASE + i, line);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"充电截止");

    /* 起始充电：低于该值才开始充；0 = 不限制。
     * 取值参考 hwmi.py 允许的 0..100 区间，菜单按步长 10 列出常用档。 */
    sub = CreatePopupMenu();
    for (int i = 0; i < START_COUNT; i++) {
        int pct = START_MIN_PCT + i * START_STEP;
        UINT flags = MF_STRING;
        if (g_state.threshold_valid && g_state.th_start == pct)
            flags |= MF_CHECKED;
        if (pct == 0)
            wcsncpy(line, L"不限制（0%）", 160);
        else
            swprintf(line, 160, L"低于 %d%% 才开始充", pct);
        AppendMenuW(sub, flags, IDM_START_BASE + i, line);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"起始充电");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    AppendMenuW(m, MF_STRING, IDM_OPENCFG, L"打开设置窗口(&S)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    /* 把状态直接写进菜单文字，用户点之前就能看清当前是开还是关 */
    g_autostart = autostart_enabled();
    swprintf(line, 160, L"开机自启：%s",
             g_autostart ? L"已启用（点击取消）" : L"未启用（点击设置）");
    AppendMenuW(m, MF_STRING, IDM_AUTOSTART, line);
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"刷新");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);         /* 否则菜单可能不消失 */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------------------------------------------------------- 窗口过程 */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYMSG:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            show_menu(hwnd);
            return 0;
        }
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            cfg_show();          /* 左键点图标 -> 打开设置窗口 */
            return 0;
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(wp);

        /* 起始充电：保留当前截止值，只改起始 */
        if (id >= IDM_START_BASE && id < IDM_START_BASE + START_COUNT) {
            int start = START_MIN_PCT + (id - IDM_START_BASE) * START_STEP;
            int end = g_state.threshold_valid ? g_state.th_end : 100;

            if (start > end) {
                MessageBoxW(hwnd,
                            L"起始阈值不能高于截止阈值。\n\n"
                            L"请先把「充电截止」调高，再设起始值。",
                            APP_TITLE, MB_ICONWARNING);
            } else if (!apply_threshold(start, end)) {
                MessageBoxW(hwnd,
                            L"设置充电阈值失败。\n\n"
                            L"本程序已以管理员权限运行；若仍失败，\n"
                            L"通常是本机 BIOS 未开放华为 HWMI OEM 接口。",
                            APP_TITLE, MB_ICONWARNING);
            }
            return 0;
        }

        if (id >= IDM_SET_BASE && id < IDM_SET_BASE + SET_COUNT) {
            int pct = SET_MIN_PCT + (id - IDM_SET_BASE) * SET_STEP;
            int start = g_state.threshold_valid ? g_state.th_start : 0;
            if (start > pct)
                start = 0;             /* 起始不能高于截止 */

            if (!apply_threshold(start, pct)) {
                MessageBoxW(hwnd,
                            L"设置充电阈值失败。\n\n"
                            L"本程序已以管理员权限运行；若仍失败，\n"
                            L"通常是本机 BIOS 未开放华为 HWMI OEM 接口，\n"
                            L"可用 tests 目录下的 hwmi.py 单独验证接口。",
                            APP_TITLE, MB_ICONWARNING);
            }
            return 0;
        }

        switch (id) {
        case IDM_OPENCFG:
            cfg_show();
            return 0;
        case IDM_REFRESH:
            refresh_state();
            return 0;
        case IDM_AUTOSTART: {
            /* 当前是开就取消，是关就设置 —— 双向都在同一个入口 */
            BOOL want = !autostart_enabled();
            if (!autostart_set(want)) {
                MessageBoxW(hwnd, L"写入开机自启项失败（注册表不可写？）",
                            APP_TITLE, MB_ICONWARNING);
            } else {
                /* 立刻回读确认，别让用户猜到底生效没有 */
                BOOL now = autostart_enabled();
                MessageBoxW(hwnd,
                            now ? L"已设置开机自启，下次登录自动运行。"
                                : L"已取消开机自启。",
                            APP_TITLE, MB_ICONINFORMATION);
            }
            g_autostart = autostart_enabled();
            return 0;
        }
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == ID_TIMER) {
            refresh_state();
            /* 设置窗口开着时一并刷新。复用托盘这一个定时器、不再另建，
             * 两处刷新节奏天然一致；窗口没开时这里几乎零开销。 */
            if (g_cfg && IsWindowVisible(g_cfg))
                cfg_refresh(g_cfg);
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        KillTimer(hwnd, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- DPI 与字体
 *
 * 进程必须声明 DPI 感知：否则在高 DPI 屏上 Windows 会把整个窗口位图拉伸，
 * 字就发糊。声明之后再按系统 DPI 放大布局尺寸与字号，控件才既清晰又不至于过小。
 */
static int   g_dpi = 96;      /* 由 WinMain 在 SetProcessDPIAware() 之后填入 */
static HFONT g_ui_font = NULL;

/* 以 96 DPI 为基准缩放一个逻辑像素值 */
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT ui_font(void)
{
    if (!g_ui_font) {
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight  = -MulDiv(9, g_dpi, 72);   /* 9pt */
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;       /* 亚像素渲染，中文更清晰 */
        wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
        g_ui_font = CreateFontIndirectW(&lf);
    }
    return g_ui_font;
}

/* 建一个子控件并统一挂上界面字体；所有坐标都走 S() 缩放 */
static HWND mkctl(const wchar_t *cls, const wchar_t *text, DWORD style,
                  int x, int y, int w, int h, HWND parent, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (c)
        SendMessageW(c, WM_SETFONT, (WPARAM)ui_font(), TRUE);
    return c;
}

/* ---------------------------------------------------------------- 设置窗口
 *
 * 托盘菜单只能做快速设置，完整设置在下面这个带控件的窗口里：
 * 上半部是实时数值，下半部能选起始/截止百分比、勾选开机自启。
 * 关闭窗口 = 隐藏到托盘（程序继续驻留），不是退出。
 */

#define APP_CFG_CLASS   L"HwBatteryCfgWnd"
#define CFG_W           400
#define CFG_H           430

#define IDC_INFO        1000   /* 电池信息多行静态文本 */
#define IDC_LBL_MODE    1001   /* 当前 BIOS 阈值那行 */
#define IDC_CMB_START   1002
#define IDC_CMB_END     1003
#define IDC_CHK_AUTO    1004
#define IDC_BTN_APPLY   1005
#define IDC_BTN_REFRESH 1006
#define IDC_BTN_HIDE    1007
#define IDC_STATUS      1008

static void cfg_fill_combo(HWND cb, int from, int to)
{
    wchar_t t[16];
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int p = from; p <= to; p += 5) {
        swprintf(t, 16, L"%d%%", p);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    }
}

static int cfg_combo_get(HWND cb)
{
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i == CB_ERR)
        return -1;
    wchar_t t[16];
    SendMessageW(cb, CB_GETLBTEXT, (WPARAM)i, (LPARAM)t);
    return _wtoi(t);
}

static void cfg_combo_set(HWND cb, int pct)
{
    wchar_t t[16];
    swprintf(t, 16, L"%d%%", pct);
    int i = (int)SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)t);
    if (i != CB_ERR)
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)i, 0);
}

/* 把最新状态刷进窗口控件 */
static void cfg_refresh(HWND h)
{
    wchar_t buf[1024];

    if (g_state.ok) {
        swprintf(buf, 1024,
                 L"当前电量：%d%%（%s）\r\n"
                 L"当前容量：%lu mWh\r\n"
                 L"满充容量：%lu mWh\r\n"
                 L"出厂设计：%lu mWh\r\n"
                 L"健康度：%d%%\r\n"
                 L"循环次数：%lu\r\n"
                 L"电压：%lu mV\r\n"
                 L"化学体系：%hs",
                 g_state.percent, g_state.online ? L"已插电" : L"电池供电",
                 (unsigned long)g_state.current,
                 (unsigned long)g_state.full_charged,
                 (unsigned long)g_state.designed,
                 g_state.health,
                 (unsigned long)g_state.cycle_count,
                 (unsigned long)g_state.voltage_mv,
                 g_state.chemistry);
    } else {
        wcscpy(buf, L"未检测到电池设备");
    }
    SetWindowTextW(GetDlgItem(h, IDC_INFO), buf);

    if (g_state.threshold_valid) {
        cfg_combo_set(GetDlgItem(h, IDC_CMB_START), g_state.th_start);
        cfg_combo_set(GetDlgItem(h, IDC_CMB_END), g_state.th_end);
        swprintf(buf, 1024, L"BIOS 当前阈值：起始 %d%%   截止 %d%%",
                 g_state.th_start, g_state.th_end);
    } else {
        wcscpy(buf, L"BIOS 当前阈值：读取失败或本机未开放 HWMI");
    }
    {
        wchar_t extra[160];
        swprintf(extra, 160, L"\r\n配置：起始 %d%%  截止 %d%%（启动下发%s）",
                 g_cfg_start, g_cfg_end, g_cfg_applied ? L"成功" : L"失败");
        wcsncat(buf, extra, 1023 - wcslen(buf));
    }
    SetWindowTextW(GetDlgItem(h, IDC_LBL_MODE), buf);

    CheckDlgButton(h, IDC_CHK_AUTO, autostart_enabled() ? BST_CHECKED : BST_UNCHECKED);
}

static void cfg_status(HWND h, const wchar_t *text)
{
    SetWindowTextW(GetDlgItem(h, IDC_STATUS), text);
}

static void cfg_create_controls(HWND h)
{
    HWND cb;

    mkctl(L"STATIC", L"电池信息", 0, 14, 10, 360, 16, h, 0);
    mkctl(L"STATIC", L"", SS_LEFT, 14, 30, 360, 130, h, IDC_INFO);

    mkctl(L"STATIC", L"充电设置", 0, 14, 168, 360, 16, h, 0);

    mkctl(L"STATIC", L"起始充电低于此值才开始充：", 0, 14, 192, 220, 18, h, 0);
    cb = mkctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
               240, 190, 90, 200, h, IDC_CMB_START);
    cfg_fill_combo(cb, 0, 95);

    mkctl(L"STATIC", L"充电到此值停止：", 0, 14, 222, 220, 18, h, 0);
    cb = mkctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
               240, 220, 90, 200, h, IDC_CMB_END);
    cfg_fill_combo(cb, 5, 100);

    mkctl(L"BUTTON", L"开机自启", BS_AUTOCHECKBOX,
          14, 254, 160, 20, h, IDC_CHK_AUTO);

    mkctl(L"BUTTON", L"应用", BS_PUSHBUTTON, 14, 288, 100, 28, h, IDC_BTN_APPLY);
    mkctl(L"BUTTON", L"刷新", BS_PUSHBUTTON, 126, 288, 100, 28, h, IDC_BTN_REFRESH);
    mkctl(L"BUTTON", L"隐藏到托盘", BS_PUSHBUTTON,
          238, 288, 136, 28, h, IDC_BTN_HIDE);

    mkctl(L"STATIC", L"", SS_LEFT, 14, 322, 360, 60, h, IDC_LBL_MODE);
    mkctl(L"STATIC", L"", SS_LEFT, 14, 352, 360, 60, h, IDC_STATUS);
}

/* DPI 变化后按比例重排子控件：此刻各控件位置仍是旧 DPI 下的像素，
 * 乘以新旧 DPI 之比即得新位置；同时换上按新 DPI 重建的字体。 */
static BOOL CALLBACK dpi_rescale_child(HWND c, LPARAM lp)
{
    double ratio = *(double *)lp;
    RECT r;

    if (GetWindowRect(c, &r)) {
        POINT tl;
        tl.x = r.left;
        tl.y = r.top;
        ScreenToClient(GetParent(c), &tl);
        SetWindowPos(c, NULL,
                     (int)(tl.x * ratio + 0.5), (int)(tl.y * ratio + 0.5),
                     (int)((r.right - r.left) * ratio + 0.5),
                     (int)((r.bottom - r.top) * ratio + 0.5),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SendMessageW(c, WM_SETFONT, (WPARAM)ui_font(), TRUE);
    return TRUE;
}

static LRESULT CALLBACK CfgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        cfg_create_controls(h);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);

        switch (id) {
        case IDC_BTN_APPLY: {
            int s = cfg_combo_get(GetDlgItem(h, IDC_CMB_START));
            int e = cfg_combo_get(GetDlgItem(h, IDC_CMB_END));
            if (s < 0 || e < 0) {
                cfg_status(h, L"请先选择起始与截止百分比。");
                break;
            }
            if (s > e) {
                cfg_status(h, L"起始不能大于截止，请重新选择。");
                break;
            }
            if (apply_threshold(s, e)) {
                cfg_refresh(h);
                cfg_status(h, L"已下发，并回读确认成功。");
            } else {
                cfg_status(h, L"下发失败：本程序已是管理员权限，"
                              L"通常是本机 BIOS 未开放 HWMI 接口。");
            }
            break;
        }

        case IDC_BTN_REFRESH:
            refresh_state();
            cfg_refresh(h);
            cfg_status(h, L"已刷新。");
            break;

        case IDC_BTN_HIDE:
            ShowWindow(h, SW_HIDE);
            break;

        case IDC_CHK_AUTO: {
            BOOL want = IsDlgButtonChecked(h, IDC_CHK_AUTO) == BST_CHECKED;
            if (!autostart_set(want)) {
                cfg_status(h, L"开机自启写入失败（注册表操作被拒）。");
                /* 写失败就把勾选状态回滚，别让它显示成已生效 */
                CheckDlgButton(h, IDC_CHK_AUTO,
                               want ? BST_UNCHECKED : BST_CHECKED);
            } else {
                cfg_status(h, want ? L"已设置开机自启。" : L"已取消开机自启。");
            }
            break;
        }

        default:
            break;
        }
        return 0;
    }

    case WM_DPICHANGED: {
        /* 拖到不同缩放的显示器：系统给出新 DPI 与建议位置。
         * 顺序要紧——先更新 g_dpi，再按新旧之比缩放子控件（此刻它们还是旧值），
         * 最后用新 DPI 重算客户区尺寸。 */
        int old = g_dpi;
        RECT *sr = (RECT *)lp;
        const DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        double ratio;
        RECT rc;

        g_dpi = LOWORD(wp);
        if (g_dpi < 96)
            g_dpi = 96;

        if (g_ui_font) {              /* 字体随 DPI 变化，重建 */
            DeleteObject(g_ui_font);
            g_ui_font = NULL;
        }

        ratio = (old > 0) ? (double)g_dpi / (double)old : 1.0;
        EnumChildWindows(h, dpi_rescale_child, (LPARAM)&ratio);

        rc.left = 0;
        rc.top = 0;
        rc.right = S(CFG_W);
        rc.bottom = S(CFG_H);
        AdjustWindowRectEx(&rc, st, FALSE, 0);
        SetWindowPos(h, NULL, sr ? sr->left : 0, sr ? sr->top : 0,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);   /* 关窗只是隐藏，程序继续驻留托盘 */
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 显示设置窗口（不存在就创建） */
static void cfg_show(void)
{
    if (!g_cfg) {
        /* 用 AdjustWindowRectEx 反推窗口尺寸，保证**客户区**正好是
         * S(CFG_W) x S(CFG_H)。直接给总高会让标题栏吃掉一部分高度，
         * 底部那行状态文字会被裁掉。 */
        const DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT r = { 0, 0, S(CFG_W), S(CFG_H) };
        AdjustWindowRectEx(&r, st, FALSE, 0);

        g_cfg = CreateWindowExW(0, APP_CFG_CLASS, L"华为笔记本充电保护", st,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_cfg)
            return;
    }
    refresh_state();
    cfg_refresh(g_cfg);
    ShowWindow(g_cfg, SW_SHOW);
    SetForegroundWindow(g_cfg);
}

/* ---------------------------------------------------------------- 命令行模式
 *
 * 开机自启也能从命令行设置/取消，便于脚本化与验证：
 *     hwbattery.exe --autostart=on      设置开机自启
 *     hwbattery.exe --autostart=off     取消开机自启
 *     hwbattery.exe --autostart=query   查询当前状态
 * 带该开关时不进托盘，附着到调用者控制台打印结果后立即退出。
 */

/* 从命令行里找 --autostart=<值>，找到返回其值，否则返回 NULL。 */
static const wchar_t *cli_find_autostart(void)
{
    static wchar_t val[32];
    const wchar_t *key = L"--autostart=";
    size_t klen = wcslen(key);

    for (const wchar_t *q = GetCommandLineW(); (q = wcsstr(q, key)) != NULL; q += klen) {
        /* 必须是独立参数，不能是别的参数的一部分 */
        if (q != GetCommandLineW() && q[-1] != L' ' && q[-1] != L'\t' && q[-1] != L'"')
            continue;

        const wchar_t *v = q + klen;
        size_t n = 0;
        while (v[n] && v[n] != L' ' && v[n] != L'\t' && v[n] != L'"' && n < 31) {
            val[n] = v[n];
            n++;
        }
        val[n] = L'\0';
        return val;
    }
    return NULL;
}

/* 命令行模式的结果同时追加到日志文件，这样即使没有控制台（双击运行、
 * 或被脚本以管道方式调用）也能确认到底执行成没成。
 * 日志路径: %TEMP%\hwbattery-autostart.log（UTF-8） */
static void cli_log(const wchar_t *msg)
{
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp))
        return;
    swprintf(path, MAX_PATH, L"%s\\hwbattery-autostart.log", tmp);

    char utf8[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 0)
        return;

    FILE *f = _wfopen(path, L"ab");
    if (!f)
        return;
    fwrite(utf8, 1, (size_t)(n - 1), f);   /* 不含结尾 NUL */
    fwrite("\r\n", 1, 2, f);
    fclose(f);
}

/* 处理 --autostart 开关，返回进程退出码（0 = 成功）。
 * 刻意不用 AllocConsole/freopen("CONOUT$")：那会把输出强行送进控制台设备，
 * 从脚本调用时反而抓不到，还可能闪出一个控制台窗口。 */
static int cli_autostart(const wchar_t *value)
{
    wchar_t msg[256];
    int rc;

    if (_wcsicmp(value, L"query") == 0) {
        swprintf(msg, 256, L"开机自启：%s", autostart_enabled() ? L"已启用" : L"未启用");
        rc = 0;
    } else if (_wcsicmp(value, L"on") == 0) {
        if (!autostart_set(TRUE)) {
            swprintf(msg, 256, L"[!] 设置开机自启失败（注册表不可写？）");
            rc = 1;
        } else {
            BOOL ok = autostart_enabled();
            swprintf(msg, 256, L"开机自启已设置：%s",
                     ok ? L"已回读确认" : L"但回读未找到该项");
            rc = ok ? 0 : 1;
        }
    } else if (_wcsicmp(value, L"off") == 0) {
        if (!autostart_set(FALSE)) {
            swprintf(msg, 256, L"[!] 取消开机自启失败");
            rc = 1;
        } else {
            BOOL still = autostart_enabled();
            swprintf(msg, 256, L"开机自启已取消：%s",
                     still ? L"但该项仍然存在" : L"已回读确认已删除");
            rc = still ? 1 : 0;
        }
    } else {
        swprintf(msg, 256, L"用法: hwbattery.exe --autostart=on|off|query");
        rc = 2;
    }

    AttachConsole(ATTACH_PARENT_PROCESS);   /* 从 cmd 调用时能顺带打在控制台上 */
    wprintf(L"%s\n", msg);
    cli_log(msg);
    return rc;
}

/* --query：命令行读取一次并输出（同时写日志）。
 * 走的是和设置窗口完全相同的数据路径，因此也可用来验证 GUI 显示是否正常。 */
static int cli_query(void)
{
    wchar_t th[32], msg[512];

    refresh_state();

    if (g_state.threshold_valid)
        swprintf(th, 32, L"%d%%-%d%%", g_state.th_start, g_state.th_end);
    else
        wcscpy(th, L"读取失败");

    if (g_state.ok) {
        swprintf(msg, 512,
                 L"电量 %d%%  健康 %d%%  设计 %lu / 满充 %lu mWh  "
                 L"循环 %lu  阈值 %s  DPI %d",
                 g_state.percent, g_state.health,
                 (unsigned long)g_state.designed,
                 (unsigned long)g_state.full_charged,
                 (unsigned long)g_state.cycle_count, th, g_dpi);
    } else {
        wcscpy(msg, L"[!] 未检测到电池设备");
    }

    AttachConsole(ATTACH_PARENT_PROCESS);
    wprintf(L"%s\n", msg);
    cli_log(msg);
    return (g_state.ok && g_state.threshold_valid) ? 0 : 1;
}

/* ---------------------------------------------------------------- 入口 */

/* 应用图标来自资源里的多尺寸 ICO（id 1，见 hwbattery.rc）。
 * 按目标尺寸加载，避免托盘 16x16 拿到 256 的大图再缩放而发虚。
 * 资源缺失时退回系统通用图标，不影响启动。 */
#define IDI_APPICON 1

static HICON app_icon(int cx, int cy)
{
    HINSTANCE hi = GetModuleHandleW(NULL);
    HICON ic = NULL;

    if (cx > 0 && cy > 0)
        ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                               cx, cy, LR_DEFAULTCOLOR);
    if (!ic)
        ic = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    if (!ic)
        ic = LoadIconW(NULL, IDI_APPLICATION);
    return ic;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    (void)hPrev;
    (void)cmdLine;
    (void)nShow;

    /* 先把 DPI 感知声明掉（进程级设置），再取系统 DPI。必须在建窗口之前，
     * 且顺序不可颠倒：未声明时 GetDeviceCaps 返回的是被虚拟化过的值。 */
    SetProcessDPIAware();
    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(NULL, hdc);
        }
        if (g_dpi < 96)
            g_dpi = 96;
    }

    /* 命令行模式：处理完开关就退出，不进托盘 */
    {
        const wchar_t *av = cli_find_autostart();
        if (av)
            return cli_autostart(av);
        if (wcsstr(GetCommandLineW(), L"--query"))
            return cli_query();
    }

    /* --show：启动后直接弹出设置窗口，便于做快捷方式 */
    BOOL want_show = (wcsstr(GetCommandLineW(), L"--show") != NULL);

    /* 单实例：托常量驻程序不允许跑两份。
     * 第二个进程直接退出——不弹提示框、也不去唤出已有实例的窗口。 */
    HANDLE once = CreateMutexW(NULL, TRUE, L"HwBatteryTray_SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(once);
        return 0;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_CLASS;
    wc.hIcon = app_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!RegisterClassExW(&wc))
        return 1;

    /* 设置窗口的窗口类也必须先注册，cfg_show 才能把它创建出来 */
    WNDCLASSEXW wc2;
    ZeroMemory(&wc2, sizeof(wc2));
    wc2.cbSize = sizeof(wc2);
    wc2.lpfnWndProc = CfgProc;
    wc2.hInstance = hInst;
    wc2.lpszClassName = APP_CFG_CLASS;
    wc2.hIcon = app_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc2.hIconSm = app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc2.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc2))
        return 1;

    /* 一个不显示的窗口，仅用于接收托盘消息与承载弹出菜单 */
    HWND hwnd = CreateWindowExW(0, APP_CLASS, APP_TITLE, WS_OVERLAPPED,
                                0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon = app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wcsncpy(g_nid.szTip, APP_TITLE, 127);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        MessageBoxW(NULL, L"添加托盘图标失败。", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    /* 读配置并把"期望阈值"下发一次。失败只写日志、不弹窗——
     * 开机自启每次登录都弹提示会很烦。 */
    {
        wchar_t msg[256];
        int s = CFG_DEF_START, e = CFG_DEF_END;

        if (cfg_load(&s, &e)) {
            g_cfg_start = s;
            g_cfg_end   = e;
            g_cfg_applied = hwmi_set_threshold(s, e);
            swprintf(msg, 256, g_cfg_applied
                     ? L"启动下发配置成功：起始 %d%%  截止 %d%%"
                     : L"[!] 启动下发配置失败（起始 %d%% 截止 %d%%）",
                     s, e);
            cli_log(msg);
        }
    }

    refresh_state();
    SetTimer(hwnd, ID_TIMER, REFRESH_MS, NULL);

    if (want_show)
        cfg_show();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (once)
        CloseHandle(once);
    return (int)msg.wParam;
}
