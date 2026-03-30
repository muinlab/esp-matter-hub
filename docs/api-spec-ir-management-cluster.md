# ESP Matter IR Hub — API 명세서 v3.2

> **대상**: 앱 개발자
> **버전**: 3.2 (2026-03-31)

---

## 1. 연결 정보

| 항목 | 값 |
|------|---|
| Cluster ID | `0x1337FC01` |
| Endpoint | 동적 (기본 10, Descriptor로 탐색) |
| Device Type | `0xFFF10001` |

---

## 2. 커맨드

### 2.1 SendSignalWithRaw (0x0B) — IR 신호 송신

허브에 IR 신호를 전송합니다. 신호는 버퍼에 저장되어 빠른 재송신에 사용됩니다.

| Tag | 필드 | 타입 | 설명 |
|-----|------|------|------|
| 0 | signal_id | uint32 | 신호 고유 ID (앱이 지정) |
| 1 | carrier_hz | uint32 | IR 캐리어 주파수 (보통 38000) |
| 2 | repeat | uint8 | 반복 횟수 (1~5) |
| 3 | ticks | octet_string | uint16 LE 인코딩 타이밍 데이터 (µs) |

**chip-tool 예시:**
```bash
chip-tool any command-by-id 0x1337FC01 0x0B \
  '{"0:U32":100, "1:U32":38000, "2:U8":1, "3:BYTES":"41248A117C02"}' \
  1 10 --commissioner-name beta
```

### 2.2 StartLearning (0x00) — IR 학습 시작

IR 수신기를 활성화하고 리모컨 신호를 대기합니다.

| Tag | 필드 | 타입 | 설명 |
|-----|------|------|------|
| 0 | timeout_ms | uint32 | 타임아웃 (기본 15000ms) |

**학습 프로세스:**
```
앱                              허브
 │── StartLearning(15000) ────→ │ IR 수신기 활성화, 타이머 시작
 │                               │ (사용자가 리모컨 버튼 누름)
 │── LearnedPayload 폴링 ─────→ │
 │←── state=1 (진행 중) ──────── │
 │── LearnedPayload 폴링 ─────→ │
 │←── state=2 + carrier + ticks  │ 학습 성공! IR 데이터 포함
 │                               │
 │  앱이 carrier + ticks 저장    │
 │                               │
 │── SendSignalWithRaw ────────→ │ 저장한 ticks로 IR 재생
```

**LearnState 값:**

| 값 | 상태 | 설명 | 앱 동작 |
|----|------|------|---------|
| 0 | IDLE | 대기 중 | StartLearning 호출 |
| 1 | IN_PROGRESS | 학습 중, IR 신호 대기 | 계속 폴링 |
| 2 | READY | 학습 성공, 데이터 준비됨 | LearnedPayload에서 carrier + ticks 추출 |
| 3 | FAILED | 타임아웃, 학습 실패 | 사용자에게 재시도 안내 |

**폴링 방법:** `LearnedPayload(0x0001)`만 읽으면 state + carrier + ticks를 한번에 확인할 수 있습니다. 별도로 `LearnState(0x0000)`를 읽을 필요 없습니다.

### 2.3 CancelLearning (0x01) — 학습 취소

진행 중인 학습을 취소합니다. 파라미터 없음.

### 2.4 OpenCommissioning (0x08) — 커미셔닝 창 열기

| Tag | 필드 | 타입 | 설명 |
|-----|------|------|------|
| 0 | timeout_s | uint16 | 타임아웃 초 (기본 300) |

### 2.5 SyncBuffer (0x0C) — 버퍼 → NVS 동기화

파라미터 없음. 버퍼의 모든 신호를 NVS에 저장한 후 버퍼를 비웁니다 (clear).

**동작**:
1. 버퍼의 모든 유효한 항목을 NVS에 저장
2. 버퍼 전체 초기화 (invalid 처리)
3. 결과를 **BufferSnapshot (0x0007)** 속성에 기록

```bash
# 실행 후 스냅샷 조회
chip-tool any command-by-id 0x1337FC01 0x0C '{}' 1 10
chip-tool any read-by-id 0x1337FC01 0x0007 1 10
```

