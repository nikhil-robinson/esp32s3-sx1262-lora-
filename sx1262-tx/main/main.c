#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "ntc_driver.h"
#include "esp_random.h"
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/rtc_io.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sx1262.h"

static const char *TAG = "MAIN";

// Define GPIO pins for SPI interface and LoRa module
#define ESP32_S3_MOSI_PIN 9
#define ESP32_S3_MISO_PIN 8
#define ESP32_S3_SCK_PIN 7
#define ESP32_S3_NSS_PIN 41
#define ESP32_S3_RST_PIN 42
#define ESP32_S3_BUSY_PIN 40
#define ESP32_S3_ANTENA_SW_PIN 38

// Define GPIO pins and ADC channels for battery and temperature sensing
#define ESP32_S3_BATTERY_ADC_PIN 1
#define ESP32_S3_BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define ESP32_S3_TEMPERATURE_ADC_PIN 2
#define ESP32_S3_TEMPERATURE_ADC_CHANNEL ADC_CHANNEL_1

// Handles for ADC and NTC device
adc_oneshot_unit_handle_t adc1_handle;
ntc_device_handle_t ntc = NULL;

static struct timeval sleep_enter_time;

/**
 * @brief Initialize the ADC unit
 */
void start_adc()
{

    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };

    if (adc1_handle == NULL)
    {
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    }
}

/**
 * @brief Deinitialize the ADC unit
 */
void stop_adc()
{
    if (adc1_handle)
    {
        adc_oneshot_del_unit(adc1_handle);
        adc1_handle = NULL;
    }
}

/**
 * @brief Configure a given ADC channel
 * @param channel ADC channel to configure
 */
void adc1_config(adc_channel_t channel)
{
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, channel, &config));
}

/**
 * @brief Read ADC value from a given channel
 * @param channel ADC channel to read
 * @param val Pointer to store the read value
 */
void adc1_read(adc_channel_t channel, int *val)
{
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, channel, val));
}

/**
 * @brief Initialize device peripherals (ADC, GPIO, NTC sensor)
 */
void device_init()
{
    esp_rom_gpio_pad_select_gpio(ESP32_S3_BATTERY_ADC_PIN);
    gpio_set_direction(ESP32_S3_BATTERY_ADC_PIN, GPIO_MODE_INPUT);

    esp_rom_gpio_pad_select_gpio(ESP32_S3_TEMPERATURE_ADC_PIN);
    gpio_set_direction(ESP32_S3_TEMPERATURE_ADC_PIN, GPIO_MODE_INPUT);

    start_adc();
    ntc_config_t ntc_config = {
        .b_value = 3950,
        .r25_ohm = 10000,
        .fixed_ohm = 10000,
        .vdd_mv = 3300,
        .circuit_mode = CIRCUIT_MODE_NTC_GND,
        .atten = ADC_ATTEN_DB_11,
        .channel = ESP32_S3_TEMPERATURE_ADC_CHANNEL,
        .unit = ADC_UNIT_1};

    ESP_ERROR_CHECK(ntc_dev_create(&ntc_config, &ntc, &adc1_handle));
    // ESP_ERROR_CHECK(ntc_dev_get_adc_handle(ntc, &adc_handle));

    adc1_config(ESP32_S3_BATTERY_ADC_CHANNEL);
}

/**
 * @brief Maps a value from one range to another
 */
int map(int x, int in_min, int in_max, int out_min, int out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/**
 * @brief Task to transmit LoRa packets
 */
void task_tx(void *pvParameters)
{
    /**
     * Prefer to use RTC mem instead of NVS to save the deep sleep enter time, unless the chip
     * does not support RTC mem(such as esp32c2). Because the time overhead of NVS will cause
     * the recorded deep sleep enter time to be not very accurate.
     */
#if !SOC_RTC_FAST_MEM_SUPPORTED
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t nvs_handle;
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        printf("Open NVS done\n");
    }

    // Get deep sleep enter time
    nvs_get_i32(nvs_handle, "slp_enter_sec", (int32_t *)&sleep_enter_time.tv_sec);
    nvs_get_i32(nvs_handle, "slp_enter_usec", (int32_t *)&sleep_enter_time.tv_usec);
#endif
    struct timeval now;
    gettimeofday(&now, NULL);
    int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)
    {
        ESP_LOGI(TAG, "Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
    }
    else
    {
        ESP_LOGI(TAG, "Not a deep sleep reset\n");
    }

    ESP_LOGI(TAG, "Start");
    uint8_t buf[256]; // Maximum Payload size of SX1261/62/68 is 255
    uint32_t id = esp_random();

    TickType_t nowTick = xTaskGetTickCount();
    float temp = 0.0;
    int voltage = 0.0;
    ntc_dev_get_temperature(ntc, &temp);
    adc1_read(ESP32_S3_BATTERY_ADC_CHANNEL, &voltage);
    voltage = map(voltage, 0, 4095, 0, 100);

    int txLen = sprintf((char *)buf, "[%lu] ID %lu TEMP %f BAT %d", nowTick, id, temp, voltage);
    ESP_LOGI(TAG, "%d byte packet sent...", txLen);
    ESP_LOGI(TAG, "Payload: %s", buf);

    // Wait for transmission to complete
    if (LoRaSend(buf, txLen, SX126x_TXMODE_SYNC) == false)
    {
        ESP_LOGE(TAG, "LoRaSend fail");
    }

    // Do not wait for the transmission to be completed
    // LoRaSend(buf, txLen, SX126x_TXMODE_ASYNC );

    int lost = GetPacketLost();
    if (lost != 0)
    {
        ESP_LOGW(TAG, "%d packets lost", lost);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    gettimeofday(&sleep_enter_time, NULL);

#if !SOC_RTC_FAST_MEM_SUPPORTED
    // record deep sleep enter time via nvs
    ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, "slp_enter_sec", sleep_enter_time.tv_sec));
    ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, "slp_enter_usec", sleep_enter_time.tv_usec));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
#endif

    esp_deep_sleep_start();
}

static void example_deep_sleep_register_rtc_timer_wakeup(void)
{
    const int wakeup_time_sec = 60 *15;
    printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000));
}

/**
 * @brief Main application entry point
 */
void app_main()
{
    example_deep_sleep_register_rtc_timer_wakeup();
    device_init();
    // Initialize LoRa
    ESP_LOGI(TAG, "SX1262 LoRa Module TX Test");
    LoRaInit(ESP32_S3_MISO_PIN, ESP32_S3_MOSI_PIN, ESP32_S3_SCK_PIN, ESP32_S3_NSS_PIN, ESP32_S3_RST_PIN, ESP32_S3_BUSY_PIN, ESP32_S3_ANTENA_SW_PIN);
    int8_t txPowerInDbm = 22;

    uint32_t frequencyInHz = 866000000;
    ESP_LOGI(TAG, "Frequency is 866MHz");

    ESP_LOGW(TAG, "Enable TCXO");
    float tcxoVoltage = 3.3;     // use TCXO
    bool useRegulatorLDO = true; // use DCDC + LDO

    LoRaDebugPrint(true);
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
    uint8_t codingRate = 5;
    uint16_t preambleLength = 8;
    uint8_t payloadLen = 0;
    bool crcOn = true;
    bool invertIrq = false;
    LoRaConfig(spreadingFactor, bandwidth, codingRate, preambleLength, payloadLen, crcOn, invertIrq);

    xTaskCreate(&task_tx, "TX", 1024 * 4, NULL, 6, NULL);
}
