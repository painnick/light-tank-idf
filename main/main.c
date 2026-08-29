// RC 탱크 - ESP-IDF v5.5.x / ESP32-C6 Super Mini
// Original: M3Stuart_ESP32C3 (Arduino/PlatformIO)
//
// Bluepad32 게임패드로 DRV8833 모터 트랙 + SG90 서보 터렛 + DFPlayer 효과음 제어
// 핀 번호: idf.py menuconfig → "RC Tank Hardware Pins"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/sdm.h"

#include "btstack_port_esp32.h"
#include "dfplayer.h"
#include "btstack_run_loop.h"
#include "uni.h"

// my_platform.c
struct uni_platform* get_my_platform(void);
void gamepad_state_init(void);

static const char* TAG = "RC_TANK";

// ============================================================================
// 핀 정의 (Kconfig — ESP32-C6 Super Mini PCB 기본값)
// ============================================================================
#define PIN_LEFT_IN1        CONFIG_PIN_LEFT_IN1
#define PIN_LEFT_IN2        CONFIG_PIN_LEFT_IN2
#define PIN_RIGHT_IN1       CONFIG_PIN_RIGHT_IN1
#define PIN_RIGHT_IN2       CONFIG_PIN_RIGHT_IN2
#define PIN_MOTOR_NSLEEP    CONFIG_PIN_MOTOR_NSLEEP  // DRV8833 nSLEEP: 0=sleep, 1=run
#define PIN_CANNON_LED      CONFIG_PIN_CANNON_LED
#define PIN_MG_LED          CONFIG_PIN_MG_LED
#define PIN_HEADLIGHT       CONFIG_PIN_HEADLIGHT
#define PIN_TURRET_SERVO    CONFIG_PIN_TURRET_SERVO   // 좌우(yaw)
#define PIN_TURRET_PITCH    CONFIG_PIN_TURRET_PITCH   // 상하(pitch), 기본 GPIO23
#define PIN_DFPLAYER_TX     CONFIG_PIN_DFPLAYER_TX
#define PIN_DFPLAYER_RX     CONFIG_PIN_DFPLAYER_RX

// ============================================================================
// 핀 설정 빌드 타임 검증 (menuconfig 실수를 컴파일 시점에 차단)
// ============================================================================
// 금지: 8/9 (strap·BOOT·온보드 RGB), 12/13 (USB Serial/JTAG), 15 (strap), 24-30 (낸드 플래시)
#define PIN_FORBIDDEN(p) \
    ((p) == 8 || (p) == 9 || (p) == 12 || (p) == 13 || (p) == 15 || ((p) >= 24 && (p) <= 30))

#define ASSERT_PIN_USABLE(pin) \
    _Static_assert(!PIN_FORBIDDEN(pin), "RC Tank pin on boot/USB/flash GPIO — see README strapping table")

ASSERT_PIN_USABLE(PIN_LEFT_IN1);
ASSERT_PIN_USABLE(PIN_LEFT_IN2);
ASSERT_PIN_USABLE(PIN_RIGHT_IN1);
ASSERT_PIN_USABLE(PIN_RIGHT_IN2);
ASSERT_PIN_USABLE(PIN_MOTOR_NSLEEP);
ASSERT_PIN_USABLE(PIN_CANNON_LED);
ASSERT_PIN_USABLE(PIN_MG_LED);
ASSERT_PIN_USABLE(PIN_HEADLIGHT);
ASSERT_PIN_USABLE(PIN_TURRET_SERVO);
ASSERT_PIN_USABLE(PIN_TURRET_PITCH);
ASSERT_PIN_USABLE(PIN_DFPLAYER_TX);
_Static_assert(PIN_DFPLAYER_RX < 0 || !PIN_FORBIDDEN(PIN_DFPLAYER_RX),
               "DFPlayer RX on boot/USB/flash GPIO");

// 모터 핀은 4/5 (JTAG/strapping) 도 금지 — 부팅 샘플링 중 모터 글리치
_Static_assert(PIN_LEFT_IN1 != 4 && PIN_LEFT_IN1 != 5, "Left IN1 on strapping GPIO4/5");
_Static_assert(PIN_LEFT_IN2 != 4 && PIN_LEFT_IN2 != 5, "Left IN2 on strapping GPIO4/5");
_Static_assert(PIN_RIGHT_IN1 != 4 && PIN_RIGHT_IN1 != 5, "Right IN1 on strapping GPIO4/5");
_Static_assert(PIN_RIGHT_IN2 != 4 && PIN_RIGHT_IN2 != 5, "Right IN2 on strapping GPIO4/5");

// 중복 할당 검출: 사용 핀 11개의 비트마스크 popcount가 11이어야 함
#define PIN_USAGE_MASK                                                                      \
    ((1u << PIN_LEFT_IN1) | (1u << PIN_LEFT_IN2) | (1u << PIN_RIGHT_IN1) |                  \
     (1u << PIN_RIGHT_IN2) | (1u << PIN_MOTOR_NSLEEP) | (1u << PIN_CANNON_LED) |            \
     (1u << PIN_MG_LED) | (1u << PIN_HEADLIGHT) | (1u << PIN_TURRET_SERVO) |                \
     (1u << PIN_TURRET_PITCH) | (1u << PIN_DFPLAYER_TX))
_Static_assert(__builtin_popcount(PIN_USAGE_MASK) == 11,
               "Duplicate GPIO assignment in RC Tank Hardware Pins");

// ============================================================================
// LEDC 채널/타이머 할당
// ============================================================================
#define LEDC_MOTOR_FREQ     5000
#define LEDC_MOTOR_RES      LEDC_TIMER_8_BIT
#define LEDC_SERVO_FREQ     50
#define LEDC_SERVO_RES      LEDC_TIMER_14_BIT

#define LEDC_CH_LEFT_IN1    LEDC_CHANNEL_0
#define LEDC_CH_LEFT_IN2    LEDC_CHANNEL_1
#define LEDC_CH_RIGHT_IN1   LEDC_CHANNEL_2
#define LEDC_CH_RIGHT_IN2   LEDC_CHANNEL_3
#define LEDC_CH_SERVO       LEDC_CHANNEL_4   // yaw 좌우
#define LEDC_CH_PITCH       LEDC_CHANNEL_5   // pitch 상하

#define LEDC_TIMER_MOTOR    LEDC_TIMER_0
#define LEDC_TIMER_SERVO    LEDC_TIMER_1     // yaw/pitch 공통 50Hz

// LEDC 채널 6개(모터 4 + 서보 2)가 가득 차서 헤드라이트는 SDM으로 평균 전압을 낮춘다.
#define HEADLIGHT_DUTY_PERCENT 10
#define HEADLIGHT_SDM_DENSITY_ON \
    ((int8_t)((HEADLIGHT_DUTY_PERCENT * 256) / 100 - 128))  // 10% → density -103
