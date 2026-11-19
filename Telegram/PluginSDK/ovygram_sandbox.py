"""OS restrictions of the plugin host process.

Linux: bubblewrap starts the host in its own user, PID, IPC and mount
namespaces. It sees the system libraries, the bundled Python and the plugin
packages read-only, the plugin data directory writable, and nothing else of
the user's files; there is no /proc of the client, no session D-Bus, X11 or
Wayland socket. A seccomp filter denies tracing, mounting, kernel keys and
nested namespaces.

The network stays shared only where Landlock can cut the host off abstract
UNIX sockets of outside processes (kernel 6.12+): an X server listens on one
and may accept any process of the user. Otherwise the host gets an empty
network namespace.

Windows: see ovygram_sandbox_windows.py. The host is created suspended and
confine() lowers its integrity level and puts it into a job object before
it runs.

Other platforms have no implementation: wrap() says so in its report and
the client shows it instead of hiding the weaker mode.
"""

import ctypes
import os
import platform
import shutil
import struct
import sys


class SandboxError(RuntimeError):
    pass


# Landlock.

_LANDLOCK_CREATE_RULESET = 444
_LANDLOCK_RESTRICT_SELF = 446
_LANDLOCK_CREATE_RULESET_VERSION = 1
_SCOPE_ABSTRACT_UNIX_SOCKET = 1
_SCOPE_SIGNAL = 2
_SCOPE_ABI = 6
_PR_SET_NO_NEW_PRIVS = 38


def _libc():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    return libc


def landlock_abi():
    """The Landlock version of the running kernel, 0 without Landlock."""
    if not sys.platform.startswith("linux"):
        return 0
    try:
        result = _libc().syscall(
            ctypes.c_long(_LANDLOCK_CREATE_RULESET), ctypes.c_void_p(None), ctypes.c_size_t(0),
            ctypes.c_uint(_LANDLOCK_CREATE_RULESET_VERSION))
    except (OSError, AttributeError):
        return 0
    return max(result, 0)


