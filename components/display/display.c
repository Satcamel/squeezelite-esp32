/* 
 *  (c) Philippe G. 2019, philippe_44@outlook.com
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "globdefs.h"
#include "platform_config.h"
#include "tools.h"
#include "display.h"
#include "services.h"
#include "gds.h"
#include "gds_default_if.h"
#include "gds_draw.h"
#include "gds_text.h"
#include "gds_font.h"
#include "gds_image.h"

static const char *TAG = "display";

#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

#define DISPLAYER_STACK_SIZE 	(3*1024)
#define SCROLLABLE_SIZE			384
#define HEADER_SIZE				64
#define	DEFAULT_SLEEP			3600
#define ARTWORK_BORDER			1

#define WIFI_BARS				4
#define WIFI_ICON_WIDTH			(WIFI_BARS * (layout.icon.bar_width + layout.icon.gap) - layout.icon.gap)
#define WIFI_ICON_HEIGHT		(WIFI_BARS * layout.icon.step)
#define BT_ICON_WIDTH			(WIFI_ICON_HEIGHT / 2 + 1)
#define ICON_GAP				(layout.icon.step * 3 + 1)	// 7 px on small screens, 10 px on large ones
#define WIFI_ICON_RESERVE		(WIFI_ICON_WIDTH + ICON_GAP + BT_ICON_WIDTH + 2)
#define WIFI_POLL_MS			3000
#define WIFI_RSSI_SMOOTH		0.3f
#define WIFI_RSSI_HYSTERESIS	3
#define WIFI_STACK_SIZE			(3*1024)

extern const uint8_t default_artwork[]   asm("_binary_note_jpg_start");
extern const uint8_t boot_logo[]         asm("_binary_bootlogo_jpg_start");

static EXT_RAM_ATTR struct {
	TaskHandle_t task;
	SemaphoreHandle_t mutex;
	int pause, speed, by;
	enum { DISPLAYER_DOWN, DISPLAYER_IDLE, DISPLAYER_ACTIVE } state;
	char header[HEADER_SIZE + 1];
	char string[SCROLLABLE_SIZE + 1];
	int offset, boundary;
	char *metadata_config;
	bool timer, refresh;
	uint32_t elapsed;
	struct {
		uint32_t value;
		char string[8]; // H:MM:SS
		bool visible;
	} duration;
	struct {
		bool enable, active;
		bool fit;
		bool updated;
		int tick;
		int offset;
	}  artwork;
	TickType_t tick;
} displayer;

// positions depending on the screen: small OLED (e.g. 128x64) or large color one (e.g. 320x240)
static EXT_RAM_ATTR struct {
	bool big;						// artwork under the title, time & bar on one row at the bottom
	int time_top, time_bottom;		// area holding track time and progress bar
	int time_y, bar_y, bar_height;
	int artwork_y, artwork_bottom;	// artwork area when not drawn beside the text
	const struct GDS_FontDef *font_small;	// for track time
	struct {
		int y, bar_width, gap, step;	// bar i is (i + 1) * step high
	} icon;
} layout;

static const char *known_drivers[] = {"SH1106",
        "SH1122",
		"SSD1306",
		"SSD1322",
		"SSD1326",
		"SSD1327",
		"SSD1675",
		"SSD1351",
		"ST7735",
		"ST7789",
		"ILI9341",
		NULL
	};
    
static void displayer_task(void *args);
static void display_sleep(void);
static void wifi_icon_task(void *args);
static void wifi_icon_overlay(struct GDS_Device *Device);

// -1 means not connected, otherwise number of bars 0..WIFI_BARS
static int wifi_level = -1;
// see display_set_bt_status()
static int bt_status = -1;

struct GDS_Device *display;   
extern GDS_DetectFunc SSD1306_Detect, SSD132x_Detect, SH1106_Detect, SH1122_Detect, SSD1675_Detect, SSD1322_Detect, SSD1351_Detect, ST77xx_Detect, ILI9341_Detect;
GDS_DetectFunc *drivers[] = { SH1106_Detect, SH1122_Detect, SSD1306_Detect, SSD132x_Detect, SSD1675_Detect, SSD1322_Detect, SSD1351_Detect, ST77xx_Detect, ILI9341_Detect, NULL };

/****************************************************************************************
 * 
 */