#define HEADLIGHT_SDM_DENSITY_OFF ((int8_t)-128)            // 0%

// ============================================================================
// NVS 키
// ============================================================================
#define NVS_NAMESPACE       "rc_tank"
#define NVS_KEY_VOLUME      "volume"

// ============================================================================
// 타이밍 상수 (ms)
// ============================================================================
#define LOOP_INTERVAL_MS        10
// DFPlayer Mini는 전원·SD 초기화가 끝나기 전 UART를 붙이면 부팅이 깨질 수 있다.
// BLE는 바로 시작하고, UART 연결과 첫 재생은 그 이후에만 한다.
#define DFPLAYER_BOOT_SETTLE_MS       3000
#define IDLE_SOUND_RETRY_INTERVAL_MS  4000
#define IDLE_SOUND_MAX_RETRIES        1
#define VOLUME_CHANGE_INTERVAL  100
#define CANNON_LED_DURATION       200
#define CANNON_FOLLOWUP_DELAY_MS  500   // 효과음 명령 후 LED 시작
#define CANNON_RECOIL_DELAY_MS    500   // LED 시작 후 트랙 후진 리코일
#define MACHINE_GUN_DURATION    500   // panzer4 MG_FIRE_MS — LED 깜빡임 지속
#define MG_LED_DELAY_MS         500   // DFPlayer 효과음 지연에 맞춰 LED 시작
#define MG_LED_BLINK_MS         75    // panzer4 게틀링 LED 깜빡임 주기
// 터렛: 0.1° 단위(x10)로 매 루프 목표·출력을 같이 움직여 "멈춤-재개" 끊김 제거
// 10ms마다 1 = 0.1° → 약 10°/s (이전 100ms/1°와 동일 속도, 연속 보간)
#define TURRET_DEG_SCALE              10  // 내부 단위 = 도 * 10
#define TURRET_SLEW_X10_PER_LOOP       2  // 출력은 목표보다 약간 빠르게 따라감 (따라잡기)
#define TURRET_CENTER_DEG             CONFIG_TURRET_YAW_CENTER_DEG
#define TURRET_MIN_DEG                CONFIG_TURRET_YAW_MIN_DEG
#define TURRET_MAX_DEG                CONFIG_TURRET_YAW_MAX_DEG
#define TURRET_CENTER_X10    (TURRET_CENTER_DEG * TURRET_DEG_SCALE)
#define TURRET_MIN_X10       (TURRET_MIN_DEG * TURRET_DEG_SCALE)
#define TURRET_MAX_X10       (TURRET_MAX_DEG * TURRET_DEG_SCALE)
#define TURRET_HOLD_X10_PER_LOOP      CONFIG_TURRET_YAW_HOLD_STEP_X10
#define TURRET_PWM_FULL_SCALE_X10     (180 * TURRET_DEG_SCALE)
#define TURRET_IDLE_DETACH_MS         2000  // 축별 무입력 + 슬루 완료 후 PWM detach

// 포탑 상하(pitch) — Kconfig: RC Tank Turret Servos
#define PITCH_STEP_DEG                 CONFIG_TURRET_PITCH_STEP_DEG
#define PITCH_HOLD_INTERVAL_MS         CONFIG_TURRET_PITCH_HOLD_INTERVAL_MS
#define PITCH_CENTER_DEG               CONFIG_TURRET_PITCH_CENTER_DEG
#define PITCH_MIN_DEG                  CONFIG_TURRET_PITCH_MIN_DEG
#define PITCH_MAX_DEG                  CONFIG_TURRET_PITCH_MAX_DEG
#define PITCH_SLEW_X10_PER_LOOP       10  // 10ms당 1° — 1° 스텝을 빠르게 따라감
#define PITCH_MIN_X10   (PITCH_MIN_DEG * TURRET_DEG_SCALE)
#define PITCH_MAX_X10   (PITCH_MAX_DEG * TURRET_DEG_SCALE)

_Static_assert(CONFIG_TURRET_YAW_MIN_DEG <= CONFIG_TURRET_YAW_CENTER_DEG
               && CONFIG_TURRET_YAW_CENTER_DEG <= CONFIG_TURRET_YAW_MAX_DEG,
               "Yaw center must be between min and max");
_Static_assert(CONFIG_TURRET_PITCH_MIN_DEG <= CONFIG_TURRET_PITCH_CENTER_DEG
               && CONFIG_TURRET_PITCH_CENTER_DEG <= CONFIG_TURRET_PITCH_MAX_DEG,
               "Pitch center must be between min and max");
_Static_assert(CONFIG_TURRET_PITCH_STEP_DEG != 0,
               "Pitch step must be non-zero (negative inverts D-Pad)");
#define GAMEPAD_CONNECT_GRACE_MS  500 // 연결 직후 입력 무시 (노이즈/잔여 D-Pad 방지)
#define DPAD_QUIET_AFTER_BUTTON_MS 300 // L1/R1·페이스 버튼 직후 hat 노이즈 무시
#define DPAD_HOLD_BLOCK_MASK \
    (BUTTON_SHOULDER_L | BUTTON_SHOULDER_R | BUTTON_A | BUTTON_B | BUTTON_X | BUTTON_Y)
#define GAMEPAD_STICK_DEADZONE    50
#define GAMEPAD_INPUT_TIMEOUT_MS  1000 // 스틱이 살아 있는데 리포트가 끊기면 트랙 정지 (BLE는 무입력 때 리포트를 안 보냄)
#define RECOIL_BACK_DURATION    40    // 포 발사 시 후진 시간 (ms)
#define RECOIL_SETTLE_DURATION  40    // 후진 후 정지 안정화 (ms)

// ============================================================================
// 모터 설정 (panzer4: 램프 가속/감속)
// ============================================================================
// 기동 최소 속도 (0~512). 너무 낮으면 고주파 윙 소리만 나고 바퀴가 안 돔.
// duty ≈ START * 255 / 512 → 400 ≈ 78%
#define MOTOR_START_SPEED   400
#define MOTOR_MAX_SPEED     512
#define LEDC_MOTOR_DUTY_MAX 255   // 8-bit LEDC
// 내부 속도 = 실제 속도 * TRACK_SPEED_SCALE (10ms마다 step)
// 기동(START)→max 약 1초, max→정지 약 0.4초
#define TRACK_SPEED_SCALE   10
#define TRACK_ACCEL_STEP    12    // 10ms당 가속 (×10 스케일)
#define TRACK_DECEL_STEP    100   // 10ms당 감속 (×10 스케일)
// 스틱 전진 = axis_y 음수 → 후진 반동은 양수 최대 속도
#define RECOIL_BACK_SPEED   MOTOR_MAX_SPEED

