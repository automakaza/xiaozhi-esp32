#include "spectrum_analyzer.h"

#include <algorithm>
#include <cmath>

SpectrumAnalyzer::SpectrumAnalyzer(int sample_rate) {
    const float pi = 3.14159265f;
    for (int i = 0; i < kFft; i++) {
        window_[i] = 0.5f - 0.5f * std::cos(2.0f * pi * i / (kFft - 1));  // Hann
    }
    for (int i = 0; i < kFft / 2; i++) {
        cos_[i] = std::cos(2.0f * pi * i / kFft);
        sin_[i] = -std::sin(2.0f * pi * i / kFft);
    }
    // Bandas em escala logarítmica de ~120 Hz até ~9 kHz (ou metade da taxa)
    const float hz_por_bin = (float)sample_rate / kFft;
    const float f_min = 120.0f;
    const float f_max = std::min(9000.0f, sample_rate / 2.0f - hz_por_bin);
    int ultimo = 0;
    for (int b = 0; b <= kBands; b++) {
        float f = f_min * std::pow(f_max / f_min, (float)b / kBands);
        int bin = std::max(1, (int)std::lround(f / hz_por_bin));
        if (bin <= ultimo) {
            bin = ultimo + 1;  // cada banda com pelo menos 1 bin
        }
        bin = std::min(bin, kFft / 2 - 1);
        band_start_[b] = bin;
        ultimo = bin;
    }
    for (auto& l : levels_) {
        l.store(0);
    }
}

void SpectrumAnalyzer::Feed(const std::vector<int16_t>& pcm) {
    if (!Enabled()) {
        return;
    }
    if (pcm.size() < (size_t)kFft) {
        return;
    }
    // Usa o trecho final do bloco (é o que está mais perto de tocar)
    Analyze(pcm.data() + pcm.size() - kFft);
}

void SpectrumAnalyzer::Analyze(const int16_t* samples) {
    // Janela + reordenação por bit-reverso
    for (int i = 0; i < kFft; i++) {
        int j = 0;
        for (int b = 0, x = i; b < 8; b++, x >>= 1) {  // 256 = 2^8
            j = (j << 1) | (x & 1);
        }
        re_[j] = samples[i] * window_[i] / 32768.0f;
        im_[j] = 0.0f;
    }
    // FFT radix-2
    for (int len = 2; len <= kFft; len <<= 1) {
        int metade = len / 2, passo = kFft / len;
        for (int i = 0; i < kFft; i += len) {
            for (int k = 0; k < metade; k++) {
                float wr = cos_[k * passo], wi = sin_[k * passo];
                int a = i + k, b = a + metade;
                float tr = re_[b] * wr - im_[b] * wi;
                float ti = re_[b] * wi + im_[b] * wr;
                re_[b] = re_[a] - tr;
                im_[b] = im_[a] - ti;
                re_[a] += tr;
                im_[a] += ti;
            }
        }
    }

    float db[kBands];
    float maior = -120.0f;
    for (int b = 0; b < kBands; b++) {
        float soma = 0.0f;
        int n = 0;
        for (int k = band_start_[b]; k < std::max(band_start_[b] + 1, band_start_[b + 1]); k++) {
            soma += re_[k] * re_[k] + im_[k] * im_[k];
            n++;
        }
        float media = soma / std::max(1, n);
        db[b] = 10.0f * std::log10(media + 1e-12f);
        // Agudos têm menos energia: compensa um pouco para as barras da
        // direita também mexerem
        db[b] += 1.5f * b;
        maior = std::max(maior, db[b]);
    }

    // Ganho automático: o pico acompanha a música (sobe rápido, desce devagar)
    if (maior > peak_db_) {
        peak_db_ = maior;
    } else {
        peak_db_ -= 0.15f;
    }
    const float faixa = 40.0f;  // dB mostrados entre barra vazia e cheia
    for (int b = 0; b < kBands; b++) {
        float nivel = (db[b] - (peak_db_ - faixa)) * 100.0f / faixa;
        levels_[b].store((uint8_t)std::clamp(nivel, 0.0f, 100.0f), std::memory_order_relaxed);
    }
}
