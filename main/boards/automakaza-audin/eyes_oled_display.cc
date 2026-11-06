#include "eyes_oled_display.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_random.h>

#define TAG "EyesOledDisplay"

// Área dos olhos: abaixo da barra de status de 16 px do OledDisplay.
static const int kLayerY = 16;
static const int kLayerH = 48;
// Com texto visível, os olhos ficam na parte de cima e a linha de texto embaixo.
static const int kTextY = 32;
static const int kEyeGap = 22;

// Altura relativa durante a piscada (em %).
static const int kBlinkScale[] = {100, 55, 8, 55, 100};
static const int kBlinkSteps = sizeof(kBlinkScale) / sizeof(kBlinkScale[0]);

static uint32_t RandomBetween(uint32_t min, uint32_t max) {
    return min + (esp_random() % (max - min + 1));
}

EyesOledDisplay::EyesOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, bool mirror_x, bool mirror_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {
    SelectShapes("neutral");
}

EyesOledDisplay::~EyesOledDisplay() {
    DisplayLockGuard lock(this);
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (layer_ != nullptr) {
        lv_obj_delete(layer_);
        layer_ = nullptr;
    }
}

static void StyleBox(lv_obj_t* obj, lv_color_t color, int radius) {
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

void EyesOledDisplay::SetupUI() {
    OledDisplay::SetupUI();
    if (height_ != 64) {
        return;  // 128x32: mantém o layout original
    }

    DisplayLockGuard lock(this);
    if (layer_ != nullptr) {
        return;
    }

    // No OLED monocromático: branco = pixel apagado (fundo), preto = aceso.
    layer_ = lv_obj_create(lv_screen_active());
    StyleBox(layer_, lv_color_white(), 0);
    lv_obj_set_size(layer_, width_, kLayerH);
    lv_obj_set_pos(layer_, 0, kLayerY);

    for (int i = 0; i < 2; i++) {
        eye_[i] = lv_obj_create(layer_);
        StyleBox(eye_[i], lv_color_black(), 8);

        // Pupila: bolinha "apagada" dentro do olho aceso
        pupil_[i] = lv_obj_create(eye_[i]);
        StyleBox(pupil_[i], lv_color_white(), LV_RADIUS_CIRCLE);

        lid_top_[i] = lv_obj_create(eye_[i]);
        StyleBox(lid_top_[i], lv_color_white(), 0);
        lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);

        lid_bottom_[i] = lv_obj_create(eye_[i]);
        StyleBox(lid_bottom_[i], lv_color_white(), LV_RADIUS_CIRCLE);
        lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
    }

    text_label_ = lv_label_create(layer_);
    lv_obj_set_width(text_label_, width_);
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(text_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(text_label_, lv_color_black(), 0);
    lv_obj_set_style_anim_duration(text_label_, lv_anim_speed_clamped(60, 300, 60000), 0);
    lv_obj_set_pos(text_label_, 0, kTextY);
    lv_label_set_text(text_label_, "");
    lv_obj_add_flag(text_label_, LV_OBJ_FLAG_HIDDEN);

    // Fica escondida até chegar uma emoção conhecida (na partida o OledDisplay
    // mostra o ícone de robô e o status).
    lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);

    uint32_t now = lv_tick_get();
    next_blink_ms_ = now + RandomBetween(2000, 4000);
    next_look_ms_ = now + RandomBetween(2000, 4000);
    ApplyShape();

    timer_ = lv_timer_create(TimerCallback, 50, this);
}

