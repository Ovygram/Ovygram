"""Runs inside the helper's private job or PID namespace, before package code."""
import os
from pathlib import Path
import sys


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    scoped, binary, *args = sys.argv[1:]
    if sys.platform == "win32":
        from ovygram_sandbox_windows import run_restricted_helper
        # Windows reports an unsigned DWORD. Preserve all bits when passing
        # it through Python's signed C int instead of collapsing NTSTATUS to -1.
        code = run_restricted_helper(binary, args)
        return code if code <= 0x7fffffff else code - 0x100000000
    if sys.platform.startswith("linux"):
        if scoped == "1":
            from ovygram_sandbox import apply_scope
            apply_scope()
        os.execve(binary, [binary, *args], {"HOME": "/tmp", "TMPDIR": "/tmp", "LANG": "C.UTF-8"})
    raise RuntimeError("Нет изоляции компонента для этой платформы.")


if __name__ == "__main__":
    sys.exit(main())
