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
DETACHED_PROCESS = 0x00000008
CREATE_NO_WINDOW = 0x08000000

_LOW_INTEGRITY = "S-1-16-4096"
_LOW_RID = 0x1000
_TOKEN_QUERY = 0x0008
_TOKEN_ADJUST_DEFAULT = 0x0080
_TOKEN_INTEGRITY_LEVEL = 25  # TOKEN_INFORMATION_CLASS.TokenIntegrityLevel
_TOKEN_LOGON_SID = 28
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


class _TokenGroups(ctypes.Structure):
    _fields_ = [("GroupCount", wintypes.DWORD), ("Groups", _SidAndAttributes * 1)]


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


def grant_helper_read(directory):
    # Only the private staging directory holding the verified executable.
    icacls = os.path.join(os.environ["SystemRoot"], "System32", "icacls.exe")
    subprocess.run([icacls, str(directory), "/grant", "*S-1-5-12:(OI)(CI)(RX)"],
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   creationflags=CREATE_NO_WINDOW, check=True)


def run_restricted_helper(binary, arguments):
    """Called in an already low-integrity, job-confined launcher. The additional
    restricted-SID access check excludes files accessible only to the user."""
    kernel, security, _ = _libraries()
    handle, dword, pointer = wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p

    class Startup(ctypes.Structure):
        _fields_ = [("cb", dword), ("reserved", pointer), ("desktop", pointer), ("title", pointer),
                    *((name, dword) for name in ("x", "y", "width", "height", "cols", "rows",
                                                 "fill", "flags")),
                    ("show", wintypes.WORD), ("reservedSize", wintypes.WORD), ("reserved2", pointer),
                    ("input", handle), ("output", handle), ("error", handle)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("process", handle), ("thread", handle), ("processId", dword), ("threadId", dword)]

    for lib, name, result, args in (
        (kernel, "GetCurrentProcess", handle, ()),
        (kernel, "GetStdHandle", handle, (dword,)),
        (kernel, "SetHandleInformation", wintypes.BOOL, (handle, dword, dword)),
        (kernel, "WaitForSingleObject", dword, (handle, dword)),
        (kernel, "GetExitCodeProcess", wintypes.BOOL, (handle, ctypes.POINTER(dword))),
        (security, "CreateRestrictedToken", wintypes.BOOL, (
            handle, dword, dword, pointer, dword, pointer, dword, pointer, ctypes.POINTER(handle))),
        (security, "CreateProcessAsUserW", wintypes.BOOL, (
            handle, wintypes.LPCWSTR, wintypes.LPWSTR, pointer, pointer, wintypes.BOOL, dword,
            pointer, wintypes.LPCWSTR, ctypes.POINTER(Startup), ctypes.POINTER(ProcessInfo))),
    ):
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    original, restricted = handle(), handle()
    info = ProcessInfo()
    sids = []
    try:
        if not security.OpenProcessToken(kernel.GetCurrentProcess(), _TOKEN_QUERY | 2 | 1,
                                         ctypes.byref(original)):
            raise _error("OpenProcessToken")
        size = dword()
        security.GetTokenInformation(original, _TOKEN_LOGON_SID, None, 0, ctypes.byref(size))
        logon_info = ctypes.create_string_buffer(size.value)
        if not security.GetTokenInformation(
                original, _TOKEN_LOGON_SID, logon_info, size, ctypes.byref(size)):
            raise _error("GetTokenInformation(TokenLogonSid)")
        logon_groups = ctypes.cast(logon_info, ctypes.POINTER(_TokenGroups)).contents
        if logon_groups.GroupCount != 1:
            raise SandboxError("The helper launcher has no Windows logon SID.")
        # The logon SID can access the session's window station and objects
        # needed by the loader. Everyone and Users can read Windows resources.
        # Restricted Code can read the staged engine. The user's SID is absent.
        restrictions = (_SidAndAttributes * 4)()
        for i, text in enumerate(("S-1-1-0", "S-1-5-32-545", "S-1-5-12")):
            sid = pointer()
            if not security.ConvertStringSidToSidW(text, ctypes.byref(sid)):
                raise _error("ConvertStringSidToSid")
            sids.append(sid)
            restrictions[i].Sid = sid
        restrictions[3].Sid = logon_groups.Groups[0].Sid
        if not security.CreateRestrictedToken(original, 1, 0, None, 0, None,
                                              len(restrictions), restrictions, ctypes.byref(restricted)):
            raise _error("CreateRestrictedToken")
        startup = Startup(cb=ctypes.sizeof(Startup), flags=0x100)
        for field, number in (("input", -10), ("output", -11), ("error", -12)):
            value = kernel.GetStdHandle(number)
            if not kernel.SetHandleInformation(value, 1, 1):
                raise _error("SetHandleInformation")
            setattr(startup, field, value)
        command = ctypes.create_unicode_buffer(subprocess.list2cmdline([binary, *arguments]))
        # The dedicated launcher inherited only its redirected standard streams.
        if not security.CreateProcessAsUserW(restricted, binary, command, None, None, True,
                                             DETACHED_PROCESS, None, os.path.dirname(binary),
                                             ctypes.byref(startup), ctypes.byref(info)):
            raise _error("CreateProcessAsUser")
        if kernel.WaitForSingleObject(info.process, 0xffffffff) != 0:
            raise _error("WaitForSingleObject")
        code = dword()
        if not kernel.GetExitCodeProcess(info.process, ctypes.byref(code)):
            raise _error("GetExitCodeProcess")
        return code.value
    finally:
        for obj in (info.process, info.thread, restricted, original):
            if obj:
                kernel.CloseHandle(obj)
        for sid in sids:
            kernel.LocalFree(sid)
