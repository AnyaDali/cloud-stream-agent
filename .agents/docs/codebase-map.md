# Карта кодовой базы

Состояние на этапе нулевого вертикального среза.

| Путь | Назначение |
| --- | --- |
| `src/protocol/` | Версионируемый бинарный wire format и его валидация |
| `src/net/` | Exact read/write сообщений поверх TCP и настройки сокета |
| `src/server/` | Точка входа сервера; далее orchestration capture/encode/send |
| `src/client/` | Точка входа клиента; далее receive/decode/render/metrics |
| `tests/protocol/` | Исполняемые проверки framing и защитных ограничений |
| `tests/integration/` | Loopback-проверка двух реальных процессов и артефактов |
| `.agents/docs/` | Архитектура, протокол и воспроизводимые команды |
| `.agents/scripts/` | Детерминированные локальные проверки окружения |

Запланированные области создаются только вместе с рабочим кодом:

- `src/capture/windows/` — Windows Graphics Capture и выбор окна;
- `src/media/` — FFmpeg encoder/decoder и ограниченная очередь кадров;
- `src/observability/` — метрики и локальный API клиента;
- `agent/` — harness, tools и evals.

После каждого вертикального среза карта обновляется по реальным entry points и
тестам, а не по предполагаемой будущей структуре.
