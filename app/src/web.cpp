// Web companion (ESP32 only): SoftAP hotspot + esp_http_server.
//
// Phase 0 spike: the board broadcasts a WiFi network; a phone joins and
// loads http://192.168.4.1/ (a static page from /spiffs/web) with a small
// /state JSON. Phase 1 adds the engine bridge (move round-trip via a
// command pipe into the existing UCI handler).

#include <cstdio>
#include <cstring>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "web.h"

namespace {

constexpr const char *const AP_SSID = "DOG-CHESS";
constexpr const char *const AP_PASS = nullptr; // open network (Phase 0)
constexpr int               AP_CHANNEL = 1;
constexpr int               AP_MAX_CONN = 4;

char ap_ip[16] = "0.0.0.0";

int count_ap_clients()
{
	// Phase 0: best-effort count from the wifi AP station list.
	int n = 0;
	wifi_sta_list_t sta = {};
	if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK)
		n = sta.num;
	return n;
}

esp_err_t serve_file(httpd_req_t *req, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");

	char buf[1024];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
			fclose(f);
			return ESP_FAIL;
		}
	}
	fclose(f);
	httpd_resp_send_chunk(req, nullptr, 0);
	return ESP_OK;
}

esp_err_t handle_root(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	return serve_file(req, "/spiffs/web/index.html");
}

esp_err_t handle_state(httpd_req_t *req)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
		"{\"ssid\":\"%s\",\"ip\":\"%s\",\"uptime\":%lu,"
		"\"heap\":%zu,\"psram\":%zu,\"clients\":%d}",
		AP_SSID, ap_ip, (unsigned long)(esp_timer_get_time() / 1000000),
		heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
		count_ap_clients());
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, buf);
}

esp_err_t handle_move(httpd_req_t *req)
{
	// Phase 1: engine bridge. Reject politely for now.
	return httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "engine bridge: phase 1");
}

httpd_handle_t start_httpd()
{
	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	cfg.stack_size = 4096;
	cfg.max_uri_handlers = 8;
	cfg.lru_purge_enable = true;

	httpd_handle_t server = nullptr;
	if (httpd_start(&server, &cfg) != ESP_OK) {
		printf("[web] httpd start failed\n");
		return nullptr;
	}

	httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = nullptr };
	httpd_uri_t state = { .uri = "/state", .method = HTTP_GET, .handler = handle_state, .user_ctx = nullptr };
	httpd_uri_t move = { .uri = "/move", .method = HTTP_POST, .handler = handle_move, .user_ctx = nullptr };
	httpd_register_uri_handler(server, &root);
	httpd_register_uri_handler(server, &state);
	httpd_register_uri_handler(server, &move);
	return server;
}

} // namespace

void init_web()
{
	printf("[web] init: SSID %s\n", AP_SSID);

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

	wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

	wifi_config_t ap_cfg = {};
	strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
	ap_cfg.ap.ssid_len = strlen(AP_SSID);
	ap_cfg.ap.channel = AP_CHANNEL;
	ap_cfg.ap.max_connection = AP_MAX_CONN;
	if (AP_PASS) {
		strncpy((char *)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password));
		ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
	} else {
		ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
	ESP_ERROR_CHECK(esp_wifi_start());

	esp_netif_ip_info_t ip = {};
	esp_netif_get_ip_info(ap_netif, &ip);
	snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&ip.ip));
	printf("[web] AP up: %s, ip %s\n", AP_SSID, ap_ip);

	if (start_httpd())
		printf("[web] httpd up: http://%s/\n", ap_ip);
	else
		printf("[web] httpd FAILED\n");
}