#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <stdlib.h>

#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "cooler.h"
#include "homeapp.h"
#include "temperatures.h"
#include "flashmem.h"
#include "ota/ota.h"
#include "mqtt_client.h"
#include "apwebserver/server.h"
#include "factoryreset.h"


#define TEMP_BUS 27
#define COOLER_BUS 16
#define STATISTICS_INTERVAL 1800
#define ESP_INTR_FLAG_DEFAULT 0


#if CONFIG_EXAMPLE_WIFI_SCAN_METHOD_FAST
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_FAST_SCAN
#elif CONFIG_EXAMPLE_WIFI_SCAN_METHOD_ALL_CHANNEL
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#endif

#if CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SIGNAL
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SECURITY
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#endif

#define WIFI_RECONNECT_RETRYCNT 50

struct netinfo {
    char *ssid;
    char *password;
    char *mqtt_server;
    char *mqtt_port;
    char *mqtt_prefix;
};


struct config {
    float temperature;
    float hysteresis;
    int mintimeon;
    float lopriceboost;
    float hipricereduce;
    char fridgesensor[20];
};

struct config setup =
{
    .temperature = 8,
    .hysteresis = 2,
    .mintimeon = 120,
    .lopriceboost = 1,
    .hipricereduce = 1
};

struct specialTemperature
{
    char timestr[11];
    float price;
};


// globals

struct specialTemperature *hitemp = NULL;
struct specialTemperature *lotemp = NULL;
int hitempcnt = 0;
int lotempcnt = 0;


struct netinfo *comminfo;
QueueHandle_t evt_queue = NULL;
char jsondata[256];
uint16_t sendcnt = 0;

static const char *TAG = "SENSORSET";
static bool isConnected = false;
static uint16_t connectcnt = 0;
static uint16_t disconnectcnt = 0;
uint16_t sensorerrors = 0;

static char statisticsTopic[64];
static char readTopic[64];
static char otaUpdateTopic[64];
static char elpriceTopic[64];
static time_t started;
static uint16_t maxQElements = 0;
static int retry_num = 0;
static char *program_version = "";
static float elpriceInfluence = 0.0;


static void sendStatistics(esp_mqtt_client_handle_t client, uint8_t *chipid, time_t now);
static void sendSetup(esp_mqtt_client_handle_t client, uint8_t *chipid);
static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid);


static char *getJsonStr(cJSON *js, char *name)
{
    cJSON *item = cJSON_GetObjectItem(js, name);
    if (item != NULL)
    {
        if (cJSON_IsString(item))
        {
            return item->valuestring;
        }
        else ESP_LOGI(TAG, "%s is not a string", name);
    }
    else ESP_LOGI(TAG,"%s not found from json", name);
    return "\0";
}

static bool getJsonFloat(cJSON *js, char *name, float *val)
{
    bool ret = false;

    cJSON *item = cJSON_GetObjectItem(js, name);
    if (item != NULL)
    {
        if (cJSON_IsNumber(item))
        {
            if (item->valuedouble != *val)
            {
                ret = true;
                *val = item->valuedouble;
                ESP_LOGI(TAG,"received variable %s:%2.2f", name, item->valuedouble);
            }
            ESP_LOGI(TAG,"%s is not changed", name);
        }
        else ESP_LOGI(TAG,"%s is not a number", name);
    }
    else ESP_LOGI(TAG,"%s not found from json", name);
    return ret;
}

static bool getJsonInt(cJSON *js, char *name, int *val)
{
    bool ret = false;

    cJSON *item = cJSON_GetObjectItem(js, name);
    if (item != NULL)
    {
        if (cJSON_IsNumber(item))
        {
            if (item->valueint != *val)
            {
                ret = true;
                *val = item->valueint;
            }
            else ESP_LOGI(TAG,"%s is not changed", name);
        }
        else ESP_LOGI(TAG,"%s is not a number", name);
    }
    else ESP_LOGI(TAG,"%s not found from json", name);
    return ret;
}


