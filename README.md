# light-tank-idf

ESP32-C6 Super Mini 기반 RC 탱크 펌웨어입니다.  
BLE 게임패드로 **캐터필러 주행**, **터렛 회전**, **포/기관총 효과**, **DFPlayer 효과음**을 제어합니다.

Arduino/PlatformIO 프로젝트 **M3Stuart_ESP32C3** 를 ESP-IDF v5.5.x 로 포팅한 뒤 **ESP32-C6 Super Mini** 로 이전한 버전이며, 입력은 [Bluepad32](https://github.com/painnick/bluepad32) + BTstack 를 사용합니다.

## 기능

| 기능 | 설명 |
|------|------|
| 트랙 구동 | DRV8833, 좌/우 스틱 Y축 — panzer4식 가속/감속 램프(~1초 최대) |
| 터렛 좌우 | SG90 — D-Pad ←/→ 연속 슬루, 3초 무입력 detach |
| 터렛 상하 | SG90(GPIO23) — 연결/Start 시 Kconfig 센터, D-Pad ↑/↓ 홀드 스텝, 3초 무입력 detach |
| 포신 | B 버튼 — 효과음 즉시, 약 0.5초 후 LED, 약 1초 후 트랙 후진 리코일 |
| 게틀링(기관총) | A 버튼 — 효과음 즉시, 0.5초 후 LED 깜빡임 (~0.5초) |
| 볼륨 | L1 / R1 (홀드 시 100ms마다 1단계, 즉시 반영), 릴리스 시 NVS 저장 |
| 사운드 | DFPlayer Mini (대기 / 포 / 기관총 / 연결음) |
| 안전 | 연결 해제 시 모터 드라이버 sleep / 리포트 1초 무수신 시 트랙 정지 (failsafe) |

## 하드웨어

- **MCU**: ESP32-C6 Super Mini (BLE — Classic BT 미사용)
- **모터 드라이버**: DRV8833
- **서보**: SG90 ×2 (좌우 yaw GPIO21, 상하 pitch GPIO23)
- **오디오**: DFPlayer Mini + microSD
- **입력**: BLE HID 게임패드 (검증: GamePadPlus V3 / ShanWan BM-769)

### 핀맵 (C6 Super Mini PCB 기본값)

핀은 **Kconfig** 로 관리합니다. 변경: `idf.py menuconfig` → **RC Tank Hardware Pins**.

보드를 **뒤집어서** 장착하는 PCB 레이아웃 기준 기본값입니다.

| 기능 (넷) | GPIO (기본) |
|-----------|-------------|
| 우측 트랙 IN1 / IN2 (Motor-IN-A1/A2) | **3 / 2** |
| 좌측 트랙 IN1 / IN2 (Motor-IN-B1/B2) | **1 / 0** |
| **DRV8833 nSLEEP** | **6** |
| 헤드라이트 (HeadLight) | **20** |
| 포신 LED (CannonLED) | **19** |
| 게틀링 LED (MG) | **18** |
| 터렛 좌우 서보 (yaw) | 21 |
| 터렛 상하 서보 (pitch) | **23** |
| DFPlayer TX (→ DFPlayer RX) | 22 |
| DFPlayer RX | **미사용 (-1)** |

**nSLEEP:** LOW=슬립(출력 Hi-Z), HIGH=동작. 부팅 직후 LOW → 모터 IN/PWM 준비 후 HIGH. 패드 해제 시 LOW, 재연결 시 HIGH.  
PCB: nSLEEP(GPIO6) **~10k pull-down** 권장. IN×4 pull-down은 생략 가능.

헤드라이트는 **Y 버튼** 토글. 게틀링은 A. DFPlayer는 **TX 전용**.

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
| **사용 중** | 모터 R **3/2** · L **1/0**, nSLEEP **6**, LED **20/19/18**, 서보 **21**, DF TX **22** |
| **의도적 미사용** | 4, 5, 7, 8, 9, 12, 13, 14, 15, 16, 17, 23 + DFPlayer RX |
| **여유 (확장)** | 7, 14, 23 등 (strap/USB 아님) |

**PCB 팁:**
- **nSLEEP (GPIO6) ~10k pull-down** — 부팅·리셋 시 드라이버 sleep.
- IN×4 pull-down은 선택. nSLEEP이 VCC 고정 모듈이면 IN pull-down 필수.
- 스트래핑 핀(4/5/8/9/15)에 강한 외장 pull 금지.
- 깨우기 순서: IN/PWM=0 → nSLEEP HIGH.

### PCB

`pcb/Schematic_Light-Tank_2026-07-20.png` 가 현재 ESP32-C6 Super Mini 기준 스케매틱입니다. 핀 배정은 위 표와 `main/Kconfig.projbuild` 가 기준이며, 회로도 넷 이름도 같은 이름을 사용합니다.

| 파일 | 설명 |
|------|------|
| `pcb/Schematic_Light-Tank_2026-07-20.png` | Light Tank C6 Super Mini 스케매틱 (2026-07-20) |

**주요 연결:**
- **DRV8833**: `MT1`/`MT2` 모터 커넥터, 입력 `Motor-IN-A1/A2/B1/B2`, `nSLEEP`=GPIO6
- **터렛 좌우**: `TURRET` — 신호 GPIO21, 5V, GND
- **터렛 상하**: pitch 서보 — 신호 **GPIO23**, 5V, GND
- **사운드**: DFPlayer Mini — ESP32 TX(GPIO22) → DFPlayer RX, `SPK` 스피커 커넥터
- **조명**: 헤드라이트 GPIO20 + Q1(IRLML6344), 포신/게틀링 LED GPIO19/18
- **확장**: `EXT3`(GPIO7) 등
- **전원**: 배터리 → BoostCharger(U3) → 전원 스위치(U4) → 5V/VCC/GND

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
| D-Pad ← / → | 터렛 **좌/우** 회전 (연속 슬루) |
| D-Pad ↑ / ↓ | 터렛 **상하** 홀드 스텝 (범위·스텝은 Kconfig) |
| Start | yaw/pitch 센터 각도로 리셋 (Kconfig) |
| A | 게틀링(기관총) — 효과음 즉시, **0.5초 후** LED 75ms 깜빡임 약 0.5초 |
| B | 포신 — **효과음 즉시**, 약 **0.5초 후** LED, 약 **1초 후** 리코일(후진) |
| Y | 헤드라이트 On/Off 토글 (길게 눌러도 1회만) |
| L1 / R1 | 볼륨 감소 / 증가 |

### 터렛 좌우 (yaw, GPIO21)

- **패드 연결 / Start**: Kconfig `TURRET_YAW_CENTER_DEG` (기본 **90°**)
- **범위**: `TURRET_YAW_MIN_DEG` ~ `TURRET_YAW_MAX_DEG` (기본 **0°~180°**)
- **D-Pad ←/→**: 0.1° 단위 연속 슬루 (`TURRET_YAW_HOLD_STEP_X10`, 기본 0.2°/10ms)
- **3초 무입력**(슬루 완료 후): detach

### 터렛 상하 (pitch, GPIO23)

- **패드 연결 / Start**: Kconfig `TURRET_PITCH_CENTER_DEG` (Light Tank 기본 **45°**)
- **범위**: `TURRET_PITCH_MIN_DEG` ~ `TURRET_PITCH_MAX_DEG` (Light Tank 기본 **15°~75°**)
- **D-Pad ↑/↓ 홀드**: `TURRET_PITCH_STEP_DEG`°씩, `TURRET_PITCH_HOLD_INTERVAL_MS` ms 간격 (기본 1° / 50ms)
- **슬루**: 목표까지 부드럽게 이동
- **3초 무입력**(슬루 완료 후): detach

각도는 `idf.py menuconfig` → **RC Tank Turret Servos** 에서 변경합니다.

### 포 발사 시퀀스 (B)

1. 포 효과음 재생 명령 (즉시) — DFPlayer 실제 재생은 약 0.5초 후
2. `CANNON_FOLLOWUP_DELAY_MS`(500ms) 후 포신 LED ON (`CANNON_LED_DURATION` 200ms)
3. LED 시작 `CANNON_RECOIL_DELAY_MS`(500ms) 후 트랙 후진 리코일 (`RECOIL_BACK_DURATION` 40ms) → 정지 안정화 (`RECOIL_SETTLE_DURATION` 40ms)

스틱 전진이 음수 축이므로 리코일 후진은 양수 모터 속도(`RECOIL_BACK_SPEED`)를 사용합니다. 리코일 중에는 스틱 모터 입력을 잠시 무시합니다.

## 게임패드 연결 (중요)

이 펌웨어는 **BLE HID** 경로를 사용합니다. DualShock 등 Classic BT 전용 패드는 연결되지 않습니다.

**한 번에 1대의 패드만** 연결됩니다. 패드가 준비되면 스캔이 멈추고, 연결이 끊기면 다시 스캔합니다 (Simple HOG 상태 머신은 단일 인스턴스 — 2대째 연결 시 첫 패드 입력이 끊기는 문제 방지).

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
  Kconfig.projbuild   # 하드웨어 핀 + 터렛 서보 각도 (menuconfig)
components/
  bluepad32/          # git submodule: https://github.com/painnick/bluepad32
                      # ESP-IDF 컴포넌트는 src/components/bluepad32
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

또는 프로젝트 루트의 배치 파일 (`env.bat`로 ESP-IDF 환경 로드):

| 스크립트 | 설명 |
|----------|------|
| `build.bat` | Light Tank 프로필 빌드 |
| `build-academy.bat` | Academy 2호 프로필 빌드 |
| `flash.bat COMx` | 플래시 + 모니터 (`FLASH_PORT` 환경 변수도 가능) |
| `build-flash.bat COMx` | Light Tank 빌드 후 플래시 |
| `build-flash-academy.bat COMx` | Academy 2호 빌드 후 플래시 |

프로필을 바꾸면 `.build-profile`과 비교해 `sdkconfig`를 자동 재생성합니다.

핀만 바꿀 때:

```bat
idf.py menuconfig
REM → RC Tank Hardware Pins / RC Tank Turret Servos
idf.py build
```

### 차종별 sdkconfig (프로필)

| 프로필 | 파일 | pitch (센터 / 범위 / 스텝) |
|--------|------|---------------------------|
| Light Tank (기본) | `sdkconfig.defaults.esp32c6` | 45° / 15°~75° / 1° |
| Academy 2호 | `sdkconfig.academy` | 80° / 60°~110° / 5° |

Academy 2호 빌드 예:

```bat
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.academy" set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

`SDKCONFIG_DEFAULTS`는 **앞에서 뒤로** 덮어씁니다. 핀맵은 `sdkconfig.defaults.esp32c6`, 터렛 각도만 `sdkconfig.academy`가 오버라이드합니다.

클론 시 Bluepad32 서브모듈을 함께 가져와야 합니다.

```bat
git clone --recurse-submodules https://github.com/painnick/light-tank-idf.git
REM 이미 클론한 경우:
git submodule update --init --recursive
```

### 주요 sdkconfig

- BLE 컨트롤러 only (`CONFIG_BT_CONTROLLER_ONLY`)
- Wi-Fi 비활성
- Bluepad32 커스텀 플랫폼
- 공통: `sdkconfig.defaults` / C6: `sdkconfig.defaults.esp32c6`
- 차종: `sdkconfig.academy` (Academy 2호 터렛 각도)
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
