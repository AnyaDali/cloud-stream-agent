#!/usr/bin/env python3

import json
import platform
import shutil
import subprocess
from typing import Optional


def version(command: str, *args: str) -> Optional[str]:
    path = shutil.which(command)
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
    macos_version = version("sw_vers", "-productVersion")
    commands = {
        "clang++": version("clang++", "--version"),
        "cmake": version("cmake", "--version"),
        "ninja": version("ninja", "--version"),
        "pkg-config": version("pkg-config", "--version"),
        "ffmpeg": version("ffmpeg", "-version"),
        "python3.11": version("python3.11", "--version"),
        "pkg:asio": version("pkg-config", "--modversion", "asio"),
        "pkg:libavcodec": version("pkg-config", "--modversion", "libavcodec"),
        "pkg:sdl3": version("pkg-config", "--modversion", "sdl3"),
    }
    report = {
        "schema_version": 1,
        "read_only": True,
        "platform": {
            "system": platform.system(),
            "release": macos_version or platform.mac_ver()[0] or platform.release(),
            "machine": platform.machine(),
        },
        "capabilities": {
            name: {
                "status": "available" if detected is not None else "missing",
                "version": detected,
            }
            for name, detected in commands.items()
        },
        "next": {
            "install": "brew bundle",
            "configure": "cmake --preset debug",
            "build": "cmake --build --preset debug",
            "test": "ctest --preset debug",
        },
    }
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