static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}


static struct specialTemperature *jsonToHiLoTable(cJSON *js, char *name, int *cnt, struct specialTemperature *tempArr)
{
    cJSON *item = cJSON_GetObjectItem(js, name);
    int itemCnt=0;

    if (item != NULL)
    {
        if (cJSON_IsArray(item))
        {
            itemCnt = cJSON_GetArraySize(item);

            // table should be allocated or reallocated, if count of lines differ.
            if (*cnt == 0 || itemCnt != *cnt) // we already had a table
            {
                *cnt = itemCnt;
                if (tempArr != NULL)
                {
                    free(tempArr);   // array sizes are different, free old one and allocate a new one.
                    tempArr = NULL;  // no array items in json.
                }
                if (itemCnt)
                {
                    tempArr = (struct specialTemperature *) malloc(itemCnt * sizeof(struct specialTemperature));
                }
            }
            if (tempArr != NULL)
            {
                for (int i=0;i<itemCnt;i++)
                {
                    cJSON *elem = cJSON_GetArrayItem(item, i);
                    if (elem != NULL)
                    {
                        cJSON *jstime = cJSON_GetObjectItem(elem, "time");
                        if (jstime != NULL)
                        {
                            if (cJSON_IsString(jstime))
                            {
                                strcpy(tempArr[i].timestr,jstime->valuestring);
                            }
                        }

                        cJSON *jsprice = cJSON_GetObjectItem(elem, "price");
                        if (jsprice != NULL)
                        {
                            if (cJSON_IsNumber(jsprice))
                            {
                                tempArr[i].price = jsprice->valuedouble;
                            }
                        }
                    }
                }
            }
        }
    }
    return tempArr;
}

static bool isInArray(char *str, struct specialTemperature *arr, int cnt)
{
    for (int i = 0; i < cnt; i++)
    {
        if (!strcmp(str,arr[i].timestr))
            return true;
    }
    return false;
}



/* ../setsetup -m '{"temperature": 8, "hysteresis": 2, "mintimeon": 120, "lopriceboost": 2, "hipricereduce", 1}'
*/

static void readSetupJson(cJSON *root)
{
    bool reinit_needed = false;

    if (getJsonFloat(root, "temperature", &setup.temperature))
    {
        flash_write_float("temperature", setup.temperature);
        reinit_needed = true;
    }
    if (getJsonFloat(root, "hysteresis", &setup.hysteresis))
    {
        flash_write_float("hysteresis", setup.hysteresis);
        reinit_needed = true;
    }
    if (getJsonInt(root, "mintimeon", &setup.mintimeon))
    {
        flash_write("mintimeon", setup.mintimeon);
        reinit_needed = true;
    }
    if (getJsonFloat(root, "lopriceboost", &setup.lopriceboost))
    {
        flash_write_float("lopriceboost", setup.lopriceboost);
        reinit_needed = true;
    }
    if (getJsonFloat(root, "lopriceboost", &setup.hipricereduce))
    {
        flash_write_float("lopriceboost", setup.hipricereduce);
        reinit_needed = true;
    }
    if (reinit_needed)
    {
        cooler_setup(setup.temperature, setup.hysteresis, setup.mintimeon);
    }
    flash_commitchanges();
}

