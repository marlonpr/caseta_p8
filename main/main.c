#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>
#include "led_panel.h"
#include "esp_timer.h"

//✅ Unified Master/Slave Code
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lora.h"

#include "ds18b20.h"
#include "ds3231.h"



//=========================== buttons config ==============================
static void init_buttons(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_34) |
                        (1ULL << GPIO_NUM_35) |
                        (1ULL << GPIO_NUM_39),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // not supported
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // not supported
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
}




//=========================================================================















// ------------------ Config ------------------
//#define LED_GPIO        GPIO_NUM_2  //=================== this line is for TX ===========
#define DEVICE_ID       1       // 0 = Master, 1..N-1 = Slave
#define NUM_DEVICES     3
#define FREQ_HZ         433000000
#define TX_INTERVAL_MS  1000
#define LOST_TIMEOUT_MS 10000
#define BLINK_PERIOD_MS 1000

static const char *TAG = "LORA_NODE";

// ------------------ IRQ flags (SX1278) ------------------
#define IRQ_TX_DONE       0x08
#define IRQ_RX_DONE       0x40
#define IRQ_VALID_HEADER  0x10
#define IRQ_CRC_ERROR     0x20
#define IRQ_RX_TIMEOUT    0x80
#define IRQ_ALL           0xFF

// ------------------ Globals ------------------
/* ---- Link alive parameters ---- */
#define LORA_RX_TIMEOUT_MS 3000   // TX considered OFF after 3s silence

/* ---- Globals shared with other tasks ---- */
volatile bool temp_received = false;
volatile int16_t temp_global = 0;

/* ---- Local state ---- */
static int64_t last_rx_time_us = 0;

typedef enum {
    PAGE_TIME_DATE = 0,
    PAGE_TEMPERATURE
} display_page_t;


//=================================== DS18B20 SECTION ==========================================//
static ds18b20_t sensor;
static int16_t current_temp = 0;
static bool temp_valid = false;
//============================================================================================




