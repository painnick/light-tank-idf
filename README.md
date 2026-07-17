# panzer2-idf

ESP32-C6 Super Mini 기반 RC 탱크 펌웨어입니다.  
BLE 게임패드로 **캐터필러 주행**, **터렛 회전**, **포/기관총 효과**, **DFPlayer 효과음**을 제어합니다.

Arduino/PlatformIO 프로젝트 **M3Stuart_ESP32C3** 를 ESP-IDF v5.5.x 로 포팅한 뒤 **ESP32-C6 Super Mini** 로 이전한 버전이며, 입력은 [Bluepad32](https://github.com/ricardoquesada/bluepad32) + BTstack 를 사용합니다.

## 기능

| 기능 | 설명 |
|------|------|
| 트랙 구동 | DRV8833, 좌/우 스틱 Y축 — panzer4식 가속/감속 램프(~1초 최대) |
| 터렛 | SG90 서보 — 패드 연결 시 attach, D-Pad 좌우 회전, 3초 무입력/해제 시 detach |
| 포신 | B 버튼 — LED·효과음 후 트랙 후진 리코일 |
| 게틀링(기관총) | A 버튼 — 게틀링 LED 깜빡임 + 효과음 |
| 볼륨 | L1 / R1, NVS에 저장 |
| 사운드 | DFPlayer Mini (대기 / 포 / 기관총 / 연결음) |

## 하드웨어

- **MCU**: ESP32-C6 Super Mini (BLE — Classic BT 미사용)
- **모터 드라이버**: DRV8833
- **서보**: SG90 (터렛)
- **오디오**: DFPlayer Mini + microSD
- **입력**: BLE HID 게임패드 (검증: GamePadPlus V3 / ShanWan BM-769)

### 핀맵 (C6 Super Mini PCB 기본값)

핀은 **Kconfig** 로 관리합니다. 변경: `idf.py menuconfig` → **RC Tank Hardware Pins**.

PCB 라우팅: LED는 **좌측**, 좌측 트랙 모터·서보·DFPlayer는 **우측** 쪽 배치.

| 기능 (넷) | GPIO (기본) | 헤더 쪽 |
|-----------|-------------|---------|
| 우측 트랙 IN1 / IN2 (Motor-IN-A1/A2) | 0 / 1 | 좌 |
| 좌측 트랙 IN1 / IN2 (Motor-IN-B1/B2) | **19 / 20** | 우 |
| 포신 LED (CannonLED) | 6 | 좌 (LED 묶음) |
| 게틀링 LED (MG) | 7 | 좌 (LED 묶음) |
| 헤드라이트 (HeadLight) | 22 | 좌 (LED 묶음) |
| 터렛 서보 (TurretServo) | 14 | 우 |
| DFPlayer TX (→ DFPlayer RX) | 18 | 우 |
| DFPlayer RX | **미사용 (-1)** | — |

헤드라이트는 **Y 버튼**으로 On/Off 토글합니다 (rising edge — 길게 눌러도 깜빡이지 않음). 패드 해제 시 소등.  
게틀링 LED는 A 버튼 발사 시에만 깜빡입니다. DFPlayer는 **TX 전용** (피드백 배선 없음).

### 스트래핑 핀 & 주의 핀 (ESP32-C6 / Super Mini)

리셋·파워업 시 레벨이 샘플링되어 부팅 모드·JTAG 등이 결정됩니다. **앱 실행 후**에는 일반 GPIO로 쓸 수 있어도, 부팅 순간 레벨이 틀리면 다운로드 모드 진입·부팅 실패·모터 글리치가 날 수 있습니다.

#### 스트래핑 핀 (칩: GPIO4, 5, 8, 9, 15)

| GPIO | Super Mini에서 | 부팅 시 의미 (요약) | 권장 |
|------|----------------|---------------------|------|
| **4** (MTMS) | 좌측 헤더 | JTAG/strapping 관련 | 앱 I/O 비권장. 특히 **모터 금지** (부팅 글리치) |
| **5** (MTDI) | 좌측 헤더 | JTAG/strapping 관련 | 동일 |
| **8** | 우측, **온보드 WS2812** | 부팅 모드 조합 (GPIO9와) | **앱 신호로 쓰지 말 것** (LED·strap 충돌) |
| **9** | 우측, **BOOT 버튼** (내부 PU) | HIGH=정상 SPI 부트, LOW=다운로드 | **앱 출력 금지**. 버튼만 |
| **15** | 우측 (보드에 LED 연결 경우 있음) | JTAG 신호 소스 등 | 기본 맵에서 제외. 외장 pull로 부팅 방해 금지 |

부팅 모드(핵심): **GPIO9=1** 이면 플래시에서 정상 부트 (기본). GPIO9를 리셋 중 LOW로 묶으면 다운로드 모드.

#### 그 외 주의 핀

| GPIO | 이유 | 권장 |
|------|------|------|
| **12 / 13** | USB Serial/JTAG (D− / D+) | 디버그·플래시용. 일반 I/O로 쓰면 USB 콘솔 끊김 |
| **16 / 17** | 기본 UART0 TX/RX (보드 표기) | 콘솔 백업용으로 비워 두는 편 권장 |
| **24–30** | 내부 SPI flash (SiP) | Super Mini에서 보통 **미노출·사용 금지** |
| **10 / 11** | 패키지/모듈에 따라 미인출 | 보드에 없으면 사용 불가 |

#### 이 프로젝트 기본 맵과의 관계

| 상태 | GPIO |
|------|------|
| **사용 중 (안전 쪽)** | 0/1·19/20 모터, 6/7/22 LED, 14 서보, 18 DFPlayer TX |
| **의도적 미사용** | 2, 3, 4, 5, 8, 9, 12, 13, 15, 16, 17 + DFPlayer RX |
| **여유 (확장)** | 21, 23 등 (strap/USB 아님) |

**PCB 팁:** DRV8833 IN 입력은 약한 **pull-down** 권장 (부팅 전 high-Z 구간 모터 방지). 스트래핑 핀에 강한 외장 pull-up/down을 걸지 말 것.

### PCB

`pcb/` 의 스케매틱 이미지는 **C3 Super Mini 시절** 참고용입니다. 현재 펌웨어 핀맵 기준은 위 표와 `main/Kconfig.projbuild` 입니다. C6 Super Mini 는 핀 수·배치가 달라 **신규 PCB** 가 필요합니다.

| 파일 | 설명 |
|------|------|
| `pcb/Schematic_M3-Stuart_2026-07-11.png` | M3 Stuart 스케매틱 (C3 레거시 참고) |

### 3D 출력 (`3dp/`)

기구부 STEP 모델입니다. 슬라이서에서 열어 STL 등으로 변환해 출력하면 됩니다.

| 파일 | 설명 |
|------|------|
| `Idler.step` | 아이들러 |
| `Motor Guide 2mm.step` | 모터 가이드 2mm |
| `Motor Guide 3mm D.step` | 모터 가이드 3mm |
| `PCB 3.2.step` | PCB 브라켓 |
| `Sproket Guide.step` | 스프로킷 가이드 |
| `Turret.step` | 터렛 |

### DFPlayer 트랙 (SD 카드)

| 파일 | 용도 |
|------|------|
| `0001.mp3` | 대기 루프 |
| `0002.mp3` | 포신 발사 |
| `0003.mp3` | 기관총 |
| `0004.mp3` | 게임패드 연결 |

## 조작

| 입력 | 동작 |
|------|------|
| 좌 스틱 Y | 좌측 트랙 전/후진 (즉시 최대 아님, ~1초 램프 가속) |
| 우 스틱 Y | 우측 트랙 전/후진 (동일) |
| D-Pad ← / → | 터렛 좌/우 회전 (0°~180°, 약 120ms마다 1°) |
| Start | 터렛 서보 중앙(90°) |
| A | 게틀링(기관총) — LED 75ms 깜빡임, 약 0.5초 |
| B | 포신 — LED·효과음 후 리코일(후진) |
| Y | 헤드라이트 On/Off 토글 (길게 눌러도 1회만) |
| L1 / R1 | 볼륨 감소 / 증가 |

### 터렛 서보

- **부팅**: 서보 미연결 (PWM 없음)
- **게임패드 연결 시**: 서보 attach 후 **중앙(90°)** 설정
- **게임패드 해제 시**: 서보 detach
- **회전 속도**: `TURRET_STEP_INTERVAL_MS`(기본 120ms)마다 1° — 전체 0°↔180°에 약 22초
- **무입력 해제**: D-Pad 터렛 입력이 `TURRET_IDLE_DISCONNECT_MS`(기본 **3초**) 없으면 detach (버즈·홀딩 전류 감소)
- **재연결**: D-Pad 좌/우로 각도를 바꿀 때 다시 attach

### 포 발사 시퀀스 (B)

1. 포신 LED ON + 포 효과음 재생
2. `RECOIL_DELAY_MS`(기본 250ms) 대기
3. 트랙 후진 리코일 (`RECOIL_BACK_DURATION` 40ms) → 정지 안정화 (`RECOIL_SETTLE_DURATION` 40ms)

스틱 전진이 음수 축이므로 리코일 후진은 양수 모터 속도(`RECOIL_BACK_SPEED`)를 사용합니다. 리코일 중에는 스틱 모터 입력을 잠시 무시합니다.

## 게임패드 연결 (중요)

이 펌웨어는 **BLE HID** 경로를 사용합니다. DualShock 등 Classic BT 전용 패드는 연결되지 않습니다.

### GamePadPlus V3 / Terios T3 / ShanWan BM-769

1. 패드를 끈 뒤, **`Home + X`** 를 약 3초 눌러 페어링 모드로 진입 (표준 Android BLE HID)
2. 펌웨어 부팅 후 자동 스캔·연결
3. 성공 시 로그 예: `Gamepad ready (...), reports=2` → `게임패드 연결됨`

`Home + A` / `Home + Y` 모드는 BLE여도 HID 데이터 형식이 달라 동작하지 않을 수 있습니다.

### 연결 구현 메모

일부 저가 BLE 패드는 BTstack 기본 `hids_client` 경로에서 HID 설정이 멈추는 문제가 있습니다.  
이 프로젝트는 **Simple HOG** 경로를 사용합니다.

- GATT 서비스 탐색 → HID(0x1812) 특성 탐색
- Report Map 읽기
- CCC 직접 쓰기 + 알림 수신
- Report ID 프리펜드 후 Bluepad32 Android 파서로 입력 처리

부팅 시 본딩 키를 지우지 않습니다. 강제 재페어링이 필요하면 콘솔에서 키 삭제 명령을 사용하세요.

## 소프트웨어 구조

```
main/
  main.c              # 모터/서보/LED, 게임패드 처리, 제어 루프
  my_platform.c       # Bluepad32 커스텀 플랫폼, 공유 입력 상태
  dfplayer.c/.h       # DFPlayer Mini UART 제어
  Kconfig.projbuild   # 하드웨어 핀 (menuconfig)
components/
  bluepad32/          # Bluepad32 (커스텀 Simple HOG 포함)
  btstack/            # BTstack ESP32 포트
```

- **bt_main**: BTstack 런 루프 (우선순위 높음)
- **tank_ctrl**: 10ms 주기로 게임패드 입력 → 액추에이터 반영

## 빌드 & 플래시

### 요구 사항

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c6/get-started/index.html)
- 타깃: `esp32c6`