static bool handleJson(esp_mqtt_event_handle_t event)
{
    cJSON *root = cJSON_Parse(event->data);
    bool ret = false;
    char id[20];

    if (root != NULL)
    {
        strcpy(id,getJsonStr(root,"id"));
    }
    if (!strcmp(id,"otaupdate"))
    {
        char *fname = getJsonStr(root,"file");
        if (strlen(fname) > 5)
        {
            ota_start(fname);
        }
    }
    else if (!strcmp(id,"setup"))
    {
        readSetupJson(root);
        ret = true;
    }
    else if (!strcmp(id,"sensorsetup"))
    {
        strncpy(setup.fridgesensor,getJsonStr(root,"fridgesensor"),20);
        flash_write_str("fridgesensor", setup.fridgesensor);
        ret = true;
    }
    else if (!strcmp(id,"elprice"))
    {
        char *topicpostfix = &event->topic[event->topic_len - 7];
        if (!memcmp(topicpostfix,"current",7))
        {
            float newInfluence = 0.0;
            char *daystr = getJsonStr(root,"day");
            if (isInArray(daystr, hitemp, hitempcnt))
            {
                ESP_LOGI(TAG,"HITEMP IS ON!");
                newInfluence = setup.lopriceboost;
            }
            else if (isInArray(daystr, lotemp, lotempcnt))
            {
                ESP_LOGI(TAG,"LOTEMP IS ON!");
                newInfluence = -1.0 * setup.hipricereduce;
            }
            else
            {
                ESP_LOGI(TAG,"normal temperature is on");
            }
            if (newInfluence != elpriceInfluence)
            {
                elpriceInfluence = newInfluence;
                ESP_LOGI(TAG,"changing target to %f", setup.temperature + elpriceInfluence);
                cooler_setup(setup.temperature + elpriceInfluence, setup.hysteresis, setup.mintimeon);
            }
        }
    }
    else if (!strcmp(id,"awhightemp"))
    {
        hitemp = jsonToHiLoTable(root,"values", &hitempcnt, hitemp);
    }
    else if (!strcmp(id,"awlowtemp"))
    {
        lotemp = jsonToHiLoTable(root,"values", &lotempcnt, lotemp);
    }

    cJSON_Delete(root);
    return ret;
}


/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        ESP_LOGI(TAG,"subscribing topic %s", readTopic);
        msg_id = esp_mqtt_client_subscribe(client, readTopic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, otaUpdateTopic , 0);
        ESP_LOGI(TAG, "sent subscribe %s successful, msg_id=%d", otaUpdateTopic, msg_id);

        msg_id = esp_mqtt_client_subscribe(client, elpriceTopic , 0);
        ESP_LOGI(TAG, "sent subscribe %s successful, msg_id=%d", elpriceTopic, msg_id);


        gpio_set_level(MQTTSTATUS_GPIO, true);
        isConnected = true;
        connectcnt++;
        sendInfo(client, (uint8_t *) handler_args);
        sendSetup(client, (uint8_t *) handler_args);
        temperature_sendall();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        disconnectcnt++;
        isConnected = false;
        gpio_set_level(MQTTSTATUS_GPIO, false);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        if (handleJson(event)) sendSetup(client, (uint8_t *) handler_args);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}


static void sntp_start()
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}


int getWifiStrength(void)
{
    wifi_ap_record_t ap;

    if (!esp_wifi_sta_get_ap_info(&ap))
        return ap.rssi;
    return 0;
}


//{"dev":"277998","id":"statistics","connectcnt":6,"disconnectcnt":399,"sendcnt":20186,"sensorerrors":81,"ts":1679761328}

