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
  -> метрики + latest-frame.jpg
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
coroutine-сессии. На текущем этапе поддерживается до восьми клиентов с одним
общим профилем потока. TCP используется для control plane, UDP — для будущего
video plane. Текущий synthetic-срез пока передаёт RAW_RGB24 по TCP и будет
заменён после фиксации protocol v2.

```text
SyntheticFrameSource / WindowsGraphicsCapture
  -> BoundedLatestQueue<RawFrame>
  -> FrameEncoder
  -> BroadcastBuffer<EncodedFrame>
  -> ClientSession cursor #1..N
```

## Этапы реализации

1. Заголовок протокола и синтетические пакеты. — готово
2. TCP server/client и синтетические изображения. — готово
3. Windows baseline: MSYS2 UCRT64 и сборка двух executable. — готово
4. Компонентный server pipeline, multi-client accept и общий broadcast buffer. — готово
5. Async TCP control plane на C++20 coroutines и Asio. — частично готово
6. Synthetic H.264 через UDP.
7. FFmpeg encode/decode и SDL3 renderer.
8. Windows Graphics Capture с явным выбором окна.
9. Локальный API, latest-frame и агентские tools/evals.

Переход к следующему этапу делается только после исполняемой проверки текущего.
