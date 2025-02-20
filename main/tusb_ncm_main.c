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
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "lwip/esp_netif_net_stack.h"
#include "tusb_ncm_demo.h"

static const char *TAG = "NCM/RNDIS";
#define DEF_IP "192.168.4.1"
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

static esp_err_t install_tinyusb_driver(void)
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    return tinyusb_driver_install(&tusb_cfg);
}

static esp_err_t create_usb_eth_if(esp_netif_t *s_netif,tusb_net_rx_cb_t tusb_net_rx_cb,tusb_net_free_tx_cb_t tusb_net_free_tx_cb)
{
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

static esp_err_t ether2usb_transmit_cb (void *h, void *buffer, size_t len)
{
#define TUSB_SEND_TO 100
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

void app_main(void)
{
    ESP_LOGI(TAG, "starting app for RNDIS and webusb");

    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Install TinyUSB driver
    ESP_ERROR_CHECK(install_tinyusb_driver());

    // Initialize the wired network interface
    init_wired_netif();

    // Initialize SPIFFS
    init_fs();
    
    ESP_ERROR_CHECK(resetful_server_start(CONFIG_EXAMPLE_WEB_MOUNT_POINT));

    tusb_cdc_handler_init();
}
