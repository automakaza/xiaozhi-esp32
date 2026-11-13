#include "sd_music_player.h"

#include "application.h"
#include "board.h"
#include "display/display.h"
#include "audio/audio_service.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "mcp_server.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include <dirent.h>
#include <sys/stat.h>

#include <cJSON.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_vfs_fat.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_ae_rate_cvt.h>
#include <sdmmc_cmd.h>

#define TAG "SdMusicPlayer"

static const char* kMountPoint = "/sdcard";
static const size_t kMaxFiles = 300;
static const size_t kReadChunk = 4096;

// ----------------------------------------------------------------------------
// Cartão SD
// ----------------------------------------------------------------------------

static esp_vfs_fat_sdmmc_mount_config_t MountConfig() {
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;
    return cfg;
}

static void LogCard(const char* modo, sdmmc_card_t* card) {
    ESP_LOGI(TAG, "Cartão SD montado (%s): %s, %llu MB", modo, card->cid.name,
             ((unsigned long long)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
}

// 1ª tentativa: SDMMC de 1 bit (mais rápido). Em modo SD o D3 tem que ficar
// em nível alto (em baixo o cartão entra em modo SPI no reset).
static bool MountSdmmc(int clk, int cmd, int d0, int d3) {
    if (d3 >= 0) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << d3;
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        gpio_set_level((gpio_num_t)d3, 1);
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // Preenchido à mão (a macro padrão usa inicializadores que o C++ não aceita bem)
    sdmmc_slot_config_t slot = {};
    slot.clk = (gpio_num_t)clk;
    slot.cmd = (gpio_num_t)cmd;
    slot.d0 = (gpio_num_t)d0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    auto mount_cfg = MountConfig();
    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC 1 bit falhou: %s", esp_err_to_name(err));
        return false;
    }
    LogCard("SDMMC 1 bit", card);
    return true;
}

// 2ª tentativa: modo SPI (CMD = MOSI, D0 = MISO, CLK = SCK, D3 = CS), no SPI2
// (o SPI3 é do display).
static bool MountSpi(int clk, int cmd, int d0, int d3) {
    if (d3 >= 0) {
        gpio_reset_pin((gpio_num_t)d3);
    }
    static bool bus_ok = false;
    if (!bus_ok) {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = cmd;
        bus.miso_io_num = d0;
        bus.sclk_io_num = clk;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;
        esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SPI2 para o cartão falhou: %s", esp_err_to_name(e));
            return false;
        }
        bus_ok = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = SPI2_HOST;
    dev.gpio_cs = (gpio_num_t)d3;

    auto mount_cfg = MountConfig();
    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &dev, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD em modo SPI falhou: %s", esp_err_to_name(err));
        return false;
    }
    LogCard("SPI", card);
    return true;
}

bool SdMusicPlayer::Mount(int clk, int cmd, int d0, int d3) {
    if (MountSdmmc(clk, cmd, d0, d3)) {
        return true;
    }
    if (MountSpi(clk, cmd, d0, d3)) {
        return true;
    }
    ESP_LOGW(TAG, "Sem cartão SD (ou cartão não FAT32)");
    return false;
}

void SdMusicPlayer::Initialize(int clk, int cmd, int d0, int d3, int volume) {
    music_volume_ = std::max(5, std::min(volume, 100));
    pin_clk_ = clk;
    pin_cmd_ = cmd;
    pin_d0_ = d0;
    pin_d3_ = d3;
    // As ferramentas são registradas mesmo sem cartão: assim a Sophia avisa
    // "não encontrei o cartão" em vez de dizer que não sabe tocar músicas.
    if (EnsureMounted()) {
        auto files = ScanFiles();
        ESP_LOGI(TAG, "%u música(s) no cartão", (unsigned)files.size());
    }

    RegisterTools();
    xTaskCreatePinnedToCore(
        [](void* arg) { static_cast<SdMusicPlayer*>(arg)->Task(); }, "sd_music", 6144, this, 3,
        &task_, 1);
}

// Tenta montar de novo se o cartão foi colocado depois de ligar.
bool SdMusicPlayer::EnsureMounted() {
    if (!mounted_) {
        mounted_ = Mount(pin_clk_, pin_cmd_, pin_d0_, pin_d3_);
    }
    return mounted_;
}

static const char* kSemCartao =
    "Cartão SD não encontrado. Verifique se está bem encaixado e formatado em FAT32.";