### 환경 설정 (Windows 예)

```bat
REM ESP-IDF export (경로는 설치에 맞게 수정)
call C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat

cd light-tank-idf
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

핀만 바꿀 때:

```bat
idf.py menuconfig
REM → RC Tank Hardware Pins
idf.py build
```

`setup.bat` / `setup.sh` 로 Bluepad32·BTstack 의존성을 준비할 수 있습니다 (이미 `components/` 가 포함되어 있으면 생략 가능).

### 주요 sdkconfig

- BLE 컨트롤러 only (`CONFIG_BT_CONTROLLER_ONLY`)
- Wi-Fi 비활성
- Bluepad32 커스텀 플랫폼
- 공통: `sdkconfig.defaults` / C6: `sdkconfig.defaults.esp32c6`
- 기본 로그 레벨: INFO (상세 BLE 단계는 DEBUG)

## 라이선스 관련

- 애플리케이션 코드: 프로젝트 정책에 따름
- **Bluepad32**: Apache-2.0
- **BTstack**: 비상업 평가 가능, 상업 사용 시 [BlueKitchen](https://bluekitchen-gmbh.com/) 라이선스 필요

## 참고

- [Bluepad32 문서](https://bluepad32.readthedocs.io/)
- [ESP-IDF ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c6/)
- 설계: `docs/superpowers/specs/2026-07-17-esp32c6-super-mini-migration-design.md`
- 원본: M3Stuart_ESP32C3 (Arduino/PlatformIO)
