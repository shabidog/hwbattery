/* wmi_probe.c —— HWMI(0x1103) 的 WMI 调用逐步诊断
 *
 * 第一轮结论：连接/查询/Get(__PATH) 全部成功，但
 *     GetMethod(OemWMIfun) on INSTANCE  -> hr=0x8004101E (WBEM_E_ILLEGAL_OPERATION)
 *
 * 第二轮目的：对比两种取方法签名的途径，确定哪一种能被这个动态 provider 接受：
 *   (a) 在 ExecQuery 拿到的实例上 GetMethod            —— 已知失败
 *   (b) 先 GetObject 取类对象，再在类上 GetMethod      —— 待验证
 *   (c) 若 (b) 可行，进一步验证从类定义 SpawnInstance -> Put -> ExecMethod 全链路
 *
 * 刻意全部使用 ASCII 提示：宽字符经管道/控制台代码页转换会被打成 '?'，
 * 纯 ASCII 才能保证输出可读。
 *
 * 编译（控制台子系统，输出可直接被管道捕获）：
 *   gcc -O2 -o wmi_probe.exe wmi_probe.c -lole32 -loleaut32 -luuid -lwbemuuid
 */

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <wbemidl.h>

#define OKFAIL(h) (SUCCEEDED(h) ? L"OK" : L"<<< FAILED")

