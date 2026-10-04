# Сетевой протокол v1

TCP используется как поток байтов: одно чтение сокета не соответствует одному
сообщению. Получатель читает ровно 16 байт заголовка, валидирует его, затем
читает ровно `payload_size` байт. Все многобайтовые числа имеют big-endian
представление. C++-структуры в сокет напрямую не копируются.

## Заголовок

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 4 | magic `CSTR` |
| 4 | 1 | версия (`1`) |
| 5 | 1 | тип сообщения |
| 6 | 2 | flags, в v1 всегда `0` |
| 8 | 4 | полный размер payload |
| 12 | 4 | sequence number |

Абсолютный предел payload — 8 MiB. Sequence ведётся отдельно для каждого
направления, начинается с нуля и увеличивается на единицу для каждого сообщения,
включая `HELLO`, `STREAM_CONFIG` и `END`. Пропуск, повтор, уменьшение или
переполнение sequence считается protocol error.

Каждое TCP-подключение имеет независимую пару sequence counters. Сервер может
обслуживать несколько клиентов одновременно; клиенты не разделяют control
state и не видят идентификаторы сообщений других сессий. Внутренний sequence
общего broadcast-буфера в wire format v1 не передаётся.

## Состояния соединения

```text
client HELLO(sequence=0)
server HELLO(sequence=0)
server STREAM_CONFIG
server VIDEO_PACKET*
server END
```

До `STREAM_CONFIG` видеопакеты запрещены. В v1 повторный `STREAM_CONFIG` не
поддерживается: изменение разрешения или extradata требует завершить текущий
поток и создать новую сессию. `ERROR` разрешён в любом состоянии и завершает
сессию. EOF до `END` считается аварийным обрывом.

После обмена `HELLO` отправитель ограничивает сообщения значением
`min(8 MiB, peer.max_payload_size)`. Заголовок `payload_size` включает также
фиксированный префикс конкретного типа сообщения.

## HELLO (`type = 1`, ровно 8 байт)

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 1 | role: `1=server`, `2=client` |
| 1 | 1 | reserved, `0` |
| 2 | 2 | capabilities bitmask |
| 4 | 4 | max payload size |

Capabilities v1: bit 0 — `RAW_RGB24`, bit 1 — `H264_ANNEX_B`. Неизвестные биты
в v1 отвергаются. Значение max payload находится в диапазоне `[29, 8 MiB]`.

## STREAM_CONFIG (`type = 2`, 20 + extradata)

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 1 | codec: `0=RAW`, `1=H264` |
| 1 | 1 | pixel format: `0=unspecified`, `1=RGB24`, `2=NV12`, `3=YUV420P` |
| 2 | 2 | flags, в v1 `0` |
| 4 | 2 | width |
| 6 | 2 | height |
| 8 | 4 | time-base numerator |
| 12 | 4 | time-base denominator |
| 16 | 4 | extradata length |
| 20 | N | codec extradata |

Размеры находятся в диапазоне `1..8192`, обе части time base — в
`1..1_000_000_000`. Для `RAW` разрешена только комбинация `RGB24` без
extradata. Для закодированного видео pixel format равен `unspecified`: renderer
использует формат реально декодированного `AVFrame`.

H.264 в v1 передаётся в Annex B. Один `VIDEO_PACKET` содержит одну
codec-specific data unit, полученную от encoder. Side data не передаётся,
SPS/PPS повторяются in-band вместе с keyframe. Extradata при наличии также имеет
Annex B representation. При передаче данных в FFmpeg локальный буфер должен
иметь `AV_INPUT_BUFFER_PADDING_SIZE` дополнительных нулевых байт; padding не
передаётся по сети.

## VIDEO_PACKET (`type = 3`, 28 + data)

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 8 | signed PTS в единицах time base |
| 8 | 8 | signed DTS в единицах time base |
| 16 | 8 | signed duration; `0` означает unknown |
| 24 | 2 | flags; bit 0 — keyframe |
| 26 | 2 | reserved, `0` |
| 28 | N | codec-specific data, не пустая |

Для `RAW_RGB24` data — один полный top-down кадр, порядок каналов RGB, без
padding между строками. Его размер обязан быть равен `width * height * 3`, а
арифметика проверяется на overflow до выделения памяти.

## ERROR (`type = 4`)

Первые четыре байта — numeric error code, затем не более 4096 байт UTF-8 без
завершающего нуля. После `ERROR` отправитель закрывает соединение.

## END (`type = 5`)

Payload отсутствует. Это единственный штатный способ завершения потока.
