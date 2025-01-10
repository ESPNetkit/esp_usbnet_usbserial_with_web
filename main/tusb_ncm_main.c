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
#include <sys/types.h>
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip_addr.h"
#include "lwip/init.h"
#include "esp_http_server.h"
#include <sys/param.h> // Add this line

static const char *TAG = "wired_tusb_ncm";
#ifdef CONFIG_TINYUSB_NET_MODE_RNDIS
DRAM_ATTR uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};//for RNDIS
#endif

//#define LOG_PAYLOAD

 
static void tinyusb_netif_free_buffer_cb(void *buffer, void *ctx)
{
free(buffer);
}

//static uint8_t buf_copy[600]={};


static esp_err_t tinyusb_netif_recv_cb(void *buffer, uint16_t len, void *ctx)
{
esp_netif_t *s_netif=ctx;// g_s_netif;
    if (s_netif) {
#ifdef LOG_PAYLOAD
ESP_LOG_BUFFER_HEX("USB->Ethernet", buffer, len);
#endif
        void *buf_copy = malloc(len);
        if (!buf_copy) {
ESP_LOGE(TAG,"No Memory for size: %d",len);
            return ESP_ERR_NO_MEM;            
        }else{
ESP_LOGD(TAG, "received bytes from ethernet %d ",len);
}

//len=sizeof(buf_copy);
        memcpy(buf_copy, buffer, len);
        return esp_netif_receive(s_netif, buf_copy, len, NULL);      
    }else{
//ESP_LOGE(TAG,"No Interface");
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
       .on_recv_callback =tusb_net_rx_cb, // tinyusb_netif_recv_cb,
       .free_tx_buffer=tusb_net_free_tx_cb, //wifi_netif_free_buffer_cb, // tinyusb_netif_free_buffer_cb,
       .user_context=s_netif               
    };
//uint8_t e_mac[6]={0};
ESP_ERROR_CHECK(esp_read_mac(net_config.mac_addr,  ESP_MAC_ETH));

    ESP_ERROR_CHECK(tinyusb_net_init(TINYUSB_USBDEV_0, &net_config));
              
    return ESP_OK; 
}


////////////////////////////////////////////////////////////////////////////////

static void netif_l2_free_cb(void *h, void *buffer)
{ 
    free(buffer);
}


#define TUSB_SEND_TO 100
static esp_err_t ether2usb_transmit_cb (void *h, void *buffer, size_t len)
{

//#ifdef LOG_PAYLOAD
//ESP_LOG_BUFFER_HEX("Ethernet->USB", buffer, len);
 // #endif
esp_err_t esp_err=tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(TUSB_SEND_TO));
if (esp_err!= ESP_OK){
ESP_LOGE("Ethernet->USB", "Failed to send, retrying , error %d" ,esp_err);
esp_err=tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(TUSB_SEND_TO)*3);
}
//esp_err_t esp_err=tinyusb_net_send_async (buffer, len,NULL);


//esp_err_t esp_err=tinyusb_net_send(buffer,len,NULL); 
    if (esp_err!= ESP_OK) {
        ESP_LOGE("Ethernet->USB", "Failed to send buffer to USB! %d" ,esp_err);
     //   free(buffer);//TODO: check this with the fill componenet that  is tinyusb_net_send_sync (1.5.0)
    }else{
ESP_LOGD("Ethernet->USB", "Sent to USB %d ",len);
}
    return ESP_OK;
}



static esp_netif_recv_ret_t ethernetif_receieve_cb(void *h, void *buffer, size_t len, void *l2_buff)
{
    return ethernetif_input(h,buffer,len,l2_buff);
}

#define NS "SNIFFER"
#define DEF_IP "192.168.4.1"
 
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
        .transmit =ether2usb_transmit_cb,         // point to static Tx function        
        .driver_free_rx_buffer =netif_l2_free_cb    // point to Free Rx buffer function
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

    /*
    uint32_t  lease_opt = 1000;// set the minimum lease time
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease_opt, sizeof(lease_opt));        
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;   
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));    
    esp_netif_dns_info_t dns={.ip.u_addr.ip4.addr=ipaddr_addr( "192.168.5.2"),dns.ip.type = IPADDR_TYPE_V4};        
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns));
    */

    // start the interface manually (as the driver has been started already)
    esp_netif_action_start(s_netif, 0, 0, 0);
    *res_s_netif =s_netif;

    return ESP_OK;
}

 /**
 *  In this scenario of configuring WiFi, we setup USB-Ethernet to create a virtual network and run DHCP server,
 *  so it could assign an IP address to the PC
 *
 *           ESP32               PC
 *      |    lwip MAC=...01   |                        eth NIC MAC=...02
 *      | <DHCP server>   usb | <->  [ USB-NCM device acting as eth-NIC ]
 *      | <HTTP server>       | 
 *
 *  From the PC's NIC perspective the board acts as a separate network with it's own IP and MAC address,
 *  but the virtual ethernet NIC has also it's own IP and MAC address (configured via tinyusb_net_init()).
 *  That's why we need to create the virtual network with *different* MAC address.
 *  Here, we use two different OUI range MAC addresses.
 */
#include "esp_mac.h"
static esp_err_t init_wired_netif(void)
{
           
static esp_netif_t *g_s_netif = NULL;    
 ESP_ERROR_CHECK(create_virtual_net_if(&g_s_netif));  
    ESP_ERROR_CHECK(create_usb_eth_if(g_s_netif,tinyusb_netif_recv_cb,tinyusb_netif_free_buffer_cb));           
    return ESP_OK;
}

esp_err_t get_handler(httpd_req_t *req)
{
    const char resp[] = "URI GET Response";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t post_handler(httpd_req_t *req)
{
    char content[100];
    size_t recv_size = MIN(req->content_len, sizeof(content));
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    const char resp[] = "URI POST Response";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t index_handler(httpd_req_t *req)
{
    const char resp[] = "<!DOCTYPE html><html><head><title>ESPNetKit</title></head>"
                        "<body><h1>This is the ESPNetKit webserver through USB Ethernet</h1>"
                        "<p>Visit our <a href=\"https://github.com/ESPNetkit/esp_usbnet_usbserial_with_web\">"
                        "GitHub repository</a> for more information.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_index = {
            .uri      = "/index.html",
            .method   = HTTP_GET,
            .handler  = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_index);
        httpd_uri_t uri_default = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_default);
        httpd_uri_t uri_get = {
            .uri      = "/uri",
            .method   = HTTP_GET,
            .handler  = get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t uri_post = {
            .uri      = "/uri",
            .method   = HTTP_POST,
            .handler  = post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_post);
    }
    return server;
}

void stop_webserver(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting app for RNDIS and webusb");
    
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Initialize the wired network interface
    init_wired_netif();
    
    // Start the HTTP server
    start_webserver();
}