int main(void)
{
    HRESULT hr;
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *inst = NULL, *cls = NULL;
    IWbemClassObject *inCls = NULL, *inInst = NULL, *outInst = NULL;
    ULONG got = 0;
    BSTR path = NULL;
    VARIANT v;

    wprintf(L"=== HWMI(0x1103) WMI probe round 2 ===\n");

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&loc);
    wprintf(L"  CoCreateInstance(WbemLocator)        hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));
    if (FAILED(hr)) goto done;

    hr = loc->lpVtbl->ConnectServer(loc, L"root\\WMI", NULL, NULL, NULL, 0,
                                    NULL, NULL, &svc);
    wprintf(L"  ConnectServer(root\\WMI)              hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));
    if (FAILED(hr)) goto done;

    CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                      NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    hr = svc->lpVtbl->ExecQuery(svc, L"WQL", L"SELECT * FROM OemWMIMethod",
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                NULL, &en);
    wprintf(L"  ExecQuery                            hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));
    if (FAILED(hr)) goto done;

    hr = en->lpVtbl->Next(en, WBEM_INFINITE, 1, &inst, &got);
    wprintf(L"  EumNext (got=%lu)                     hr=0x%08lX %s\n",
            (unsigned long)got, (unsigned)hr, OKFAIL(hr));
    if (FAILED(hr) || got == 0) goto done;

    VariantInit(&v);
    hr = inst->lpVtbl->Get(inst, L"__PATH", 0, &v, NULL, NULL);
    wprintf(L"  Get(__PATH)                          hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));
    if (FAILED(hr) || v.vt != VT_BSTR) goto done;
    path = v.bstrVal;
    wprintf(L"  path = %s\n", path);

    /* ---------- (a) 实例上取方法：已知失败，复现一次作对照 ---------- */
    {
        IWbemClassObject *tmp = NULL;
        HRESULT ha = inst->lpVtbl->GetMethod(inst, L"OemWMIfun", 0, &tmp, NULL);
        wprintf(L"\n  (a) GetMethod on INSTANCE            hr=0x%08lX %s\n",
                (unsigned)ha, OKFAIL(ha));
        if (tmp) tmp->lpVtbl->Release(tmp);
    }

    /* ---------- (b) 类对象上取方法 ---------- */
    wprintf(L"\n  -- trying (b): class-based GetMethod --\n");
    hr = svc->lpVtbl->GetObject(svc, L"OemWMIMethod", 0, NULL, &cls, NULL);
    wprintf(L"  (b1) GetObject(OemWMIMethod)         hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));

    if (SUCCEEDED(hr) && cls) {
        /* 类上是否存在该方法名 */
        VARIANT vm;
        VariantInit(&vm);
        HRESULT hm = cls->lpVtbl->Get(cls, L"__METHOD", 0, &vm, NULL, NULL);
        wprintf(L"  (b2) class Get(__METHOD)             hr=0x%08lX\n", (unsigned)hm);
        VariantClear(&vm);

        HRESULT hb = cls->lpVtbl->GetMethod(cls, L"OemWMIfun", 0, &inCls, NULL);
        wprintf(L"  (b3) GetMethod on CLASS              hr=0x%08lX %s\n",
                (unsigned)hb, OKFAIL(hb));

        /* 若类方式也不行，试试直接在类实例上按方法名枚举 */
        if (FAILED(hb)) {
            wprintf(L"  (b4) 类方式也失败，尝试不用方法签名直接 ExecMethod ...\n");
            inCls = NULL;
        }
    }

    /* ---------- (c) 用可用的 inCls 走完 Put -> ExecMethod ---------- */
    if (inCls) {
        hr = inCls->lpVtbl->SpawnInstance(inCls, 0, &inInst);
        wprintf(L"  (c1) SpawnInstance                   hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));
    }

    if (inInst) {
        VARIANT in;
        wchar_t buf[32];
        VariantInit(&in);
        swprintf(buf, 32, L"%llu", 4355ULL);   /* 4355 = 0x1103 */
        in.vt = VT_BSTR;
        in.bstrVal = SysAllocString(buf);
        hr = inInst->lpVtbl->Put(inInst, L"u64Input", 0, &in, 0);
        wprintf(L"  (c2) Put u64Input as VT_BSTR         hr=0x%08lX %s\n",
                (unsigned)hr, OKFAIL(hr));
        VariantClear(&in);
        if (FAILED(hr)) {
            VariantInit(&in);
            in.vt = VT_UI8;
            in.ullVal = 4355ULL;
            hr = inInst->lpVtbl->Put(inInst, L"u64Input", 0, &in, 0);
            wprintf(L"  (c2') Put u64Input as VT_UI8         hr=0x%08lX %s\n",
                    (unsigned)hr, OKFAIL(hr));
            VariantClear(&in);
        }
    }

    /* ExecMethod：即使没有 inInst 也试一次，看 provider 是否接受 NULL 入参 */
    hr = svc->lpVtbl->ExecMethod(svc, path, L"OemWMIfun", 0, NULL, inInst,
                                 &outInst, NULL);
    wprintf(L"  (c3) ExecMethod                      hr=0x%08lX %s\n", (unsigned)hr, OKFAIL(hr));

    if (SUCCEEDED(hr) && outInst) {
        VARIANT o;
        VariantInit(&o);
        hr = outInst->lpVtbl->Get(outInst, L"u8Output", 0, &o, NULL, NULL);
        wprintf(L"  (c4) Get(u8Output)                   hr=0x%08lX %s  vt=0x%04X\n",
                (unsigned)hr, OKFAIL(hr), (unsigned)o.vt);
        if (SUCCEEDED(hr) && (o.vt & VT_ARRAY) && o.parray) {
            BYTE *d = NULL;
            if (SUCCEEDED(SafeArrayAccessData(o.parray, (void **)&d)) && d) {
                wprintf(L"  first 16 bytes:");
                for (int i = 0; i < 16; i++)
                    wprintf(L" %02X", d[i]);
                wprintf(L"\n  status byte = 0x%02X\n", d[0]);
                SafeArrayUnaccessData(o.parray);
            }
        }
        VariantClear(&o);
    }

done:
    if (loc) loc->lpVtbl->Release(loc);
    if (svc) svc->lpVtbl->Release(svc);
    if (en) en->lpVtbl->Release(en);
    if (inst) inst->lpVtbl->Release(inst);
    if (cls) cls->lpVtbl->Release(cls);
    if (inCls) inCls->lpVtbl->Release(inCls);
    if (inInst) inInst->lpVtbl->Release(inInst);
    if (outInst) outInst->lpVtbl->Release(outInst);
    if (path) SysFreeString(path);
    wprintf(L"=== probe done ===\n");
    return 0;
}
