#ifndef __HOMEAPP__
#define __HOMEAPP__

#include "freertos/queue.h"

enum meastype
{
    COUNT,
    TEMPERATURE,
    STATE,
    OTA
};

struct measurement {
    enum meastype id;
    int gpio;
    union {
        int count;
        bool state;
        float temperature;
    } data;
};

extern QueueHandle_t evt_queue;
extern char jsondata[256];
extern uint16_t sendcnt;
extern uint16_t sensorerrors;

#define BLINK_GPIO         23
#define SETUP_GPIO         CONFIG_SETUPLED_GPIO
#define WLANSTATUS_GPIO    CONFIG_WLANSTATUS_GPIO
#define MQTTSTATUS_GPIO    CONFIG_MQTTSTATUS_GPIO
#define MIN_EPOCH   1650000000

#endif