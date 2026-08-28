/**
 * @file dfplayer.c
 * @brief DFPlayer Mini UART 제어 (panzer4-idf rctank_dfplayer 기반)
 */
#include "dfplayer.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "dfplayer";

/* 핀: menuconfig → RC Tank Hardware Pins
 * C6 Super Mini 기본 TX=22, RX=-1 (TX 전용 — 피드백 배선/태스크 없음).
 * 피하기: GPIO8/9(strap·BOOT·RGB), 12/13(USB), 15(strap).
 */
#define PIN_DFPLAYER_TX  CONFIG_PIN_DFPLAYER_TX
#define PIN_DFPLAYER_RX  CONFIG_PIN_DFPLAYER_RX

#define UART_NUM         UART_NUM_1
#define UART_BAUD        9600
#define UART_BUF_SIZE    256

/* DFPlayer 패킷 상수 */
#define DFPLAYER_SB          0x7E
#define DFPLAYER_VER         0xFF
#define DFPLAYER_LEN         0x06
#define DFPLAYER_NO_FEEDBACK 0x00
#define DFPLAYER_EB          0xEF

/* 명령 코드 */
#define DFPLAYER_CMD_PLAY           0x03
#define DFPLAYER_CMD_VOLUME         0x06
#define DFPLAYER_CMD_LOOP_TRACK     0x08  /* DFRobot loop(fileNumber) — 해당 파일 반복 */
#define DFPLAYER_CMD_SET_DEVICE     0x09  /* 2 = SD */
#define DFPLAYER_CMD_REPEAT_PLAY    0x11  /* 0=전체 반복 해제 */
#define DFPLAYER_CMD_STOP           0x16
#define DFPLAYER_CMD_REPEAT_TRACK   0x19  /* DFRobot: 0=현재곡 반복, 1=해제 */

#define DFPLAYER_DEVICE_SD          2
#define DFPLAYER_STACK_SIZE 10
#define DFPLAYER_CMD_GAP_MS 200
#define DFPLAYER_STOP_SETTLE_MS 300
#define DFPLAYER_SOURCE_SETTLE_MS 500

typedef struct {
    uint8_t start_byte;
    uint8_t version;
    uint8_t length;
    uint8_t command_value;
    uint8_t feedback_value;
    uint8_t param_msb;
    uint8_t param_lsb;
    uint8_t checksum_msb;
    uint8_t checksum_lsb;
    uint8_t end_byte;
} dfplayer_stack_t;

static void dfplayer_find_checksum(dfplayer_stack_t* stack) {
    uint16_t sum = (uint16_t)(stack->version + stack->length + stack->command_value
                              + stack->feedback_value + stack->param_msb + stack->param_lsb);
    uint16_t checksum = (uint16_t)((~sum) + 1);
    stack->checksum_msb = (uint8_t)(checksum >> 8);
    stack->checksum_lsb = (uint8_t)(checksum & 0xFF);
}

static esp_err_t dfplayer_send_stack(const dfplayer_stack_t* stack) {
    uint8_t buf[DFPLAYER_STACK_SIZE] = {
        stack->start_byte, stack->version, stack->length,
        stack->command_value, stack->feedback_value,
        stack->param_msb, stack->param_lsb,
        stack->checksum_msb, stack->checksum_lsb, stack->end_byte,
    };
    int n = uart_write_bytes(UART_NUM, buf, DFPLAYER_STACK_SIZE);
    return (n == DFPLAYER_STACK_SIZE) ? ESP_OK : ESP_FAIL;
}

static int64_t s_last_cmd_ms;
static bool s_track_looping;

static void dfplayer_wait_cmd_gap(void) {
    if (s_last_cmd_ms == 0)
        return;
    int64_t elapsed = (esp_timer_get_time() / 1000) - s_last_cmd_ms;
    if (elapsed < DFPLAYER_CMD_GAP_MS)
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(DFPLAYER_CMD_GAP_MS - elapsed)));
}

static esp_err_t dfplayer_send_cmd(uint8_t cmd, uint8_t param_msb, uint8_t param_lsb) {
    dfplayer_stack_t stack = {
        .start_byte = DFPLAYER_SB,
        .version = DFPLAYER_VER,
        .length = DFPLAYER_LEN,
        .command_value = cmd,
        .feedback_value = DFPLAYER_NO_FEEDBACK,
        .param_msb = param_msb,
        .param_lsb = param_lsb,
        .end_byte = DFPLAYER_EB,
    };
    dfplayer_find_checksum(&stack);
    dfplayer_wait_cmd_gap();
    esp_err_t ret = dfplayer_send_stack(&stack);
    s_last_cmd_ms = esp_timer_get_time() / 1000;
    return ret;
}

static esp_err_t dfplayer_send_cmd_now(uint8_t cmd, uint8_t param_msb, uint8_t param_lsb) {
    dfplayer_stack_t stack = {
        .start_byte = DFPLAYER_SB,
        .version = DFPLAYER_VER,
        .length = DFPLAYER_LEN,
        .command_value = cmd,
        .feedback_value = DFPLAYER_NO_FEEDBACK,
        .param_msb = param_msb,
        .param_lsb = param_lsb,
        .end_byte = DFPLAYER_EB,
    };
    dfplayer_find_checksum(&stack);
    esp_err_t ret = dfplayer_send_stack(&stack);
    s_last_cmd_ms = esp_timer_get_time() / 1000;
    return ret;
}

