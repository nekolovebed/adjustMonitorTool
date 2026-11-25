#include "brightness.h"

#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <combaseapi.h>

#include <algorithm>
#include <cwchar>

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static std::vector<CtDisplay> g_disp;

struct PhysGroup {
    DWORD n = 0;
    std::vector<PHYSICAL_MONITOR> arr;
};
static std::vector<PhysGroup> g_groups;

// 显示器友好名 / 设备路径（来自 DisplayConfig）
struct NameEntry {
    std::wstring gdi;
    std::wstring friendly;
    std::wstring devPath;
};
static std::vector<NameEntry> g_names;

// WMI（笔记本内屏）
static bool g_wmiInitTried = false;
static IWbemServices* g_wmi = nullptr;
static std::wstring g_internalToken;
static bool g_internalTokenTried = false;

// 上次写入时间：写入后短时间内不回读，避免滑块被旧值顶回去
static DWORD g_lastSetTick = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static std::wstring MonToken(const std::wstring& path) {
    std::wstring up = path;
    for (auto& c : up) c = (wchar_t)towupper(c);
    size_t p = up.find(L"DISPLAY");
    if (p == std::wstring::npos) return L"";
    p += 7;
    while (p < up.size() && (up[p] == L'\\' || up[p] == L'#' || up[p] == L'?' || up[p] == L'&')) ++p;
    size_t e = p;
    while (e < up.size() && up[e] != L'\\' && up[e] != L'#' && up[e] != L'_') ++e;
    if (e <= p) return L"";
    return up.substr(p, e - p);
}

static void CollectFriendlyNames() {
    g_names.clear();
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return;
    paths.resize(pathCount);
    for (auto& p : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;

        NameEntry e;
        e.gdi = sn.viewGdiDeviceName;
        e.friendly = tn.monitorFriendlyDeviceName;
        e.devPath = tn.monitorDevicePath;
        g_names.push_back(e);
    }
}

static std::wstring FriendlyFor(const std::wstring& gdi) {
    for (auto& n : g_names)
        if (n.gdi == gdi && !n.friendly.empty()) return n.friendly;
    return L"";
}

static std::wstring DevPathFor(const std::wstring& gdi) {
    for (auto& n : g_names)
        if (n.gdi == gdi) return n.devPath;
    return L"";
}

// ---------------------------------------------------------------------------
// DDC/CI（外接显示器）
// ---------------------------------------------------------------------------
static bool DdcRead(HANDLE h, int& cur, int& mx) {
    if (!h) return false;
    // 显示器忙/刚唤醒时读取会超时，多试几次
    for (int attempt = 0; attempt < 6; ++attempt) {
        DWORD c = 0, m = 0;
        if (GetVCPFeatureAndVCPFeatureReply(h, 0x10, nullptr, &c, &m) && m > 0) {
            cur = (int)c;
            mx = (int)m;
            return true;
        }
        Sleep(60);
    }
    DWORD mn = 0, c = 0, m = 0;
    if (GetMonitorBrightness(h, &mn, &c, &m) && m > mn) {
        cur = (int)c;
        mx = (int)m;
        return true;
    }
    return false;
}

static bool DdcWrite(HANDLE h, int v) {
    if (!h) return false;
    if (SetVCPFeature(h, 0x10, (DWORD)v)) return true;
    return SetMonitorBrightness(h, (DWORD)v) != FALSE;
}

// ---------------------------------------------------------------------------
// WMI（笔记本内屏）
// ---------------------------------------------------------------------------
static bool WmiEnsure() {
    if (g_wmiInitTried) return g_wmi != nullptr;
    g_wmiInitTried = true;
    IWbemLocator* loc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                  reinterpret_cast<void**>(&loc));
    if (FAILED(hr) || !loc) return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_wmi);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr) || !g_wmi) {
        g_wmi = nullptr;
        return false;
    }
    CoSetProxyBlanket(g_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

static IWbemClassObject* WmiFirstInstance(const wchar_t* query, IEnumWbemClassObject** keepEnum) {
    if (keepEnum) *keepEnum = nullptr;
    if (!WmiEnsure()) return nullptr;
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(query);
    HRESULT hr = g_wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !en) return nullptr;
    IWbemClassObject* obj = nullptr;
    ULONG got = 0;
    hr = en->Next(WBEM_INFINITE, 1, &obj, &got);
    if (FAILED(hr) || got != 1 || !obj) {
        en->Release();
        return nullptr;
    }
    if (keepEnum) *keepEnum = en;
    else en->Release();
    return obj;
}