// ============================================================================
// 외부 함수 선언 (my_platform.c)
// ============================================================================
extern bool gamepad_is_connected(void);
extern bool gamepad_read_new_connection(void);
extern int64_t gamepad_last_report_ms(void);
extern uint32_t gamepad_report_seq(void);
extern bool gamepad_read(int32_t* axis_y, int32_t* axis_ry, uint16_t* buttons,
                         uint8_t* dpad, uint8_t* misc_buttons);

// ============================================================================
// 전역 상태
// ============================================================================

// NVS
static nvs_handle_t g_nvs_handle;

// DFPlayer
static int g_current_volume = 20;

// 트랙 모터 (목표 속도 vs 램프 현재 속도)
static int g_target_left_speed = 0;
static int g_target_right_speed = 0;
static int g_current_left_x10 = 0;
static int g_current_right_x10 = 0;

// 터렛 좌우(yaw) (x10 = 0.1°). hold 플래그는 process_turret_slew 에서 목표 연속 갱신
static int g_turret_target_x10 = TURRET_CENTER_X10;
static int g_turret_current_x10 = TURRET_CENTER_X10;
static bool g_turret_attached = false;
static bool g_turret_hold_left = false;
static bool g_turret_hold_right = false;
static int64_t g_turret_last_command_ms = 0;

// GamePadPlus V3: L1/R1 릴리스 리포트에 hat 노이즈가 섞임. 새 BLE 리포트만 반영.
static uint32_t s_last_dpad_report_seq = 0;
static uint16_t s_prev_block_buttons = 0;
static int64_t s_dpad_quiet_until_ms = 0;

// 포탑 상하(pitch) — D-Pad 홀드 시 1° 연속
static int g_pitch_target_x10 = PITCH_CENTER_DEG * TURRET_DEG_SCALE;
static int g_pitch_current_x10 = PITCH_CENTER_DEG * TURRET_DEG_SCALE;
static int64_t g_pitch_last_input_ms = 0;
static int64_t g_pitch_last_step_ms = 0;
static bool g_pitch_attached = false;
static bool g_pitch_hold_up = false;
static bool g_pitch_hold_down = false;

// 게임패드 연결 직후 입력 무시 시각
static int64_t g_input_ignore_until_ms = 0;

// 포신 발사 — pending: 효과음 후 대기, firing: LED ON 구간
static bool g_cannon_pending = false;
static int64_t g_cannon_followup_at_ms = 0;
static bool g_cannon_firing = false;
static int64_t g_cannon_end_ms = 0;

// 리코일 (pending: LED 후 대기 → active: 후진/안정화)
static bool g_recoil_pending = false;
static int64_t g_recoil_at_ms = 0;
static bool g_recoil_active = false;
static int64_t g_recoil_start_time = 0;

// 기관총(게틀링) — 효과음 즉시, LED는 MG_LED_DELAY_MS 후 깜빡임
static bool g_machinegun_firing = false;
static int64_t g_machinegun_led_start_ms = 0;
static int64_t g_machinegun_end_ms = 0;
static bool g_mg_led_on = false;
static int64_t g_mg_led_last_toggle = 0;

// 효과음
static bool g_dfplayer_ok = false;
static bool g_dfplayer_failed = false;
static int64_t g_dfplayer_ready_at_ms = 0;
static bool g_dfplayer_volume_sent = false;
static bool g_idle_sound_started = false;
static bool g_pending_connect_sound = false;
static int64_t g_last_idle_sound_time = 0;
static int g_idle_sound_retries = 0;

// 볼륨 조절
static bool g_l1_pressed = false;
static bool g_r1_pressed = false;
static int64_t g_l1_last_change = 0;
static int64_t g_r1_last_change = 0;
static bool g_volume_dirty = false;

// Start 버튼 (터렛 중앙) 엣지 감지
static bool g_start_pressed = false;

// Y 버튼: 헤드라이트 토글 (길게 눌러도 1회만 — rising edge)
static bool g_y_pressed = false;
static bool g_headlight_on = false;
static sdm_channel_handle_t s_headlight_sdm;

// 현재 시간 (us)
static inline int64_t now_us(void) {
    return esp_timer_get_time();
}

static inline int64_t now_ms(void) {
    return now_us() / 1000;
}

// ============================================================================
// NVS 볼륨 관리
// ============================================================================
static void load_volume_from_nvs(void) {
    int32_t stored = -1;
    esp_err_t err = nvs_get_i32(g_nvs_handle, NVS_KEY_VOLUME, &stored);
    if (err != ESP_OK || stored < 11 || stored > 30) {
        stored = 20;
        ESP_LOGI(TAG, "NVS 볼륨 없음, 기본값 사용: %d", stored);
    } else {
        ESP_LOGI(TAG, "NVS 볼륨 로드: %d", stored);
    }
    g_current_volume = (int)stored;
}

static void save_volume_to_nvs(int vol) {
    int32_t clamped = vol;
    if (clamped < 11) clamped = 11;
    if (clamped > 30) clamped = 30;
    nvs_set_i32(g_nvs_handle, NVS_KEY_VOLUME, clamped);
    nvs_commit(g_nvs_handle);
    ESP_LOGI(TAG, "NVS 볼륨 저장: %d", clamped);
}

// ============================================================================
// DRV8833 nSLEEP (LOW=sleep / outputs Hi-Z, HIGH=enable)
// PCB: ~10k pull-down on nSLEEP so boot keeps driver off without IN pull-downs.
// ============================================================================
static void motor_driver_set_enabled(bool enable) {
    gpio_set_level(PIN_MOTOR_NSLEEP, enable ? 1 : 0);
    ESP_LOGI(TAG, "DRV8833 nSLEEP=%s", enable ? "HIGH(run)" : "LOW(sleep)");
}

// ============================================================================
// LEDC 초기화
// ============================================================================

// 채널별 GPIO 매핑 테이블
typedef struct {
    gpio_num_t gpio;
    ledc_channel_t channel;
} motor_pin_map_t;

static const motor_pin_map_t g_motor_pins[] = {
    { .gpio = PIN_LEFT_IN1,  .channel = LEDC_CH_LEFT_IN1  },
    { .gpio = PIN_LEFT_IN2,  .channel = LEDC_CH_LEFT_IN2  },
    { .gpio = PIN_RIGHT_IN1, .channel = LEDC_CH_RIGHT_IN1 },
    { .gpio = PIN_RIGHT_IN2, .channel = LEDC_CH_RIGHT_IN2 },
};

static void init_ledc(void) {
    // 모터 타이머: 5kHz, 8-bit
    ledc_timer_config_t motor_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_MOTOR_RES,
        .timer_num = LEDC_TIMER_MOTOR,
        .freq_hz = LEDC_MOTOR_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&motor_timer);

    // 모터 채널 초기화
    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t ch = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = g_motor_pins[i].channel,
            .timer_sel = LEDC_TIMER_MOTOR,
            .gpio_num = g_motor_pins[i].gpio,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&ch);
    }

    // 서보 타이머: 50Hz, 14-bit (채널은 입력 시 attach)
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_SERVO_RES,
        .timer_num = LEDC_TIMER_SERVO,
        .freq_hz = LEDC_SERVO_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&servo_timer);
}

