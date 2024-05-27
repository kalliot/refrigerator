#include <ctype.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include <esp_http_server.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "flashmem.h"
#include "driver/gpio.h"
#include "server.h"
#include "homeapp.h"


#define ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define MAX_STA_CONN       CONFIG_ESP_MAX_STA_CONN


static const char *TAG = "wifi_AP_WEBserver";

struct form_field {
    char *formname;
    char *flashname;
    int  ftype;
};

struct form_field form_fields[] = {
    { "ssid",               "ssid",         1},
    { "password",           "password",     1},
    { "mqtt_server",        "mqtt_server",  1},
    { "mqtt_port",          "mqtt_port",    1},
    { "mqtt_topic_prefix",  "mqtt_prefix",  1},
    { "","",0}
};


static char *urlDecode(char *str) 
{
    int originalLen = strlen(str);
    char *dStr = (char *) malloc(originalLen + 1);
    char eStr[] = "00"; /* for a hex code */

    strcpy(dStr, str);
    for(int i=0;i<strlen(dStr);++i) {
      if(dStr[i] == '%') {
        if(dStr[i+1] == 0)
          return dStr;

        if(isxdigit((int) dStr[i+1]) && isxdigit((int) dStr[i+2])) {
          eStr[0] = dStr[i+1];
          eStr[1] = dStr[i+2];

          long int x = strtol(eStr, NULL, 16);
          memmove(&dStr[i+1], &dStr[i+3], strlen(&dStr[i+3])+1);
          dStr[i] = x;
        }
      }
      else if(dStr[i] == '+') { dStr[i] = ' '; }
    }
    strncpy(str, dStr, originalLen);
    free(dStr);
    return str;
}

/* An HTTP GET handler */
static esp_err_t form_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-1") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-1", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Test-Header-1: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;

    bool success = true;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[32];

            /* Get value of expected key from query string */
            for (int i=0; form_fields[i].ftype != 0; i++)
            {
                if (httpd_query_key_value(buf, form_fields[i].formname, param, sizeof(param)) == ESP_OK)
                {
                    if (form_fields[i].ftype == 1)
                    {
                        char *result = urlDecode(param);
                        ESP_LOGI(TAG, "%s=%s", form_fields[i].formname, result);
                        flash_write_str(form_fields[i].flashname, result);
                    }    
                }
                else
                {
                    ESP_LOGI(TAG, "Field %s was not set from form", form_fields[i].formname);
                    success = false;
                }
            }
            
        }
        if (success) {
            req->user_ctx = "<html><body><br><h2>Parameters saved, now reboot</h2><br></body></html>";
            flash_commitchanges();
        }    
        else
            req->user_ctx = "<html><body><br><h2>Failed, try again.</h2><br></body></html>";
        free(buf);
    }

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, strlen(resp_str));

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0)
    {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}


static const httpd_uri_t form = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = form_get_handler,
    .user_ctx  = "<html>"
                 "<body style=\"background-color:dimgray;\" style=\"color:white;\"><table>"
                 "<form action=\"/\" method=\"get\">"
                 "<tr><td>wlan ssid:</td><td><input type=\"text\" name=\"ssid\"></input></td></tr>"
                 "<tr><td>wlan password:</td><td><input type=\"text\" name=\"password\"></input></td></tr>"
                 "<tr><td>mqtt server:</td><td><input type=\"text\" value=\"test.mosquitto.org\" name=\"mqtt_server\"></input></td></tr>"
                 "<tr><td>mqtt port:</td><td><input type=\"text\" value=\"1883\" name=\"mqtt_port\"></input></td></tr>"
                 "<tr><td>mqtt topic prefix:</td><td><input type=\"text\" value=\"esp\" name=\"mqtt_topic_prefix\"></input></td></tr></table><br>"
                 "<input type=\"submit\" value=\"submit\"></input></form>"
};


esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/ URI is not available");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &form);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    httpd_stop(server);
}


static void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}


static void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
        gpio_set_level(WLANSTATUS_GPIO, true);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
        gpio_set_level(WLANSTATUS_GPIO, false);
    }
}

static esp_err_t wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s",
             ESP_WIFI_SSID, ESP_WIFI_PASS);
    return ESP_OK;
}


void server_init()
{
    httpd_handle_t server = NULL;

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_LOGI(TAG, "init softAP");
    ESP_ERROR_CHECK(wifi_init_softap());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    server = start_webserver();
}
