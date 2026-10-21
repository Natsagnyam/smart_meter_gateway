/*
 * Zephyr RTOS Smart kWh Meter & Billing Gateway
 * Author: Natska Natsagnyam NAMKHAI
 * Target: ESP32-WROOM-32E (esp32_devkitc/esp32/procpu)
 *
 * Architecture Rules:
 * - Zero Dynamic Memory Allocation (No malloc)
 * - Lock-Free Ring Buffer with Bitwise Masking (& operator, no %)
 * - Cache-Line Isolation (__attribute__((aligned(64))))
 * - Bearer Token Authentication ("Natska2026") on Relay Cut/Restore Endpoints
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(billing_gateway, LOG_LEVEL_INF);

/* -------------------------------------------------------------------- */
/* Configuration & Macros                                               */
/* -------------------------------------------------------------------- */
#define WIFI_SSID "Temka room"
#define WIFI_PSK  "Namutemuhasar3"

#define RING_BUFFER_SIZE 64  /* Power of 2 for bitwise masking */
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define AUTH_BEARER_TOKEN "Bearer Natska2026"
#define HTTP_PORT 80

/* GPIO 4 controlling simulated mains relay */
static const struct gpio_dt_spec relay = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

/* -------------------------------------------------------------------- */
/* Cache-Line Isolated Telemetry Structures                              */
/* -------------------------------------------------------------------- */
struct telemetry_sample {
    uint32_t timestamp_ms;
    float kwh_reading;
    bool relay_state;
};

struct producer_ctrl {
    uint8_t pad_top[60];
    volatile uint32_t tail;
    uint8_t pad_bottom[60];
} __attribute__((aligned(64)));

struct consumer_ctrl {
    uint8_t pad_top[60];
    volatile uint32_t head;
    uint8_t pad_bottom[60];
} __attribute__((aligned(64)));

static struct telemetry_sample ring_buffer[RING_BUFFER_SIZE];
static struct producer_ctrl prod = { .tail = 0 };
static struct consumer_ctrl cons = { .head = 0 };

static float current_kwh = 124.55f;
static bool relay_closed = true;

/* -------------------------------------------------------------------- */
/* Ring Buffer Operations                                               */
/* -------------------------------------------------------------------- */
static void ring_buffer_push(float kwh, bool relay_status) {
    uint32_t current_tail = prod.tail;
    uint32_t next_tail = (current_tail + 1) & RING_BUFFER_MASK;

    ring_buffer[current_tail & RING_BUFFER_MASK].timestamp_ms = k_uptime_get_32();
    ring_buffer[current_tail & RING_BUFFER_MASK].kwh_reading = kwh;
    ring_buffer[current_tail & RING_BUFFER_MASK].relay_state = relay_status;

    prod.tail = next_tail;
}

/* -------------------------------------------------------------------- */
/* Wi-Fi Connection Helper                                              */
/* -------------------------------------------------------------------- */
static void wifi_connect(void) {
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No default network interface found");
        return;
    }

    struct wifi_connect_req_params params = {
        .ssid = WIFI_SSID,
        .ssid_length = strlen(WIFI_SSID),
        .psk = WIFI_PSK,
        .psk_length = strlen(WIFI_PSK),
        .channel = WIFI_CHANNEL_ANY,
        .security = WIFI_SECURITY_TYPE_PSK,
    };

    LOG_INF("Connecting to Wi-Fi SSID: %s...", WIFI_SSID);

    if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params))) {
        LOG_ERR("Wi-Fi connection request failed");
    }
}

