# Карта кодовой базы

Состояние на этапе нулевого вертикального среза.

| Путь | Назначение |
| --- | --- |
| `src/protocol/` | Версионируемый бинарный wire format и его валидация |
| `src/net/` | Exact read/write сообщений поверх TCP и настройки сокета |
| `src/capture/windows/` | Перечень окон и Windows Graphics Capture через C++/WinRT + D3D11 |
| `src/media/ffmpeg/` | libx264 encoder и программный H.264 decoder/converter |
| `src/pipeline/` | Bounded queues, общий broadcast buffer, UDP packetizer/reassembler |
| `src/server/` | ServerApp, coroutine accept, registry и независимые client sessions |
| `src/client/` | TCP/UDP receive, decoder worker, SDL3 render и PPM/JSON artifacts |
| `tests/protocol/` | Исполняемые проверки framing и защитных ограничений |
| `tests/integration/` | Loopback-проверка двух реальных процессов и артефактов |
| `.agents/docs/` | Архитектура, протокол и воспроизводимые команды |
| `.agents/scripts/` | Детерминированные локальные проверки окружения |

Запланированные области создаются только вместе с рабочим кодом:

- `src/observability/` — метрики и локальный API клиента;
- `agent/` — harness, tools и evals.

После каждого вертикального среза карта обновляется по реальным entry points и
тестам, а не по предполагаемой будущей структуре.