void display_init(char *welcome) {
	bool init = false;
	char *config = config_alloc_get_str("display_config", CONFIG_DISPLAY_CONFIG, "N/A");
	
	int width = -1, height = -1, backlight_pin = -1;
	char *drivername = strstr(config, "driver");

	PARSE_PARAM(config, "width", '=', width);
	PARSE_PARAM(config, "height", '=', height);
	PARSE_PARAM(config, "back", '=', backlight_pin);
		
	// query drivers to see if we have a match
	ESP_LOGI(TAG, "Trying to configure display with %s", config);
	if (backlight_pin >= 0) {
		struct GDS_BacklightPWM PWMConfig = { .Channel = pwm_system.base_channel++, .Timer = pwm_system.timer, .Max = pwm_system.max, .Init = false	};
		display = GDS_AutoDetect(drivername, drivers, &PWMConfig);
	} else {
		display = GDS_AutoDetect(drivername, drivers, NULL);
	}

	// so far so good
	if (display && width > 0 && height > 0) {
		int RST_pin = -1;
		PARSE_PARAM(config, "reset", '=', RST_pin);
		
		// Detect driver interface
		if (strcasestr(config, "I2C") && i2c_system_port != -1) {
			int address = 0x3C;
				
			PARSE_PARAM(config, "address", '=', address);
				
			init = true;
			GDS_I2CInit( i2c_system_port, -1, -1, i2c_system_speed ) ;
			GDS_I2CAttachDevice( display, width, height, address, RST_pin, backlight_pin );
		
			ESP_LOGI(TAG, "Display is I2C on port %u", address);
		} else if (strcasestr(config, "SPI") && spi_system_host != -1) {
			int CS_pin = -1, speed = 0, mode = 0;
		
			PARSE_PARAM(config, "cs", '=', CS_pin);
			PARSE_PARAM(config, "speed", '=', speed);
			PARSE_PARAM(config, "mode", '=', mode);
		
			init = true;
			GDS_SPIInit( spi_system_host, spi_system_dc_gpio );
			GDS_SPIAttachDevice( display, width, height, CS_pin, RST_pin, backlight_pin, speed, mode );
				
			ESP_LOGI(TAG, "Display is SPI host %u with cs:%d", spi_system_host, CS_pin);
		} else {
			display = NULL;
			ESP_LOGI(TAG, "Unsupported display interface or serial link not configured");
		}
	} else {
		display = NULL;
		ESP_LOGW(TAG, "No display driver");
	}	
	
	if (init) {
		static DRAM_ATTR StaticTask_t xTaskBuffer __attribute__ ((aligned (4)));
		static EXT_RAM_ATTR StackType_t xStack[DISPLAYER_STACK_SIZE] __attribute__ ((aligned (4)));
		struct GDS_Layout Layout = {
			.HFlip = strcasestr(config, "HFlip"), 
			.VFlip = strcasestr(config, "VFlip"), 
			.Rotate = strcasestr(config, "rotate"), 
			.Invert = strcasestr(config, "invert"),
			.ColorSwap = strcasestr(config, "cswap"),
		};	

		GDS_SetLayout(display, &Layout);
		GDS_SetFont(display, &Font_line_2);

		// large (color) screens show a logo, others the welcome text
		if (width >= 240 && height >= 160) {
			GDS_ClearExt(display, true);
			GDS_DrawJPEG(display, (uint8_t*) boot_logo, 0, 0, GDS_IMAGE_CENTER | GDS_IMAGE_FIT);
			GDS_Update(display);
		} else {
			GDS_TextPos(display, GDS_FONT_DEFAULT, GDS_TEXT_CENTERED, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, welcome);
		}

		// start the task that will handle scrolling & counting
		displayer.mutex = xSemaphoreCreateMutex();
		displayer.by = 2;
		displayer.pause = 3600;
		displayer.speed = 33;
		displayer.task = xTaskCreateStatic( (TaskFunction_t) displayer_task, "common_displayer", DISPLAYER_STACK_SIZE, NULL, ESP_TASK_PRIO_MIN + 1, xStack, &xTaskBuffer);
		
		// set lines for "fixed" text mode, large screens get more room between lines
		layout.big = width >= 240 && height >= 160;

		if (layout.big) {
			GDS_TextSetFont(display, 1, &Font_ubuntu_14, 3);
			GDS_TextSetFont(display, 2, &Font_ubuntu_24, 4);
			layout.font_small = &Font_ubuntu_14;
			// line 2 ends at 45, then artwork (~170 pixels high) and the time/bar row at the bottom
			layout.time_top = height - 20;
			layout.time_y = height - 17;
			layout.bar_y = layout.time_y + 3;
			layout.bar_height = 7;
			layout.time_bottom = height - 1;
			layout.artwork_y = 48;
			layout.artwork_bottom = layout.time_top - 3;
			layout.icon.y = 3;
			layout.icon.bar_width = 3;
			layout.icon.gap = 2;
			layout.icon.step = 3;
		} else {
			GDS_TextSetFontAuto(display, 1, GDS_FONT_LINE_1, -3);
			GDS_TextSetFontAuto(display, 2, GDS_FONT_LINE_2, -3);
			layout.font_small = &Font_line_1;
			layout.time_top = height / 2;
			layout.time_y = layout.time_top + 3;
			layout.bar_y = height - 6;
			layout.bar_height = 5;
			layout.time_bottom = height - 1;
			layout.artwork_y = 32;
			layout.artwork_bottom = height - 1;
			layout.icon.y = 0;
			layout.icon.bar_width = 2;
			layout.icon.gap = 1;
			layout.icon.step = 2;
		}

		// wifi signal icon in the top-right corner, kept up to date by its own task
		// (internal stack as it calls the wifi driver)
		if (width >= 64 && height >= 32) {
			GDS_SetOverlay(display, wifi_icon_overlay, WIFI_ICON_RESERVE);
			xTaskCreate( (TaskFunction_t) wifi_icon_task, "wifi_icon", WIFI_STACK_SIZE, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
		}
		
		displayer.metadata_config = config_alloc_get(NVS_TYPE_STR, "metadata_config");
		
		// leave room for artwork is display is horizontal-style
		if (strcasestr(displayer.metadata_config, "artwork")) {
			displayer.artwork.enable = true;
			displayer.artwork.fit = true;
			if (height <= 64 && width > height * 2) displayer.artwork.offset = width - height - ARTWORK_BORDER;
			PARSE_PARAM(displayer.metadata_config, "artwork", ':', displayer.artwork.fit);
		}	
        
        // and finally register ourselves to power off upon deep sleep
        services_sleep_setsuspend(display_sleep);
	}
	
	free(config);
}

/****************************************************************************************
 * 
 */
static void display_sleep(void) {
    GDS_DisplayOff(display);
}

/****************************************************************************************
 * Color from RGB for whatever the screen supports (white on monochrome ones). Like the
 * JPEG scalers in gds_image.c, blue goes in the high bits (drivers set their BGR mode)
 */
static int make_color(struct GDS_Device *Device, uint8_t r, uint8_t g, uint8_t b) {
	switch (GDS_GetMode(Device)) {
	case GDS_RGB565: return ((b & 0xf8) << 8) | ((g & 0xfc) << 3) | (r >> 3);
	case GDS_RGB888: return (b << 16) | (g << 8) | r;
	case GDS_RGB666: return ((b >> 2) << 12) | ((g >> 2) << 6) | (r >> 2);
	case GDS_RGB555: return ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3);
	case GDS_RGB444: return ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4);
	case GDS_RGB332: return (b & 0xe0) | ((g & 0xe0) >> 3) | (r >> 6);
	case GDS_GRAYSCALE: return ((r * 30 + g * 59 + b * 11) / 100) >> (8 - GDS_GetDepth(Device));
	default: return GDS_COLOR_WHITE;
	}
}

