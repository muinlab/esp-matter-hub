# ESP Matter IR Hub — 앱 개발자 API 명세서

> **버전**: 2.0 (2026-03-17)
> **대상**: iOS/Android 모바일 앱 개발자
> **허브 펌웨어**: ESP32-S3 기반 Matter Bridge + IR 학습/송신

---

## 1. 시스템 개요

ESP Matter IR Hub는 적외선 리모컨 신호를 학습하고, Matter 프로토콜을 통해 가전제품을 제어하는 브릿지 장치입니다.

### 1.1 핵심 개념

| 개념 | 설명 |
|------|------|
| **Signal** | 학습된 IR 신호 데이터 (carrier 주파수 + timing payload) |
| **Device** | 가상 장치 — IR 신호를 On/Off/Up/Down 동작에 바인딩 |
| **Slot** | 브릿지 엔드포인트 — 가상 장치를 Matter 네트워크에 노출 |
| **Fabric** | Matter 컨트롤러 네트워크 (Apple Home, Google Home 등) |

### 1.2 아키텍처

```
                          Matter Network
                               │
┌──────────────────────────────┼──────────────────────────────┐
│  ESP Matter IR Hub           │                              │
│                              │                              │
│  Endpoint 0: Root Node       │                              │
│  Endpoint 1: Aggregator ─────┼─── 브릿지 부모               │
│  Endpoint 2: Slot 0 (light) ─┤                              │
│  Endpoint 3: Slot 1 (light) ─┤                              │
│  Endpoint 4: Slot 2 (light) ─┤   ← OnOff + LevelControl    │
│  Endpoint 5: Slot 3 (light) ─┤     (표준 Matter 클러스터)   │
│  Endpoint 6: Slot 4 (light) ─┤                              │
│  Endpoint 7: Slot 5 (light) ─┤                              │
│  Endpoint 8: Slot 6 (light) ─┤                              │
│  Endpoint 9: Slot 7 (light) ─┘                              │
│  Endpoint 10: IrManagement ────── 커스텀 클러스터 (0x1337FC01)│
│                                                              │
│  HTTP Server (port 80) ────────── REST API (같은 LAN 전용)   │
└──────────────────────────────────────────────────────────────┘
```

### 1.3 두 가지 API 인터페이스

| 인터페이스 | 프로토콜 | 용도 | 접근 범위 |
|-----------|---------|------|----------|
| **Matter Custom Cluster** | Matter (UDP/TCP) | 앱에서 IR 관리 | 커미셔닝된 모든 컨트롤러 |
| **HTTP REST API** | HTTP | 개발/디버깅, 대시보드 | 같은 LAN 내 |

**앱 개발 시 Matter Custom Cluster를 권장합니다.** HTTP REST API는 개발 편의용이며, 프로덕션 앱에서는 Matter 인터페이스를 사용하세요.

---

## 2. 커미셔닝 (장치 연결)

### 2.1 커미셔닝 파라미터

| 항목 | 값 |
|------|---|
| **Setup PIN** | `20202021` |
| **Discriminator** | `3840` (0xF00) |
| **Vendor ID** | `0xFFF1` (테스트) |
| **Product ID** | `0x8000` |
| **QR Code** | `MT:Y.K9042C00KA0648G00` |
| **Manual Code** | `34970112332` |
| **Rendezvous** | BLE 전용 |

### 2.2 커미셔닝 흐름

```
1. 앱 → BLE 스캔 → 허브 발견 (MATTER- 접두사)
2. 앱 → QR 코드 스캔 또는 Manual Code 입력
3. 앱 → BLE → PASE (PIN 인증)
4. 앱 → BLE → Wi-Fi 크레덴셜 전달
5. 허브 → Wi-Fi 연결
6. 앱 → IP → CASE 세션 수립
7. 커미셔닝 완료 → Fabric 등록
```

### 2.3 Multi-Fabric 지원

허브는 여러 컨트롤러(Apple Home, Google Home, 커스텀 앱 등)에 동시 등록 가능합니다. 추가 컨트롤러를 등록하려면 기존 컨트롤러에서 **커미셔닝 윈도우를 열어야** 합니다:

- Matter 방법: `OpenCommissioningWindow` 커맨드 (cmd `0x08`)
- HTTP 방법: `POST /api/commissioning/open`

---