static void set_headlight(bool on) {
    int8_t density = on ? HEADLIGHT_SDM_DENSITY_ON : HEADLIGHT_SDM_DENSITY_OFF;
    sdm_channel_set_pulse_density(s_headlight_sdm, density);
}

static void init_headlight_pwm(void) {
    sdm_config_t config = {
        .gpio_num = PIN_HEADLIGHT,
        .clk_src = SDM_CLK_SRC_DEFAULT,
        .sample_rate_hz = 1000000,
    };
    ESP_ERROR_CHECK(sdm_new_channel(&config, &s_headlight_sdm));
    // enable 전에 0% — 기본 density 0은 약 50%라서 켜지기 전에 깜빡인다
    ESP_ERROR_CHECK(sdm_channel_set_pulse_density(s_headlight_sdm, HEADLIGHT_SDM_DENSITY_OFF));
    ESP_ERROR_CHECK(sdm_channel_enable(s_headlight_sdm));
}

// ============================================================================
// 모터 제어 (panzer4식 가속/감속 램프)
// ============================================================================
static void apply_motor_pwm(ledc_channel_t ch_in1, ledc_channel_t ch_in2, int speed) {
    int abs_sp = abs(speed);
    if (abs_sp == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_in1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_in1);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_in2, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_in2);
        return;
    }

    if (abs_sp > MOTOR_MAX_SPEED) abs_sp = MOTOR_MAX_SPEED;
    // 저속 구간은 기동 토크 확보 (윙 소리만 나는 구간 회피)
    if (abs_sp < MOTOR_START_SPEED) abs_sp = MOTOR_START_SPEED;

    uint32_t duty = (uint32_t)abs_sp * LEDC_MOTOR_DUTY_MAX / MOTOR_MAX_SPEED;
    if (duty > LEDC_MOTOR_DUTY_MAX) duty = LEDC_MOTOR_DUTY_MAX;

    if (speed > 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_in1, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_in1);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_in2, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_in2);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_in1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_in1);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_in2, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_in2);
    }
}

// panzer4: 0 기동 시 START 점프, 가속/감속 step, 정지 시 START 이하 → 0
static void update_track_speed_x10(int* current_x10, int target) {
    int cur = *current_x10;
    int target_x10 = target * TRACK_SPEED_SCALE;
    int min_x10 = MOTOR_START_SPEED * TRACK_SPEED_SCALE;

    if (cur == target_x10) {
        return;
    }

    if (cur == 0 && target_x10 != 0) {
        int sign = (target_x10 > 0) ? 1 : -1;
        int start = sign * min_x10;
        int target_abs = (target_x10 > 0) ? target_x10 : -target_x10;
        if (target_abs <= min_x10) {
            *current_x10 = target_x10;
            return;
        }
        cur = start;
        *current_x10 = cur;
    }

    int diff = target_x10 - cur;
    bool is_decelerating = false;
    if (cur > 0) {
        if (target_x10 < cur) is_decelerating = true;
    } else if (cur < 0) {
        if (target_x10 > cur) is_decelerating = true;
    }

    int step = is_decelerating ? TRACK_DECEL_STEP : TRACK_ACCEL_STEP;
    if (diff > 0) {
        *current_x10 = (diff > step) ? (cur + step) : target_x10;
    } else {
        *current_x10 = (-diff > step) ? (cur - step) : target_x10;
    }

    if (target_x10 == 0) {
        int cur_abs = (*current_x10 > 0) ? *current_x10 : -*current_x10;
        if (cur_abs < min_x10) {
            *current_x10 = 0;
        }
    }
}

// 스틱 값 → 목표 속도. 0이 아니면 최소 MOTOR_START_SPEED 보장.
static int clamp_track_target(int v) {
    if (v > MOTOR_MAX_SPEED) v = MOTOR_MAX_SPEED;
    if (v < -MOTOR_MAX_SPEED) v = -MOTOR_MAX_SPEED;
    if (v != 0 && abs(v) < MOTOR_START_SPEED) {
        v = (v > 0) ? MOTOR_START_SPEED : -MOTOR_START_SPEED;
    }
    return v;
}

static void set_track_targets(int left, int right) {
    g_target_left_speed = clamp_track_target(left);
    g_target_right_speed = clamp_track_target(right);
}

// 리코일·비상 정지 등 램프 우회
static void set_track_immediate(int left, int right) {
    if (left > MOTOR_MAX_SPEED) left = MOTOR_MAX_SPEED;
    if (left < -MOTOR_MAX_SPEED) left = -MOTOR_MAX_SPEED;
    if (right > MOTOR_MAX_SPEED) right = MOTOR_MAX_SPEED;
    if (right < -MOTOR_MAX_SPEED) right = -MOTOR_MAX_SPEED;
    g_target_left_speed = left;
    g_target_right_speed = right;
    g_current_left_x10 = left * TRACK_SPEED_SCALE;
    g_current_right_x10 = right * TRACK_SPEED_SCALE;
    apply_motor_pwm(LEDC_CH_LEFT_IN1, LEDC_CH_LEFT_IN2, left);
    apply_motor_pwm(LEDC_CH_RIGHT_IN1, LEDC_CH_RIGHT_IN2, right);
}

static void process_motor_ramp(void) {
    if (g_recoil_active) return;

    update_track_speed_x10(&g_current_left_x10, g_target_left_speed);
    update_track_speed_x10(&g_current_right_x10, g_target_right_speed);
    apply_motor_pwm(LEDC_CH_LEFT_IN1, LEDC_CH_LEFT_IN2,
                    g_current_left_x10 / TRACK_SPEED_SCALE);
    apply_motor_pwm(LEDC_CH_RIGHT_IN1, LEDC_CH_RIGHT_IN2,
                    g_current_right_x10 / TRACK_SPEED_SCALE);
}

// ============================================================================
// 서보 제어 — 0.1° 단위 연속 슬루 (D-Pad 유지 시 매 루프 목표 전진)
// ============================================================================
static int clamp_turret_x10(int x10) {
    if (x10 < TURRET_MIN_X10) return TURRET_MIN_X10;
    if (x10 > TURRET_MAX_X10) return TURRET_MAX_X10;
    return x10;
}

// LEDC 해제 후 핀을 GPIO LOW로 두면 SG90이 한쪽으로 달려가고,
// 그 다음 ledc_channel_config 만으로는 PWM이 핀에 다시 안 붙는 경우가 있다.
static void servo_ledc_bind(ledc_channel_t channel, gpio_num_t pin) {
    gpio_reset_pin(pin);
    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel,
        .timer_sel = LEDC_TIMER_SERVO,
        .gpio_num = pin,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
}