static void sendStatistics(esp_mqtt_client_handle_t client, uint8_t *chipid, time_t now)
{
    if (now < MIN_EPOCH || started < MIN_EPOCH) return;
    gpio_set_level(BLINK_GPIO, true);

    static char *datafmt = "{\"dev\":\"%x%x%x\",\"id\":\"statistics\",\"connectcnt\":%d,\"disconnectcnt\":%d,\"sendcnt\":%d,\"sensorerrors\":%d, \"max_queued\":%d,\"ts\":%jd,\"started\":%jd,\"rssi\":%d}";
    
    sprintf(jsondata, datafmt, 
                chipid[3],chipid[4],chipid[5],
                connectcnt,
                disconnectcnt,
                sendcnt,
                sensorerrors,
                maxQElements,
                now,
                started,
                getWifiStrength());
    esp_mqtt_client_publish(client, statisticsTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}

static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    gpio_set_level(BLINK_GPIO, true);

    char infoTopic[42];

    sprintf(infoTopic,"%s/refrigerator/%x%x%x/info",
         comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"info\",\"memfree\":%d,\"idfversion\":\"%s\",\"progversion\":%s, \"tempsensors\":[%s]}",
                chipid[3],chipid[4],chipid[5],
                esp_get_free_heap_size(),
                esp_get_idf_version(),
                program_version,
                temperatures_info());
    esp_mqtt_client_publish(client, infoTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}

/* ../setsetup -m '{"temperature": 8, "hysteresis": 2, "mintimeon": 120, "lopriceboost": 2, "hipricereduce", 1}'
*/

static void sendSetup(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    gpio_set_level(BLINK_GPIO, true);

    char setupTopic[42];

    sprintf(setupTopic,"%s/refrigerator/%x%x%x/setup",
         comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"setup\",\"temperature\":%.2f,\"hysteresis\":%.2f,\"mintimeon\":%d,\"lopriceboost\":%.2f,\"hipricereduce\":%.2f }",
                chipid[3],chipid[4],chipid[5],
                setup.temperature, setup.hysteresis, setup.mintimeon, setup.lopriceboost, setup.hipricereduce);
    esp_mqtt_client_publish(client, setupTopic, jsondata , 0, 0, 1);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    sprintf(setupTopic,"%s/refrigerator/%x%x%x/sensorsetup",
         comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"sensorsetup\",\"fridgesensor\":\"%s\"}",
                chipid[3],chipid[4],chipid[5],
                setup.fridgesensor);

    esp_mqtt_client_publish(client, setupTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}

static esp_mqtt_client_handle_t mqtt_app_start(uint8_t *chipid)
{
    char client_id[128];
    char uri[64];
    
    sprintf(client_id,"client_id=%s%x%x%x",
        comminfo->mqtt_prefix ,chipid[3],chipid[4],chipid[5]);
    sprintf(uri,"mqtt://%s:%s",comminfo->mqtt_server, comminfo->mqtt_port);

    ESP_LOGI(TAG,"built client id=[%s]",client_id);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = client_id
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, chipid);
    esp_mqtt_client_start(client);
    return client;
}

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,void *event_data)
{
    if(event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG,"WIFI CONNECTING");
    }
    else if (event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG,"WiFi CONNECTED");
    }
    else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG,"WiFi lost connection");
        gpio_set_level(WLANSTATUS_GPIO, false);
        if(retry_num < WIFI_RECONNECT_RETRYCNT){
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG,"Retrying to Connect");
        }
    }
    else if (event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG,"Wifi got IP\n");
        gpio_set_level(WLANSTATUS_GPIO, true);
        retry_num = 0;
        ota_cancel_rollback();
    }
}


void wifi_connect(char *ssid, char *password)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.rssi = -127,
            .threshold.authmode = WIFI_AUTH_OPEN,
        }
    };
    strcpy((char*)wifi_configuration.sta.ssid, ssid);
    strcpy((char*)wifi_configuration.sta.password, password);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_connect();
}


struct netinfo *get_networkinfo()
{
    static struct netinfo ni;
    char *default_ssid = "XXXXXXXX";

    ni.ssid = flash_read_str("ssid",default_ssid, 20);
    if (!strcmp(ni.ssid,"XXXXXXXX"))
        return NULL;

    ni.password    = flash_read_str("password","pass", 20);
    ni.mqtt_server = flash_read_str("mqtt_server","test.mosquitto.org", 20);
    ni.mqtt_port   = flash_read_str("mqtt_port","1883", 6);
    ni.mqtt_prefix = flash_read_str("mqtt_prefix","home/esp", 20);
    return &ni;
}