## 3. Matter Custom Cluster — IrManagement

### 3.1 클러스터 식별

| 항목 | 값 | 설명 |
|------|---|------|
| **Cluster ID** | `0x1337FC01` | Vendor Prefix `0x1337` + Suffix `0xFC01` |
| **Endpoint** | `10` (현재 고정) | 앱은 반드시 Descriptor 탐색으로 확인해야 함 |
| **Device Type** | `0xFFF10001` | Manufacturer-specific |

> **중요**: 엔드포인트 ID는 동적 할당됩니다. 하드코딩하지 말고, root endpoint(0)의 Descriptor 클러스터 → PartsList를 순회하여 `0x1337FC01`을 가진 엔드포인트를 찾으세요.

### 3.2 엔드포인트 탐색 알고리즘

```
1. Root Endpoint(0) → Descriptor(0x001D) → PartsList(0x0003) 읽기
   → [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] 반환
2. 각 엔드포인트의 Descriptor → ServerList(0x0001) 읽기
3. ServerList에 0x1337FC01이 포함된 엔드포인트 = IrManagement 엔드포인트
```

### 3.3 속성 (Attributes) — 읽기 전용

#### `0x0000` LearnState

| 항목 | 값 |
|------|---|
| **타입** | uint8 (enum) |
| **용도** | IR 학습 세션의 현재 상태 |

| 값 | 상태 | 설명 |
|----|------|------|
| `0` | IDLE | 대기 중 |
| `1` | IN_PROGRESS | 학습 진행 중 — IR 수신 대기 |
| `2` | READY | 학습 완료 — SaveSignal로 저장 가능 |
| `3` | FAILED | 학습 실패 (타임아웃 등) |

#### `0x0001` LearnedPayload

| 항목 | 값 |
|------|---|
| **타입** | LongCharString (JSON) |
| **용도** | 현재 학습 세션의 상세 정보 |

```json
{
  "state": 1,
  "elapsed": 3200,
  "timeout": 15000,
  "last_id": 0,
  "rx": 1,
  "len": 67,
  "quality": 85
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `state` | int | LearnState와 동일 |
| `elapsed` | int | 경과 시간 (ms) |
| `timeout` | int | 설정된 타임아웃 (ms) |
| `last_id` | int | 마지막으로 저장된 signal ID (SaveSignal 후) |
| `rx` | int | 수신한 RX 채널 번호 (1 또는 2) |
| `len` | int | 캡처된 payload 길이 |
| `quality` | int | 신호 품질 점수 (0~100) |

#### `0x0002` SavedSignalsList

| 항목 | 값 |
|------|---|
| **타입** | LongCharString (JSON, 최대 8192 bytes) |
| **용도** | 저장된 모든 IR 신호 목록 |

```json
[
  {
    "id": 15,
    "name": "light_on",
    "type": "light",
    "carrier": 38000,
    "repeat": 1,
    "len": 67
  }
]
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `id` | uint32 | 신호 고유 ID |
| `name` | string | 신호 이름 (최대 47자) |
| `type` | string | 장치 유형 (light, tv, fan 등) |
| `carrier` | uint32 | 반송파 주파수 (Hz, 일반적으로 38000) |
| `repeat` | uint8 | 반복 횟수 |
| `len` | uint8 | payload 길이 |

#### `0x0003` SlotAssignments

| 항목 | 값 |
|------|---|
| **타입** | LongCharString (JSON, 최대 2048 bytes) |
| **용도** | 8개 브릿지 슬롯의 할당 상태 |

```json
[
  {
    "slot": 0,
    "ep": 2,
    "dev": 1,
    "name": "Light 1",
    "on": 15,
    "off": 16,
    "up": 15,
    "down": 16
  }
]
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `slot` | uint8 | 슬롯 번호 (0~7) |
| `ep` | uint16 | Matter 엔드포인트 ID (2~9) |
| `dev` | uint32 | 할당된 디바이스 ID (0 = 미할당) |
| `name` | string | 표시 이름 |
| `on` | uint32 | On 동작 signal ID |
| `off` | uint32 | Off 동작 signal ID |
| `up` | uint32 | Level Up 동작 signal ID |
| `down` | uint32 | Level Down 동작 signal ID |

#### `0x0004` RegisteredDevicesList

| 항목 | 값 |
|------|---|
| **타입** | LongCharString (JSON, 최대 4096 bytes) |
| **용도** | 등록된 가상 장치 목록 |

```json
[
  {
    "id": 1,
    "name": "Light 1",
    "type": "light",
    "on": 15,
    "off": 16,
    "up": 15,
    "down": 16
  }
]
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `id` | uint32 | 디바이스 고유 ID |
| `name` | string | 장치 이름 (최대 39자) |
| `type` | string | 장치 유형 |
| `on/off/up/down` | uint32 | 바인딩된 signal ID (0 = 미할당) |

