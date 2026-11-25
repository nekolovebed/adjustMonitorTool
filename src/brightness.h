#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <string>
#include <vector>

// 一个可调光的显示器条目
struct CtDisplay {
    std::wstring key;   // 唯一标识
    std::wstring name;  // 友好名称，如 "H25T7-2"
    std::wstring sub;   // 副标题 / 不支持原因
    std::wstring devPath;    // 显示器设备路径（用于识别内置屏）
    std::wstring gdi;        // GDI 设备名，如 \\.\DISPLAY1（软件调光用）
    HANDLE phys = nullptr;   // DDC/CI 物理显示器句柄
    int min = 0;
    int max = 100;
    int cur = 50;
    bool controllable = false;
    bool internal = false;   // 是否为内置屏（走 WMI）
};

void BrightInit();
void BrightRefresh();
void BrightShutdown();
const std::vector<CtDisplay>& BrightList();
bool BrightSet(int index, int value);   // 立即写入
bool BrightRead(int index, int& value);
int  BrightPercent(int index);