/****************************************************************************************
 * Bluetooth rune in a box of BT_ICON_WIDTH x WIFI_ICON_HEIGHT at x0,y0
 */
static void bt_icon_draw(struct GDS_Device *Device, int x0, int y0, int Color, bool bold) {
	int h = WIFI_ICON_HEIGHT, cx = x0 + BT_ICON_WIDTH / 2, half = BT_ICON_WIDTH / 2;
	int top = y0, bottom = y0 + h - 1, q1 = y0 + h / 4, q3 = y0 + (3 * h) / 4 - 1;

	// bold: same rune drawn twice, one pixel apart
	for (int dx = 0; dx <= (bold ? 1 : 0); dx++) {
		GDS_DrawLine(Device, cx + dx, top, cx + dx, bottom, Color);
		GDS_DrawLine(Device, cx + dx, top, cx + half + dx, q1, Color);
		GDS_DrawLine(Device, cx + half + dx, q1, cx - half + dx, q3, Color);
		GDS_DrawLine(Device, cx + dx, bottom, cx + half + dx, q3, Color);
		GDS_DrawLine(Device, cx + half + dx, q3, cx - half + dx, q1, Color);
	}
}

/****************************************************************************************
 * Draw wifi bars in the top-right corner, bluetooth on their left. Called by GDS right
 * before each update, so whoever owns the display, the icons stay on top.
 */
