/**
 * @file board.c
 * Panel and touch bring-up for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * The pin numbers, the vendor power-on sequence and the 22 px column offset
 * below are transcribed from Waveshare's own BSP for this board
 * (waveshare/esp32_s3_touch_amoled_2_06 v2.0.0 in the ESP component registry).
 * They describe one specific piece of hardware and cannot be derived from
 * anything, so they are copied rather than worked out.
 */

#include "board.h"

#include <assert.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "turn.h"  /* VOICE_RATE: the microphone opens at the gate's rate */

static const char * TAG = "board";

/* One I2C bus for three chips: the touch controller, and the two codecs.
 * board_touch_init() creates it, board_audio_init() borrows it, so the order
 * of the two calls in app_main is load-bearing. */
static i2c_master_bus_handle_t i2c;

/* QSPI to the display controller. */
#define BOARD_LCD_HOST  SPI2_HOST
#define BOARD_LCD_CS    12
#define BOARD_LCD_PCLK  11
#define BOARD_LCD_DATA0 4
#define BOARD_LCD_DATA1 5
#define BOARD_LCD_DATA2 6
#define BOARD_LCD_DATA3 7
#define BOARD_LCD_RST   8

/* I2C to the touch controller. */
#define BOARD_I2C_PORT  I2C_NUM_0
#define BOARD_I2C_SCL   14
#define BOARD_I2C_SDA   15
#define BOARD_I2C_HZ    400000
#define BOARD_TOUCH_RST 9
#define BOARD_TOUCH_INT 38

/* I2S to the two codecs, and the pin that powers the amplifier. One duplex
 * bus: the ES8311 takes DOUT and the ES7210 gives back DSIN. */
#define BOARD_I2S_PORT   I2S_NUM_0
#define BOARD_I2S_MCLK   16
#define BOARD_I2S_BCLK   41
#define BOARD_I2S_WS     45
#define BOARD_I2S_DOUT   40
#define BOARD_I2S_DSIN   42
#define BOARD_POWER_AMP  46

/* The speaker's volume and the microphone's gain, both as the vendor sets
 * them. The gain matters to the gate in voice/turn.c: it was measured against
 * a Mac's own microphone, so a wildly different level here is the first thing
 * to suspect if the watch never hears a sentence end. */
#define BOARD_SPEAKER_VOLUME 70
#define BOARD_MIC_GAIN_DB    30.0f

/* The controller's frame memory is wider than the glass in front of it:
 * column 0 of the panel is column 22 of the controller. */
#define BOARD_LCD_X_GAP 22

/* Two draw buffers, so LVGL can render the next strip while the last one is
 * still going out over DMA. They have to be internal RAM to be DMA-capable;
 * 410 x 50 x 2 bytes is 41 kB each, together about what Waveshare's own BSP
 * spends on its single buffer. */
#define BOARD_LCD_BUF_LINES 50
#define BOARD_LCD_BUF_BYTES (BOARD_LCD_H_RES * BOARD_LCD_BUF_LINES * 2)

/* The vendor's power-on sequence. The two window commands are the panel
 * describing itself: 0x2A sets columns 0x16..0x1AF and 0x2B rows 0..0x1F5,
 * which is the 410 x 502 of glass sitting inside the wider frame memory. */
static const sh8601_lcd_init_cmd_t LCD_INIT_CMDS[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},                 /* sleep out */
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},                   /* tearing effect line on */
    {0x53, (uint8_t[]){0x20}, 1, 10},                  /* brightness control on */
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},                  /* dark, until it is on */
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0}, /* columns 22..431 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0}, /* rows 0..501 */
    {0x29, (uint8_t[]){0x00}, 0, 10},                  /* display on */
    {0x51, (uint8_t[]){0xFF}, 1, 0},                   /* and up to full */
};

/* The DMA transfer is finished, so the buffer LVGL handed over is free. */
static bool flush_done(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t * event, void * display)
{
    LV_UNUSED(io);
    LV_UNUSED(event);

    lv_display_flush_ready(display);

    return false;
}

/* LVGL renders RGB565 little-endian; this panel reads it big-endian. The swap
 * is in place, before the buffer is handed to DMA. */
