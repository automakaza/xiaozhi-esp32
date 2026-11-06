#include "presence_sensor.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>

#define TAG "Presenca"

namespace {
constexpr uint8_t kHead[4] = {0xF4, 0xF3, 0xF2, 0xF1};
constexpr uint8_t kTail[4] = {0xF8, 0xF7, 0xF6, 0xF5};
constexpr int kBufSize = 256;
constexpr int64_t kSerialTimeoutUs = 3 * 1000 * 1000;
}  // namespace

PresenceSensor::PresenceSensor(gpio_num_t out_gpio, uart_port_t uart, gpio_num_t rx_gpio,
                               gpio_num_t tx_gpio, int baud)
    : out_gpio_(out_gpio), uart_(uart), rx_gpio_(rx_gpio), tx_gpio_(tx_gpio), baud_(baud) {}

void PresenceSensor::Start() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << out_gpio_;
    io.mode = GPIO_MODE_INPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;  // sem sensor ligado = "ninguém"
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    uart_config_t cfg = {};
    cfg.baud_rate = baud_;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(uart_, 1024, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_, tx_gpio_, rx_gpio_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(TaskEntry, "presenca", 4096, this, 3, nullptr);
    ESP_LOGI(TAG, "HLK-LD2401: OUT=%d, serial RX=%d TX=%d, %d baud", out_gpio_, rx_gpio_, tx_gpio_,
             baud_);
}

void PresenceSensor::TaskEntry(void* arg) {
    static_cast<PresenceSensor*>(arg)->Task();
    vTaskDelete(nullptr);
}

void PresenceSensor::Task() {
    uint8_t buf[kBufSize];
    int len = 0;
    int64_t last_log_us = 0;
    while (true) {
        int n = uart_read_bytes(uart_, buf + len, kBufSize - len, pdMS_TO_TICKS(100));
        if (n > 0) {
            len += n;
            // procura quadros completos no buffer
            int pos = 0;
            while (len - pos >= 10) {
                if (memcmp(buf + pos, kHead, 4) != 0) {
                    pos++;
                    continue;
                }
                int data_len = buf[pos + 4] | (buf[pos + 5] << 8);
                if (data_len <= 0 || data_len > kBufSize - 10) {
                    pos++;
                    continue;
                }
                int total = 4 + 2 + data_len + 4;
                if (len - pos < total) {
                    break;  // quadro ainda incompleto
                }
                if (memcmp(buf + pos + 6 + data_len, kTail, 4) == 0) {
                    ParseFrame(buf + pos + 6, data_len);
                    pos += total;
                } else {
                    pos++;
                }
            }
            if (pos > 0) {
                memmove(buf, buf + pos, len - pos);
                len -= pos;
            }
            if (len >= kBufSize) {
                len = 0;  // lixo: recomeça
            }
        }

        // OUT e validade da serial
        bool out = gpio_get_level(out_gpio_) == 1;
        int64_t now = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.serial_ok = last_frame_us_ != 0 && (now - last_frame_us_) < kSerialTimeoutUs;
            if (out) {
                detected_ = true;
            }
            bool serial_present = state_.serial_ok && (state_.moving || state_.still);
            state_.present = out || serial_present;
            if (now - last_log_us > 10 * 1000 * 1000) {
                last_log_us = now;
                ESP_LOGI(TAG, "presente=%d (OUT=%d) movendo=%d(%dcm,e%d) parado=%d(%dcm,e%d) dist=%dcm serial=%d",
                         state_.present, out, state_.moving, state_.moving_cm, state_.moving_energy,
                         state_.still, state_.still_cm, state_.still_energy, state_.distance_cm,
                         state_.serial_ok);
            }
        }
    }
}

// dados: tipo(1) AA estado(1) dist_mov(2) energia_mov(1) dist_parado(2)
//        energia_parado(1) dist_deteccao(2) [engenharia...] 55 00
void PresenceSensor::ParseFrame(const uint8_t* d, int len) {
    if (len < 13 || d[1] != 0xAA) {
        return;
    }
    uint8_t alvo = d[2];
    int mov_cm = d[3] | (d[4] << 8);
    int mov_e = d[5];
    int par_cm = d[6] | (d[7] << 8);
    int par_e = d[8];
    int det_cm = d[9] | (d[10] << 8);

    std::lock_guard<std::mutex> lock(mutex_);
    last_frame_us_ = esp_timer_get_time();
    detected_ = true;
    if (alvo > 0x03) {
        return;  // 4..6 = calibração de ruído de fundo
    }
    state_.moving = (alvo == 0x01 || alvo == 0x03);
    state_.still = (alvo == 0x02 || alvo == 0x03);
    state_.moving_cm = mov_cm;
    state_.still_cm = par_cm;
    state_.moving_energy = mov_e;
    state_.still_energy = par_e;
    int dist = -1;
    if (det_cm > 0) {
        dist = det_cm;
    } else if (state_.moving && mov_cm > 0) {
        dist = mov_cm;
    } else if (state_.still && par_cm > 0) {
        dist = par_cm;
    } else if (mov_cm > 0) {
        dist = mov_cm;
    }
    state_.distance_cm = (alvo == 0) ? -1 : dist;
}

PresenceSensor::State PresenceSensor::Get() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string PresenceSensor::ToJson() {
    State s = Get();
    std::string j = "{\"tem_alguem\":";
    j += s.present ? "true" : "false";
    j += ",\"movimento\":\"";
    j += s.moving ? "se movendo" : (s.still ? "parado" : "nenhum");
    j += "\",\"distancia_cm\":";
    j += std::to_string(s.distance_cm);
    j += "}";
    return j;
}
