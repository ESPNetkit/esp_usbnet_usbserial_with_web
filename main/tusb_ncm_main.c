/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* DESCRIPTION:
 * This example demonstrates using ESP32-S2/S3 as a USB network device. It initializes WiFi in station mode,
 * connects and bridges the WiFi and USB networks, so the USB device acts as a standard network interface that
 * acquires an IP address from the AP/router which the WiFi station connects to.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include "esp_spiffs.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip_addr.h"
#include "lwip/init.h"
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_vfs.h"
#include "cJSON.h"

static const char *TAG = "wired_tusb_ncm";
#define DEF_IP "192.168.4.1"

#ifdef CONFIG_TINYUSB_NET_MODE_RNDIS
DRAM_ATTR uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};//for RNDIS
#endif

 
static void tinyusb_netif_free_buffer_cb(void *buffer, void *ctx)
{
    //TODO use slot instead of buffer from heap
    free(buffer);
}

static esp_err_t tinyusb_netif_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    esp_netif_t *s_netif=ctx;
    if (s_netif) {
        void *buf_copy = malloc(len);
        if (!buf_copy) {
            ESP_LOGE(TAG,"No Memory for size: %d",len);
            return ESP_ERR_NO_MEM;            
        } else {
            ESP_LOGD(TAG, "received bytes from ethernet %d ",len);
        }

        memcpy(buf_copy, buffer, len);
        return esp_netif_receive(s_netif, buf_copy, len, NULL);      
    } else {
        //Shall we assert here? 
    }
    return ESP_OK;
}

static esp_err_t create_usb_eth_if(esp_netif_t *s_netif,tusb_net_rx_cb_t tusb_net_rx_cb,tusb_net_free_tx_cb_t tusb_net_free_tx_cb)
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,      
    };
    
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    
    tinyusb_net_config_t net_config = {
        // locally administrated address for the ncm device as it's going to be used internally        
       .mac_addr ={0},                   
       .on_recv_callback = tusb_net_rx_cb, // tinyusb_netif_recv_cb,
       .free_tx_buffer = tusb_net_free_tx_cb, //wifi_netif_free_buffer_cb, // tinyusb_netif_free_buffer_cb,
       .user_context=s_netif               
    };
    ESP_ERROR_CHECK(esp_read_mac(net_config.mac_addr,  ESP_MAC_ETH));
    ESP_ERROR_CHECK(tinyusb_net_init(TINYUSB_USBDEV_0, &net_config));
              
    return ESP_OK; 
}


static void netif_l2_free_cb(void *h, void *buffer)
{ 
    free(buffer);
}

#define TUSB_SEND_TO 100
static esp_err_t ether2usb_transmit_cb (void *h, void *buffer, size_t len)
{
    esp_err_t esp_err = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(TUSB_SEND_TO));
    if (esp_err != ESP_OK){
        ESP_LOGE("Ethernet->USB", "Failed to send, retrying, error %d: %s", esp_err, esp_err_to_name(esp_err));
        esp_err = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(TUSB_SEND_TO) * 3);
    }
    if (esp_err != ESP_OK) {
        ESP_LOGE("Ethernet->USB", "Failed to send buffer to USB! %d: %s", esp_err, esp_err_to_name(esp_err));
    } else {
        ESP_LOGD("Ethernet->USB", "Sent to USB %zu ", len);
    }
    return ESP_OK;
}

static esp_netif_recv_ret_t ethernetif_receieve_cb(void *h, void *buffer, size_t len, void *l2_buff)
{
    return ethernetif_input(h,buffer,len,l2_buff);
}

static u_int32_t load_ip(const char* def_ip)
{
    int32_t def_ip_addr=ipaddr_addr(def_ip);
    return def_ip_addr;
} 

