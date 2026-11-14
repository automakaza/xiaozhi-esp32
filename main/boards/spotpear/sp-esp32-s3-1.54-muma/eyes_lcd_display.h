#ifndef _EYES_LCD_DISPLAY_H_
#define _EYES_LCD_DISPLAY_H_

// ============================================================================
// EyesLcdDisplay - olhos animados da Sophia no LCD colorido 240x240 (ST7789).
//
// Versão colorida dos olhos da AUDIN (EyesOledDisplay): mesma lógica de
// emoções, piscadas, olhadinhas e pupilas, só que bem maiores e em cor.
// Uma camada própria cobre a tela por baixo das barras de status e de texto
// do LcdDisplay; quando a conversa mostra texto embaixo, os olhos sobem e
// encolhem um pouco. Nomes que não são emoção (ícones de sistema) escondem
// a camada e o LcdDisplay original mostra o ícone, como antes.
// As formas continuam descritas na escala do OLED (128x48) e são ampliadas
// em ApplyShape().
// ============================================================================

#include "display/lcd_display.h"

#include <functional>
#include <string>

class EyesLcdDisplay : public SpiLcdDisplay {
public:
    EyesLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
    ~EyesLcdDisplay();

    virtual void SetupUI() override;
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;

    // Sono: 0 = acordada, 1 = sonolenta (pálpebras meio fechadas),
    // 2 = dormindo (olhos fechados).
    void SetSono(int nivel);

    // Cor base dos olhos (definida pela placa conforme o estado: ouvindo,
    // pensando, falando...). A troca é suave. Durante a música os olhos
    // ficam passeando pelas cores do arco-íris; dormindo, ficam azul-escuros.
    void SetBaseColor(uint32_t rgb);

    // Chamado quando chega o texto do que a pessoa falou ("entendi")
    void SetOnUserText(std::function<void()> callback) { on_user_text_ = std::move(callback); }

private:
    struct EyeShape {
        int w;          // largura (px na escala do OLED)
        int h;          // altura (px na escala do OLED)
        int r;          // raio dos cantos
        int dx;         // deslocamento horizontal
        int dy;         // deslocamento vertical
        int lid_top;    // pálpebra de cima (px cobertos)
        int tilt;       // inclinação da pálpebra (0,1°): + caída para fora (triste),
                        // - caída para dentro (bravo)
        bool happy;     // corte embaixo em arco (olho "sorrindo")
    };

    lv_obj_t* layer_ = nullptr;
    lv_obj_t* eye_[2] = {nullptr, nullptr};
    lv_obj_t* pupil_[2] = {nullptr, nullptr};
    lv_obj_t* shine_[2] = {nullptr, nullptr};
    lv_obj_t* lid_top_[2] = {nullptr, nullptr};
    lv_obj_t* lid_bottom_[2] = {nullptr, nullptr};
    lv_timer_t* timer_ = nullptr;

    EyeShape shape_[2] = {};
    int sono_ = 0;
    int acordar_fase_ = 0;          // reação ao acordar: susto -> olha para os lados
    uint32_t acordar_ms_ = 0;
    uint32_t piscadinha_ms_ = 0;        // piscadinha é passageira: depois volta
    std::string piscadinha_volta_ = "neutral";
    std::string last_emotion_ = "neutral";
    bool text_visible_ = false;
    bool allow_blink_ = true;
    bool allow_look_ = true;
    bool dancing_ = false;          // emoção "music": balança no ritmo
    int dance_step_ = 0;
    uint32_t next_dance_ms_ = 0;
    std::string music_text_;
    std::function<void()> on_user_text_;
    uint32_t base_color_ = 0x8EE6F2;  // alvo da cor (estado)
    uint32_t cor_atual_ = 0x8EE6F2;   // cor mostrada agora (vai até o alvo)
    int matiz_ = 0;                   // arco-íris da música (0..359)        // nome da música (volta quando a conversa limpa a tela)
    int blink_step_ = 0;
    int look_dx_ = 0;
    int look_dy_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t next_look_ms_ = 0;

    // Pupilas: posição em % do espaço livre dentro do olho (-100..100)
    bool pupil_show_ = true;
    int pupil_base_x_ = 0;
    int pupil_base_y_ = 0;
    int pupil_size_ = 100;
    int pupil_x_ = 0;
    int pupil_y_ = 0;
    int pupil_tx_ = 0;
    int pupil_ty_ = 0;

    bool SelectShapes(const char* emotion);
    void ApplyShape();
    void UpdateTextVisible();
    void RestoreMusicText();
    void UpdateColor();
    static void TimerCallback(lv_timer_t* timer);
    void OnTimer();
};

#endif  // _EYES_LCD_DISPLAY_H_