### 3.4 커맨드 (Commands) — 클라이언트 → 서버

모든 커맨드는 성공 시 `DefaultSuccessResponse`(상태 코드 `0x00`)를 반환합니다.

#### `0x00` StartLearning

IR 학습 세션을 시작합니다. 허브의 IR 수신기가 활성화되고, LearnState가 `IN_PROGRESS(1)`로 전환됩니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint32 | 선택 | 타임아웃 (ms), 기본값 15000 |

**사용 후**: `LearnState` 속성을 1~2초 간격으로 폴링하거나, `LearningCompleted` 이벤트를 구독하세요.

#### `0x01` CancelLearning

현재 학습을 취소합니다.

> **참고**: 현재 구현에서는 `UNSUPPORTED(0x81)` 응답을 반환합니다. 학습은 타임아웃으로 자동 종료됩니다.

#### `0x02` SaveSignal

학습 완료 상태(`READY`)에서 캡처된 IR 신호를 NVS에 저장합니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | string | 필수 | 신호 이름 (최대 47자) |
| `1` | string | 선택 | 장치 유형 (기본값 "light", 최대 23자) |

**반환**: 성공 시 `LearnedPayload.last_id`에 새로 부여된 signal ID가 설정됩니다.

#### `0x03` DeleteSignal

저장된 IR 신호를 삭제합니다. 이 신호를 참조하는 디바이스의 바인딩은 자동으로 `0`(해제)으로 설정됩니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint32 | 필수 | 삭제할 signal ID |

#### `0x04` AssignSignalToDevice

등록된 디바이스에 IR 신호를 바인딩합니다. 각 동작(On/Off/Up/Down)에 대해 개별 신호를 지정합니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint32 | 필수 | 대상 device ID |
| `1` | uint32 | 필수 | On 동작 signal ID (0 = 해제) |
| `2` | uint32 | 필수 | Off 동작 signal ID |
| `3` | uint32 | 필수 | Level Up signal ID |
| `4` | uint32 | 필수 | Level Down signal ID |

#### `0x05` RegisterDevice

새로운 가상 장치를 생성합니다. 최대 16개 등록 가능.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | string | 필수 | 장치 이름 (최대 39자) |
| `1` | string | 선택 | 장치 유형 (기본값 "light", 최대 15자) |

#### `0x06` RenameDevice

기존 장치의 이름을 변경합니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint32 | 필수 | 대상 device ID |
| `1` | string | 필수 | 새 이름 (최대 39자) |

#### `0x07` AssignDeviceToSlot

등록된 디바이스를 브릿지 슬롯(0~7)에 할당합니다. 할당 후 해당 슬롯의 Matter 엔드포인트에서 OnOff/LevelControl 명령으로 IR 신호가 송신됩니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint8 | 필수 | 슬롯 번호 (0~7) |
| `1` | uint32 | 필수 | device ID (0 = 할당 해제) |

#### `0x08` OpenCommissioningWindow

추가 컨트롤러 등록을 위한 커미셔닝 윈도우를 엽니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint16 | 선택 | 타임아웃 (초, 기본값 300) |

#### `0x09` SendSignal

저장된 IR 신호를 즉시 송신합니다. device/slot 바인딩 없이 signal_id만으로 직접 제어할 수 있어, 커스텀 앱에서 TV 볼륨, 채널, 에어컨 모드 등 다양한 버튼을 자유롭게 구현할 수 있습니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint32 | 필수 | 송신할 signal ID |

**사용 예시**: 앱이 `SavedSignalsList`에서 신호 목록을 조회한 뒤, 사용자가 선택한 신호의 ID를 `SendSignal`로 전송.

#### `0x0A` GetSignalPayload

저장된 IR 신호의 raw timing payload를 조회합니다. 커맨드 실행 후 `SignalPayloadData`(attr `0x0005`) 속성에 결과가 저장되므로, 커맨드 호출 → 속성 읽기 순서로 사용합니다.

