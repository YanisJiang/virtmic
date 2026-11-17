"""Named-pipe control server for SetPitchSemitones (same binary layout as C++)."""

from __future__ import annotations

import ctypes
import struct
import threading
from ctypes import wintypes
from typing import Callable

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PIPE_ACCESS_DUPLEX = 0x00000003
PIPE_TYPE_BYTE = 0x00000000
PIPE_READMODE_BYTE = 0x00000000
PIPE_WAIT = 0x00000000
PIPE_UNLIMITED_INSTANCES = 255
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

PROTOCOL_MAGIC = 0x564D4943
CMD_SET_PITCH = 6

CreateNamedPipeW = kernel32.CreateNamedPipeW
CreateNamedPipeW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.LPVOID,
]
CreateNamedPipeW.restype = wintypes.HANDLE

ConnectNamedPipe = kernel32.ConnectNamedPipe
ConnectNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
ConnectNamedPipe.restype = wintypes.BOOL

DisconnectNamedPipe = kernel32.DisconnectNamedPipe
DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
DisconnectNamedPipe.restype = wintypes.BOOL

ReadFile = kernel32.ReadFile
ReadFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
ReadFile.restype = wintypes.BOOL

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL

PIPE_NAME = r"\\.\pipe\VirtMicInferControl"


def _read_exact(handle: int, nbytes: int) -> bytes | None:
    buf = (ctypes.c_ubyte * nbytes)()
    got_total = 0
    while got_total < nbytes:
        got = wintypes.DWORD(0)
        ok = ReadFile(
            handle,
            ctypes.byref(buf, got_total),
            nbytes - got_total,
            ctypes.byref(got),
            None,
        )
        if not ok or got.value == 0:
            return None
        got_total += got.value
    return bytes(buf)


class PitchControlServer:
    def __init__(self, on_pitch: Callable[[int], None]) -> None:
        self._on_pitch = on_pitch
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, name="infer-ctl", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        # Wake a blocking ConnectNamedPipe by opening as client.
        try:
            CreateFile = kernel32.CreateFileW
            CreateFile.argtypes = [
                wintypes.LPCWSTR,
                wintypes.DWORD,
                wintypes.DWORD,
                wintypes.LPVOID,
                wintypes.DWORD,
                wintypes.DWORD,
                wintypes.HANDLE,
            ]
            CreateFile.restype = wintypes.HANDLE
            h = CreateFile(
                PIPE_NAME,
                0xC0000000,  # GENERIC_READ|GENERIC_WRITE
                0,
                None,
                3,  # OPEN_EXISTING
                0,
                None,
            )
            if h and h != INVALID_HANDLE_VALUE:
                CloseHandle(h)
        except Exception:
            pass
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None

    def _loop(self) -> None:
        while not self._stop.is_set():
            handle = CreateNamedPipeW(
                PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                None,
            )
            if not handle or handle == INVALID_HANDLE_VALUE:
                if self._stop.wait(0.05):
                    break
                continue
            connected = ConnectNamedPipe(handle, None)
            err = ctypes.get_last_error()
            if self._stop.is_set():
                CloseHandle(handle)
                break
            if not connected and err != 535:  # ERROR_PIPE_CONNECTED
                CloseHandle(handle)
                continue
            self._serve(handle)
            DisconnectNamedPipe(handle)
            CloseHandle(handle)

    def _serve(self, handle: int) -> None:
        hdr = _read_exact(handle, 12)
        if not hdr:
            return
        magic, typ, nbytes = struct.unpack("<III", hdr)
        if magic != PROTOCOL_MAGIC or nbytes > 64:
            return
        payload = b""
        if nbytes:
            payload = _read_exact(handle, nbytes)
            if payload is None:
                return
        if typ == CMD_SET_PITCH and nbytes == 4:
            (semitones,) = struct.unpack("<i", payload)
            self._on_pitch(semitones)
