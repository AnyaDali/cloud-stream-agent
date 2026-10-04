#!/usr/bin/env python3

import json
import os
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Optional


UCRT64_BIN = Path(os.environ.get("MSYSTEM_PREFIX", r"C:\msys64\ucrt64")) / "bin"


def find_command(command: str) -> Optional[str]:
    path = shutil.which(command)
    if path is not None:
        return path
    candidate = UCRT64_BIN / f"{command}.exe"
    return str(candidate) if candidate.is_file() else None


def version(command: str, *args: str) -> Optional[str]:
    path = find_command(command)
    if path is None:
        return None
    try:
        result = subprocess.run(
            [path, *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    first_line = (result.stdout or result.stderr).splitlines()
    return first_line[0].strip() if first_line else "unknown"


def main() -> int:
    commands = {
        "gcc": version("gcc", "--version"),
        "g++": version("g++", "--version"),
        "cmake": version("cmake", "--version"),
        "ninja": version("ninja", "--version"),
        "pkg-config": version("pkg-config", "--version"),
        "ffmpeg": version("ffmpeg", "-version"),
        "pkg:asio": version("pkg-config", "--modversion", "asio"),
        "pkg:libavcodec": version("pkg-config", "--modversion", "libavcodec"),
        "pkg:sdl3": version("pkg-config", "--modversion", "sdl3"),
    }
    report = {
        "schema_version": 1,
        "read_only": True,
        "platform": {
            "system": platform.system(),
            "release": platform.win32_ver()[1] or platform.release(),
            "machine": platform.machine(),
            "toolchain": "MSYS2 UCRT64",
            "toolchain_root": str(UCRT64_BIN.parent),
        },
        "capabilities": {
            name: {
                "status": "available" if detected is not None else "missing",
                "version": detected,
            }
            for name, detected in commands.items()
        },
        "next": {
            "install": (
                "pacman -S --needed mingw-w64-ucrt-x86_64-gcc "
                "mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja "
                "mingw-w64-ucrt-x86_64-asio"
            ),
            "configure": "cmake --preset debug",
            "build": "cmake --build --preset debug --target stream-server stream-client",
            "test": "deferred by current milestone",
        },
    }
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
