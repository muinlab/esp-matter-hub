# ESP Matter IR Hub v3.2 — 테스트 계획

> **작성일**: 2026-03-31
> **대상**: v3.2 신규/변경 기능

---

## TC-01: BLE WiFi 커미셔닝

1. `erase-flash` 후 `./run_esp32-s3`로 플래시
2. REPL에서 `pair_auto` 실행
3. **기대**: "Paired successfully" + WiFi 연결 + IP 할당
4. **확인**: 시리얼 로그에서 `Commissioning completed successfully`

## TC-02: IR 학습 + LearnedPayload

1. REPL에서 `learn 15000`
2. IR 리모컨 버튼 누름
3. `learned_payload` 읽기
4. **기대**: state=2, carrier=38000, ticks="A801F4..." (hex)
5. **확인**: ticks가 비어있지 않고 LE hex 형식

## TC-03: SendSignalWithRaw

1. TC-02에서 얻은 ticks를 사용
2. `send_raw 100 <ticks_hex> 38000 1`
3. **기대**: OK (Status=0x0), IR LED 송신
4. **확인**: 시리얼 로그에서 `SendSignalWithRaw sig=100`

## TC-04: Signal Buffer 동작

1. TC-03을 3회 반복 (signal_id 100, 200, 300)
2. 웹 UI에서 Signal Buffer 확인
3. **기대**: 3개 엔트리 표시
4. **확인**: signal_id, carrier, repeat, item_count 값 정상

## TC-05: SyncBuffer (0x0C)

1. TC-04 상태에서 `sync_buffer`
2. `buffer_snapshot` 읽기
3. **기대**: JSON 배열에 3개 신호, ref_count=1
4. **확인**: 웹 UI에서 버퍼가 비어있음 (clear 확인)

## TC-06: DumpNVS (0x0E)

1. TC-05 후 `send_raw 400 <ticks> 38000 1` (새 신호)
2. `dump_nvs`
3. `buffer_snapshot` 읽기
4. **기대**: NVS의 4개 신호 (100, 200, 300, 400) + ref_count 정상

## TC-07: 슬롯 설정 (웹 UI)

1. 웹 UI 접속 → API Key 입력 → Unlock
2. Slot 0: ONOFF, signal_a=100, signal_b=101 → Save
3. **기대**: 저장 성공, 새로고침 후 값 유지
4. **확인**: 재부팅 후에도 슬롯 설정 유지 (NVS)

## TC-08: 웹 API 키 인증

1. 웹 접속 → API Key 없이 접근 시도
2. **기대**: 잠금 화면만 표시
3. 올바른 키 입력 → Unlock
4. **기대**: 대시보드 전체 표시
5. 재부팅 후 같은 키로 접속
6. **기대**: 키가 NVS에 유지되어 동일 키 사용 가능

## TC-09: FactoryReset (0x0D)

1. `send_cmd 0x0D`
2. **기대**: 디바이스 재부팅, 커미셔닝 초기화
3. **확인**: 재부팅 후 BLE 광고 시작, 커미셔닝 필요

## TC-10: SNTP 시간 동기화

1. 커미셔닝 후 WiFi 연결 확인
2. 시리얼 로그에서 `SNTP initialized` 확인
3. `send_raw` 후 `dump_nvs` → `buffer_snapshot`
4. **기대**: last_seen_at 값이 0이 아닌 Unix timestamp
