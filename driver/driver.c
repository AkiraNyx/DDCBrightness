/*
 * DDCBrightness KMDF 过滤驱动 - 驱动入口和设备管理
 *
 * 作为 Monitor 设备类的上层过滤驱动，注册亮度设备接口使 Windows
 * 在快捷设置中显示亮度滑块。通过控制设备与用户态服务通信，
 * 将亮度变化请求转发给服务，由服务通过 DDC/CI 实际控制显示器。
 */

#include "driver.h"

//
// 全局: 控制设备句柄和创建状态
//
WDFDEVICE g_ControlDevice = NULL;
BOOLEAN g_ControlDeviceCreated = FALSE;

// 过滤设备计数 (用于决定何时销毁控制设备)
static LONG g_FilterDeviceCount = 0;

//
// DriverEntry - 驱动加载入口
//
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: DriverEntry\n"));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = DDCBrt_EvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, DDCBrt_EvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: WdfDriverCreate 失败 0x%x\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 驱动加载成功\n"));

    return STATUS_SUCCESS;
}

//
// EvtDeviceAdd - 当 PnP 管理器检测到新显示器设备时调用
//
NTSTATUS
DDCBrt_EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PFILTER_DEVICE_CONTEXT deviceContext;
    WDF_IO_QUEUE_CONFIG queueConfig;

    UNREFERENCED_PARAMETER(Driver);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: EvtDeviceAdd\n"));

    //
    // 标记为过滤设备
    //
    WdfFdoInitSetFilter(DeviceInit);

    //
    // 创建过滤设备对象
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes,
                                             FILTER_DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = DDCBrt_EvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: WdfDeviceCreate 失败 0x%x\n", status));
        return status;
    }

    //
    // 初始化设备上下文
    //
    deviceContext = FilterGetDeviceContext(device);
    RtlZeroMemory(deviceContext, sizeof(FILTER_DEVICE_CONTEXT));
    deviceContext->Device = device;

    // 创建自旋锁
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                &deviceContext->Lock);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: WdfSpinLockCreate 失败 0x%x\n", status));
        return status;
    }

    //
    // 创建默认 I/O 队列 (处理亮度 IOCTL)
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = DDCBrt_EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &deviceContext->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: 创建默认队列失败 0x%x\n", status));
        return status;
    }

    //
    // 注册亮度设备接口
    // 这是让 Windows 显示亮度滑块的关键步骤
    //
    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_BRIGHTNESS,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: 注册亮度接口失败 0x%x\n", status));
        return status;
    }

    deviceContext->BrightnessInterfaceEnabled = TRUE;

    // 设置默认亮度
    for (int i = 0; i < DDCBRT_MAX_MONITORS; i++) {
        deviceContext->Monitors[i].Active = FALSE;
        deviceContext->Monitors[i].ACBrightness = 50;
        deviceContext->Monitors[i].DCBrightness = 50;
        deviceContext->Monitors[i].CurrentBrightnessPercent = 50;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 亮度设备接口已注册\n"));

    //
    // 创建控制设备 (仅在第一个过滤设备创建时)
    //
    InterlockedIncrement(&g_FilterDeviceCount);

    if (!g_ControlDeviceCreated) {
        status = DDCBrt_CreateControlDevice(device);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "DDCBrightness: 创建控制设备失败 0x%x (非致命)\n",
                       status));
            // 控制设备创建失败不影响亮度接口的注册
        }
    }

    // 将此过滤设备注册到控制设备
    if (g_ControlDevice != NULL) {
        PCONTROL_DEVICE_CONTEXT ctrlCtx =
            ControlGetDeviceContext(g_ControlDevice);
        WdfSpinLockAcquire(ctrlCtx->Lock);
        if (ctrlCtx->FilterDeviceCount < DDCBRT_MAX_MONITORS) {
            ctrlCtx->FilterDevices[ctrlCtx->FilterDeviceCount] = device;
            ctrlCtx->FilterDeviceCount++;
        }
        WdfSpinLockRelease(ctrlCtx->Lock);
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: EvtDeviceAdd 完成\n"));

    return STATUS_SUCCESS;
}

