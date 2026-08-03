"""Cross-process serialization for one Unreal Editor command endpoint."""

from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path
from typing import Any


def _lock_key(node_id: str) -> str:
    return hashlib.sha256(node_id.encode("utf-8")).hexdigest()


class EditorCommandLock:
    """Block concurrent uepy command connections to the same editor node.

    Unreal's Python Remote Execution command endpoint accepts one active local
    request reliably. This operating-system lock keeps independent uepy
    processes from opening competing command connections while still allowing
    requests to different editor nodes to proceed in parallel.
    """

    def __init__(self, node_id: str) -> None:
        if not node_id:
            raise ValueError("An Unreal Editor node ID is required for command locking.")
        self.node_id = node_id
        self._acquired = False
        self._handle: Any | None = None
        self._file: Any | None = None

    def __enter__(self) -> "EditorCommandLock":
        self.acquire()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.release()

    def acquire(self) -> None:
        if self._acquired:
            return
        if os.name == "nt":
            self._acquire_windows()
        else:
            self._acquire_posix()
        self._acquired = True

    def release(self) -> None:
        if not self._acquired:
            return
        try:
            if os.name == "nt":
                self._release_windows()
            else:
                self._release_posix()
        finally:
            self._acquired = False

    def _acquire_windows(self) -> None:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_mutex = kernel32.CreateMutexW
        create_mutex.argtypes = (wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR)
        create_mutex.restype = wintypes.HANDLE
        wait_for_single_object = kernel32.WaitForSingleObject
        wait_for_single_object.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        wait_for_single_object.restype = wintypes.DWORD

        handle = create_mutex(None, False, f"Local\\uepy-editor-command-{_lock_key(self.node_id)}")
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())

        wait_result = wait_for_single_object(handle, 0xFFFFFFFF)
        if wait_result not in (0x00000000, 0x00000080):
            error_code = ctypes.get_last_error()
            kernel32.CloseHandle(handle)
            if wait_result == 0xFFFFFFFF:
                raise ctypes.WinError(error_code)
            raise OSError(f"Unexpected Windows mutex wait result: 0x{wait_result:08X}")
        self._handle = handle

    def _release_windows(self) -> None:
        import ctypes
        from ctypes import wintypes

        if self._handle is None:
            return
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        release_mutex = kernel32.ReleaseMutex
        release_mutex.argtypes = (wintypes.HANDLE,)
        release_mutex.restype = wintypes.BOOL
        close_handle = kernel32.CloseHandle
        close_handle.argtypes = (wintypes.HANDLE,)
        close_handle.restype = wintypes.BOOL

        handle = self._handle
        self._handle = None
        released = bool(release_mutex(handle))
        error_code = ctypes.get_last_error() if not released else 0
        close_handle(handle)
        if not released:
            raise ctypes.WinError(error_code)

    def _acquire_posix(self) -> None:
        import fcntl

        path = Path(tempfile.gettempdir()) / f"uepy-editor-command-{_lock_key(self.node_id)}.lock"
        lock_file = path.open("a+b")
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        except Exception:
            lock_file.close()
            raise
        self._file = lock_file

    def _release_posix(self) -> None:
        import fcntl

        if self._file is None:
            return
        lock_file = self._file
        self._file = None
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
        finally:
            lock_file.close()
