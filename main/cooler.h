#ifndef __COOLER__
#define __COOLER__

#include "mqtt_client.h"
#include "homeapp.h"

extern void cooler_init(char *prefix, uint8_t *chip, int gpio);
extern void cooler_settarget(float start, float hysteresis);
extern void cooler_check(float temperature);
extern bool cooler_publish(struct measurement *data, esp_mqtt_client_handle_t client);
extern void cooler_cleanup(void);


#endif