void app_main(void)
{
    uint8_t chipid[8];
    time_t now, prevStatsTs;
    esp_efuse_mac_get_default(chipid);

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gpio_reset_pin(BLINK_GPIO);
    gpio_reset_pin(WLANSTATUS_GPIO);
    gpio_reset_pin(SETUP_GPIO);
    gpio_reset_pin(MQTTSTATUS_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(WLANSTATUS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(SETUP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(MQTTSTATUS_GPIO, GPIO_MODE_OUTPUT);

    flash_open("storage");
    comminfo = get_networkinfo();
    if (comminfo == NULL)
    {
        gpio_set_level(SETUP_GPIO, true);
        server_init();
    }
    else
    {
        gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
        factoryreset_init();
        wifi_connect(comminfo->ssid, comminfo->password);
        evt_queue = xQueueCreate(10, sizeof(struct measurement));
        temperatures_init(TEMP_BUS, chipid);
        cooler_init(comminfo->mqtt_prefix, chipid, COOLER_BUS);

        setup.temperature  = flash_read_float("temperature", setup.temperature);
        setup.hysteresis   = flash_read_float("hysteresis", setup.hysteresis);
        setup.mintimeon    = flash_read("mintimeon", setup.mintimeon);
        setup.lopriceboost = flash_read_float("lopriceboost", setup.lopriceboost);
        setup.hipricereduce= flash_read_float("hipricereduce", setup.hipricereduce);
        strcpy(setup.fridgesensor, flash_read_str("fridgesensor", setup.fridgesensor,20));
        cooler_setup(setup.temperature, setup.hysteresis, setup.mintimeon);

        esp_mqtt_client_handle_t client = mqtt_app_start(chipid);
        sntp_start();
        ESP_LOGI(TAG, "[APP] All init done, app_main, last line.");

        sprintf(statisticsTopic,"%s/refrigerator/%x%x%x/statistics",
            comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);
        ESP_LOGI(TAG,"statisticsTopic=[%s]", statisticsTopic);

        sprintf(otaUpdateTopic,"%s/refrigerator/%x%x%x/otaupdate",
            comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);

        sprintf(readTopic,"%s/refrigerator/%x%x%x/setsetup",
            comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);

        sprintf(elpriceTopic,"%s/elprice/#", comminfo->mqtt_prefix);

        program_version = ota_init(comminfo->mqtt_prefix, "refrigerator", chipid);
        // it is very propable, we will not get correct timestamp here.
        // It takes some time to get correct timestamp from ntp.
        time(&started);
        prevStatsTs = now = started;


        ESP_LOGI(TAG, "gpios: mqtt=%d wlan=%d",MQTTSTATUS_GPIO,WLANSTATUS_GPIO);
        while (1)
        {
            struct measurement meas;
            // send statistics after 4 hours, if nothing happens.
            // this is typical if we have only slow changing state sensor

            if(xQueueReceive(evt_queue, &meas, STATISTICS_INTERVAL * 1000 / portTICK_PERIOD_MS)) {
                time(&now);
                uint16_t qcnt = uxQueueMessagesWaiting(evt_queue);
                if (started < MIN_EPOCH)
                {
                    prevStatsTs = started = now;
                    if (isConnected) sendStatistics(client, chipid , now);
                }
                if (qcnt > maxQElements)
                {
                    maxQElements = qcnt;
                }
                if (now - prevStatsTs >= STATISTICS_INTERVAL) {
                    if (isConnected) sendStatistics(client, chipid, now);
                    prevStatsTs = now;
                }
                switch (meas.id) {
                    case TEMPERATURE:
                        if (!strcmp(setup.fridgesensor,temperature_getsensor(meas.gpio)))
                        {
                            cooler_check(meas.data.temperature);
                        }
                        if (isConnected) temperature_send(comminfo->mqtt_prefix, &meas, client);
                    break;

                    case STATE:
                        if (isConnected) cooler_publish(&meas, client);
                    break;

                    case OTA:
                        ota_status_publish(&meas, client);
                        sendcnt++;
                    break;

                    default:
                        ESP_LOGD(TAG, "unknown data type" );
                }
            }
            else
            {   // timeout
                ESP_LOGI(TAG,"timeout");
                time(&now);
                if (isConnected) sendStatistics(client, chipid, now);
                prevStatsTs = now;
            }
        }
    }
    cooler_cleanup();
}
