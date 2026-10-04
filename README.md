# Cloud Stream Agent

Windows-only C++20 демонстрация собственного клиент-серверного видеостриминга.
Сервер захватывает явно выбранное окно через Windows Graphics Capture, один раз
кодирует поток в H.264 и рассылает его нескольким клиентам по UDP. Клиент
собирает фрагменты, программно декодирует H.264 через FFmpeg, показывает кадры
через SDL3 и сохраняет наблюдаемые artifacts.

## Архитектура текущего среза

```text
выбранное HWND -> WGC/D3D11 -> bounded raw queue -> libx264
    -> общий bounded BroadcastBuffer
    -> независимые ClientSession/cursor -> UDP packetizer -> клиент

UDP -> bounded reassembler -> FFmpeg decoder -> SDL3
                                      `-> latest-frame.ppm + stream-metrics.json
```

TCP используется как control plane: HELLO, привязка UDP endpoint,
STREAM_CONFIG, завершение и контроль жизни клиента. Захват и кодирование общие,
но у каждой сессии собственные TCP-состояние, UDP endpoint, `session_id`, cursor
и packet counter. Это позволяет в следующей итерации добавить уникальный
per-client ключ и AEAD непосредственно перед UDP send без повторного H.264
кодирования.

Шифрования в текущей версии ещё нет: `key_epoch` равен нулю, а `probe_token`
только связывает UDP endpoint с TCP-сессией. Пока поток следует использовать в
доверенной локальной сети.

## Подготовка Windows

Установить [MSYS2](https://www.msys2.org/) в `C:\msys64`, открыть
**MSYS2 UCRT64** и выполнить:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-asio \
  mingw-w64-ucrt-x86_64-cppwinrt \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-sdl3
```

Read-only проверка окружения из PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .agents/scripts/preflight.ps1
```

## Сборка

В **MSYS2 UCRT64** из корня репозитория:

```sh
cmake --preset debug
cmake --build --preset debug --target stream-server stream-client
```

Текущий milestone проверяет сборку executable и ручной local stream smoke.
Автоматические тесты временно не запускаются и новые тесты не добавляются до
отдельного согласования.

## Запуск

При запуске из PowerShell сначала добавить runtime DLL MSYS2 в `PATH`:

```powershell
cd C:\Users\annadali\projects\cloud-stream-agent
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
```

Получить список доступных окон:

```powershell
.\build\debug\stream-server.exe --list-windows
```

Запустить сервер с явным HWND из списка:

```powershell
.\build\debug\stream-server.exe `
  --window-id 0x123456 `
  --tcp-port 9010 `
  --bind 127.0.0.1 `
  --max-clients 8 `
  --width 1280 `
  --height 720 `
  --fps 30 `
  --bitrate-kbps 4000
```

Вместо id можно передать `--window-title TEXT`, только если подстрока находит
ровно одно окно. Неявный захват desktop или первого попавшегося окна запрещён.

Каждый клиент запускается отдельно:

```powershell
.\build\debug\stream-client.exe 127.0.0.1 9010 `
  .\artifacts\client-1\latest-frame.ppm
```

Можно запустить до восьми клиентов. Закрытие SDL-окна штатно завершает клиента;
сервер замечает закрытие TCP control channel и удаляет только его сессию.
Клиент после handshake запрашивает свежий IDR и ждёт полного keyframe с SPS/PPS,
поэтому подключение к уже идущему потоку не зависит от ранее отправленных
H.264-пакетов.

## Документация

- [Архитектура](.agents/docs/architecture.md)
- [Протокол v2](.agents/docs/protocol.md)
- [Разработка и проверки](.agents/docs/development.md)
- [Карта кодовой базы](.agents/docs/codebase-map.md)
