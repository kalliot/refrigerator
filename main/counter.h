#ifndef __MCOUNTER__
#define __MCOUNTER__

#include "mqtt_client.h"

extern void counter_send(int ticks, esp_mqtt_client_handle_t client);
extern void counter_init(char *prefix, uint8_t *chip, uint16_t frequency);
extern void counter_restart(uint16_t frequency);
extern uint16_t counter_getinterval();

#endif