bool EyesOledDisplay::SelectShapes(const char* emotion) {
    if (emotion == nullptr) {
        return false;
    }
    auto is = [emotion](const char* name) { return strcmp(emotion, name) == 0; };

    //                     w   h   r   dx  dy lid tilt happy
    const EyeShape normal = {36, 30, 9, 0, 0, 0, 0, false};
    EyeShape left = normal;
    EyeShape right = normal;
    allow_blink_ = true;
    allow_look_ = false;
    pupil_show_ = true;
    pupil_base_x_ = 0;
    pupil_base_y_ = 0;
    pupil_size_ = 100;

    if (is("neutral") || is("relaxed")) {
        allow_look_ = true;
    } else if (is("happy") || is("laughing") || is("funny") || is("loving") ||
               is("delicious") || is("kissy") || is("silly")) {
        left.happy = right.happy = true;
        allow_blink_ = false;
        pupil_show_ = false;  // olho em arco "∩" não tem bolinha
    } else if (is("sad") || is("crying")) {
        left = right = {34, 26, 9, 0, 4, 9, 160, false};
        pupil_base_y_ = 70;   // olhar para baixo
    } else if (is("angry")) {
        left = right = {34, 28, 7, 0, 2, 12, -250, false};
        pupil_base_y_ = 20;
    } else if (is("surprised") || is("shocked")) {
        left = right = {32, 32, LV_RADIUS_CIRCLE, 0, 0, 0, 0, false};
        pupil_size_ = 70;     // pupila pequena de susto
    } else if (is("thinking")) {
        left = {34, 26, 9, 8, -7, 0, 0, false};
        right = {34, 20, 9, 8, -7, 0, 0, false};
        pupil_base_x_ = 80;   // olhando para cima e para o lado
        pupil_base_y_ = -90;
    } else if (is("confused")) {
        left = {30, 18, 7, -4, -2, 0, 0, false};
        right = {36, 30, 9, -4, 0, 0, 0, false};
        pupil_base_x_ = -70;
        pupil_base_y_ = -40;
    } else if (is("sleepy")) {
        left = right = {36, 4, 2, 0, 6, 0, 0, false};
        allow_blink_ = false;
        pupil_show_ = false;
    } else if (is("winking")) {
        right = {36, 4, 2, 0, 4, 0, 0, false};
        allow_blink_ = false;
    } else if (is("embarrassed")) {
        left = right = {32, 22, 8, 0, 8, 0, 0, false};
        pupil_base_x_ = -60;  // olhar de lado, sem graça
        pupil_base_y_ = 80;
    } else if (is("cool") || is("confident")) {
        left = right = {38, 26, 6, 0, 2, 12, 0, false};
        pupil_base_y_ = 30;
    } else {
        return false;  // não é emoção (ícone de sistema)
    }

    // Sem ninguém por perto há um tempo: o sono vence a emoção
    if (sono_ == 1) {
        left = right = {36, 12, 5, 0, 7, 0, 0, false};
        allow_blink_ = true;
        allow_look_ = false;
        pupil_show_ = true;
        pupil_base_x_ = 0;
        pupil_base_y_ = 60;  // olhar caído
        pupil_size_ = 100;
    } else if (sono_ >= 2) {
        left = right = {36, 3, 1, 0, 9, 0, 0, false};
        allow_blink_ = false;
        allow_look_ = false;
        pupil_show_ = false;
    }

    shape_[0] = left;
    shape_[1] = right;
    blink_step_ = 0;
    pupil_tx_ = pupil_base_x_;
    pupil_ty_ = pupil_base_y_;
    if (!allow_look_) {
        look_dx_ = 0;
        look_dy_ = 0;
    }
    return true;
}

