#include "stream_server.h"
#include "ws_log.h"
#include "camera_manager.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <lwip/sockets.h>
#include <errno.h>
#include "esp_heap_caps.h"

static const char* TAG = "StreamSrv";

static httpd_handle_t stream_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %lu\r\nX-Frame-Age: %lu\r\n\r\n";
static const int MAX_DETECTION_STREAM_CLIENTS = 2;

struct StreamRouteConfig {
    const char* path;
    const char* role;
    bool detection;
    int maxClients;
};

static StreamRouteConfig guiStreamConfig = { "/stream", "gui", false, MAX_STREAM_CLIENTS };
static StreamRouteConfig detectionStreamConfig = { "/detection-stream", "detection", true, MAX_DETECTION_STREAM_CLIENTS };

static int getRouteClientCount(const StreamRouteConfig* cfg) {
    return cfg->detection ? getDetectionStreamClientCount() : getStreamClientCount();
}

static void routeClientConnected(const StreamRouteConfig* cfg) {
    if (cfg->detection) detectionStreamClientConnected();
    else streamClientConnected();
}

static void routeClientDisconnected(const StreamRouteConfig* cfg) {
    if (cfg->detection) detectionStreamClientDisconnected();
    else streamClientDisconnected();
}

// MJPEG stream handler - serves unique frames from the shared ring buffer.
static esp_err_t streamHandler(httpd_req_t* req) {
    StreamRouteConfig* cfg = (StreamRouteConfig*)req->user_ctx;
    if (!cfg) cfg = &guiStreamConfig;

    if (getRouteClientCount(cfg) >= cfg->maxClients) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Too many stream clients", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    esp_err_t res = ESP_OK;
    char part_buf[160];
    uint32_t lastSentTimestamp = 0;

    // Set socket send timeout so stale A12/GUI clients do not hold the task forever.
    int fd = httpd_req_to_sockfd(req);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", String(appConfig.active_fps).c_str());
    httpd_resp_set_hdr(req, "X-Stream-Role", cfg->role);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    routeClientConnected(cfg);
    logCapture("[%s] %s stream client connected (fd=%d)\n", TAG, cfg->role, fd);

    uint32_t lastActivityMs = millis();
    while (true) {
        // Idle too long (no new frame to send): a dead peer is otherwise never
        // noticed because only writes fail. Peek the socket without consuming data;
        // 0 = orderly close, a hard error other than EAGAIN = reset.
        if (millis() - lastActivityMs > 2000) {
            char probe;
            int r = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                res = ESP_FAIL;
                break;
            }
            lastActivityMs = millis();   // probe again in another 2 s
        }

        const uint8_t* buf = NULL;
        size_t len = 0;

        int rh = ringBufferGetLatest(&buf, &len);
        if (rh < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t ts = ringBufferGetTimestamp(rh);
        if (ts > 0 && ts == lastSentTimestamp) {
            ringBufferRelease(rh);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        lastSentTimestamp = ts;
        lastActivityMs = millis();
        uint32_t now = millis();
        uint32_t age = (ts > 0 && now >= ts) ? (now - ts) : 0;

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) {
            ringBufferRelease(rh);
            break;
        }

        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                               (unsigned)len,
                               (unsigned long)(ts > 0 ? ts : now),
                               (unsigned long)age);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            ringBufferRelease(rh);
            break;
        }

        res = httpd_resp_send_chunk(req, (const char*)buf, len);
        ringBufferRelease(rh);

        if (res != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    routeClientDisconnected(cfg);
    logCapture("[%s] %s stream client disconnected\n", TAG, cfg->role);
    return res;
}

// Snapshot handler - returns single JPEG frame.
static esp_err_t snapshotHandler(httpd_req_t* req) {
    const uint8_t* buf = NULL;
    size_t len = 0;

    // A slow client must not hold a ring slot (it would starve the capture task).
    int fd = httpd_req_to_sockfd(req);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int rh = ringBufferGetLatest(&buf, &len);
    if (rh >= 0) {
        uint32_t ts = ringBufferGetTimestamp(rh);
        // Copy the frame out and release the slot before the (blocking) send.
        uint8_t* copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (copy) memcpy(copy, buf, len);
        else if (!(copy = (uint8_t*)malloc(len))) { ringBufferRelease(rh); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
        else memcpy(copy, buf, len);
        ringBufferRelease(rh);
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        if (ts > 0) httpd_resp_set_hdr(req, "X-Timestamp", String(ts).c_str());
        esp_err_t res = httpd_resp_send(req, (const char*)copy, len);
        free(copy);
        return res;
    }

    camera_fb_t* fb = captureFrame();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
    releaseFrame(fb);
    return res;
}

// CORS preflight handler.
static esp_err_t corsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS, HEAD");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// HEAD handler for stream probing.
static esp_err_t streamHeadHandler(httpd_req_t* req) {
    StreamRouteConfig* cfg = (StreamRouteConfig*)req->user_ctx;
    if (!cfg) cfg = &guiStreamConfig;
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", String(appConfig.active_fps).c_str());
    httpd_resp_set_hdr(req, "X-Stream-Role", cfg->role);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void registerStreamRoute(const StreamRouteConfig* cfg) {
    httpd_uri_t get_uri = {
        .uri = cfg->path, .method = HTTP_GET,
        .handler = streamHandler, .user_ctx = (void*)cfg
    };
    httpd_register_uri_handler(stream_httpd, &get_uri);

    httpd_uri_t opts_uri = {
        .uri = cfg->path, .method = HTTP_OPTIONS,
        .handler = corsHandler, .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &opts_uri);

    httpd_uri_t head_uri = {
        .uri = cfg->path, .method = HTTP_HEAD,
        .handler = streamHeadHandler, .user_ctx = (void*)cfg
    };
    httpd_register_uri_handler(stream_httpd, &head_uri);
}

bool streamServerInit() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = STREAM_PORT;
    config.ctrl_port = STREAM_PORT + 1;
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.core_id = 0;

    logCapture("[%s] Starting stream server on port %d\n", TAG, STREAM_PORT);

    if (httpd_start(&stream_httpd, &config) != ESP_OK) {
        logCapture("[%s] Failed to start stream server\n", TAG);
        return false;
    }

    registerStreamRoute(&guiStreamConfig);
    registerStreamRoute(&detectionStreamConfig);

    // /snapshot on stream port too.
    httpd_uri_t snap_uri = {
        .uri = "/snapshot", .method = HTTP_GET,
        .handler = snapshotHandler, .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &snap_uri);

    logCapture("[%s] Stream server started\n", TAG);
    return true;
}

void streamServerStop() {
    if (stream_httpd) {
        httpd_stop(stream_httpd);
        stream_httpd = NULL;
        logCapture("[%s] Stream server stopped\n", TAG);
    }
}
