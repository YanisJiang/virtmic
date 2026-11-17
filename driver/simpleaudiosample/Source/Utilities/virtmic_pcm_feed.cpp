/*++
  VirtMic PCM feed ring + control device (IOCTL push from usermode feeder).
--*/
#include "definitions.h"
#include "virtmic_pcm_feed.h"

PDRIVER_DISPATCH g_PcCreate = NULL;
PDRIVER_DISPATCH g_PcClose = NULL;
PDRIVER_DISPATCH g_PcDeviceControl = NULL;

static PDEVICE_OBJECT g_ControlDevice = NULL;
static UNICODE_STRING g_ControlSymlink = {};

static KSPIN_LOCK g_FeedLock;
static LONG g_FeedReady = 0;

static BYTE g_Ring[VIRTMIC_FEED_RING_FRAMES][VIRTMIC_FEED_BYTES_PER_FRAME];
static ULONG g_WriteIndex = 0; /* frames pushed */
static ULONG g_ReadIndex = 0;  /* frames consumed */
static ULONG g_PartialOffset = 0; /* bytes into current read frame */

static LONG ConvertFloatToPcm32(_In_ float Value)
{
    if (Value > 1.0f) Value = 1.0f;
    if (Value < -1.0f) Value = -1.0f;
    return (LONG)(Value * 2147483647.0f);
}

NTSTATUS VirtMicFeedInitialize()
{
    KeInitializeSpinLock(&g_FeedLock);
    RtlZeroMemory(g_Ring, sizeof(g_Ring));
    g_WriteIndex = 0;
    g_ReadIndex = 0;
    g_PartialOffset = 0;
    InterlockedExchange(&g_FeedReady, 1);
    return STATUS_SUCCESS;
}

VOID VirtMicFeedCleanup()
{
    InterlockedExchange(&g_FeedReady, 0);
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_FeedLock, &oldIrql);
    g_WriteIndex = 0;
    g_ReadIndex = 0;
    g_PartialOffset = 0;
    RtlZeroMemory(g_Ring, sizeof(g_Ring));
    KeReleaseSpinLock(&g_FeedLock, oldIrql);
}

