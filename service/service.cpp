/*
 * DDCBrightness 用户态服务
 *
 * 作为 Windows 服务运行，负责:
 * 1. 枚举所有支持 DDC/CI 的外置显示器
 * 2. 向驱动注册这些显示器
 * 3. 监听驱动的亮度变化通知
 * 4. 通过 DDC/CI 协议实际设置显示器亮度
 *
 * 编译: cl /EHsc service.cpp /link dxva2.lib user32.lib advapi32.lib
 */

#include "service.h"
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <userenv.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

//
// 全局状态
//
static SERVICE_STATUS g_ServiceStatus = {};
static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static HANDLE g_StopEvent = NULL;
static HANDLE g_DriverHandle = INVALID_HANDLE_VALUE;

static MonitorDDCInfo g_Monitors[DDCBRT_MAX_MONITORS] = {};
static DWORD g_MonitorCount = 0;

// 用于保存 GetPhysicalMonitorsFromHMONITOR 分配的句柄
static PHYSICAL_MONITOR *g_PhysicalMonitors[DDCBRT_MAX_MONITORS] = {};
static DWORD g_PhysicalMonitorCounts[DDCBRT_MAX_MONITORS] = {};

//
// 日志辅助
//
static void LogInfo(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 写入调试输出
    OutputDebugStringA("[DDCBrightness] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // 同时写入控制台 (调试模式)
    printf("[DDCBrightness] %s\n", buf);
}

static void LogError(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA("[DDCBrightness ERROR] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    fprintf(stderr, "[DDCBrightness ERROR] %s\n", buf);
}

//
// 枚举显示器回调
//
struct EnumContext {
    DWORD count;
};

static BOOL CALLBACK MonitorEnumCallback(
    HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    UNREFERENCED_PARAMETER(hdcMonitor);
    UNREFERENCED_PARAMETER(lprcMonitor);

    EnumContext *ctx = (EnumContext *)dwData;

    if (ctx->count >= DDCBRT_MAX_MONITORS) {
        return FALSE;
    }

    MONITORINFOEXW monInfo = {};
    monInfo.cbSize = sizeof(monInfo);
    if (!GetMonitorInfoW(hMonitor, &monInfo)) {
        return TRUE;
    }

    // 获取物理显示器
    DWORD numPhysical = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &numPhysical) ||
        numPhysical == 0) {
        return TRUE;
    }

    PHYSICAL_MONITOR *pm = (PHYSICAL_MONITOR *)
        malloc(sizeof(PHYSICAL_MONITOR) * numPhysical);
    if (!pm) return TRUE;

    if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, numPhysical, pm)) {
        free(pm);
        return TRUE;
    }

    // 测试每个物理显示器的 DDC/CI 亮度控制
    bool pmOwnershipTaken = false;
    for (DWORD i = 0; i < numPhysical && ctx->count < DDCBRT_MAX_MONITORS; i++) {
        DWORD minB = 0, curB = 0, maxB = 0;
        if (GetMonitorBrightness(pm[i].hPhysicalMonitor,
                                  &minB, &curB, &maxB)) {
            // DDC/CI 亮度可用
            DWORD idx = ctx->count;
            g_Monitors[idx].active = true;
            g_Monitors[idx].index = idx;
            g_Monitors[idx].hLogicalMonitor = hMonitor;
            g_Monitors[idx].hPhysicalMonitor = pm[i].hPhysicalMonitor;
            g_Monitors[idx].minBrightness = minB;
            g_Monitors[idx].maxBrightness = maxB;
            g_Monitors[idx].currentBrightness = curB;

            StringCchCopyW(g_Monitors[idx].deviceName,
                           _countof(g_Monitors[idx].deviceName),
                           monInfo.szDevice);
            StringCchCopyW(g_Monitors[idx].description,
                           _countof(g_Monitors[idx].description),
                           pm[i].szPhysicalMonitorDescription);

            // 仅第一个槽位持有 pm 数组的所有权 (负责清理)
            if (!pmOwnershipTaken) {
                g_PhysicalMonitors[idx] = pm;
                g_PhysicalMonitorCounts[idx] = numPhysical;
                pmOwnershipTaken = true;
            } else {
                g_PhysicalMonitors[idx] = NULL;
                g_PhysicalMonitorCounts[idx] = 0;
            }

            LogInfo("发现 DDC/CI 显示器 #%lu: %ls (%ls), "
                    "亮度范围: %lu-%lu, 当前: %lu",
                    idx, pm[i].szPhysicalMonitorDescription,
                    monInfo.szDevice, minB, maxB, curB);

            ctx->count++;
        }
    }

    if (!pmOwnershipTaken) {
        // 没有 DDC/CI 可用的显示器，释放 pm
        DestroyPhysicalMonitors(numPhysical, pm);
        free(pm);
    }

    return TRUE;
}