// angle_x10: 0.1° 단위 → 14-bit duty (0.5ms~2.5ms)
static void turret_apply_pwm_x10(int angle_x10) {
    angle_x10 = clamp_turret_x10(angle_x10);
    uint32_t duty = 409 + (uint32_t)((int32_t)angle_x10 * (2048 - 409) / TURRET_PWM_FULL_SCALE_X10);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CH_SERVO, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CH_SERVO);
}

static void turret_attach(void) {
    if (g_turret_attached) return;

    servo_ledc_bind(LEDC_CH_SERVO, PIN_TURRET_SERVO);
    g_turret_attached = true;
    turret_apply_pwm_x10(g_turret_current_x10);
    ESP_LOGI(TAG, "터렛 yaw 서보 attach (GPIO%d) cur=%d°", PIN_TURRET_SERVO,
             g_turret_current_x10 / TURRET_DEG_SCALE);
}

static void turret_detach(void) {
    if (!g_turret_attached) return;

    // 핀을 먼저 LEDC에서 떼서 idle_level LOW가 SG90로 나가지 않게 한다.
    gpio_reset_pin(PIN_TURRET_SERVO);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CH_SERVO, 0);
    g_turret_attached = false;
    g_turret_hold_left = false;
    g_turret_hold_right = false;
    ESP_LOGI(TAG, "터렛 yaw 서보 detach");
}

// 목표 각도(도). immediate=true 이면 슬루 없이 현재=목표
static void set_turret_target_deg(int angle_deg, bool immediate) {
    int x10 = clamp_turret_x10(angle_deg * TURRET_DEG_SCALE);
    g_turret_target_x10 = x10;
    if (immediate) {
        g_turret_current_x10 = x10;
    }
    g_turret_last_command_ms = now_ms();
    if (!g_turret_attached) {
        turret_attach();
    } else if (immediate) {
        turret_apply_pwm_x10(g_turret_current_x10);
    }
}

// 매 10ms: D-Pad 홀드 시 목표 연속 증가 + current 슬루 + PWM
static void process_turret_slew(void) {
    // 홀드 중이면 목표를 매 루프 조금씩 이동 (100ms 점프 제거)
    if (g_turret_hold_left && !g_turret_hold_right) {
        g_turret_last_command_ms = now_ms();
        int next = g_turret_target_x10 - TURRET_HOLD_X10_PER_LOOP;
        g_turret_target_x10 = clamp_turret_x10(next);
        if (!g_turret_attached) {
            turret_attach();
        }
    } else if (g_turret_hold_right && !g_turret_hold_left) {
        g_turret_last_command_ms = now_ms();
        int next = g_turret_target_x10 + TURRET_HOLD_X10_PER_LOOP;
        g_turret_target_x10 = clamp_turret_x10(next);
        if (!g_turret_attached) {
            turret_attach();
        }
    }

    if (!g_turret_attached) return;
    if (g_turret_current_x10 == g_turret_target_x10) return;

    int diff = g_turret_target_x10 - g_turret_current_x10;
    int step = TURRET_SLEW_X10_PER_LOOP;
    if (diff > step) {
        g_turret_current_x10 += step;
    } else if (diff < -step) {
        g_turret_current_x10 -= step;
    } else {
        g_turret_current_x10 = g_turret_target_x10;
    }
    turret_apply_pwm_x10(g_turret_current_x10);
}

static void process_turret_idle(void) {
    if (!g_turret_attached) return;
    if (g_turret_hold_left || g_turret_hold_right) return;
    if (g_turret_current_x10 != g_turret_target_x10) return;
    if ((now_ms() - g_turret_last_command_ms) < TURRET_IDLE_DETACH_MS) return;
    turret_detach();
}

// ============================================================================
// 포탑 상하(pitch) — GPIO23
// ============================================================================
static int clamp_pitch_x10(int x10) {
    if (x10 < PITCH_MIN_X10) return PITCH_MIN_X10;
    if (x10 > PITCH_MAX_X10) return PITCH_MAX_X10;
    return x10;
}

static void pitch_apply_pwm_x10(int angle_x10) {
    angle_x10 = clamp_pitch_x10(angle_x10);
    uint32_t duty = 409 + (uint32_t)((int32_t)angle_x10 * (2048 - 409) / TURRET_PWM_FULL_SCALE_X10);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CH_PITCH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CH_PITCH);
}

static void pitch_attach(void) {
    if (g_pitch_attached) return;

    servo_ledc_bind(LEDC_CH_PITCH, PIN_TURRET_PITCH);
    g_pitch_attached = true;
    pitch_apply_pwm_x10(g_pitch_current_x10);
    ESP_LOGI(TAG, "포탑 pitch 서보 attach (GPIO%d)", PIN_TURRET_PITCH);
}

static void pitch_detach(void) {
    if (!g_pitch_attached) return;

    gpio_reset_pin(PIN_TURRET_PITCH);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CH_PITCH, 0);
    g_pitch_attached = false;
    g_pitch_hold_up = false;
    g_pitch_hold_down = false;
    ESP_LOGI(TAG, "포탑 pitch 서보 detach");
}

static void set_pitch_target_deg(int angle_deg, bool immediate) {
    int x10 = clamp_pitch_x10(angle_deg * TURRET_DEG_SCALE);
    g_pitch_target_x10 = x10;
    if (immediate) {
        g_pitch_current_x10 = x10;
    }
    g_pitch_last_input_ms = now_ms();
    if (!g_pitch_attached) {
        pitch_attach();
    } else if (immediate) {
        pitch_apply_pwm_x10(g_pitch_current_x10);
    }
}

// 목표 ±PITCH_STEP_DEG (Kconfig min~max)
static void pitch_nudge(int delta_deg) {
    int cur_deg = g_pitch_target_x10 / TURRET_DEG_SCALE;
    int next = cur_deg + delta_deg;
    if (next < PITCH_MIN_DEG) next = PITCH_MIN_DEG;
    if (next > PITCH_MAX_DEG) next = PITCH_MAX_DEG;
    if (next * TURRET_DEG_SCALE == g_pitch_target_x10) return;
    set_pitch_target_deg(next, false);
}

static void process_pitch_slew(void) {
    // 홀드 중: 일정 간격으로 1°씩 목표 갱신 (계속 움직임)
    if (g_pitch_hold_up || g_pitch_hold_down) {
        int64_t now = now_ms();
        g_pitch_last_input_ms = now;
        if (!g_pitch_attached) {
            pitch_attach();
        }
        if (now - g_pitch_last_step_ms >= PITCH_HOLD_INTERVAL_MS) {
            g_pitch_last_step_ms = now;
            if (g_pitch_hold_up && !g_pitch_hold_down) {
                pitch_nudge(+PITCH_STEP_DEG);
            } else if (g_pitch_hold_down && !g_pitch_hold_up) {
                pitch_nudge(-PITCH_STEP_DEG);
            }
        }
    }

    if (!g_pitch_attached) return;
    if (g_pitch_current_x10 == g_pitch_target_x10) return;

    int diff = g_pitch_target_x10 - g_pitch_current_x10;
    int step = PITCH_SLEW_X10_PER_LOOP;
    if (diff > step) {
        g_pitch_current_x10 += step;
    } else if (diff < -step) {
        g_pitch_current_x10 -= step;
    } else {
        g_pitch_current_x10 = g_pitch_target_x10;
    }
    pitch_apply_pwm_x10(g_pitch_current_x10);
}

