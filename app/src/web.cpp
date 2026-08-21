// Web companion (ESP32 only): SoftAP hotspot + esp_http_server.
//
// Phase 0 spike: the board broadcasts a WiFi network; a phone joins and
// loads http://192.168.4.1/ (a static page from /spiffs/web) with a small
// /state JSON.
// Phase 1: engine bridge. POST /move runs a real search through the SAME
// registered UCI handlers as the serial path (see main.cpp web_engine_*),
// so the web game shares the search, time management, book, TT and
// pondering bit-identically with serial UCI mode. The page sends the full
// move list on every /move, so the position is re-derived from scratch each
// request and any console-side tinkering self-heals.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#include "cJSON.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "web.h"

namespace {

constexpr const char *const AP_SSID = "DOG-CHESS";
constexpr const char *const AP_PASS = nullptr; // open network (Phase 0)
constexpr int               AP_CHANNEL = 1;
constexpr int               AP_MAX_CONN = 4;

char ap_ip[16] = "0.0.0.0";

// ---- Phase 2: game session state. Owned exclusively by the single httpd
// task (handlers run on it and /move blocks it for the search), so no lock
// is needed -- the engine bridge has its own locks in main.cpp. ----
constexpr int64_t BASE_CLOCK_MS = 30 * 60 * 1000; // 30:00 per side (casual)

int         g_web_level         = 4;
std::string g_web_human_color   = "white";
int64_t     g_clock_white_ms    = BASE_CLOCK_MS;
int64_t     g_clock_black_ms    = BASE_CLOCK_MS;
std::vector<std::string> g_web_moves; // full move list of the web game
bool        g_web_game_over     = false;
std::string g_web_game_result;        // white_wins / black_wins / draw / human flag
int64_t     g_web_last_seq      = 0;  // last booked /move seq (retry guard)
// When the side to move began thinking (esp_timer ms). Anchored by /new and
// after every booked move; the continuous clock model deducts real time
// between anchors so the server's clocks match what every page displays.
int64_t     g_turn_started_ms   = 0;

// ---- Multi-player: one game at a time, everyone else watches or queues.
// A page identifies itself with a random pid (localStorage); the seat
// holder plays, the rest see a waitlist. A missing pid is the legacy/dev
// path and bypasses arbitration.
std::string g_owner_pid;            // current seat holder ("" = free board)
std::string g_owner_name;
std::vector<std::pair<std::string, std::string>> g_queue; // {pid, name}
int64_t     g_last_activity_ms  = 0;  // last booked move or /new

constexpr int64_t SEAT_IDLE_TAKEOVER_MS = 3 * 60 * 1000; // abandoned-seat takeover

// Names end up in JSON served to every client: keep them boring.
static std::string sanitize_name(const std::string & in)
{
	std::string out;
	for (char ch : in) {
		if ((unsigned char)ch >= 0x20 && ch != '"' && ch != '\\' &&
		    ch != '<' && ch != '>' && out.size() < 16)
			out += ch;
	}
	return out.empty() ? "Guest" : out;
}

static void touch_activity()
{
	g_last_activity_ms = esp_timer_get_time() / 1000;
}

// The seat is considered free for takeover when no game is running, or the
// seat holder has been inactive for a while (closed the tab and walked off).
static bool seat_takeable()
{
	if (g_web_game_over)
		return true;
	if (!g_last_activity_ms)
		return true;
	return esp_timer_get_time() / 1000 - g_last_activity_ms > SEAT_IDLE_TAKEOVER_MS;
}

// Difficulty -> per-move budget. The page's slider sends the level; the
// mapping lives here (server-authoritative) and always goes through the
// same go handler as serial UCI, so the ESP32 time-budget fix governs the
// board's spending at every level.
int level_movetime_ms(int level)
{
	// casual ladder: near-instant (weak) up to a serious-but-playable
	// 30 s think; anything beyond that makes a phone game crawl
	static const int table[] = { 300, 500, 1000, 2000, 3000,
	                             5000, 8000, 12000, 20000, 30000 };
	return table[level - 1];
}

int level_inc_ms(int level)
{
	static const int table[] = { 250, 500, 750, 1000, 1500,
	                             2000, 2500, 3000, 4000, 5000 };
	return table[level - 1];
}

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

// Captive-portal landing: instructions + info only (the game itself lives
// at /). Phones land here via the catch-all redirect below.
esp_err_t handle_welcome(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	return serve_file(req, "/spiffs/web/portal.html");
}

// ---- Captive portal ----
//
// Minimal DNS responder: answers EVERY query with an A record pointing at
// the board. Phones probing captive.apple.com / connectivitycheck.gstatic.com
// / msftconnecttest.com all land on the httpd, where the catch-all handler
// 302s them to the game page -- which is what makes the OS pop its
// "sign in to network" sheet straight onto DOG-CHESS.
static void dns_task_fn(void *)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return;
	sockaddr_in addr = {};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(53);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
		close(sock);
		return;
	}

	// answer record: name pointer | type A | class IN | ttl 60 | rdlen 4 | ip
	static const uint8_t answer[] = {
		0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
		192, 168, 4, 1
	};

	uint8_t buf[512], out[600];
	for (;;) {
		sockaddr_in src = {};
		socklen_t srclen = sizeof(src);
		ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr *)&src, &srclen);
		if (n < 12)
			continue;
		const uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
		if (qd < 1)
			continue;
		// skip the question section: QNAME labels, then QTYPE+QCLASS
		size_t i = 12;
		while (i + 1 < (size_t)n && buf[i])
			i += buf[i] + 1;
		i++;
		if (i + 4 > (size_t)n)
			continue;
		i += 4;

		buf[2] = 0x81; buf[3] = 0x80;   // response | recursion available
		buf[6] = 0;    buf[7] = 1;      // ANCOUNT = 1
		buf[8] = 0;    buf[9] = 0;      // NSCOUNT = 0
		buf[10] = 0;   buf[11] = 0;     // ARCOUNT = 0

		size_t len = i;
		memcpy(out, buf, i);
		memcpy(out + len, answer, sizeof(answer));
		len += sizeof(answer);
		sendto(sock, out, len, 0, (sockaddr *)&src, srclen);
	}
}