// ----------------------------------------------------------------------------
// Lista de músicas
// ----------------------------------------------------------------------------

std::string SdMusicPlayer::Lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (c < 128) ? std::tolower(c) : c; });
    return r;
}

static bool IsAudioFile(const std::string& name) {
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (c < 128) ? std::tolower(c) : c; });
    auto ends = [&lower](const char* ext) {
        size_t n = strlen(ext);
        return lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0;
    };
    return ends(".ogg") || ends(".opus") || ends(".mp3") || ends(".wav") || ends(".flac") ||
           ends(".m4a") || ends(".aac");
}

// Tipo do decodificador pela extensão (NONE = Ogg Opus, caminho próprio)
static esp_audio_simple_dec_type_t DecoderType(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == "mp3") return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (ext == "wav") return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (ext == "flac") return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (ext == "m4a") return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (ext == "aac") return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static void ScanDir(const std::string& dir, std::vector<std::string>& out, int depth) {
    if (depth > 3 || out.size() >= kMaxFiles) {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && out.size() < kMaxFiles) {
        if (e->d_name[0] == '.') {
            continue;  // ocultos, "." e ".."
        }
        std::string path = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (strcasecmp(e->d_name, "System Volume Information") != 0) {
                ScanDir(path, out, depth + 1);
            }
        } else if (IsAudioFile(e->d_name)) {
            out.push_back(path);
        }
    }
    closedir(d);
}

std::vector<std::string> SdMusicPlayer::ScanFiles() {
    std::vector<std::string> files;
    ScanDir(kMountPoint, files, 0);
    std::sort(files.begin(), files.end());
    return files;
}

// "/sdcard/musicas/caminhada/Asa Branca.ogg" -> "caminhada/Asa Branca"
std::string SdMusicPlayer::DisplayName(const std::string& path) {
    std::string name = path;
    const std::string raiz = std::string(kMountPoint) + "/";
    if (name.compare(0, raiz.size(), raiz) == 0) {
        name = name.substr(raiz.size());
    }
    // pasta "musicas" (Musicas, MUSICAS, Músicas...) não faz parte do nome
    auto barra = name.find('/');
    if (barra != std::string::npos) {
        std::string pasta = Normalize(name.substr(0, barra));
        if (pasta == "musicas" || pasta == "musica" || pasta == "music") {
            name = name.substr(barra + 1);
        }
    }
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    return name;
}