//
// 创建控制设备 - 用于与用户态服务通信
//
NTSTATUS
DDCBrt_CreateControlDevice(
    _In_ WDFDEVICE FilterDevice
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT deviceInit = NULL;
    WDFDEVICE controlDevice;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PCONTROL_DEVICE_CONTEXT controlContext;
    UNICODE_STRING deviceName = RTL_CONSTANT_STRING(DDCBRT_DEVICE_NAME);
    UNICODE_STRING symlinkName = RTL_CONSTANT_STRING(DDCBRT_SYMLINK_NAME);

    UNREFERENCED_PARAMETER(FilterDevice);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 创建控制设备\n"));

    //
    // 分配控制设备初始化结构
    //
    deviceInit = WdfControlDeviceInitAllocate(
        WdfGetDriver(),
        &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX
    );
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // 设置设备名称
    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    //
    // 创建控制设备
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
                                             CONTROL_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&deviceInit, &attributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        // deviceInit 在失败时已被 WdfDeviceCreate 释放
        return status;
    }

    //
    // 创建符号链接
    //
    status = WdfDeviceCreateSymbolicLink(controlDevice, &symlinkName);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    //
    // 初始化控制设备上下文
    //
    controlContext = ControlGetDeviceContext(controlDevice);
    RtlZeroMemory(controlContext, sizeof(CONTROL_DEVICE_CONTEXT));
    controlContext->ControlDevice = controlDevice;

    // 创建自旋锁
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                &controlContext->Lock);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    //
    // 创建控制设备的默认 I/O 队列
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = DDCBrt_EvtControlDeviceIoControl;

    status = WdfIoQueueCreate(controlDevice, &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &controlContext->ControlQueue);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    //
    // 创建亮度变化等待队列 (手动调度，用于反向调用)
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(controlDevice, &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &controlContext->BrightnessWaitQueue);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    //
    // 完成控制设备初始化
    //
    WdfControlFinishInitializing(controlDevice);

    g_ControlDevice = controlDevice;
    g_ControlDeviceCreated = TRUE;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 控制设备创建成功 (%wZ)\n", &symlinkName));

    return STATUS_SUCCESS;
}

//
// 销毁控制设备
//
VOID
DDCBrt_DestroyControlDevice(VOID)
{
    if (g_ControlDevice != NULL) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "DDCBrightness: 销毁控制设备\n"));
        WdfObjectDelete(g_ControlDevice);
        g_ControlDevice = NULL;
        g_ControlDeviceCreated = FALSE;
    }
}

//
// 过滤设备清理回调
//
VOID
DDCBrt_EvtDeviceContextCleanup(
    _In_ WDFOBJECT Object
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 设备上下文清理\n"));

    LONG remaining = InterlockedDecrement(&g_FilterDeviceCount);

    // 从控制设备中移除此过滤设备的引用
    if (g_ControlDevice != NULL) {
        WDFDEVICE device = (WDFDEVICE)Object;
        PCONTROL_DEVICE_CONTEXT ctrlCtx =
            ControlGetDeviceContext(g_ControlDevice);

        WdfSpinLockAcquire(ctrlCtx->Lock);
        for (ULONG i = 0; i < ctrlCtx->FilterDeviceCount; i++) {
            if (ctrlCtx->FilterDevices[i] == device) {
                // 用最后一个填充空位
                ctrlCtx->FilterDeviceCount--;
                if (i < ctrlCtx->FilterDeviceCount) {
                    ctrlCtx->FilterDevices[i] =
                        ctrlCtx->FilterDevices[ctrlCtx->FilterDeviceCount];
                }
                break;
            }
        }
        WdfSpinLockRelease(ctrlCtx->Lock);
    }

    // 最后一个过滤设备被移除时，销毁控制设备
    if (remaining == 0) {
        DDCBrt_DestroyControlDevice();
    }
}

//
// 驱动清理回调
//
VOID
DDCBrt_EvtDriverContextCleanup(
    _In_ WDFOBJECT Object
)
{
    UNREFERENCED_PARAMETER(Object);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 驱动卸载\n"));
}
