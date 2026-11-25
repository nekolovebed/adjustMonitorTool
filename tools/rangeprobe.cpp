#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>

static PHYSICAL_MONITOR g_pm[8];
static DWORD g_n = 0;

static BOOL CALLBACK MonProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &n) || !n) return TRUE;
    PHYSICAL_MONITOR arr[8];
    if (GetPhysicalMonitorsFromHMONITOR(hMon, n > 8 ? 8 : n, arr))
        for (DWORD i = 0; i < n && g_n < 8; ++i) g_pm[g_n++] = arr[i];
    return TRUE;
}

int main() {
    EnumDisplayMonitors(nullptr, nullptr, MonProc, 0);
    for (DWORD i = 0; i < g_n; ++i) {
        HANDLE h = g_pm[i].hPhysicalMonitor;
        printf("[%lu] handle=%p desc=%ls\n", i, (void*)h, g_pm[i].szPhysicalMonitorDescription);

        DWORD mn = 0, cur = 0, mx = 0;
        BOOL ok = GetMonitorBrightness(h, &mn, &cur, &mx);
        printf("   GetMonitorBrightness ok=%d min=%lu cur=%lu max=%lu\n", ok, ok ? mn : 0, ok ? cur : 0, ok ? mx : 0);

        DWORD c2 = 0, m2 = 0; 
        BOOL ok2 = GetVCPFeatureAndVCPFeatureReply(h, 0x10, nullptr, &c2, &m2);
        printf("   VCP 0x10 ok=%d cur=%lu max=%lu \n", ok2, ok2 ? c2 : 0, ok2 ? m2 : 0);

        DWORD caps = 0, ct = 0;
        BOOL ok3 = GetMonitorCapabilities(h, &caps, &ct);
        printf("   GetMonitorCapabilities ok=%d caps=0x%08lX err=%lu\n", ok3, ok3 ? caps : 0, ok3 ? 0 : GetLastError());
    }
    return 0;
}