static void process_pitch_idle(void) {
    if (!g_pitch_attached) return;
    if (g_pitch_hold_up || g_pitch_hold_down) return;
    if (g_pitch_current_x10 != g_pitch_target_x10) return;
    if ((now_ms() - g_pitch_last_input_ms) < TURRET_IDLE_DETACH_MS) return;
    pitch_detach();
}

// ============================================================================
// 게임패드 처리
// ============================================================================
static uint8_t dpad_filter_opposing(uint8_t mask, uint8_t neg, uint8_t pos) {
    bool neg_on = (mask & neg) != 0;
    bool pos_on = (mask & pos) != 0;
    if (neg_on && pos_on) {
        mask &= (uint8_t)~(neg | pos);
    }
    return mask;
}

static void reset_dpad_holds(void) {
    s_last_dpad_report_seq = 0;
    s_prev_block_buttons = 0;
    s_dpad_quiet_until_ms = 0;
    g_turret_hold_left = false;
    g_turret_hold_right = false;
    g_pitch_hold_up = false;
    g_pitch_hold_down = false;
}

static void apply_dpad_holds(uint8_t dpad, uint16_t buttons, uint32_t report_seq) {
    if (report_seq == s_last_dpad_report_seq) {
        return;
    }
    s_last_dpad_report_seq = report_seq;

    uint16_t block = buttons & DPAD_HOLD_BLOCK_MASK;
    if (block != s_prev_block_buttons) {
        s_dpad_quiet_until_ms = now_ms() + DPAD_QUIET_AFTER_BUTTON_MS;
        s_prev_block_buttons = block;
    }
    if (block != 0 || now_ms() < s_dpad_quiet_until_ms) {
        g_turret_hold_left = false;
        g_turret_hold_right = false;
        g_pitch_hold_up = false;
        g_pitch_hold_down = false;
        return;
    }

    uint8_t yaw = dpad_filter_opposing(dpad, DPAD_LEFT, DPAD_RIGHT);
    uint8_t pitch = dpad_filter_opposing(dpad, DPAD_UP, DPAD_DOWN);
    g_turret_hold_left = (yaw & DPAD_LEFT) != 0;
    g_turret_hold_right = (yaw & DPAD_RIGHT) != 0;
    g_pitch_hold_up = (pitch & DPAD_UP) != 0;
    g_pitch_hold_down = (pitch & DPAD_DOWN) != 0;
}

static void process_gamepad(int32_t axis_y, int32_t axis_ry,
                            uint16_t buttons, uint8_t dpad, uint8_t misc_buttons,
                            uint32_t report_seq) {
    if (now_ms() < g_input_ignore_until_ms) {
        if (!g_recoil_active) {
            set_track_targets(0, 0);
        }
        return;
    }

    int left_y = (abs(axis_y) < GAMEPAD_STICK_DEADZONE) ? 0 : (int)axis_y;
    int right_y = (abs(axis_ry) < GAMEPAD_STICK_DEADZONE) ? 0 : (int)axis_ry;

    if (!g_recoil_active) {
        set_track_targets(left_y, right_y);
    }

    apply_dpad_holds(dpad, buttons, report_seq);

    if (misc_buttons & MISC_BUTTON_START) {
        if (!g_start_pressed) {
            g_start_pressed = true;
            g_turret_hold_left = false;
            g_turret_hold_right = false;
            g_pitch_hold_up = false;
            g_pitch_hold_down = false;
            set_turret_target_deg(TURRET_CENTER_DEG, true);
            set_pitch_target_deg(PITCH_CENTER_DEG, true);
        }
    } else {
        g_start_pressed = false;
    }

    // Y: 헤드라이트 On/Off (press edge only — hold 시 재토글 없음)
    if (buttons & BUTTON_Y) {
        if (!g_y_pressed) {
            g_y_pressed = true;
            g_headlight_on = !g_headlight_on;
            set_headlight(g_headlight_on);
            ESP_LOGI(TAG, "헤드라이트 %s", g_headlight_on ? "ON" : "OFF");
        }
    } else {
        g_y_pressed = false;
    }

    // B 버튼: 포신 — 효과음 즉시 → 500ms 후 LED → 500ms 후 리코일
    if ((buttons & BUTTON_B) && !g_cannon_pending && !g_cannon_firing && !g_machinegun_firing
            && !g_recoil_pending && !g_recoil_active) {
        int64_t press_ms = now_ms();
        dfplayer_play_effect(DFPLAYER_TRACK_CANNON);
        g_cannon_pending = true;
        g_cannon_followup_at_ms = press_ms + CANNON_FOLLOWUP_DELAY_MS;
        gpio_set_level(PIN_CANNON_LED, 0);
    }

    // A 버튼: 기관총 — 효과음 즉시, LED는 DFPlayer 지연(MG_LED_DELAY_MS) 후 깜빡임
    if ((buttons & BUTTON_A) && !g_machinegun_firing && !g_cannon_pending && !g_cannon_firing) {
        int64_t press_ms = now_ms();
        dfplayer_play_effect(DFPLAYER_TRACK_MACHINEGUN);
        g_machinegun_firing = true;
        g_machinegun_led_start_ms = press_ms + MG_LED_DELAY_MS;
        g_machinegun_end_ms = g_machinegun_led_start_ms + MACHINE_GUN_DURATION;
        g_mg_led_on = false;
        g_mg_led_last_toggle = 0;
        gpio_set_level(PIN_MG_LED, 0);
    }

    // L1: 볼륨 감소 (홀드 중 100ms마다 1단계, 즉시 반영 / 릴리스 시 NVS 저장)
    if (buttons & BUTTON_SHOULDER_L) {
        if (!g_l1_pressed) {
            g_l1_pressed = true;
            g_l1_last_change = 0; // 첫 스텝 즉시 적용
        }
        if (now_ms() - g_l1_last_change >= VOLUME_CHANGE_INTERVAL) {
            g_l1_last_change = now_ms();
            if (g_current_volume > 11) {
                g_current_volume--;
                dfplayer_set_volume(g_current_volume);
                g_volume_dirty = true;
            }
        }
    } else if (g_l1_pressed) {
        g_l1_pressed = false;
        if (g_volume_dirty) {
            g_volume_dirty = false;
            save_volume_to_nvs(g_current_volume);
        }
    }

    // R1: 볼륨 증가 (L1과 동일)
    if (buttons & BUTTON_SHOULDER_R) {
        if (!g_r1_pressed) {
            g_r1_pressed = true;
            g_r1_last_change = 0; // 첫 스텝 즉시 적용
        }
        if (now_ms() - g_r1_last_change >= VOLUME_CHANGE_INTERVAL) {
            g_r1_last_change = now_ms();
            if (g_current_volume < 30) {
                g_current_volume++;
                dfplayer_set_volume(g_current_volume);
                g_volume_dirty = true;
            }
        }
    } else if (g_r1_pressed) {
        g_r1_pressed = false;
        if (g_volume_dirty) {
            g_volume_dirty = false;
            save_volume_to_nvs(g_current_volume);
        }
    }
}

