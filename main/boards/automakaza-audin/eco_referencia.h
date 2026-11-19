// Sophia: referência de eco por software para o AEC do ESP-SR.
//
// A placa tem um microfone só e o ES8311 não devolve o som do alto-falante
// (não há canal de "loopback"). Então guardamos aqui tudo o que vai para o
// alto-falante, numa linha do tempo medida em amostras do microfone, e na
// leitura do microfone entregamos 2 canais: [mic, referência].
//
// Entrada e saída usam o mesmo clock I2S (duplex), então o relógio do
// microfone serve de relógio para os dois lados:
//  - Gravar(): cada bloco que vai tocar é posto na linha do tempo logo depois
//    do anterior (som contínuo). Se a saída ficou parada (o fim do bloco
//    anterior já passou), recomeça em "agora + atraso".
//  - Ler(): devolve a referência das posições que o microfone acabou de
//    capturar e apaga essas posições (silêncio se nada tocou ali).
//
// O atraso (SOPHIA_AEC_ATRASO_MS) é o tempo entre mandar o som para o I2S e
// ele sair no alto-falante. É melhor errar para menos: o AEC tolera a
// referência chegando um pouco ANTES do eco, mas não depois.

#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <esp_heap_caps.h>

class EcoReferencia {
public:
    // tamanho: amostras na linha do tempo (potência de 2). 8192 @ 24 kHz ≈ 340 ms
    EcoReferencia(int taxa_hz, int atraso_ms, int tamanho = 8192)
        : tamanho_(tamanho), mascara_(tamanho - 1),
          atraso_(taxa_hz * atraso_ms / 1000) {
        buf_ = static_cast<int16_t*>(
            heap_caps_calloc(tamanho_, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    }
    ~EcoReferencia() { heap_caps_free(buf_); }

    bool ok() const { return buf_ != nullptr; }

    // Chamado ANTES de escrever o bloco no I2S (tarefa de saída de áudio).
    void Gravar(const int16_t* dados, int n) {
        if (buf_ == nullptr || n <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t agora = pos_mic_;
        // Saída parada (ou primeira vez): recomeça a linha do tempo.
        if (fim_saida_ < agora + atraso_) {
            fim_saida_ = agora + atraso_;
        }
        // Microfone parado e saída andando (ex.: entrada desligada): não deixa
        // a linha do tempo dar a volta por cima de dados que ainda serão lidos.
        if (fim_saida_ + n - agora > tamanho_) {
            fim_saida_ = agora + atraso_;
            if (atraso_ + n > tamanho_) return;
        }
        for (int i = 0; i < n; ++i) {
            buf_[(fim_saida_ + i) & mascara_] = dados[i];
        }
        fim_saida_ += n;
    }

    // Chamado depois de ler n amostras do microfone (tarefa de entrada).
    // Escreve a referência em ref[] (n amostras) e avança o relógio.
    void Ler(int16_t* ref, int n) {
        if (buf_ == nullptr) {
            memset(ref, 0, n * sizeof(int16_t));
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < n; ++i) {
            int idx = (pos_mic_ + i) & mascara_;
            ref[i] = buf_[idx];
            buf_[idx] = 0;
        }
        pos_mic_ += n;
    }

private:
    int16_t* buf_ = nullptr;
    const int tamanho_;
    const int mascara_;
    const int atraso_;
    int64_t pos_mic_ = 0;    // amostras já lidas do microfone (relógio)
    int64_t fim_saida_ = 0;  // posição (no relógio do mic) onde termina o som agendado
    std::mutex mutex_;
};
