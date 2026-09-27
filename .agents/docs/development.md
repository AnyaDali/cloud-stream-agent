# Разработка и проверки

## Поддерживаемое окружение

- macOS 26.x, Apple Silicon;
- Apple Clang с поддержкой C++20;
- CMake 3.25+ и Ninja;
- FFmpeg, SDL3 и standalone Asio из Homebrew;
- Python 3.11+ для agent harness.

Проверка доступности выполняется командой:

```sh
python3 .agents/scripts/preflight.py
```

Она ничего не устанавливает и не запускает сетевые сервисы.

## Лестница проверок

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Для быстрой проверки чистой логики протокола, если CMake ещё не установлен:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Isrc \
  src/protocol/message_header.cpp tests/protocol/message_header_test.cpp \
  -o /tmp/cloud-stream-protocol-test
/tmp/cloud-stream-protocol-test
```

Интеграционная проверка видеопотока должна фиксировать: поступление кадров,
ненулевой FPS, возраст последнего кадра, декодированное разрешение и обновление
latest-frame. Проверки захвата выполняются локально: GitHub-hosted runner не
имеет интерактивного пользовательского окна и разрешения Screen Recording.

Текущий `synthetic-stream-test` поднимает loopback TCP server/client, принимает
пять RGB24-кадров и проверяет итоговые метрики и PPM-артефакт. В изолированной
среде агента тесту может потребоваться разрешение на создание loopback-сокета.

Синхронные receive/send операции установленного сокета имеют таймаут 10 секунд.
Начальные DNS resolve и TCP connect пока используют системные таймауты; перед
поддержкой удалённого запуска их нужно перевести на async operation с timer и
cancel.

## Секреты

API-ключ мультимодальной модели хранится только в переменной окружения
`OPENAI_API_KEY` или в выбранном менеджере секретов. Значение не попадает в
репозиторий, логи и скриншоты.
