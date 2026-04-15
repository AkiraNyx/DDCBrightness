/*
 * DDCBrightness 用户态服务 - 头文件
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxva2api.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include "../common/public.h"

//
// 服务名称
//
#define SERVICE_NAME        L"DDCBrightnessService"
#define SERVICE_DISPLAY     L"DDC/CI Brightness Control Service"
#define SERVICE_DESCRIPTION L"通过 DDC/CI 协议控制外置显示器亮度，配合 DDCBrightness 驱动使 Windows 亮度滑块可用"

//
// 单个物理显示器的 DDC/CI 信息
//
struct MonitorDDCInfo {
    bool active;
    DWORD index;                    // 在驱动中的索引
    HMONITOR hLogicalMonitor;       // 逻辑显示器句柄
    HANDLE hPhysicalMonitor;        // 物理显示器句柄 (DDC/CI)
    WCHAR deviceName[64];           // 设备名称
    WCHAR description[128];         // 显示器描述
    DWORD minBrightness;            // DDC/CI 最小亮度
    DWORD maxBrightness;            // DDC/CI 最大亮度
    DWORD currentBrightness;        // DDC/CI 当前亮度 (原始值)
};

//
// 亮度映射: Windows 百分比 <-> DDC/CI 原始值
//
inline DWORD MapPercentToDDC(DWORD percent, DWORD minVal, DWORD maxVal)
{
    if (percent > 100) percent = 100;
    if (maxVal <= minVal) return minVal;
    return minVal + (percent * (maxVal - minVal)) / 100;
}

inline DWORD MapDDCToPercent(DWORD value, DWORD minVal, DWORD maxVal)
{
    if (maxVal <= minVal) return 0;
    if (value < minVal) return 0;
    if (value > maxVal) return 100;
    return ((value - minVal) * 100) / (maxVal - minVal);
}
