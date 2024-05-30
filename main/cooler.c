
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "cooler.h"

static float startTemp;
static int coolergpio;
static float hysteresis;
static char *coolerprefix;
static uint8_t *chipid;
static bool currentstate;
static char coolerTopic[64];
static int minTime = 120;
static time_t started;
static float prevTemp = 0;

static const char *TAG = "COOLER";

void cooler_init(char *prefix, uint8_t *chip, int gpio)
{
    coolerprefix = prefix;
    coolergpio = gpio;
    chipid = chip;
    gpio_reset_pin(coolergpio);
    gpio_set_direction(coolergpio, GPIO_MODE_OUTPUT);
    gpio_set_level(coolergpio, false);    
    sprintf(coolerTopic,"%s/refrigerator/%x%x%x/parameters/state/%d", prefix, chipid[3], chipid[4], chipid[5], coolergpio);
    return;
}


void cooler_setup(float start, float hyst, int mintime)
{
    startTemp = start;
    hysteresis = hyst;
    minTime = mintime;
    cooler_check(prevTemp);
}


void cooler_check(float temperature)
{
    static bool state = false;
    time_t now;

    time(&now);
    prevTemp = temperature;
    if (temperature > startTemp && state == false)
    {
        ESP_LOGI(TAG,"temperature bigger than %f -> state=true", startTemp);
        started = now;
        state = true;
    }
    else if ((temperature < (startTemp - hysteresis)) && state == true)
    {
        int runtime = now - started;
        if (runtime > minTime)
        {
            ESP_LOGI(TAG,"temperature is smaller than %f -> state=false", startTemp - hysteresis);
            state = false;
        }
        else
        {
            ESP_LOGI(TAG,"runtime is only %d seconds, keeping compressor on", runtime);
            return;
        }
    }
    else
    {
        return;
    }

    if (state != currentstate)
    {
        gpio_set_level(coolergpio, state);    
        struct measurement meas;
        meas.gpio = coolergpio;
        meas.id = STATE;
        meas.data.state = state;
        xQueueSendFromISR(evt_queue, &meas, NULL);
        currentstate = state;
    }
}


bool cooler_publish(struct measurement *data, esp_mqtt_client_handle_t client)
{
    time_t now;
    
    time(&now);
    gpio_set_level(BLINK_GPIO, true);

    static char *datafmt = "{\"dev\":\"%x%x%x\",\"id\":\"cooler\",\"value\":%d,\"ts\":%jd,\"unit\":\"state\"}";

    sprintf(jsondata, datafmt,
                chipid[3],chipid[4],chipid[5],
                data->data.state,
                now);
    esp_mqtt_client_publish(client, coolerTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
    return true;
}


void cooler_cleanup(void)
{
    gpio_set_level(BLINK_GPIO, false);
    return;
}
