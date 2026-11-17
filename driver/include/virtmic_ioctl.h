/*++
  Shared IOCTL contract: usermode feeder -> VirtMic capture driver.
  PCM geometry matches shared/include/virtmic/pcm.hpp (48 kHz mono float32, 480 samples).
--*/
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#define VIRTMIC_FEED_DEVICE_NAME     L"\\Device\\VirtMicFeed"
#define VIRTMIC_FEED_SYMLINK_NAME    L"\\DosDevices\\VirtMicFeed"
#define VIRTMIC_FEED_USER_PATH       L"\\\\.\\VirtMicFeed"

#define VIRTMIC_IOCTL_SAMPLES_FRAME  480u
#define VIRTMIC_IOCTL_CHANNELS_IN    1u

#define IOCTL_VIRTMIC_PUSH_FRAME \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A0, METHOD_BUFFERED, FILE_WRITE_DATA)

/* Input buffer: VIRTMIC_IOCTL_SAMPLES_FRAME floats (mono, little-endian). */
