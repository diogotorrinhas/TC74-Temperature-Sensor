#include "temp_sensor_TC74.h"
#include <stdio.h>
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <esp_http_server.h>
#include "driver/gpio.h"
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/api.h>
#include <lwip/netdb.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_spiffs.h"


#define I2C_MASTER_SCL_IO           18 //18 22
#define I2C_MASTER_SDA_IO           19 //19 21
#define I2C_MASTER_FREQ_HZ          50000
#define TC74_SENSOR_ADDR            0x4D

#define FAN_PWM_CHANNEL LEDC_CHANNEL_0
#define FAN_PWM_FREQ_HZ 5000
#define FAN_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define FAN_GPIO_PIN 5
#define LEDC_MODE LEDC_LOW_SPEED_MODE

#define ESP_WIFI_SSID "OpenLab" //"OpenLab"
#define ESP_WIFI_PASSWORD "open.LAB" //"open.LAB"
#define ESP_MAXIMUM_RETRY 5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define SPIFFS_LOG_PATH    "/spiffs/logs.txt"

static const char *TAG = "TC74";
static i2c_port_t i2c_port = I2C_NUM_0;

QueueHandle_t temperatureQueue; // Global variable for the temperature queue

char html_page[] = "<!DOCTYPE HTML><html>\n"
                   "<head>\n"
                   "  <title>Diogo/Joao Torrinhas TC74</title>\n"
                   "  <meta http-equiv=\"refresh\" content=\"1\">\n"
                   "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                   "  <link rel=\"stylesheet\" href=\"https://use.fontawesome.com/releases/v5.7.2/css/all.css\" integrity=\"sha384-fnmOCqbTlWIlj8LyTjo7mOUStjsKC4pOpQbqyi7RrhN7udi9RwhKkMHpvLbHG9Sr\" crossorigin=\"anonymous\">\n"
                   "  <link rel=\"icon\" href=\"data:,\">\n"
                   "  <style>\n"
                   "    html {font-family: Courier ; display: inline-block; text-align: center;}\n"
                   "    p {  font-size: 1.2rem;}\n"
                   "    body {  margin: 0;}\n"
                   "    .topnav { overflow: hidden; background-color: #F46142; color: white; font-size: 1.6rem; }\n"
                   "    .content { padding: 20px; }\n"
                   "    .card { background-color: white; box-shadow: 2px 2px 12px 1px rgba(140,0,0,.5); }\n"
                   "    .cards { max-width: 650px; margin: 0 auto; display: grid; grid-gap: 2rem; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }\n"
                   "    .reading { font-size: 2.8rem; }\n"
                   "    .card.temperatureC { color: #F46142; }\n"
                   "    .card.fan { color: #F46142; }\n"  // Define CSS style for fan card
                   "  </style>\n"
                   "</head>\n"
                   "<body>\n"
                   "  <div class=\"topnav\">\n"
                   "    <h3>Diogo/Joao Torrinhas TC74</h3>\n"
                   "  </div>\n"
                   "  <div class=\"content\">\n"
                   "    <div class=\"cards\">\n"
                   "      <div class=\"card temperatureC\">\n"
                   "        <h4><i class=\"fas fa-fan\"></i> Sensor Temperature</h4><p><span class=\"reading\">%u&deg;C</span></p>\n"
                   "      </div>\n"
                   "      <div class=\"card fan\">\n"  
                   "        <h4><i class=\"fas fa-fan\"></i> Fan Duty Cycle</h4><p><span class=\"reading\">%u%%</span></p>\n" 
                   "      </div>\n"
                   "    </div>\n"
                   "  </div>\n"
                   "</body>\n"
                   "</html>";

static EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;
int wifi_connect_status = 0;
uint8_t temperature = 0;
uint8_t duty = 0;
uint8_t duty_percentage = 0;
static bool dynamic = true;         //Change duty-cycle according the temperature values readed from the sensor
static bool read_logs = false;      //Variable to read logs
static bool deleteLogs = false;    //Variable to delete logs present in the file (partition)


void calculate_duty_cycle(int8_t temperature, uint8_t* duty, uint8_t* duty_percentage)
{
    if (temperature >= 28) {
        *duty = 255; // Set maximum duty cycle
    } else if (temperature <= 25) {
        *duty = 0; // Set minimum duty cycle
    } else {
        *duty = (temperature - 25) * 255 / 3; // Calculate duty cycle based on temperature
        
    }

    *duty_percentage = (*duty * 100) / 255;

    ledc_set_duty(LEDC_MODE, FAN_PWM_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, FAN_PWM_CHANNEL);
}

