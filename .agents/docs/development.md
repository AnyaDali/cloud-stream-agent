# Разработка и проверки

## Поддерживаемое окружение

- Windows 10/11 x64;
- MSYS2 UCRT64 в `C:\msys64`;
- GCC с поддержкой C++20;
- CMake 3.25+, Ninja и standalone Asio из MSYS2 UCRT64;
- Python 3.11+ для будущего agent harness.

Минимальные пакеты текущего этапа:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-asio
```

Проверка доступности выполняется командой:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .agents/scripts/preflight.ps1
```

Она ничего не устанавливает и не запускает сетевые сервисы.

## Лестница проверок

```sh
cmake --preset debug
cmake --build --preset debug --target stream-server stream-client
```

Команды сборки выполняются из **MSYS2 UCRT64**. Текущий milestone ограничен
сборкой executable-файлов. Новые тесты не добавляются и существующие не
запускаются до отдельного обсуждения.

Интеграционная проверка видеопотока должна фиксировать: поступление кадров,
ненулевой FPS, возраст последнего кадра, декодированное разрешение и обновление
latest-frame. Проверки Windows Graphics Capture будут выполняться локально:
обычный GitHub-hosted runner не имеет выбранного интерактивного окна.

Текущий `synthetic-stream-test` поднимает loopback TCP server/client, принимает
пять RGB24-кадров и проверяет итоговые метрики и PPM-артефакт. В изолированной
среде агента тесту может потребоваться разрешение на создание loopback-сокета.

Текущие receive/send операции синхронные. Следующий этап переведёт accept,
connect, exact read/write и таймеры на `asio::awaitable` и C++20 coroutines.

## Секреты

API-ключ мультимодальной модели хранится только в переменной окружения
`OPENAI_API_KEY` или в выбранном менеджере секретов. Значение не попадает в
репозиторий, логи и скриншоты.
