#ifndef _EYES_OLED_DISPLAY_H_
#define _EYES_OLED_DISPLAY_H_

// ============================================================================
// EyesOledDisplay - OLED 128x64 com dois olhos grandes animados.
//
// Uma camada própria cobre a área abaixo da barra de status (y 16..63) e
// desenha os olhos (piscam e olham em volta sozinhos, mudam com a emoção).
// O texto da conversa aparece numa linha rolando embaixo, e os olhos sobem.
// Nomes que não são emoção (ícones de sistema: aviso, Wi-Fi, download...)
// escondem a camada e o OledDisplay original mostra o ícone, como antes.
// Em displays 128x32 a camada não é criada (fica o layout original).
// ============================================================================

#include "display/oled_display.h"

#include <string>

class EyesOledDisplay : public OledDisplay {
public:
    EyesOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, bool mirror_x, bool mirror_y);
    ~EyesOledDisplay();

    virtual void SetupUI() override;
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;

private:
    struct EyeShape {
        int w;          // largura (px)
        int h;          // altura (px) com a área toda livre
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
    lv_obj_t* lid_top_[2] = {nullptr, nullptr};
    lv_obj_t* lid_bottom_[2] = {nullptr, nullptr};
    lv_obj_t* text_label_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    EyeShape shape_[2] = {};
    bool text_visible_ = false;
    bool allow_blink_ = true;
    bool allow_look_ = true;
    int blink_step_ = 0;
    int look_dx_ = 0;
    int look_dy_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t next_look_ms_ = 0;

    // Pupilas (bolinhas): posição em % do espaço livre dentro do olho (-100..100)
    bool pupil_show_ = true;
    int pupil_base_x_ = 0;     // posição de repouso da emoção
    int pupil_base_y_ = 0;
    int pupil_size_ = 100;     // % do tamanho normal (surpreso = menor)
    int pupil_x_ = 0;          // posição atual
    int pupil_y_ = 0;
    int pupil_tx_ = 0;         // posição alvo (olhadinha)
    int pupil_ty_ = 0;

    bool SelectShapes(const char* emotion);
    void ApplyShape();
    void SetText(const std::string& text);
    static void TimerCallback(lv_timer_t* timer);
    void OnTimer();
};

#endif  // _EYES_OLED_DISPLAY_H_