// Catch-all for probe requests that arrive under a foreign host name.
// Registered LAST so every real endpoint matches first.
esp_err_t handle_catchall(httpd_req_t *req)
{
	char host[64] = { 0 };
	const size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
	if (host_len > 0 && host_len < sizeof(host))
		httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));

	if (strstr(host, ap_ip) != host) {
		// foreign host = OS connectivity probe -> bounce to the
		// instructions page (the game is one tap away from there)
		httpd_resp_set_status(req, "302 Found");
		httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/welcome");
		return httpd_resp_sendstr(req, "");
	}
	// our own origin but an unknown path: nothing there
	return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

esp_err_t handle_state(httpd_req_t *req)
{
	std::string body;
	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"ssid\":\"%s\",\"ip\":\"%s\",\"uptime\":%lu,"
		"\"heap\":%zu,\"psram\":%zu,\"clients\":%d",
		AP_SSID, ap_ip, (unsigned long)(esp_timer_get_time() / 1000000),
		heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
		count_ap_clients());
	body += buf;

	// One consistent engine snapshot (single state-lock acquisition in
	// main.cpp), so fen/legal/last can never be a torn pre/post-reply mix.
	std::string fen;
	std::vector<std::string> legal;
	web_search_result_t last;
	web_engine_snapshot(fen, legal, last);
	if (last.valid) {
		snprintf(buf, sizeof(buf),
			",\"last\":{\"bestmove\":\"%s\",\"score\":%d,\"depth\":%d}",
			last.best_move.c_str(), last.score, last.depth);
		body += buf;
	}

	snprintf(buf, sizeof(buf), ",\"fen\":\"%s\"", fen.c_str());
	body += buf;

	// legal moves for the position (server-provided: the page highlights
	// tap targets from this list and never generates moves itself)
	body += ",\"legal\":[";
	for (size_t i = 0; i < legal.size(); i++) {
		if (i)
			body += ",";
		body += "\"";
		body += legal[i];
		body += "\"";
	}
	body += "]";

	// session: level, human color, clocks, full move list (page refresh
	// recovery), game over. The clocks are continuous: the side to move
	// drains in real time from the turn anchor, and a player who lets the
	// clock run out is flagged HERE even if they never submit another move
	// (the old model only deducted on /move, so such a game hung forever).
	int64_t w = g_clock_white_ms, b = g_clock_black_ms;
	if (!g_web_game_over && g_turn_started_ms) {
		const bool white_to_move = (g_web_moves.size() % 2 == 0);
		int64_t & stm_clock = white_to_move ? w : b;
		stm_clock -= esp_timer_get_time() / 1000 - g_turn_started_ms;
		if (stm_clock <= 0) {
			stm_clock         = 0;
			g_web_game_over   = true;
			g_web_game_result = white_to_move ? "black_wins" : "white_wins";
			printf("[web] /state: flag fall (%s)\n", g_web_game_result.c_str());
		}
		g_clock_white_ms = w;
		g_clock_black_ms = b;
	}
	snprintf(buf, sizeof(buf),
		",\"level\":%d,\"color\":\"%s\","
		"\"clock\":{\"white\":%lld,\"black\":%lld,\"inc\":%d}",
		g_web_level, g_web_human_color.c_str(),
		(long long)w, (long long)b,
		level_inc_ms(g_web_level));
	body += buf;

	// multi-player: seat holder + waitlist + holder idle time
	body += ",\"owner\":{\"pid\":\"" + g_owner_pid + "\",\"name\":\"" + g_owner_name + "\"}";
	body += ",\"queue\":[";
	for (size_t i = 0; i < g_queue.size(); i++) {
		if (i)
			body += ",";
		body += "{\"pid\":\"";
		body += g_queue[i].first;
		body += "\",\"name\":\"";
		body += g_queue[i].second;
		body += "\"}";
	}
	body += "]";
	snprintf(buf, sizeof(buf), ",\"idle_ms\":%lld",
	         g_last_activity_ms
	             ? (long long)(esp_timer_get_time() / 1000 - g_last_activity_ms)
	             : -1LL);
	body += buf;

	body += ",\"moves\":[";
	for (size_t i = 0; i < g_web_moves.size(); i++) {
		if (i)
			body += ",";
		body += "\"";
		body += g_web_moves[i];
		body += "\"";
	}
	body += "]";

	if (g_web_game_over) {
		snprintf(buf, sizeof(buf), ",\"game_over\":true,\"result\":\"%s\"",
			g_web_game_result.c_str());
		body += buf;
	}

	body += "}";

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, body.c_str());
}