void EyesOledDisplay::ApplyShape() {
    if (layer_ == nullptr) {
        return;
    }
    const int area_h = text_visible_ ? kTextY : kLayerH;
    const int center_y = area_h / 2;
    const int scale_area = text_visible_ ? 70 : 100;  // olhos menores com texto
    const int blink = kBlinkScale[blink_step_];

    for (int i = 0; i < 2; i++) {
        const EyeShape& s = shape_[i];
        int w = s.w;
        int h = std::max(2, s.h * scale_area / 100 * blink / 100);
        int r = (s.r == LV_RADIUS_CIRCLE) ? h / 2 : std::min(s.r, h / 2);

        int center_x = (i == 0) ? (width_ / 2 - kEyeGap / 2 - s.w / 2)
                                : (width_ / 2 + kEyeGap / 2 + s.w / 2);
        center_x += s.dx + look_dx_;
        int y = center_y - h / 2 + s.dy * scale_area / 100 + look_dy_;
        y = std::max(0, std::min(y, area_h - h));
        int x = std::max(0, std::min(center_x - w / 2, width_ - w));

        lv_obj_set_pos(eye_[i], x, y);
        lv_obj_set_size(eye_[i], w, h);
        lv_obj_set_style_radius(eye_[i], r, 0);

        // Pupila: elipse em pé, que anda dentro da parte VISÍVEL do olho
        // (abaixo da pálpebra, quando ela existe).
        {
            const int full_h = s.h * scale_area / 100;  // altura sem piscada
            int lid_px = (blink == 100) ? s.lid_top * scale_area / 100 : 0;
            if (s.tilt != 0 && lid_px > 0) {
                lid_px += 3;  // pálpebra inclinada desce mais num dos cantos
            }
            const int margem = 2;
            int topo = std::min(h, lid_px + margem);   // primeira linha livre
            int fundo = h - margem;                      // última linha livre
            int livre_h = fundo - topo;
            int pw = std::max(4, w * 25 / 100 * pupil_size_ / 100);
            int ph = std::max(4, full_h * 42 / 100 * pupil_size_ / 100);
            ph = std::min(ph, livre_h - 2);
            pw = std::min(pw, std::max(4, ph + 2));      // não fica mais largo que alto+2
            if (!pupil_show_ || s.happy || h < 8 || ph < 4) {
                lv_obj_add_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                int livre_x = std::max(0, (w - pw) / 2 - margem);
                int livre_y = std::max(0, (livre_h - ph) / 2);
                // Nos cantos (diagonal) anda menos, para não "furar" a borda
                // arredondada do olho (sem recorte de cantos, que pesa no LVGL).
                int ox = pupil_x_, oy = pupil_y_;
                if (std::abs(ox) > 50 && std::abs(oy) > 50) {
                    ox = ox * 70 / 100;
                    oy = oy * 70 / 100;
                }
                int px = (w - pw) / 2 + livre_x * ox / 100;
                int py = topo + (livre_h - ph) / 2 + livre_y * oy / 100;
                lv_obj_set_size(pupil_[i], pw, ph);
                lv_obj_set_pos(pupil_[i], px, py);
                lv_obj_remove_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Pálpebra de cima (olho meio fechado), podendo inclinar: a borda de
        // baixo da pálpebra gira em torno do próprio meio. Some na piscada.
        int lid = s.lid_top * scale_area / 100;
        if (lid > 0 && blink == 100 && lid < h) {
            const int lid_w = w + 24;
            const int lid_h = h;
            int angle = (i == 0) ? -s.tilt : s.tilt;
            lv_obj_set_size(lid_top_[i], lid_w, lid_h);
            lv_obj_set_pos(lid_top_[i], -12, lid - lid_h);
            lv_obj_set_style_transform_pivot_x(lid_top_[i], lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(lid_top_[i], lid_h, 0);
            lv_obj_set_style_transform_rotation(lid_top_[i], angle, 0);
            lv_obj_remove_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Olho "sorrindo": um círculo apagado sobe por baixo e deixa um arco "∩".
        if (s.happy && h > 8) {
            const int d = w + 12;
            lv_obj_set_pos(lid_bottom_[i], -6, h * 42 / 100);
            lv_obj_set_size(lid_bottom_[i], d, d);
            lv_obj_remove_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void EyesOledDisplay::SetText(const std::string& text) {
    if (text_label_ == nullptr) {
        return;
    }
    if (text.empty()) {
        lv_label_set_text(text_label_, "");
        lv_obj_add_flag(text_label_, LV_OBJ_FLAG_HIDDEN);
        text_visible_ = false;
    } else {
        lv_label_set_text(text_label_, text.c_str());
        lv_obj_remove_flag(text_label_, LV_OBJ_FLAG_HIDDEN);
        text_visible_ = true;
    }
    ApplyShape();
}

bool EyesOledDisplay::SetEmotion(const char* emotion) {
    bool known = false;
    {
        DisplayLockGuard lock(this);
        if (layer_ != nullptr) {
            known = SelectShapes(emotion);
            if (known) {
                last_emotion_ = emotion;
                lv_obj_remove_flag(layer_, LV_OBJ_FLAG_HIDDEN);
                ApplyShape();
            } else {
                lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    // Mantém o OledDisplay em dia (fica por baixo da camada dos olhos).
    bool shown = OledDisplay::SetEmotion(emotion);
    return known || shown;
}

void EyesOledDisplay::SetSono(int nivel) {
    DisplayLockGuard lock(this);
    if (nivel == sono_) {
        return;
    }
    sono_ = nivel;
    if (layer_ == nullptr || lv_obj_has_flag(layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;  // há um ícone de sistema na tela; vale na próxima emoção
    }
    SelectShapes(last_emotion_.c_str());
    ApplyShape();
}

void EyesOledDisplay::SetChatMessage(const char* role, const char* content) {
    OledDisplay::SetChatMessage(role, content);
    DisplayLockGuard lock(this);
    std::string text = (content != nullptr) ? content : "";
    std::replace(text.begin(), text.end(), '\n', ' ');
    SetText(text);
}

void EyesOledDisplay::RemoveChatMessage(const char* role, const char* content) {
    OledDisplay::RemoveChatMessage(role, content);
    DisplayLockGuard lock(this);
    if (text_label_ == nullptr || content == nullptr) {
        return;
    }
    std::string text = content;
    std::replace(text.begin(), text.end(), '\n', ' ');
    if (text == lv_label_get_text(text_label_)) {
        SetText("");
    }
}

void EyesOledDisplay::TimerCallback(lv_timer_t* timer) {
    auto self = static_cast<EyesOledDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnTimer();
    }
}

// Roda na tarefa do LVGL (já com a trava do display).
void EyesOledDisplay::OnTimer() {
    if (layer_ == nullptr || lv_obj_has_flag(layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    uint32_t now = lv_tick_get();
    bool changed = false;

    if (blink_step_ > 0) {
        blink_step_++;
        if (blink_step_ >= kBlinkSteps) {
            blink_step_ = 0;
        }
        changed = true;
    } else if (allow_blink_ && now >= next_blink_ms_) {
        blink_step_ = 1;
        // Às vezes pisca duas vezes seguidas.
        next_blink_ms_ = now + (RandomBetween(0, 5) == 0 ? 350 : RandomBetween(2500, 6000));
        changed = true;
    }

    if (allow_look_ && blink_step_ == 0 && now >= next_look_ms_) {
        // As pupilas olham para um lado; o olho acompanha só um pouquinho.
        if (RandomBetween(0, 3) == 0) {
            pupil_tx_ = 0;  // volta a olhar para frente
            pupil_ty_ = 0;
        } else {
            // posições bem marcadas: lados, cantos, cima e baixo
            static const int kX[] = {-100, -100, -60, 60, 100, 100};
            static const int kY[] = {-80, -40, 0, 0, 60};
            pupil_tx_ = kX[RandomBetween(0, 5)];
            pupil_ty_ = kY[RandomBetween(0, 4)];
        }
        look_dx_ = pupil_tx_ * 4 / 100;
        look_dy_ = pupil_ty_ * 3 / 100;
        next_look_ms_ = now + RandomBetween(1200, 3800);
        changed = true;
    }

    // Movimento rápido mas suave das pupilas até o alvo (como um olhar de verdade)
    if (pupil_x_ != pupil_tx_ || pupil_y_ != pupil_ty_) {
        auto passo = [](int atual, int alvo) {
            int d = alvo - atual;
            if (d == 0) return atual;
            int p = d / 2;
            if (p == 0) p = (d > 0) ? 1 : -1;
            return atual + p;
        };
        pupil_x_ = passo(pupil_x_, pupil_tx_);
        pupil_y_ = passo(pupil_y_, pupil_ty_);
        changed = true;
    }

    if (changed) {
        ApplyShape();
    }
}
