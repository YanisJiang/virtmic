/*++
  Kernel ring buffer fed by IOCTL_VIRTMIC_PUSH_FRAME; capture streams pull PCM.
--*/
#pragma once

#include "definitions.h"
#include "virtmic_ioctl.h"

#define VIRTMIC_FEED_RING_FRAMES     32u
#define VIRTMIC_FEED_OUT_CHANNELS    2u   /* stereo duplicate of mono */
#define VIRTMIC_FEED_BYTES_PER_SAMPLE 4u  /* 32-bit PCM */
#define VIRTMIC_FEED_SAMPLES_FRAME   VIRTMIC_IOCTL_SAMPLES_FRAME
#define VIRTMIC_FEED_BYTES_PER_FRAME \
  (VIRTMIC_FEED_SAMPLES_FRAME * VIRTMIC_FEED_OUT_CHANNELS * VIRTMIC_FEED_BYTES_PER_SAMPLE)

NTSTATUS VirtMicFeedInitialize();
VOID     VirtMicFeedCleanup();

NTSTATUS VirtMicFeedPushFloatMono(
    _In_reads_(SampleCount) const float* Samples,
    _In_ ULONG SampleCount
);

/* Fill capture DMA bytes; silence on underrun. Sets *Underrun if ring empty. */
VOID VirtMicFeedFillBytes(
    _Out_writes_bytes_(ByteCount) BYTE* Buffer,
    _In_ size_t ByteCount,
    _Out_opt_ BOOLEAN* Underrun
);

NTSTATUS VirtMicFeedCreateControlDevice(_In_ PDRIVER_OBJECT DriverObject);
VOID     VirtMicFeedDeleteControlDevice();

BOOLEAN  VirtMicFeedIsControlDevice(_In_ PDEVICE_OBJECT DeviceObject);

NTSTATUS VirtMicFeedDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
NTSTATUS VirtMicFeedDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
NTSTATUS VirtMicFeedDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

extern PDRIVER_DISPATCH g_PcCreate;
extern PDRIVER_DISPATCH g_PcClose;
extern PDRIVER_DISPATCH g_PcDeviceControl;