// Read the request body into a fixed buffer (move lists are small).
bool read_body(httpd_req_t *req, char *buf, size_t cap)
{
	size_t total = 0;
	int n;
	while (total < cap - 1 && (n = httpd_req_recv(req, buf + total, cap - 1 - total)) > 0)
		total += n;
	if (n < 0 && n != HTTPD_SOCK_ERR_TIMEOUT)
		return false;
	buf[total] = '\0';
	return true;
}

std::vector<std::string> split_moves(const char *s)
{
	std::vector<std::string> moves;
	const char *p = s;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		moves.emplace_back(start, p);
	}
	return moves;
}

// The engine handlers must not run on the httpd task: its stack is 4 KB
// (Phase-0 heap tuning) and the go/position handlers need the same headroom
// the serial go-thread has (24 KB). Running them on the httpd stack
// overflowed it, corrupted the heap and panicked the searcher in
// esp_timer's timer_insert. So each /move spawns a PSRAM-stack thread --
// the identical pattern the serial UCI path uses for its per-go pthread.
std::mutex              g_web_req_mutex;      // one engine task at a time
std::mutex              g_web_done_mutex;
std::condition_variable g_web_done_cv;
bool                    g_web_done = false;

// Run an engine task on a PSRAM-stack thread (24 KB, like the serial
// go-pthread) -- never on the httpd task's 4 KB stack. Returns false if
// the task did not finish within the timeout.
//
// The task is taken BY VALUE: on the timeout path the worker is detached
// and outlives the caller's stack, so everything it touches (the task, and
// what the task itself captured) must be owned by the worker.
bool run_web_task(std::function<void()> task)
{
	g_web_done = false;

	esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
	cfg.stack_size = 24 * 1024;
	cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
	esp_pthread_set_cfg(&cfg);

	std::thread worker([task] {
		task();
		{
			std::lock_guard<std::mutex> lk(g_web_done_mutex);
			g_web_done = true;
		}
		g_web_done_cv.notify_all();
	});

	esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
	esp_pthread_set_cfg(&default_cfg);

	std::unique_lock<std::mutex> lk(g_web_done_mutex);
	bool done = g_web_done_cv.wait_for(lk, std::chrono::seconds(150),
	                                   [] { return g_web_done; });
	lk.unlock();

	if (!done) {
		// The serial console may be mid-"go infinite"; the worker finishes
		// on its own (the engine mutex serializes it with any later search).
		worker.detach();
		return false;
	}
	worker.join();
	return true;
}

