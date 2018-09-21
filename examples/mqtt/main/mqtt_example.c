/*
 * Copyright (c) 2014-2016 Alibaba Group. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "iot_import.h"
#include "iot_export.h"

#include "dht11.h"
#include "ds18b20.h"
#include "nvs_str.h"

#include "tcpip_adapter.h"
#include "esp_smartconfig.h"

#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

#define PRODUCT_KEY             CONFIG_PRODUCT_KEY
#define DEVICE_NAME             CONFIG_DEVICE_NAME
#define DEVICE_SECRET           CONFIG_DEVICE_SECRET

static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

static EventGroupHandle_t wifi_event_group;
static const char *TAG = "MQTT";
static const char *SBH = "SMART CONFIG";

xQueueHandle xDHTQueue;
xQueueHandle x18B20Queue;
TaskHandle_t vSCTask_Handle;

int wifiSta = 0, mqttSta = 0, scSta = 0;
uint8_t flagSmartConfig = 0;
wifi_config_t nvs_wifi;

static char __product_key[PRODUCT_KEY_LEN + 1];
static char __device_name[DEVICE_NAME_LEN + 1];
static char __device_secret[DEVICE_SECRET_LEN + 1];

/* These are pre-defined topics */
//#define TOPIC_UPDATE           "/"PRODUCT_KEY"/"DEVICE_NAME"/user/update"
#define TOPIC_UPDATE            "/sys/"PRODUCT_KEY"/"DEVICE_NAME"/thing/event/property/post"
#define TOPIC_ERROR             "/"PRODUCT_KEY"/"DEVICE_NAME"/user/update/error"
#define TOPIC_GET               "/"PRODUCT_KEY"/"DEVICE_NAME"/user/get"
#define TOPIC_DATA              "/"PRODUCT_KEY"/"DEVICE_NAME"/user/data"

/* These are pre-defined topics format*/
#define TOPIC_UPDATE_FMT            "/%s/%s/update"
#define TOPIC_ERROR_FMT             "/%s/%s/update/error"
#define TOPIC_GET_FMT               "/%s/%s/get"
#define TOPIC_DATA_FMT              "/%s/%s/data"

#define MQTT_MSGLEN             (1024)

