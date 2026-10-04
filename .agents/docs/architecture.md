# Архитектура

## Цель

Минимальная демонстрация полного пути видеокадра на Windows 10/11:

```text
окно приложения
  -> Windows Graphics Capture
  -> ограниченная очередь кадров
  -> FFmpeg H.264 encoder
  -> собственный протокол / UDP
  -> FFmpeg decoder
  -> SDL3 renderer
  -> метрики + latest-frame.ppm
  -> локальный API клиента
  -> agent harness + мультимодальная LLM
```

## Границы компонентов

- `protocol` не зависит от UI, FFmpeg или API операционной системы.
- Сервер явно выбирает одно окно, захватывает его и кодирует видеопакеты.
- Клиент принимает сообщения, декодирует, показывает последний доступный кадр
  и публикует наблюдаемые метрики.
- Агент не читает память приложения и не угадывает состояние. Он использует
  локальный API клиента и сохранённый последний кадр.

## Модель выполнения

Источник кадров, кодирование и отправка разделены явными компонентами. Между
источником и encoder worker находится ограниченная latest-wins очередь. После
кодирования кадр один раз помещается в общий ограниченный `BroadcastBuffer`.
Каждая `ClientSession` хранит собственный monotonic cursor, поэтому чтение одним
клиентом не удаляет данные для остальных. Медленный клиент не удерживает память:
при отставании от начала буфера он пропускает данные до следующего keyframe.

`ClientAcceptor` непрерывно выполняет asynchronous accept и создаёт независимые
coroutine-сессии. Поддерживается до восьми клиентов с одним общим профилем
потока. TCP используется для handshake, управления жизненным циклом и будущего
TLS; H.264 Annex B передаётся фрагментами по UDP. У каждой `ClientSession` свой
UDP endpoint, `session_id`, 64-битный packet counter и cursor. Поэтому будущая
персональная криптография добавляется в session sender и не требует повторного
кодирования кадра.

Жизненным циклом клиентов управляет явный `ClientRegistry::event_loop`.
`ClientAcceptor` отправляет событие `ClientConnected`, а сессии — события
`ClientReady` и `ClientDisconnected`. Тот же цикл обрабатывает появление данных,
завершение потока и остановку сервера. Только event loop добавляет и удаляет
элементы из registry и выполняет переходы `running -> finishing/stopping ->
stopped`.

```text
WindowsGraphicsCaptureSource
  -> BoundedLatestQueue<RawFrame>
  -> H264Encoder (libx264, один раз на поток)
  -> BroadcastBuffer<EncodedFrame>
  -> ClientSession cursor #1..N
  -> UdpPacketizer (per-session header; далее здесь появится AEAD)
  -> UDP

UDP -> FrameReassembler -> H264Decoder -> SDL3 + latest-frame.ppm
```

## Этапы реализации

1. Заголовок протокола и синтетические пакеты. — готово
2. TCP server/client и синтетические изображения. — готово
3. Windows baseline: MSYS2 UCRT64 и сборка двух executable. — готово
4. Компонентный server pipeline, multi-client accept и общий broadcast buffer. — готово
5. Async TCP control plane на C++20 coroutines и Asio. — готово
6. H.264 Annex B и UDP video plane. — готово
7. FFmpeg encode/decode и SDL3 renderer. — готово
8. Windows Graphics Capture с явным выбором окна. — готово
9. Локальный API, latest-frame и агентские tools/evals.

## Потоки выполнения

- WGC `CreateFreeThreaded` получает BGRA-кадры и кладёт их в bounded
  latest-wins очередь ёмкостью два кадра.
- Единственный encoder worker масштабирует кадр к фиксированному профилю и
  кодирует libx264 с `zerolatency`, `max_b_frames=0` и периодическим IDR.
- Один Asio `io_context` обслуживает TCP accept, registry event loop и все
  клиентские TCP/UDP coroutine. Медленный клиент пропускает старые записи до
  keyframe и не блокирует encoder или другие сессии.
- На клиенте network worker делает reassembly, decoder worker вызывает FFmpeg,
  а главный поток владеет SDL window/renderer/texture.
- UDP sender ограничивает размер burst, а клиент начинает декодирование только
  с полностью собранного keyframe. Ошибка decoder сбрасывает codec state и
  возвращает клиента в ожидание следующего IDR вместо завершения процесса.

Захват начинается при старте сервера и требует явный `--window-id` либо
однозначный `--window-title`; полный desktop не выбирается неявно.

Переход к следующему этапу делается только после исполняемой проверки текущего.
