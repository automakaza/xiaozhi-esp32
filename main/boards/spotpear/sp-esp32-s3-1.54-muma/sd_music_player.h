#ifndef _SD_MUSIC_PLAYER_H_
#define _SD_MUSIC_PLAYER_H_

// ============================================================================
// SdMusicPlayer - toca músicas do cartão SD por comando de voz (ferramentas MCP).
//
// Arquivos aceitos: MP3, WAV, FLAC, M4A e AAC (decodificados aqui, estéreo vira
// mono e a taxa é convertida para a do alto-falante) e Ogg Opus mono, que vai
// direto ao decodificador Opus do Xiaozhi (o mais leve).
// Coloque na pasta "musicas" do cartão (ou na raiz). Subpastas funcionam como
// playlists: "musicas/caminhada/...".
//
// O pedido chega no meio da conversa; a música começa quando a Sophia termina
// de responder. Chamar a Sophia no meio da música PAUSA; depois da resposta ela
// continua do mesmo ponto (ou pula/para, se foi isso que se pediu).
// ============================================================================

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class SdMusicPlayer {
public:
    static SdMusicPlayer& GetInstance() {
        static SdMusicPlayer instance;
        return instance;
    }

    // Monta o cartão (SDMMC 1 bit) e registra as ferramentas MCP.
    // volume: % da música em relação à voz (MP3/WAV/FLAC/M4A/AAC)
    void Initialize(int clk, int cmd, int d0, int d3, int volume = 100);

    bool IsMounted() const { return mounted_; }
    bool IsPlaying() const { return playing_; }
    // Para a música (ex.: botão BOOT)
    void Stop() {
        play_pending_ = false;
        stop_req_ = true;
    }

private:
    SdMusicPlayer() = default;

    bool Mount(int clk, int cmd, int d0, int d3);
    bool EnsureMounted();
    void RegisterTools();
    std::vector<std::string> ScanFiles();
    std::vector<std::string> Filter(const std::vector<std::string>& files, const std::string& termo);
    static std::string DisplayName(const std::string& path);
    static std::string Lower(const std::string& s);
    static std::string Normalize(const std::string& s);
    std::string PlaylistsSummary(const std::vector<std::string>& files);

    enum Result { kFinished, kSkipped, kStopped };

    void Task();
    bool WaitUntilFree(uint32_t timeout_ms);
    Result StreamFile(const std::string& path);
    Result StreamOpus(FILE* f, const std::string& path);
    Result StreamDecoded(FILE* f, const std::string& path, int type);
    bool Gate(Result& result);
    void ShowNowPlaying();
    void ClearNowPlaying(bool reset_eyes);

    bool mounted_ = false;
    int music_volume_ = 100;
    uint32_t last_eyes_ms_ = 0;
    int pin_clk_ = -1;
    int pin_cmd_ = -1;
    int pin_d0_ = -1;
    int pin_d3_ = -1;
    std::atomic<bool> playing_{false};
    std::atomic<bool> play_pending_{false};
    std::atomic<bool> stop_req_{false};
    std::atomic<bool> next_req_{false};
    std::mutex mutex_;
    std::vector<std::string> pending_list_;
    std::string current_;
    TaskHandle_t task_ = nullptr;
};

#endif  // _SD_MUSIC_PLAYER_H_