| TLV 태그 | 타입 | 필수 | 설명 |
|----------|------|------|------|
| `0` | uint32 | 필수 | 조회할 signal ID |

**사용 흐름**:
1. `GetSignalPayload(signal_id)` 커맨드 전송
2. `SignalPayloadData`(attr `0x0005`) 읽기 → JSON 반환

**응답 JSON** (attr `0x0005`에 저장):
```json
{
  "id": 15,
  "name": "light_on",
  "carrier": 38000,
  "repeat": 1,
  "ticks": [9265, 4490, 636, 539, 583, 563, ...]
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `id` | uint32 | 신호 ID |
| `name` | string | 신호 이름 |
| `carrier` | uint32 | 반송파 주파수 (Hz) |
| `repeat` | uint8 | 반복 횟수 |
| `ticks` | uint16[] | IR timing 데이터 (mark/space 교대, 단위: μs) |

**용도**: 앱 내 신호 백업, 다른 허브로 동기화, 신호 파형 시각화 등.

### 3.4.1 속성 (확장)

#### `0x0005` SignalPayloadData

| 항목 | 값 |
|------|---|
| **타입** | LongCharString (JSON, 최대 1024 bytes) |
| **용도** | `GetSignalPayload` 커맨드로 선택한 신호의 raw payload |
| **갱신 시점** | `GetSignalPayload` 커맨드 호출 시 |

초기값은 빈 객체(`{}`)입니다. `GetSignalPayload` 커맨드 호출 후에만 유효한 데이터가 들어갑니다.

### 3.5 이벤트 (Events)

#### `0x0000` LearningCompleted

IR 학습 세션이 완료(성공 또는 타임아웃)될 때 발생합니다.

| 항목 | 값 |
|------|---|
| **Priority** | Info |
| **발생 시점** | LearnState가 READY(2) 또는 FAILED(3)로 전환 시 |

이벤트 수신 후 `LearnedPayload` 속성을 읽어 결과를 확인하세요.

---

## 4. 브릿지 슬롯 제어 (표준 Matter 클러스터)

슬롯에 디바이스가 할당되면, 해당 엔드포인트의 표준 Matter 클러스터로 IR 제어가 가능합니다.

### 4.1 엔드포인트 매핑

| 슬롯 | 엔드포인트 | 표준 클러스터 |
|------|-----------|-------------|
| Slot 0 | Endpoint 2 | OnOff (0x0006), LevelControl (0x0008) |
| Slot 1 | Endpoint 3 | 〃 |
| ... | ... | 〃 |
| Slot 7 | Endpoint 9 | 〃 |

### 4.2 IR 신호 트리거 규칙

| Matter 동작 | IR 신호 |
|------------|--------|
| OnOff → On | `on_signal_id` 송신 |
| OnOff → Off | `off_signal_id` 송신 |
| LevelControl → 밝기 증가 | `level_up_signal_id` 송신 |
| LevelControl → 밝기 감소 | `level_down_signal_id` 송신 |
| LevelControl → 0으로 설정 | `off_signal_id` 송신 |
| LevelControl → 0에서 양수로 | `on_signal_id` 송신 |

### 4.3 Apple Home / Google Home에서 제어

커미셔닝 후 Apple Home이나 Google Home 앱에서 슬롯이 "조명"으로 표시됩니다. 사용자가 토글하면 바인딩된 IR 신호가 자동 송신됩니다.

---

## 5. HTTP REST API (개발/디버깅용)

> **접근**: `http://<hub-ip>/` (같은 LAN 내에서만 접근 가능)
> **Content-Type**: `application/json`
> **IP 확인**: DHCP로 할당되므로 mDNS(`esp-matter-hub-XXXXXX.local`) 또는 공유기에서 확인

### 5.1 시스템

#### `GET /api/health`

```json
{
  "status": "ok",
  "service": "esp-matter-hub",
  "slots": 8,
  "hostname": "esp-matter-hub-66c524",
  "fqdn": "esp-matter-hub-66c524.local",
  "mdns": "ready",
  "led_state": "ready"
}
```

### 5.2 신호 관리

#### `GET /api/signals`

저장된 모든 IR 신호 목록을 반환합니다.