static void wifi_icon_overlay(struct GDS_Device *Device) {
	int x0 = GDS_GetWidth(Device) - WIFI_ICON_WIDTH;
	int y0 = layout.icon.y, bottom = y0 + WIFI_ICON_HEIGHT - 1;
	int level = wifi_level, bt_x = x0 - ICON_GAP - BT_ICON_WIDTH;

	GDS_ClearWindow(Device, bt_x - 1, y0, x0 + WIFI_ICON_WIDTH - 1, bottom, GDS_COLOR_BLACK);

	// bluetooth: bright blue and bold when a device is connected, thin grey while waiting
	// (monochrome: only when connected)
	if (bt_status == 1) bt_icon_draw(Device, bt_x, y0, make_color(Device, 0x28, 0xa0, 0xff), true);
	else if (bt_status == 0 && GDS_GetMode(Device) != GDS_MONO) bt_icon_draw(Device, bt_x, y0, make_color(Device, 0x70, 0x70, 0x70), false);

	for (int i = 0; i < WIFI_BARS; i++) {
		int x = x0 + i * (layout.icon.bar_width + layout.icon.gap);
		int h = (i + 1) * layout.icon.step;

		// active bars are filled, inactive ones are only a baseline dot
		if (i < level) GDS_DrawBox(Device, x, bottom - h + 1, x + layout.icon.bar_width - 1, bottom, GDS_COLOR_WHITE, true);
		else GDS_DrawHLine(Device, x, bottom, layout.icon.bar_width, GDS_COLOR_WHITE);
	}

	// not connected: small cross on the left of the bars
	if (level < 0) {
		int size = WIFI_ICON_HEIGHT / 2;
		GDS_DrawLine(Device, x0, y0, x0 + size - 1, y0 + size - 1, GDS_COLOR_WHITE);
		GDS_DrawLine(Device, x0, y0 + size - 1, x0 + size - 1, y0, GDS_COLOR_WHITE);
	}
}

/****************************************************************************************
 * Called by the bluetooth sink on (dis)connection
 */
void display_set_bt_status(int status) {
	if (status == bt_status) return;
	bt_status = status;
	if (!display) return;
	GDS_SetDirty(display);
	GDS_Update(display);
}

/****************************************************************************************
 * Poll wifi signal strength and refresh display when the number of bars changes
 */
static int wifi_rssi_to_level(float rssi) {
	// minimum rssi for 1..4 bars
	static const int thresholds[WIFI_BARS] = { -85, -75, -65, -55 };
	int level = 0;
	while (level < WIFI_BARS && rssi >= thresholds[level]) level++;
	return level;
}

static void wifi_icon_task(void *args) {
	float rssi = 0;

	while (1) {
		wifi_ap_record_t ap;
		int level = -1;

		if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
			// smooth readings, restart from the raw value after (re)connection
			rssi = wifi_level < 0 ? ap.rssi : rssi * (1 - WIFI_RSSI_SMOOTH) + ap.rssi * WIFI_RSSI_SMOOTH;

			// hysteresis: only move when clearly past a threshold, so it does not flicker at the edge
			int up = wifi_rssi_to_level(rssi - WIFI_RSSI_HYSTERESIS), down = wifi_rssi_to_level(rssi + WIFI_RSSI_HYSTERESIS);
			if (wifi_level < 0) level = wifi_rssi_to_level(rssi);
			else if (up > wifi_level) level = up;
			else if (down < wifi_level) level = down;
			else level = wifi_level;
		}

		if (level != wifi_level) {
			ESP_LOGI(TAG, "wifi level %d", level);
			wifi_level = level;
			GDS_SetDirty(display);
			GDS_Update(display);
		}

		vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
	}
}