/* -------------------------------------------------------------------- */
/* Authentication Helper                                                */
/* -------------------------------------------------------------------- */
static bool is_authenticated(const char *request_buf) {
    const char *auth_hdr = strstr(request_buf, "Authorization: ");
    if (!auth_hdr) {
        return false;
    }

    auth_hdr += strlen("Authorization: ");
    if (strncmp(auth_hdr, AUTH_BEARER_TOKEN, strlen(AUTH_BEARER_TOKEN)) == 0) {
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------- */
/* HTTP Request Handler & Router                                        */
/* -------------------------------------------------------------------- */
static void handle_client(int client_sock) {
    static char rx_buf[1024];
    static char tx_buf[2048];

    /* Set socket receive timeout (2 seconds) */
    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    zsock_setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int bytes_received = zsock_recv(client_sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (bytes_received <= 0) {
        zsock_close(client_sock);
        return;
    }
    rx_buf[bytes_received] = '\0';

    /* Route: GET /api/v1/relay/cut (Protected) */
    if (strstr(rx_buf, "GET /api/v1/relay/cut") != NULL) {
        if (!is_authenticated(rx_buf)) {
            const char *unauth = "HTTP/1.1 401 Unauthorized\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Connection: close\r\n\r\n"
                                 "{\"error\": \"Unauthorized - Incorrect Password Natska2026\"}";
            zsock_send(client_sock, unauth, strlen(unauth), 0);
        } else {
            relay_closed = false;
            if (device_is_ready(relay.port)) {
                gpio_pin_set_dt(&relay, 0);
            }
            LOG_WRN("BILLING ALERT: Payment missed! Power disconnected via authenticated API.");
            const char *resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Connection: close\r\n\r\n"
                               "{\"status\": \"success\", \"relay\": \"CUT\"}";
            zsock_send(client_sock, resp, strlen(resp), 0);
        }
    }
    /* Route: GET /api/v1/relay/restore (Protected) */
    else if (strstr(rx_buf, "GET /api/v1/relay/restore") != NULL) {
        if (!is_authenticated(rx_buf)) {
            const char *unauth = "HTTP/1.1 401 Unauthorized\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Connection: close\r\n\r\n"
                                 "{\"error\": \"Unauthorized - Incorrect Password Natska2026\"}";
            zsock_send(client_sock, unauth, strlen(unauth), 0);
        } else {
            relay_closed = true;
            if (device_is_ready(relay.port)) {
                gpio_pin_set_dt(&relay, 1);
            }
            LOG_INF("BILLING NOTIFICATION: Payment verified. Power restored via authenticated API.");
            const char *resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Connection: close\r\n\r\n"
                               "{\"status\": \"success\", \"relay\": \"RESTORED\"}";
            zsock_send(client_sock, resp, strlen(resp), 0);
        }
    }
    /* Route: GET /api/v1/meter (Public Read) */
    else if (strstr(rx_buf, "GET /api/v1/meter") != NULL) {
        int json_len = snprintf(tx_buf, sizeof(tx_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n"
            "{\"kwh\": %.2f, \"relay_closed\": %s, \"uptime_ms\": %u}",
            (double)current_kwh, relay_closed ? "true" : "false", k_uptime_get_32());

        zsock_send(client_sock, tx_buf, json_len, 0);
    }
    /* Route: GET / (Interactive Smart Dashboard HTML) */
    else if (strstr(rx_buf, "GET / ") != NULL || strstr(rx_buf, "GET /index.html") != NULL) {
        const char *html = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html>\n"
            "<html>\n"
            "<head>\n"
            "    <meta charset=\"utf-8\">\n"
            "    <title>Smart Meter & Billing Gateway | Natska Namkhai</title>\n"
            "    <style>\n"
            "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 2rem; display: flex; justify-content: center; }\n"
            "        .container { max-width: 650px; width: 100%; background: #1e293b; padding: 2rem; border-radius: 12px; box-shadow: 0 10px 25px rgba(0,0,0,0.3); border: 1px solid #334155; }\n"
            "        .header { display: flex; align-items: center; gap: 1.5rem; margin-bottom: 1.5rem; border-bottom: 1px solid #334155; padding-bottom: 1rem; }\n"
            "        .profile-img { width: 85px; height: 85px; border-radius: 50%%; object-fit: cover; border: 2px solid #38bdf8; }\n"
            "        h2 { margin: 0; font-size: 1.4rem; color: #38bdf8; }\n"
            "        .author { margin: 0.25rem 0 0 0; color: #94a3b8; font-size: 0.95rem; }\n"
            "        .card-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 1.5rem; }\n"
            "        .card { background: #0f172a; padding: 1rem; border-radius: 8px; border: 1px solid #334155; }\n"
            "        .card-title { font-size: 0.8rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.05em; }\n"
            "        .card-value { font-size: 1.4rem; font-weight: bold; margin-top: 0.5rem; color: #f8fafc; }\n"
            "        .status-ok { color: #4ade80; }\n"
            "        .status-alert { color: #f87171; }\n"
            "        .btn-group { display: flex; gap: 1rem; }\n"
            "        button { flex: 1; padding: 0.8rem 1rem; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; transition: background 0.2s; font-size: 0.95rem; }\n"
            "        .btn-cut { background: #dc2626; color: white; }\n"
            "        .btn-cut:hover { background: #b91c1c; }\n"
            "        .btn-restore { background: #16a34a; color: white; }\n"
            "        .btn-restore:hover { background: #15803d; }\n"
            "        .log-box { background: #0f172a; padding: 0.75rem 1rem; border-radius: 6px; font-family: monospace; font-size: 0.85rem; color: #38bdf8; margin-top: 1rem; border: 1px solid #334155; }\n"
            "    </style>\n"
            "</head>\n"
            "<body>\n"
            "    <div class=\"container\">\n"
            "        <div class=\"header\">\n"
            "            <img src=\"https://raw.githubusercontent.com/Natsagnyam/smart_meter_gateway/main/jpeg/me.jpg\" class=\"profile-img\" alt=\"Natska Natsagnyam\">\n"
            "            <div>\n"
            "                <h2>⚡ Zephyr Smart kWh Meter & Billing Gateway</h2>\n"
            "                <p class=\"author\">Lead Architect: <b>Natska Natsagnyam NAMKHAI</b></p>\n"
            "            </div>\n"
            "        </div>\n"
            "        <div class=\"card-grid\">\n"
            "            <div class=\"card\">\n"
            "                <div class=\"card-title\">Energy Consumption</div>\n"
            "                <div class=\"card-value\" id=\"kwh-val\">Loading...</div>\n"
            "            </div>\n"
            "            <div class=\"card\">\n"
            "                <div class=\"card-title\">Mains Relay State</div>\n"
            "                <div class=\"card-value\" id=\"relay-val\">Loading...</div>\n"
            "            </div>\n"
            "            <div class=\"card\">\n"
            "                <div class=\"card-title\">Billing Status</div>\n"
            "                <div class=\"card-value\" id=\"payment-val\">Loading...</div>\n"
            "            </div>\n"
            "            <div class=\"card\">\n"
            "                <div class=\"card-title\">System Uptime</div>\n"
            "                <div class=\"card-value\" id=\"uptime-val\">Loading...</div>\n"
            "            </div>\n"
            "        </div>\n"
            "        <div class=\"btn-group\">\n"
            "            <button class=\"btn-cut\" onclick=\"controlRelay('cut')\">🔴 Cut Power</button>\n"
            "            <button class=\"btn-restore\" onclick=\"controlRelay('restore')\">🟢 Restore Power</button>\n"
            "        </div>\n"
            "        <div class=\"log-box\" id=\"status-log\">Dashboard active. Polling telemetry every 2s...</div>\n"
            "    </div>\n"
            "    <script>\n"
            "        async function fetchTelemetry() {\n"
            "            try {\n"
            "                let res = await fetch('/api/v1/meter');\n"
            "                let data = await res.json();\n"
            "                document.getElementById('kwh-val').innerText = data.kwh.toFixed(2) + ' kWh';\n"
            "                document.getElementById('relay-val').innerHTML = data.relay_closed ? '<span class=\"status-ok\">CLOSED (ON)</span>' : '<span class=\"status-alert\">OPEN (CUT)</span>';\n"
            "                document.getElementById('payment-val').innerHTML = data.relay_closed ? '<span class=\"status-ok\">Payment Made</span>' : '<span class=\"status-alert\">Overdue / Tripped</span>';\n"
            "                document.getElementById('uptime-val').innerText = Math.floor(data.uptime_ms / 1000) + 's';\n"
            "            } catch(e) {\n"
            "                console.error(e);\n"
            "            }\n"
            "        }\n"
            "        async function controlRelay(action) {\n"
            "            let pwd = prompt('Enter password (Natska2026) to ' + action + ' power:');\n"
            "            if (pwd === null) return;\n"
            "            if (pwd !== 'Natska2026') {\n"
            "                alert('Incorrect password! Access denied.');\n"
            "                return;\n"
            "            }\n"
            "            document.getElementById('status-log').innerText = 'Executing ' + action + ' command...';\n"
            "            try {\n"
            "                let res = await fetch('/api/v1/relay/' + action, {\n"
            "                    headers: { 'Authorization': 'Bearer ' + pwd }\n"
            "                });\n"
            "                let result = await res.json();\n"
            "                if (res.status === 200) {\n"
            "                    document.getElementById('status-log').innerText = 'Success: Relay ' + result.relay;\n"
            "                    fetchTelemetry();\n"
            "                } else {\n"
            "                    document.getElementById('status-log').innerText = 'Error: ' + (result.error || 'Unauthorized');\n"
            "                }\n"
            "            } catch(e) {\n"
            "                document.getElementById('status-log').innerText = 'Network error executing command.';\n"
            "            }\n"
            "        }\n"
            "        setInterval(fetchTelemetry, 2000);\n"
            "        fetchTelemetry();\n"
            "    </script>\n"
            "</body>\n"
            "</html>\n";

        zsock_send(client_sock, html, strlen(html), 0);
    }
    else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        zsock_send(client_sock, not_found, strlen(not_found), 0);
    }

    zsock_shutdown(client_sock, ZSOCK_SHUT_RDWR);
    zsock_close(client_sock);
}

/* -------------------------------------------------------------------- */
/* HTTP Server Thread                                                   */
/* -------------------------------------------------------------------- */
static void http_server_thread(void *p1, void *p2, void *p3) {
    int serv_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_sock < 0) {
        LOG_ERR("Failed to create HTTP socket");
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int opt = 1;
    zsock_setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (zsock_bind(serv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERR("Failed to bind HTTP socket on port %d", HTTP_PORT);
        zsock_close(serv_sock);
        return;
    }

    zsock_listen(serv_sock, 5);
    LOG_INF("HTTP Web Server listening on port %d...", HTTP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int client_sock = zsock_accept(serv_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock >= 0) {
            handle_client(client_sock);
        }
    }
}

K_THREAD_DEFINE(http_thread_id, 4096, http_server_thread, NULL, NULL, NULL, 5, 0, 0);

/* -------------------------------------------------------------------- */
/* Telemetry Sampler Thread                                             */
/* -------------------------------------------------------------------- */
static void telemetry_thread(void *p1, void *p2, void *p3) {
    LOG_INF("Starting Smart kWh Meter Gateway...");

    if (device_is_ready(relay.port)) {
        gpio_pin_configure_dt(&relay, GPIO_OUTPUT_ACTIVE);
        LOG_INF("Hardware initialized: Relay set to CLOSED (Power ON)");
    }

    /* Trigger Wi-Fi Association */
    wifi_connect();

    while (1) {
        k_msleep(5000);
        current_kwh += 0.05f;
        ring_buffer_push(current_kwh, relay_closed);
        LOG_INF("Current Meter Reading: %.2f kWh", (double)current_kwh);
    }
}

K_THREAD_DEFINE(telemetry_thread_id, 2048, telemetry_thread, NULL, NULL, NULL, 7, 0, 0);

/* Main Entry Point */
int main(void) {
    LOG_INF("Zephyr RTOS Application Initialized.");
    return 0;
}
