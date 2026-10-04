# Сетевой протокол v2

Протокол разделён на надёжный TCP control plane и UDP video plane. Все
многобайтовые числа кодируются в big-endian; C++-структуры в сеть напрямую не
копируются. Один серверный H.264-поток используется всеми клиентами, но каждая
сессия имеет собственные TCP-состояние, UDP endpoint, `session_id`, счётчик
пакетов и cursor общего broadcast-буфера.

Текущая версия не шифрует трафик. Поле `key_epoch` зарезервировано и равно нулю.
Случайный `probe_token` связывает UDP endpoint с уже открытым TCP-соединением,
но не заменяет TLS, аутентификацию или AEAD.

## TCP framing

Получатель читает ровно 16 байт заголовка, валидирует его, затем читает ровно
`payload_size` байт. Абсолютный предел payload — 8 MiB.

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 4 | magic `CSTR` |
| 4 | 1 | версия (`2`) |
| 5 | 1 | тип сообщения |
| 6 | 2 | flags, сейчас `0` |
| 8 | 4 | размер payload |
| 12 | 4 | sequence number |

Sequence ведётся отдельно для каждого направления, начинается с нуля и строго
увеличивается на единицу. Каждое TCP-подключение имеет независимые counters.

Типы сообщений: `1=HELLO`, `2=UDP_CONFIG`, `3=UDP_READY`,
`4=STREAM_CONFIG`, `5=REQUEST_KEYFRAME`, `6=PING`, `7=PONG`, `8=ERROR`,
`9=END`. PING/PONG зарезервированы; текущая реализация их ещё не отправляет.

## Handshake и состояния

```text
client -> server: HELLO(sequence=0, H264_ANNEX_B)
server -> client: HELLO(sequence=0, H264_ANNEX_B)
server -> client: UDP_CONFIG(sequence=1)
client -> server: UDP probe
server -> client: UDP_READY(sequence=2)
server -> client: STREAM_CONFIG(sequence=3)
client -> server: REQUEST_KEYFRAME(sequence=1)
server -> client: UDP video datagrams*
server -> client: END(sequence=4+)             # штатное завершение
```

TCP остаётся открытым на всё время сессии. EOF позволяет серверу сразу удалить
ушедшего клиента. `REQUEST_KEYFRAME` имеет пустой payload и отдельный client
sequence; сервер может запросить общий encoder создать IDR для восстановления
конкретного отставшего клиента.

После handshake клиент запрашивает свежий keyframe и не передаёт P-frames в
decoder, пока полностью не собран IDR с in-band SPS/PPS. Сервер использует
увеличенный UDP send buffer и делает паузу 1 ms после каждых 16 datagrams;
клиент использует увеличенный receive buffer. Это предотвращает потерю
крупного стартового IDR из-за локального UDP burst.

## TCP payloads

### HELLO — 8 байт

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 1 | role: `1=server`, `2=client` |
| 1 | 1 | reserved, `0` |
| 2 | 2 | capabilities; bit 1 — `H264_ANNEX_B` |
| 4 | 4 | максимальный TCP payload |

### UDP_CONFIG — 32 байта

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 8 | ненулевой `session_id` |
| 8 | 16 | случайный `probe_token` |
| 24 | 2 | UDP port этой серверной сессии |
| 26 | 2 | максимальный размер UDP datagram, сейчас 1200 |
| 28 | 4 | `key_epoch`, сейчас строго `0` |

### STREAM_CONFIG — 20 + extradata

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 1 | codec: `1=H264` |
| 1 | 1 | pixel format: `0=unspecified` |
| 2 | 2 | flags, `0` |
| 4 | 2 | width |
| 6 | 2 | height |
| 8 | 4 | time-base numerator |
| 12 | 4 | time-base denominator |
| 16 | 4 | extradata length |
| 20 | N | codec extradata |

H.264 передаётся в Annex B. Encoder работает с `repeat-headers=1`, поэтому
SPS/PPS повторяются in-band с keyframe; текущий `extradata` пуст.

`ERROR` содержит четырёхбайтовый код и до 4096 байт UTF-8. `END`, `UDP_READY`
и `REQUEST_KEYFRAME` не имеют payload.

## UDP probe — 32 байта

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 4 | magic `CSPB` |
| 4 | 1 | версия (`2`) |
| 5 | 1 | type (`1`) |
| 6 | 2 | reserved, `0` |
| 8 | 8 | `session_id` |
| 16 | 16 | `probe_token` из UDP_CONFIG |

Сервер принимает endpoint только при совпадении token, session id и IP-адреса
TCP peer. Port узнаётся из фактического source endpoint probe.

## UDP video datagram

Размер datagram не превышает согласованные 1200 байт. Заголовок занимает 56
байт, остаток — фрагмент одного H.264 access unit.

| Смещение | Размер | Поле |
| ---: | ---: | --- |
| 0 | 4 | magic `CSVD` |
| 4 | 1 | версия (`2`) |
| 5 | 1 | type (`1`) |
| 6 | 2 | flags; bit 0 — keyframe |
| 8 | 8 | `session_id` |
| 16 | 4 | `key_epoch`, сейчас `0` |
| 20 | 2 | `fragment_index` |
| 22 | 2 | `fragment_count` |
| 24 | 8 | монотонный per-session `packet_number` |
| 32 | 8 | `frame_id` |
| 40 | 8 | signed PTS |
| 48 | 4 | полный размер access unit |
| 52 | 2 | размер фрагмента |
| 54 | 2 | reserved, `0` |
| 56 | N | fragment bytes |

Клиент одновременно держит не более трёх незавершённых кадров и удаляет их
через 250 ms. Полный access unit ограничен 8 MiB. Перед FFmpeg decoder данные
копируются через `av_new_packet`, который добавляет требуемый
`AV_INPUT_BUFFER_PADDING_SIZE`.

В следующем crypto-этапе неизменяемый UDP header станет associated data, а
payload — ciphertext с authentication tag. Ключ будет уникальным для сессии и
передаваться/выводиться через TLS control plane; nonce будет строиться из
`key_epoch` и `packet_number`.
