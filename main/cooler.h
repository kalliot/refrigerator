#ifndef __COOLER__
#define __COOLER__

#include "mqtt_client.h"
#include "homeapp.h"

extern void cooler_init(char *prefix, char *name, uint8_t *chip, int gpio);
extern void cooler_setup(float start, float hysteresis, int mintime);
extern void cooler_check(float temperature);
extern bool cooler_publish(struct measurement *data, esp_mqtt_client_handle_t client);
extern void cooler_send_currentstate(void);
extern void cooler_cleanup(void);


#endif