### 2.6 DumpNVS (0x0E) — NVS 전체 신호 조회

파라미터 없음. 허브에 저장된 모든 NVS 신호를 조회합니다.

**동작**:
1. SyncBuffer 실행 (버퍼 → NVS 저장 + 버퍼 비움)
2. NVS의 모든 IR 신호 데이터 읽기
3. 결과를 **BufferSnapshot (0x0007)** 속성에 JSON 배열로 기록

**사용 사례**: 앱이 허브의 모든 저장된 신호를 한 번에 조회

```bash
chip-tool any command-by-id 0x1337FC01 0x0E '{}' 1 10
chip-tool any read-by-id 0x1337FC01 0x0007 1 10
```

### 2.7 FactoryReset (0x0D) — 팩토리 리셋

파라미터 없음. Matter 팩토리 리셋을 수행합니다. NVS 전체 초기화 + 재부팅.

---

## 3. 속성 (읽기 전용)

| ID | 이름 | 타입 | 설명 |
|----|------|------|------|
| 0x0000 | LearnState | uint8 | 학습 상태: 0=IDLE, 1=IN_PROGRESS, 2=READY, 3=FAILED |
| 0x0001 | LearnedPayload | long_char_str | 학습 결과 JSON (READY 상태일 때 carrier와 ticks 포함) |
| 0x0007 | BufferSnapshot | long_char_str | DumpNVS/SyncBuffer 실행 후 신호 데이터 JSON 배열 |

**LearnedPayload 형식 (READY 상태):**
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

**주의**: `ticks`는 uint16 LE 인코딩 바이트 hex입니다. 이 값을 그대로 SendSignalWithRaw의 BYTES 파라미터에 사용할 수 있습니다.

**BufferSnapshot 형식 (SyncBuffer/DumpNVS 실행 후):**
```json
[
  {"signal_id":100, "carrier_hz":38000, "repeat":1, "item_count":32, "ref_count":3, "last_seen_at":1743400000},
  ...
]
```

---

## 4. Signal Buffer

- 16개 엔트리, LRU 교체
- `SendSignalWithRaw`로 전송된 신호가 자동 저장
- 버퍼가 가득 차면 NVS에 일괄 저장 (ref_count 증가, timestamp 기록)
- `SyncBuffer(0x0C)`로 수동 flush 가능
- 재부팅 시 슬롯에 설정된 signal_id를 NVS에서 복원

---

## 5. 앱 개발 흐름

```
1. 커미셔닝
   OpenCommissioning(0x08) → BLE WiFi 페어링

2. IR 학습
   StartLearning(0x00) → 리모컨 버튼 누름 → LearnState 폴링 (READY 확인)
   → LearnedPayload 읽기 (carrier + ticks 획득) → 앱에 저장

3. IR 송신
   SendSignalWithRaw(0x0B, signal_id, carrier, repeat, ticks)
   → 허브가 IR 송신 + 버퍼 저장

4. NVS 동기화
   DumpNVS(0x0E) → BufferSnapshot(0x0007) 읽기 → 모든 NVS 신호 조회

5. 초기화
   FactoryReset(0x0D) → 허브 전체 초기화
```

**주의사항**:
- LearnedPayload의 ticks hex는 SendSignalWithRaw에 그대로 사용 가능
- SyncBuffer(0x0C)는 앱이 허브 리부팅 전에 버퍼를 강제 저장할 때 사용
- DumpNVS(0x0E)는 허브의 모든 저장된 신호를 조회할 때 사용

---

## 6. 변경 이력

| 버전 | 날짜 | 변경 내용 |
|------|------|-----------|
| 3.2.1 | 2026-03-31 | DumpNVS(0x0E) 추가, LearnedPayload에 ticks 포함, SyncBuffer 버퍼 비움, 미사용 커맨드/속성 제거 |
| 3.2 | 2026-03-31 | button_type 슬롯 모델, SendSignalWithRaw 4-param 단순화, SyncBuffer/FactoryReset 추가 |
| 3.1 | 2026-03-30 | 앱 중심 신호 관리, SendSignalWithRaw 도입, 캐시+NVS 하이브리드 |
