# Agent instructions

## Purpose

Build a Windows-only C++20 client/server video-streaming demo and a separate
agent harness that verifies the stream from observable client evidence.

## Read first

- Architecture or component-boundary changes: `.agents/docs/architecture.md`.
- Wire-format changes: `.agents/docs/protocol.md`.
- Build, test, and environment work: `.agents/docs/development.md`.
- Finding the right files: `.agents/docs/codebase-map.md`.

## Common commands

```sh
powershell -NoProfile -ExecutionPolicy Bypass -File .agents/scripts/preflight.ps1
cmake --preset debug
cmake --build --preset debug --target stream-server stream-client
```

Run the CMake commands in the MSYS2 UCRT64 shell. The current milestone is
build only: do not add or run tests until the user asks to resume test work.

## Invariants

1. The server owns capture and encoding; the client owns decoding, rendering,
   stream metrics, and the latest-frame artifact.
2. The project targets Windows 10/11 only. Windows capture code lives under a
   `windows` path; do not add unused cross-platform capture abstractions.
3. Never send C++ structs directly over the network. Serialize fixed-width
   integers explicitly in network byte order and validate lengths before
   allocation.
4. Keep frame queues bounded. For real-time video, stale frames are dropped
   rather than allowing latency to grow without limit.
5. Agent answers must cite observable evidence from the client API/artifacts;
   model output alone is not proof that video is flowing.
6. Capture and encoding happen once per stream. Multiple clients read the same
   bounded broadcast buffer through independent monotonic cursors; a slow
   client must never retain the buffer or block other sessions.
7. Client lifecycle transitions and mutations of the session registry happen
   only in `ClientRegistry::event_loop`; producers communicate with it through
   explicit events.

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
| Update dependencies | MSYS2 package list in `.agents/docs/development.md`, CMake config | preflight + configure/build |

## Completion

Update relevant Russian documentation when contracts, commands, or boundaries
change. For the current milestone, verify that `stream-server.exe` and
`stream-client.exe` build and state that tests were intentionally not run. Keep
commits and PR creation human-approved.