```json
{
  "signals": [
    {
      "signal_id": 15,
      "name": "light_on",
      "device_type": "light",
      "carrier_hz": 38000,
      "repeat": 1,
      "payload_len": 67
    }
  ]
}
```

#### `DELETE /api/signals/{id}`

지정된 ID의 IR 신호를 삭제합니다. 참조하던 바인딩은 자동 해제됩니다.

### 5.3 IR 학습

#### `POST /api/learn/start`

IR 학습 세션을 시작합니다.

**요청**: `{"timeout_ms": 15000}`

#### `GET /api/learn/status`

학습 상태를 조회합니다.

```json
{
  "state": "idle",
  "elapsed_ms": 0,
  "timeout_ms": 0,
  "last_signal_id": 0,
  "rx_source": 0,
  "captured_len": 0,
  "quality_score": 0
}
```

| state 값 | 설명 |
|----------|------|
| `idle` | 대기 중 |
| `in_progress` | 학습 중 — IR 수신 대기 |
| `ready` | 완료 — commit 가능 |
| `failed` | 실패 (타임아웃 등) |

#### `POST /api/learn/commit`

학습된 신호를 저장합니다.

**요청**: `{"name": "TV 전원", "device_type": "tv"}`

**응답**: `{"status": "ok", "signal_id": 17}`

### 5.4 디바이스 관리

#### `GET /api/devices`

등록된 모든 가상 장치 목록을 반환합니다.

```json
{
  "devices": [
    {
      "device_id": 1,
      "name": "Light 1",
      "device_type": "light",
      "on_signal_id": 15,
      "off_signal_id": 16,
      "level_up_signal_id": 15,
      "level_down_signal_id": 16
    }
  ]
}
```

#### `POST /api/devices/register`

새 가상 장치를 등록합니다.

**요청**: `{"name": "거실 TV", "device_type": "tv"}`

#### `PUT /api/devices/{id}`

장치 이름을 변경합니다.

**요청**: `{"name": "새 이름"}`

### 5.5 슬롯 관리

#### `GET /api/slots`

8개 브릿지 슬롯의 할당 상태를 반환합니다.

```json
{
  "slots": [
    {
      "slot_id": 0,
      "role": "light",
      "endpoint_id": 2,
      "device_id": 1,
      "display_name": "Light 1",
      "on_signal_id": 15,
      "off_signal_id": 16,
      "level_up_signal_id": 15,
      "level_down_signal_id": 16
    }
  ]
}
```

#### `POST /api/slots/{id}/bind`

슬롯에 IR 신호를 바인딩합니다.

**요청**:
```json
{
  "on_signal_id": 15,
  "off_signal_id": 16,
  "level_up_signal_id": 15,
  "level_down_signal_id": 16
}
```

### 5.6 커미셔닝

#### `POST /api/commissioning/open`

커미셔닝 윈도우를 엽니다 (Multi-Fabric용).

**요청**: `{"timeout_s": 300}`

**응답**: `{"status": "ok", "commissioning_window": "opened", "timeout_s": 300}`

### 5.7 데이터 내보내기

#### `GET /api/export/nvs?scope={scope}`

NVS 데이터를 JSON으로 내보냅니다.

| scope | 설명 |
|-------|------|
| `signals` | 신호 데이터만 |
| `bindings` | 바인딩 + 디바이스 + 신호 |
| `all` | 전체 데이터 |

---

## 6. 앱 개발 가이드 — 일반적인 워크플로우

### 6.1 초기 설정 흐름

```
[1] 커미셔닝
    앱 → BLE → 허브 페어링 (QR 코드 또는 Manual Code)

[2] 엔드포인트 탐색
    Root(0) → Descriptor → PartsList 순회 → 0x1337FC01 찾기

[3] IR 신호 학습 (리모컨 ON 버튼)
    StartLearning(15000) → 폴링 LearnState → READY → SaveSignal("TV On", "tv")

[4] IR 신호 학습 (리모컨 OFF 버튼)
    StartLearning(15000) → 폴링 LearnState → READY → SaveSignal("TV Off", "tv")

[5] 디바이스 등록
    RegisterDevice("거실 TV", "tv") → device_id 반환

[6] 신호 바인딩
    AssignSignalToDevice(device_id, on_id, off_id, 0, 0)

[7] 슬롯 할당
    AssignDeviceToSlot(0, device_id)
    → 이제 Endpoint 2의 OnOff 토글로 TV 제어 가능
```