static bool dfplayer_verify_checksum(const dfplayer_stack_t* stack) {
    uint16_t sum = (uint16_t)(stack->version + stack->length + stack->command_value
                              + stack->feedback_value + stack->param_msb + stack->param_lsb);
    uint16_t checksum = (uint16_t)((~sum) + 1);
    return (stack->checksum_msb == (uint8_t)(checksum >> 8)) &&
           (stack->checksum_lsb == (uint8_t)(checksum & 0xFF));
}

/**
 * RX 태스크: DFPlayer 응답 수신, 트랙 완료 시 대기음 재생
 */
static void dfplayer_task(void* arg) {
    (void)arg;
    uint8_t data[128];
    uint8_t pkt_buf[DFPLAYER_STACK_SIZE];
    int pkt_idx = 0;

    while (1) {
        int len = uart_read_bytes(UART_NUM, data, sizeof(data), pdMS_TO_TICKS(50));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t b = data[i];
                if (pkt_idx == 0) {
                    if (b == DFPLAYER_SB) {
                        pkt_buf[pkt_idx++] = b;
                    }
                } else {
                    pkt_buf[pkt_idx++] = b;
                    if (pkt_idx >= DFPLAYER_STACK_SIZE) {
                        if (pkt_buf[9] == DFPLAYER_EB) {
                            dfplayer_stack_t* s = (dfplayer_stack_t*)pkt_buf;
                            if (dfplayer_verify_checksum(s)) {
                                /* 0x3D = Track Finished */
                                if (s->command_value == 0x3D) {
                                    dfplayer_play_loop(DFPLAYER_TRACK_IDLE);
                                }
                            }
                        }
                        pkt_idx = 0;
                    }
                }
            }
        }
    }
}

void dfplayer_hold_tx_idle(void) {
    gpio_reset_pin(PIN_DFPLAYER_TX);
    gpio_set_direction(PIN_DFPLAYER_TX, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DFPLAYER_TX, 1);
}

esp_err_t dfplayer_init(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(UART_NUM, &uart_config);
    if (ret != ESP_OK) return ret;

    const int rx_buf = (PIN_DFPLAYER_RX < 0) ? 0 : UART_BUF_SIZE;
    ret = uart_driver_install(UART_NUM, UART_BUF_SIZE, rx_buf, 0, NULL, 0);
    if (ret != ESP_OK) return ret;

    const int rx_pin = (PIN_DFPLAYER_RX < 0) ? UART_PIN_NO_CHANGE : PIN_DFPLAYER_RX;
    ret = uart_set_pin(UART_NUM, PIN_DFPLAYER_TX, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(200));
    dfplayer_send_cmd(DFPLAYER_CMD_SET_DEVICE, 0, DFPLAYER_DEVICE_SD);
    vTaskDelay(pdMS_TO_TICKS(DFPLAYER_SOURCE_SETTLE_MS));

    if (PIN_DFPLAYER_RX >= 0) {
        xTaskCreate(dfplayer_task, "dfplayer_rx", 2048, NULL, 5, NULL);
    }
    ESP_LOGI(TAG, "DFPlayer init TX=%d RX=%d", PIN_DFPLAYER_TX, PIN_DFPLAYER_RX);
    return ESP_OK;
}

/* 0x08 반복은 STOP만으로는 안 풀린다. 일회 재생 전에 모드를 지운다.
 * 0x19는 재생 중에 보내면 이 모듈에서 즉시 정지되므로, play 이후에 보내지 않는다. */
static void dfplayer_clear_loop_mode(void) {
    dfplayer_send_cmd(DFPLAYER_CMD_STOP, 0, 0);
    dfplayer_send_cmd(DFPLAYER_CMD_REPEAT_PLAY, 0, 0);
    dfplayer_send_cmd(DFPLAYER_CMD_REPEAT_TRACK, 0, 1);
    vTaskDelay(pdMS_TO_TICKS(DFPLAYER_STOP_SETTLE_MS));
    s_track_looping = false;
}

esp_err_t dfplayer_play(uint8_t track) {
    if (track < 1) return ESP_ERR_INVALID_ARG;
    if (s_track_looping)
        dfplayer_clear_loop_mode();
    return dfplayer_send_cmd(DFPLAYER_CMD_PLAY, 0, track);
}

esp_err_t dfplayer_play_effect(uint8_t track) {
    if (track < 1) return ESP_ERR_INVALID_ARG;
    /* STOP·루프 해제 생략 — 대기 루프 위 PLAY 겹침 (명령 전송 지연 ~500ms+ 단축) */
    s_track_looping = false;
    return dfplayer_send_cmd_now(DFPLAYER_CMD_PLAY, 0, track);
}

esp_err_t dfplayer_play_loop(uint8_t track) {
    if (track < 1) return ESP_ERR_INVALID_ARG;
    /* 0x03+0x19는 클론에서 재생 직후 정지로 해석되는 경우가 있다. */
    esp_err_t ret = dfplayer_send_cmd(DFPLAYER_CMD_LOOP_TRACK, 0, track);
    s_track_looping = true;
    return ret;
}

esp_err_t dfplayer_set_volume(uint8_t vol) {
    if (vol > 30) vol = 30;
    return dfplayer_send_cmd(DFPLAYER_CMD_VOLUME, 0, vol);
}

esp_err_t dfplayer_stop(void) {
    s_track_looping = false;
    return dfplayer_send_cmd(DFPLAYER_CMD_STOP, 0, 0);
}
