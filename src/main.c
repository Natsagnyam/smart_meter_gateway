#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(billing_gateway, LOG_LEVEL_INF);

#define PORT 80
#define RING_BUFFER_SIZE 16 // Must be a power of 2 for bitwise masking

/* --- Wi-Fi Credentials --- */
#define WIFI_SSID "Temka room"
#define WIFI_PSK  "Namutemuhasar3"

/* --- RULE: Isolation & Cache-Line Padding for Control Structures --- */
struct producer_ctrl {
    uint8_t pad1[124]; 
    volatile uint32_t tail;
    uint8_t pad2[124]; 
} __attribute__((aligned(64)));

struct consumer_ctrl {
    uint8_t pad1[124]; 
    volatile uint32_t head;
    uint8_t pad2[124]; 
} __attribute__((aligned(64)));

static struct producer_ctrl p_ctrl = { .tail = 0 };
static struct consumer_ctrl c_ctrl = { .head = 0 };

static float kWh_meter_buffer[RING_BUFFER_SIZE];

/* Direct GPIO Spec for ESP32 GPIO 4 (Mains Power Cutoff Relay simulation) */
#define GPIO0_NODE DT_NODELABEL(gpio0)
static const struct device *gpio_dev = DEVICE_DT_GET(GPIO0_NODE);
#define RELAY_PIN 4

/* Complete HTML Web Dashboard with both element IDs */
static const char index_html[] = 
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html; charset=UTF-8\r\n"
"Connection: close\r\n\r\n"
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <title>Smart kWh Meter & Billing Gateway</title>\n"
"    <style>\n"
"        body { font-family: sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; display: flex; justify-content: center; }\n"
"        .container { width: 100%; max-width: 500px; background: #1e293b; padding: 20px; border-radius: 10px; }\n"
"        h1 { font-size: 20px; color: #38bdf8; text-align: center; margin-top: 0; }\n"
"        .card { background: #0f172a; padding: 15px; border-radius: 6px; margin-bottom: 15px; border: 1px solid #334155; }\n"
"        .label { font-size: 13px; color: #94a3b8; }\n"
"        .value { font-size: 24px; font-weight: bold; margin-top: 5px; }\n"
"        .badge { padding: 4px 8px; border-radius: 4px; font-size: 13px; display: inline-block; }\n"
"        .closed { background: #166534; color: #4ade80; }\n"
"        .open { background: #991b1b; color: #f87171; }\n"
"        .btn-group { display: flex; gap: 10px; }\n"
"        button { flex: 1; padding: 10px; border: none; border-radius: 5px; font-weight: bold; cursor: pointer; color: white; }\n"
"        .btn-cut { background: #dc2626; }\n"
"        .btn-restore { background: #16a34a; }\n"
"        .btn-refresh { background: #0284c7; width: 100%; margin-top: 10px; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <h1>⚡ Zephyr Smart Meter</h1>\n"
"        <div class=\"card\">\n"
"            <div class=\"label\">Current Reading</div>\n"
"            <div class=\"value\" id=\"kwh-val\">Loading...</div>\n"
"        </div>\n"
"        <div class=\"card\">\n"
"            <div class=\"label\">Mains Relay Status</div>\n"
"            <div class=\"value\" id=\"relay-val\"><span class=\"badge\">Loading...</span></div>\n"
"        </div>\n"
"        <div class=\"btn-group\">\n"
"            <button class=\"btn-cut\" onclick=\"cmd('/api/v1/relay/cut')\">Cut Power</button>\n"
"            <button class=\"btn-restore\" onclick=\"cmd('/api/v1/relay/restore')\">Restore Power</button>\n"
"        </div>\n"
"        <button class=\"btn-refresh\" onclick=\"fetchData()\">Refresh</button>\n"
"    </div>\n"
"    <script>\n"
"        function fetchData() {\n"
"            fetch('/api/v1/meter')\n"
"                .then(r => r.json())\n"
"                .then(d => {\n"
"                    document.getElementById('kwh-val').innerText = d.kwh.toFixed(2) + ' kWh';\n"
"                    let b = d.relay_state === 1 ? '<span class=\"badge closed\">CLOSED (ON)</span>' : '<span class=\"badge open\">OPEN (CUT)</span>';\n"
"                    document.getElementById('relay-val').innerHTML = b;\n"
"                })\n"
"                .catch(err => console.error('Fetch error:', err));\n"
"        }\n"
"        function cmd(url) {\n"
"            fetch(url).then(() => fetchData());\n"
"        }\n"
"        setInterval(fetchData, 5000);\n"
"        fetchData();\n"
"    </script>\n"
"</body>\n"
"</html>\n";

