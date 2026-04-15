/*
 * DDCBrightness KMDF 过滤驱动 - 内部定义
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <ntstrsafe.h>
#include <wdmsec.h>

#include "../common/public.h"

//
// DISPLAYPOLICY 定义 (某些 WDK 版本可能未定义)
//
#ifndef DISPLAYPOLICY_AC
#define DISPLAYPOLICY_AC    1
#endif
#ifndef DISPLAYPOLICY_DC
#define DISPLAYPOLICY_DC    2
#endif
#ifndef DISPLAYPOLICY_BOTH
#define DISPLAYPOLICY_BOTH  3
#endif

//
// 驱动 Pool Tag
//
#define DDCBRT_TAG 'tBDD'

//
// GUID_DEVINTERFACE_BRIGHTNESS 已在 ntddvdeo.h 中定义
// {FDE5BBA4-B3F9-46FB-BDAA-0728CE3100B4}
//

//
// 控制设备 GUID (用于服务通信)
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
//
DEFINE_GUID(GUID_DDCBRT_CONTROL_DEVICE,
    0xA1B2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90);

//
// 单个显示器的亮度状态
//
typedef struct _MONITOR_BRIGHTNESS_STATE {
    BOOLEAN Active;                         // 此槽位是否活跃
    DDCBRT_MONITOR_INFO Info;               // 显示器信息
    UCHAR CurrentBrightnessPercent;         // 当前亮度百分比 (0-100)
    UCHAR TargetBrightnessPercent;          // 目标亮度百分比 (0-100)
    UCHAR ACBrightness;                     // AC 亮度
    UCHAR DCBrightness;                     // DC 亮度
} MONITOR_BRIGHTNESS_STATE, *PMONITOR_BRIGHTNESS_STATE;

//
// 过滤设备上下文 (每个过滤设备对象一份)
//
typedef struct _FILTER_DEVICE_CONTEXT {
    WDFDEVICE Device;                       // WDF 设备句柄
    BOOLEAN BrightnessInterfaceEnabled;     // 是否启用了亮度接口
    WDFQUEUE DefaultQueue;                  // 默认 I/O 队列
    WDFQUEUE NotificationQueue;             // 反向调用通知队列 (手动调度)
    MONITOR_BRIGHTNESS_STATE Monitors[DDCBRT_MAX_MONITORS];
    ULONG MonitorCount;                     // 已注册的显示器数量
    WDFSPINLOCK Lock;                       // 保护显示器状态的自旋锁
} FILTER_DEVICE_CONTEXT, *PFILTER_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_DEVICE_CONTEXT, FilterGetDeviceContext)

//
// 控制设备上下文 (单例，用于与服务通信)
//
typedef struct _CONTROL_DEVICE_CONTEXT {
    WDFDEVICE ControlDevice;                // 控制设备句柄
    WDFQUEUE ControlQueue;                  // 控制设备 I/O 队列
    WDFQUEUE BrightnessWaitQueue;           // 亮度变化等待队列 (手动调度)
    WDFSPINLOCK Lock;                       // 保护控制设备状态的自旋锁

    // 过滤设备引用 (用于将亮度请求路由到正确的过滤设备)
    WDFDEVICE FilterDevices[DDCBRT_MAX_MONITORS];
    ULONG FilterDeviceCount;

    // 全局显示器状态
    MONITOR_BRIGHTNESS_STATE Monitors[DDCBRT_MAX_MONITORS];
    ULONG MonitorCount;
} CONTROL_DEVICE_CONTEXT, *PCONTROL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, ControlGetDeviceContext)

//
// 全局变量
//
extern WDFDEVICE g_ControlDevice;
extern BOOLEAN g_ControlDeviceCreated;

//
// 驱动入口和设备管理 (driver.c)
//
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DDCBrt_EvtDeviceAdd;
EVT_WDF_DEVICE_CONTEXT_CLEANUP DDCBrt_EvtDeviceContextCleanup;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DDCBrt_EvtDriverContextCleanup;

NTSTATUS DDCBrt_CreateControlDevice(_In_ WDFDEVICE FilterDevice);
VOID DDCBrt_DestroyControlDevice(VOID);

//
// 亮度 IOCTL 处理 (brightness.c)
//
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DDCBrt_EvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DDCBrt_EvtControlDeviceIoControl;

NTSTATUS DDCBrt_HandleQuerySupportedBrightness(
    _In_ WDFREQUEST Request,
    _In_ PFILTER_DEVICE_CONTEXT DeviceContext
);

NTSTATUS DDCBrt_HandleQueryDisplayBrightness(
    _In_ WDFREQUEST Request,
    _In_ PFILTER_DEVICE_CONTEXT DeviceContext
);

NTSTATUS DDCBrt_HandleSetDisplayBrightness(
    _In_ WDFREQUEST Request,
    _In_ PFILTER_DEVICE_CONTEXT DeviceContext
);

NTSTATUS DDCBrt_HandleRegisterMonitor(
    _In_ WDFREQUEST Request
);

NTSTATUS DDCBrt_HandleUnregisterMonitor(
    _In_ WDFREQUEST Request
);

NTSTATUS DDCBrt_HandleWaitBrightnessChange(
    _In_ WDFREQUEST Request
);

NTSTATUS DDCBrt_HandleReportBrightness(
    _In_ WDFREQUEST Request
);

VOID DDCBrt_NotifyBrightnessChange(
    _In_ ULONG MonitorIndex,
    _In_ ULONG BrightnessPercent,
    _In_ UCHAR DisplayPolicy
);