static void flush(lv_display_t * display, const lv_area_t * area, uint8_t * pixels)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);

    lv_draw_sw_rgb565_swap(pixels, lv_area_get_width(area) * lv_area_get_height(area));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
}

/* The controller addresses its frame memory in pairs of pixels, so a dirty
 * area with an odd edge has to grow out to the next even one. Without this a
 * narrow redraw — a digit changing, an eye blinking — lands a pixel over.
 *
 * It also decides how tall a strip can be: LVGL calls this from get_max_row()
 * with a trial area and shrinks it until the rounded height still fits the
 * draw buffer, so no strip boundary can land back on an odd row. */
static void round_area(lv_event_t * event)
{
    lv_area_t * area = lv_event_get_param(event);

    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

lv_display_t * board_display_init(void)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        BOARD_LCD_PCLK, BOARD_LCD_DATA0, BOARD_LCD_DATA1, BOARD_LCD_DATA2,
        BOARD_LCD_DATA3, BOARD_LCD_BUF_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    /* The display comes first: the panel IO takes it as the context its
     * transfer-done callback hands back to LVGL. */
    lv_display_t * display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    const esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(BOARD_LCD_CS, flush_done, display);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) BOARD_LCD_HOST,
                                             &io_config, &io));

    sh8601_vendor_config_t vendor = {
        .init_cmds = LCD_INIT_CMDS,
        .init_cmds_size = sizeof LCD_INIT_CMDS / sizeof LCD_INIT_CMDS[0],
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, BOARD_LCD_X_GAP, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    void * buffer_a = heap_caps_malloc(BOARD_LCD_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void * buffer_b = heap_caps_malloc(BOARD_LCD_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(buffer_a != NULL && buffer_b != NULL);

    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer_a, buffer_b, BOARD_LCD_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    ESP_LOGI(TAG, "panel up, %d x %d", BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    return display;
}

/* One finger is all this UI asks for: a button, and later a face to tap. */
static void touch_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    esp_lcd_touch_handle_t touch = lv_indev_get_user_data(indev);
    esp_lcd_touch_point_data_t point = { 0 };
    uint8_t count = 0;

    esp_lcd_touch_read_data(touch);

    if (esp_lcd_touch_get_data(touch, &point, &count, 1) == ESP_OK && count > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

lv_indev_t * board_touch_init(lv_display_t * display)
{
    const i2c_master_bus_config_t bus = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &i2c));

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_config.scl_speed_hz = BOARD_I2C_HZ;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c, &io_config, &io));

    const esp_lcd_touch_config_t touch_config = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RST,
        .int_gpio_num = BOARD_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(io, &touch_config, &touch));

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_user_data(indev, touch);
    lv_indev_set_read_cb(indev, touch_read);

    ESP_LOGI(TAG, "touch up");

    return indev;
}

/* ------------------------------------------------------------------ */
/* Audio.                                                                     */
/*                                                                            */
/* Two chips, both transcribed from the vendor BSP: an ES8311 driving the     */
/* speaker through the amplifier on GPIO46, and an ES7210 in front of the     */
/* microphones. They share one duplex I2S bus, which is why they share one    */
/* clock and cannot be open at once at two different rates.                   */
/* ------------------------------------------------------------------ */

static i2s_chan_handle_t i2s_tx;
static i2s_chan_handle_t i2s_rx;
static const audio_codec_data_if_t * i2s_data;
static esp_codec_dev_handle_t speaker;
static esp_codec_dev_handle_t microphone;
static bool mic_open;
static bool speaker_open;

/* One codec's control interface on the shared bus. */
static const audio_codec_ctrl_if_t * codec_ctrl(uint8_t address)
{
    audio_codec_i2c_cfg_t cfg = {
        .port = BOARD_I2C_PORT,
        .addr = address,
        .bus_handle = i2c,
    };
    return audio_codec_new_i2c_ctrl(&cfg);
}

