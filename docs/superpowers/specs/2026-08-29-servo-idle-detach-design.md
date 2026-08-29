# Design: Servo idle detach (yaw + pitch)

**Date:** 2026-08-29  
**Status:** Implemented  
**Project:** light-tank-idf

## Summary

게임패드가 연결된 동안 yaw/pitch 서보 PWM을 계속 붙이지 않는다.  
**detach 여부를 플래그로 파악**하고, **움직여야 할 때만 attach**한 뒤, **해당 축에 대한 무입력이 약 2초 지속되고 슬루가 끝나면 detach**한다.

## Goals

1. 좌우(yaw)와 상하(pitch) 모두 idle detach.
2. 움직임이 필요할 때만 LEDC PWM을 핀에 붙인다.
3. detach 시 GPIO를 LOW로 몰지 않는다 (SG90이 한쪽으로 달리는 문제 재발 방지).
4. 재attach 시 실제 PWM이 핀에 다시 붙고, 마지막 각도에서 이어진다.

## Non-goals

- 서보 전원(5V) 차단. PWM 신호만 끊는다.
- menuconfig에 idle 시간 항목 추가. 상수 `2000` ms.
- 트랙 모터·LED·DFPlayer 동작 변경.
- 패드 연결 중 센터 각도로 주기적 재정렬.

## Current behavior

- `g_turret_attached` / `g_pitch_attached`로 이미 상태를 안다.
- D-Pad 홀드·`set_*_target_deg()`는 이미 필요 시 attach한다.
- 게임패드 연결 / Start는 센터로 붙인 뒤 **연결이 끊길 때까지 PWM을 유지**한다.
- detach는 **패드 연결 해제 때만** 호출한다.
- 현재 `turret_detach()` / `pitch_detach()`는 `ledc_stop(..., idle_level=0)` 후 `gpio_reset_pin()`이라, 핀이 잠깐 LOW로 나간다. 이것이 이전 idle detach 실패의 핵심이다.

## Behavior

### Attach (움직여야 할 때)

이미 붙어 있으면 no-op. 떨어져 있으면 `servo_ledc_bind()` 후 현재 각도 PWM.

Attach 하는 입력:

| 이벤트 | yaw | pitch |
|--------|-----|-------|
| 게임패드 신규 연결 (센터) | attach | attach |
| Start (센터) | attach | attach |
| D-Pad ← / → 홀드 | attach | — |
| D-Pad ↑ / ↓ 홀드 | — | attach |

재attach 후에도 소프트웨어 각도는 그대로다. 센터로 돌아가지 않는다 (연결/Start만 센터).

### Idle 타이머 (축별)

각 축은 자기 **마지막 명령 시각**만 본다. 스틱·버튼·다른 축 입력은 타이머를 리셋하지 않는다.

명령을 본 때 타이머를 지금으로 갱신:

- 그 축 D-Pad 홀드가 참인 제어 루프
- 그 축을 건드리는 연결 센터 / Start 센터

Detach 조건 (둘 다 만족, 축별):

1. 그 축이 attach 되어 있다.
2. 그 축 D-Pad 홀드가 없다.
3. `current == target` (슬루 완료).
4. `now - last_command_ms >= 2000`.

슬루 중에는 detach하지 않는다. 홀드를 떼고 슬루가 남은 경우, 2초는 **마지막 홀드 루프 시각**부터 친다.

### Detach (안전)

순서 (핀에 LOW가 나가지 않게):

1. `gpio_reset_pin()`으로 LEDC와 GPIO 매트릭스를 먼저 끊는다. 핀은 플로팅(입력).
2. 그 다음 `ledc_stop()`으로 채널만 정리한다. 이때 `idle_level`은 이미 핀에 연결되지 않는다.
3. `g_*_attached = false`.
4. 홀드 플래그는 idle detach 조건상 이미 false. 추가로 건드리지 않아도 된다. **패드 연결 해제 때**의 강제 detach는 지금처럼 홀드를 클리어해도 된다.

`gpio_set_direction(OUTPUT)` + `gpio_set_level(0)` 는 쓰지 않는다.

패드 연결 해제는 지금처럼 **즉시** 양쪽 detach. 2초를 기다리지 않는다.

### 연결 / Start

지금처럼 센터 PWM을 한 번 준다. 그 시각부터 2초 무명령이면 그 축을 detach한다.  
첫 D-Pad까지 attach를 미루지 않는다.

## Tradeoffs (수락)

- Pitch는 PWM이 없으면 중력으로 처질 수 있다. 의도된 동작이다.
- Yaw도 외부 힘이 있으면 기어 백래시로 조금 밀릴 수 있다.
- 2초 안에 D-Pad를 다시 누르면 attach/detach가 반복되지 않는다.

## Files

- `main/main.c` — idle 타이머, 안전 detach 순서, 연결 중 idle detach 호출.
- `README.md` — “패드 연결 중 PWM 유지”를 위 동작으로 교체.

Kconfig·핀맵 변경 없음.

## Test plan (실기)

1. 패드 연결 → 센터로 움직임 → 약 2초 후 로그 `서보 detach`, 버징 감소.
2. D-Pad ←/→ 홀드 → 즉시 회전. 떼고 슬루 끝난 뒤 ~2초 후 detach. 다시 누르면 **마지막 각도에서** 이어짐.
3. D-Pad ↑/↓ 동일.
4. Start → 센터 PWM → ~2초 후 detach.
5. 스틱만 조작 → 터렛 idle 타이머가 리셋되지 않고 2초 후 detach 유지.
6. 패드 해제 → 즉시 양쪽 detach, 모터 sleep.
7. detach 중 서보가 한쪽으로 달려가지 않음.
8. L1/R1 볼륨(NVS) 때 터렛이 혼자 움직이지 않음.
