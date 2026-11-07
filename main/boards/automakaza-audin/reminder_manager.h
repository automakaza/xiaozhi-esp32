#ifndef _REMINDER_MANAGER_H_
#define _REMINDER_MANAGER_H_

// ============================================================================
// ReminderManager - lembretes guardados na própria placa (NVS).
//
// O servidor manda a lista completa ({"type":"lembretes","lista":[...]}) sempre
// que ela muda e a cada conexão. Cada item: id, t (hora local em segundos, no
// mesmo relógio que o servidor acerta pela OTA), txt (texto) e r (repetição em
// segundos: 0 = uma vez, 86400 = todo dia, 604800 = toda semana).
// Na hora certa a placa acorda sozinha e avisa o servidor, que fala o lembrete.
// ============================================================================

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class ReminderManager {
public:
    void Start();
    void SetFromJson(const std::string& json);
    // Diz se tem alguém por perto (radar). Sem ninguém, o lembrete espera a
    // pessoa chegar e então é avisado como "enquanto você estava fora...".
    void SetPresenceCheck(std::function<bool()> f) { tem_alguem_ = std::move(f); }

private:
    struct Item {
        std::string id;
        int64_t t = 0;
        std::string txt;
        int64_t r = 0;
    };
    std::mutex mutex_;
    std::vector<Item> itens_;
    std::vector<Item> esperando_;   // venceram sem ninguém por perto
    std::function<bool()> tem_alguem_;

    void Load();
    void Save();
    void Loop();
};

#endif  // _REMINDER_MANAGER_H_