def apply_scope():
    """Cuts this process and its descendants off abstract UNIX sockets and
    signals of every process outside. Call before the first thread starts:
    Landlock restricts the calling thread and what it creates later."""
    libc = _libc()
    # struct landlock_ruleset_attr: handled_access_fs, handled_access_net, scoped.
    attributes = struct.pack("QQQ", 0, 0, _SCOPE_ABSTRACT_UNIX_SOCKET | _SCOPE_SIGNAL)
    ruleset = libc.syscall(
        ctypes.c_long(_LANDLOCK_CREATE_RULESET), ctypes.c_char_p(attributes),
        ctypes.c_size_t(len(attributes)), ctypes.c_uint(0))
    if ruleset < 0:
        raise SandboxError("Landlock: " + os.strerror(ctypes.get_errno()))
    try:
        if libc.prctl(_PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 or libc.syscall(
                ctypes.c_long(_LANDLOCK_RESTRICT_SELF), ctypes.c_int(ruleset), ctypes.c_uint(0)) != 0:
            raise SandboxError("Landlock: " + os.strerror(ctypes.get_errno()))
    finally:
        os.close(ruleset)


# seccomp.

_SAME = {  # Numbers shared by every architecture that has them.
    "open_tree": 428, "move_mount": 429, "fsopen": 430, "fsconfig": 431, "fsmount": 432,
    "fspick": 433, "pidfd_getfd": 438, "process_madvise": 440, "mount_setattr": 442,
    "io_uring_setup": 425, "io_uring_enter": 426, "io_uring_register": 427,
}
SYSCALLS = {
    "x86_64": (0xC000003E, {
        "ptrace": 101, "process_vm_readv": 310, "process_vm_writev": 311, "kcmp": 312,
        "perf_event_open": 298, "bpf": 321, "userfaultfd": 323, "mount": 165, "umount2": 166,
        "pivot_root": 155, "chroot": 161, "unshare": 272, "setns": 308, "keyctl": 250,
        "add_key": 248, "request_key": 249, "kexec_load": 246, "kexec_file_load": 320,
        "init_module": 175, "finit_module": 313, "delete_module": 176, "reboot": 169,
        "swapon": 167, "swapoff": 168, "acct": 163, "quotactl": 179,
        "name_to_handle_at": 303, "open_by_handle_at": 304, **_SAME,
        "clone": 56, "clone3": 435,
    }),
    "aarch64": (0xC00000B7, {
        "ptrace": 117, "process_vm_readv": 270, "process_vm_writev": 271, "kcmp": 272,
        "perf_event_open": 241, "bpf": 280, "userfaultfd": 282, "mount": 40, "umount2": 39,
        "pivot_root": 41, "chroot": 51, "unshare": 97, "setns": 268, "keyctl": 219,
        "add_key": 217, "request_key": 218, "kexec_load": 104, "kexec_file_load": 294,
        "init_module": 105, "finit_module": 273, "delete_module": 106, "reboot": 142,
        "swapon": 224, "swapoff": 225, "acct": 89, "quotactl": 60,
        "name_to_handle_at": 264, "open_by_handle_at": 265, **_SAME,
        "clone": 220, "clone3": 435,
    }),
}

_LOAD, _JEQ, _JGE, _JSET, _RET = 0x20, 0x15, 0x35, 0x45, 0x06
_KILL, _ALLOW, _ERRNO = 0x80000000, 0x7FFF0000, 0x00050000
_EPERM, _ENOSYS = 1, 38
_CLONE_NEWUSER = 0x10000000
_X32 = 0x40000000


def seccomp_filter(machine=None):
    """The classic BPF program bubblewrap installs before it runs the host."""
    machine = machine or platform.machine()
    if machine not in SYSCALLS:
        raise SandboxError(f"Изоляция плагинов не поддерживает архитектуру {machine}.")
    architecture, numbers = SYSCALLS[machine]
    program = [
        (_LOAD, 0, 0, 4),  # seccomp_data.arch
        (_JEQ, 1, 0, architecture),
        (_RET, 0, 0, _KILL),
        (_LOAD, 0, 0, 0),  # seccomp_data.nr
    ]
    if machine == "x86_64":
        # The x32 ABI reaches the same calls under other numbers.
        program += [(_JGE, 0, 1, _X32), (_RET, 0, 0, _ERRNO | _EPERM)]
    for name, number in sorted(numbers.items(), key=lambda item: item[1]):
        if name not in ("clone", "clone3"):
            program += [(_JEQ, 0, 1, number), (_RET, 0, 0, _ERRNO | _EPERM)]
    program += [
        # clone3 passes its flags in memory a filter can't read: libc falls
        # back to clone on ENOSYS.
        (_JEQ, 0, 1, numbers["clone3"]), (_RET, 0, 0, _ERRNO | _ENOSYS),
        (_JEQ, 0, 3, numbers["clone"]),
        (_LOAD, 0, 0, 16),  # The low half of the flags, the first argument.
        (_JSET, 0, 1, _CLONE_NEWUSER),
        (_RET, 0, 0, _ERRNO | _EPERM),
        (_RET, 0, 0, _ALLOW),
    ]
    return b"".join(struct.pack("HBBI", *instruction) for instruction in program)


# The command.

_SYSTEM = (
    "/usr", "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/nix", "/etc/ssl", "/etc/ca-certificates",
    "/etc/pki", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/ld.so.cache",
    "/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/localtime", "/etc/alternatives",
    "/run/systemd/resolve",
)


def wrap(command, environment, read_only=(), writable=(), directory=None):
    """(argv, file descriptors to pass, report) that run command restricted.

    report is what the client shows: {"active", "network", "reason"}, with
    "creation_flags" for subprocess.Popen where the process must then go
    through confine(). Raises SandboxError where the restrictions exist but
    can't be set up: the host must not start without them.
    """
    if sys.platform == "win32":
        import ovygram_sandbox_windows as windows
        for path in writable:
            windows.label_low(path)
        return list(command), (), {
            "active": True, "network": True, "reason": "",
            "creation_flags": windows.CREATE_SUSPENDED}
    if not sys.platform.startswith("linux"):
        return list(command), (), {
            "active": False, "network": True,
            "reason": "Изоляция процесса плагинов на этой платформе ещё не реализована."}
    bwrap = shutil.which("bwrap")
    if bwrap is None:
        raise SandboxError("Для запуска плагинов установите bubblewrap (bwrap).")
    program = seccomp_filter()
    scoped = landlock_abi() >= _SCOPE_ABI
    arguments = [bwrap, "--unshare-user", "--unshare-pid", "--unshare-ipc", "--unshare-uts",
                 "--unshare-cgroup-try", "--die-with-parent", "--new-session", "--clearenv"]
    if not scoped:
        arguments.append("--unshare-net")
    for path in _SYSTEM:
        if path.count("/") == 1 and os.path.islink(path):
            arguments += ["--symlink", os.readlink(path), path]
        elif os.path.exists(path):
            arguments += ["--ro-bind", path, path]
    arguments += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"]
    for path in read_only:
        arguments += ["--ro-bind", str(path), str(path)]
    for path in writable:
        arguments += ["--bind", str(path), str(path)]
    for key, value in environment.items():
        arguments += ["--setenv", key, value]
    if directory is not None:
        arguments += ["--chdir", str(directory)]
    descriptor = os.memfd_create("ovygram-seccomp")
    try:
        os.write(descriptor, program)
        os.lseek(descriptor, 0, os.SEEK_SET)
    except BaseException:
        os.close(descriptor)
        raise
    arguments += ["--seccomp", str(descriptor), "--", *command]
    return arguments, (descriptor,), {
        "active": True, "network": scoped, "scoped": scoped,
        "reason": "" if scoped else "Сеть плагинам недоступна: ядру нужен Landlock 6 (Linux 6.12)."}


def confine(process):
    """Restricts and resumes a process started with the "creation_flags" of
    wrap(). Returns what release() needs once the process has exited."""
    import ovygram_sandbox_windows as windows
    return windows.confine(process)


def release(guard):
    if guard is not None:
        import ovygram_sandbox_windows as windows
        windows.release(guard)