/****************************************************************************************
 * Tall displays have room below the title (line 2) for the track time, unless a
 * small one draws artwork there (large ones fit both)
 */
static bool time_below(void) {
	return layout.big || (GDS_GetHeight(display) >= 64 && !(displayer.artwork.active && !displayer.artwork.offset));
}

static void format_time(char *buf, uint32_t seconds) {
	if (seconds < 3600) sprintf(buf, "%u:%02u", seconds / 60, seconds % 60);
	else sprintf(buf, "%u:%02u:%02u", (seconds / 3600) % 100, (seconds % 3600) / 60, seconds % 60);
}

/****************************************************************************************
 * Elapsed time on the left, duration on the right and a progress bar underneath
 */
static void draw_time_below(uint32_t elapsed) {
	int width = GDS_GetWidth(display), bar = layout.bar_y;
	uint32_t duration = displayer.duration.value;
	char buf[12];

	int x0 = 0, x1 = width - 1;

	GDS_ClearWindow(display, 0, layout.time_top, -1, layout.time_bottom, GDS_COLOR_BLACK);
	GDS_SetFont(display, layout.font_small);

	format_time(buf, elapsed);
	GDS_FontDrawString(display, 0, layout.time_y, buf, GDS_COLOR_WHITE);
	// on large screens the bar sits between both times (fixed start so it does not move)
	if (layout.big) x0 = max(GDS_FontMeasureString(display, buf), GDS_FontMeasureString(display, "00:00")) + 6;

	if (duration) {
		format_time(buf, duration);
		int w = GDS_FontMeasureString(display, buf);
		GDS_FontDrawString(display, width - w - 1, layout.time_y, buf, GDS_COLOR_WHITE);
		if (layout.big) x1 = width - w - 8;

		// outlined bar, filled 1 pixel inside
		int fill = (x1 - x0 - 3) * min(elapsed, duration) / duration;
		GDS_DrawBox(display, x0, bar, x1, bar + layout.bar_height - 1, GDS_COLOR_WHITE, false);
		if (fill) GDS_DrawBox(display, x0 + 2, bar + 2, x0 + 1 + fill, bar + layout.bar_height - 3, GDS_COLOR_WHITE, true);
	}

	GDS_Update(display);
}

/****************************************************************************************
 * This is not thread-safe as displayer_task might be in the middle of line drawing
 * but it won't crash (I think) and making it thread-safe would be complicated for a
 * feature which is secondary (the LMS version of scrolling is thread-safe)
 */