### 6.2 IR 학습 시퀀스 다이어그램

```
앱                   허브                  사용자
 │                    │                     │
 │──StartLearning──→ │                     │
 │                    │──IR 수신 대기──→    │
 │                    │                     │──리모컨 버튼 누름
 │                    │←──IR 신호 캡처──    │
 │←─LearnState=2────│                     │
 │                    │                     │
 │──SaveSignal──────→│                     │
 │←─Success─────────│                     │
 │                    │                     │
 │──Read Signals────→│                     │
 │←─[{id:17,...}]───│                     │
```

---

## 7. 모바일 SDK 연동

### 7.1 iOS (Matter.framework)

커스텀 클러스터는 `MTRBaseClusterOnOff` 같은 타입별 API를 사용할 수 없습니다. `MTRBaseDevice`의 범용 메서드를 사용합니다.

#### 엔드포인트 탐색

```swift
let device: MTRBaseDevice = ... // 커미셔닝 완료 후 획득
let irMgmtClusterId: NSNumber = NSNumber(value: 0x1337FC01)

// 1. Root endpoint(0)에서 PartsList 읽기
device.readAttributes(withEndpointID: NSNumber(value: 0),
                       clusterID: NSNumber(value: 0x001D),    // Descriptor
                       attributeID: NSNumber(value: 0x0003),  // PartsList
                       params: nil) { values, error in
    guard let values = values as? [[String: Any]] else { return }
    // values에서 endpoint ID 목록 추출

    // 2. 각 endpoint의 ServerList에서 0x1337FC01 검색
    for endpointId in endpointIds {
        device.readAttributes(
            withEndpointID: endpointId,
            clusterID: NSNumber(value: 0x001D),
            attributeID: NSNumber(value: 0x0001),  // ServerList
            params: nil
        ) { clusterValues, _ in
            // ServerList에 0x1337FC01이 있으면 이 endpoint가 IrManagement
        }
    }
}
```

#### 속성 읽기

```swift
let ep = NSNumber(value: 10)  // 탐색으로 확인한 엔드포인트
let cluster = NSNumber(value: 0x1337FC01)

// SavedSignalsList 읽기
device.readAttributes(withEndpointID: ep,
                       clusterID: cluster,
                       attributeID: NSNumber(value: 0x0002),
                       params: nil) { values, error in
    guard let values = values as? [[String: Any]],
          let data = values.first?["data"] as? [String: Any],
          let jsonString = data["value"] as? String,
          let jsonData = jsonString.data(using: .utf8) else { return }

    let signals = try? JSONSerialization.jsonObject(with: jsonData)
    // signals: [[String: Any]] — 신호 배열
}
```

#### 커맨드 전송

```swift
// StartLearning (cmd 0x00)
let fields: [String: Any] = [
    "type": "Structure",
    "value": [
        ["contextTag": 0, "type": "UnsignedInteger", "value": 15000]
    ]
]

device.invokeCommand(withEndpointID: ep,
                      clusterID: cluster,
                      commandID: NSNumber(value: 0x00),
                      commandFields: fields as NSDictionary,
                      timedInvokeTimeout: nil) { _, error in
    if error == nil { print("학습 시작 성공") }
}

// SaveSignal (cmd 0x02)
let saveFields: [String: Any] = [
    "type": "Structure",
    "value": [
        ["contextTag": 0, "type": "UTF8String", "value": "TV 전원"],
        ["contextTag": 1, "type": "UTF8String", "value": "tv"]
    ]
]

device.invokeCommand(withEndpointID: ep,
                      clusterID: cluster,
                      commandID: NSNumber(value: 0x02),
                      commandFields: saveFields as NSDictionary,
                      timedInvokeTimeout: nil) { _, error in
    // 저장 결과 처리
}

// AssignSignalToDevice (cmd 0x04)
let assignFields: [String: Any] = [
    "type": "Structure",
    "value": [
        ["contextTag": 0, "type": "UnsignedInteger", "value": deviceId],
        ["contextTag": 1, "type": "UnsignedInteger", "value": onSignalId],
        ["contextTag": 2, "type": "UnsignedInteger", "value": offSignalId],
        ["contextTag": 3, "type": "UnsignedInteger", "value": 0],
        ["contextTag": 4, "type": "UnsignedInteger", "value": 0]
    ]
]

device.invokeCommand(withEndpointID: ep,
                      clusterID: cluster,
                      commandID: NSNumber(value: 0x04),
                      commandFields: assignFields as NSDictionary,
                      timedInvokeTimeout: nil) { _, error in
    // 바인딩 결과 처리
}
```