// ============================================================================
// 타이머 기반 처리
// ============================================================================
static void process_cannon_sequence(void) {
    int64_t now = now_ms();

    if (g_cannon_pending && now >= g_cannon_followup_at_ms) {
        g_cannon_pending = false;
        g_cannon_firing = true;
        g_cannon_end_ms = now + CANNON_LED_DURATION;
        gpio_set_level(PIN_CANNON_LED, 1);

        g_recoil_pending = true;
        g_recoil_at_ms = now + CANNON_RECOIL_DELAY_MS;
    }

    if (g_cannon_firing && now >= g_cannon_end_ms) {
        g_cannon_firing = false;
        gpio_set_level(PIN_CANNON_LED, 0);
    }
}

// MG_LED_DELAY_MS 후 MG_LED_BLINK_MS 주기로 토글, MACHINE_GUN_DURATION 후 소등
static void process_machinegun_firing(void) {
    if (!g_machinegun_firing) return;

    int64_t now = now_ms();
    if (now >= g_machinegun_end_ms) {
        g_machinegun_firing = false;
        g_mg_led_on = false;
        gpio_set_level(PIN_MG_LED, 0);
        return;
    }

    if (now < g_machinegun_led_start_ms) {
        return;
    }

    if (g_mg_led_last_toggle < g_machinegun_led_start_ms) {
        g_mg_led_on = true;
        g_mg_led_last_toggle = now;
        gpio_set_level(PIN_MG_LED, 1);
        return;
    }

    if (now - g_mg_led_last_toggle >= MG_LED_BLINK_MS) {
        g_mg_led_last_toggle = now;
        g_mg_led_on = !g_mg_led_on;
        gpio_set_level(PIN_MG_LED, g_mg_led_on ? 1 : 0);
    }
}

static void process_recoil(void) {
    int64_t now = now_ms();

    if (g_recoil_pending) {
        if (now < g_recoil_at_ms) {
            return;
        }
        g_recoil_pending = false;
        g_recoil_active = true;
        g_recoil_start_time = now;
        set_track_immediate(RECOIL_BACK_SPEED, RECOIL_BACK_SPEED);
    }

    if (!g_recoil_active) return;
    int64_t elapsed = now - g_recoil_start_time;

    if (elapsed < RECOIL_BACK_DURATION) {
        set_track_immediate(RECOIL_BACK_SPEED, RECOIL_BACK_SPEED);
        return;
    }
    if (elapsed < RECOIL_BACK_DURATION + RECOIL_SETTLE_DURATION) {
        set_track_immediate(0, 0);
        return;
    }
    g_recoil_active = false;
}

static bool dfplayer_boot_ready(void) {
    return g_dfplayer_ok && now_ms() >= g_dfplayer_ready_at_ms;
}

static void start_idle_sound(void) {
    if (!dfplayer_boot_ready())
        return;
    dfplayer_play_loop(DFPLAYER_TRACK_IDLE);
    g_idle_sound_started = true;
    g_last_idle_sound_time = now_ms();
    g_idle_sound_retries = 0;
    ESP_LOGI(TAG, "DFPlayer idle loop (0001)");
}

// SD 초기화 후에만 UART를 붙이고 볼륨·대기음·연결음을 보낸다. BLE 시작은 막지 않는다.
static void process_dfplayer_boot(void) {
    if (g_dfplayer_failed)
        return;
    if (now_ms() < g_dfplayer_ready_at_ms)
        return;

    if (!g_dfplayer_ok) {
        if (dfplayer_init() != ESP_OK) {
            ESP_LOGW(TAG, "DFPlayer init failed — continuing without sound");
            g_dfplayer_failed = true;
            return;
        }
        g_dfplayer_ok = true;
        ESP_LOGI(TAG, "DFPlayer UART attached");
        return;
    }

    if (!g_dfplayer_volume_sent) {
        dfplayer_set_volume(g_current_volume);
        g_dfplayer_volume_sent = true;
        return;
    }

    if (g_pending_connect_sound) {
        g_pending_connect_sound = false;
        dfplayer_play(DFPLAYER_TRACK_CONNECTED);
        return;
    }

    if (gamepad_is_connected())
        return;
    if (g_idle_sound_started)
        return;
    start_idle_sound();
}

// loop 재생이 이미 진행 중이면 재전송으로 트랙이 재시작되므로,
// DFPlayer SD 초기화 구간(명령 유실 가능) 동안에만 재전송한다.
static void process_idle_sound(void) {
    if (!dfplayer_boot_ready()) return;
    if (gamepad_is_connected()) return;
    if (!g_idle_sound_started) return;
    if (g_idle_sound_retries >= IDLE_SOUND_MAX_RETRIES) return;

    if (now_ms() - g_last_idle_sound_time >= IDLE_SOUND_RETRY_INTERVAL_MS) {
        dfplayer_play_loop(DFPLAYER_TRACK_IDLE);
        g_last_idle_sound_time = now_ms();
        g_idle_sound_retries++;
    }
}

