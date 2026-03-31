# ESP Matter IR Hub — API 명세서 v3.5

> **대상**: 앱 개발자
> **버전**: 3.5.0 (2026-03-31)

---

## 1. 연결 정보

| 항목 | 값 |
|------|---|
| Cluster ID | `0x1337FC01` |
| Endpoint | 동적 (기본 10, Descriptor로 탐색) |
| Device Type | `0xFFF10001` |

---

## 2. 커맨드

모든 커맨드의 응답은 Matter Status Code로 반환됩니다.

| Status | 값 | 설명 |
|--------|---|------|
| SUCCESS | 0x00 | 성공 |
| FAILURE | 0x01 | 일반 실패 |
| INVALID_COMMAND | 0x85 | 잘못된 파라미터 |
| INVALID_ACTION | 0x86 | 현재 상태에서 실행 불가 |

### 2.1 SendSignalWithRaw (0x0B) — IR 신호 송신

**Request:**

| Tag | 필드 | 타입 | 필수 | 설명 |
|-----|------|------|------|------|
| 0 | signal_id | uint32 | O | 신호 고유 ID (앱이 지정) |
| 1 | carrier_hz | uint32 | O | IR 캐리어 주파수 (보통 38000) |
| 2 | repeat | uint8 | O | 반복 횟수 (1~5) |
| 3 | ticks | octet_string | O | uint16 LE 인코딩 타이밍 데이터 (µs). hex string 또는 바이너리 모두 지원 |

**ticks 인코딩 주의사항:**
- chip-tool의 BYTES 타입은 hex string을 ASCII 그대로 전달합니다 (hex→byte 디코딩 안 함)
- 허브가 자동 감지하여 hex 디코딩합니다 (ASCII hex 문자열 → 바이트 변환)
- 앱에서 직접 Matter TLV로 보낼 경우 바이너리 octet_string으로 전달 가능
- LearnedPayload의 ticks hex를 그대로 사용 가능

**Response:** Matter Status Code

| 상황 | Status |
|------|--------|
| IR 송신 성공 | SUCCESS (0x00) |
| ticks가 비어있거나 홀수 바이트 | INVALID_COMMAND (0x85) |
| 학습 진행 중 | INVALID_ACTION (0x86) |

### 2.2 StartLearning (0x00) — IR 학습 시작

**Request:**

| Tag | 필드 | 타입 | 필수 | 설명 |
|-----|------|------|------|------|
| 0 | timeout_ms | uint32 | X | 타임아웃 (기본 15000ms) |

**Response:** Matter Status Code

| 상황 | Status |
|------|--------|
| 학습 시작됨 | SUCCESS (0x00) |
| 이미 학습 진행 중 | INVALID_ACTION (0x86) |

**학습 결과 확인 — LearnedPayload(0x0001) 속성 폴링:**

```
앱                              허브
 │── StartLearning(15000) ────→ │ IR 수신기 활성화, 타이머 시작
 │                               │ (사용자가 리모컨 버튼 누름)
 │── LearnedPayload 폴링 ─────→ │
 │←── {"state":1, ...} ───────── │ 진행 중
 │── LearnedPayload 폴링 ─────→ │
 │←── {"state":2, "carrier":38000, "ticks":"A801..."} │ 성공!
 │                               │
 │  앱이 carrier + ticks 저장    │
 │── SendSignalWithRaw ────────→ │ 저장한 ticks로 IR 재생
```

**LearnState 값:**

| 값 | 상태 | 설명 | 앱 동작 |
|----|------|------|---------|
| 0 | IDLE | 대기 중 | StartLearning 호출 |
| 1 | IN_PROGRESS | 학습 중, IR 신호 대기 | 계속 폴링 (~500ms 간격) |
| 2 | READY | 학습 성공, 데이터 준비됨 | carrier + ticks 추출 후 앱에 저장 |
| 3 | FAILED | 타임아웃, 학습 실패 | 사용자에게 재시도 안내 |

### 2.3 CancelLearning (0x01) — 학습 취소

**Request:** 파라미터 없음

**Response:** Matter Status Code (SUCCESS)

### 2.4 OpenCommissioning (0x08) — 커미셔닝 창 열기

**Request:**

| Tag | 필드 | 타입 | 필수 | 설명 |
|-----|------|------|------|------|
| 0 | timeout_s | uint16 | X | 타임아웃 초 (기본 300) |

**Response:** Matter Status Code (SUCCESS / FAILURE)

### 2.5 SyncBuffer (0x0C) — DumpNVS 별칭

**Request:** 파라미터 없음

**Response:** Matter Status Code (SUCCESS)

> v3.3부터 RAM 버퍼가 제거되어 DumpNVS(0x0E)와 동일하게 동작합니다.

### 2.6 DumpNVS (0x0E) — NVS 전체 신호 조회

**Request:** 파라미터 없음

**Response:** Matter Status Code (SUCCESS)

**동작 후 속성 변화:**
- NVS의 모든 IR 신호를 BufferSnapshot(0x0007)에 JSON 배열로 기록

### 2.7 FactoryReset (0x0D) — 팩토리 리셋

**Request:** 파라미터 없음

**Response:** 없음 (디바이스 즉시 재부팅)

---

## 3. 속성 (읽기 전용)

### 3.1 LearnState (0x0000)

| 항목 | 값 |
|------|---|
| 타입 | uint8 |
| 읽기 | `read-by-id 0x1337FC01 0x0000` |

**Response:** 정수 값 (0~3)

```
0 = IDLE
1 = IN_PROGRESS
2 = READY
3 = FAILED
```

### 3.2 LearnedPayload (0x0001)