void lora_task(void *arg)
{
    uint8_t buf[16];

    /* Enable RX once at startup */
    lora_enable_rx();
    ESP_LOGI(TAG, "RX enabled");

    while (1)
    {
        uint8_t irq = lora_read_reg(REG_IRQ_FLAGS);

        /* -------- Packet received -------- */
        if (irq & IRQ_RX_DONE)
        {
            /* CRC error → ignore packet, but still clear IRQ */
            if (irq & IRQ_CRC_ERROR)
            {
                ESP_LOGW(TAG, "CRC error");
            }
            else
            {
                int len = lora_receive_packet(buf, sizeof(buf));

                ESP_LOGI(TAG,
                    "RX RAW len=%d b0=0x%02X b1=0x%02X",
                    len, buf[0], buf[1]);

                /* Validate protocol frame */
                if (len == 8 && buf[0] == 0xAA)
                {
                    uint8_t rx_id = buf[1];
                    int16_t temp = (buf[2] << 8) | buf[3];

                    uint32_t cnt =
                        (buf[4] << 24) |
                        (buf[5] << 16) |
                        (buf[6] << 8)  |
                        buf[7];

                    int8_t snr_raw = (int8_t)lora_read_reg(REG_PKT_SNR_VALUE);
                    float snr = snr_raw / 4.0f;
                    int rssi = lora_read_reg(REG_PKT_RSSI_VALUE) - 157;

                    temp_global = temp;
                    temp_received = true;
                    last_rx_time_us = esp_timer_get_time();

                    ESP_LOGI(TAG,
                        "RX OK id=%d temp=%d cnt=%lu RSSI=%d SNR=%.2f",
                        rx_id, temp, cnt, rssi, snr);
                }
            }

            /* ---- Clear IRQs and re-arm RX ---- */
            lora_write_reg(REG_IRQ_FLAGS, 0xFF);
            lora_write_reg(REG_FIFO_ADDR_PTR, 0);
            lora_enable_rx();
        }

        /* -------- Link timeout check -------- */
        if (temp_received)
        {
            int64_t now = esp_timer_get_time();

            if ((now - last_rx_time_us) >
                (LORA_RX_TIMEOUT_MS * 1000))
            {
                temp_received = false;
                ESP_LOGW(TAG, "LoRa TX timeout (no packets)");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* -------------------- DRAWING TASK (example counter) -------------------- */
void drawing_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(50));   // allow refresh_task to start
    ds3231_dev_t *rtc = (ds3231_dev_t *)arg; 
    
    char buf[50];   // buffer size used for snprintf    
    char buf2[50];   // buffer size used for snprintf
    //long_scroll_start("HELLO|WORLD! ", 0, 10);
    
    display_page_t page = PAGE_TIME_DATE;
    int64_t last_switch_time = esp_timer_get_time(); // µs

    while (1) {
        // Clear + rebuild back framebuffer
        prepare_frame_back();
                 
        snprintf(buf, sizeof(buf), "%d*C", temp_global); 
        snprintf(buf2, sizeof(buf2), "%d*C", current_temp);   //current_temp        

        int64_t now = esp_timer_get_time();

        // Switch page every 5 seconds
        if ((now - last_switch_time) >= 5000000) {
            page = (page == PAGE_TIME_DATE) ? PAGE_TEMPERATURE : PAGE_TIME_DATE;
            last_switch_time = now;
        }

        if (page == PAGE_TIME_DATE) 
        {	

	        //============================ TIME and DATE ======================================  
	
	        ds3231_time_t time;
	        ESP_ERROR_CHECK(ds3231_get_time(rtc, &time));    
	
			// ==== TIME ====
			int hour12 = time.hour;
			char buf_time[16];
			
			char colon = ((time.second & 1) == 0) ? ':' : ' ';  // blink colon
			
			snprintf(
			    buf_time,
			    sizeof(buf_time),
			    (hour12 < 10) ? " %1d%c%02d" : "%02d%c%02d",
			    hour12,
			    colon,
			    time.minute
			);
			
			draw_text_back(1, 10, buf_time, 255, 255, 255);    
			
	        //====DATE======            
	        char buf4[32];
	        snprintf(buf4, sizeof(buf4), "%02d-%02d-%02d",
	                 time.day,time.month,
	                 time.year-2000);
	                 
	        draw_text_back_2(1, 1, buf4, 0, 255, 0);   // green           
    
			// =========================================================================


        } else 
        {

            // Draw ENVIRONMENT TEMPERATURE
            draw_text_back_2(1, 1, "AMBIENTE", 255, 255, 255); // white
            draw_text_back(5, 10, buf2, 255, 0, 0);          // red
        }
         
        draw_text_back_2(44, 1, "ALBERCA", 255, 255, 255);  // white
        
        if(temp_received)
        {
	        draw_text_back(45, 10, buf, 255, 0, 0);  // white			
		}else
		{
			draw_text_back(45, 10, "T:--", 255, 0, 0);  // white
		}
        // Swap buffers safely
        present_frame_back();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void temp_task(void *arg)
{
    while (1) {
        int16_t t;
        if (ds18b20_read_temperature_int(&sensor, &t) == ESP_OK) 
        {
            current_temp = t;
            if (current_temp < -9)
            {
				current_temp = -9;
			}
            temp_valid = true;
        } else {
            temp_valid = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // read every 5s
    }
}


void button_task(void *arg)
{
    int last34 = 1, last35 = 1, last36 = 1;

    while (1) {
        int b34 = gpio_get_level(GPIO_NUM_34);
        int b35 = gpio_get_level(GPIO_NUM_35);
        int b36 = gpio_get_level(GPIO_NUM_39);

        if (b34 != last34) {
            ESP_LOGI("BTN", "GPIO34 %s", b34 ? "RELEASED" : "PRESSED");
            last34 = b34;
        }

        if (b35 != last35) {
            ESP_LOGI("BTN", "GPIO35 %s", b35 ? "RELEASED" : "PRESSED");
            last35 = b35;
        }

        if (b36 != last36) {
            ESP_LOGI("BTN", "GPIO36 %s", b36 ? "RELEASED" : "PRESSED");
            last36 = b36;
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // basic debounce
    }
}


void app_main(void)
{
    init_pins();
    map_src_to_flat_row_init(&pm,40, 20,   VIRTUAL_NUM_PANEL_HORIZONTAL,   VIRTUAL_NUM_PANEL_VERTICAL);
    
    assert(pm.total_src_w == 40 * pm.N);
    assert(pm.total_src_h == 20 * pm.M);

    set_gains_and_brightness(1.0f, 1.0f, 1.0f, 255);

    init_max_brightness(); 
    
    init_buttons();

 

    ds3231_dev_t rtc;
    ESP_ERROR_CHECK(init_ds3231(&rtc));


	ds3231_time_t now;
	ds3231_get_time(&rtc, &now);

	if(now.year < 2026)
	{
		ds3231_time_t set_time = {2026, 2, 10, 14, 22, 0, 3};
    	ESP_ERROR_CHECK(ds3231_set_time(&rtc, &set_time));
	}	
	
	
    xTaskCreatePinnedToCore(refresh_task,    "refresh_task",4096,NULL,1,NULL,1);
    xTaskCreatePinnedToCore(drawing_task,"drawing_task",8192,&rtc,1,NULL,0);
    

	vTaskDelay(pdMS_TO_TICKS(1000));
	
    ds18b20_init(&sensor, GPIO_NUM_3); // Use GPIO4 with 4.7kΩ pull-up resistor    
    xTaskCreatePinnedToCore(temp_task,      "TempTask",      1024, NULL, 2, NULL, 0);

	vTaskDelay(pdMS_TO_TICKS(1000));

    // LoRa init
    if (lora_init() != ESP_OK) {
        ESP_LOGE(TAG, "LoRa init failed");
        return;
    }
    lora_set_frequency(FREQ_HZ);
    ESP_LOGI(TAG, "Device %d started on %.1f MHz", DEVICE_ID, FREQ_HZ / 1e6);
   	xTaskCreatePinnedToCore(lora_task,      "LoraTask",      4096, NULL, 1, NULL, 0);
   	
   		vTaskDelay(pdMS_TO_TICKS(2000));

   	
   	
   	xTaskCreatePinnedToCore(button_task, "button_task", 2048, NULL, 3, NULL, 0);



    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}



/*
			// Draw TIME AND DATE: 
			draw_text_back(1, 10, "10:25", 255, 255, 255); // white 
			draw_text_back_2(1, 1, "10-02-26", 0, 255, 0); // green 
*/