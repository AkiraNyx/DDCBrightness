/*
 * DDC/CI 测试工具
 * 用于验证外置显示器的 DDC/CI 可用性和亮度范围
 * 编译: cl /EHsc ddc_test.cpp /link dxva2.lib user32.lib
 */

#include <windows.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")

//
// 亮度映射辅助函数
//
static DWORD MapDDCToPercent(DWORD value, DWORD minVal, DWORD maxVal)
{
    if (maxVal <= minVal) return 0;
    return ((value - minVal) * 100) / (maxVal - minVal);
}

static DWORD MapPercentToDDC(DWORD percent, DWORD minVal, DWORD maxVal)
{
    if (percent > 100) percent = 100;
    return minVal + (percent * (maxVal - minVal)) / 100;
}

//
// 全局存储，用于 --set 模式
//
static DWORD g_targetBrightness = 0;

//
// 测试单个显示器的 DDC/CI 功能
//
static void TestMonitor(HMONITOR hMonitor, int monitorIndex)
{
    MONITORINFOEXW monInfo = {};
    monInfo.cbSize = sizeof(monInfo);
    if (!GetMonitorInfoW(hMonitor, &monInfo)) {
        printf("  [!] GetMonitorInfo 失败, 错误码: %lu\n", GetLastError());
        return;
    }

    printf("\n--- 显示器 #%d ---\n", monitorIndex);
    printf("  设备名称: %ls\n", monInfo.szDevice);
    printf("  区域: (%ld,%ld)-(%ld,%ld)\n",
           monInfo.rcMonitor.left, monInfo.rcMonitor.top,
           monInfo.rcMonitor.right, monInfo.rcMonitor.bottom);
    printf("  主显示器: %s\n",
           (monInfo.dwFlags & MONITORINFOF_PRIMARY) ? "是" : "否");

    // 获取物理显示器
    DWORD numPhysical = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &numPhysical)) {
        printf("  [!] GetNumberOfPhysicalMonitors 失败, 错误码: %lu\n",
               GetLastError());
        return;
    }

    printf("  物理显示器数量: %lu\n", numPhysical);

    PHYSICAL_MONITOR *physicalMonitors = (PHYSICAL_MONITOR *)
        malloc(sizeof(PHYSICAL_MONITOR) * numPhysical);
    if (!physicalMonitors) {
        printf("  [!] 内存分配失败\n");
        return;
    }

    if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, numPhysical,
                                          physicalMonitors)) {
        printf("  [!] GetPhysicalMonitors 失败, 错误码: %lu\n",
               GetLastError());
        free(physicalMonitors);
        return;
    }

    for (DWORD i = 0; i < numPhysical; i++) {
        printf("\n  物理显示器 #%lu:\n", i);
        printf("    描述: %ls\n",
               physicalMonitors[i].szPhysicalMonitorDescription);

        HANDLE hPhysical = physicalMonitors[i].hPhysicalMonitor;

        // 测试亮度控制 (DDC/CI VCP code 0x10)
        DWORD minBrightness = 0, curBrightness = 0, maxBrightness = 0;
        if (GetMonitorBrightness(hPhysical,
                                  &minBrightness,
                                  &curBrightness,
                                  &maxBrightness)) {
            printf("    [OK] DDC/CI 亮度控制可用!\n");
            printf("    亮度范围: %lu - %lu\n", minBrightness, maxBrightness);
            printf("    当前亮度: %lu (DDC/CI 原始值)\n", curBrightness);
            printf("    当前亮度: %lu%% (映射后百分比)\n",
                   MapDDCToPercent(curBrightness, minBrightness, maxBrightness));

            if (minBrightness == 0 && maxBrightness == 100) {
                printf("    映射: 1:1 (无需转换)\n");
            } else {
                printf("    映射: 需要范围转换 [%lu-%lu] <-> [0-100]\n",
                       minBrightness, maxBrightness);
                printf("    示例: 50%% -> DDC值 %lu\n",
                       MapPercentToDDC(50, minBrightness, maxBrightness));
            }
        } else {
            printf("    [X] DDC/CI 亮度控制不可用 (错误码: %lu)\n",
                   GetLastError());
        }

        // 测试对比度控制
        DWORD minContrast = 0, curContrast = 0, maxContrast = 0;
        if (GetMonitorContrast(hPhysical,
                                &minContrast,
                                &curContrast,
                                &maxContrast)) {
            printf("    [OK] DDC/CI 对比度控制可用\n");
            printf("    对比度范围: %lu - %lu, 当前: %lu\n",
                   minContrast, maxContrast, curContrast);
        } else {
            printf("    [X] DDC/CI 对比度控制不可用\n");
        }

        // 获取显示器能力
        DWORD capFlags = 0, colorTempFlags = 0;
        if (GetMonitorCapabilities(hPhysical, &capFlags, &colorTempFlags)) {
            printf("    显示器能力标志: 0x%lx\n", capFlags);
            printf("    支持的色温: 0x%lx\n", colorTempFlags);
        }

        // 读取 VCP 特性 (低级 API)
        MC_VCP_CODE_TYPE codeType;
        DWORD currentValue = 0, maxValue = 0;
        if (GetVCPFeatureAndVCPFeatureReply(hPhysical, 0x10,
                                             &codeType,
                                             &currentValue,
                                             &maxValue)) {
            printf("    [VCP 0x10] 亮度 (低级API): 当前=%lu, 最大=%lu, 类型=%s\n",
                   currentValue, maxValue,
                   (codeType == MC_MOMENTARY) ? "Momentary" : "Continuous");
        }
    }

    DestroyPhysicalMonitors(numPhysical, physicalMonitors);
    free(physicalMonitors);
}

