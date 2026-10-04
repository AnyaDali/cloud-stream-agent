# Разработка и проверки

## Поддерживаемое окружение

- Windows 10/11 x64;
- MSYS2 UCRT64 в `C:\msys64`;
- GCC с поддержкой C++20;
- CMake 3.25+, Ninja, standalone Asio, C++/WinRT, FFmpeg/libx264 и SDL3 из
  MSYS2 UCRT64;
- Python 3.11+ для будущего agent harness.

Минимальные пакеты текущего этапа:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-asio \
  mingw-w64-ucrt-x86_64-cppwinrt \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-sdl3
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

Серверные accept, exact read/write и ожидание нового кадра используют
`asio::awaitable` и C++20 coroutines. События жизненного цикла клиентов
передаются через `asio::experimental::channel` в одну coroutine
`ClientRegistry::event_loop`. Все сетевые session state, registry и cursors
выполняются одним вызовом `io_context::run()`; capture/encode разделены bounded
queue и отдельным encoder worker. На клиенте UDP receive/reassembly и FFmpeg
decode выполняются в двух workers, SDL3 — в main thread.

Для ручной проверки сначала выбрать окно:

```powershell
.\build\debug\stream-server.exe --list-windows
.\build\debug\stream-server.exe --window-id 0x123456 --tcp-port 9010
.\build\debug\stream-client.exe 127.0.0.1 9010 .\artifacts\client-1\latest-frame.ppm
```

Перед запуском из PowerShell добавить `C:\msys64\ucrt64\bin` в `PATH`, чтобы
Windows нашла DLL FFmpeg, libx264, SDL3 и MinGW runtime.

## Секреты

API-ключ мультимодальной модели хранится только в переменной окружения
`OPENAI_API_KEY` или в выбранном менеджере секретов. Значение не попадает в
репозиторий, логи и скриншоты.