static void displayer_task(void *args) {
	int scroll_sleep = 0, timer_sleep;
		
	while (1) {
		// suspend ourselves if nothing to do
		if (displayer.state < DISPLAYER_ACTIVE) {
			if (displayer.state == DISPLAYER_IDLE && layout.big) {
				// nothing playing anymore: back to the boot logo instead of a stale title
				GDS_ClearExt(display, true);
				GDS_DrawJPEG(display, (uint8_t*) boot_logo, 0, 0, GDS_IMAGE_CENTER | GDS_IMAGE_FIT);
				GDS_Update(display);
			} else if (displayer.state == DISPLAYER_IDLE) {
				// stale track time would be misleading once idle
				if (time_below()) GDS_ClearWindow(display, 0, layout.time_top, -1, layout.time_bottom, GDS_COLOR_BLACK);
				GDS_TextLine(display, 2, 0, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, displayer.string);
			}
			vTaskSuspend(NULL);
			scroll_sleep = 0;
			GDS_ClearExt(display, true);
			GDS_TextLine(display, 1, GDS_TEXT_LEFT, GDS_TEXT_UPDATE, displayer.header);
		} else if (displayer.refresh) {
			// little trick when switching master while in IDLE and missing it
			GDS_TextLine(display, 1, GDS_TEXT_LEFT, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, displayer.header);	
			displayer.refresh = false;			
		}
		
		// we have been waken up before our requested time
		if (scroll_sleep <= 10) {
			// something to scroll (or we'll wake-up every pause ms ... no big deal)
			if (*displayer.string && displayer.state == DISPLAYER_ACTIVE) {
				xSemaphoreTake(displayer.mutex, portMAX_DELAY);
				
				// need to work with local copies as we don't want to suspend caller
				int offset = -displayer.offset;
				char *string = strdup(displayer.string);
				scroll_sleep = displayer.offset ? displayer.speed : displayer.pause;
				displayer.offset = displayer.offset >= displayer.boundary ? 0 : (displayer.offset + min(displayer.by, displayer.boundary - displayer.offset));			
				
				xSemaphoreGive(displayer.mutex);				
				
				// now display using safe copies, can be lengthy
				GDS_TextLine(display, 2, offset, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, string);
				free(string);
			} else {
				scroll_sleep = DEFAULT_SLEEP;
			}	
		}	
		
		// handler elapsed track time
		if (displayer.timer && displayer.state == DISPLAYER_ACTIVE) {
			char line[19] = "-", *_line = line + 1; // [-]H:MM:SS / H:MM:SS
			TickType_t tick = xTaskGetTickCount();
			uint32_t elapsed = (tick - displayer.tick) * portTICK_PERIOD_MS;

			if (elapsed >= 1000) {
				xSemaphoreTake(displayer.mutex, portMAX_DELAY);
				displayer.tick = tick;
				elapsed = displayer.elapsed += elapsed / 1000;
				xSemaphoreGive(displayer.mutex);

				// room below the title: elapsed, duration and progress bar there, line 1 untouched
				if (time_below()) {
					draw_time_below(elapsed);
				} else {
					// when we have duration but no space, display remaining time
					if (displayer.duration.value && !displayer.duration.visible) elapsed = displayer.duration.value - elapsed;

					if (elapsed < 3600) sprintf(_line, "%u:%02u", elapsed / 60, elapsed % 60);
					else sprintf(_line, "%u:%02u:%02u", (elapsed / 3600) % 100, (elapsed % 3600) / 60, elapsed % 60);

					// concatenate if we have room for elapsed / duration
					if (displayer.duration.visible) {
						strcat(_line, "/");
						strcat(_line, displayer.duration.string);
					} else if (displayer.duration.value) {
						_line--;
					}

					// just re-write the whole line it's easier
					GDS_TextLine(display, 1, GDS_TEXT_LEFT, GDS_TEXT_CLEAR, displayer.header);
					GDS_TextLine(display, 1, GDS_TEXT_RIGHT, GDS_TEXT_UPDATE, _line);
				}
				
				// if we have not received artwork after 5s, display a default icon
				if (displayer.artwork.active && !displayer.artwork.updated && tick - displayer.artwork.tick > pdMS_TO_TICKS(5000)) {
					ESP_LOGI(TAG, "no artwork received, setting default");
					displayer_artwork((uint8_t*) default_artwork);
				}	
				timer_sleep = 1000;
			} else timer_sleep = max(1000 - elapsed, 0);	
		} else timer_sleep = DEFAULT_SLEEP;
		
		// then sleep the min amount of time
		int sleep = min(scroll_sleep, timer_sleep);
		ESP_LOGD(TAG, "timers s:%d t:%d", scroll_sleep, timer_sleep);
		scroll_sleep -= sleep;
		vTaskDelay(sleep / portTICK_PERIOD_MS);
	}
}	

/****************************************************************************************
 * 
 */
void displayer_artwork(uint8_t *data) {
	if (!displayer.artwork.active) return;
	
	int x = displayer.artwork.offset ? displayer.artwork.offset + ARTWORK_BORDER : 0;
	int y = x ? 0 : layout.artwork_y;
	int bottom = x ? GDS_GetHeight(display) - 1 : layout.artwork_bottom;
	GDS_ClearWindow(display, x, y, -1, bottom, GDS_COLOR_BLACK);
	if (data) {
		displayer.artwork.updated = true;
		GDS_DrawJPEGArea(display, data, x, y, GDS_GetWidth(display) - x, bottom - y + 1, GDS_IMAGE_CENTER | (displayer.artwork.fit ? GDS_IMAGE_FIT : 0));
	} else {
		displayer.artwork.updated = false;
		displayer.artwork.tick = xTaskGetTickCount();
	}	
	
}

