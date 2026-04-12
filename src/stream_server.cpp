#include "stream_server.h"
#include "camera_manager.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <lwip/sockets.h>

static const char* TAG = "StreamSrv";

static httpd_handle_t stream_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %lu\r\n\r\n";

// MJPEG stream handler - serves frames from ring buffer
static esp_err_t streamHandler(httpd_req_t* req) {
    esp_err_t res = ESP_OK;
    char part_buf[128];

    // Set socket send timeout
    int fd = httpd_req_to_sockfd(req);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "15");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    streamClientConnected();
    Serial.printf("[%s] Stream client connected (fd=%d)\n", TAG, fd);

    while (true) {
        const uint8_t* buf = NULL;
        size_t len = 0;

        if (!ringBufferGetLatest(&buf, &len)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Send boundary
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) {
            ringBufferRelease();
            break;
        }

        // Send part header
        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                               (unsigned)len, (unsigned long)(millis()));
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            ringBufferRelease();
            break;
        }

        // Send JPEG data
        res = httpd_resp_send_chunk(req, (const char*)buf, len);
        ringBufferRelease();

        if (res != ESP_OK) break;

        // Small yield to prevent WDT
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    streamClientDisconnected();
    Serial.printf("[%s] Stream client disconnected\n", TAG);
    return res;
}

// Snapshot handler - returns single JPEG frame
static esp_err_t snapshotHandler(httpd_req_t* req) {
    const uint8_t* buf = NULL;
    size_t len = 0;

    // Try ring buffer first
    if (ringBufferGetLatest(&buf, &len)) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        esp_err_t res = httpd_resp_send(req, (const char*)buf, len);
        ringBufferRelease();
        return res;
    }

    // Fallback to direct capture
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

// CORS preflight handler
static esp_err_t corsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// HEAD handler for stream probing
static esp_err_t streamHeadHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

bool streamServerInit() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = STREAM_PORT;
    config.ctrl_port = STREAM_PORT + 1;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.core_id = 0;

    Serial.printf("[%s] Starting stream server on port %d\n", TAG, STREAM_PORT);

    if (httpd_start(&stream_httpd, &config) != ESP_OK) {
        Serial.printf("[%s] Failed to start stream server\n", TAG);
        return false;
    }

    // /stream - MJPEG stream
    httpd_uri_t stream_uri = {
        .uri = "/stream", .method = HTTP_GET,
        .handler = streamHandler, .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &stream_uri);

    // /stream OPTIONS (CORS)
    httpd_uri_t stream_opts = {
        .uri = "/stream", .method = HTTP_OPTIONS,
        .handler = corsHandler, .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &stream_opts);

    // /stream HEAD (probing)
    httpd_uri_t stream_head = {
        .uri = "/stream", .method = HTTP_HEAD,
        .handler = streamHeadHandler, .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &stream_head);

    // /snapshot on stream port too
    httpd_uri_t snap_uri = {
        .uri = "/snapshot", .method = HTTP_GET,
        .handler = snapshotHandler, .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &snap_uri);

    Serial.printf("[%s] Stream server started\n", TAG);
    return true;
}

void streamServerStop() {
    if (stream_httpd) {
        httpd_stop(stream_httpd);
        stream_httpd = NULL;
        Serial.printf("[%s] Stream server stopped\n", TAG);
    }
}
