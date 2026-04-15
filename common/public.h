/*
 * DDCBrightness - 公共定义头文件
 * 驱动和用户态服务共享的 IOCTL 定义、数据结构和常量
 */

#pragma once

//
// 驱动设备名称和符号链接
//
#define DDCBRT_DEVICE_NAME      L"\\Device\\DDCBrightness"
#define DDCBRT_SYMLINK_NAME     L"\\DosDevices\\DDCBrightnessCtl"
#define DDCBRT_USERMODE_PATH    L"\\\\.\\DDCBrightnessCtl"

//
// 设备类型 (使用自定义范围 0x8000+)
//
#define FILE_DEVICE_DDCBRIGHTNESS   0x8001

//
// 最大支持的显示器数量
//
#define DDCBRT_MAX_MONITORS     16

//
// 亮度级别数量 (0-100, 共101级)
//
#define DDCBRT_BRIGHTNESS_LEVELS    101

//
// 自定义 IOCTL 控制码
// 用于驱动和服务之间的通信
//

// 服务 -> 驱动: 注册一个 DDC/CI 可用的显示器
// Input:  DDCBRT_MONITOR_INFO
// Output: 无
#define IOCTL_DDCBRT_REGISTER_MONITOR \
    CTL_CODE(FILE_DEVICE_DDCBRIGHTNESS, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 服务 -> 驱动: 注销一个显示器
// Input:  DDCBRT_MONITOR_ID
// Output: 无
#define IOCTL_DDCBRT_UNREGISTER_MONITOR \
    CTL_CODE(FILE_DEVICE_DDCBRIGHTNESS, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 服务 -> 驱动: 反向调用 - 等待亮度变化事件
// Input:  无
// Output: DDCBRT_BRIGHTNESS_REQUEST (驱动在亮度变化时完成此请求)
#define IOCTL_DDCBRT_WAIT_BRIGHTNESS_CHANGE \
    CTL_CODE(FILE_DEVICE_DDCBRIGHTNESS, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 服务 -> 驱动: 报告当前亮度 (同步亮度值)
// Input:  DDCBRT_BRIGHTNESS_REPORT
// Output: 无
#define IOCTL_DDCBRT_REPORT_BRIGHTNESS \
    CTL_CODE(FILE_DEVICE_DDCBRIGHTNESS, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 服务 -> 驱动: 查询所有已注册的显示器
// Input:  无
// Output: DDCBRT_MONITOR_LIST
#define IOCTL_DDCBRT_QUERY_MONITORS \
    CTL_CODE(FILE_DEVICE_DDCBRIGHTNESS, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

//
// 数据结构
//

#pragma pack(push, 1)

// 显示器标识
typedef struct _DDCBRT_MONITOR_ID {
    ULONG MonitorIndex;                     // 显示器索引 (0-based)
    WCHAR DevicePath[128];                  // 显示器设备路径
} DDCBRT_MONITOR_ID, *PDDCBRT_MONITOR_ID;

// 显示器信息 (注册时使用)
typedef struct _DDCBRT_MONITOR_INFO {
    DDCBRT_MONITOR_ID Id;
    ULONG MinBrightness;                    // DDC/CI 最小亮度
    ULONG MaxBrightness;                    // DDC/CI 最大亮度
    ULONG CurrentBrightness;                // DDC/CI 当前亮度
    WCHAR DisplayName[64];                  // 显示器名称
} DDCBRT_MONITOR_INFO, *PDDCBRT_MONITOR_INFO;

// 亮度变化请求 (反向调用输出)
typedef struct _DDCBRT_BRIGHTNESS_REQUEST {
    ULONG MonitorIndex;                     // 目标显示器索引
    ULONG BrightnessPercent;                // 目标亮度百分比 (0-100)
    UCHAR DisplayPolicy;                    // AC=1, DC=2
} DDCBRT_BRIGHTNESS_REQUEST, *PDDCBRT_BRIGHTNESS_REQUEST;

// 亮度报告 (服务向驱动同步当前亮度)
typedef struct _DDCBRT_BRIGHTNESS_REPORT {
    ULONG MonitorIndex;                     // 显示器索引
    ULONG BrightnessPercent;                // 当前亮度百分比 (0-100)
} DDCBRT_BRIGHTNESS_REPORT, *PDDCBRT_BRIGHTNESS_REPORT;

// 显示器列表
typedef struct _DDCBRT_MONITOR_LIST {
    ULONG Count;                            // 已注册的显示器数量
    DDCBRT_MONITOR_INFO Monitors[DDCBRT_MAX_MONITORS];
} DDCBRT_MONITOR_LIST, *PDDCBRT_MONITOR_LIST;

#pragma pack(pop)
