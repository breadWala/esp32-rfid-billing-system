#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "rc522.h"

// ============================================
// TAG - defined first, used everywhere
// ============================================
static const char* TAG = "RFID_CART";

// ============================================
// DEFINES - all uppercase, consistent
// ============================================
#define WIFI_SSID       "Kotida"
#define WIFI_PASSWORD   "12345678"
#define SALE_PASSWORD   "1234"
#define MAX_PRODUCTS    3
#define MAX_TAGS        6
#define MAX_CART_ITEMS  6

// ============================================
// STRUCTS
// ============================================

// 1. Master catalog - stores product info
typedef struct {
    uint8_t  product_id;
    char     name[32];
    uint32_t price;
    uint8_t  total_stock; // total tags owned under this product
    uint8_t  available;   // tags not currently in any cart
} product_t;

// 2. RFID tag - many UIDs map to one product_id
typedef struct {
    uint64_t uid;
    uint8_t  product_id;
    bool     in_cart;
} rfid_tag_t;

// 3. Items currently in cart
typedef struct {
    uint64_t uid;
    uint8_t  product_id; // received from rfid_tag_t
    char     name[32];   // received from product_t
    uint32_t price;      // received from product_t
} cart_item_t;

// ============================================
// DATABASE - hardcoded
// ============================================
product_t products[MAX_PRODUCTS] = {
    {1, "Chips", 20, 3, 3},
    {2, "Soap",  30, 2, 2},
    {3, "Cards", 50, 1, 1}
};

rfid_tag_t rfid_tags[MAX_TAGS] = {
    // Chips (product_id = 1) - 3 tags
    {0x5A2CCF73CA,   1, false},
    {0x812CB6716A,   1, false},
    {0x5E4D3C2B,   1, false},
    // Soap (product_id = 2) - 2 tags
    {0x4A2CB468BA,   2, false},
    {0xB42CBE2C0A,   2, false},
    // Cards (product_id = 3) - 1 tag (5-byte UID)
    {0xE3029098E9, 3, false}
};

// ============================================
// CART STATE - lives in RAM, resets on reboot
// ============================================
cart_item_t cart[MAX_CART_ITEMS];
uint8_t     cart_count  = 0;
uint32_t    total_price = 0;

// ============================================
// FORWARD DECLARATION
// ============================================
void start_http_server(void);

// ============================================
// LOOKUP FUNCTIONS
// ============================================

// UID -> rfid_tag_t* (which tag was scanned?)
rfid_tag_t* find_tag(uint64_t uid)
{
    for (int i = 0; i < MAX_TAGS; i++)
    {
        if (rfid_tags[i].uid == uid)
            return &rfid_tags[i];
    }
    return NULL;
}

// product_id -> product_t* (get product details)
product_t* find_product(uint8_t product_id)
{
    for (int i = 0; i < MAX_PRODUCTS; i++)
    {
        if (products[i].product_id == product_id)
            return &products[i];
    }
    return NULL;
}

// Check if UID already in cart - prevents double scan
bool is_in_cart(uint64_t uid)
{
    for (int i = 0; i < cart_count; i++)
    {
        if (cart[i].uid == uid)
            return true;
    }
    return false;
}

// ============================================
// CART OPERATIONS
// ============================================

void add_to_cart(rfid_tag_t *rfid_entry)
{
    if (cart_count >= MAX_CART_ITEMS) {
        ESP_LOGW(TAG, "Cart full!");
        return;
    }

    product_t *product = find_product(rfid_entry->product_id);
    if (product == NULL) return;

    if (product->available == 0) {
        ESP_LOGW(TAG, "%s out of stock!", product->name);
        return;
    }

    cart[cart_count].uid        = rfid_entry->uid;
    cart[cart_count].product_id = rfid_entry->product_id;
    strncpy(cart[cart_count].name, product->name, 32);
    cart[cart_count].price      = product->price;
    cart_count++;
    total_price                += product->price;
    rfid_entry->in_cart         = true;
    product->available--;

    ESP_LOGI(TAG, "[+] %s - Rs.%lu | Total: Rs.%lu",
             product->name, (unsigned long)product->price,
             (unsigned long)total_price);
}