//
// 枚举所有支持 DDC/CI 的显示器
//
static DWORD EnumerateMonitors()
{
    // 清理旧状态
    for (DWORD i = 0; i < g_MonitorCount; i++) {
        if (g_PhysicalMonitors[i]) {
            DestroyPhysicalMonitors(g_PhysicalMonitorCounts[i],
                                     g_PhysicalMonitors[i]);
            free(g_PhysicalMonitors[i]);
            g_PhysicalMonitors[i] = NULL;
        }
    }
    ZeroMemory(g_Monitors, sizeof(g_Monitors));
    g_MonitorCount = 0;

    EnumContext ctx = {};
    EnumDisplayMonitors(NULL, NULL, MonitorEnumCallback, (LPARAM)&ctx);
    g_MonitorCount = ctx.count;

    LogInfo("枚举完成: 发现 %lu 台 DDC/CI 显示器", g_MonitorCount);
    return g_MonitorCount;
}

//
// 打开驱动控制设备
//
static HANDLE OpenDriver()
{
    HANDLE hDevice = CreateFileW(
        DDCBRT_USERMODE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (hDevice == INVALID_HANDLE_VALUE) {
        LogError("无法打开驱动设备 %ls, 错误码: %lu",
                 DDCBRT_USERMODE_PATH, GetLastError());
    } else {
        LogInfo("已连接到驱动设备 %ls", DDCBRT_USERMODE_PATH);
    }

    return hDevice;
}

//
// 向驱动注册显示器
//
static bool RegisterMonitorWithDriver(HANDLE hDriver, MonitorDDCInfo *mon)
{
    DDCBRT_MONITOR_INFO info = {};
    info.Id.MonitorIndex = mon->index;
    StringCchCopyW(info.Id.DevicePath, _countof(info.Id.DevicePath),
                   mon->deviceName);
    info.MinBrightness = mon->minBrightness;
    info.MaxBrightness = mon->maxBrightness;
    info.CurrentBrightness = mon->currentBrightness;
    StringCchCopyW(info.DisplayName, _countof(info.DisplayName),
                   mon->description);

    DWORD bytesReturned = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    BOOL ok = DeviceIoControl(
        hDriver,
        IOCTL_DDCBRT_REGISTER_MONITOR,
        &info, sizeof(info),
        NULL, 0,
        &bytesReturned,
        &ov
    );
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(hDriver, &ov, &bytesReturned, TRUE) ? TRUE : FALSE;
    }
    CloseHandle(ov.hEvent);

    if (ok) {
        LogInfo("显示器 #%lu '%ls' 已注册到驱动", mon->index, mon->description);
    } else {
        LogError("注册显示器 #%lu 失败, 错误码: %lu",
                 mon->index, GetLastError());
    }

    return ok != FALSE;
}

//
// 向驱动报告当前亮度
//
static void ReportBrightnessToDriver(HANDLE hDriver, DWORD monitorIndex,
                                      DWORD brightnessPercent)
{
    DDCBRT_BRIGHTNESS_REPORT report = {};
    report.MonitorIndex = monitorIndex;
    report.BrightnessPercent = brightnessPercent;

    DWORD bytesReturned = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    DeviceIoControl(
        hDriver,
        IOCTL_DDCBRT_REPORT_BRIGHTNESS,
        &report, sizeof(report),
        NULL, 0,
        &bytesReturned,
        &ov
    );
    if (GetLastError() == ERROR_IO_PENDING) {
        GetOverlappedResult(hDriver, &ov, &bytesReturned, TRUE);
    }
    CloseHandle(ov.hEvent);
}

//
// 通过 DDC/CI 设置显示器亮度
//
static bool SetMonitorBrightnessDDC(MonitorDDCInfo *mon, DWORD brightnessPercent)
{
    if (!mon->active || !mon->hPhysicalMonitor) {
        return false;
    }

    // 映射: 百分比 -> DDC/CI 原始值
    DWORD ddcValue = MapPercentToDDC(
        brightnessPercent, mon->minBrightness, mon->maxBrightness);

    LogInfo("设置显示器 #%lu 亮度: %lu%% -> DDC值 %lu (范围 %lu-%lu)",
            mon->index, brightnessPercent, ddcValue,
            mon->minBrightness, mon->maxBrightness);

    BOOL ok = SetMonitorBrightness(mon->hPhysicalMonitor, ddcValue);
    if (ok) {
        mon->currentBrightness = ddcValue;
    } else {
        LogError("SetMonitorBrightness 失败, 错误码: %lu", GetLastError());
    }

    return ok != FALSE;
}