/****************************************************************************************
 * 
 */
bool displayer_can_artwork(void) {
	return displayer.artwork.active;
}

/****************************************************************************************
 * 
 */
void displayer_metadata(char *artist, char *album, char *title) {
	char *string = displayer.string, *p;
	int len = SCROLLABLE_SIZE;
	
	// need a display!
	if (!display) return;
	
	// just do title if there is no config set
	if (!displayer.metadata_config) {
		strncpy(displayer.string, title ? title : "", SCROLLABLE_SIZE);
		return;
	}
	
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);
	
	// format metadata parameters and write them directly
	if ((p = strcasestr(displayer.metadata_config, "format")) != NULL) {
		char token[16], *q;
		int space = len;
		bool skip = false;
			
		displayer.string[0] = '\0';	
		p = strchr(displayer.metadata_config, '=');
			
		while (p++) {
			// find token and copy what's after when reaching last one
			if (sscanf(p, "%*[^%%]%%%[^%]%%", token) < 0) {
				q = strchr(p, ',');
				strncat(string, p, q ? min(q - p, space) : space);
				break;
			}

			// copy what's before token (be safe)
			if ((q = strchr(p, '%')) == NULL) break;
			
			// skip whatever is after a token if this token is empty
			if (!skip) {
				strncat(string, p, min(q - p, space));
				space = len - strlen(string);
			}	

			// then copy token's content
			if (!strncasecmp(q + 1, "artist", 6) && artist) strncat(string, p = artist, space);
			else if (!strncasecmp(q + 1, "album", 5) && album) strncat(string, p = album, space);
			else if (!strncasecmp(q + 1, "title", 5) && title) strncat(string, p = title, space);
			space = len - strlen(string);
				
			// flag to skip the data following an empty field
			if (*p) skip = false;
			else skip = true;

			// advance to next separator
			p = strchr(q + 1, '%');
		}
	} else {
		strncpy(string, title ? title : "", SCROLLABLE_SIZE);
	}
	
	// get optional scroll speed & pause
	PARSE_PARAM(displayer.metadata_config, "speed", '=', displayer.speed);
	PARSE_PARAM(displayer.metadata_config, "pause", '=', displayer.pause);
	
	displayer.offset = 0;
	// give this track's artwork time to arrive before falling back to the default icon
	displayer.artwork.tick = xTaskGetTickCount();
	utf8_decode(displayer.string);
	ESP_LOGI(TAG, "playing %s", displayer.string);
	displayer.boundary = GDS_TextStretch(display, 2, displayer.string, SCROLLABLE_SIZE);
		
	xSemaphoreGive(displayer.mutex);
}	

/****************************************************************************************
 *
 */
void displayer_scroll(char *string, int speed, int pause) {
	// need a display!
	if (!display) return;
	
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);

	if (speed) displayer.speed = speed;
	if (pause) displayer.pause = pause;
	displayer.offset = 0;	
	strncpy(displayer.string, string, SCROLLABLE_SIZE);
	displayer.string[SCROLLABLE_SIZE] = '\0';
	displayer.boundary = GDS_TextStretch(display, 2, displayer.string, SCROLLABLE_SIZE);
		
	xSemaphoreGive(displayer.mutex);
}

/****************************************************************************************
 * 
 */
void displayer_timer(enum displayer_time_e mode, int elapsed, int duration) {
	// need a display!
	if (!display) return;
	
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);

	if (displayer.timer) displayer.tick = xTaskGetTickCount();
	if (elapsed >= 0) displayer.elapsed = elapsed / 1000;	
	if (duration > 0) {
		displayer.duration.visible = true;
		displayer.duration.value = duration / 1000;

		if (displayer.duration.value > 3600) sprintf(displayer.duration.string, "%u:%02u:%02u", (displayer.duration.value / 3600) % 10,
													(displayer.duration.value % 3600) / 60, displayer.duration.value % 60);
		else sprintf(displayer.duration.string, "%u:%02u", displayer.duration.value / 60, displayer.duration.value % 60);

		char *buf;
		asprintf(&buf, "%s %s/%s", displayer.header, displayer.duration.string, displayer.duration.string);
		if (GDS_GetTextWidth(display, 1, 0, buf) > GDS_GetWidth(display) - GDS_GetOverlayReserve(display)) {
			ESP_LOGW(TAG, "Can't fit duration %s (%d) on screen using elapsed only", buf, GDS_GetTextWidth(display, 1, 0, buf));
			displayer.duration.visible = false;
		}
		free(buf);
	} else if (!duration) {
		displayer.duration.visible = false;
		displayer.duration.value = 0;
	}
		
	xSemaphoreGive(displayer.mutex);
}	

