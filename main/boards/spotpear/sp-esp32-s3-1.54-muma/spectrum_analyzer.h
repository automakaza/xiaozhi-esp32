#ifndef _SPECTRUM_ANALYZER_H_
#define _SPECTRUM_ANALYZER_H_

// ============================================================================
// SpectrumAnalyzer - transforma o áudio que vai para o alto-falante em níveis
// de barras de frequência (equalizador visual) para a tela.
//
// Recebe o PCM mono de saída (antes do volume do codec, então as barras não
// dependem do volume), faz uma FFT de 256 pontos e agrupa em bandas com
// escala logarítmica (graves à esquerda, agudos à direita). Os níveis ficam
// em variáveis atômicas: a tela lê quando quiser, sem travar o áudio.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <vector>

class SpectrumAnalyzer {
public:
    static const int kBands = 12;

    explicit SpectrumAnalyzer(int sample_rate);

    // Chamado na tarefa de saída de áudio, a cada bloco que vai tocar
    void Feed(const std::vector<int16_t>& pcm);

    // Níveis 0..100 de cada banda (leitura sem trava)
    uint8_t Level(int band) const { return levels_[band].load(std::memory_order_relaxed); }

    // Só calcula quando alguém está mostrando as barras
    void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // true se chegou áudio nos últimos ms (sem som, as barras devem cair)
    bool Active(uint32_t now_ms, uint32_t ms = 200) const {
        return now_ms - last_feed_ms_.load(std::memory_order_relaxed) < ms;
    }

private:
    static const int kFft = 256;

    int band_start_[kBands + 1];
    float window_[kFft];
    float cos_[kFft / 2];
    float sin_[kFft / 2];
    float re_[kFft];
    float im_[kFft];
    float peak_db_ = -20.0f;
    std::atomic<uint8_t> levels_[kBands];
    std::atomic<bool> enabled_{false};
    std::atomic<uint32_t> last_feed_ms_{0};

    void Analyze(const int16_t* samples);
};

#endif  // _SPECTRUM_ANALYZER_H_