// Busca pelo nome da música ou da pasta (playlist), sem diferenciar maiúsculas.
// Minúsculas, sem acento e com '_', '-', '.' virando espaço: "Pop_Rock" ->
// "pop rock", "Flávio José" -> "flavio jose".
std::string SdMusicPlayer::Normalize(const std::string& in) {
    static const struct { const char* utf8; char ascii; } kAcentos[] = {
        {"á", 'a'}, {"à", 'a'}, {"â", 'a'}, {"ã", 'a'}, {"ä", 'a'}, {"Á", 'a'}, {"À", 'a'},
        {"Â", 'a'}, {"Ã", 'a'}, {"é", 'e'}, {"ê", 'e'}, {"è", 'e'}, {"É", 'e'}, {"Ê", 'e'},
        {"í", 'i'}, {"Í", 'i'}, {"ó", 'o'}, {"ô", 'o'}, {"õ", 'o'}, {"ö", 'o'}, {"Ó", 'o'},
        {"Ô", 'o'}, {"Õ", 'o'}, {"ú", 'u'}, {"ü", 'u'}, {"Ú", 'u'}, {"ç", 'c'}, {"Ç", 'c'},
        {"ñ", 'n'}, {"Ñ", 'n'},
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char c = in[i];
        if (c < 128) {
            char ch = (char)std::tolower(c);
            if (ch == '_' || ch == '-' || ch == '.' || ch == '(' || ch == ')' || ch == '[' ||
                ch == ']' || ch == '{' || ch == '}' || ch == ',' || ch == '/') {
                ch = ' ';
            }
            if (!(ch == ' ' && (out.empty() || out.back() == ' '))) {
                out.push_back(ch);
            }
            i++;
            continue;
        }
        bool trocou = false;
        for (const auto& a : kAcentos) {
            size_t n = strlen(a.utf8);
            if (in.compare(i, n, a.utf8) == 0) {
                out.push_back(a.ascii);
                i += n;
                trocou = true;
                break;
            }
        }
        if (!trocou) {
            out.push_back((char)c);
            i++;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

static std::vector<std::string> Palavras(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// A palavra pedida "bate" com o nome se aparece nele, ou se começa com uma
// palavra do nome (ASR: "rocking" -> "rock", "remixes" -> "remix").
static bool PalavraBate(const std::string& palavra, const std::vector<std::string>& nome,
                        const std::string& nome_inteiro) {
    if (nome_inteiro.find(palavra) != std::string::npos) {
        return true;
    }
    for (const auto& w : nome) {
        if (w.size() >= 3 && palavra.compare(0, w.size(), w) == 0) {
            return true;
        }
    }
    return false;
}

// Busca pelo nome da música ou da pasta (playlist). Primeiro a frase inteira;
// se não achar nada, aceita quando todas as palavras pedidas batem.
std::vector<std::string> SdMusicPlayer::Filter(const std::vector<std::string>& files,
                                               const std::string& termo) {
    std::string t = Normalize(termo);
    if (t.empty()) {
        return files;
    }
    std::vector<std::string> out;
    for (const auto& f : files) {
        if (Normalize(DisplayName(f)).find(t) != std::string::npos) {
            out.push_back(f);
        }
    }
    if (!out.empty()) {
        return out;
    }
    auto pedidas = Palavras(t);
    for (const auto& f : files) {
        std::string n = Normalize(DisplayName(f));
        auto palavras_nome = Palavras(n);
        bool todas = true;
        for (const auto& p : pedidas) {
            if (!PalavraBate(p, palavras_nome, n)) {
                todas = false;
                break;
            }
        }
        if (todas) {
            out.push_back(f);
        }
    }
    return out;
}

// Pastas (playlists) com a quantidade de músicas de cada uma.
std::string SdMusicPlayer::PlaylistsSummary(const std::vector<std::string>& files) {
    std::vector<std::pair<std::string, int>> pastas;
    int soltas = 0;
    for (const auto& f : files) {
        std::string n = DisplayName(f);
        auto barra = n.find('/');
        if (barra == std::string::npos) {
            soltas++;
            continue;
        }
        std::string pasta = n.substr(0, barra);
        auto it = std::find_if(pastas.begin(), pastas.end(),
                               [&pasta](const auto& p) { return p.first == pasta; });
        if (it == pastas.end()) {
            pastas.push_back({pasta, 1});
        } else {
            it->second++;
        }
    }
    std::string out;
    for (const auto& p : pastas) {
        if (!out.empty()) out += "; ";
        out += p.first + " (" + std::to_string(p.second) + ")";
    }
    if (soltas > 0) {
        if (!out.empty()) out += "; ";
        out += "músicas avulsas (" + std::to_string(soltas) + ")";
    }
    return out;
}

// ----------------------------------------------------------------------------
// Ferramentas MCP
// ----------------------------------------------------------------------------

void SdMusicPlayer::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.music.list",
        "Músicas do cartão SD do aparelho. Sem 'busca': lista as playlists (pastas) com a "
        "quantidade de músicas. Com 'busca' (nome de playlist, artista ou música): lista os "
        "nomes das músicas que batem. Use quando perguntarem o que tem no cartão.",
        PropertyList({Property("busca", kPropertyTypeString, std::string(""))}),
        [this](const PropertyList& props) -> ToolResult {
            if (!EnsureMounted()) {
                return std::unexpected<std::string>(kSemCartao);
            }
            auto files = ScanFiles();
            if (files.empty()) {
                return std::string("O cartão SD não tem músicas (MP3, WAV, FLAC, M4A, AAC ou OGG).");
            }
            std::string busca = props["busca"].value<std::string>();
            if (busca.empty()) {
                return std::to_string(files.size()) +
                       " músicas no cartão. Playlists: " + PlaylistsSummary(files);
            }
            auto achadas = Filter(files, busca);
            if (achadas.empty()) {
                return "Nada com '" + busca + "'. Playlists: " + PlaylistsSummary(files);
            }
            cJSON* arr = cJSON_CreateArray();
            size_t n = 0;
            for (const auto& f : achadas) {
                if (++n > 60) {
                    break;  // não afoga o modelo com listas enormes
                }
                cJSON_AddItemToArray(arr, cJSON_CreateString(DisplayName(f).c_str()));
            }
            return arr;
        });

    mcp.AddTool(
        "self.music.play",
        "Toca músicas do cartão SD do próprio aparelho (não é rádio nem internet). "
        "IMPORTANTE: se uma rádio estiver tocando, chame parar_radio ANTES desta ferramenta, "
        "senão a rádio volta e cobre a música. "
        "'busca' é parte do nome da música ou da pasta/playlist; vazio toca todas. "
        "'aleatorio' embaralha. A música começa assim que você terminar de responder, "
        "então responda bem curto (ex.: 'Tocando!'). Se te chamarem durante a música ela "
        "pausa e volta sozinha depois da sua resposta; para parar de vez use "
        "self.music.stop, para pular use self.music.next.",
        PropertyList({Property("busca", kPropertyTypeString, std::string("")),
                      Property("aleatorio", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& props) -> ToolResult {
            std::string busca = props["busca"].value<std::string>();
            bool aleatorio = props["aleatorio"].value<bool>();
            if (!EnsureMounted()) {
                return std::unexpected<std::string>(kSemCartao);
            }
            auto files = Filter(ScanFiles(), busca);
            if (files.empty()) {
                auto todas = ScanFiles();
                if (todas.empty()) {
                    return std::string("Não há músicas no cartão SD. Nada foi tocado.");
                }
                // Resposta normal (não erro), para a assistente oferecer as opções
                return "Não encontrei '" + busca + "' no cartão; nada foi tocado. Playlists "
                       "disponíveis: " + PlaylistsSummary(todas) +
                       ". Pergunte qual a pessoa quer.";
            }
            if (aleatorio) {
                for (size_t i = files.size() - 1; i > 0; i--) {
                    std::swap(files[i], files[esp_random() % (i + 1)]);
                }
            }
            std::string primeira = DisplayName(files[0]);
            size_t total = files.size();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_list_ = std::move(files);
            }
            play_pending_ = true;
            stop_req_ = true;  // interrompe o que estiver tocando
            xTaskNotifyGive(task_);
            return std::string("Vai tocar ") + std::to_string(total) +
                   (total == 1 ? " música: " : " músicas, começando por: ") + primeira;
        });

    mcp.AddTool("self.music.next", "Pula para a próxima música do cartão SD.", PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    if (!playing_ && !play_pending_) {
                        return std::unexpected<std::string>("Nenhuma música tocando.");
                    }
                    next_req_ = true;
                    return true;
                });

    mcp.AddTool("self.music.stop", "Para a música do cartão SD.", PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    play_pending_ = false;
                    stop_req_ = true;
                    return true;
                });

    mcp.AddTool("self.music.status", "Diz se há música do cartão SD tocando e qual.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!playing_ && !play_pending_) {
                        return std::string("Nada tocando.");
                    }
                    return std::string("Tocando: ") +
                           (current_.empty() ? "(começando)" : current_);
                });
}

