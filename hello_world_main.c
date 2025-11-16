/*
 * Global Solution 2 - SISTEMAS DE TEMPO REAL
 * ALUNOS DO GRUPO:
 * Evelyn Dryellen Silva Pereira - RM8805
 * Filipe Giovannetti – RM86925
 * Laura Figueredo - RM86612
 *
 * Monitor de Redes Wi-Fi Seguras em Tempo Real com FreeRTOS
 * Requisitos atendidos:
 * - Tasks com prioridades diferentes
 * - Fila para comunicação entre tarefas
 * - Semáforo para proteger lista de redes seguras
 * - Lista com pelo menos 5 redes seguras
 * - Técnicas de robustez
 * - Log simples (printf) quando uma rede não autorizada for detectada
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_task_wdt.h"

#define MAX_SSID_LEN     32
#define SAFE_WIFI_COUNT   5
#define WDT_TIMEOUT_MS 5000

typedef struct {
    char ssid[MAX_SSID_LEN];
} wifi_event_t;

/* Fila para enviar o SSID atual para a task de verificação */
static QueueHandle_t wifiQueue = NULL;

/* Semáforo para proteger o acesso à lista de redes seguras */
static SemaphoreHandle_t safeListMutex = NULL;

/* Lista de redes consideradas seguras */
static const char *safe_networks[SAFE_WIFI_COUNT] = {
    "FIAP_SECURE",
    "Casa",
    "Lab_IoT",
    "EmpresaXD",
    "Rede_Segura_5"
};

/* Redes vistas pelo dispositivo (simulação) */
static const char *simulated_networks[] = {
    "FIAP_SECURE",
    "Cafeteria_FREE_WIFI",
    "Lab_IoT",
    "Hacker_AP",
    "EmpresaXD",
    "Rede_Segura_5",
    "Invasor_123"
};

/* TASK 1: MONITOR DE REDE (PRODUTOR) */
void vWifiMonitorTask(void *pvParameters)
{
    int index = 0;
    int total_networks = sizeof(simulated_networks) / sizeof(simulated_networks[0]);

    for (;;) {
        const char *current_ssid = simulated_networks[index];

        wifi_event_t *evt = (wifi_event_t *) malloc(sizeof(wifi_event_t));
        if (evt == NULL) {
            printf("Falha ao alocar memória para wifi_event_t\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        strncpy(evt->ssid, current_ssid, MAX_SSID_LEN - 1);
        evt->ssid[MAX_SSID_LEN - 1] = '\0';

        if (xQueueSend(wifiQueue, &evt, pdMS_TO_TICKS(100)) != pdTRUE) {
            printf("Fila cheia, SSID '%s' não enviado\n", evt->ssid);
            free(evt);
        } else {
            printf("Dispositivo conectado à rede: %s\n", evt->ssid);
        }

        index = (index + 1) % total_networks;

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(3000));  // simula mudança de rede a cada 3s
    }
}

/* TASK 2: VERIFICADOR DE SEGURANÇA (CONSUMIDOR) */

void vSecurityCheckTask(void *pvParameters)
{
    wifi_event_t *evt = NULL;
    int timeout_count = 0;

    for (;;) {
        if (xQueueReceive(wifiQueue, &evt, pdMS_TO_TICKS(2000)) == pdTRUE) {
            timeout_count = 0; /* reset do contador de timeout */

            int is_safe = 0;

            /* Protege a lista de redes seguras com semáforo (mutex) */
            if (xSemaphoreTake(safeListMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                for (int i = 0; i < SAFE_WIFI_COUNT; i++) {
                    if (strcmp(evt->ssid, safe_networks[i]) == 0) {
                        is_safe = 1;
                        break;
                    }
                }
                xSemaphoreGive(safeListMutex);
            }

            if (is_safe) {
                printf("[OK] Rede segura detectada: %s\n", evt->ssid);
            } else {
                /* LOG / ALERTA quando a rede não é autorizada */
                printf("[ALERTA] Rede NÃO AUTORIZADA: %s\n", evt->ssid);
                printf("Ação recomendada: desconectar da rede e avisar o usuário.\n");
            }

            if (evt != NULL) {
                free(evt);
                evt = NULL;
            }

            esp_task_wdt_reset();
        } else {
            /* Não recebeu nada da fila dentro do timeout => robustez */
            timeout_count++;
            printf("[WARN] Timeout aguardando atualização de rede (%d)\n",
                   timeout_count);

            if (timeout_count == 3) {
                printf("[RECUPERAÇÃO] 3 timeouts seguidos, limpando fila.\n");
                xQueueReset(wifiQueue);
            } else if (timeout_count == 6) {
                printf("[RECUPERAÇÃO] 6 timeouts seguidos, reiniciando sistema.\n");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();   // estratégia de recuperação agressiva
            }

            esp_task_wdt_reset();
        }
    }
}

/* TASK 3: LOG/HEARTBEAT*/

void vHeartbeatTask(void *pvParameters)
{
    for (;;) {
        unsigned int heap = (unsigned int) esp_get_free_heap_size();
        printf("[HEARTBEAT] Monitor de redes Wi-Fi em execução. Heap livre: %u bytes\n",
               heap);

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    /* Configuração do Watchdog (técnica de robustez 1) */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);

    /* Cria fila (para ponteiros de wifi_event_t) */
    wifiQueue = xQueueCreate(5, sizeof(wifi_event_t *));
    /* Cria mutex para lista de redes seguras */
    safeListMutex = xSemaphoreCreateMutex();

    if (wifiQueue == NULL || safeListMutex == NULL) {
        printf("Erro ao criar fila ou mutex. Reiniciando...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    TaskHandle_t hMonitor, hChecker, hHeartbeat;

    xTaskCreate(vWifiMonitorTask,  "WifiMonitor",   4096, NULL, 5, &hMonitor);
    xTaskCreate(vSecurityCheckTask,"SecurityCheck", 4096, NULL, 4, &hChecker);
    xTaskCreate(vHeartbeatTask,    "Heartbeat",     2048, NULL, 3, &hHeartbeat);


    esp_task_wdt_add(hMonitor);
    esp_task_wdt_add(hChecker);
    esp_task_wdt_add(hHeartbeat);
}