static esp_err_t create_virtual_net_if(esp_netif_t **res_s_netif)
{

    int32_t ip = load_ip(DEF_IP);    
    const esp_netif_ip_info_t esp_netif_soft_ap_ip = {
        .ip = { .addr = ip },
        .gw = { .addr = ip}, 
        .netmask = { .addr = ipaddr_addr("255.255.255.0")},
    };
    ESP_LOGI(TAG,"*********IP is: " IPSTR,IP2STR(&esp_netif_soft_ap_ip.ip)); 

    // 1) Derive the base config (very similar to IDF's default WiFi AP with DHCP server)
    esp_netif_inherent_config_t base_cfg =  {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP, 
        .ip_info = &esp_netif_soft_ap_ip,                   
        .if_key = "wired",
        .if_desc = "USB ncm config device",       
        .route_prio = 10
    };

    // 2) Use static config for driver's config pointing only to static transmit and free functions
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,                // not using an instance, USB-NCM is a static singleton (must be != NULL)                
        .transmit = ether2usb_transmit_cb,         // point to static Tx function        
        .driver_free_rx_buffer = netif_l2_free_cb    // point to Free Rx buffer function
    };

    // 3) USB-NCM is an Ethernet netif from lwip perspective, we already have IO definitions for that:
    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_receieve_cb,
        }        
    };


    esp_netif_config_t cfg = { // Config the esp-netif with:
        .base = &base_cfg,//   1) inherent config (behavioural settings of an interface)
        .driver = &driver_cfg,//   2) driver's config (connection to IO functions -- usb)
        .stack = &lwip_netif_config//   3) stack config (using lwip IO functions -- derive from eth)
    };
    esp_netif_t *s_netif= esp_netif_new(&cfg);    
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "Cannot initialize if interface Net device");
        return ESP_FAIL;
    }

    {
        uint8_t lwip_addr[6]={0};        
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_base_mac_addr_get(lwip_addr));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_mac(s_netif, lwip_addr));
    }

    // start the interface manually (as the driver has been started already)
    esp_netif_action_start(s_netif, 0, 0, 0);
    *res_s_netif =s_netif;

    return ESP_OK;
}

#include "esp_mac.h"
static esp_err_t init_wired_netif(void)
{
    static esp_netif_t *g_s_netif = NULL;    
    ESP_ERROR_CHECK(create_virtual_net_if(&g_s_netif));  
    ESP_ERROR_CHECK(create_usb_eth_if(g_s_netif,tinyusb_netif_recv_cb,tinyusb_netif_free_buffer_cb));           
    return ESP_OK;
}

static esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_EXAMPLE_WEB_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

static const char *REST_TAG = "esp-rest";
#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(REST_TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    rest_server_context_t *rest_context = (rest_server_context_t *)req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(REST_TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(REST_TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(REST_TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(REST_TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Simple handler for light brightness control */
static esp_err_t light_brightness_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *)(req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    int red = cJSON_GetObjectItem(root, "red")->valueint;
    int green = cJSON_GetObjectItem(root, "green")->valueint;
    int blue = cJSON_GetObjectItem(root, "blue")->valueint;
    ESP_LOGI(REST_TAG, "Light control: red = %d, green = %d, blue = %d", red, green, blue);
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

/* Simple handler for getting system handler */
static esp_err_t system_info_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "version", IDF_VER);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Simple handler for getting temperature data */
static esp_err_t temperature_data_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "raw", esp_random() % 20);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t start_rest_server(const char *base_path)
{
    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(REST_TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/v1/system/info",
        .method = HTTP_GET,
        .handler = system_info_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for fetching temperature data */
    httpd_uri_t temperature_data_get_uri = {
        .uri = "/api/v1/temp/raw",
        .method = HTTP_GET,
        .handler = temperature_data_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &temperature_data_get_uri);

    /* URI handler for light brightness control */
    httpd_uri_t light_brightness_post_uri = {
        .uri = "/api/v1/light/brightness",
        .method = HTTP_POST,
        .handler = light_brightness_post_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &light_brightness_post_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    return ESP_OK;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting app for RNDIS and webusb");
    
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Initialize the wired network interface
    init_wired_netif();

    //init spiffs
    init_fs();
    
    ESP_ERROR_CHECK(start_rest_server(CONFIG_EXAMPLE_WEB_MOUNT_POINT));
}