bool run_web_search(const std::vector<std::string> & moves, int movetime)
{
	// capture by value: the worker may outlive this frame (timeout path)
	return run_web_task([moves, movetime] {
		web_engine_set_position(moves);
		web_engine_go_movetime(movetime);
	});
}

esp_err_t handle_move(httpd_req_t *req)
{
	// move lists stay well under ~1 KB (200 plies * 5 chars); cap the body
	// buffer so the httpd task stack (8 KB) is never at risk.
	char body[2048];
	if (!read_body(req, body, sizeof(body)))
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");

	cJSON *json = cJSON_Parse(body);
	if (!json)
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");

	cJSON *moves_json = cJSON_GetObjectItem(json, "moves");
	cJSON *mt_json    = cJSON_GetObjectItem(json, "movetime");
	if (!moves_json || !cJSON_IsString(moves_json)) {
		cJSON_Delete(json);
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need moves");
	}

	std::vector<std::string> moves = split_moves(moves_json->valuestring);
	// Phase 2: the page sends the move list + human_ms only; the per-move
	// budget comes from the stored level. An explicit movetime stays as the
	// dev/console override (and is what the automated gates use).
	int movetime = level_movetime_ms(g_web_level);
	if (mt_json && cJSON_IsNumber(mt_json) && mt_json->valuedouble >= 1)
		movetime = (int)mt_json->valuedouble;
	if (movetime > 120000)
		movetime = 120000;

	// The page tags each submission with a monotonically increasing seq so a
	// retry of the SAME request (response lost on the wire) books the clocks
	// only once. A different page/session has a different seq, so spectator
	// moves still book normally.
	int64_t seq = 0;
	cJSON *seq_json = cJSON_GetObjectItem(json, "seq");
	if (seq_json && cJSON_IsNumber(seq_json) && seq_json->valuedouble > 0)
		seq = (int64_t)seq_json->valuedouble;

	// seat enforcement: only the current player may move. A missing pid is
	// the legacy/dev path and stays allowed.
	std::string pid;
	cJSON *pid_json = cJSON_GetObjectItem(json, "pid");
	if (pid_json && cJSON_IsString(pid_json))
		pid = pid_json->valuestring;
	if (!pid.empty() && !g_owner_pid.empty() && pid != g_owner_pid) {
		char resp[192];
		snprintf(resp, sizeof(resp),
			"{\"error\":\"not_owner\",\"owner\":\"%s\"}", g_owner_name.c_str());
		cJSON_Delete(json);
		printf("[web] /move: rejected %s (seat held by %s)\n",
		       pid.substr(0, 8).c_str(), g_owner_name.c_str());
		httpd_resp_set_type(req, "application/json");
		httpd_resp_set_status(req, "409 Conflict");
		return httpd_resp_sendstr(req, resp);
	}

	// Continuous chess-clock model: the server owns the clocks. The page's
	// legacy human_ms field is accepted but ignored -- real time is deducted
	// from the turn anchor instead, so every client and the server agree.

	cJSON_Delete(json);

	std::lock_guard<std::mutex> req_lock(g_web_req_mutex);

	// Idempotency: if the engine already answered this exact request (the
	// page's fetch failed and retried, or a reloaded page re-sent the same
	// moves), the game log holds the human's moves plus the engine's reply.
	// Return the stored reply instead of re-searching, so one human move
	// never produces two engine moves. Runs after the mutex, so an
	// in-flight search has already landed its reply in g_web_moves.
	if (!g_web_moves.empty() && g_web_moves.size() == moves.size() + 1) {
		bool prefix = true;
		for (size_t i = 0; i < moves.size(); i++) {
			if (g_web_moves[i] != moves[i]) { prefix = false; break; }
		}
		if (prefix) {
			const web_search_result_t last = web_engine_last_result();
			if (last.valid && !last.best_move.empty() && !last.game_over) {
				std::string pv;
				for (size_t i = 0; i < last.pv.size(); i++) {
					if (i)
						pv += " ";
					pv += last.pv[i];
				}
				char resp[640];
				snprintf(resp, sizeof(resp),
					"{\"bestmove\":\"%s\",\"score\":%d,\"depth\":%d,\"pv\":\"%s\","
					"\"clock\":{\"white\":%lld,\"black\":%lld,\"inc\":%d}}",
					last.best_move.c_str(), last.score, last.depth, pv.c_str(),
					(long long)g_clock_white_ms, (long long)g_clock_black_ms,
					level_inc_ms(g_web_level));
				printf("[web] /move: idempotent reply for %s (already answered)\n",
				       moves.empty() ? "startpos" : moves.back().c_str());
				httpd_resp_set_type(req, "application/json");
				return httpd_resp_sendstr(req, resp);
			}
		}
	}

	// Continuous chess-clock model, server-authoritative. The human's think
	// ends when this request arrives (deducted from the turn anchor); the
	// engine's ends when the reply has landed. Deducting at those boundaries
	// (once per booked seq) keeps every client and the server on one clock,
	// and lets /state flag a player who never moves again.
	int64_t & human_clock  = g_web_human_color == "white" ? g_clock_white_ms : g_clock_black_ms;
	int64_t & engine_clock = g_web_human_color == "white" ? g_clock_black_ms : g_clock_white_ms;
	const int inc = level_inc_ms(g_web_level);
	const bool book = (seq != g_web_last_seq);
	if (book)
		g_web_last_seq = seq;
	const int64_t arrived_ms = esp_timer_get_time() / 1000;
	bool human_flags = false;
	if (book && !g_web_game_over && !moves.empty()) {
		// moves.empty() is the engine's auto-move as white: no human think.
		human_clock -= arrived_ms - g_turn_started_ms;
		if (human_clock <= 0) {
			human_clock = 0;
			human_flags = true;
		}
	}

	if (!run_web_search(moves, movetime)) {
		printf("[web] /move: search timed out\n");
		// Re-anchor so a retry does not charge the human for the stuck
		// engine window.
		g_turn_started_ms = esp_timer_get_time() / 1000;
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "search timeout");
	}

	const web_search_result_t last = web_engine_last_result();

	bool engine_flags = false;
	if (book && !g_web_game_over) {
		if (last.valid && !last.game_over) {
			engine_clock -= esp_timer_get_time() / 1000 - arrived_ms;
			if (engine_clock <= 0) {
				engine_clock = 0;
				engine_flags = true;
			}
		}
		human_clock += inc;
		engine_clock += inc;
		g_turn_started_ms = esp_timer_get_time() / 1000;
		touch_activity();
	}

	// game-over resolution: terminal position first, then clock flags
	const char *game_result = nullptr;
	if (last.game_over)
		game_result = last.game_state.c_str();
	else if (human_flags)
		game_result = g_web_human_color == "white" ? "black_wins" : "white_wins";
	else if (engine_flags)
		game_result = g_web_human_color == "white" ? "white_wins" : "black_wins";

	if (game_result) {
		g_web_game_over   = true;
		g_web_game_result = game_result;
		// keep the human's last move in the game log (page refresh + history)
		g_web_moves = moves;
		char resp[160];
		snprintf(resp, sizeof(resp),
			"{\"game_over\":true,\"result\":\"%s\",\"score\":%d,"
			"\"clock\":{\"white\":%lld,\"black\":%lld,\"inc\":%d}}",
			game_result, last.score,
			(long long)g_clock_white_ms, (long long)g_clock_black_ms, inc);
		printf("[web] /move: game over (%s)\n", game_result);
		httpd_resp_set_type(req, "application/json");
		return httpd_resp_sendstr(req, resp);
	}
	if (!last.valid || last.best_move.empty()) {
		printf("[web] /move: search produced no move\n");
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no move");
	}

	// the web game's move log (page refresh recovery + self-healing resend)
	g_web_moves = moves;
	g_web_moves.push_back(last.best_move);

	std::string pv;
	for (size_t i = 0; i < last.pv.size(); i++) {
		if (i)
			pv += " ";
		pv += last.pv[i];
	}

	char resp[640];
	snprintf(resp, sizeof(resp),
		"{\"bestmove\":\"%s\",\"score\":%d,\"depth\":%d,\"pv\":\"%s\","
		"\"clock\":{\"white\":%lld,\"black\":%lld,\"inc\":%d}}",
		last.best_move.c_str(), last.score, last.depth, pv.c_str(),
		(long long)g_clock_white_ms, (long long)g_clock_black_ms, inc);

	printf("[web] /move: %s -> %s (score %d, depth %d, clocks %lld/%lld)\n",
	       moves.empty() ? "startpos" : moves.back().c_str(),
	       last.best_move.c_str(), last.score, last.depth,
	       (long long)g_clock_white_ms, (long long)g_clock_black_ms);

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