//
// 亮度控制主循环
// 监听驱动的亮度变化通知，通过 DDC/CI 设置显示器
//
static void BrightnessControlLoop(HANDLE hDriver)
{
    LogInfo("亮度控制循环启动");

    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        // 发送反向调用请求 (等待亮度变化)
        DDCBRT_BRIGHTNESS_REQUEST brightnessReq = {};
        DWORD bytesReturned = 0;

        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!overlapped.hEvent) {
            LogError("CreateEvent 失败");
            Sleep(1000);
            continue;
        }

        BOOL ok = DeviceIoControl(
            hDriver,
            IOCTL_DDCBRT_WAIT_BRIGHTNESS_CHANGE,
            NULL, 0,
            &brightnessReq, sizeof(brightnessReq),
            &bytesReturned,
            &overlapped
        );

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            LogError("WAIT_BRIGHTNESS_CHANGE 失败, 错误码: %lu",
                     GetLastError());
            CloseHandle(overlapped.hEvent);
            Sleep(1000);
            continue;
        }

        // 等待亮度变化通知或停止事件
        HANDLE waitHandles[2] = { overlapped.hEvent, g_StopEvent };
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE,
                                                   INFINITE);

        if (waitResult == WAIT_OBJECT_0) {
            // 收到亮度变化通知
            if (GetOverlappedResult(hDriver, &overlapped,
                                     &bytesReturned, FALSE)) {
                LogInfo("收到亮度变化通知: 显示器=%lu, 亮度=%lu%%",
                        brightnessReq.MonitorIndex,
                        brightnessReq.BrightnessPercent);

                DWORD targetPercent = brightnessReq.BrightnessPercent;

                if (brightnessReq.MonitorIndex == 0xFFFFFFFF) {
                    // 设置所有显示器
                    for (DWORD i = 0; i < g_MonitorCount; i++) {
                        if (g_Monitors[i].active) {
                            SetMonitorBrightnessDDC(&g_Monitors[i],
                                                     targetPercent);
                            ReportBrightnessToDriver(hDriver, i,
                                                      targetPercent);
                        }
                    }
                } else if (brightnessReq.MonitorIndex < g_MonitorCount) {
                    // 设置指定显示器
                    DWORD idx = brightnessReq.MonitorIndex;
                    if (g_Monitors[idx].active) {
                        SetMonitorBrightnessDDC(&g_Monitors[idx],
                                                 targetPercent);
                        ReportBrightnessToDriver(hDriver, idx,
                                                  targetPercent);
                    }
                }
            }
        } else if (waitResult == WAIT_OBJECT_0 + 1) {
            // 停止事件: 取消未完成的 I/O 并等待取消完成
            CancelIo(hDriver);
            GetOverlappedResult(hDriver, &overlapped, &bytesReturned, TRUE);
            CloseHandle(overlapped.hEvent);
            break;
        }

        CloseHandle(overlapped.hEvent);
    }

    LogInfo("亮度控制循环退出");
}

//
// 在用户会话中启动辅助进程 (解决 Session 0 无法枚举显示器的问题)
// 服务 (Session 0) 通过此函数在用户的交互式会话中启动自身的 console 模式
//
static BOOL LaunchInUserSession()
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        LogError("no active console session found");
        return FALSE;
    }

    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        LogError("WTSQueryUserToken failed, error: %lu", GetLastError());
        return FALSE;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                           SecurityIdentification, TokenPrimary, &hDupToken)) {
        LogError("DuplicateTokenEx failed, error: %lu", GetLastError());
        CloseHandle(hToken);
        return FALSE;
    }

    LPVOID pEnv = NULL;
    CreateEnvironmentBlock(&pEnv, hDupToken, FALSE);

    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    // 以 "--session-worker" 参数启动，标记为会话内工作进程
    WCHAR cmdLine[MAX_PATH + 64];
    StringCchPrintfW(cmdLine, _countof(cmdLine), L"\"%s\" --session-worker", exePath);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessAsUserW(
        hDupToken,
        NULL,
        cmdLine,
        NULL, NULL,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        pEnv,
        NULL,
        &si,
        &pi
    );

    if (ok) {
        LogInfo("launched session worker in session %lu, PID %lu",
                sessionId, pi.dwProcessId);

        // 等待辅助进程结束或服务停止
        HANDLE waitHandles[2] = { pi.hProcess, g_StopEvent };
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1) {
            // 服务停止，终止辅助进程
            TerminateProcess(pi.hProcess, 0);
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        LogError("CreateProcessAsUser failed, error: %lu", GetLastError());
    }

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hDupToken);
    CloseHandle(hToken);

    return ok;
}

