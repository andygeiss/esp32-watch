/**
 * @file net.c
 * WiFi, and the time that comes with it.
 *
 * main.c sets the clock from the RTC at boot; SNTP over WiFi is what corrects
 * it, and each correction goes back into the RTC from here so the next boot
 * has it.
 *
 * Both are optional and off by default: with no SSID configured the radio
 * stays down, net_is_up() stays false, and the WiFi corner draws dim rather
 * than disappearing — which is exactly the reading that dimming is for.
 *
 * With one configured, the radio is still off most of the time. It comes up
 * for two reasons only: once at boot, until SNTP has answered or
 * NET_BOOT_HOLD_MS has gone by, and whenever voice.c asks — which it does
 * when the microphone hears someone start to speak, and stops doing when
 * what they said was not for the watch or the conversation is over. A watch
 * being looked at in silence has no radio on.
 *
 * The SSID and the password are WATCH_WLAN_SSID and WATCH_WLAN_PASS, two
 * macros main/CMakeLists.txt defines out of .env rather than Kconfig
 * entries: the password is a secret, and .env is where this repository
 * keeps those on both builds. Both always exist, empty when unset.
 */

#include "net.h"
#include "board.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char * TAG = "net";

/* How long the radio stays up at boot for SNTP, when SNTP does not answer. */
#define NET_BOOT_HOLD_MS 60000

/* Written from the event task, read from the LVGL and voice tasks. */
static atomic_bool link_up;

/* The two reasons to have the radio on, and whether it is. Both holds are
 * changed only under `lock`, from the voice task and the esp_timer task,
 * so a start and a stop can never cross. */
static SemaphoreHandle_t lock;
static bool              configured;
static bool              boot_hold;
static bool              voice_hold;
static atomic_bool       started;
static esp_timer_handle_t boot_timer;

static void apply(void)
{
    const bool want = boot_hold || voice_hold;

    if (want && !atomic_load(&started)) {
        atomic_store(&started, true);
        if (esp_wifi_start() == ESP_OK) ESP_LOGI(TAG, "radio on, joining %s", WATCH_WLAN_SSID);
        else atomic_store(&started, false);
    }
    else if (!want && atomic_load(&started)) {
        atomic_store(&started, false);
        atomic_store(&link_up, false);
        esp_wifi_stop();
        ESP_LOGI(TAG, "radio off");
    }
}

static void hold(bool * which, bool on)
{
    if (!configured) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    *which = on;
    apply();
    xSemaphoreGive(lock);
}

static void boot_release(void * arg)
{
    (void) arg;
    hold(&boot_hold, false);
}

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
         * retry, not a spin — as long as somebody still wants the radio. */
        if (atomic_load(&started)) esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&link_up, true);
        ESP_LOGI(TAG, "associated");
        /* The radio is up for seconds at a time now, so SNTP is asked the
         * moment there is a network rather than on its own hourly round. */
        esp_netif_sntp_start();
    }
}

/* Runs in the SNTP task, which is why board_rtc_write() locks the bus per
 * transfer rather than assuming one caller. The boot hold is let go from the
 * timer task rather than here: stopping the radio from inside the network
 * stack's own callback is asking it to wait on itself. */
static void on_time_synced(struct timeval * tv)
{
    (void) tv;
    if (board_rtc_write(time(NULL))) ESP_LOGI(TAG, "SNTP set the clock and the RTC");
    else ESP_LOGI(TAG, "SNTP set the clock");
    esp_timer_stop(boot_timer);
    esp_timer_start_once(boot_timer, 1000 * 1000);
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
    setenv("TZ", CONFIG_WATCH_TIMEZONE, 1);
    tzset();

    if (WATCH_WLAN_SSID[0] == '\0') {
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
    strlcpy((char *) config.sta.ssid, WATCH_WLAN_SSID, sizeof config.sta.ssid);
    strlcpy((char *) config.sta.password, WATCH_WLAN_PASS, sizeof config.sta.password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));

    /* SNTP waits for the association by itself, and is asked again each time
     * the radio comes back — see IP_EVENT_STA_GOT_IP — and hands each
     * correction to the RTC, so the next boot has it without a network. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_WATCH_SNTP_SERVER);
    sntp.sync_cb = on_time_synced;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    const esp_timer_create_args_t timer = { .callback = boot_release, .name = "net-boot" };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &boot_timer));
    lock = xSemaphoreCreateMutex();
    configured = true;

    /* Up once for the clock, then down until someone speaks. */
    hold(&boot_hold, true);
    esp_timer_start_once(boot_timer, (uint64_t) NET_BOOT_HOLD_MS * 1000);
    /* Up, it may as well sleep between beacons: every request is one the
     * watch starts, so nothing waits on an incoming packet. Set after the
     * first start, because the driver takes it only once it is running. */
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

bool net_available(void)
{
    return configured;
}

void net_want(bool on)
{
    hold(&voice_hold, on);
}

bool net_wait_up(uint32_t ms)
{
    const int64_t until = esp_timer_get_time() / 1000 + ms;

    while (!atomic_load(&link_up)) {
        if (!atomic_load(&started) || esp_timer_get_time() / 1000 >= until) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

bool net_is_up(void)
{
    return atomic_load(&link_up);
}