#define EXAMPLE_TRACE(fmt, ...)  \
    do { \
        HAL_Printf("%s|%03d :: ", __func__, __LINE__); \
        HAL_Printf(fmt, ##__VA_ARGS__); \
        HAL_Printf("%s", "\r\n"); \
    } while(0)

void toggle_led_task(void* pvParameters)
{
	static int level = 0;
	int time = 1000;

	while(1)
	{
		if(scSta == 1){
			time = 150;
			gpio_set_level(GPIO_NUM_2, level);
			level = 1-level;
		}else{
			if(wifiSta == 1){
				gpio_set_level(GPIO_NUM_2, level);
				level = 1-level;
				time = 1000;
			}
			if(mqttSta == 1)time = 500;
		}
		vTaskDelay(time / portTICK_PERIOD_MS);
	}
}

void event_handle(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    uintptr_t packet_id = (uintptr_t)msg->msg;
    iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;

    switch (msg->event_type) {
        case IOTX_MQTT_EVENT_UNDEF:
            EXAMPLE_TRACE("undefined event occur.");
            break;

        case IOTX_MQTT_EVENT_DISCONNECT:
        	mqttSta = 0;
            EXAMPLE_TRACE("MQTT disconnect.");
            break;

        case IOTX_MQTT_EVENT_RECONNECT:
        	mqttSta = 0;
            EXAMPLE_TRACE("MQTT reconnect.");
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_SUCCESS:
            EXAMPLE_TRACE("subscribe success, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_TIMEOUT:
            EXAMPLE_TRACE("subscribe wait ack timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_NACK:
            EXAMPLE_TRACE("subscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_SUCCESS:
            EXAMPLE_TRACE("unsubscribe success, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_TIMEOUT:
            EXAMPLE_TRACE("unsubscribe timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_NACK:
            EXAMPLE_TRACE("unsubscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_SUCCESS:
        	mqttSta = 1;
            EXAMPLE_TRACE("publish success, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_TIMEOUT:
        	mqttSta = 0;
            EXAMPLE_TRACE("publish timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_NACK:
        	mqttSta = 0;
            EXAMPLE_TRACE("publish nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_RECVEIVED:
        	mqttSta = 0;
            EXAMPLE_TRACE("topic message arrived but without any related handle: topic=%.*s, topic_msg=%.*s",
                          topic_info->topic_len,
                          topic_info->ptopic,
                          topic_info->payload_len,
                          topic_info->payload);
            break;

        case IOTX_MQTT_EVENT_BUFFER_OVERFLOW:
            EXAMPLE_TRACE("buffer overflow, %s", msg->msg);
            break;

        default:
            EXAMPLE_TRACE("Should NOT arrive here.");
            break;
    }
}

static void _demo_message_arrive(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    iotx_mqtt_topic_info_pt ptopic_info = (iotx_mqtt_topic_info_pt) msg->msg;

    /* print topic name and topic message */
    EXAMPLE_TRACE("----");
    EXAMPLE_TRACE("packetId: %d", ptopic_info->packet_id);
    EXAMPLE_TRACE("Topic: '%.*s' (Length: %d)",
                  ptopic_info->topic_len,
                  ptopic_info->ptopic,
                  ptopic_info->topic_len);
    EXAMPLE_TRACE("Payload: '%.*s' (Length: %d)",
                  ptopic_info->payload_len,
                  ptopic_info->payload,
                  ptopic_info->payload_len);
    EXAMPLE_TRACE("----");
}

#ifndef MQTT_ID2_AUTH
int mqtt_client(void)
{
    int rc = 0, msg_len;
    void *pclient;
    iotx_conn_info_pt pconn_info;
    iotx_mqtt_param_t mqtt_params;
    iotx_mqtt_topic_info_t topic_msg;
    char msg_pub[128];
    char *msg_buf = NULL, *msg_readbuf = NULL;
    envT env_ToAliyun;
    float temp_float;
    portBASE_TYPE xStatus_DHT, xStatus_18B20;

    if (NULL == (msg_buf = (char *)HAL_Malloc(MQTT_MSGLEN))) {
        EXAMPLE_TRACE("not enough memory");
        rc = -1;
        goto do_exit;
    }

    if (NULL == (msg_readbuf = (char *)HAL_Malloc(MQTT_MSGLEN))) {
        EXAMPLE_TRACE("not enough memory");
        rc = -1;
        goto do_exit;
    }

    HAL_GetProductKey(__product_key);
    HAL_GetDeviceName(__device_name);
    HAL_GetDeviceSecret(__device_secret);

    /* Device AUTH */
    if (0 != IOT_SetupConnInfo(__product_key, __device_name, __device_secret, (void **)&pconn_info)) {
        EXAMPLE_TRACE("AUTH request failed!");
        rc = -1;
        goto do_exit;
    }

    /* Initialize MQTT parameter */
    memset(&mqtt_params, 0x0, sizeof(mqtt_params));

    mqtt_params.port = pconn_info->port;
    mqtt_params.host = pconn_info->host_name;
    mqtt_params.client_id = pconn_info->client_id;
    mqtt_params.username = pconn_info->username;
    mqtt_params.password = pconn_info->password;
    mqtt_params.pub_key = pconn_info->pub_key;

    mqtt_params.request_timeout_ms = 2000;
    mqtt_params.clean_session = 0;
    mqtt_params.keepalive_interval_ms = 60000;
    mqtt_params.pread_buf = msg_readbuf;
    mqtt_params.read_buf_size = MQTT_MSGLEN;
    mqtt_params.pwrite_buf = msg_buf;
    mqtt_params.write_buf_size = MQTT_MSGLEN;

    mqtt_params.handle_event.h_fp = event_handle;
    mqtt_params.handle_event.pcontext = NULL;


    /* Construct a MQTT client with specify parameter */
    pclient = IOT_MQTT_Construct(&mqtt_params);
    if (NULL == pclient) {
        EXAMPLE_TRACE("MQTT construct failed");
        rc = -1;
        goto do_exit;
    }

    /* Initialize topic information */
    memset(&topic_msg, 0x0, sizeof(iotx_mqtt_topic_info_t));
    strcpy(msg_pub, "update: hello! start!");

    topic_msg.qos = IOTX_MQTT_QOS1;
    topic_msg.retain = 0;
    topic_msg.dup = 0;
    topic_msg.payload = (void *)msg_pub;
    topic_msg.payload_len = strlen(msg_pub);

    rc = IOT_MQTT_Publish(pclient, TOPIC_UPDATE, &topic_msg);
    if (rc < 0) {
        IOT_MQTT_Destroy(&pclient);
        EXAMPLE_TRACE("error occur when publish");
        rc = -1;
        goto do_exit;
    }

    EXAMPLE_TRACE("\n publish message: \n topic: %s\n payload: \%s\n rc = %d", TOPIC_UPDATE, topic_msg.payload, rc);

    /* Subscribe the specific topic */
    rc = IOT_MQTT_Subscribe(pclient, TOPIC_DATA, IOTX_MQTT_QOS1, _demo_message_arrive, NULL);
    if (rc < 0) {
        IOT_MQTT_Destroy(&pclient);
        EXAMPLE_TRACE("IOT_MQTT_Subscribe() failed, rc = %d", rc);
        rc = -1;
        goto do_exit;
    }

    /* Initialize topic information */
    memset(msg_pub, 0x0, 128);
    strcpy(msg_pub, "data: hello! start!");
    memset(&topic_msg, 0x0, sizeof(iotx_mqtt_topic_info_t));
    topic_msg.qos = IOTX_MQTT_QOS1;
    topic_msg.retain = 0;
    topic_msg.dup = 0;
    topic_msg.payload = (void *)msg_pub;
    topic_msg.payload_len = strlen(msg_pub);

    rc = IOT_MQTT_Publish(pclient, TOPIC_DATA, &topic_msg);
    EXAMPLE_TRACE("\n publish message: \n topic: %s\n payload: \%s\n rc = %d", TOPIC_DATA, topic_msg.payload, rc);

    IOT_MQTT_Yield(pclient, 200);

    do {
    	xStatus_DHT = xQueueReceive(xDHTQueue, &env_ToAliyun, 0);
    	if(xStatus_DHT == pdPASS)
    	{
    		msg_len = snprintf(msg_pub, sizeof(msg_pub), "{params: {temperature: %d, humidity: %d, Ftemp: %d}, method: 'thing.event.property.post'}",
    				env_ToAliyun.temp, env_ToAliyun.humidity, env_ToAliyun.Ftemp);

			topic_msg.payload = (void *)msg_pub;
			topic_msg.payload_len = msg_len;

			rc = IOT_MQTT_Publish(pclient, TOPIC_UPDATE, &topic_msg);
			if (rc < 0) {
				EXAMPLE_TRACE("error occur when publish");
				rc = -1;
				break;
			}
    	}

    	xStatus_18B20 = xQueueReceive(x18B20Queue, &temp_float, 0);
    	if(xStatus_18B20 == pdPASS)
    	{
    		msg_len = snprintf(msg_pub, sizeof(msg_pub), "{params: {temp_float: %f}, method: 'thing.event.property.post'}",
    				temp_float);

			topic_msg.payload = (void *)msg_pub;
			topic_msg.payload_len = msg_len;

			rc = IOT_MQTT_Publish(pclient, TOPIC_UPDATE, &topic_msg);
			if (rc < 0) {
				EXAMPLE_TRACE("error occur when publish");
				rc = -1;
				break;
			}
    	}

        /* handle the MQTT packet received from TCP or SSL connection */
        IOT_MQTT_Yield(pclient, 200);
        HAL_SleepMs(500);
    } while (1);

    IOT_MQTT_Yield(pclient, 200);

    IOT_MQTT_Unsubscribe(pclient, TOPIC_DATA);

    IOT_MQTT_Yield(pclient, 200);

    IOT_MQTT_Destroy(&pclient);

do_exit:
    if (NULL != msg_buf) {
        HAL_Free(msg_buf);
    }

    if (NULL != msg_readbuf) {
        HAL_Free(msg_readbuf);
    }

    return rc;
}
#endif /* MQTT_ID2_AUTH */

void mqtt_task(void *pvParameter)
{
    ESP_LOGI(TAG, "MQTT task started...");

    while (1) { // reconnect to tls

        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

        IOT_OpenLog("mqtt");
        IOT_SetLogLevel(IOT_LOG_DEBUG);

        HAL_SetProductKey(PRODUCT_KEY);
        HAL_SetDeviceName(DEVICE_NAME);
        HAL_SetDeviceSecret(DEVICE_SECRET);

        ESP_LOGI(TAG, "MQTT client example begin, free heap size:%d", esp_get_free_heap_size());

#ifndef MQTT_ID2_AUTH
        mqtt_client();
#else
        mqtt_client_secure();
#endif

        IOT_DumpMemoryStats(IOT_LOG_DEBUG);
        IOT_CloseLog();

        ESP_LOGI(TAG, "MQTT client example end, free heap size:%d", esp_get_free_heap_size());
    }
}

static void DHT11_GetData_Task(void* pvParameters)
{
	portBASE_TYPE xStatus;
	envT envData;
	setDHTPin(4);
	printf("Starting DHT measurement!\n");

	while(1)
	{
		if(getEnvData(&envData) > 0)
		{
			xStatus = xQueueSendToBack( xDHTQueue, &envData, 0);
			if(xStatus != pdPASS)
			{
				printf("Could not send to the DHT11 queue.\r\n");
			}
		}
		vTaskDelay(3000 / portTICK_RATE_MS);
	}
}

static void DS18B20_GetData_Task(void* pvParameters)
{
	portBASE_TYPE xStatus;
	float temp;
	ds18b20_init(5);
	printf("Starting DS18B20 measurement!\n");

	while(1)
	{
		if((temp = ds18b20_get_temp()) < 200)
		{
			xStatus = xQueueSendToBack( x18B20Queue, &temp, 0);
			if(xStatus != pdPASS)
			{
				printf("Could not send to the 18B20 queue.\r\n");
			}
		}
		vTaskDelay(3000 / portTICK_RATE_MS);
	}
}

static void sc_callback(smartconfig_status_t status, void *pdata)
{
	static int  i = 0;
    switch (status) {
        case SC_STATUS_WAIT:
            ESP_LOGI(SBH, "SC_STATUS_WAIT");
            break;
        case SC_STATUS_FIND_CHANNEL:
            ESP_LOGI(SBH, "SC_STATUS_FINDING_CHANNEL");
            break;
        case SC_STATUS_GETTING_SSID_PSWD:
        	scSta = 1;
            ESP_LOGI(SBH, "SC_STATUS_GETTING_SSID_PSWD");
            break;
        case SC_STATUS_LINK:
            ESP_LOGI(SBH, "SC_STATUS_LINK");
            wifi_config_t *wifi_config = pdata;
            ESP_LOGI(SBH, "SSID:%s", wifi_config->sta.ssid);
            ESP_LOGI(SBH, "PASSWORD:%s", wifi_config->sta.password);
            ESP_ERROR_CHECK( esp_wifi_disconnect() );
            ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_config) );
            ESP_ERROR_CHECK( esp_wifi_connect() );
            break;
        case SC_STATUS_LINK_OVER:
            ESP_LOGI(SBH, "SC_STATUS_LINK_OVER");
            if (pdata != NULL) {
                uint8_t phone_ip[4] = { 0 };
                memcpy(phone_ip, (uint8_t* )pdata, 4);
                ESP_LOGI(SBH, "Phone ip: %d.%d.%d.%d\n", phone_ip[0], phone_ip[1], phone_ip[2], phone_ip[3]);
            }
            xEventGroupSetBits(wifi_event_group, ESPTOUCH_DONE_BIT);
            break;
        default:
            break;
    }
}

void smartconfig_example_task(void * parm)
{
    EventBits_t uxBits;
    gpio_set_level(GPIO_NUM_2, 1);
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS) );
    ESP_ERROR_CHECK( esp_smartconfig_start(sc_callback) );

    while (1) {
        uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if(uxBits & CONNECTED_BIT) {
        	scSta = 0;
            ESP_LOGI(SBH, "WiFi Connected to ap");
        }
        if(uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(SBH, "smartconfig over");
            esp_smartconfig_stop();
            flagSmartConfig = 0;
            wifi_config_t cfg;
            if(esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg) == ESP_OK){
            	if(save_AP_Info(&cfg) == ESP_OK)ESP_LOGI(SBH, "ap info saved!");
            }
            vTaskDelete(NULL);
        }
    }
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
	static int cnt = 0;
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
    	if(flagSmartConfig){
    		xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 6, &vSCTask_Handle);
    	}else{
    		esp_wifi_connect();
    	}
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
    	ESP_LOGI(SBH, "mqtt got IP!");
		wifiSta = 1;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    	if(flagSmartConfig == 1)
    	{
    		if(cnt < 3)
    		{
    			cnt++;
    			esp_wifi_connect();
    		}else if(cnt == 3){
    			cnt = 0;
				esp_smartconfig_stop();
				ESP_LOGI(SBH, "smartconfig password error, stop it!");
				vTaskDelete(vSCTask_Handle);
				scSta = 0;
				gpio_set_level(GPIO_NUM_2, 0);
				vTaskDelay(2000 / portTICK_PERIOD_MS);
				xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 6, &vSCTask_Handle);
				ESP_LOGI(SBH,"restart it!");
    		}
    	}else{
	   /* This is a workaround as ESP32 WiFi libs don't currently
		   auto-reassociate. */
        	wifiSta = 0;
        	gpio_set_level(GPIO_NUM_2, 0);
    		esp_wifi_connect();
    	}
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi_smartConfig(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void initialise_wifi_normal(wifi_config_t *wifi_cfg)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s", wifi_cfg->sta.ssid);
    ESP_LOGI(TAG, "Setting WiFi configuration PASSWORD %s", wifi_cfg->sta.password);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_cfg) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

void app_main()
{
    // Initialize NVS.
	esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
	gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
	if (gpio_get_level(GPIO_NUM_0) == 0) {
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		if(gpio_get_level(GPIO_NUM_0) == 0) {
			initialise_wifi_smartConfig();  //smartconfig
			flagSmartConfig = 1;
		}
	}

	if(!flagSmartConfig){
	    if(get_AP_Info(&nvs_wifi) == ESP_OK){
			initialise_wifi_normal(&nvs_wifi);		//normalconfig
	    }else{
	    	initialise_wifi_smartConfig();  //smartconfig
	    	flagSmartConfig = 1;
	    }
	}

	xTaskCreate(&toggle_led_task, "tled_task", 2048, NULL, 5, NULL);
	xDHTQueue = xQueueCreate(3, sizeof(envT));
	x18B20Queue = xQueueCreate(3, sizeof(float));
	if(xDHTQueue != NULL){
		xTaskCreate(&DHT11_GetData_Task, "dht_task", 2048, NULL, 4, NULL);
	}
	if(x18B20Queue != NULL){
		xTaskCreate(&DS18B20_GetData_Task, "ds18b20_task", 2048, NULL, 4, NULL);
	}
	xTaskCreate(&mqtt_task, "mqtt_task", 8192, NULL, 5, NULL);
}