//
// 服务工作线程
//
static DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    UNREFERENCED_PARAMETER(lpParam);

    LogInfo("service worker thread started");

    // 检测是否在 Session 0 (服务模式)
    DWORD currentSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSession);
    LogInfo("running in session %lu", currentSession);

    // 等待系统稳定
    Sleep(2000);

    // 尝试枚举显示器
    DWORD monitorCount = EnumerateMonitors();
    if (monitorCount == 0 && currentSession == 0) {
        // Session 0 无法枚举显示器, 启动用户会话辅助进程
        LogInfo("Session 0: no monitors found, launching user session worker...");

        // 等待用户登录
        while (WaitForSingleObject(g_StopEvent, 5000) == WAIT_TIMEOUT) {
            if (WTSGetActiveConsoleSessionId() != 0xFFFFFFFF) {
                Sleep(3000); // 等待用户会话完全初始化
                if (LaunchInUserSession()) {
                    return 0; // 辅助进程已接管工作
                }
                LogError("failed to launch session worker, retrying in 30s...");
            }
            if (WaitForSingleObject(g_StopEvent, 30000) != WAIT_TIMEOUT) {
                return 0;
            }
        }
        return 0;
    }

    if (monitorCount == 0) {
        LogInfo("未发现 DDC/CI 显示器, 5秒后重试...");
        Sleep(5000);
        monitorCount = EnumerateMonitors();
        if (monitorCount == 0) {
            LogError("仍未发现 DDC/CI 显示器, 服务将继续等待...");

            // 每30秒重试一次
            while (WaitForSingleObject(g_StopEvent, 30000) == WAIT_TIMEOUT) {
                monitorCount = EnumerateMonitors();
                if (monitorCount > 0) break;
            }

            if (monitorCount == 0) {
                return 0;
            }
        }
    }

    // 步骤 2: 连接驱动
    g_DriverHandle = OpenDriver();
    if (g_DriverHandle == INVALID_HANDLE_VALUE) {
        LogError("无法连接驱动, 10秒后重试...");
        Sleep(10000);
        g_DriverHandle = OpenDriver();
        if (g_DriverHandle == INVALID_HANDLE_VALUE) {
            LogError("仍无法连接驱动, 服务退出");
            return 1;
        }
    }

    // 步骤 3: 向驱动注册所有 DDC/CI 显示器
    for (DWORD i = 0; i < g_MonitorCount; i++) {
        if (g_Monitors[i].active) {
            RegisterMonitorWithDriver(g_DriverHandle, &g_Monitors[i]);

            // 向驱动报告初始亮度
            DWORD brightnessPercent = MapDDCToPercent(
                g_Monitors[i].currentBrightness,
                g_Monitors[i].minBrightness,
                g_Monitors[i].maxBrightness
            );
            ReportBrightnessToDriver(g_DriverHandle, i, brightnessPercent);
        }
    }

    // 步骤 4: 进入亮度控制循环
    BrightnessControlLoop(g_DriverHandle);

    // 清理
    if (g_DriverHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_DriverHandle);
        g_DriverHandle = INVALID_HANDLE_VALUE;
    }

    return 0;
}

//
// 服务控制处理
//
static VOID WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
        LogInfo("收到停止服务请求");
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
            break;

        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 4;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        SetEvent(g_StopEvent);
        break;

    case SERVICE_CONTROL_INTERROGATE:
        break;

    default:
        break;
    }
}

//
// 服务主函数
//
static VOID WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    // 注册服务控制处理器
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME,
                                                  ServiceCtrlHandler);
    if (!g_StatusHandle) {
        LogError("RegisterServiceCtrlHandler 失败");
        return;
    }

    // 初始化服务状态
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // 创建停止事件
    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // 报告服务已启动
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwCheckPoint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    LogInfo("服务已启动");

    // 启动工作线程
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    // 清理
    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;

    // 清理物理显示器
    for (DWORD i = 0; i < DDCBRT_MAX_MONITORS; i++) {
        if (g_PhysicalMonitors[i]) {
            DestroyPhysicalMonitors(g_PhysicalMonitorCounts[i],
                                     g_PhysicalMonitors[i]);
            free(g_PhysicalMonitors[i]);
            g_PhysicalMonitors[i] = NULL;
        }
    }

    // 报告服务已停止
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    LogInfo("服务已停止");
}

