# MQTT Specification

## 1. Connection

| Item | Value |
|---|---|
| Broker location | Runs on the board itself |
| Broker address | Board IPv4 address |
| Broker port | `1883`, listening on all interfaces (`0.0.0.0`) |
| Transport | TCP |
| MQTT version | 3.1.1 |
| Authentication | None (no username / password) |
| Clean session | `1` (no stored sessions) |
| Keepalive | 60 s |
| Subscribers | Broker delivers each message to every connected subscriber |

### Keepalive rules

- Every client (both notify module and user device) sends `PINGREQ` if it has sent nothing for 60 s.
- The broker answers every `PINGREQ` with `PINGRESP`.
- The broker closes a connection that has sent nothing for 90 s (1.5 × keepalive).

## 2. Topic and delivery

| Item | Value |
|---|---|
| Topic name | `audio-event` |
| QoS | 0 (message can be lost) |
| Retain | 0 (message not retained for clients subscribing later) |
| Publisher | Notify module on the board, connected to `127.0.0.1:1883` |
| Subscriber | User device client, connected to `<board IP>:1883` |

### When a message is published

- One message per detected event, not one per frame.

## 3. Payload

UTF-8 JSON, one object per PUBLISH.

| Field | Type | Description |
|---|---|---|
| `device` | string | Board name set by user, in configuration file |
| `event_id` | integer | Increases by 1 per event, starting at 1 |
| `class` | string | Name of the detected class |
| `score` | number | Score of the class, 0.0 - 1.0 |
| `threshold` | number | Threshold of the class, 0.0 - 1.0 |
| `time` | integer | seconds since 1970-01-01 UTC |

### Example

```json
{
  "device": "CRZ AM62x (Linux)",
  "event_id": 17,
  "class": "Siren",
  "score": 0.873,
  "threshold": 0.4,
  "time": 1795064602
}
```

`1795064602` = 2026-11-19 14:03:22 KST.
