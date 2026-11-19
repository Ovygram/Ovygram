"""Restrictions of the plugin host process on Windows.

The host is created suspended, and before its first instruction runs:

- its token is lowered to the low integrity level. Windows then refuses it
  write and read access to the memory of the client, window messages, hooks
  and synthetic input to the client's windows, and writing to the user's
  files. Reading the user's files stays possible: that needs an AppContainer.
- it is put into a job object that kills it with the client and cuts it off
  the clipboard, the desktop and the window handles of other processes.

The plugin data directory is labelled low so that the host can write there.
Every step that fails stops the start: there is no weaker mode.
"""

import ctypes
from ctypes import wintypes
import os
import subprocess

from ovygram_sandbox import SandboxError

CREATE_SUSPENDED = 0x00000004
CREATE_NO_WINDOW = 0x08000000

_LOW_INTEGRITY = "S-1-16-4096"
_LOW_RID = 0x1000
_TOKEN_QUERY = 0x0008
_TOKEN_ADJUST_DEFAULT = 0x0080
_TOKEN_INTEGRITY_LEVEL = 25  # TOKEN_INFORMATION_CLASS.TokenIntegrityLevel
_SE_GROUP_INTEGRITY = 0x00000020
_JOB_BASIC_UI_RESTRICTIONS = 4
_JOB_EXTENDED_LIMIT_INFORMATION = 9
_JOB_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
# Handles, clipboard, system parameters, display settings, global atoms,
# desktops and ExitWindows.
_JOB_UI_ALL = 0x000000FF


class _SidAndAttributes(ctypes.Structure):
    _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wintypes.DWORD)]


class _TokenMandatoryLabel(ctypes.Structure):
    _fields_ = [("Label", _SidAndAttributes)]


class _BasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD)]


class _IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class _ExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _BasicLimitInformation), ("IoInfo", _IoCounters),
        ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


class _UiRestrictions(ctypes.Structure):
    _fields_ = [("UIRestrictionsClass", wintypes.DWORD)]


def _libraries():
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")
    handle, pointer, dword = wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD
    for library, name, result, arguments in (
        (kernel32, "CloseHandle", wintypes.BOOL, (handle,)),
        (kernel32, "LocalFree", pointer, (pointer,)),
        (kernel32, "CreateJobObjectW", handle, (pointer, wintypes.LPCWSTR)),
        (kernel32, "SetInformationJobObject", wintypes.BOOL, (handle, ctypes.c_int, pointer, dword)),
        (kernel32, "AssignProcessToJobObject", wintypes.BOOL, (handle, handle)),
        (advapi32, "OpenProcessToken", wintypes.BOOL, (handle, dword, ctypes.POINTER(handle))),
        (advapi32, "ConvertStringSidToSidW", wintypes.BOOL, (wintypes.LPCWSTR, ctypes.POINTER(pointer))),
        (advapi32, "GetLengthSid", dword, (pointer,)),
        (advapi32, "SetTokenInformation", wintypes.BOOL, (handle, ctypes.c_int, pointer, dword)),
        (advapi32, "GetTokenInformation", wintypes.BOOL,
         (handle, ctypes.c_int, pointer, dword, ctypes.POINTER(dword))),
        (advapi32, "GetSidSubAuthorityCount", ctypes.POINTER(ctypes.c_ubyte), (pointer,)),
        (advapi32, "GetSidSubAuthority", ctypes.POINTER(dword), (pointer, dword)),
        (ntdll, "NtResumeProcess", ctypes.c_long, (handle,)),
    ):
        function = getattr(library, name)
        function.restype = result
        function.argtypes = arguments
    return kernel32, advapi32, ntdll


def _error(step):
    return SandboxError(f"Изоляция плагинов: {step}: {ctypes.FormatError(ctypes.get_last_error()).strip()}")


