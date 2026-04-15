/*
 * DDCBrightness KMDF 过滤驱动 - 亮度 IOCTL 处理
 *
 * 处理 Windows 亮度子系统发送的 IOCTL，以及来自用户态服务的
 * 控制 IOCTL。实现反向调用机制将亮度变化通知给服务。
 */

#include "driver.h"

//
// 过滤设备的 IOCTL 处理
// 拦截亮度相关 IOCTL，其余 PassThrough
//
VOID
DDCBrt_EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PFILTER_DEVICE_CONTEXT deviceContext = FilterGetDeviceContext(device);

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS:
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL,
                   "DDCBrightness: QUERY_SUPPORTED_BRIGHTNESS\n"));
        status = DDCBrt_HandleQuerySupportedBrightness(Request, deviceContext);
        if (NT_SUCCESS(status)) {
            return; // 请求已完成
        }
        break;

    case IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS:
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL,
                   "DDCBrightness: QUERY_DISPLAY_BRIGHTNESS\n"));
        status = DDCBrt_HandleQueryDisplayBrightness(Request, deviceContext);
        if (NT_SUCCESS(status)) {
            return;
        }
        break;

    case IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS:
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "DDCBrightness: SET_DISPLAY_BRIGHTNESS\n"));
        status = DDCBrt_HandleSetDisplayBrightness(Request, deviceContext);
        if (NT_SUCCESS(status)) {
            return;
        }
        break;

    default:
        // 不是亮度 IOCTL，下传给底层驱动
        break;
    }

    // 将请求下传给底层驱动
    WdfRequestFormatRequestUsingCurrentType(Request);

    WDF_REQUEST_SEND_OPTIONS sendOptions;
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions,
                                   WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    if (!WdfRequestSend(Request,
                         WdfDeviceGetIoTarget(device),
                         &sendOptions)) {
        status = WdfRequestGetStatus(Request);
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: 转发请求失败 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }
}

//
// 处理 IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS
// 返回支持的亮度级别列表 (0-100)
//
NTSTATUS
DDCBrt_HandleQuerySupportedBrightness(
    _In_ WDFREQUEST Request,
    _In_ PFILTER_DEVICE_CONTEXT DeviceContext
)
{
    NTSTATUS status;
    PVOID outputBuffer = NULL;
    size_t outputSize = 0;

    UNREFERENCED_PARAMETER(DeviceContext);

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        1,  // 最少1字节
        &outputBuffer,
        &outputSize
    );

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return status;
    }

    // 返回 101 个亮度级别 (0-100)
    ULONG levelsToReturn = DDCBRT_BRIGHTNESS_LEVELS;
    if (outputSize < levelsToReturn) {
        levelsToReturn = (ULONG)outputSize;
    }

    PUCHAR levels = (PUCHAR)outputBuffer;
    for (ULONG i = 0; i < levelsToReturn; i++) {
        levels[i] = (UCHAR)i;
    }

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, levelsToReturn);
    return STATUS_SUCCESS;
}

//
// 处理 IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS
// 返回当前亮度值
//
NTSTATUS
DDCBrt_HandleQueryDisplayBrightness(
    _In_ WDFREQUEST Request,
    _In_ PFILTER_DEVICE_CONTEXT DeviceContext
)
{
    NTSTATUS status;
    PDISPLAY_BRIGHTNESS outputBuffer = NULL;

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(DISPLAY_BRIGHTNESS),
        (PVOID *)&outputBuffer,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return status;
    }

    // 返回当前缓存的亮度值
    WdfSpinLockAcquire(DeviceContext->Lock);

    // 使用第一个活跃显示器的亮度，如果没有则使用默认值
    UCHAR acBrightness = 50;
    UCHAR dcBrightness = 50;

    for (ULONG i = 0; i < DDCBRT_MAX_MONITORS; i++) {
        if (DeviceContext->Monitors[i].Active) {
            acBrightness = DeviceContext->Monitors[i].ACBrightness;
            dcBrightness = DeviceContext->Monitors[i].DCBrightness;
            break;
        }
    }

    WdfSpinLockRelease(DeviceContext->Lock);

    outputBuffer->ucDisplayPolicy = DISPLAYPOLICY_BOTH;
    outputBuffer->ucACBrightness = acBrightness;
    outputBuffer->ucDCBrightness = dcBrightness;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                       sizeof(DISPLAY_BRIGHTNESS));
    return STATUS_SUCCESS;
}

