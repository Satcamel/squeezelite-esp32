/*
 *  Squeezelite for esp32
 *
 *  Simple web page to set a 6-bands equalizer and loudness without LMS
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "platform_config.h"
typedef uint8_t u8_t;
#include "equalizer.h"
#include "cspot_sink.h"

#define UI_BANDS	6
#define EQ_BANDS	10
#define GAIN_MIN	-12
#define GAIN_MAX	12
#define LOUD_MAX	10

static const char *TAG = "eq_web";

// user-facing bands and the fixed bands of esp_equalizer they are spread onto
static const float ui_freqs[UI_BANDS] = { 60, 150, 400, 1000, 3000, 10000 };
static const float eq_freqs[EQ_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
// closest eq band for each ui band, used when only a 10-bands setting exists
static const int ui_nearest[UI_BANDS] = { 1, 2, 4, 5, 7, 8 };

static int8_t ui_gain[UI_BANDS];

extern httpd_handle_t http_get_server(int *port);

static const char eq_page[] =
"<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Equalizer</title><style>"
":root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--acc:#0a7cff;--card:#f2f2f5}"
"@media(prefers-color-scheme:dark){:root{--bg:#121214;--fg:#f2f2f5;--mut:#9a9aa0;--acc:#4da3ff;--card:#1f1f23}}"
"body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif}"
"main{max-width:480px;margin:0 auto;padding:16px}"
"h1{font-size:22px;margin:4px 0 16px}"
".card{background:var(--card);border-radius:12px;padding:12px 16px;margin-bottom:16px}"
".row{display:grid;grid-template-columns:64px 1fr 56px;align-items:center;gap:8px;min-height:44px}"
".row label{color:var(--mut)}.row output{text-align:right;font-variant-numeric:tabular-nums}"
"input[type=range]{width:100%;accent-color:var(--acc)}"
"button{font:inherit;padding:10px 16px;border-radius:10px;border:0;background:var(--acc);color:#fff}"
"a{color:var(--acc)}#st{color:var(--mut);margin-left:12px}"
".warn{background:#b3261e;color:#fff;border-radius:12px;padding:12px 16px;margin-bottom:16px}.warn a{color:#fff}"
"</style></head><body><main>"
"<h1>Equalizer</h1>"
"<div class=\"warn\" id=\"key\" hidden><b>Spotify-Schl&uuml;ssel ung&uuml;ltig.</b> Spotify lehnt Client-ID/Secret ab. "
"Im <a href=\"https://developer.spotify.com/dashboard\" target=\"_blank\" rel=\"noopener\">Spotify-Dashboard</a> pr&uuml;fen, "
"die GitHub-Secrets SPOTIFY_CLIENT_ID/SECRET erneuern und die Firmware neu bauen.</div>"
"<div class=\"card\" id=\"eq\"></div>"
"<div class=\"card\"><div class=\"row\"><label for=\"l\">Loudness</label>"
"<input type=\"range\" id=\"l\" min=\"0\" max=\"10\" step=\"1\"><output id=\"lo\"></output></div></div>"
"<button id=\"flat\">Zur&uuml;cksetzen</button><span id=\"st\"></span>"
"<p><a href=\"/\">&larr; Einstellungen</a></p>"
"</main><script>"
"const F=['60 Hz','150 Hz','400 Hz','1 kHz','3 kHz','10 kHz'],eq=document.getElementById('eq'),"
"l=document.getElementById('l'),lo=document.getElementById('lo'),st=document.getElementById('st');let t;"
"F.forEach((f,i)=>{eq.insertAdjacentHTML('beforeend','<div class=\"row\"><label for=\"b'+i+'\">'+f+'</label>"
"<input type=\"range\" id=\"b'+i+'\" min=\"-12\" max=\"12\" step=\"1\"><output id=\"o'+i+'\"></output></div>')});"
"const B=F.map((f,i)=>document.getElementById('b'+i));"
"function show(){B.forEach((b,i)=>{const v=+b.value;document.getElementById('o'+i).textContent=(v>0?'+':'')+v+' dB'});"
"lo.textContent=l.value==0?'aus':l.value}"
"function fill(d){d.bands.forEach((v,i)=>B[i].value=v);l.value=d.loudness;"
"document.getElementById('key').hidden=d.spotify_key!==false;show()}"
"function send(){clearTimeout(t);t=setTimeout(()=>{st.textContent='...';"
"fetch('/eq.json',{method:'POST',body:'b='+B.map(b=>b.value).join(',')+'&l='+l.value})"
".then(r=>r.json()).then(d=>{fill(d);st.textContent='gespeichert'}).catch(()=>st.textContent='Fehler')},250)}"
"[...B,l].forEach(e=>e.addEventListener('input',()=>{show();send()}));"
"document.getElementById('flat').onclick=()=>{B.forEach(b=>b.value=0);l.value=0;show();send()};"
"fetch('/eq.json').then(r=>r.json()).then(fill).catch(()=>st.textContent='Fehler');"
"</script></body></html>";

/****************************************************************************************
 * spread user bands onto equalizer bands (linear interpolation on a log frequency scale)
 */