//
// 枚举回调: 测试模式 (显示信息)
//
typedef struct _ENUM_CONTEXT {
    int count;
} ENUM_CONTEXT;

static BOOL CALLBACK EnumProc_Info(HMONITOR hMonitor, HDC hdcMonitor,
                                    LPRECT lprcMonitor, LPARAM dwData)
{
    ENUM_CONTEXT *ctx = (ENUM_CONTEXT *)dwData;
    ctx->count++;
    TestMonitor(hMonitor, ctx->count);
    return TRUE;
}

//
// 枚举回调: 设置亮度模式
//
static BOOL CALLBACK EnumProc_SetBrightness(HMONITOR hMonitor, HDC hdcMonitor,
                                              LPRECT lprcMonitor, LPARAM dwData)
{
    ENUM_CONTEXT *ctx = (ENUM_CONTEXT *)dwData;
    ctx->count++;

    DWORD numPhysical = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &numPhysical)) {
        return TRUE;
    }

    PHYSICAL_MONITOR *pm = (PHYSICAL_MONITOR *)
        malloc(sizeof(PHYSICAL_MONITOR) * numPhysical);
    if (!pm) return TRUE;

    if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, numPhysical, pm)) {
        free(pm);
        return TRUE;
    }

    for (DWORD i = 0; i < numPhysical; i++) {
        DWORD minB = 0, curB = 0, maxB = 0;
        if (GetMonitorBrightness(pm[i].hPhysicalMonitor, &minB, &curB, &maxB)) {
            DWORD targetDDC = MapPercentToDDC(g_targetBrightness, minB, maxB);
            if (SetMonitorBrightness(pm[i].hPhysicalMonitor, targetDDC)) {
                printf("  显示器 #%d.%lu: 亮度设置为 %lu%% (DDC值: %lu, "
                       "范围 %lu-%lu)\n",
                       ctx->count, i, g_targetBrightness, targetDDC,
                       minB, maxB);
            } else {
                printf("  显示器 #%d.%lu: 设置失败, 错误码: %lu\n",
                       ctx->count, i, GetLastError());
            }
        } else {
            printf("  显示器 #%d.%lu: DDC/CI 不可用, 跳过\n",
                   ctx->count, i);
        }
    }

    DestroyPhysicalMonitors(numPhysical, pm);
    free(pm);
    return TRUE;
}

int main(int argc, char *argv[])
{
    SetConsoleOutputCP(65001);

    printf("========================================\n");
    printf("  DDC/CI 显示器测试工具\n");
    printf("  DDCBrightness Project\n");
    printf("========================================\n");

    if (argc >= 3 && strcmp(argv[1], "--set") == 0) {
        g_targetBrightness = (DWORD)atoi(argv[2]);
        if (g_targetBrightness > 100) g_targetBrightness = 100;

        printf("\n设置所有 DDC/CI 显示器亮度到 %lu%%...\n", g_targetBrightness);

        ENUM_CONTEXT ctx = {};
        EnumDisplayMonitors(NULL, NULL, EnumProc_SetBrightness, (LPARAM)&ctx);

        printf("\n完成，共处理 %d 台显示器\n", ctx.count);
        return 0;
    }

    // 默认模式: 枚举并测试所有显示器
    ENUM_CONTEXT ctx = {};
    EnumDisplayMonitors(NULL, NULL, EnumProc_Info, (LPARAM)&ctx);

    printf("\n========================================\n");
    printf("总结:\n");
    printf("  检测到 %d 台显示器\n", ctx.count);
    printf("========================================\n");

    if (ctx.count == 0) {
        printf("\n[!] 未检测到任何显示器。\n");
        printf("    请确保显示器已连接并处于开启状态。\n");
    }

    printf("\n用法:\n");
    printf("  ddc_test             枚举所有显示器并显示 DDC/CI 信息\n");
    printf("  ddc_test --set <N>   设置所有显示器亮度为 N%% (0-100)\n");
    printf("  例如: ddc_test --set 50\n");

    return 0;
}