// ============================================================================
// 제어 태스크 (FreeRTOS)
// ============================================================================
static void control_task(void* arg) {
    (void)arg;

    int32_t axis_y, axis_ry;
    uint16_t buttons;
    uint8_t dpad, misc_buttons;

    bool prev_connected = false;
    bool prev_input_stale = false;

    while (1) {
        bool cur_connected = gamepad_is_connected();

        // 연결 직후: 터렛 서보 중앙 PWM + 입력 유예 + 효과음 (이후 2초 idle이면 detach)
        if (gamepad_read_new_connection()) {
            int64_t now = now_ms();
            g_input_ignore_until_ms = now + GAMEPAD_CONNECT_GRACE_MS;
            reset_dpad_holds();
            set_track_targets(0, 0);
            set_turret_target_deg(TURRET_CENTER_DEG, true);
            set_pitch_target_deg(PITCH_CENTER_DEG, true);

            if (dfplayer_boot_ready()) {
                dfplayer_play(DFPLAYER_TRACK_CONNECTED);
            } else {
                g_pending_connect_sound = true;
            }
            // 헤드라이트는 Y 토글 — 연결 시 자동 ON 하지 않음
            g_y_pressed = false;
            // 모터 드라이버 enable (IN은 이미 0 / 램프 정지)
            motor_driver_set_enabled(true);
            ESP_LOGI(TAG, "게임패드 연결됨");
        }

        // 연결 해제 직후: 정리 (서보 포함)
        if (prev_connected && !cur_connected) {
            dfplayer_set_volume(g_current_volume); // 부팅 시와 동일하게 설정 볼륨 유지
            start_idle_sound();
            set_track_immediate(0, 0);
            motor_driver_set_enabled(false); // nSLEEP LOW — 부팅 외 안전 컷오프
            turret_detach();
            pitch_detach();
            reset_dpad_holds();
            g_cannon_pending = false;
            g_cannon_firing = false;
            g_recoil_pending = false;
            g_recoil_active = false;
            g_machinegun_firing = false;
            g_mg_led_on = false;
            g_y_pressed = false;
            g_headlight_on = false;
            gpio_set_level(PIN_MG_LED, 0);
            gpio_set_level(PIN_CANNON_LED, 0);
            set_headlight(false);
            ESP_LOGI(TAG, "게임패드 연결 해제됨");
        }
        prev_connected = cur_connected;

        // BLE HID는 상태 변화가 있을 때만 리포트를 보낸다. 무입력(스틱 0)을
        // 타임아웃으로 보면 버튼/D-Pad가 1초마다 끊긴다. 스틱이 살아 있는데
        // 리포트가 멈춘 경우만 트랙을 강제 정지한다.
        if (cur_connected) {
            bool got_pad = gamepad_read(&axis_y, &axis_ry, &buttons, &dpad, &misc_buttons);
            uint32_t report_seq = gamepad_report_seq();
            bool report_old = (now_ms() - gamepad_last_report_ms()) > GAMEPAD_INPUT_TIMEOUT_MS;
            bool stick_live = got_pad && (abs(axis_y) >= GAMEPAD_STICK_DEADZONE ||
                                          abs(axis_ry) >= GAMEPAD_STICK_DEADZONE);
            bool input_stale = report_old && stick_live;
            if (input_stale) {
                if (!g_recoil_active) {
                    set_track_targets(0, 0);
                }
                reset_dpad_holds();
                if (!prev_input_stale) {
                    ESP_LOGW(TAG, "게임패드 입력 타임아웃 — 트랙 정지");
                }
            } else if (got_pad) {
                process_gamepad(axis_y, axis_ry, buttons, dpad, misc_buttons, report_seq);
            }
            prev_input_stale = input_stale;
        } else {
            prev_input_stale = false;
        }

        process_dfplayer_boot();
        process_cannon_sequence();
        process_machinegun_firing();
        process_recoil();
        process_motor_ramp();
        process_turret_slew();
        process_pitch_slew();
        process_turret_idle();
        process_pitch_idle();
        process_idle_sound();

        vTaskDelay(pdMS_TO_TICKS(LOOP_INTERVAL_MS));
    }
}

// ============================================================================
// Bluepad32 메인 (BTstack 루프 - 절대 리턴 안 함)
// ============================================================================
static void bt_main_task(void* arg) {
    (void)arg;

    btstack_init();
    uni_platform_set_custom(get_my_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();

    // 여기까지 오면 안 됨
    vTaskDelete(NULL);
}

// ============================================================================
// 앱 메인
// ============================================================================
void app_main(void) {
    // 1) DRV8833 즉시 sleep (외장 nSLEEP pull-down + 소프트웨어 LOW)
    //    부팅 중 IN 플로팅이 있어도 모터 브리지는 꺼진 상태 유지
    gpio_reset_pin(PIN_MOTOR_NSLEEP);
    gpio_set_direction(PIN_MOTOR_NSLEEP, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_MOTOR_NSLEEP, 0);
    // DFPlayer RX가 부팅 중 글리치를 보지 않도록 UART idle(High) 유지
    dfplayer_hold_tx_idle();

    // 캐패시터 충전 대기 (이 동안 nSLEEP=LOW)
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "RC Tank 초기화 시작");

    // NVS 초기화
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    load_volume_from_nvs();

    // 2) 모터 IN 및 기타 GPIO — 전부 정지 레벨로 설정한 뒤 드라이버 enable
    gpio_reset_pin(PIN_LEFT_IN1);
    gpio_reset_pin(PIN_LEFT_IN2);
    gpio_reset_pin(PIN_RIGHT_IN1);
    gpio_reset_pin(PIN_RIGHT_IN2);
    gpio_reset_pin(PIN_CANNON_LED);
    gpio_reset_pin(PIN_MG_LED);
    gpio_reset_pin(PIN_HEADLIGHT);
    // 터렛 서보(좌우/상하)는 게임패드 연결 시 attach (부팅 시 PWM 안 함)
    gpio_reset_pin(PIN_TURRET_SERVO);
    gpio_reset_pin(PIN_TURRET_PITCH);

    gpio_set_direction(PIN_LEFT_IN1, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LEFT_IN2, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_RIGHT_IN1, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_RIGHT_IN2, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_CANNON_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_MG_LED, GPIO_MODE_OUTPUT);

    gpio_set_level(PIN_LEFT_IN1, 0);
    gpio_set_level(PIN_LEFT_IN2, 0);
    gpio_set_level(PIN_RIGHT_IN1, 0);
    gpio_set_level(PIN_RIGHT_IN2, 0);
    gpio_set_level(PIN_CANNON_LED, 0);
    gpio_set_level(PIN_MG_LED, 0);

    // LEDC 초기화 (서보 채널은 패드 연결/D-Pad 시 attach, 무입력 2초 후 detach)
    init_ledc();
    init_headlight_pwm();  // GPIO20 SDM 10% — LEDC 채널이 없어서 별도

    // 3) IN/PWM 준비 완료 후 드라이버 enable
    //    (게임패드 없으면 바로 enable — 스틱 입력 전에도 정지 유지)
    motor_driver_set_enabled(true);

    // DFPlayer UART는 SD 부팅이 끝난 뒤 control_task에서 연결 (실패해도 탱크/BT는 계속)
    g_dfplayer_ready_at_ms = now_ms() + DFPLAYER_BOOT_SETTLE_MS;
    ESP_LOGI(TAG, "DFPlayer: UART in %d ms (wait for SD)", DFPLAYER_BOOT_SETTLE_MS);

    ESP_LOGI(TAG, "초기화 완료 — BTstack 시작");

    // 게임패드 상태 뮤프스 생성 (BTstack보다 먼저)
    gamepad_state_init();

    // 제어 태스크 (BTstack보다 낮은 우선순위 — C6 단일 코어에서 BLE 안정성)
    xTaskCreate(control_task, "tank_ctrl", 4096, NULL, 4, NULL);

    // BTstack 메인 태스크 (높은 우선순위, 코어 0)
    xTaskCreatePinnedToCore(bt_main_task, "bt_main", 8192, NULL, 10, NULL, 0);
}
