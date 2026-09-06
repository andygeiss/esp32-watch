/**
 * @file net.c
 * WiFi, and the time that comes with it.
 *
 * This board has no RTC, so time(NULL) in ui.c counts from the instant the
 * chip came out of reset until something tells it otherwise. SNTP over WiFi
 * is the only thing on this board that can.
 *
 * Both are optional and off by default: with no SSID configured the radio
 * stays down, net_is_up() stays false, and the WiFi corner draws dim rather
 * than disappearing — which is exactly the reading that dimming is for.
 */

#include "net.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char * TAG = "net";

/* Written from the event task, read from the LVGL task. */
static atomic_bool link_up;

static void on_event(void * arg, esp_event_base_t base, int32_t id, void * data)
{
    (void) arg;
    (void) data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&link_up, false);
        /* A watch on a wrist walks out of range and back. Each attempt costs
         * seconds inside the driver, so asking again on every drop is a
         * retry, not a spin. */
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&link_up, true);
        ESP_LOGI(TAG, "associated");
    }
}

/* NVS holds the radio's calibration data, so WiFi needs it mounted. A version
 * change or a full partition is recoverable by wiping it: nothing in there is
 * worth keeping across a reflash. */
static void nvs_start(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

void net_start(void)
{
    /* Always, radio or not: without it localtime_r() would hand ui.c UTC. */
    setenv("TZ", CONFIG_KAI_TIMEZONE, 1);
    tzset();

    if (CONFIG_KAI_WIFI_SSID[0] == '\0') {
        ESP_LOGI(TAG, "no SSID set: radio stays off, clock runs from boot");
        return;
    }

    nvs_start();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, NULL, NULL));

    wifi_config_t config = { 0 };
    strlcpy((char *) config.sta.ssid, CONFIG_KAI_WIFI_SSID, sizeof config.sta.ssid);
    strlcpy((char *) config.sta.password, CONFIG_KAI_WIFI_PASSWORD, sizeof config.sta.password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* SNTP starts here and waits for the association by itself, then keeps
     * the clock honest for as long as the watch is up. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_KAI_SNTP_SERVER);
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    ESP_LOGI(TAG, "joining %s", CONFIG_KAI_WIFI_SSID);
}

bool net_is_up(void)
{
    return atomic_load(&link_up);
}
