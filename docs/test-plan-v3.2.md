# ESP Matter IR Hub v3.3 — 테스트 계획

> **작성일**: 2026-03-31
> **대상**: v3.3 신규/변경 기능

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
4. **기대**: state=2, carrier=38000, ticks="A801F4..." (LE hex)
5. **확인**: ticks 비어있지 않음. 속성이 자동 갱신됨 (별도 폴링 불필요)

## TC-03: SendSignalWithRaw (hub_api_test)

1. TC-02에서 얻은 ticks 사용
2. `send_raw 100 <ticks_hex> 38000 1`
3. **기대**: OK, IR LED 송신, 시리얼에 `ticks=67 (raw_bytes=268 decoded=134)`
4. **확인**: hex 자동 디코딩 동작. IR 기기 반응.

## TC-04: 웹 IR Replay

1. 웹 UI에서 IR Learn → Start → 리모컨 누름
2. Captured! 후 Replay 버튼 + repeat 입력 표시
3. repeat=3으로 Replay 클릭
4. **기대**: "Replayed! (x3)", IR 기기 반응
5. **확인**: 시리얼에 `TX raw signal_id=0 items=34 repeat=3`

## TC-05: NVS 즉시 저장

1. TC-03 실행 (send_raw 100)
2. `dump_nvs` 실행
3. **기대**: signal_id=100이 NVS에 저장됨. ref_count=1, last_seen_at > 0
4. 같은 signal_id로 3회 더 send_raw
5. `dump_nvs` 다시 실행
6. **기대**: ref_count=4

## TC-06: NVS 재부팅 생존

1. TC-05 후 디바이스 재부팅 (`./run_esp32-s3`)
2. 커미셔닝 후 `dump_nvs`
3. **기대**: signal_id=100이 NVS에 여전히 존재. ref_count 유지.

## TC-07: DumpNVS (0x0E)

1. send_raw로 여러 signal_id 전송 (100, 200, 300)
2. `dump_nvs`
3. **기대**: 3개 신호 모두 JSON 배열로 반환
4. **확인**: 각 항목에 signal_id, carrier_hz, repeat, tick_count, ref_count, last_seen_at

## TC-08: 슬롯 설정 (웹 UI)

1. 웹 UI 접속 → API Key 입력 → Unlock
2. Slot 0: ONOFF, signal_a=100, signal_b=101 → Save
3. **기대**: 저장 성공, 새로고침 후 값 유지
4. **확인**: 재부팅 후에도 슬롯 설정 유지 (NVS)

## TC-09: 웹 API 키 인증

1. 웹 접속 → API Key 없이 접근
2. **기대**: 잠금 화면만 표시
3. 올바른 키 입력 → Unlock
4. **기대**: 대시보드 전체 표시
5. 잘못된 키 입력
6. **기대**: "Invalid key" 표시
7. 재부팅 후 같은 키로 접속
8. **기대**: NVS에 키 유지

## TC-10: FactoryReset (0x0D)

1. `send_cmd 0x0D`
2. **기대**: 디바이스 재부팅, 커미셔닝 초기화
3. **확인**: 재부팅 후 BLE 광고 시작, 새 API 키 생성

## TC-11: SNTP 시간 동기화

1. 커미셔닝 후 WiFi 연결 확인
2. 시리얼 로그에서 `SNTP: Time synced: 2026-xx-xx` 확인
3. SNTP 동기화 후 send_raw → dump_nvs
4. **기대**: last_seen_at이 현재 Unix timestamp (1970년 아님)

## TC-12: Apple Home 슬롯 동작

1. TC-08에서 슬롯 0을 ONOFF, signal_a=100(on), signal_b=101(off) 설정
2. signal_id 100, 101을 send_raw로 NVS에 저장
3. Apple Home에서 슬롯 0 On/Off 토글
4. **기대**: On → signal_id=100 IR 송신, Off → signal_id=101 IR 송신
5. **확인**: 시리얼에서 `TX signal_id=100/101` 로그

---

## 발견된 버그 이력

| 버그 | 원인 | 해결 | 커밋 |
|------|------|------|------|
| IR TX 간헐 실패 | rmt_set_tx_carrier 런타임 호출 | 제거, 초기화 시 config만 사용 | 39cb0e3 |
| send_raw ticks 깨짐 | chip-tool BYTES가 hex ASCII 그대로 전달 | hex 자동 디코딩 추가 | db5495a |
| LearnedPayload 안 갱신 | READY 시 속성 갱신 안 됨 | ir_mgmt_refresh_attributes 추가 | b17c1d9 |
| 웹 Unlock 무반응 | JS SyntaxError + tryAutoUnlock 위치 | 괄호 수정 + 스크립트 끝으로 이동 | b17c1d9 |
| ref_count 반영 안 됨 | 버퍼 저장 시 ref_count 미설정 | buffer 제거, 직접 NVS 저장 | 4d13124 |
| last_seen_at 1970년 | SNTP 미동기화 상태에서 time() | SNTP sync 콜백 추가 | 4d13124 |
