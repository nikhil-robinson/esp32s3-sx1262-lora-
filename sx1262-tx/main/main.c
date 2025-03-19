#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "sx1262.h"

static const char *TAG = "MAIN";

#define ESP32_S3_MOSI_PIN 9
#define ESP32_S3_MISO_PIN 8
#define ESP32_S3_SCK_PIN 7
#define ESP32_S3_NSS_PIN 41
#define ESP32_S3_RST_PIN 42
#define ESP32_S3_BUSY_PIN 40
#define ESP32_S3_ANTENA_SW_PIN 38


void task_tx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start");
    uint8_t buf[256]; // Maximum Payload size of SX1261/62/68 is 255
    while (1)
    {
        TickType_t nowTick = xTaskGetTickCount();
        int txLen = sprintf((char *)buf, "Hello World %" PRIu32, nowTick);
        ESP_LOGI(pcTaskGetName(NULL), "%d byte packet sent...", txLen);

        // Wait for transmission to complete
        if (LoRaSend(buf, txLen, SX126x_TXMODE_SYNC) == false)
        {
            ESP_LOGE(pcTaskGetName(NULL), "LoRaSend fail");
        }

        // Do not wait for the transmission to be completed
        // LoRaSend(buf, txLen, SX126x_TXMODE_ASYNC );

        int lost = GetPacketLost();
        if (lost != 0)
        {
            ESP_LOGW(pcTaskGetName(NULL), "%d packets lost", lost);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    } // end while
}

void app_main()
{
    // Initialize LoRa
    LoRaInit(ESP32_S3_MISO_PIN, ESP32_S3_MOSI_PIN, ESP32_S3_SCK_PIN, ESP32_S3_NSS_PIN, ESP32_S3_RST_PIN, ESP32_S3_BUSY_PIN, ESP32_S3_ANTENA_SW_PIN);
    int8_t txPowerInDbm = 22;

    uint32_t frequencyInHz = 433000000;
    ESP_LOGI(TAG, "Frequency is 433MHz");

    ESP_LOGW(TAG, "Disable TCXO");
    float tcxoVoltage = 0.0;      // don't use TCXO
    bool useRegulatorLDO = false; // use only LDO in all modes

    // LoRaDebugPrint(true);
    if (LoRaBegin(frequencyInHz, txPowerInDbm, tcxoVoltage, useRegulatorLDO) != 0)
    {
        ESP_LOGE(TAG, "Does not recognize the module");
        while (1)
        {
            vTaskDelay(1);
        }
    }

    uint8_t spreadingFactor = 7;
    uint8_t bandwidth = 4;
    uint8_t codingRate = 1;
    uint16_t preambleLength = 8;
    uint8_t payloadLen = 0;
    bool crcOn = true;
    bool invertIrq = false;
    LoRaConfig(spreadingFactor, bandwidth, codingRate, preambleLength, payloadLen, crcOn, invertIrq);


    xTaskCreate(&task_tx, "TX", 1024 * 4, NULL, 5, NULL);

}
