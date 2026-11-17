"""Windows named shared-memory rings matching C++ ShmHeader layout."""

from __future__ import annotations

import ctypes
import struct
from ctypes import wintypes
from typing import Optional, Tuple

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

FILE_MAP_ALL_ACCESS = 0x000F001F
PAGE_READWRITE = 0x04
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

# Must match shared/include/virtmic/shm_layout.hpp + pcm.hpp + infer_shm.hpp
HEADER_FMT = "<IIIIIIII"  # 8 x uint32 little-endian
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SAMPLES_PER_FRAME = 480
BYTES_PER_FRAME = SAMPLES_PER_FRAME * 4
DEFAULT_FRAMES_CAPACITY = 32

IN_NAME = "Local\\VirtMicInferInV1"
OUT_NAME = "Local\\VirtMicInferOutV1"
IN_MAGIC = 0x564D494E
OUT_MAGIC = 0x564D4F55
VERSION = 1

OpenFileMappingW = kernel32.OpenFileMappingW
OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
OpenFileMappingW.restype = wintypes.HANDLE

MapViewOfFile = kernel32.MapViewOfFile
MapViewOfFile.argtypes = [
    wintypes.HANDLE,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.c_size_t,
]
MapViewOfFile.restype = wintypes.LPVOID

UnmapViewOfFile = kernel32.UnmapViewOfFile
UnmapViewOfFile.argtypes = [wintypes.LPCVOID]
UnmapViewOfFile.restype = wintypes.BOOL

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL


def mapping_bytes(frames_capacity: int = DEFAULT_FRAMES_CAPACITY) -> int:
    return HEADER_SIZE + frames_capacity * BYTES_PER_FRAME


class ShmRing:
    def __init__(self, name: str, expect_magic: int) -> None:
        self._name = name
        self._expect_magic = expect_magic
        self._handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, name)
        if not self._handle:
            raise OSError(f"OpenFileMappingW failed for {name!r}: {ctypes.get_last_error()}")
        size = mapping_bytes()
        self._view = MapViewOfFile(self._handle, FILE_MAP_ALL_ACCESS, 0, 0, size)
        if not self._view:
            err = ctypes.get_last_error()
            CloseHandle(self._handle)
            raise OSError(f"MapViewOfFile failed for {name!r}: {err}")
        self._buf = (ctypes.c_char * size).from_address(self._view)
        self._last_in_seq = 0
        magic, version, write_index, frames_capacity, *_ = struct.unpack_from(
            HEADER_FMT, self._buf, 0
        )
        if magic != expect_magic:
            self.close()
            raise OSError(
                f"{name}: magic mismatch got=0x{magic:08X} want=0x{expect_magic:08X}"
            )
        if version != VERSION:
            self.close()
            raise OSError(f"{name}: version mismatch {version}")
        self._frames_capacity = frames_capacity

    def close(self) -> None:
        if getattr(self, "_view", None):
            UnmapViewOfFile(self._view)
            self._view = None
        if getattr(self, "_handle", None):
            CloseHandle(self._handle)
            self._handle = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def write_index(self) -> int:
        return struct.unpack_from("<I", self._buf, 8)[0]

    def try_read_latest(self) -> Optional[Tuple[bytes, int]]:
        wi = self.write_index()
        if wi == 0 or wi == self._last_in_seq:
            return None
        slot = (wi - 1) % self._frames_capacity
        off = HEADER_SIZE + slot * BYTES_PER_FRAME
        data = bytes(self._buf[off : off + BYTES_PER_FRAME])
        self._last_in_seq = wi
        return data, wi

    def write_frame(self, pcm_bytes: bytes) -> None:
        if len(pcm_bytes) != BYTES_PER_FRAME:
            raise ValueError("bad frame size")
        wi = self.write_index()
        slot = wi % self._frames_capacity
        off = HEADER_SIZE + slot * BYTES_PER_FRAME
        self._buf[off : off + BYTES_PER_FRAME] = pcm_bytes
        # publish
        struct.pack_into("<I", self._buf, 8, wi + 1)