static void spread_gain(const int8_t *ui, int8_t *eq) {
	for (int i = 0; i < EQ_BANDS; i++) {
		float f = log2f(eq_freqs[i]), gain;

		if (f <= log2f(ui_freqs[0])) gain = ui[0];
		else if (f >= log2f(ui_freqs[UI_BANDS - 1])) gain = ui[UI_BANDS - 1];
		else {
			int k = 0;
			while (f > log2f(ui_freqs[k + 1])) k++;
			float t = (f - log2f(ui_freqs[k])) / (log2f(ui_freqs[k + 1]) - log2f(ui_freqs[k]));
			gain = ui[k] + t * (ui[k + 1] - ui[k]);
		}

		eq[i] = lroundf(gain);
	}
}

/****************************************************************************************
 * load user bands from NVS, or derive them from the 10-bands setting (e.g. set by LMS)
 */
static void load_ui_gain(void) {
	char *config = config_alloc_get(NVS_TYPE_STR, "eq6");
	int v[UI_BANDS];

	if (config && sscanf(config, "%d,%d,%d,%d,%d,%d", v, v + 1, v + 2, v + 3, v + 4, v + 5) == UI_BANDS) {
		for (int i = 0; i < UI_BANDS; i++) ui_gain[i] = v[i];
	} else {
		int8_t eq[EQ_BANDS];
		equalizer_get_gain(eq);
		for (int i = 0; i < UI_BANDS; i++) ui_gain[i] = eq[ui_nearest[i]];
	}

	free(config);
}

/****************************************************************************************
 *
 */
static esp_err_t send_state(httpd_req_t *req) {
	char json[112];

	snprintf(json, sizeof(json), "{\"bands\":[%d,%d,%d,%d,%d,%d],\"loudness\":%u,\"spotify_key\":%s}",
			 ui_gain[0], ui_gain[1], ui_gain[2], ui_gain[3], ui_gain[4], ui_gain[5],
			 (unsigned) equalizer_get_loudness(), cspot_credentials_ok() ? "true" : "false");

	httpd_resp_set_type(req, HTTPD_TYPE_JSON);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/****************************************************************************************
 *
 */
static esp_err_t eq_page_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	return httpd_resp_send(req, eq_page, sizeof(eq_page) - 1);
}

static esp_err_t eq_get_handler(httpd_req_t *req) {
	return send_state(req);
}

/****************************************************************************************
 * body is "b=g1,g2,g3,g4,g5,g6&l=loudness", both parts optional
 */
static esp_err_t eq_post_handler(httpd_req_t *req) {
	char body[64] = { };
	int received = 0, v[UI_BANDS], loudness;

	if (req->content_len >= sizeof(body)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too long");
		return ESP_FAIL;
	}

	while (received < req->content_len) {
		int n = httpd_req_recv(req, body + received, req->content_len - received);
		if (n <= 0) {
			if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
			return ESP_FAIL;
		}
		received += n;
	}

	char *p = strstr(body, "b=");
	if (p && sscanf(p + 2, "%d,%d,%d,%d,%d,%d", v, v + 1, v + 2, v + 3, v + 4, v + 5) == UI_BANDS) {
		char config[UI_BANDS * 4 + 1];
		int8_t eq[EQ_BANDS];
		int n = 0;

		for (int i = 0; i < UI_BANDS; i++) {
			ui_gain[i] = v[i] < GAIN_MIN ? GAIN_MIN : (v[i] > GAIN_MAX ? GAIN_MAX : v[i]);
			n += snprintf(config + n, sizeof(config) - n, "%d%s", ui_gain[i], i < UI_BANDS - 1 ? "," : "");
		}

		config_set_value(NVS_TYPE_STR, "eq6", config);
		spread_gain(ui_gain, eq);
		equalizer_set_gain(eq);
	}

	p = strstr(body, "l=");
	if (p && sscanf(p + 2, "%d", &loudness) == 1) {
		loudness = loudness < 0 ? 0 : (loudness > LOUD_MAX ? LOUD_MAX : loudness);
		equalizer_set_loudness(loudness);
	}

	ESP_LOGI(TAG, "set %s", body);
	return send_state(req);
}

/****************************************************************************************
 * register handlers on the (already running) http server
 */
void equalizer_web_init(void) {
	static bool registered;
	httpd_handle_t server = http_get_server(NULL);

	if (registered) return;
	if (!server) {
		ESP_LOGW(TAG, "no http server, equalizer page not available");
		return;
	}

	load_ui_gain();

	httpd_uri_t page = { .uri = "/eq", .method = HTTP_GET, .handler = eq_page_handler };
	httpd_uri_t get = { .uri = "/eq.json", .method = HTTP_GET, .handler = eq_get_handler };
	httpd_uri_t post = { .uri = "/eq.json", .method = HTTP_POST, .handler = eq_post_handler };

	if (httpd_register_uri_handler(server, &page) != ESP_OK ||
		httpd_register_uri_handler(server, &get) != ESP_OK ||
		httpd_register_uri_handler(server, &post) != ESP_OK) {
		ESP_LOGE(TAG, "can't register equalizer page handlers");
		return;
	}

	registered = true;
	ESP_LOGI(TAG, "equalizer page available at /eq");
}
