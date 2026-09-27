# Agent instructions

## Purpose

Build a macOS-first C++20 client/server video-streaming demo and a separate
agent harness that verifies the stream from observable client evidence.

## Read first

- Architecture or component-boundary changes: `.agents/docs/architecture.md`.
- Wire-format changes: `.agents/docs/protocol.md`.
- Build, test, and environment work: `.agents/docs/development.md`.
- Finding the right files: `.agents/docs/codebase-map.md`.

## Common commands

```sh
python3 .agents/scripts/preflight.py
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Invariants

1. The server owns capture and encoding; the client owns decoding, rendering,
   stream metrics, and the latest-frame artifact.
2. Platform APIs stay behind adapters. macOS-specific sources use `.mm` and
   live under a `mac` path or carry a `_mac` suffix.
3. Never send C++ structs directly over the network. Serialize fixed-width
   integers explicitly in network byte order and validate lengths before
   allocation.
4. Keep frame queues bounded. For real-time video, stale frames are dropped
   rather than allowing latency to grow without limit.
5. Agent answers must cite observable evidence from the client API/artifacts;
   model output alone is not proof that video is flowing.

## Safety and compatibility

- Do not read, print, or commit API keys or token files. Document environment
  variable names only.
- Do not capture the full desktop implicitly. Window selection must be explicit
  by command-line identifier or an interactive list.
- Keep protocol-version checks and payload-size limits at every network boundary.
- Do not install packages, push, open a PR, or enable paid API calls without
  explicit user approval.

## Change routing

| Intent | Start here | Required verification |
| --- | --- | --- |
| Change message framing | `src/protocol/` | `protocol-header-test` |
| Add server capture/encoding | `src/server/`, future `src/capture/`, `src/media/` | focused unit test + local stream smoke test |
| Add client decode/render | `src/client/`, future `src/media/` | focused unit test + local stream smoke test |
| Add agent tool/API | future `agent/` and client observability API | deterministic fixture/eval + API test |
| Update dependencies | `Brewfile`, CMake config | preflight + configure/build/test |

## Completion

Update relevant Russian documentation when contracts, commands, or boundaries
change. Run the strongest available verification tier and state exactly what was
not run. Keep commits and PR creation human-approved.