// ----------------------------------------------------------------------------
// Reprodução
// ----------------------------------------------------------------------------

// Espera a Sophia terminar de responder e encerra a conversa (sem isso ela
// fica ouvindo e a música não volta). false = pedido cancelado (parar ou
// outra música pedida) ou conversa longa demais.
bool SdMusicPlayer::WaitUntilFree(uint32_t timeout_ms) {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    const TickType_t start = xTaskGetTickCount();
    bool saw_speaking = false;
    bool stop_sent = false;

    while (true) {
        if (stop_req_ || play_pending_) {
            return false;
        }
        auto st = app.GetDeviceState();
        uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);

        if (st == kDeviceStateIdle && audio.IsPlaybackIdle()) {
            return true;
        }
        if (st == kDeviceStateSpeaking) {
            saw_speaking = true;
            stop_sent = false;  // depois de cada resposta, encerra de novo
        }
        if (st == kDeviceStateListening && !stop_sent && (saw_speaking || elapsed_ms > 12000)) {
            ESP_LOGI(TAG, "Encerrando a conversa para seguir com a música");
            app.StopListening();
            stop_sent = true;
        }
        if (elapsed_ms > timeout_ms) {
            ESP_LOGW(TAG, "Conversa longa demais: música cancelada");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Mostra o nome da música na barra de texto da tela (os olhos sobem um pouco)
// e deixa os olhos "curtindo" (felizes) enquanto toca.
void SdMusicPlayer::ShowNowPlaying() {
    std::string nome;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nome = current_;
    }
    auto barra = nome.find_last_of('/');
    if (barra != std::string::npos) {
        nome = nome.substr(barra + 1);  // sem a pasta/playlist
    }
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || nome.empty()) {
        return;
    }
    display->SetEmotion("happy");
    display->SetChatMessage("system", ("Tocando: " + nome).c_str());
}

