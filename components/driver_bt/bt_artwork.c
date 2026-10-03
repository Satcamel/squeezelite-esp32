/*
 *  Artwork for the bluetooth sink
 *
 *  AVRCP cover art is not available in this ESP-IDF version, so the cover is looked
 *  up from artist / album (or title) with the iTunes search API and then downloaded
 *  in a size that fits the artwork area.
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "tools.h"
#include "display.h"
#include "bt_artwork.h"

#define SEARCH_MAX		(32*1024)
#define ARTWORK_MAX		(128*1024)
#define ARTWORK_SIZE	"170x170bb"
#define TERM_LEN		256

static const char TAG[] = "bt_artwork";

// bumped on every new lookup/reset so late replies of a previous track are dropped
static volatile int generation;
static char last_key[2*TERM_LEN], song_term[TERM_LEN];

// context passed to the downloads: generation and whether this is the song fallback
#define CONTEXT(gen, song)	((void*) (intptr_t) (((gen) << 1) | (song)))
#define CONTEXT_GEN(ctx)	((int) ((intptr_t) (ctx) >> 1))
#define CONTEXT_SONG(ctx)	((int) ((intptr_t) (ctx) & 1))

static void search(const char *term, const char *entity, void *context);

/****************************************************************************************
 * Cover image received
 */
static void got_image(uint8_t *data, size_t len, void *context) {
	if (!data) {
		ESP_LOGW(TAG, "artwork download failed");
		return;
	}

	if (CONTEXT_GEN(context) == generation) {
		ESP_LOGI(TAG, "got artwork of %zu bytes", len);
		displayer_artwork(data);
	}
	free(data);
}

/****************************************************************************************
 * Search result received: take the first match's artwork url in our size
 */
static void got_search(uint8_t *data, size_t len, void *context) {
	if (!data) {
		ESP_LOGW(TAG, "artwork search failed");
		return;
	}

	if (CONTEXT_GEN(context) != generation) {
		free(data);
		return;
	}

	cJSON *root = cJSON_Parse((char*) data);
	free(data);

	cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "results"), 0);
	cJSON *art = cJSON_GetObjectItem(item, "artworkUrl100");
	char *p;

	if (cJSON_IsString(art) && (p = strstr(art->valuestring, "100x100bb")) != NULL) {
		char url[512];
		snprintf(url, sizeof(url), "%.*s" ARTWORK_SIZE "%s", (int) (p - art->valuestring), art->valuestring, p + strlen("100x100bb"));
		ESP_LOGI(TAG, "requesting artwork %s", url);
		http_download(url, ARTWORK_MAX, got_image, context);
	} else if (!CONTEXT_SONG(context) && *song_term) {
		// no such album (e.g. a compilation name), try the song instead
		ESP_LOGI(TAG, "no album match, searching song");
		search(song_term, "song", CONTEXT(CONTEXT_GEN(context), 1));
	} else {
		ESP_LOGI(TAG, "no artwork found");
	}

	cJSON_Delete(root);
}

/****************************************************************************************
 * Append words to a url-encoded search term
 */
static void append_term(char *term, size_t size, const char *words) {
	size_t len = strlen(term);

	if (!words || !*words) return;
	if (len && len + 1 < size) term[len++] = '+';

	for (const unsigned char *s = (const unsigned char*) words; *s && len + 4 < size; s++) {
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("-_.~", *s)) term[len++] = *s;
		else if (*s == ' ') term[len++] = '+';
		else len += sprintf(term + len, "%%%02X", *s);
	}

	term[len] = '\0';
}

static void search(const char *term, const char *entity, void *context) {
	char url[TERM_LEN + 128];
	snprintf(url, sizeof(url), "https://itunes.apple.com/search?media=music&entity=%s&limit=1&country=DE&term=%s", entity, term);
	ESP_LOGI(TAG, "searching artwork %s", url);
	http_download(url, SEARCH_MAX, got_search, context);
}

/****************************************************************************************
 * Look up the cover for a track (only when the album changes)
 */
void bt_artwork_lookup(const char *artist, const char *album, const char *title) {
	char key[sizeof(last_key)], album_term[TERM_LEN] = "";

	if (!displayer_can_artwork() || !artist || !*artist) return;

	snprintf(key, sizeof(key), "%s|%s", artist, album && *album ? album : title ? title : "");
	if (!strcmp(key, last_key)) return;
	strcpy(last_key, key);

	int gen = ++generation;

	*song_term = '\0';
	append_term(song_term, sizeof(song_term), artist);
	append_term(song_term, sizeof(song_term), title);

	if (album && *album) {
		append_term(album_term, sizeof(album_term), artist);
		append_term(album_term, sizeof(album_term), album);
		search(album_term, "album", CONTEXT(gen, 0));
	} else if (title && *title) {
		search(song_term, "song", CONTEXT(gen, 1));
	}
}

/****************************************************************************************
 * Playback started/stopped: forget the previous track, drop pending replies
 */
void bt_artwork_reset(void) {
	generation++;
	*last_key = '\0';
}