#### 이벤트 구독

```swift
// LearningCompleted 이벤트 구독
device.subscribeToEvents(
    withEndpointID: ep,
    clusterID: cluster,
    eventID: NSNumber(value: 0x0000),
    params: MTRSubscribeParams(minInterval: 1, maxInterval: 10)
) { values, error in
    if values != nil {
        // 학습 완료 → LearnedPayload 읽기
    }
}
```

### 7.2 Android (CHIP SDK)

`ChipDeviceController`의 `invoke` 및 `readPath` 메서드를 사용합니다. TLV 인코딩은 `TlvWriter`로 직접 구성합니다.

#### 속성 읽기

```kotlin
val endpointId = 10  // 탐색으로 확인한 엔드포인트
val clusterId = 0x1337FC01L
val attrId = 0x0002L  // SavedSignalsList

val path = ChipAttributePath.newInstance(endpointId.toLong(), clusterId, attrId)

controller.readPath(
    object : ReportCallback {
        override fun onReport(nodeState: NodeState) {
            val attr = nodeState
                .getEndpointState(endpointId)
                ?.getClusterState(clusterId)
                ?.getAttributeState(attrId)

            val jsonString = attr?.value?.let { TlvReader(it).getString(AnonymousTag) }
            val signals = JSONArray(jsonString)
            // signals 처리
        }
        override fun onError(path: ChipAttributePath?, ex: Exception) {
            Log.e("IrMgmt", "읽기 실패", ex)
        }
    },
    devicePtr,
    listOf(path),
    0
)
```

#### 커맨드 전송

```kotlin
// StartLearning (cmd 0x00)
val tlv = TlvWriter()
tlv.startStructure(AnonymousTag)
tlv.put(ContextSpecificTag(0), 15000u)  // timeoutMs
tlv.endStructure()

val element = InvokeElement.newInstance(
    endpointId.toLong(), clusterId, 0x00L, tlv.getEncoded(), null
)

controller.invoke(
    object : InvokeCallback {
        override fun onResponse(element: InvokeElement?, successCode: Long) {
            Log.d("IrMgmt", "StartLearning 성공")
        }
        override fun onError(ex: Exception) {
            Log.e("IrMgmt", "StartLearning 실패", ex)
        }
    },
    devicePtr, element, 0, 0
)

// RegisterDevice (cmd 0x05)
val regTlv = TlvWriter()
regTlv.startStructure(AnonymousTag)
regTlv.put(ContextSpecificTag(0), "거실 TV")
regTlv.put(ContextSpecificTag(1), "tv")
regTlv.endStructure()

val regElement = InvokeElement.newInstance(
    endpointId.toLong(), clusterId, 0x05L, regTlv.getEncoded(), null
)
controller.invoke(callback, devicePtr, regElement, 0, 0)

// AssignSignalToDevice (cmd 0x04)
val bindTlv = TlvWriter()
bindTlv.startStructure(AnonymousTag)
bindTlv.put(ContextSpecificTag(0), deviceId.toUInt())   // deviceId
bindTlv.put(ContextSpecificTag(1), onSignalId.toUInt())  // on
bindTlv.put(ContextSpecificTag(2), offSignalId.toUInt()) // off
bindTlv.put(ContextSpecificTag(3), 0u)                   // up
bindTlv.put(ContextSpecificTag(4), 0u)                   // down
bindTlv.endStructure()

val bindElement = InvokeElement.newInstance(
    endpointId.toLong(), clusterId, 0x04L, bindTlv.getEncoded(), null
)
controller.invoke(callback, devicePtr, bindElement, 0, 0)
```

> **참고**: 속성에서 반환되는 JSON 문자열은 앱 내에서 직접 파싱해야 합니다.
> iOS: `JSONSerialization`, Android: `org.json.JSONArray`/`JSONObject`

---

## 8. 에러 처리

### 8.1 Matter 상태 코드

