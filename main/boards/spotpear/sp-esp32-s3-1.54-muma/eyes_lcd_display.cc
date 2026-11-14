#include "eyes_lcd_display.h"

#include "settings.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_random.h>

#define TAG "EyesLcdDisplay"

// ----------------------------------------------------------------------------
// Aparência (ajuste à vontade)
// ----------------------------------------------------------------------------
static const uint32_t kCorFundo = 0x000000;   // fundo da tela
static const uint32_t kCorOlho = 0x8EE6F2;    // ciano claro, como o rosto de fábrica
static const uint32_t kCorPupila = 0x08202A;  // pupila escura
static const uint32_t kCorBrilho = 0xFFFFFF;  // reflexo na pupila

// Ampliação das formas do OLED (em %): largura, altura e raio dos cantos.
static const int kEscalaX = 230;
static const int kEscalaY = 290;
static const int kEscalaR = 260;
static const int kVaoOlhos = 36;     // espaço entre os olhos (px reais)

// Área dos olhos na tela (px reais)
static const int kTopo = 30;         // abaixo da barra de status
static const int kRodapeLivre = 10;  // margem embaixo sem texto
static const int kRodapeTexto = 42;  // altura reservada à barra de texto

// Altura relativa durante a piscada (em %).
static const int kBlinkScale[] = {100, 55, 8, 55, 100};
static const int kBlinkSteps = sizeof(kBlinkScale) / sizeof(kBlinkScale[0]);

static inline int EX(int v) { return v * kEscalaX / 100; }
static inline int EY(int v) { return v * kEscalaY / 100; }
static inline int ER(int v) { return v * kEscalaR / 100; }

static uint32_t RandomBetween(uint32_t min, uint32_t max) {
    return min + (esp_random() % (max - min + 1));
}

// Mistura duas cores RGB (t de 0 a 100)
static uint32_t MisturaCor(uint32_t a, uint32_t b, int t) {
    auto canal = [t](uint32_t x, uint32_t y, int sh) {
        int ca = (x >> sh) & 0xFF, cb = (y >> sh) & 0xFF;
        return (uint32_t)(ca + (cb - ca) * t / 100) << sh;
    };
    return canal(a, b, 16) | canal(a, b, 8) | canal(a, b, 0);
}

// Matiz (0..359) -> RGB pastel (saturação ~45%, brilho máximo)
static uint32_t CorArcoIris(int h) {
    const int v = 255, minimo = 140;  // pastel: nenhum canal abaixo de 140
    int setor = (h / 60) % 6, f = (h % 60) * 255 / 60;
    int sobe = minimo + (v - minimo) * f / 255;
    int desce = v - (v - minimo) * f / 255;
    int r, g, b;
    switch (setor) {
        case 0: r = v; g = sobe; b = minimo; break;
        case 1: r = desce; g = v; b = minimo; break;
        case 2: r = minimo; g = v; b = sobe; break;
        case 3: r = minimo; g = desce; b = v; break;
        case 4: r = sobe; g = minimo; b = v; break;
        default: r = v; g = minimo; b = desce; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static const uint32_t kCorDormindo = 0x3A6EA5;

EyesLcdDisplay::EyesLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    // Tema escuro por padrão (combina com o fundo preto dos olhos), mas
    // respeita o tema que o usuário escolher depois.
    Settings settings("display", false);
    auto theme = LvglThemeManager::GetInstance().GetTheme(settings.GetString("theme", "dark"));
    if (theme != nullptr) {
        current_theme_ = theme;
    }
    SelectShapes("neutral");
}

EyesLcdDisplay::~EyesLcdDisplay() {
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

void EyesLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    if (layer_ != nullptr) {
        return;
    }

    layer_ = lv_obj_create(lv_screen_active());
    StyleBox(layer_, lv_color_hex(kCorFundo), 0);
    lv_obj_set_size(layer_, width_, height_);
    lv_obj_set_pos(layer_, 0, 0);

    for (int i = 0; i < 2; i++) {
        eye_[i] = lv_obj_create(layer_);
        StyleBox(eye_[i], lv_color_hex(kCorOlho), ER(9));

        pupil_[i] = lv_obj_create(eye_[i]);
        StyleBox(pupil_[i], lv_color_hex(kCorPupila), LV_RADIUS_CIRCLE);

        // Reflexo: pontinho branco no canto da pupila (dá "vida" ao olhar)
        shine_[i] = lv_obj_create(pupil_[i]);
        StyleBox(shine_[i], lv_color_hex(kCorBrilho), LV_RADIUS_CIRCLE);

        lid_top_[i] = lv_obj_create(eye_[i]);
        StyleBox(lid_top_[i], lv_color_hex(kCorFundo), 0);
        lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);

        lid_bottom_[i] = lv_obj_create(eye_[i]);
        StyleBox(lid_bottom_[i], lv_color_hex(kCorFundo), LV_RADIUS_CIRCLE);
        lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Fica logo acima do emoji padrão e abaixo da imagem de prévia, das
    // barras de status/texto e do aviso de bateria fraca.
    if (emoji_box_ != nullptr) {
        lv_obj_move_to_index(layer_, lv_obj_get_index(emoji_box_) + 1);
    }

    // Escondida até chegar uma emoção conhecida (na partida o LcdDisplay
    // mostra o ícone de robô e o status).
    lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);

    uint32_t now = lv_tick_get();
    next_blink_ms_ = now + RandomBetween(2000, 4000);
    next_look_ms_ = now + RandomBetween(2000, 4000);
    ApplyShape();

    timer_ = lv_timer_create(TimerCallback, 50, this);
}

