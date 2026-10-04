#!/usr/bin/env python3

import json
import selectors
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: synthetic_stream_test.py SERVER CLIENT")

    with tempfile.TemporaryDirectory(prefix="cloud-stream-test-") as directory:
        frame_path = Path(directory) / "latest-frame.ppm"
        server = subprocess.Popen(
            [sys.argv[1], "0", "5", "0", "127.0.0.1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            selector = selectors.DefaultSelector()
            selector.register(server.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=5):
                raise TimeoutError("server did not publish its listening port")
            readiness = server.stdout.readline().strip()
            if not readiness.startswith("listening_on=127.0.0.1:"):
                raise RuntimeError(f"unexpected server readiness line: {readiness!r}")
            port = int(readiness.rsplit(":", 1)[1])
            client = subprocess.run(
                [sys.argv[2], "127.0.0.1", str(port), str(frame_path)],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            server_stdout, server_stderr = server.communicate(timeout=10)
        except BaseException:
            server.kill()
            server.communicate()
            raise

        if client.returncode != 0 or server.returncode != 0:
            print(server_stdout, server_stderr, client.stdout, client.stderr, file=sys.stderr)
            return 1

        metrics = json.loads((Path(directory) / "stream-metrics.json").read_text())
        if metrics["connection_state"] != "ended" or metrics["video_messages_received_total"] != 5:
            print(metrics, file=sys.stderr)
            return 1
        if metrics["payload_bytes_received_total"] != 320 * 180 * 3 * 5:
            print(metrics, file=sys.stderr)
            return 1
        if not frame_path.read_bytes().startswith(b"P6\n320 180\n255\n"):
            print("latest frame is not the expected binary PPM", file=sys.stderr)
            return 1

    print("synthetic stream integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