/****************************************************************************************
 * See above comment
 */
void displayer_control(enum displayer_cmd_e cmd, ...) {
	va_list args;
	
	if (!display) return;
	
	va_start(args, cmd);
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);
		
	switch(cmd) {
	case DISPLAYER_ACTIVATE: {	
		char *header = va_arg(args, char*);
		displayer.artwork.active = displayer.artwork.enable && va_arg(args, int);
		strncpy(displayer.header, header, HEADER_SIZE);
		displayer.header[HEADER_SIZE] = '\0';
		displayer.state = DISPLAYER_ACTIVE;
		displayer.timer = false;
		displayer.refresh = true;
		displayer.string[0] = '\0';
		displayer.elapsed = displayer.duration.value = 0;
		displayer.duration.visible = false;
		displayer.offset = displayer.boundary = 0;
		displayer.artwork.tick = xTaskGetTickCount();
		display_bus(&displayer, DISPLAY_BUS_TAKE);
		if (displayer.artwork.active) GDS_SetTextWidth(display, displayer.artwork.offset);
		vTaskResume(displayer.task);
		break;
	}	
	case DISPLAYER_SUSPEND:		
		// task will display the line 2 from beginning and suspend
		displayer.state = DISPLAYER_IDLE;
		displayer_artwork(NULL);
		display_bus(&displayer, DISPLAY_BUS_GIVE);
		break;		
	case DISPLAYER_SHUTDOWN:
		// let the task self-suspend (we might be doing i2c_write)
		GDS_SetTextWidth(display, 0);
		displayer_artwork(NULL);
		displayer.state = DISPLAYER_DOWN;
		display_bus(&displayer, DISPLAY_BUS_GIVE);
		break;
	case DISPLAYER_TIMER_RUN:
		if (!displayer.timer) {
			display_bus(&displayer, DISPLAY_BUS_TAKE);
			displayer.timer = true;		
			displayer.tick = xTaskGetTickCount();		
		}	
		break;
	case DISPLAYER_TIMER_PAUSE:
		displayer.timer = false;
		break;
	default:
		break;
	}	
	
	xSemaphoreGive(displayer.mutex);
	va_end(args);
}

/****************************************************************************************
 *
 */
bool display_is_valid_driver(const char * driver){
	return display_conf_get_driver_name(driver)!=NULL;
}

/****************************************************************************************
 *
 */
const char *display_conf_get_driver_name(const char * driver){
	for(uint8_t i=0;known_drivers[i]!=NULL && strlen(known_drivers[i])>0;i++ ){
		if(strcasestr(driver,known_drivers[i])){
			return known_drivers[i];
		}
	}
	return NULL;
}

/****************************************************************************************
 *
 */
char * display_get_supported_drivers(void){
	int total_size = 1;
	char * supported_drivers=NULL;
	const char * separator = "|";
	int separator_len = strlen(separator);

	for(uint8_t i=0;known_drivers[i]!=NULL && strlen(known_drivers[i])>0;i++ ){
		total_size += strlen(known_drivers[i])+separator_len;
	}
	total_size+=2;
	supported_drivers = malloc(total_size);
	memset(supported_drivers,0x00,total_size);
	strcat(supported_drivers,"<");
	for(uint8_t i=0;known_drivers[i]!=NULL && strlen(known_drivers[i])>0;i++ ){
		supported_drivers = strcat(supported_drivers,known_drivers[i]);
		supported_drivers = strcat(supported_drivers,separator);
	}
	strcat(supported_drivers,">");
	return supported_drivers;
}