static int send_all(int sock, const char *buf, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        int sent = zsock_send(sock, buf + total_sent, len - total_sent, 0);
        if (sent < 0) {
            return -1;
        }
        total_sent += sent;
    }
    return 0;
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        LOG_INF("Wi-Fi Connected successfully! Waiting for DHCP lease...");
    } else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        LOG_WRN("Wi-Fi Disconnected. Reconnecting...");
    } else if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        char buf[NET_IPV4_ADDR_LEN];
        struct net_if_addr *if_addr = cb->info;
        if (if_addr->addr_type == NET_ADDR_DHCP) {
            net_addr_ntop(AF_INET, &if_addr->address.in_addr, buf, sizeof(buf));
            LOG_INF("==================================================");
            LOG_INF(" WEB DASHBOARD & REST API: http://%s/", buf);
            LOG_INF("==================================================");
        }
    }
}

static struct net_mgmt_event_callback wifi_cb;

void wifi_init_connection(void)
{
    net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT | 
                                 NET_EVENT_WIFI_DISCONNECT_RESULT | 
                                 NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&wifi_cb);

    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No network interface found!");
        return;
    }

    net_if_up(iface);

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
        LOG_ERR("Wi-Fi connection request failed!");
    }
}

void initialize_hardware(void)
{
    if (!device_is_ready(gpio_dev)) {
        LOG_ERR("GPIO controller device not ready!");
        return;
    }
    gpio_pin_configure(gpio_dev, RELAY_PIN, GPIO_OUTPUT_ACTIVE);
    gpio_pin_set(gpio_dev, RELAY_PIN, 1);
    LOG_INF("Hardware initialized: Relay set to CLOSED (Power ON)");
}

/* Simulated Metering Thread (Producer) */
void meter_sampling_thread(void)
{
    float simulated_kwh = 124.50f;

    while (1) {
        k_sleep(K_SECONDS(5));
        simulated_kwh += 0.05f; 

        /* --- RULE: No % operation. Use bitwise & for ring buffer masking --- */
        uint32_t idx = p_ctrl.tail & (RING_BUFFER_SIZE - 1);
        kWh_meter_buffer[idx] = simulated_kwh;
        p_ctrl.tail++;

        LOG_INF("Current Meter Reading: %.2f kWh", (double)simulated_kwh);
    }
}

K_THREAD_DEFINE(meter_tid, 1024, meter_sampling_thread, NULL, NULL, NULL, 5, 0, 0);

/* Web Dashboard & REST API Server Thread */
void networking_server_thread(void)
{
    int serv_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    serv_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_sock < 0) {
        LOG_ERR("Failed to create socket: %d", errno);
        return;
    }

    int opt = 1;
    zsock_setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (zsock_bind(serv_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERR("Failed to bind socket: %d", errno);
        zsock_close(serv_sock);
        return;
    }

    if (zsock_listen(serv_sock, 4) < 0) {
        LOG_ERR("Failed to listen: %d", errno);
        zsock_close(serv_sock);
        return;
    }

    LOG_INF("HTTP Web Server listening on port %d...", PORT);

    while (1) {
        client_sock = zsock_accept(serv_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            continue;
        }

        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        zsock_setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char rx_buffer[256];
        int bytes_received = zsock_recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        
        if (bytes_received > 0) {
            rx_buffer[bytes_received] = '\0';

            char response[512];
            int len = 0;

            if (strstr(rx_buffer, "GET /api/v1/relay/cut")) {
                gpio_pin_set(gpio_dev, RELAY_PIN, 0);
                LOG_WRN("BILLING ALERT: Payment missed! Power disconnected via relay.");
                len = snprintk(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                    "{\"status\":\"success\",\"action\":\"cut\",\"relay_state\":0}\n");
                send_all(client_sock, response, len);
            } 
            else if (strstr(rx_buffer, "GET /api/v1/relay/restore")) {
                gpio_pin_set(gpio_dev, RELAY_PIN, 1);
                LOG_INF("BILLING NOTIFICATION: Payment verified. Power automatically restored.");
                len = snprintk(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                    "{\"status\":\"success\",\"action\":\"restore\",\"relay_state\":1}\n");
                send_all(client_sock, response, len);
            }
            else if (strstr(rx_buffer, "GET /api/v1/meter")) {
                uint32_t latest_idx = (p_ctrl.tail - 1) & (RING_BUFFER_SIZE - 1);
                int current_relay = gpio_pin_get(gpio_dev, RELAY_PIN);
                len = snprintk(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                    "{\"device\":\"ESP32-SmartMeter\",\"kwh\":%.2f,\"relay_state\":%d}\n",
                    (double)kWh_meter_buffer[latest_idx], current_relay);
                send_all(client_sock, response, len);
            }
            else {
                /* Serve the complete Web Dashboard */
                send_all(client_sock, index_html, sizeof(index_html) - 1);
            }
        }

        zsock_shutdown(client_sock, SHUT_RDWR);
        zsock_close(client_sock);
    }
}

K_THREAD_DEFINE(net_tid, 2048, networking_server_thread, NULL, NULL, NULL, 6, 0, 0);

int main(void)
{
    LOG_INF("Starting Smart kWh Meter Gateway...");
    initialize_hardware();
    wifi_init_connection();
    
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