void SdMusicPlayer::ClearNowPlaying() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    display->SetChatMessage("system", "");
    display->SetEmotion("neutral");
}

// Antes de entregar cada pedaço de áudio: obedece parar/pular/nova música e
// pausa enquanto alguém conversa com a Sophia. false = encerrar a faixa.
bool SdMusicPlayer::Gate(Result& result) {
    if (stop_req_ || play_pending_) {
        result = kStopped;
        return false;
    }
    if (next_req_) {
        result = kSkipped;
        return false;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        // Alguém chamou a Sophia: pausa aqui e continua do mesmo ponto
        ESP_LOGI(TAG, "Música pausada (conversa)");
        if (!WaitUntilFree(10 * 60 * 1000)) {
            result = kStopped;
            return false;
        }
        if (next_req_) {
            result = kSkipped;
            return false;
        }
        ESP_LOGI(TAG, "Música retomada");
        ShowNowPlaying();  // a conversa limpou a tela
    }
    return true;
}

SdMusicPlayer::Result SdMusicPlayer::StreamFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Não abriu %s", path.c_str());
        return kSkipped;
    }
    auto type = DecoderType(path);
    Result result = (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) ? StreamOpus(f, path)
                                                             : StreamDecoded(f, path, (int)type);
    fclose(f);

    auto& app = Application::GetInstance();
    if (result != kFinished && app.GetDeviceState() == kDeviceStateIdle) {
        app.GetAudioService().ResetDecoder();  // corta o que ainda estava na fila
    }
    return result;
}