esp_err_t handle_new(httpd_req_t *req)
{
	char body[1024];
	if (!read_body(req, body, sizeof(body)))
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");

	cJSON *json = cJSON_Parse(body);
	if (!json)
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");

	std::string color = "white";
	int level = 1;
	int64_t base_ms = BASE_CLOCK_MS;
	std::string pid, name;
	// copy out of the cJSON tree before it is freed below
	cJSON *c = cJSON_GetObjectItem(json, "color");
	if (c && cJSON_IsString(c))
		color = c->valuestring;
	c = cJSON_GetObjectItem(json, "level");
	if (c && cJSON_IsNumber(c))
		level = (int)c->valuedouble;
	// dev/test override for the time control (clamped to 5s..10m)
	c = cJSON_GetObjectItem(json, "base_ms");
	if (c && cJSON_IsNumber(c) && c->valuedouble >= 5000)
		base_ms = (int64_t)c->valuedouble;
	if (base_ms > BASE_CLOCK_MS)
		base_ms = BASE_CLOCK_MS;
	c = cJSON_GetObjectItem(json, "pid");
	if (c && cJSON_IsString(c))
		pid = c->valuestring;
	c = cJSON_GetObjectItem(json, "name");
	if (c && cJSON_IsString(c))
		name = sanitize_name(c->valuestring);
	cJSON_Delete(json);

	if (color != "white" && color != "black")
		color = "white";
	if (level < 1 || level > 10)
		level = 4;

	// Seat arbitration: one game at a time. A different pid may take the
	// seat only when it is free, or when the requester heads the waitlist
	// and the game is over / the holder went idle. A missing pid is the
	// legacy/dev path and always passes.
	if (!pid.empty() && !g_owner_pid.empty() && pid != g_owner_pid) {
		int pos = 0;
		for (size_t k = 0; k < g_queue.size(); k++)
			if (g_queue[k].first == pid) { pos = (int)k + 1; break; }
		const bool head = pos == 1;
		if (!head || !seat_takeable()) {
			char resp[256];
			snprintf(resp, sizeof(resp),
				"{\"error\":\"busy\",\"owner\":\"%s\",\"queue_pos\":%d}",
				g_owner_name.c_str(), pos);
			printf("[web] /new: %s is busy (seat held by %s)\n",
			       pid.substr(0, 8).c_str(), g_owner_name.c_str());
			httpd_resp_set_type(req, "application/json");
			httpd_resp_set_status(req, "409 Conflict");
			return httpd_resp_sendstr(req, resp);
		}
	}

	if (!run_web_task([] { web_engine_set_position({}); })) {
		printf("[web] /new: engine busy\n");
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "engine busy");
	}

	// fresh session: position reset (above), clocks at base, move log empty
	g_web_human_color = color;
	g_web_level       = level;
	g_clock_white_ms  = base_ms;
	g_clock_black_ms  = base_ms;
	g_web_moves.clear();
	g_web_game_over   = false;
	g_web_game_result.clear();
	g_web_last_seq    = 0;
	g_turn_started_ms = esp_timer_get_time() / 1000;

	// seat bookkeeping
	if (!pid.empty()) {
		g_owner_pid  = pid;
		if (!name.empty())
			g_owner_name = name;
		else if (g_owner_name.empty())
			g_owner_name = "Guest";
		for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
			if (it->first == pid) { g_queue.erase(it); break; }
	} else {
		// legacy/dev client: clean slate, free seat afterwards
		g_owner_pid.clear();
		g_owner_name.clear();
	}
	touch_activity();

	char resp[256];
	snprintf(resp, sizeof(resp),
		"{\"ok\":true,\"color\":\"%s\",\"level\":%d,"
		"\"clock\":{\"white\":%lld,\"black\":%lld,\"inc\":%d}}",
		color.c_str(), level,
		(long long)g_clock_white_ms, (long long)g_clock_black_ms,
		level_inc_ms(level));
	printf("[web] /new: %s, level %d\n", color.c_str(), level);

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