//
// 安装服务
//
static int InstallService()
{
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCManager) {
        fprintf(stderr, "OpenSCManager 失败, 错误码: %lu\n", GetLastError());
        fprintf(stderr, "请以管理员身份运行\n");
        return 1;
    }

    // 获取当前可执行文件路径
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    SC_HANDLE hService = CreateServiceW(
        hSCManager,
        SERVICE_NAME,
        SERVICE_DISPLAY,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        exePath,
        NULL, NULL, NULL, NULL, NULL
    );

    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            printf("服务已存在\n");
        } else {
            fprintf(stderr, "CreateService 失败, 错误码: %lu\n", err);
        }
        CloseServiceHandle(hSCManager);
        return (err == ERROR_SERVICE_EXISTS) ? 0 : 1;
    }

    // 设置服务描述
    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = (LPWSTR)SERVICE_DESCRIPTION;
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &desc);

    printf("服务安装成功: %ls\n", SERVICE_NAME);

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);
    return 0;
}

//
// 卸载服务
//
static int UninstallService()
{
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) {
        fprintf(stderr, "OpenSCManager 失败, 错误码: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE hService = OpenServiceW(hSCManager, SERVICE_NAME,
                                       SERVICE_ALL_ACCESS);
    if (!hService) {
        fprintf(stderr, "OpenService 失败, 错误码: %lu\n", GetLastError());
        CloseServiceHandle(hSCManager);
        return 1;
    }

    // 停止服务
    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);
    Sleep(1000);

    // 删除服务
    if (DeleteService(hService)) {
        printf("服务卸载成功\n");
    } else {
        fprintf(stderr, "DeleteService 失败, 错误码: %lu\n", GetLastError());
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);
    return 0;
}

//
// 控制台调试模式 (非服务模式运行)
//
static int RunConsoleMode()
{
    SetConsoleOutputCP(65001);
    printf("========================================\n");
    printf("  DDCBrightness 服务 (控制台调试模式)\n");
    printf("========================================\n");

    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) {
        fprintf(stderr, "CreateEvent 失败\n");
        return 1;
    }

    // 设置 Ctrl+C 处理
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
            printf("\n收到停止信号...\n");
            SetEvent(g_StopEvent);
            return TRUE;
        }
        return FALSE;
    }, TRUE);

    printf("按 Ctrl+C 停止\n\n");

    // 直接运行工作线程
    ServiceWorkerThread(NULL);

    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;

    // 清理物理显示器
    for (DWORD i = 0; i < DDCBRT_MAX_MONITORS; i++) {
        if (g_PhysicalMonitors[i]) {
            DestroyPhysicalMonitors(g_PhysicalMonitorCounts[i],
                                     g_PhysicalMonitors[i]);
            free(g_PhysicalMonitors[i]);
        }
    }

    return 0;
}

//
// 程序入口
//
int wmain(int argc, wchar_t *argv[])
{
    // 处理命令行参数
    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"install") == 0 ||
            _wcsicmp(argv[1], L"/install") == 0) {
            return InstallService();
        }
        if (_wcsicmp(argv[1], L"uninstall") == 0 ||
            _wcsicmp(argv[1], L"/uninstall") == 0) {
            return UninstallService();
        }
        if (_wcsicmp(argv[1], L"console") == 0 ||
            _wcsicmp(argv[1], L"/console") == 0 ||
            _wcsicmp(argv[1], L"debug") == 0) {
            return RunConsoleMode();
        }
        if (_wcsicmp(argv[1], L"--session-worker") == 0) {
            // 由服务在用户会话中启动的辅助进程模式
            return RunConsoleMode();
        }

        printf("用法:\n");
        printf("  DDCBrightnessService install    安装服务\n");
        printf("  DDCBrightnessService uninstall  卸载服务\n");
        printf("  DDCBrightnessService console    控制台调试模式\n");
        printf("  (无参数运行时作为 Windows 服务启动)\n");
        return 0;
    }

    // 作为 Windows 服务启动
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // 不是从 SCM 启动的，以控制台模式运行
            printf("提示: 非服务模式启动，自动切换到控制台调试模式\n\n");
            return RunConsoleMode();
        }
        fprintf(stderr, "StartServiceCtrlDispatcher 失败, 错误码: %lu\n",
                err);
        return 1;
    }

    return 0;
}
