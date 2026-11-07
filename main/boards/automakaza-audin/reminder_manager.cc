#include "reminder_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>

#include "application.h"
#include "settings.h"

#define TAG "Lembretes"

namespace {
constexpr size_t kMaxItens = 20;
constexpr int64_t kRelogioValido = 1700000000;  // relógio já acertado pela OTA
constexpr int64_t kAtrasoMax = 30 * 60;          // atrasou mais que isso (placa desligada): descarta
constexpr int64_t kEsperaMax = 12 * 3600;        // sem ninguém: guarda o aviso por até 12 h
}  // namespace

void ReminderManager::Start() {
    Load();
    xTaskCreate(
        [](void* arg) {
            static_cast<ReminderManager*>(arg)->Loop();
            vTaskDelete(nullptr);
        },
        "lembretes", 4096, this, 2, nullptr);
}

void ReminderManager::Load() {
    Settings settings("lembretes", false);
    std::string json = settings.GetString("lista", "[]");
    cJSON* arr = cJSON_Parse(json.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    itens_.clear();
    if (cJSON_IsArray(arr)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr) {
            Item item;
            cJSON* id = cJSON_GetObjectItem(it, "id");
            cJSON* t = cJSON_GetObjectItem(it, "t");
            cJSON* txt = cJSON_GetObjectItem(it, "txt");
            cJSON* r = cJSON_GetObjectItem(it, "r");
            if (!cJSON_IsString(id) || !cJSON_IsNumber(t)) {
                continue;
            }
            item.id = id->valuestring;
            item.t = (int64_t)t->valuedouble;
            item.txt = cJSON_IsString(txt) ? txt->valuestring : "";
            item.r = cJSON_IsNumber(r) ? (int64_t)r->valuedouble : 0;
            itens_.push_back(item);
            if (itens_.size() >= kMaxItens) {
                break;
            }
        }
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "%d lembrete(s) guardado(s)", (int)itens_.size());
}

void ReminderManager::Save() {
    // chamar com mutex_ travado
    cJSON* arr = cJSON_CreateArray();
    for (auto& item : itens_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", item.id.c_str());
        cJSON_AddNumberToObject(o, "t", (double)item.t);
        cJSON_AddStringToObject(o, "txt", item.txt.c_str());
        cJSON_AddNumberToObject(o, "r", (double)item.r);
        cJSON_AddItemToArray(arr, o);
    }
    char* s = cJSON_PrintUnformatted(arr);
    if (s) {
        Settings settings("lembretes", true);
        settings.SetString("lista", s);
        cJSON_free(s);
    }
    cJSON_Delete(arr);
}

void ReminderManager::SetFromJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        return;
    }
    cJSON* lista = cJSON_GetObjectItem(root, "lista");
    if (cJSON_IsArray(lista)) {
        char* s = cJSON_PrintUnformatted(lista);
        if (s) {
            {
                Settings settings("lembretes", true);
                settings.SetString("lista", s);
            }
            cJSON_free(s);
            Load();
        }
    }
    cJSON_Delete(root);
}

void ReminderManager::Loop() {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t agora = (int64_t)time(nullptr);
        if (agora < kRelogioValido) {
            continue;  // relógio ainda não acertado
        }
        std::vector<Item> vencidos;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool mudou = false;
            for (auto it = itens_.begin(); it != itens_.end();) {
                if (it->t > agora) {
                    ++it;
                    continue;
                }
                if (agora - it->t <= kAtrasoMax) {
                    vencidos.push_back(*it);
                } else {
                    ESP_LOGW(TAG, "Lembrete perdido (placa estava desligada): %s", it->txt.c_str());
                }
                mudou = true;
                if (it->r > 0) {
                    while (it->t <= agora) {
                        it->t += it->r;
                    }
                    ++it;
                } else {
                    it = itens_.erase(it);
                }
            }
            if (mudou) {
                Save();
            }
        }
        bool alguem = !tem_alguem_ || tem_alguem_();
        for (auto& item : vencidos) {
            if (!alguem) {
                ESP_LOGI(TAG, "Lembrete sem ninguém por perto, esperando alguém chegar: %s",
                         item.txt.c_str());
                item.t = agora;  // marca quando venceu
                std::lock_guard<std::mutex> lock(mutex_);
                esperando_.push_back(item);
                continue;
            }
            ESP_LOGI(TAG, "Hora do lembrete: %s", item.txt.c_str());
            Application::GetInstance().SendReminderEvent(item.id, item.txt, false);
        }

        // Alguém chegou: avisa os lembretes que venceram enquanto estava fora
        std::vector<Item> chegou;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = esperando_.begin(); it != esperando_.end();) {
                if (agora - it->t > kEsperaMax) {
                    it = esperando_.erase(it);
                } else {
                    ++it;
                }
            }
            if (alguem && !esperando_.empty()) {
                chegou.swap(esperando_);
            }
        }
        for (auto& item : chegou) {
            ESP_LOGI(TAG, "Alguém chegou, avisando lembrete atrasado: %s", item.txt.c_str());
            Application::GetInstance().SendReminderEvent(item.id, item.txt, true);
            vTaskDelay(pdMS_TO_TICKS(15000));  // um de cada vez
        }
    }
}