// ---- waitlist ----
static bool parse_pid_name(httpd_req_t *req, std::string & pid, std::string & name)
{
	char body[256];
	if (!read_body(req, body, sizeof(body)))
		return false;
	cJSON *json = cJSON_Parse(body);
	if (!json)
		return false;
	cJSON *c = cJSON_GetObjectItem(json, "pid");
	if (c && cJSON_IsString(c))
		pid = c->valuestring;
	c = cJSON_GetObjectItem(json, "name");
	if (c && cJSON_IsString(c))
		name = sanitize_name(c->valuestring);
	cJSON_Delete(json);
	return !pid.empty();
}

esp_err_t handle_queue(httpd_req_t *req)
{
	std::string pid, name;
	if (!parse_pid_name(req, pid, name))
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need pid");

	// dedupe: re-joining moves you to the back
	for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
		if (it->first == pid) { g_queue.erase(it); break; }
	g_queue.push_back({ pid, name });

	char resp[128];
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"pos\":%zu}", g_queue.size());
	printf("[web] queue: %s joined (pos %zu)\n", name.c_str(), g_queue.size());
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

esp_err_t handle_unqueue(httpd_req_t *req)
{
	std::string pid, name;
	if (!parse_pid_name(req, pid, name))
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need pid");
	for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
		if (it->first == pid) { g_queue.erase(it); break; }
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t handle_yield(httpd_req_t *req)
{
	std::string pid, name;
	if (!parse_pid_name(req, pid, name))
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need pid");

	// the holder may always yield; anyone else only when the game is over
	// or the holder went idle (walked off), so a seat is never stuck
	const bool allowed = pid == g_owner_pid || seat_takeable();
	if (!allowed) {
		char resp[192];
		snprintf(resp, sizeof(resp),
			"{\"error\":\"active\",\"owner\":\"%s\"}", g_owner_name.c_str());
		httpd_resp_set_type(req, "application/json");
		httpd_resp_set_status(req, "409 Conflict");
		return httpd_resp_sendstr(req, resp);
	}
	printf("[web] yield: %s releases the seat\n", g_owner_name.c_str());
	g_owner_pid.clear();
	g_owner_name.clear();
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t handle_battery(httpd_req_t *req)
{
	// XIAO ESP32S3 battery sense: ADC1_CH2 (GPIO2), 2:1 divider. The raw
	// read works today; voltage scaling is calibrated in Phase 2/3.
	static adc_oneshot_unit_handle_t unit = nullptr;
	if (!unit) {
		adc_oneshot_unit_init_cfg_t init_cfg = {
			.unit_id = ADC_UNIT_1,
			.clk_src = ADC_RTC_CLK_SRC_DEFAULT,
			.ulp_mode = ADC_ULP_MODE_DISABLE,
		};
		if (adc_oneshot_new_unit(&init_cfg, &unit) != ESP_OK) {
			unit = nullptr;
			return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "adc init failed");
		}
		adc_oneshot_chan_cfg_t chan_cfg = {
			.atten = ADC_ATTEN_DB_12,
			.bitwidth = ADC_BITWIDTH_12,
		};
		adc_oneshot_config_channel(unit, ADC_CHANNEL_2, &chan_cfg);
	}

	int raw = 0;
	if (adc_oneshot_read(unit, ADC_CHANNEL_2, &raw) != ESP_OK)
		raw = -1;

	char resp[128];
	snprintf(resp, sizeof(resp), "{\"adc_raw\":%d,\"v_mv_est\":%d,\"calibrated\":false}",
	         raw, raw >= 0 ? raw * 2600 / 4095 : 0);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

httpd_handle_t start_httpd()
{
	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	// 8 KB: the Phase-1 /move handler parses JSON + spawns the search
	// worker here; 4 KB overflowed and corrupted the adjacent heap.
	cfg.stack_size = 8192;
	cfg.max_uri_handlers = 12;
	cfg.lru_purge_enable = true;
	// wildcard matcher for the captive-portal catch-all ("/*"); exact
	// registered URIs still match exactly, in registration order
	cfg.uri_match_fn = httpd_uri_match_wildcard;

	httpd_handle_t server = nullptr;
	if (httpd_start(&server, &cfg) != ESP_OK) {
		printf("[web] httpd start failed\n");
		return nullptr;
	}

	httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = nullptr };
	httpd_uri_t welcome = { .uri = "/welcome", .method = HTTP_GET, .handler = handle_welcome, .user_ctx = nullptr };
	httpd_uri_t state = { .uri = "/state", .method = HTTP_GET, .handler = handle_state, .user_ctx = nullptr };
	httpd_uri_t move = { .uri = "/move", .method = HTTP_POST, .handler = handle_move, .user_ctx = nullptr };
	httpd_uri_t newgame = { .uri = "/new", .method = HTTP_POST, .handler = handle_new, .user_ctx = nullptr };
	httpd_uri_t battery = { .uri = "/battery", .method = HTTP_GET, .handler = handle_battery, .user_ctx = nullptr };
	httpd_uri_t queue = { .uri = "/queue", .method = HTTP_POST, .handler = handle_queue, .user_ctx = nullptr };
	httpd_uri_t unqueue = { .uri = "/unqueue", .method = HTTP_POST, .handler = handle_unqueue, .user_ctx = nullptr };
	httpd_uri_t yield_seat = { .uri = "/yield", .method = HTTP_POST, .handler = handle_yield, .user_ctx = nullptr };
	httpd_register_uri_handler(server, &root);
	httpd_register_uri_handler(server, &welcome);
	httpd_register_uri_handler(server, &state);
	httpd_register_uri_handler(server, &move);
	httpd_register_uri_handler(server, &newgame);
	httpd_register_uri_handler(server, &battery);
	httpd_register_uri_handler(server, &queue);
	httpd_register_uri_handler(server, &unqueue);
	httpd_register_uri_handler(server, &yield_seat);
	// LAST: the captive-portal catch-all for foreign-host probes
	httpd_uri_t fallback = { .uri = "/*", .method = HTTP_GET, .handler = handle_catchall, .user_ctx = nullptr };
	httpd_register_uri_handler(server, &fallback);
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

	// captive-portal DNS: every query resolves to the board
	xTaskCreate(dns_task_fn, "dns", 4096, nullptr, 5, nullptr);

	if (start_httpd())
		printf("[web] httpd up: http://%s/\n", ap_ip);
	else
		printf("[web] httpd FAILED\n");
}