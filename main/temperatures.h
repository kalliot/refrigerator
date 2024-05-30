#ifndef __TEMPERATURES__
#define __TEMPERATURES__

#include "homeapp.h"

extern bool temperature_send(char *prefix, struct measurement *data, esp_mqtt_client_handle_t client);
extern bool temperatures_init(int gpio, uint8_t *chip);
extern char *temperatures_info(void);
extern char *temperature_getsensor(int index);
extern void temperature_sendall(void);
extern bool temperature_set_friendlyname(char *sensorName, char *friendlyName);

#endif