| 코드 | 이름 | 의미 | 발생 조건 |
|------|------|------|----------|
| `0x00` | Success | 성공 | 정상 처리 |
| `0x01` | Failure | 일반 실패 | NVS 저장 실패, 내부 오류 등 |
| `0x81` | UnsupportedCommand | 미지원 | CancelLearning 호출 시 |
| `0x85` | InvalidCommand | 잘못된 인자 | 존재하지 않는 ID, 범위 초과 등 |

### 8.2 주요 에러 시나리오

| 시나리오 | 커맨드 | 응답 |
|---------|-------|------|
| 학습 중이 아닌데 SaveSignal | `0x02` | `InvalidCommand (0x85)` |
| 존재하지 않는 signal ID 삭제 | `0x03` | `InvalidCommand (0x85)` |
| 존재하지 않는 device ID 바인딩 | `0x04` | `InvalidCommand (0x85)` |
| 슬롯 번호 8 이상 지정 | `0x07` | `InvalidCommand (0x85)` |
| 디바이스 등록 한도(16개) 초과 | `0x05` | `Failure (0x01)` |
| 신호 저장 한도(64개) 초과 | `0x02` | `Failure (0x01)` |

---

## 9. 제한사항 및 참고사항

### 9.1 용량 제한

| 항목 | 최대값 |
|------|-------|
| 저장 가능 IR 신호 | 64개 |
| 등록 가능 디바이스 | 16개 |
| 브릿지 슬롯 | 8개 (고정) |
| 신호 이름 | 47자 |
| 디바이스 이름 | 39자 |
| 디바이스 유형 | 15~23자 |

### 9.2 알아두어야 할 점

- **엔드포인트 ID 하드코딩 금지**: 현재 10이지만, Descriptor 탐색으로 확인해야 합니다.
- **JSON 속성 파싱**: 모든 목록 속성은 JSON 문자열로 반환되므로 앱에서 파싱해야 합니다.
- **학습 타임아웃**: 기본 15초. IR 신호를 수신하지 못하면 `FAILED` 상태로 전환됩니다.
- **신호 삭제 cascade**: 신호 삭제 시 해당 신호를 참조하는 모든 디바이스 바인딩이 자동 해제됩니다.
- **IP 주소 변동**: 허브 IP는 DHCP로 할당됩니다. mDNS hostname을 사용하거나, 공유기에서 고정 IP를 설정하세요.
- **Vendor ID 0xFFF1**: 테스트용 Vendor ID입니다. Apple Home에서 사용하려면 Apple Developer Profile이 설치되어 있어야 할 수 있습니다.

### 9.3 chip-tool CLI 테스트

개발 중 chip-tool로 빠르게 테스트할 수 있습니다:

```bash
# 속성 읽기
chip-tool any read-by-id 0x1337FC01 0x0002 <node-id> <endpoint-id>

# StartLearning
chip-tool any command-by-id 0x1337FC01 0x00 '{"0:U32":15000}' <node-id> <endpoint-id>

# SaveSignal
chip-tool any command-by-id 0x1337FC01 0x02 '{"0:STR":"TV Power","1:STR":"tv"}' <node-id> <endpoint-id>

# RegisterDevice
chip-tool any command-by-id 0x1337FC01 0x05 '{"0:STR":"My TV","1:STR":"tv"}' <node-id> <endpoint-id>

# AssignSignalToDevice
chip-tool any command-by-id 0x1337FC01 0x04 '{"0:U32":1,"1:U32":1,"2:U32":2,"3:U32":0,"4:U32":0}' <node-id> <endpoint-id>

# AssignDeviceToSlot
chip-tool any command-by-id 0x1337FC01 0x07 '{"0:U8":0,"1:U32":1}' <node-id> <endpoint-id>

# OpenCommissioningWindow
chip-tool any command-by-id 0x1337FC01 0x08 '{"0:U16":300}' <node-id> <endpoint-id>

# SendSignal (임의 IR 신호 즉시 송신)
chip-tool any command-by-id 0x1337FC01 0x09 '{"0:U32":15}' <node-id> <endpoint-id>

# GetSignalPayload (신호 raw payload 조회) → 이후 attr 0x0005 읽기
chip-tool any command-by-id 0x1337FC01 0x0A '{"0:U32":15}' <node-id> <endpoint-id>
chip-tool any read-by-id 0x1337FC01 0x0005 <node-id> <endpoint-id>
```