static bool WmiReadBrightness(int& cur) {
    IEnumWbemClassObject* en = nullptr;
    IWbemClassObject* obj = WmiFirstInstance(L"SELECT CurrentBrightness FROM WmiMonitorBrightness", &en);
    if (!obj) return false;
    bool ok = false;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) && (v.vt == VT_UI1 || v.vt == VT_I4)) {
        cur = (v.vt == VT_UI1) ? (int)v.bVal : (int)v.lVal;
        ok = true;
    }
    VariantClear(&v);
    obj->Release();
    en->Release();
    return ok;
}

static bool WmiWriteBrightness(int value) {
    IEnumWbemClassObject* en = nullptr;
    IWbemClassObject* obj = WmiFirstInstance(L"SELECT * FROM WmiMonitorBrightnessMethods", &en);
    if (!obj) return false;

    IWbemClassObject* cls = nullptr;
    IWbemClassObject* inSig = nullptr;
    IWbemClassObject* inInst = nullptr;
    bool ok = false;

    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    HRESULT hcls = g_wmi->GetObject(clsName, 0, nullptr, &cls, nullptr);
    SysFreeString(clsName);
    if (SUCCEEDED(hcls) && cls) {
        HRESULT hgm = cls->GetMethod(L"WmiSetBrightness", 0, &inSig, nullptr);
        if (SUCCEEDED(hgm) && inSig) {
            HRESULT hsp = inSig->SpawnInstance(0, &inInst);
            if (SUCCEEDED(hsp) && inInst) {
                VARIANT path;
                VariantInit(&path);
                HRESULT hgp = obj->Get(L"__PATH", 0, &path, nullptr, nullptr);
                if (SUCCEEDED(hgp) && path.vt == VT_BSTR) {
                    VARIANT timeout;
                    VariantInit(&timeout);
                    timeout.vt = VT_I4;
                    timeout.lVal = 0;
                    inInst->Put(L"Timeout", 0, &timeout, CIM_UINT32);

                    VARIANT bright;
                    VariantInit(&bright);
                    bright.vt = VT_UI1;
                    bright.bVal = (BYTE)value;
                    inInst->Put(L"Brightness", 0, &bright, CIM_UINT8);

                    IWbemClassObject* out = nullptr;
                    BSTR method = SysAllocString(L"WmiSetBrightness");
                    HRESULT hr = g_wmi->ExecMethod(path.bstrVal, method, 0, nullptr, inInst, &out, nullptr);
                    SysFreeString(method);
                    if (SUCCEEDED(hr)) ok = true;
                    if (out) out->Release();
                }
                VariantClear(&path);
            }
        }
    }
    if (inInst) inInst->Release();
    if (inSig) inSig->Release();
    if (cls) cls->Release();
    obj->Release();
    en->Release();
    return ok;
}

// 内置屏在 WMI 里的实例名（DISPLAY\BOE0B69\...），用于精确识别内屏
static const std::wstring& InternalToken() {
    if (g_internalTokenTried) return g_internalToken;
    g_internalTokenTried = true;
    IEnumWbemClassObject* en = nullptr;
    IWbemClassObject* obj = WmiFirstInstance(L"SELECT InstanceName FROM WmiMonitorBrightness", &en);
    if (obj) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"InstanceName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
            g_internalToken = MonToken(v.bstrVal);
        VariantClear(&v);
        obj->Release();
    }
    if (en) en->Release();
    if (g_internalToken.empty()) g_internalTokenTried = false;   // 下次刷新再试
    return g_internalToken;
}

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------
struct EnumCtx {
    int seq = 1;
};

static void AddPlain(const std::wstring& gdi, const std::wstring& name, const std::wstring& devPath) {
    CtDisplay d;
    d.key = gdi;
    d.name = name;
    d.devPath = devPath;
    d.gdi = gdi;
    d.min = 0;
    d.max = 100;
    d.cur = 50;
    g_disp.push_back(d);
}