NTSTATUS VirtMicFeedPushFloatMono(
    _In_reads_(SampleCount) const float* Samples,
    _In_ ULONG SampleCount
)
{
    if (Samples == NULL || SampleCount != VIRTMIC_FEED_SAMPLES_FRAME)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (InterlockedCompareExchange(&g_FeedReady, 1, 1) == 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    BYTE frame[VIRTMIC_FEED_BYTES_PER_FRAME];
    LONG* pcm = reinterpret_cast<LONG*>(frame);

    KFLOATING_SAVE saveData;
    NTSTATUS status = KeSaveFloatingPointState(&saveData);
    if (!NT_SUCCESS(status))
    {
        RtlZeroMemory(frame, sizeof(frame));
    }
    else
    {
        for (ULONG i = 0; i < VIRTMIC_FEED_SAMPLES_FRAME; ++i)
        {
            LONG s = ConvertFloatToPcm32(Samples[i]);
            pcm[i * VIRTMIC_FEED_OUT_CHANNELS + 0] = s;
            pcm[i * VIRTMIC_FEED_OUT_CHANNELS + 1] = s;
        }
        KeRestoreFloatingPointState(&saveData);
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_FeedLock, &oldIrql);

    ULONG depth = g_WriteIndex - g_ReadIndex;
    if (depth >= VIRTMIC_FEED_RING_FRAMES)
    {
        /* Drop oldest frame. */
        ++g_ReadIndex;
        g_PartialOffset = 0;
    }

    ULONG slot = g_WriteIndex % VIRTMIC_FEED_RING_FRAMES;
    RtlCopyMemory(g_Ring[slot], frame, VIRTMIC_FEED_BYTES_PER_FRAME);
    ++g_WriteIndex;

    KeReleaseSpinLock(&g_FeedLock, oldIrql);
    return STATUS_SUCCESS;
}

VOID VirtMicFeedFillBytes(
    _Out_writes_bytes_(ByteCount) BYTE* Buffer,
    _In_ size_t ByteCount,
    _Out_opt_ BOOLEAN* Underrun
)
{
    if (Underrun)
    {
        *Underrun = FALSE;
    }

    if (Buffer == NULL || ByteCount == 0)
    {
        return;
    }

    if (InterlockedCompareExchange(&g_FeedReady, 1, 1) == 0)
    {
        RtlZeroMemory(Buffer, ByteCount);
        if (Underrun)
        {
            *Underrun = TRUE;
        }
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_FeedLock, &oldIrql);

    size_t remaining = ByteCount;
    BYTE* dst = Buffer;
    BOOLEAN sawUnderrun = FALSE;

    while (remaining > 0)
    {
        if (g_WriteIndex == g_ReadIndex)
        {
            RtlZeroMemory(dst, remaining);
            sawUnderrun = TRUE;
            break;
        }

        ULONG slot = g_ReadIndex % VIRTMIC_FEED_RING_FRAMES;
        ULONG avail = VIRTMIC_FEED_BYTES_PER_FRAME - g_PartialOffset;
        ULONG copy = (ULONG)min(remaining, (size_t)avail);

        RtlCopyMemory(dst, g_Ring[slot] + g_PartialOffset, copy);
        dst += copy;
        remaining -= copy;
        g_PartialOffset += copy;

        if (g_PartialOffset >= VIRTMIC_FEED_BYTES_PER_FRAME)
        {
            g_PartialOffset = 0;
            ++g_ReadIndex;
        }
    }

    KeReleaseSpinLock(&g_FeedLock, oldIrql);

    if (Underrun)
    {
        *Underrun = sawUnderrun;
    }
}

NTSTATUS VirtMicFeedCreateControlDevice(_In_ PDRIVER_OBJECT DriverObject)
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;

    if (g_ControlDevice != NULL)
    {
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&deviceName, VIRTMIC_FEED_DEVICE_NAME);
    RtlInitUnicodeString(&symlinkName, VIRTMIC_FEED_SYMLINK_NAME);

    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_ControlDevice);

    if (!NT_SUCCESS(status))
    {
        g_ControlDevice = NULL;
        return status;
    }

    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
        return status;
    }

    g_ControlSymlink = symlinkName;
    g_ControlDevice->Flags |= DO_BUFFERED_IO;
    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

VOID VirtMicFeedDeleteControlDevice()
{
    if (g_ControlDevice == NULL)
    {
        return;
    }

    UNICODE_STRING symlinkName;
    RtlInitUnicodeString(&symlinkName, VIRTMIC_FEED_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlinkName);
    IoDeleteDevice(g_ControlDevice);
    g_ControlDevice = NULL;
}

BOOLEAN VirtMicFeedIsControlDevice(_In_ PDEVICE_OBJECT DeviceObject)
{
    return (DeviceObject != NULL && DeviceObject == g_ControlDevice);
}

NTSTATUS VirtMicFeedDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    if (VirtMicFeedIsControlDevice(DeviceObject))
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    if (g_PcCreate)
    {
        return g_PcCreate(DeviceObject, Irp);
    }

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS VirtMicFeedDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    if (VirtMicFeedIsControlDevice(DeviceObject))
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    if (g_PcClose)
    {
        return g_PcClose(DeviceObject, Irp);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS VirtMicFeedDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    if (!VirtMicFeedIsControlDevice(DeviceObject))
    {
        if (g_PcDeviceControl)
        {
            return g_PcDeviceControl(DeviceObject, Irp);
        }
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_VIRTMIC_PUSH_FRAME)
    {
        const ULONG expected =
            VIRTMIC_FEED_SAMPLES_FRAME * sizeof(float);

        if (stack->Parameters.DeviceIoControl.InputBufferLength < expected)
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            auto* samples = static_cast<float*>(Irp->AssociatedIrp.SystemBuffer);
            status = VirtMicFeedPushFloatMono(samples, VIRTMIC_FEED_SAMPLES_FRAME);
            if (NT_SUCCESS(status))
            {
                info = expected;
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