| 항목 | 값 |
|------|---|
| 타입 | long_char_str (JSON) |
| 최대 크기 | 1280 bytes |
| 읽기 | `read-by-id 0x1337FC01 0x0001` |

**Response (state != READY):**
```json
{
  "state": 1,
  "elapsed": 2500,
  "timeout": 15000,
  "last_id": 0,
  "rx": 0,
  "len": 0,
  "quality": 0
}
```

**Response (state == READY — 학습 성공):**
```json
{
  "state": 2,
  "elapsed": 3200,
  "timeout": 15000,
  "last_id": 0,
  "rx": 1,
  "len": 48,
  "quality": 48,
  "carrier": 38000,
  "ticks": "A801F402380258035803..."
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| state | int | 학습 상태 (0~3) |
| elapsed | int | 경과 시간 (ms) |
| timeout | int | 설정된 타임아웃 (ms) |
| last_id | int | 마지막 신호 ID |
| rx | int | 수신 소스 번호 |
| len | int | 캡처된 ticks 수 |
| quality | int | 신호 품질 점수 |
| carrier | int | IR 캐리어 주파수 (Hz). **READY일 때만** |
| ticks | string | IR 타이밍 데이터 (hex). **READY일 때만** |

**ticks 형식:** uint16 LE 바이트 hex. `SendSignalWithRaw`의 `ticks` 파라미터에 그대로 사용 가능.

### 3.3 BufferSnapshot (0x0007)

| 항목 | 값 |
|------|---|
| 타입 | long_char_str (JSON 배열) |
| 최대 크기 | 2048 bytes |
| 읽기 | `read-by-id 0x1337FC01 0x0007` |
| 갱신 시점 | SyncBuffer(0x0C) 또는 DumpNVS(0x0E) 실행 후 |

**Response:**
```json
[
  {
    "signal_id": 100,
    "carrier_hz": 38000,
    "repeat": 1,
    "tick_count": 48,
    "ref_count": 3,
    "last_seen_at": 1743400000
  },
  {
    "signal_id": 200,
    "carrier_hz": 38000,
    "repeat": 1,
    "tick_count": 32,
    "ref_count": 1,
    "last_seen_at": 1743400500
  }
]
```

| 필드 | 타입 | 설명 |
|------|------|------|
| signal_id | int | 신호 고유 ID |
| carrier_hz | int | 캐리어 주파수 (Hz) |
| repeat | int | 반복 횟수 |
| tick_count | int | ticks 수 |
| ref_count | int | NVS 저장 횟수 (버퍼→NVS 시 증가) |
| last_seen_at | int | 마지막 저장 Unix timestamp (SNTP 동기화 전이면 0) |

---

## 4. Signal Buffer (RAM)

- 16개 엔트리, LRU 교체 (재부팅 시 소멸)
- `SendSignalWithRaw`로 전송된 신호가 자동 저장 (RAM + NVS 즉시 저장)
- 동일 signal_id 재수신 시 최상단으로 이동 (중복 없음)
- NVS에는 매 송신마다 즉시 저장 (ref_count 증가, timestamp history 기록)

---

## 5. NVS 네임스페이스 (v3.5)

| 네임스페이스 | 용도 | 키 형식 |
|-------------|------|---------|
| `web_config` | API 키 | `api_key` (string) |
| `bridge_map` | 슬롯 설정 | `s{N}_type`, `s{N}_a`, `s{N}_b` |
| `ir_cache` | IR 신호 데이터 | `c{signal_id}` (blob, 56B header + ticks) |
| `act_log` | 활동 로그 | `head`, `count` (u16), `e{N}` (blob, N=0..49) |
| `test_sig` | 테스트 신호 | `count` (u8), `t{N}` (blob, N=0..15) |

---

## 6. 앱 개발 흐름

```
1. 커미셔닝
   OpenCommissioning(0x08) → BLE WiFi 페어링

2. IR 학습
   StartLearning(0x00) → 리모컨 버튼 누름
   → LearnedPayload(0x0001) 폴링 (~500ms)
   → state==2: carrier + ticks 추출 → 앱에 저장
   → state==3: 실패, 재시도

3. IR 송신
   SendSignalWithRaw(0x0B, signal_id, carrier, repeat, ticks)
   → ticks는 학습에서 받은 hex 그대로 사용

4. NVS 동기화
   DumpNVS(0x0E) → BufferSnapshot(0x0007) 읽기

5. 초기화
   FactoryReset(0x0D)
```

---

## 7. 변경 이력

| 버전 | 날짜 | 변경 내용 |
|------|------|-----------|
| 3.5.0 | 2026-03-31 | NVS 네임스페이스 정리 (act_log, test_sig 추가), Activity Log NVS FIFO 50개, IR Learn 완료 시 로그 기록, Signal Buffer RAM LRU 16개, Saved Test Signals NVS 16개, SyncBuffer/DumpNVS 별칭 유지 |
| 3.2.3 | 2026-03-31 | IR TX 안정화 (rmt_set_tx_carrier 제거), chip-tool BYTES hex 자동 디코딩, ticks 인코딩 주의사항 추가 |
| 3.2.2 | 2026-03-31 | Request/Response 인터페이스 명시, SaveSignal(0x02) 제거 |
| 3.2.1 | 2026-03-31 | DumpNVS(0x0E) 추가, LearnedPayload에 ticks 포함, SyncBuffer 버퍼 비움, 미사용 커맨드/속성 제거 |
| 3.2 | 2026-03-31 | button_type 슬롯 모델, SendSignalWithRaw 4-param 단순화, SyncBuffer/FactoryReset 추가 |
| 3.1 | 2026-03-30 | 앱 중심 신호 관리, SendSignalWithRaw 도입, 캐시+NVS 하이브리드 |
