#ifndef _PRESENCE_SENSOR_H_
#define _PRESENCE_SENSOR_H_

// ============================================================================
// PresenceSensor - radar de presença HLK-LD2401 (24 GHz).
//
// - Pino OUT: nível alto = tem gente.
// - Serial (256000 8N1): quadros "F4 F3 F2 F1 | len | dados | F8 F7 F6 F5"
//   com estado do alvo (movendo / parado) e distâncias em cm.
// O sensor detecta pessoas paradas (respiração), não só movimento.
// ============================================================================

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <mutex>
#include <string>

class PresenceSensor {
public:
    struct State {
        bool present = false;       // tem alguém (OUT ou serial)
        bool moving = false;        // alvo em movimento
        bool still = false;         // alvo parado
        int distance_cm = -1;       // distância do alvo principal (-1 = desconhecida)
        int moving_cm = 0;
        int still_cm = 0;
        int moving_energy = 0;
        int still_energy = 0;
        bool serial_ok = false;     // recebeu quadros nos últimos segundos
    };

    PresenceSensor(gpio_num_t out_gpio, uart_port_t uart, gpio_num_t rx_gpio, gpio_num_t tx_gpio,
                   int baud);
    void Start();
    State Get();
    // true se o sensor já respondeu pela serial alguma vez (ou se o OUT já subiu)
    bool Detected() const { return detected_; }
    std::string ToJson();

private:
    gpio_num_t out_gpio_;
    uart_port_t uart_;
    gpio_num_t rx_gpio_;
    gpio_num_t tx_gpio_;
    int baud_;
    std::mutex mutex_;
    State state_;
    int64_t last_frame_us_ = 0;
    volatile bool detected_ = false;

    static void TaskEntry(void* arg);
    void Task();
    void ParseFrame(const uint8_t* data, int len);
};

#endif  // _PRESENCE_SENSOR_H_