// Ogg Opus mono: entra na fila do decodificador Opus do próprio Xiaozhi.
SdMusicPlayer::Result SdMusicPlayer::StreamOpus(FILE* f, const std::string& path) {
    auto& audio = Application::GetInstance().GetAudioService();
    // Liga a saída de áudio (e o desligamento automático dela depois)
    audio.PlaySound(std::string_view());

    Result result = kFinished;
    bool running = true;
    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnPacket([this, &audio, &result, &running](const uint8_t* data, int sample_rate,
                                                       int frame_duration_ms, size_t size) {
        if (!running) {
            return;
        }
        if (!Gate(result)) {
            running = false;
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->payload.assign(data, data + size);
        audio.PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();

    std::vector<uint8_t> buf(kReadChunk);
    while (running) {
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            break;
        }
        size_t done = 0;
        while (done < n && running) {
            size_t used = demuxer->Process(buf.data() + done, n - done);
            if (used == 0) {
                break;
            }
            done += used;
        }
        if (demuxer->HasError()) {
            ESP_LOGE(TAG, "%s não é Ogg Opus mono; pulando", path.c_str());
            return kSkipped;
        }
    }
    return result;
}

// MP3, WAV, FLAC, M4A e AAC: decodifica aqui, junta os canais em mono, converte
// para a taxa do alto-falante e entrega o PCM pronto à fila de reprodução.
SdMusicPlayer::Result SdMusicPlayer::StreamDecoded(FILE* f, const std::string& path, int type) {
    static bool registrado = false;
    if (!registrado) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        registrado = true;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = (esp_audio_simple_dec_type_t)type;
    esp_audio_simple_dec_handle_t dec = nullptr;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
        ESP_LOGE(TAG, "Formato não suportado: %s", path.c_str());
        return kSkipped;
    }

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int in_rate = 0;
    int channels = 0;
    bool erro = false;

    Result result = kFinished;
    std::vector<uint8_t> in_buf(kReadChunk);
    std::vector<uint8_t> out_buf(8192);
    std::vector<int16_t> mono;
    std::vector<int16_t> resampled;
    std::vector<int16_t> pending;
    const size_t chunk_samples = out_rate / 10;  // entrega em pedaços de ~100 ms
    pending.reserve(chunk_samples * 2);

    auto entregar = [&](bool tudo) -> bool {
        while (pending.size() >= chunk_samples || (tudo && !pending.empty())) {
            if (!Gate(result)) {
                return false;
            }
            size_t n = std::min(pending.size(), chunk_samples);
            std::vector<int16_t> pcm(pending.begin(), pending.begin() + n);
            pending.erase(pending.begin(), pending.begin() + n);
            if (!audio.PushPcmToPlaybackQueue(std::move(pcm), true)) {
                // fila limpa no meio (alguém chamou a Sophia): passa pela pausa
                if (!Gate(result)) {
                    return false;
                }
            }
        }
        return true;
    };

    esp_audio_simple_dec_raw_t raw = {};
    bool running = true;
    while (running) {
        size_t lidos = fread(in_buf.data(), 1, in_buf.size(), f);
        if (lidos == 0) {
            break;
        }
        raw.buffer = in_buf.data();
        raw.len = lidos;
        raw.eos = (lidos < in_buf.size());

        while (raw.len > 0 && running) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = out_buf.data();
            out.len = out_buf.size();
            auto ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                out_buf.resize(out.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Erro %d decodificando %s", (int)ret, path.c_str());
                erro = true;
                running = false;
                break;
            }
            if (out.decoded_size > 0) {
                if (channels == 0) {
                    esp_audio_simple_dec_info_t info = {};
                    esp_audio_simple_dec_get_info(dec, &info);
                    if (info.bits_per_sample != 16 || info.channel == 0) {
                        ESP_LOGE(TAG, "%s: só áudio de 16 bits (tem %d)", path.c_str(),
                                 info.bits_per_sample);
                        erro = true;
                        running = false;
                        break;
                    }
                    in_rate = info.sample_rate;
                    channels = info.channel;
                    ESP_LOGI(TAG, "%d Hz, %d canal(is) -> %d Hz mono", in_rate, channels, out_rate);
                    if (in_rate != out_rate) {
                        esp_ae_rate_cvt_cfg_t rc = {};
                        rc.src_rate = (uint32_t)in_rate;
                        rc.dest_rate = (uint32_t)out_rate;
                        rc.channel = 1;
                        rc.bits_per_sample = ESP_AUDIO_BIT16;
                        rc.complexity = 2;
                        rc.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                        if (esp_ae_rate_cvt_open(&rc, &resampler) != ESP_AE_ERR_OK) {
                            ESP_LOGE(TAG, "Falha no conversor de taxa");
                            erro = true;
                            running = false;
                            break;
                        }
                    }
                }

                // estéreo (ou mais) -> mono
                const int16_t* pcm = reinterpret_cast<const int16_t*>(out.buffer);
                size_t frames = out.decoded_size / sizeof(int16_t) / channels;
                mono.resize(frames);
                for (size_t i = 0; i < frames; i++) {
                    int32_t soma = 0;
                    for (int c = 0; c < channels; c++) {
                        soma += pcm[i * channels + c];
                    }
                    mono[i] = (int16_t)(soma / channels * music_volume_ / 100);
                }

                if (resampler != nullptr) {
                    uint32_t max_out = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, frames, &max_out);
                    resampled.resize(max_out);
                    uint32_t produzidos = max_out;
                    esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(), frames,
                                            (esp_ae_sample_t)resampled.data(), &produzidos);
                    pending.insert(pending.end(), resampled.begin(), resampled.begin() + produzidos);
                } else {
                    pending.insert(pending.end(), mono.begin(), mono.end());
                }
                if (!entregar(false)) {
                    running = false;
                    break;
                }
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
        if (raw.eos) {
            break;
        }
    }
    if (running && !erro) {
        entregar(true);  // o finalzinho da música
    }

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    esp_audio_simple_dec_close(dec);
    if (erro && result == kFinished) {
        return kSkipped;
    }
    return result;
}

void SdMusicPlayer::Task() {
    auto& app = Application::GetInstance();
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (play_pending_) {
            std::vector<std::string> list;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                list = pending_list_;
                current_.clear();
            }
            play_pending_ = false;
            stop_req_ = false;
            next_req_ = false;

            if (!WaitUntilFree(90 * 1000)) {
                continue;
            }

            playing_ = true;
            for (size_t i = 0; i < list.size(); i++) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    current_ = DisplayName(list[i]);
                }
                ESP_LOGI(TAG, "Tocando %s", list[i].c_str());
                next_req_ = false;
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    ShowNowPlaying();
                }
                if (StreamFile(list[i]) == kStopped) {
                    break;
                }
            }
            playing_ = false;
            if (app.GetDeviceState() == kDeviceStateIdle && !play_pending_) {
                ClearNowPlaying();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                current_.clear();
            }
            ESP_LOGI(TAG, "Fim da música");
        }
    }
}