static BOOL CALLBACK MonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, reinterpret_cast<MONITORINFO*>(&mi))) return TRUE;
    std::wstring gdi = mi.szDevice;

    std::wstring name = FriendlyFor(gdi);
    if (name.empty()) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesW(gdi.c_str(), 0, &dd, 0) && dd.DeviceString[0]) name = dd.DeviceString;
    }
    if (name.empty()) name = L"显示器 " + std::to_wstring(ctx->seq);
    std::wstring devPath = DevPathFor(gdi);
    ctx->seq++;

    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) {
        AddPlain(gdi, name, devPath);
        return TRUE;
    }

    // 这台机器的 DDC 驱动第一次调用常常返回空句柄，第二次才给有效句柄，
    // 所以直接重复调用（不要在中途 Destroy，会把状态清掉）。
    PhysGroup g;
    g.n = count;
    g.arr.resize(count);
    BOOL got = FALSE;
    for (int attempt = 0; attempt < 5; ++attempt) {
        got = GetPhysicalMonitorsFromHMONITOR(hMon, count, g.arr.data());
        if (got && g.arr[0].hPhysicalMonitor) break;
        Sleep(40);
    }
    if (!got || !g.arr[0].hPhysicalMonitor) {
        AddPlain(gdi, name, devPath);
        return TRUE;
    }

    for (DWORD i = 0; i < count; ++i) {
        CtDisplay d;
        d.key = gdi + L"#" + std::to_wstring(i);
        d.name = name;
        d.devPath = devPath;
        d.gdi = gdi;
        d.phys = g.arr[i].hPhysicalMonitor;
        d.controllable = d.phys != nullptr;
        d.min = 0;
        d.max = 100;
        d.cur = 50;
        int cur = 0, mx = 100;
        if (d.phys && DdcRead(d.phys, cur, mx)) {
            d.max = mx;
            d.cur = std::clamp(cur, 0, mx);
        }
        g_disp.push_back(d);
    }
    g_groups.push_back(std::move(g));
    return TRUE;
}

void BrightRefresh() {
    for (auto& g : g_groups) {
        if (g.n && !g.arr.empty()) DestroyPhysicalMonitors(g.n, g.arr.data());
    }
    g_groups.clear();
    g_disp.clear();

    CollectFriendlyNames();
    EnumCtx ctx;
    EnumDisplayMonitors(nullptr, nullptr, MonProc, reinterpret_cast<LPARAM>(&ctx));

    // 笔记本内屏：即使它也能开出 DDC 句柄，也一律走 WMI（内屏的 DDC 写入通常无效）
    const std::wstring& tok = InternalToken();
    if (!tok.empty()) {
        for (auto& d : g_disp) {
            if (MonToken(d.devPath) != tok) continue;
            int cur = 0;
            if (!WmiReadBrightness(cur)) continue;
            d.internal = true;
            d.controllable = true;
            d.min = 0;
            d.max = 100;
            d.cur = std::clamp(cur, 0, 100);
        }
    }
}

void BrightInit() { BrightRefresh(); }

void BrightShutdown() {
    for (auto& g : g_groups) {
        if (g.n && !g.arr.empty()) DestroyPhysicalMonitors(g.n, g.arr.data());
    }
    g_groups.clear();
    g_disp.clear();
    if (g_wmi) {
        g_wmi->Release();
        g_wmi = nullptr;
    }
}

const std::vector<CtDisplay>& BrightList() { return g_disp; }

bool BrightSet(int index, int value) {
    if (index < 0 || index >= (int)g_disp.size()) return false;
    auto& d = g_disp[index];
    if (!d.controllable) return false;
    value = std::clamp(value, d.min, d.max);

    g_lastSetTick = GetTickCount();
    bool ok = d.internal ? WmiWriteBrightness(value) : DdcWrite(d.phys, value);
    if (ok) d.cur = value;
    return ok;
}

bool BrightRead(int index, int& value) {
    if (index < 0 || index >= (int)g_disp.size()) return false;
    auto& d = g_disp[index];
    if (!d.controllable) return false;
    if (GetTickCount() - g_lastSetTick < 1500) return false;   // 刚写过，别把滑块顶回去

    int cur = 0, mx = 100;
    if (d.internal) {
        if (!WmiReadBrightness(cur)) return false;
    } else {
        if (!DdcRead(d.phys, cur, mx)) return false;
        d.max = mx;
    }
    value = std::clamp(cur, 0, mx);
    d.cur = value;
    return true;
}

int BrightPercent(int index) {
    if (index < 0 || index >= (int)g_disp.size()) return 0;
    const auto& d = g_disp[index];
    if (d.max <= d.min) return 0;
    return (int)((d.cur - d.min) * 100 / (d.max - d.min));
}