void read_temperature_task(void *param)
{
    TickType_t previousWakeTime;

    while (1) {
        previousWakeTime = xTaskGetTickCount();

        //Wake up the sensor
        esp_err_t err_wakeup = tc74_wakeup(i2c_port, TC74_SENSOR_ADDR, pdMS_TO_TICKS(100));
        if (err_wakeup == ESP_OK){
            //Wait for the temperature readings to be available
            while(!tc74_is_temperature_ready(i2c_port, TC74_SENSOR_ADDR, pdMS_TO_TICKS(100))); 
        }

        esp_err_t err = tc74_read_temp_after_cfg(i2c_port, TC74_SENSOR_ADDR, pdMS_TO_TICKS(100), &temperature);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "0 Temperature: %d°C", (int8_t)temperature);
            xQueueSend(temperatureQueue, &temperature, 0);

            FILE *fp = fopen(SPIFFS_LOG_PATH, "a");
            if (fp == NULL) {
                printf("Error opening data file\n");
                return;
            }
            fprintf(fp, "Temperature captured: %dºC\n", temperature);
            fclose(fp);

        } else {
            ESP_LOGE(TAG, "Failed to read temperature FIRST TIME: %d", err);
        }

        // Wake up and do the first read (it works as well!)
        // esp_err_t err = tc74_wakeup_and_read_temp(i2c_port, TC74_SENSOR_ADDR, pdMS_TO_TICKS(100), &temperature);
        // if (err == ESP_OK) {
        //     ESP_LOGI(TAG, "0 Temperature: %d°C", (int8_t)temperature);
        // } else {
        //     ESP_LOGE(TAG, "Failed to read temperature FIRST TIME: %d", err);
        // }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay for 100 ms between readings

        for (int i = 0; i < 2; i++) {
            temperature = 0;
            esp_err_t err = tc74_read_temp_after_temp(i2c_port, TC74_SENSOR_ADDR, pdMS_TO_TICKS(100), &temperature);

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "%d Temperature: %d°C", i+1, (int8_t)temperature);
                xQueueSend(temperatureQueue, &temperature, 0);

                FILE *fp = fopen(SPIFFS_LOG_PATH, "a");
                if (fp == NULL) {
                    printf("Error opening data file\n");
                    return;
                }
                fprintf(fp, "Temperature captured: %dºC\n", temperature);
                fclose(fp);

            } else {
                ESP_LOGE(TAG, "Failed to read temperature OTHER 2 TIMES: %d", err);
            }

            vTaskDelay(pdMS_TO_TICKS(100)); // Delay for 100 ms between readings
        }

        // Put the sensor into standby mode
        esp_err_t standby_result = tc74_standby(i2c_port, TC74_SENSOR_ADDR, pdMS_TO_TICKS(100));
        if (standby_result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to put sensor into standby mode: %d", standby_result);
        }

        vTaskDelayUntil(&previousWakeTime, pdMS_TO_TICKS(5000));
    }
}

void console_task(void *pvParameters) {

    char c;

    while (1) {
        // Wait for user input (character by character) using stdin
        while (scanf("%c", &c) != 1) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait for 100 ms
        }

        printf("%c\n", c);

        // Process the input character
        if (dynamic) {
            if (c == 'm') {
                // Switch to manual mode
                dynamic = false;
                printf("Switched to manual mode\n");
            } else if (c == 'n') {
                // Already in dynamic mode
                printf("Already in dynamic mode\n");

            }else if (c == 'r'){
                printf("Reading logs...\n");
                read_logs = true;

            }else if (c == 'e'){
                printf("Deleting logs...\n");
                deleteLogs = true;
            } else {
                printf("Invalid input. Please enter 'm' ,'n', 'r' or 'e'.\n");
            }

        }else { //In manual mode
            if (c == 'm') {
                // Already in manual mode
                printf("Already in manual mode\n");
            } else if (c == 'n') {
                // Switch to dynamic mode
                dynamic = true;
                printf("Switched to dynamic mode\n");
            }
            else if (c == 'l') {
                // Set duty cycle to 100 (max)
                duty = 255;
                duty_percentage = (duty * 100) / 255;
                ESP_LOGI(TAG, "(TERMINAL CHANGE) Duty: %u, Duty Percentage: %u%%", duty, duty_percentage);
                ledc_set_duty(LEDC_MODE, FAN_PWM_CHANNEL, duty);
                ledc_update_duty(LEDC_MODE, FAN_PWM_CHANNEL);

            } else if (c == 'd') {
                // Set duty cycle to 0 (min)
                duty = 0;
                duty_percentage = (duty * 100) / 255;
                ESP_LOGI(TAG, "(TERMINAL CHANGE) Duty: %u, Duty Percentage: %u%%", duty, duty_percentage);
                ledc_set_duty(LEDC_MODE, FAN_PWM_CHANNEL, duty);
                ledc_update_duty(LEDC_MODE, FAN_PWM_CHANNEL);

            } else if (c == 'r'){
                printf("Reading logs...\n");
                read_logs = true;

            }else if (c == 'e'){
                printf("Deleting logs...\n");
                deleteLogs = true;
            }else {
                printf("Invalid input. Please enter 'm', 'n', 'l', 'd', 'r' or 'e'.\n");
            }
        }
    }

    vTaskDelete(NULL);
}