//
// 处理 IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS
// 缓存新亮度值并通知服务
//
NTSTATUS
DDCBrt_HandleSetDisplayBrightness(
    _In_ WDFREQUEST Request,
    _In_ PFILTER_DEVICE_CONTEXT DeviceContext
)
{
    NTSTATUS status;
    PDISPLAY_BRIGHTNESS inputBuffer = NULL;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(DISPLAY_BRIGHTNESS),
        (PVOID *)&inputBuffer,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return status;
    }

    UCHAR brightness = inputBuffer->ucACBrightness;
    UCHAR policy = inputBuffer->ucDisplayPolicy;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 设置亮度 AC=%u DC=%u Policy=%u\n",
               inputBuffer->ucACBrightness,
               inputBuffer->ucDCBrightness,
               policy));

    // 更新缓存的亮度值
    WdfSpinLockAcquire(DeviceContext->Lock);

    for (ULONG i = 0; i < DDCBRT_MAX_MONITORS; i++) {
        if (DeviceContext->Monitors[i].Active) {
            DeviceContext->Monitors[i].ACBrightness =
                inputBuffer->ucACBrightness;
            DeviceContext->Monitors[i].DCBrightness =
                inputBuffer->ucDCBrightness;
            DeviceContext->Monitors[i].TargetBrightnessPercent = brightness;
        }
    }

    WdfSpinLockRelease(DeviceContext->Lock);

    // 通知服务亮度变化 (对所有已注册的显示器)
    // 使用 AC 亮度作为目标值
    DDCBrt_NotifyBrightnessChange(0xFFFFFFFF, brightness, policy);

    WdfRequestComplete(Request, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

//
// 控制设备的 IOCTL 处理
// 处理来自用户态服务的请求
//
VOID
DDCBrt_EvtControlDeviceIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_DDCBRT_REGISTER_MONITOR:
        status = DDCBrt_HandleRegisterMonitor(Request);
        break;

    case IOCTL_DDCBRT_UNREGISTER_MONITOR:
        status = DDCBrt_HandleUnregisterMonitor(Request);
        break;

    case IOCTL_DDCBRT_WAIT_BRIGHTNESS_CHANGE:
        status = DDCBrt_HandleWaitBrightnessChange(Request);
        if (status == STATUS_PENDING) {
            return; // 请求已放入等待队列，不完成
        }
        break;

    case IOCTL_DDCBRT_REPORT_BRIGHTNESS:
        status = DDCBrt_HandleReportBrightness(Request);
        break;

    case IOCTL_DDCBRT_QUERY_MONITORS:
        {
            PDDCBRT_MONITOR_LIST outputBuffer = NULL;
            status = WdfRequestRetrieveOutputBuffer(
                Request,
                sizeof(DDCBRT_MONITOR_LIST),
                (PVOID *)&outputBuffer,
                NULL
            );

            if (NT_SUCCESS(status) && g_ControlDevice != NULL) {
                PCONTROL_DEVICE_CONTEXT ctrlCtx =
                    ControlGetDeviceContext(g_ControlDevice);

                WdfSpinLockAcquire(ctrlCtx->Lock);
                outputBuffer->Count = ctrlCtx->MonitorCount;
                for (ULONG i = 0; i < ctrlCtx->MonitorCount &&
                     i < DDCBRT_MAX_MONITORS; i++) {
                    outputBuffer->Monitors[i] = ctrlCtx->Monitors[i].Info;
                }
                WdfSpinLockRelease(ctrlCtx->Lock);

                WdfRequestCompleteWithInformation(
                    Request, STATUS_SUCCESS, sizeof(DDCBRT_MONITOR_LIST));
                return;
            }
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}

//
// 处理 IOCTL_DDCBRT_REGISTER_MONITOR
// 服务注册一个 DDC/CI 可用的显示器
//
NTSTATUS
DDCBrt_HandleRegisterMonitor(
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PDDCBRT_MONITOR_INFO inputBuffer = NULL;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(DDCBRT_MONITOR_INFO),
        (PVOID *)&inputBuffer,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (g_ControlDevice == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    PCONTROL_DEVICE_CONTEXT ctrlCtx =
        ControlGetDeviceContext(g_ControlDevice);

    WdfSpinLockAcquire(ctrlCtx->Lock);

    if (ctrlCtx->MonitorCount >= DDCBRT_MAX_MONITORS) {
        WdfSpinLockRelease(ctrlCtx->Lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // 查找空闲槽位
    ULONG slot = ctrlCtx->MonitorCount;
    ctrlCtx->Monitors[slot].Active = TRUE;
    ctrlCtx->Monitors[slot].Info = *inputBuffer;
    ctrlCtx->Monitors[slot].CurrentBrightnessPercent =
        (UCHAR)((inputBuffer->CurrentBrightness * 100) /
                 (inputBuffer->MaxBrightness > 0 ?
                  inputBuffer->MaxBrightness : 100));
    ctrlCtx->Monitors[slot].ACBrightness =
        ctrlCtx->Monitors[slot].CurrentBrightnessPercent;
    ctrlCtx->Monitors[slot].DCBrightness =
        ctrlCtx->Monitors[slot].CurrentBrightnessPercent;
    ctrlCtx->MonitorCount++;

    WdfSpinLockRelease(ctrlCtx->Lock);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 注册显示器 #%lu '%ls' "
               "(亮度范围: %lu-%lu, 当前: %lu)\n",
               slot, inputBuffer->DisplayName,
               inputBuffer->MinBrightness,
               inputBuffer->MaxBrightness,
               inputBuffer->CurrentBrightness));

    // 同时更新过滤设备的状态
    if (ctrlCtx->FilterDeviceCount > 0) {
        WDFDEVICE filterDevice = ctrlCtx->FilterDevices[0];
        PFILTER_DEVICE_CONTEXT filterCtx =
            FilterGetDeviceContext(filterDevice);

        WdfSpinLockAcquire(filterCtx->Lock);
        if (slot < DDCBRT_MAX_MONITORS) {
            filterCtx->Monitors[slot].Active = TRUE;
            filterCtx->Monitors[slot].Info = *inputBuffer;
            filterCtx->Monitors[slot].CurrentBrightnessPercent =
                ctrlCtx->Monitors[slot].CurrentBrightnessPercent;
            filterCtx->Monitors[slot].ACBrightness =
                ctrlCtx->Monitors[slot].ACBrightness;
            filterCtx->Monitors[slot].DCBrightness =
                ctrlCtx->Monitors[slot].DCBrightness;
            filterCtx->MonitorCount = ctrlCtx->MonitorCount;
        }
        WdfSpinLockRelease(filterCtx->Lock);
    }

    return STATUS_SUCCESS;
}

//
// 处理 IOCTL_DDCBRT_UNREGISTER_MONITOR
//
NTSTATUS
DDCBrt_HandleUnregisterMonitor(
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PDDCBRT_MONITOR_ID inputBuffer = NULL;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(DDCBRT_MONITOR_ID),
        (PVOID *)&inputBuffer,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (g_ControlDevice == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    PCONTROL_DEVICE_CONTEXT ctrlCtx =
        ControlGetDeviceContext(g_ControlDevice);

    WdfSpinLockAcquire(ctrlCtx->Lock);

    ULONG index = inputBuffer->MonitorIndex;
    if (index < DDCBRT_MAX_MONITORS && ctrlCtx->Monitors[index].Active) {
        ctrlCtx->Monitors[index].Active = FALSE;
        RtlZeroMemory(&ctrlCtx->Monitors[index],
                       sizeof(MONITOR_BRIGHTNESS_STATE));

        // 重新计算活跃显示器数量
        ULONG activeCount = 0;
        for (ULONG i = 0; i < DDCBRT_MAX_MONITORS; i++) {
            if (ctrlCtx->Monitors[i].Active) {
                activeCount = i + 1;
            }
        }
        ctrlCtx->MonitorCount = activeCount;
    }

    WdfSpinLockRelease(ctrlCtx->Lock);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "DDCBrightness: 注销显示器 #%lu\n", index));

    return STATUS_SUCCESS;
}

//
// 处理 IOCTL_DDCBRT_WAIT_BRIGHTNESS_CHANGE
// 反向调用: 将请求放入等待队列，亮度变化时完成
//
NTSTATUS
DDCBrt_HandleWaitBrightnessChange(
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;

    if (g_ControlDevice == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    PCONTROL_DEVICE_CONTEXT ctrlCtx =
        ControlGetDeviceContext(g_ControlDevice);

    // 将请求放入手动队列
    status = WdfRequestForwardToIoQueue(Request,
                                         ctrlCtx->BrightnessWaitQueue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "DDCBrightness: 放入等待队列失败 0x%x\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL,
               "DDCBrightness: 亮度变化等待请求已入队\n"));

    return STATUS_PENDING;
}

//
// 处理 IOCTL_DDCBRT_REPORT_BRIGHTNESS
// 服务向驱动报告当前实际亮度
//
NTSTATUS
DDCBrt_HandleReportBrightness(
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PDDCBRT_BRIGHTNESS_REPORT inputBuffer = NULL;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(DDCBRT_BRIGHTNESS_REPORT),
        (PVOID *)&inputBuffer,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (g_ControlDevice == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    PCONTROL_DEVICE_CONTEXT ctrlCtx =
        ControlGetDeviceContext(g_ControlDevice);

    WdfSpinLockAcquire(ctrlCtx->Lock);

    ULONG index = inputBuffer->MonitorIndex;
    if (index < DDCBRT_MAX_MONITORS && ctrlCtx->Monitors[index].Active) {
        ctrlCtx->Monitors[index].CurrentBrightnessPercent =
            (UCHAR)inputBuffer->BrightnessPercent;
        ctrlCtx->Monitors[index].ACBrightness =
            (UCHAR)inputBuffer->BrightnessPercent;
        ctrlCtx->Monitors[index].DCBrightness =
            (UCHAR)inputBuffer->BrightnessPercent;
    }

    WdfSpinLockRelease(ctrlCtx->Lock);

    // 同步到过滤设备
    if (ctrlCtx->FilterDeviceCount > 0) {
        WDFDEVICE filterDevice = ctrlCtx->FilterDevices[0];
        PFILTER_DEVICE_CONTEXT filterCtx =
            FilterGetDeviceContext(filterDevice);

        WdfSpinLockAcquire(filterCtx->Lock);
        if (index < DDCBRT_MAX_MONITORS) {
            filterCtx->Monitors[index].CurrentBrightnessPercent =
                (UCHAR)inputBuffer->BrightnessPercent;
            filterCtx->Monitors[index].ACBrightness =
                (UCHAR)inputBuffer->BrightnessPercent;
            filterCtx->Monitors[index].DCBrightness =
                (UCHAR)inputBuffer->BrightnessPercent;
        }
        WdfSpinLockRelease(filterCtx->Lock);
    }

    return STATUS_SUCCESS;
}

//
// 通知服务亮度变化
// 完成等待队列中的反向调用请求
//
VOID
DDCBrt_NotifyBrightnessChange(
    _In_ ULONG MonitorIndex,
    _In_ ULONG BrightnessPercent,
    _In_ UCHAR DisplayPolicy
)
{
    NTSTATUS status;
    WDFREQUEST request;

    if (g_ControlDevice == NULL) {
        return;
    }

    PCONTROL_DEVICE_CONTEXT ctrlCtx =
        ControlGetDeviceContext(g_ControlDevice);

    // 从等待队列中取出一个请求
    status = WdfIoQueueRetrieveNextRequest(
        ctrlCtx->BrightnessWaitQueue,
        &request
    );

    if (!NT_SUCCESS(status)) {
        // 没有等待的请求 (服务可能还没启动或没有发送等待请求)
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "DDCBrightness: 没有等待的亮度变化请求\n"));
        return;
    }

    // 填充输出缓冲区
    PDDCBRT_BRIGHTNESS_REQUEST outputBuffer = NULL;
    status = WdfRequestRetrieveOutputBuffer(
        request,
        sizeof(DDCBRT_BRIGHTNESS_REQUEST),
        (PVOID *)&outputBuffer,
        NULL
    );

    if (NT_SUCCESS(status)) {
        outputBuffer->MonitorIndex = MonitorIndex;
        outputBuffer->BrightnessPercent = BrightnessPercent;
        outputBuffer->DisplayPolicy = DisplayPolicy;

        WdfRequestCompleteWithInformation(
            request, STATUS_SUCCESS, sizeof(DDCBRT_BRIGHTNESS_REQUEST));

        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "DDCBrightness: 亮度变化通知已发送 "
                   "(Monitor=%lu, Brightness=%lu%%)\n",
                   MonitorIndex, BrightnessPercent));
    } else {
        WdfRequestComplete(request, status);
    }
}