void clear_cart(void)
{
    for (int i = 0; i < cart_count; i++)
    {
        rfid_tag_t *rfid_entry = find_tag(cart[i].uid);
        if (rfid_entry)
            rfid_entry->in_cart = false;
        // available NOT restored - item was sold
    }
    memset(cart, 0, sizeof(cart));
    cart_count  = 0;
    total_price = 0;
    ESP_LOGI(TAG, "Cart cleared. Ready for next customer.");
}

// ============================================
// HTTP HANDLERS
// ============================================

// GET /cart - show current bill
esp_err_t cart_handler(httpd_req_t *req)
{
    char response[2048] = {0};
    char line[128];

    strcat(response,
        "<!DOCTYPE html><html><head>"
        "<title>RFID Cart</title>"
        "<meta http-equiv='refresh' content='3'>"
        "<style>"
        "body{font-family:sans-serif;padding:20px;}"
        "table{width:100%;border-collapse:collapse;}"
        "td,th{padding:10px;border:1px solid #ddd;}"
        "th{background:#4CAF50;color:white;}"
        ".total{font-size:1.5em;font-weight:bold;color:#4CAF50;}"
        ".btn{background:#e74c3c;color:white;padding:15px 30px;"
        "border:none;font-size:1.2em;cursor:pointer;border-radius:5px;}"
        "</style></head><body>"
        "<h2>Shopping Cart</h2>"
        "<table><tr><th>#</th><th>Product</th><th>Price</th></tr>"
    );

    for (int i = 0; i < cart_count; i++) {
        snprintf(line, sizeof(line),
            "<tr><td>%d</td><td>%s</td><td>Rs.%lu</td></tr>",
            i + 1, cart[i].name, (unsigned long)cart[i].price);
        strcat(response, line);
    }

    char footer[512];
    snprintf(footer, sizeof(footer),
        "</table><br>"
        "<p class='total'>Total: Rs.%lu (%d items)</p>"
        "<hr><h3>Finalize Sale</h3>"
        "<form action='/finalize' method='POST'>"
        "<input type='password' name='pin' placeholder='Enter PIN' "
        "style='padding:10px;font-size:1em;'>"
        "<button class='btn' type='submit'>Confirm Sale</button>"
        "</form></body></html>",
        (unsigned long)total_price, cart_count);
    strcat(response, footer);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// GET /inventory - show stock levels
esp_err_t inventory_handler(httpd_req_t *req)
{
    char response[2048] = {0};
    char line[256];

    strcat(response,
        "<!DOCTYPE html><html><head>"
        "<title>Inventory</title>"
        "<meta http-equiv='refresh' content='5'>"
        "<style>"
        "body{font-family:sans-serif;padding:20px;}"
        "table{width:100%;border-collapse:collapse;}"
        "td,th{padding:10px;border:1px solid #ddd;}"
        "th{background:#3498db;color:white;}"
        ".low{color:red;font-weight:bold;}"
        "</style></head><body>"
        "<h2>Inventory</h2>"
        "<table><tr>"
        "<th>Product</th><th>Price</th>"
        "<th>Total Stock</th><th>Available</th><th>Sold</th>"
        "</tr>"
    );

    for (int i = 0; i < MAX_PRODUCTS; i++) {
        uint8_t sold = products[i].total_stock - products[i].available;
        snprintf(line, sizeof(line),
            "<tr><td>%s</td><td>Rs.%lu</td><td>%d</td>"
            "<td class='%s'>%d</td><td>%d</td></tr>",
            products[i].name,
            (unsigned long)products[i].price,
            products[i].total_stock,
            products[i].available <= 1 ? "low" : "",
            products[i].available,
            sold);
        strcat(response, line);
    }

    strcat(response, "</table></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// POST /finalize - confirm sale with PIN
esp_err_t finalize_handler(httpd_req_t *req)
{
    char body[128] = {0};

    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *pin_start = strstr(body, "pin=");
    if (!pin_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing PIN");
        return ESP_FAIL;
    }
    char *pin = pin_start + 4; // skip "pin="

    if (strcmp(pin, SALE_PASSWORD) != 0) {
        const char *fail =
            "<html><body>"
            "<h2>Wrong PIN!</h2>"
            "<a href='/cart'>Back to Cart</a>"
            "</body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, fail, strlen(fail));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Sale finalized! Total: Rs.%lu, Items: %d",
             (unsigned long)total_price, cart_count);

    uint32_t final_total = total_price;
    uint8_t  final_count = cart_count;
    clear_cart();

    char response[512];
    snprintf(response, sizeof(response),
        "<html><body style='font-family:sans-serif;padding:20px;'>"
        "<h2>Sale Complete!</h2>"
        "<p>Items sold: %d</p>"
        "<p><strong>Amount: Rs.%lu</strong></p><hr>"
        "<a href='/cart'>New Customer</a> | "
        "<a href='/inventory'>View Inventory</a>"
        "</body></html>",
        final_count, (unsigned long)final_total);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// Register all routes and start server
void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t cart_uri = {
        .uri     = "/cart",
        .method  = HTTP_GET,
        .handler = cart_handler
    };
    httpd_uri_t inventory_uri = {
        .uri     = "/inventory",
        .method  = HTTP_GET,
        .handler = inventory_handler
    };
    httpd_uri_t finalize_uri = {
        .uri     = "/finalize",
        .method  = HTTP_POST,
        .handler = finalize_handler
    };

    httpd_register_uri_handler(server, &cart_uri);
    httpd_register_uri_handler(server, &inventory_uri);
    httpd_register_uri_handler(server, &finalize_uri);

    ESP_LOGI(TAG, "HTTP server ready!");
    ESP_LOGI(TAG, "  /cart      -> current bill");
    ESP_LOGI(TAG, "  /inventory -> stock levels");
    ESP_LOGI(TAG, "  /finalize  -> confirm sale");
}

// ============================================
// WIFI
// ============================================

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_http_server();
    }
}

void connect_wifi(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to: %s", WIFI_SSID);
}

// ============================================
// RC522 HANDLER
// ============================================

static void rc522_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (id != RC522_EVENT_TAG_SCANNED) return;

    rc522_event_data_t *event   = (rc522_event_data_t *) data;
    rc522_tag_t        *hw_scan = (rc522_tag_t *) event->ptr;
    uint64_t            uid     = hw_scan->serial_number;

    ESP_LOGI(TAG, "Card detected: %llX", uid);

    // 1. Duplicate check
    if (is_in_cart(uid)) {
        ESP_LOGD(TAG, "Duplicate ignored: %llX", uid);
        return;
    }

    // 2. Find tag in database
    rfid_tag_t *rfid_entry = find_tag(uid);
    if (rfid_entry == NULL) {
        ESP_LOGW(TAG, "Unknown tag: %llX", uid);
        return;
    }

    // 3. Add to cart
    add_to_cart(rfid_entry);
}

// ============================================
// MAIN
// ============================================

void app_main(void)
{
    // NVS init - required for WiFi internally
    nvs_flash_init();

    // Connect WiFi - HTTP server starts once connected
    connect_wifi();

    // Start RFID scanner
    rc522_config_t config = {
        .spi.host      = SPI3_HOST,
        .spi.miso_gpio = 19,
        .spi.mosi_gpio = 23,
        .spi.sck_gpio  = 18,
        .spi.sda_gpio  = 5
    };

    rc522_handle_t scanner;
    rc522_create(&config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_ANY, rc522_handler, NULL);
    rc522_start(scanner);

    ESP_LOGI(TAG, "System ready. Waiting for cards...");
}