void show_logs_task(void *pvParameters){
    while (1) {
        if (read_logs){
            FILE *file = fopen(SPIFFS_LOG_PATH, "r");
            char line[256];
            while (fgets(line, sizeof(line), file) != NULL) {
                printf(line);
            }
            fclose(file);
            read_logs = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay for 100 milliseconds 
    }
}

void delete_logs(void *pvParameters){
    while (1){
        if(deleteLogs){
            FILE *file;
            file = fopen(SPIFFS_LOG_PATH, "w");

            if (file == NULL) {
                printf("Failed to open the file.\n");
            }

            fclose(file);
            printf("File content cleared successfully.\n");

            deleteLogs = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay for 100 milliseconds
    }
}

void temperature_timer_callback(void *arg)
{
    xTaskCreate(read_temperature_task, "Read Temperature", 2048, NULL, 5, NULL);
}

void ledc_update_task(void *param)
{
    uint8_t temperature_aux = 0;

    while (1) {
        // Wait for a new temperature value
        if (xQueueReceive(temperatureQueue, &temperature_aux, portMAX_DELAY) == pdPASS) {
            //duty_percentage = 0;
            //ESP_LOGI(TAG, "LEDC (A) Temperature: %d°C, Duty: %d, Duty Percentage: %d%%", (int8_t)temperature_aux, duty, duty_percentage);
            if (dynamic){
                calculate_duty_cycle((int8_t)temperature_aux, &duty, &duty_percentage);
                ESP_LOGI(TAG, "LEDC (D) Temperature: %d°C, Duty: %d, Duty Percentage: %d%%", (int8_t)temperature_aux, duty, duty_percentage);
            }
        }
    }
}


static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
           
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        wifi_connect_status = 0;
        
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_connect_status = 1;
    }
}

void connect_wifi(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

   
    vEventGroupDelete(s_wifi_event_group);
}

esp_err_t send_web_page(httpd_req_t *req)
{
    int response;
    char response_data[sizeof(html_page) + 50];
    memset(response_data, 0, sizeof(response_data));
    
    ESP_LOGI(TAG, "->TEMPERATURE TO WEB PAGE: %u°C AND DUTY_PERCENTAGE: %u%%", temperature, duty_percentage);
    
    int len = snprintf(response_data, sizeof(response_data), html_page, temperature, duty_percentage);
    
    response = httpd_resp_send(req, response_data, len);
    
    return response;
}

esp_err_t get_req_handler(httpd_req_t *req)
{
    return send_web_page(req);
}

httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = get_req_handler,
    .user_ctx = NULL};

httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_get);
    }

    return server;
}


void app_main(void)
{
    esp_vfs_spiffs_conf_t spiffs_conf = {
    .base_path = "/spiffs",        // Mount path for the SPIFFS file system
    .partition_label = NULL,   // Partition label defined in the partition table
    .max_files = 5,                // Maximum number of files that can be open simultaneously
    .format_if_mount_failed = true // Format the partition if mounting fails
    };

    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted successfully");
    } else {
        return;
    }


    esp_err_t init_result = tc74_init(i2c_port, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);
    ESP_LOGI("TC74", "Init result: %d", init_result);

    temperatureQueue = xQueueCreate(100, sizeof(uint8_t)); 

    ledc_timer_config_t ledc_timer = {
        .duty_resolution = FAN_PWM_RESOLUTION,
        .freq_hz = FAN_PWM_FREQ_HZ,
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER_0
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel = FAN_PWM_CHANNEL,
        .duty = 0,
        .gpio_num = FAN_GPIO_PIN,
        .speed_mode = LEDC_MODE,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);

    if (init_result == ESP_OK) {
        xTaskCreate(console_task, "console_task", 2048, NULL, 5, NULL);
        xTaskCreate(read_temperature_task, "Read Temperature", 2048, NULL, 5, NULL);
        xTaskCreate(ledc_update_task, "LEDC Update", 2048, NULL, 5, NULL);
        xTaskCreate(show_logs_task, "Show Logs", 2048, NULL, 5, NULL);
        xTaskCreate(delete_logs, "Delete Logs", 2048, NULL, 5, NULL);
        
        connect_wifi();
        if (wifi_connect_status)
        {
            setup_server();
        }
    }
}