bool EyesLcdDisplay::SelectShapes(const char* emotion) {
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
    dancing_ = false;
    pupil_show_ = true;
    pupil_base_x_ = 0;
    pupil_base_y_ = 0;
    pupil_size_ = 100;

    if (is("neutral") || is("relaxed")) {
        allow_look_ = true;
    } else if (is("music")) {
        // Curtindo a música: olhos sorrindo que balançam no ritmo (OnTimer)
        left.happy = right.happy = true;
        allow_blink_ = false;
        pupil_show_ = false;
        dancing_ = true;
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

    if (sono_ >= 1) {
        dancing_ = false;
    }
    if (sono_ == 1) {
        left = right = {36, 12, 5, 0, 7, 0, 0, false};
        allow_blink_ = true;
        allow_look_ = false;
        pupil_show_ = true;
        pupil_base_x_ = 0;
        pupil_base_y_ = 60;
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

void EyesLcdDisplay::ApplyShape() {
    if (layer_ == nullptr) {
        return;
    }
    const int area_top = kTopo;
    const int area_bottom = height_ - (text_visible_ ? kRodapeTexto : kRodapeLivre);
    const int center_y = (area_top + area_bottom) / 2;
    const int scale_area = text_visible_ ? 80 : 100;  // olhos menores com texto
    const int blink = kBlinkScale[blink_step_];

    for (int i = 0; i < 2; i++) {
        const EyeShape& s = shape_[i];
        const int full_w = EX(s.w);
        const int full_h = EY(s.h) * scale_area / 100;  // altura sem piscada
        int w = full_w;
        int h = std::max(4, full_h * blink / 100);
        int r = (s.r == LV_RADIUS_CIRCLE) ? h / 2 : std::min(ER(s.r), h / 2);

        int center_x = (i == 0) ? (width_ / 2 - kVaoOlhos / 2 - full_w / 2)
                                : (width_ / 2 + kVaoOlhos / 2 + full_w / 2);
        center_x += EX(s.dx + look_dx_);
        int y = center_y - h / 2 + EY(s.dy) * scale_area / 100 + EY(look_dy_);
        y = std::max(area_top, std::min(y, area_bottom - h));
        int x = std::max(0, std::min(center_x - w / 2, width_ - w));

        lv_obj_set_pos(eye_[i], x, y);
        lv_obj_set_size(eye_[i], w, h);
        lv_obj_set_style_radius(eye_[i], r, 0);

        // Pupila: elipse em pé que anda dentro da parte VISÍVEL do olho
        {
            int lid_px = (blink == 100) ? EY(s.lid_top) * scale_area / 100 : 0;
            if (s.tilt != 0 && lid_px > 0) {
                lid_px += EY(3);  // pálpebra inclinada desce mais num dos cantos
            }
            const int margem = 5;
            int topo = std::min(h, lid_px + margem);
            int fundo = h - margem;
            int livre_h = fundo - topo;
            int pw = std::max(8, w * 34 / 100 * pupil_size_ / 100);
            int ph = std::max(8, full_h * 46 / 100 * pupil_size_ / 100);
            ph = std::min(ph, livre_h - 4);
            pw = std::min(pw, std::max(8, ph + 6));
            if (!pupil_show_ || s.happy || h < 20 || ph < 10) {
                lv_obj_add_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                int livre_x = std::max(0, (w - pw) / 2 - margem);
                int livre_y = std::max(0, (livre_h - ph) / 2);
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

                int sd = std::max(3, pw / 4);
                lv_obj_set_size(shine_[i], sd, sd);
                lv_obj_set_pos(shine_[i], pw * 55 / 100, ph * 18 / 100);
            }
        }

        // Pálpebra de cima (olho meio fechado), podendo inclinar. Some na piscada.
        int lid = EY(s.lid_top) * scale_area / 100;
        if (lid > 0 && blink == 100 && lid < h) {
            const int lid_w = w + EX(24);
            const int lid_h = h;
            int angle = (i == 0) ? -s.tilt : s.tilt;
            lv_obj_set_size(lid_top_[i], lid_w, lid_h);
            lv_obj_set_pos(lid_top_[i], -EX(12), lid - lid_h);
            lv_obj_set_style_transform_pivot_x(lid_top_[i], lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(lid_top_[i], lid_h, 0);
            lv_obj_set_style_transform_rotation(lid_top_[i], angle, 0);
            lv_obj_remove_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Olho "sorrindo": um círculo da cor do fundo sobe por baixo e deixa um arco "∩".
        if (s.happy && h > 20) {
            const int d = w + EX(12);
            lv_obj_set_pos(lid_bottom_[i], -EX(6), h * 42 / 100);
            lv_obj_set_size(lid_bottom_[i], d, d);
            lv_obj_remove_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Os olhos sobem quando a barra de texto do LcdDisplay está visível.
// Chamar com a trava do display.
void EyesLcdDisplay::UpdateTextVisible() {
    bool visible = bottom_bar_ != nullptr && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    if (visible != text_visible_) {
        text_visible_ = visible;
        ApplyShape();
    }
}

bool EyesLcdDisplay::SetEmotion(const char* emotion) {
    bool known = false;
    {
        DisplayLockGuard lock(this);
        // Já dançando: não reinicia a animação
        if (emotion != nullptr && strcmp(emotion, "music") == 0 && last_emotion_ == "music" &&
            layer_ != nullptr && !lv_obj_has_flag(layer_, LV_OBJ_FLAG_HIDDEN)) {
            return true;
        }
        if (layer_ != nullptr) {
            std::string anterior = last_emotion_;
            known = SelectShapes(emotion);
            if (known) {
                piscadinha_ms_ = 0;
                if (strcmp(emotion, "winking") == 0) {
                    piscadinha_ms_ = lv_tick_get() + 1200;
                    piscadinha_volta_ = (anterior == "winking") ? "neutral" : anterior;
                }
                last_emotion_ = emotion;
                lv_obj_remove_flag(layer_, LV_OBJ_FLAG_HIDDEN);
                ApplyShape();
            } else {
                lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    // Mantém o LcdDisplay em dia (o emoji fica por baixo da camada dos olhos).
    bool shown = SpiLcdDisplay::SetEmotion(emotion);
    return known || shown;
}

void EyesLcdDisplay::SetSono(int nivel) {
    DisplayLockGuard lock(this);
    if (nivel == sono_) {
        return;
    }
    bool acordou = (sono_ > 0 && nivel == 0);
    sono_ = nivel;
    acordar_fase_ = 0;
    if (layer_ == nullptr || lv_obj_has_flag(layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (acordou) {
        SelectShapes("surprised");
        look_dx_ = look_dy_ = 0;
        pupil_tx_ = pupil_ty_ = 0;
        acordar_fase_ = 1;
        acordar_ms_ = lv_tick_get() + 900;
    } else {
        SelectShapes(last_emotion_.c_str());
    }
    ApplyShape();
}

// role "music": nome da música que fica "grudado" na barra de texto. Quando a
// conversa limpa a barra, o nome da música volta sozinho.
void EyesLcdDisplay::SetChatMessage(const char* role, const char* content) {
    std::string texto = (content != nullptr) ? content : "";
    if (role != nullptr && strcmp(role, "music") == 0) {
        bool mostrando_musica = false;
        {
            DisplayLockGuard lock(this);
            mostrando_musica = chat_message_label_ != nullptr && !music_text_.empty() &&
                               music_text_ == lv_label_get_text(chat_message_label_);
            music_text_ = texto;
        }
        // Fim da música: só apaga a barra se ela ainda mostra o nome da música
        // (não apaga uma legenda da conversa que esteja na tela).
        if (texto.empty() && !mostrando_musica) {
            return;
        }
    } else if (texto.empty() && !music_text_.empty()) {
        texto = music_text_;
    }
    SpiLcdDisplay::SetChatMessage(role, texto.c_str());
    {
        DisplayLockGuard lock(this);
        UpdateTextVisible();
    }
    if (role != nullptr && strcmp(role, "user") == 0 && content != nullptr && content[0] != '\0' &&
        on_user_text_) {
        on_user_text_();
    }
}

void EyesLcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::RemoveChatMessage(role, content);
    RestoreMusicText();
}

void EyesLcdDisplay::ClearChatMessages() {
    SpiLcdDisplay::ClearChatMessages();
    RestoreMusicText();
}

void EyesLcdDisplay::RestoreMusicText() {
    std::string texto;
    {
        DisplayLockGuard lock(this);
        texto = music_text_;
    }
    if (!texto.empty()) {
        SpiLcdDisplay::SetChatMessage("music", texto.c_str());
    }
    DisplayLockGuard lock(this);
    UpdateTextVisible();
}

void EyesLcdDisplay::SetBaseColor(uint32_t rgb) {
    DisplayLockGuard lock(this);
    base_color_ = rgb;
}

// Leva a cor dos olhos aos poucos até o alvo. Roda no timer do LVGL.
void EyesLcdDisplay::UpdateColor() {
    uint32_t alvo = base_color_;
    if (dancing_) {
        matiz_ = (matiz_ + 3) % 360;  // volta completa em ~6 s
        alvo = CorArcoIris(matiz_);
    } else if (sono_ >= 1 || last_emotion_ == "sleepy") {
        alvo = kCorDormindo;
    }
    if (alvo == cor_atual_) {
        return;
    }
    uint32_t nova = dancing_ ? alvo : MisturaCor(cor_atual_, alvo, 35);
    if (nova == cor_atual_) {
        nova = alvo;  // últimos passos
    }
    cor_atual_ = nova;
    for (int i = 0; i < 2; i++) {
        if (eye_[i] != nullptr) {
            lv_obj_set_style_bg_color(eye_[i], lv_color_hex(cor_atual_), 0);
        }
    }
}

void EyesLcdDisplay::TimerCallback(lv_timer_t* timer) {
    auto self = static_cast<EyesLcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnTimer();
    }
}

// Roda na tarefa do LVGL (já com a trava do display).
void EyesLcdDisplay::OnTimer() {
    if (layer_ == nullptr || lv_obj_has_flag(layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    uint32_t now = lv_tick_get();
    bool changed = false;
    UpdateColor();

    if (piscadinha_ms_ != 0 && now >= piscadinha_ms_) {
        piscadinha_ms_ = 0;
        last_emotion_ = piscadinha_volta_;
        SelectShapes(last_emotion_.c_str());
        changed = true;
    }

    // Reação ao acordar: 1 = susto, 2 = olha para um lado, 3 = para o outro, 4 = frente
    if (acordar_fase_ > 0 && now >= acordar_ms_) {
        if (acordar_fase_ == 1) {
            SelectShapes(last_emotion_.c_str());
            pupil_tx_ = -100;
            pupil_ty_ = 0;
        } else if (acordar_fase_ == 2) {
            pupil_tx_ = 100;
        } else {
            pupil_tx_ = pupil_base_x_;
            pupil_ty_ = pupil_base_y_;
        }
        look_dx_ = (pupil_tx_ - pupil_base_x_) * 4 / 100;
        look_dy_ = (pupil_ty_ - pupil_base_y_) * 3 / 100;
        acordar_fase_ = (acordar_fase_ >= 3) ? 0 : acordar_fase_ + 1;
        acordar_ms_ = now + 650;
        next_look_ms_ = now + 2500;
        blink_step_ = 0;
        changed = true;
    }

    if (blink_step_ > 0) {
        blink_step_++;
        if (blink_step_ >= kBlinkSteps) {
            blink_step_ = 0;
        }
        changed = true;
    } else if (allow_blink_ && now >= next_blink_ms_) {
        blink_step_ = 1;
        next_blink_ms_ = now + (RandomBetween(0, 5) == 0 ? 350 : RandomBetween(2500, 6000));
        changed = true;
    }

    // Dançando: sobe e desce no ritmo e às vezes balança para os lados
    if (dancing_ && now >= next_dance_ms_) {
        dance_step_ = (dance_step_ + 1) % 8;
        static const int kDy[] = {0, -2, 0, -2, 0, -2, 0, -2};
        static const int kDx[] = {0, 0, 2, 2, 0, 0, -2, -2};
        look_dy_ = kDy[dance_step_];
        look_dx_ = kDx[dance_step_];
        next_dance_ms_ = now + 260;
        changed = true;
    }

    if (allow_look_ && blink_step_ == 0 && now >= next_look_ms_) {
        if (RandomBetween(0, 3) == 0) {
            pupil_tx_ = 0;
            pupil_ty_ = 0;
        } else {
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