def label_low(directory):
    """Lets a low-integrity process write into the directory and below."""
    icacls = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "icacls.exe")
    try:
        result = subprocess.run(
            # /L: a link or junction a plugin left there is labelled itself,
            # the walk never follows it out of the directory.
            [icacls, str(directory), "/setintegritylevel", "(OI)(CI)L", "/T", "/L", "/C", "/Q"],
            stdin=subprocess.DEVNULL, capture_output=True, creationflags=CREATE_NO_WINDOW)
    except OSError as error:
        raise SandboxError(f"Изоляция плагинов: icacls: {error}") from error
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).decode("utf-8", "replace").strip()[:200]
        raise SandboxError(f"Изоляция плагинов: icacls: {detail}")


def _lower_integrity(kernel32, advapi32, process):
    token = wintypes.HANDLE()
    if not advapi32.OpenProcessToken(process, _TOKEN_QUERY | _TOKEN_ADJUST_DEFAULT, ctypes.byref(token)):
        raise _error("OpenProcessToken")
    sid = ctypes.c_void_p()
    try:
        if not advapi32.ConvertStringSidToSidW(_LOW_INTEGRITY, ctypes.byref(sid)):
            raise _error("ConvertStringSidToSid")
        label = _TokenMandatoryLabel()
        label.Label.Sid = sid
        label.Label.Attributes = _SE_GROUP_INTEGRITY
        if not advapi32.SetTokenInformation(
                token, _TOKEN_INTEGRITY_LEVEL, ctypes.byref(label),
                ctypes.sizeof(label) + advapi32.GetLengthSid(sid)):
            raise _error("SetTokenInformation")
        # Read it back: the start must not go on with the level unchanged.
        size = wintypes.DWORD()
        advapi32.GetTokenInformation(token, _TOKEN_INTEGRITY_LEVEL, None, 0, ctypes.byref(size))
        buffer = ctypes.create_string_buffer(size.value)
        if not advapi32.GetTokenInformation(
                token, _TOKEN_INTEGRITY_LEVEL, buffer, size, ctypes.byref(size)):
            raise _error("GetTokenInformation")
        current = ctypes.cast(buffer, ctypes.POINTER(_TokenMandatoryLabel)).contents.Label.Sid
        count = advapi32.GetSidSubAuthorityCount(current).contents.value
        if advapi32.GetSidSubAuthority(current, count - 1).contents.value > _LOW_RID:
            raise SandboxError("Изоляция плагинов: уровень целостности процесса не понижен.")
    finally:
        if sid:
            kernel32.LocalFree(sid)
        kernel32.CloseHandle(token)


def confine(process):
    """Restricts and resumes a subprocess.Popen created with CREATE_SUSPENDED.

    Returns the job handle; the process lives until release() closes it.
    On SandboxError the caller kills the process, which never ran.
    """
    kernel32, advapi32, ntdll = _libraries()
    handle = wintypes.HANDLE(int(process._handle))
    _lower_integrity(kernel32, advapi32, handle)
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        raise _error("CreateJobObject")
    try:
        limits = _ExtendedLimitInformation()
        limits.BasicLimitInformation.LimitFlags = _JOB_LIMIT_KILL_ON_JOB_CLOSE
        interface = _UiRestrictions(_JOB_UI_ALL)
        if not kernel32.SetInformationJobObject(
                job, _JOB_EXTENDED_LIMIT_INFORMATION, ctypes.byref(limits), ctypes.sizeof(limits)
        ) or not kernel32.SetInformationJobObject(
                job, _JOB_BASIC_UI_RESTRICTIONS, ctypes.byref(interface), ctypes.sizeof(interface)):
            raise _error("SetInformationJobObject")
        if not kernel32.AssignProcessToJobObject(job, handle):
            raise _error("AssignProcessToJobObject")
        status = ntdll.NtResumeProcess(handle)
        if status != 0:
            raise SandboxError(f"Изоляция плагинов: NtResumeProcess: 0x{status & 0xFFFFFFFF:08X}")
    except BaseException:
        kernel32.CloseHandle(job)
        raise
    return job


def release(job):
    ctypes.WinDLL("kernel32").CloseHandle(wintypes.HANDLE(job))