bool board_audio_init(void)
{
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    const audio_codec_if_t * es8311;
    const audio_codec_if_t * es7210;

    /* The rate here is only the one I2S starts at: esp_codec_dev_open()
     * reconfigures the clock every time the loop opens a device, once at
     * VOICE_RATE to listen and again at whatever the reply came back as. */
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = BOARD_I2S_DSIN,
            .invert_flags = { false, false, false },
        },
    };

    if (i2c == NULL) {
        ESP_LOGE(TAG, "audio needs the I2C bus board_touch_init() makes");
        return false;
    }

    channels.auto_clear = true; /* so a stopped speaker goes quiet, not buzzy */
    if (i2s_new_channel(&channels, &i2s_tx, &i2s_rx) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(i2s_tx, &std) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(i2s_rx, &std) != ESP_OK) return false;
    if (i2s_channel_enable(i2s_tx) != ESP_OK) return false;
    if (i2s_channel_enable(i2s_rx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t data = {
        .port = BOARD_I2S_PORT,
        .rx_handle = i2s_rx,
        .tx_handle = i2s_tx,
    };
    i2s_data = audio_codec_new_i2s_data(&data);
    if (i2s_data == NULL) return false;

    /* The DAC side. pa_pin is the amplifier, which the codec driver raises
     * when the device opens and drops when it closes, so nothing hisses
     * between replies. The two voltages are the vendor's own numbers. */
    es8311_codec_cfg_t dac = {
        .ctrl_if = codec_ctrl(ES8311_CODEC_DEFAULT_ADDR),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_POWER_AMP,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    if (dac.ctrl_if == NULL) return false;
    es8311 = es8311_codec_new(&dac);
    if (es8311 == NULL) return false;

    es7210_codec_cfg_t adc = { .ctrl_if = codec_ctrl(ES7210_CODEC_DEFAULT_ADDR) };
    if (adc.ctrl_if == NULL) return false;
    es7210 = es7210_codec_new(&adc);
    if (es7210 == NULL) return false;

    esp_codec_dev_cfg_t out = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311, .data_if = i2s_data,
    };
    esp_codec_dev_cfg_t in = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210, .data_if = i2s_data,
    };
    speaker = esp_codec_dev_new(&out);
    microphone = esp_codec_dev_new(&in);
    if (speaker == NULL || microphone == NULL) return false;

    ESP_LOGI(TAG, "audio up: ES8311 out, ES7210 in");
    return true;
}

bool board_mic_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 1,
        .sample_rate = VOICE_RATE,
    };

    if (microphone == NULL) return false;
    if (mic_open) return true;

    board_speaker_close(); /* one clock, so only one of them at a time */

    if (esp_codec_dev_open(microphone, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "the microphone would not open at %d Hz mono", VOICE_RATE);
        return false;
    }
    esp_codec_dev_set_in_gain(microphone, BOARD_MIC_GAIN_DB);
    mic_open = true;
    return true;
}

void board_mic_close(void)
{
    if (microphone != NULL && mic_open) esp_codec_dev_close(microphone);
    mic_open = false;
}

size_t board_mic_read(int16_t * into, size_t samples)
{
    int bytes = (int) (samples * sizeof(int16_t));

    if (!mic_open || bytes <= 0) return 0;
    /* esp_codec_dev_read blocks until the whole buffer is full, so the caller
     * asking for one block's worth is what paces the recording loop. */
    if (esp_codec_dev_read(microphone, into, bytes) != ESP_CODEC_DEV_OK) return 0;
    return samples;
}

bool board_speaker_open(int rate, int channels, int bits)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = (uint8_t) bits,
        .channel = (uint8_t) channels,
        .channel_mask = 1,
        .sample_rate = (uint32_t) rate,
    };

    if (speaker == NULL) return false;

    board_mic_close();
    board_speaker_close();

    if (esp_codec_dev_open(speaker, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "the speaker would not open at %d Hz, %d ch, %d bit",
                 rate, channels, bits);
        return false;
    }
    esp_codec_dev_set_out_vol(speaker, BOARD_SPEAKER_VOLUME);
    speaker_open = true;
    return true;
}

bool board_speaker_write(const void * pcm, size_t bytes)
{
    if (!speaker_open || bytes == 0) return false;
    /* The signature is not const, but the driver only reads. */
    return esp_codec_dev_write(speaker, (void *) pcm, (int) bytes) == ESP_CODEC_DEV_OK;
}

void board_speaker_close(void)
{
    if (speaker != NULL && speaker_open) esp_codec_dev_close(speaker);
    speaker_open = false;
}
