/*
 * Zephyr RTOS Smart kWh Meter & Billing Gateway
 * Author: Natska Natsagnyam NAMKHAI
 * Target: ESP32-WROOM-32E (esp32_devkitc/esp32/procpu)
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

#define WIFI_SSID "Temka room"
#define WIFI_PSK  "Namutemuhasar3"

#define RING_BUFFER_SIZE 64
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define AUTH_BEARER_TOKEN "Bearer Natska2026"
#define HTTP_PORT 80

static const struct gpio_dt_spec relay = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

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

static struct telemetry_sample ring_buffer[RING_BUFFER_SIZE];
static struct producer_ctrl prod = { .tail = 0 };

static float current_kwh = 124.55f;
static bool relay_closed = true;

static void ring_buffer_push(float kwh, bool relay_status) {
    uint32_t current_tail = prod.tail;
    uint32_t next_tail = (current_tail + 1) & RING_BUFFER_MASK;

    ring_buffer[current_tail & RING_BUFFER_MASK].timestamp_ms = k_uptime_get_32();
    ring_buffer[current_tail & RING_BUFFER_MASK].kwh_reading = kwh;
    ring_buffer[current_tail & RING_BUFFER_MASK].relay_state = relay_status;

    prod.tail = next_tail;
}

static void wifi_connect(void) {
    struct net_if *iface = net_if_get_default();
    if (!iface) return;

    struct wifi_connect_req_params params = {
        .ssid = WIFI_SSID,
        .ssid_length = strlen(WIFI_SSID),
        .psk = WIFI_PSK,
        .psk_length = strlen(WIFI_PSK),
        .channel = WIFI_CHANNEL_ANY,
        .security = WIFI_SECURITY_TYPE_PSK,
    };

    net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

static bool is_authenticated(const char *request_buf) {
    const char *auth_hdr = strstr(request_buf, "Authorization: ");
    if (!auth_hdr) return false;
    auth_hdr += strlen("Authorization: ");
    return (strncmp(auth_hdr, AUTH_BEARER_TOKEN, strlen(AUTH_BEARER_TOKEN)) == 0);
}

static void handle_client(int client_sock) {
    static char rx_buf[1024];
    static char tx_buf[8192];

    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    zsock_setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int bytes_received = zsock_recv(client_sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (bytes_received <= 0) {
        zsock_close(client_sock);
        return;
    }
    rx_buf[bytes_received] = '\0';

    if (strstr(rx_buf, "GET /api/v1/relay/cut") != NULL) {
        if (!is_authenticated(rx_buf)) {
            const char *unauth = "HTTP/1.1 401 Unauthorized\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\": \"Unauthorized\"}";
            zsock_send(client_sock, unauth, strlen(unauth), 0);
        } else {
            relay_closed = false;
            if (device_is_ready(relay.port)) gpio_pin_set_dt(&relay, 0);
            LOG_WRN("BILLING ALERT: Power cut via API.");
            const char *resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\": \"success\", \"relay\": \"CUT\"}";
            zsock_send(client_sock, resp, strlen(resp), 0);
        }
    }
    else if (strstr(rx_buf, "GET /api/v1/relay/restore") != NULL) {
        if (!is_authenticated(rx_buf)) {
            const char *unauth = "HTTP/1.1 401 Unauthorized\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\": \"Unauthorized\"}";
            zsock_send(client_sock, unauth, strlen(unauth), 0);
        } else {
            relay_closed = true;
            if (device_is_ready(relay.port)) gpio_pin_set_dt(&relay, 1);
            LOG_INF("BILLING NOTIFICATION: Power restored via API.");
            const char *resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\": \"success\", \"relay\": \"RESTORED\"}";
            zsock_send(client_sock, resp, strlen(resp), 0);
        }
    }
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
    else if (strstr(rx_buf, "GET / ") != NULL || strstr(rx_buf, "GET /index.html") != NULL) {
        const char *r_txt = relay_closed ? "<span style=\"color:#16a34a;font-weight:bold;\">CLOSED (ON)</span>" : "<span style=\"color:#dc2626;font-weight:bold;\">OPEN (CUT)</span>";
        const char *b_txt = relay_closed ? "<span style=\"color:#16a34a;\">Paid</span>" : "<span style=\"color:#dc2626;\">Overdue</span>";
        unsigned int up = (unsigned int)(k_uptime_get_32() / 1000);

        int html_len = snprintf(tx_buf, sizeof(tx_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><title>Smart Meter Gateway</title>"
            "<style>"
            "body{font-family:sans-serif;background:#0f172a;color:#f8fafc;padding:1.5rem;display:flex;justify-content:center;}"
            ".container{max-width:550px;width:100%%;display:flex;flex-direction:column;gap:1rem;}"
            ".panel{background:#1e293b;padding:1.2rem;border-radius:10px;border:1px solid #334155;}"
            "h2{color:#38bdf8;margin:0 0 0.5rem 0;font-size:1.1rem;}"
            "p{font-size:0.9rem;margin:0.4rem 0;}"
            ".chart{width:100%%;height:160px;background:#ffffff;border-radius:6px;border:1px solid #cbd5e1;margin-top:0.5rem;overflow:hidden;}"
            "button{width:100%%;padding:0.7rem;color:white;border:none;border-radius:6px;font-weight:bold;cursor:pointer;font-size:0.95rem;margin-top:0.5rem;background:%s;}"
            "</style></head><body>"
            "<div class=\"container\">"
            "  <div class=\"panel\">"
            "    <h2>📊 Monitor Panel</h2>"
            "    <p><b>Energy:</b> <span id=\"kwh\">%.2f</span> kWh</p>"
            "    <p><b>Relay State:</b> <span id=\"relay\">%s</span></p>"
            "    <p><b>Billing:</b> <span id=\"bill\">%s</span></p>"
            "    <p><b>Uptime:</b> <span id=\"up\">%u</span> s</p>"
            "  </div>"
            "  <div class=\"panel\">"
            "    <h2>⚡ 24-Hour Load Demand Profile</h2>"
            "    <div class=\"chart\">"
            "      <svg viewBox=\"0 0 500 160\" width=\"100%%\" height=\"100%%\" style=\"display:block;\">"
            "        <rect width=\"500\" height=\"160\" fill=\"#ffffff\"/>"
            "        <line x1=\"40\" y1=\"20\" x2=\"40\" y2=\"130\" stroke=\"#000000\" stroke-width=\"1.5\"/>"
            "        <line x1=\"40\" y1=\"130\" x2=\"480\" y2=\"130\" stroke=\"#000000\" stroke-width=\"1.5\"/>"
            "        <line x1=\"40\" y1=\"75\" x2=\"480\" y2=\"75\" stroke=\"#e2e8f0\" stroke-dasharray=\"3\"/>"
            "        <text x=\"5\" y=\"23\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">10000</text>"
            "        <text x=\"5\" y=\"78\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">7000</text>"
            "        <text x=\"5\" y=\"133\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">4000</text>"
            "        <text x=\"37\" y=\"148\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">0</text>"
            "        <text x=\"142\" y=\"148\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">6</text>"
            "        <text x=\"257\" y=\"148\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">12</text>"
            "        <text x=\"367\" y=\"148\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">18</text>"
            "        <text x=\"467\" y=\"148\" fill=\"#000000\" font-size=\"10\" font-weight=\"bold\">24</text>"
            "        <polyline fill=\"none\" stroke=\"#000000\" stroke-width=\"2\" points=\"40,90 60,110 80,120 100,125 120,120 140,135 160,105 180,35 200,20 220,45 240,75 260,85 280,90 300,128 320,95 340,110 360,122 380,100 400,72 420,40 440,35 460,125 480,60\"/>"
            "        <circle cx=\"40\" cy=\"90\" r=\"3\" fill=\"#000000\"/>"
            "        <circle cx=\"100\" cy=\"125\" r=\"3\" fill=\"#000000\"/>"
            "        <circle cx=\"200\" cy=\"20\" r=\"3\" fill=\"#000000\"/>"
            "        <circle cx=\"300\" cy=\"128\" r=\"3\" fill=\"#000000\"/>"
            "        <circle cx=\"440\" cy=\"35\" r=\"3\" fill=\"#000000\"/>"
            "        <circle cx=\"480\" cy=\"60\" r=\"3\" fill=\"#000000\"/>"
            "      </svg>"
            "    </div>"
            "  </div>"
            "  <div class=\"panel\">"
            "    <h2>⚙️ Control Panel</h2>"
            "    <button id=\"btn\" onclick=\"tog()\">%s</button>"
            "    <p id=\"res\" style=\"font-family:monospace;color:#38bdf8;font-size:0.8rem;margin-top:0.5rem;\">Ready</p>"
            "  </div>"
            "</div>"
            "<script>"
            "let rc = %s;"
            "async function poll() {"
            "  try {"
            "    let r = await fetch('/api/v1/meter');"
            "    let d = await r.json();"
            "    rc = d.relay_closed;"
            "    document.getElementById('kwh').innerText = d.kwh.toFixed(2);"
            "    document.getElementById('relay').innerHTML = rc ? '<span style=\"color:#16a34a;font-weight:bold;\">CLOSED (ON)</span>' : '<span style=\"color:#dc2626;font-weight:bold;\">OPEN (CUT)</span>';"
            "    document.getElementById('bill').innerHTML = rc ? '<span style=\"color:#16a34a;\">Paid</span>' : '<span style=\"color:#dc2626;\">Overdue</span>';"
            "    document.getElementById('up').innerText = Math.floor(d.uptime_ms / 1000);"
            "    let b = document.getElementById('btn');"
            "    b.style.background = rc ? '#dc2626' : '#16a34a';"
            "    b.innerText = rc ? '🔴 Cut Power' : '🟢 Restore Power';"
            "  } catch(e) {}"
            "}"
            "async function tog() {"
            "  let pw = prompt('Enter Password (Bearer Token):', 'Natska2026');"
            "  if (!pw) return;"
            "  let act = rc ? 'cut' : 'restore';"
            "  document.getElementById('res').innerText = 'Sending...';"
            "  try {"
            "    let res = await fetch('/api/v1/relay/' + act, {headers: {'Authorization': 'Bearer ' + pw}});"
            "    let j = await res.json();"
            "    document.getElementById('res').innerText = res.status === 200 ? 'Success: ' + j.relay : 'Err: ' + j.error;"
            "    poll();"
            "  } catch(e) {"
            "    document.getElementById('res').innerText = 'Connection error';"
            "  }"
            "}"
            "setInterval(poll, 3000);"
            "</script></body></html>",
            relay_closed ? "#dc2626" : "#16a34a",
            (double)current_kwh, r_txt, b_txt, up,
            relay_closed ? "🔴 Cut Power" : "🟢 Restore Power",
            relay_closed ? "true" : "false"
        );

        zsock_send(client_sock, tx_buf, html_len, 0);
    }
    else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        zsock_send(client_sock, not_found, strlen(not_found), 0);
    }

    zsock_shutdown(client_sock, ZSOCK_SHUT_RDWR);
    zsock_close(client_sock);
}

static void http_server_thread(void *p1, void *p2, void *p3) {
    int serv_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_sock < 0) return;

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int opt = 1;
    zsock_setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    zsock_bind(serv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    zsock_listen(serv_sock, 5);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = zsock_accept(serv_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock >= 0) handle_client(client_sock);
    }
}

K_THREAD_DEFINE(http_thread_id, 4096, http_server_thread, NULL, NULL, NULL, 5, 0, 0);

static void telemetry_thread(void *p1, void *p2, void *p3) {
    if (device_is_ready(relay.port)) {
        gpio_pin_configure_dt(&relay, GPIO_OUTPUT_ACTIVE);
    }
    wifi_connect();

    while (1) {
        k_msleep(5000);
        current_kwh += 0.05f;
        ring_buffer_push(current_kwh, relay_closed);
        LOG_INF("Current Meter Reading: %.2f kWh", (double)current_kwh);
    }
}

K_THREAD_DEFINE(telemetry_thread_id, 2048, telemetry_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void) {
    return 0;
}
