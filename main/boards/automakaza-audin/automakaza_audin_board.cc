// ============================================================================
// Automakaza AUDIN - placa ESP_S3-AUDIN RE1.1 (ESP32-S3 N16R8 + ES8388)
//
// - Áudio: Es8388AudioCodec (I2S duplex), com as saídas analógicas do DAC
//   desligadas quando o áudio fica ocioso (o HT6872 não tem enable).
// - Display: OLED I2C SSD1306/SH1106 opcional (SDA GPIO17, SCL GPIO18). Se não
//   houver display ligado, a placa funciona sem ele. Em 128x64 mostra olhos
//   grandes animados (EyesOledDisplay; desligável em DISPLAY_ROBOT_EYES).
// - Botões: BOOT (módulo) e meio = conversar / configurar Wi-Fi
//   (segurar o meio = configuração de Wi-Fi a qualquer momento);
//   esquerda = volume -, direita = volume +.
// - LED: verde da placa principal (GPIO21).
// - Presença: radar HLK-LD2401 (OUT GPIO4, serial GPIO6/5). Sem ninguém por
//   perto ela fica sonolenta, dorme, apaga a tela e pausa a rádio (tempos no
//   config.h). Ferramenta self.presence.get_status para a IA saber a distância.
// ============================================================================

#include "wifi_board.h"
#include "codecs/es8388_audio_codec.h"
#include "display/oled_display.h"
#include "eyes_oled_display.h"
#include "presence_sensor.h"
#include "reminder_manager.h"
#include "mcp_server.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/gpio_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "AutomakazaAudin"

// ES8388 com desligamento das saídas analógicas do DAC em repouso.
// No Es8388AudioCodec original, EnableOutput(false) só aplica mudo digital; o
// DAC continua ligado e, como o amplificador desta placa fica sempre ligado,
// sobra um chiado. Aqui o registrador 0x04 (DACPOWER) vai a 0xC0 em repouso e
// volta a 0x3C antes de tocar.
class AudinAudioCodec : public Es8388AudioCodec {
public:
    AudinAudioCodec(i2c_master_bus_handle_t i2c_bus, int input_sample_rate, int output_sample_rate)
        : Es8388AudioCodec((void*)i2c_bus, I2C_NUM_0, input_sample_rate, output_sample_rate,
                           AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                           AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                           AUDIO_CODEC_ES8388_ADDR, AUDIO_INPUT_REFERENCE) {
        // Volume inicial depois de gravar firmware novo (a gravação completa
        // apaga o volume salvo). Depois disso vale o último volume ajustado.
        output_volume_ = AUDIO_DEFAULT_OUTPUT_VOLUME;
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = AUDIO_CODEC_ES8388_ADDR_7BIT;
        dev_cfg.scl_speed_hz = 100 * 1000;
        if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dac_dev_) != ESP_OK) {
            ESP_LOGW(TAG, "Sem acesso direto ao ES8388; DAC sempre ligado e volume analógico padrão");
            dac_dev_ = nullptr;
        }
    }

    virtual void EnableOutput(bool enable) override {
        if (enable) {
            SetDacPower(true);
            Es8388AudioCodec::EnableOutput(true);
            // O Es8388AudioCodec põe a saída analógica em 0 dB; com o HT6872 isso
            // fica alto demais (10% já era forte). Abaixa a escala inteira.
            SetAnalogOutput(AUDIO_ANALOG_OUTPUT_DB);
        } else {
            Es8388AudioCodec::EnableOutput(false);
            SetDacPower(false);
        }
    }

private:
    i2c_master_dev_handle_t dac_dev_ = nullptr;

    // Volume analógico das saídas (regs 46..49 = LOUT1/ROUT1/LOUT2/ROUT2):
    // 0..33 = -45 dB .. +4,5 dB, passos de 1,5 dB (30 = 0 dB).
    void SetAnalogOutput(float db) {
        if (dac_dev_ == nullptr) {
            return;
        }
        int val = 30 + static_cast<int>(db / 1.5f);
        if (val < 0) val = 0;
        if (val > 33) val = 33;
        for (uint8_t reg = 46; reg <= 49; reg++) {
            uint8_t buf[2] = {reg, static_cast<uint8_t>(val)};
            i2c_master_transmit(dac_dev_, buf, sizeof(buf), 100);
        }
    }

    void SetDacPower(bool on) {
#if AUDIO_DAC_POWER_DOWN_ON_IDLE
        if (dac_dev_ == nullptr) {
            return;
        }
        uint8_t buf[2] = {0x04, static_cast<uint8_t>(on ? 0x3C : 0xC0)};
        esp_err_t err = i2c_master_transmit(dac_dev_, buf, sizeof(buf), 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao escrever DACPOWER: %s", esp_err_to_name(err));
        }
#else
        (void)on;
#endif
    }
};

class AutomakazaAudinBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    EyesOledDisplay* eyes_ = nullptr;
    PresenceSensor* presence_ = nullptr;
    ReminderManager lembretes_;
    int ausente_s_ = 0;          // segundos sem ninguém
    bool tela_apagada_ = false;
    bool ausente_enviado_ = false;  // avisou o servidor (rádio) e espera a volta
    Button boot_button_;
    Button talk_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &codec_i2c_bus_));
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeOledDisplay() {
        // Display é opcional: sem resposta em 0x3C, segue sem tela.
        if (i2c_master_probe(display_i2c_bus_, DISPLAY_I2C_ADDR, 100) != ESP_OK) {
            ESP_LOGW(TAG, "OLED não encontrado em 0x%02X; seguindo sem display", DISPLAY_I2C_ADDR);
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = DISPLAY_I2C_ADDR,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao iniciar o OLED; seguindo sem display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

#if DISPLAY_ROBOT_EYES
        eyes_ = new EyesOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = eyes_;
#else
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#endif
    }

    void ToggleChatOrConfigureWifi() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() { ToggleChatOrConfigureWifi(); });
        talk_button_.OnClick([this]() { ToggleChatOrConfigureWifi(); });
        // Segurar o botão do meio: entra na configuração de Wi-Fi a qualquer
        // momento (para trocar de rede ou o endereço OTA do servidor).
        talk_button_.OnLongPress([this]() { EnterWifiConfigMode(); });

        volume_up_button_.OnClick([this]() { ChangeVolume(10); });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() { ChangeVolume(-10); });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // ---------------------------------------------------------------- presença
    void InitializePresence() {
#if PRESENCE_ENABLED
        presence_ = new PresenceSensor(PRESENCE_OUT_GPIO, PRESENCE_UART_NUM, PRESENCE_UART_RX_GPIO,
                                       PRESENCE_UART_TX_GPIO, PRESENCE_UART_BAUD);
        presence_->Start();
        McpServer::GetInstance().AddTool(
            "self.presence.get_status",
            "Radar de presença do aparelho: diz se tem alguém por perto, se está se movendo ou "
            "parado e a distância em centímetros. Use quando não entender bem a fala da pessoa "
            "(se distancia_cm > 150, peça para ela chegar mais perto) ou quando perguntarem se "
            "tem alguém na sala.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                return presence_->ToJson();
            });
        xTaskCreate([](void* arg) {
            static_cast<AutomakazaAudinBoard*>(arg)->PresenceLoop();
        }, "presenca_ctl", 4096, this, 2, nullptr);
#endif
    }

    void Acordar() {
        if (eyes_ != nullptr) {
            eyes_->SetSono(0);
        }
        if (tela_apagada_ && panel_ != nullptr) {
            esp_lcd_panel_disp_on_off(panel_, true);
            tela_apagada_ = false;
            ESP_LOGI(TAG, "Presença: tela acesa");
        }
    }

    void PresenceLoop() {
        // espera o sensor dar sinal de vida
        vTaskDelay(pdMS_TO_TICKS(8000));
        if (!presence_->Detected()) {
            ESP_LOGW(TAG, "Sensor de presença não respondeu: seguindo sem ele");
            vTaskDelete(nullptr);
            return;
        }
        ESP_LOGI(TAG, "Sensor de presença ativo");
        auto& app = Application::GetInstance();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            auto st = app.GetDeviceState();
            bool presente = presence_->Get().present;
            // falar com ela (palavra de ativação, botão) também conta como presença
            bool interacao = (st == kDeviceStateListening || st == kDeviceStateConnecting ||
                              st == kDeviceStateWifiConfiguring || st == kDeviceStateUpgrading);

            if (presente || interacao) {
                if (ausente_s_ >= 60) {
                    ESP_LOGI(TAG, "Presença: alguém chegou depois de %d s", ausente_s_);
                }
                if (ausente_s_ > 0) {
                    Acordar();
                }
                ausente_s_ = 0;
                if (presente && ausente_enviado_) {
                    ausente_enviado_ = false;
                    app.SendPresenceEvent("voltou");
                }
                continue;
            }

            ausente_s_++;
            if (PRESENCA_SONOLENTA_S > 0 && ausente_s_ == PRESENCA_SONOLENTA_S && eyes_ != nullptr) {
                ESP_LOGI(TAG, "Presença: ninguém há %d s, sonolenta", ausente_s_);
                eyes_->SetSono(1);
            }
            if (PRESENCA_DORME_S > 0 && ausente_s_ == PRESENCA_DORME_S && eyes_ != nullptr) {
                ESP_LOGI(TAG, "Presença: ninguém há %d s, dormindo", ausente_s_);
                eyes_->SetSono(2);
            }
            if (PRESENCA_TELA_S > 0 && ausente_s_ == PRESENCA_TELA_S && panel_ != nullptr &&
                !tela_apagada_) {
                ESP_LOGI(TAG, "Presença: ninguém há %d s, tela apagada", ausente_s_);
                esp_lcd_panel_disp_on_off(panel_, false);
                tela_apagada_ = true;
            }
            if (PRESENCA_RADIO_S > 0 && ausente_s_ == PRESENCA_RADIO_S &&
                st == kDeviceStateSpeaking && !ausente_enviado_) {
                ESP_LOGI(TAG, "Presença: ninguém há %d s com áudio tocando, avisando o servidor",
                         ausente_s_);
                ausente_enviado_ = true;
                app.SendPresenceEvent("ausente");
            }
        }
    }

public:
    AutomakazaAudinBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        talk_button_(TALK_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeDisplayI2c();
        InitializeOledDisplay();
        InitializeButtons();
        InitializePresence();
        // Lembretes: o servidor manda a lista; a placa guarda e dispara na hora
        lembretes_.Start();
        Application::GetInstance().SetCustomJsonHandler(
            [this](const std::string& tipo, const std::string& json) {
                if (tipo == "lembretes") {
                    lembretes_.SetFromJson(json);
                }
            });
    }

    virtual Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static AudinAudioCodec audio_codec(codec_i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(AutomakazaAudinBoard);
