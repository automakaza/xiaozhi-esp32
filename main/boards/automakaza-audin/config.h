#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// ============================================================================
// Automakaza AUDIN - placa ESP_S3-AUDIN RE1.1
// Módulo ESP32-S3-WROOM-1 N16R8 (16 MB flash quad / 8 MB PSRAM octal)
// Codec ES8388 + amplificador HT6872 + 2 microfones de eletreto
//
// Pinagem levantada e testada em 03/11/2026 (mapeamento por software +
// testes de áudio no ESPHome).
// ============================================================================

#include <driver/gpio.h>

// --- Áudio ------------------------------------------------------------------
// Es8388AudioCodec exige a mesma taxa na entrada e na saída.
#define AUDIO_INPUT_SAMPLE_RATE     24000
#define AUDIO_OUTPUT_SAMPLE_RATE    24000
#define AUDIO_DEFAULT_OUTPUT_VOLUME 50   // volume inicial após gravar o firmware

// Sem referência de saída na entrada: os dois microfones ocupam LIN1/RIN1 do
// ES8388 (não há laço de retorno do DAC para cancelamento de eco).
#define AUDIO_INPUT_REFERENCE       false

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_8
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_3
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_9
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_46   // ESP -> ES8388 (DSDIN)
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10   // ES8388 (ASDOUT) -> ESP

// O HT6872 não tem enable ligado a GPIO (fica sempre ligado).
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_NC
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_14
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_47
#define AUDIO_CODEC_ES8388_ADDR  ES8388_CODEC_DEFAULT_ADDR   // 0x20 (8 bits) = 0x10
#define AUDIO_CODEC_ES8388_ADDR_7BIT 0x10

// Como o amplificador não desliga, em repouso as saídas analógicas do DAC são
// desligadas (registrador 0x04) para tirar o chiado. Pôr 0 para desativar.
#define AUDIO_DAC_POWER_DOWN_ON_IDLE 1

// Volume máximo real da saída analógica do ES8388 (dB, -45 a +4,5; 0 = original).
// Com o HT6872 o original (0 dB) é forte demais: 10% no volume já soava alto e o
// som saturava no máximo. -15 dB desloca a escala toda para baixo.
#define AUDIO_ANALOG_OUTPUT_DB -15

// --- Botões e LED -------------------------------------------------------------
#define BOOT_BUTTON_GPIO         GPIO_NUM_0    // BOOT do módulo
#define TALK_BUTTON_GPIO         GPIO_NUM_11   // botão do meio
#define VOLUME_DOWN_BUTTON_GPIO  GPIO_NUM_12   // botão da esquerda
#define VOLUME_UP_BUTTON_GPIO    GPIO_NUM_13   // botão da direita
#define BUILTIN_LED_GPIO         GPIO_NUM_21   // LED verde da placa principal

// --- Display OLED I2C (conector lateral) ---------------------------------------
#define DISPLAY_SDA_PIN GPIO_NUM_17
#define DISPLAY_SCL_PIN GPIO_NUM_18
#define DISPLAY_I2C_ADDR 0x3C
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

// Olhos grandes animados no lugar da carinha (só 128x64). 0 = carinha original.
#define DISPLAY_ROBOT_EYES 1

// Se a imagem aparecer de cabeça para baixo, inverta os dois valores.
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// --- Sensor de presença HLK-LD2401 (alimentar em 3,3 V!) -----------------------
// 0 = placa sem sensor. Se o sensor não responder nos primeiros segundos, a
// placa segue como se sempre houvesse alguém (nada dorme nem apaga).
#define PRESENCE_ENABLED        1
#define PRESENCE_OUT_GPIO       GPIO_NUM_4    // OUT do sensor
#define PRESENCE_UART_NUM       UART_NUM_1
#define PRESENCE_UART_RX_GPIO   GPIO_NUM_6    // TX do sensor
#define PRESENCE_UART_TX_GPIO   GPIO_NUM_5    // RX do sensor
#define PRESENCE_UART_BAUD      256000

// Tempos sem ninguém (segundos). 0 = desliga aquela etapa.
#define PRESENCA_SONOLENTA_S    60    // olhos sonolentos
#define PRESENCA_DORME_S        180   // olhos fechados
#define PRESENCA_TELA_S         300   // tela apaga
#define PRESENCA_RADIO_S        600   // pergunta se tem alguém e, sem resposta, desliga a rádio

#endif // _BOARD_CONFIG_H_
