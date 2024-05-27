#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "homeapp.h"
#include "driver/pulse_cnt.h"
#include "counter.h"

#define PCNT_HIGH_LIMIT 0x7fff
static const char *TAG = "COUNTER";

static struct {
    int maxcnt;
    int *samples;
    int last_index;
    int count;
} avgs = { 0, NULL, 0 , 0};

static uint16_t prevCnt = 0;
static uint32_t prevTicks = 0;
static uint16_t interval = 10;
static char dataTopic[64];
static uint8_t *chipid;
static pcnt_unit_handle_t pcnt_unit = NULL;
static esp_timer_handle_t periodic_timer;

static void initavg(int max)
{
    if (avgs.samples != NULL) {
        free(avgs.samples);
    }
    memset(&avgs,0,sizeof(avgs));
    avgs.samples = malloc(sizeof(int) * max);
    memset(avgs.samples,0,sizeof(int) * max);
    avgs.maxcnt = max;
}


static void addavg(int sample)
{
    avgs.samples[avgs.last_index] = sample;
    avgs.last_index++;
    if (avgs.last_index == avgs.maxcnt)
        avgs.last_index = 0;
    if (avgs.count < avgs.maxcnt)
        avgs.count++;
}

static int average()
{
  int sum = 0;
  {
    for (int i=0; i < avgs.count;i++) {
        sum += avgs.samples[i];
    }
  }
  if (sum) {
    return sum / avgs.count;
  }
  return 0;
}


static void periodic_timer_callback(void* arg)
{
    int pulse_count = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(pcnt_unit, &pulse_count));

    int ticks = pulse_count - prevCnt;
    if (ticks < 0) {
        ticks = PCNT_HIGH_LIMIT - prevCnt + pulse_count;
    }
    prevCnt = pulse_count;

    struct measurement meas;

    meas.id = COUNT;
    meas.data.count = ticks;
    xQueueSend(evt_queue, &meas, 0);
}

uint16_t counter_getinterval()
{
    return interval;
}

void counter_restart(uint16_t frequency)
{
    interval = frequency;
    initavg(60 / interval);
    ESP_ERROR_CHECK(esp_timer_restart(periodic_timer, interval * 1000000)); // microseconds
}

void counter_init(char *prefix, uint8_t *chip, uint16_t frequency)
{
    pcnt_unit_config_t unit_config = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit = -1,
    };
    
    chipid = chip;
    interval = frequency;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &pcnt_unit));

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = COUNTER_GPIO,
        .level_gpio_num = -1
    };

    gpio_reset_pin(COUNTER_GPIO);
    gpio_set_direction(COUNTER_GPIO, GPIO_MODE_INPUT);
    gpio_set_intr_type(COUNTER_GPIO, GPIO_INTR_ANYEDGE);


    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_config, &pcnt_chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE));

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 10000 // 100Hz
    };

    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config));
    pcnt_unit_enable(pcnt_unit);
    pcnt_unit_start(pcnt_unit);

    initavg(60 / interval);
    sprintf(dataTopic,"%s/refrigerator/%x%x%x/parameters/counter/%d",
        prefix, chipid[3],chipid[4],chipid[5],COUNTER_GPIO);
    ESP_LOGI(TAG,"dataTopic=[%s]", dataTopic);

    const esp_timer_create_args_t periodic_timer_args = {
           .callback = &periodic_timer_callback,
           .name = "periodic"
    };

    ESP_LOGD(TAG,"creating timer");
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, interval * 1000000)); // microseconds
}



// home/kallio/clientA2601F/counter { "id":"clientA2601F","ts":1212023203233","value":56,"unit":"count"}
// to watts was count * 36

void counter_send(int ticks, esp_mqtt_client_handle_t client)
{
    time_t now;
    static time_t prevSendTs = 0;
    uint16_t avg;
    uint32_t combined = 0;

    addavg(ticks);
    avg = average();
    
    combined = (avg << 16);
    combined += ticks;

    // combined and prevTicks are using both counter and average for
    // comparison of changes.
    if (combined != prevTicks) {
        gpio_set_level(BLINK_GPIO, true);
        time(&now);

        char *datafmt = "{\"dev\":\"%x%x%x\",\"sensor\":%d,\"id\":\"counter\",\"value\":%d,\"avg1min\":%d,\"ts\":%jd,\"unit\":\"count\"}";
        if (prevSendTs && prevSendTs < (now - interval)) {
            ESP_LOGI(TAG,"sending extra message for prev 10 sec timestamp");
            sprintf(jsondata, datafmt, 
                        chipid[3],chipid[4],chipid[5],
                        COUNTER_GPIO,
                        prevTicks & 0x0000ffff,
                        avg,
                        now - interval);
            esp_mqtt_client_publish(client, dataTopic, jsondata , 0, 0, 1);
            sendcnt++;
        }    
        
        
        sprintf(jsondata, datafmt, 
                    chipid[3],chipid[4],chipid[5],
                    COUNTER_GPIO,
                    ticks,
                    avg,
                    now);
        esp_mqtt_client_publish(client, dataTopic, jsondata , 0, 0, 1);
        sendcnt++;
        prevSendTs = now;
        
        gpio_set_level(BLINK_GPIO, false);
    }
    prevTicks = combined;
}

