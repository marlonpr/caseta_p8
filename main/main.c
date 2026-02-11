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





//==================================== BUTTONS SECTION ==========================================//

#define MENU_TIMEOUT_US   (10 * 1000000)  // 10 seconds

//buttons
#define PIN_MENU    GPIO_NUM_34
#define PIN_UP      GPIO_NUM_36
#define PIN_DOWN    GPIO_NUM_39

#define DEBOUNCE_MS    500      // minimum time between presses
#define REPEAT_DELAY   500     // initial delay before repeating
#define REPEAT_RATE    500    // repeat interval while holding


static int menu_active = 0;
static int64_t last_button_time = 0;

static TickType_t menu_hold_start = 0;

bool block_next_menu_edge = false;





bool mode_flag = false;
bool mode_entering = false;
bool format_entering = false;

typedef enum {
    BTN_MENU = 0,
    BTN_UP   = 1,
    BTN_DOWN= 2
} button_t;

static QueueHandle_t button_queue;


static void IRAM_ATTR button_isr_handler(void* arg)
{
    button_t btn = (button_t)(uint32_t)arg;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(button_queue, &btn, &hp);
    if (hp) portYIELD_FROM_ISR();
}



static void init_buttons(void)
{
    /* ---------- UP + DOWN (polling capable) ---------- */
    gpio_config_t io_int = {
        .pin_bit_mask = (1ULL << PIN_UP) |
                        (1ULL << PIN_DOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // OK if pins support it
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE        // 🔑 IMPORTANT
    };
    ESP_ERROR_CHECK(gpio_config(&io_int));

    /* ---------- MENU (interrupt only) ---------- */
    gpio_config_t io_up = {
        .pin_bit_mask = (1ULL << PIN_MENU),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,    // ignored anyway on GPIO35
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&io_up));

    /* ---------- Queue & ISR ---------- */
    button_queue = xQueueCreate(10, sizeof(button_t));
    assert(button_queue);

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    gpio_isr_handler_add(PIN_MENU, button_isr_handler, (void*)BTN_MENU);
    
}



typedef enum {
    MENU_IDLE,
    MENU_HOUR,
    MENU_MINUTE,
    MENU_DAY,
    MENU_MONTH,
    MENU_YEAR
} menu_state_t;

static menu_state_t menu_state = MENU_IDLE;
static ds3231_time_t tmp_time;   // temporary time editing



static void handle_menu_button(button_t btn, ds3231_dev_t *rtc)
{
    
    last_button_time = esp_timer_get_time();
    
    switch (menu_state)
    {
        case MENU_IDLE:
            if (btn == BTN_MENU) {
				ESP_ERROR_CHECK(ds3231_get_time(rtc, &tmp_time));
				
				//temporal_brightness = brightness_level;
				
				menu_state = MENU_HOUR;
			    menu_active = 1;
			    last_button_time = esp_timer_get_time();  // start inactivity timer
			    printf("Menu entered\n");
			    printf("Menu HOUR\n");
			}			
			
            break;

        case MENU_HOUR:
            if (btn == BTN_UP) tmp_time.hour = (tmp_time.hour + 1) % 24;
            if (btn == BTN_DOWN) tmp_time.hour = (tmp_time.hour + 23) % 24;
            if (btn == BTN_MENU)
            {
			    printf("Menu MINUTE\n");
			    menu_state = MENU_MINUTE;

			} 
            break;

        case MENU_MINUTE:
            if (btn == BTN_UP) tmp_time.minute = (tmp_time.minute + 1) % 60;
            if (btn == BTN_DOWN) tmp_time.minute = (tmp_time.minute + 59) % 60;
            if (btn == BTN_MENU)
            {
				printf("Menu DAY\n");
				menu_state = MENU_DAY;
			} 
            break;      
		    
		case MENU_DAY:
		    if (btn == BTN_UP) tmp_time.day = (tmp_time.day % 31) + 1;
		    if (btn == BTN_DOWN) tmp_time.day = ((tmp_time.day + 29) % 31) + 1;
		    if (btn == BTN_MENU)
		    {
				printf("Menu MONTH\n");
				menu_state = MENU_MONTH;
			} 
		    break;
		
		case MENU_MONTH:
		    if (btn == BTN_UP) tmp_time.month = (tmp_time.month % 12) + 1;
		    if (btn == BTN_DOWN) tmp_time.month = ((tmp_time.month + 10) % 12) + 1;
		    if (btn == BTN_MENU)
		    {
				menu_state = MENU_YEAR;
				printf("Menu YEAR\n");
			} 
		    break;
		
		case MENU_YEAR:
			if (btn == BTN_UP)
			    tmp_time.year = 2000 + ((tmp_time.year - 2000 + 1) % 100);
			if (btn == BTN_DOWN)
			    tmp_time.year = 2000 + ((tmp_time.year - 2000 + 99) % 100);
		    if (btn == BTN_MENU) 
		    {
		        tmp_time.second = 0;
				ESP_ERROR_CHECK(ds3231_set_time(rtc, &tmp_time));	
				
						
                menu_active = 0;
                printf("Menu end -> exiting\n");				
		        menu_state = MENU_IDLE;
		    }
		    break;
    }
}

#define BTN_PRESSED(pin)   (gpio_get_level(pin) == 0)

static void menu_task(void *arg)
{
    ds3231_dev_t *rtc = (ds3231_dev_t *)arg;

    TickType_t menu_hold_start = 0;
    TickType_t up_start = 0, down_start = 0;
    TickType_t up_repeat = 0, down_repeat = 0;

    bool menu_irq_enabled = true;

    while (1)
    {
        TickType_t now = xTaskGetTickCount();
        button_t btn;

        /* ========= MENU EDGE (INTERRUPT) ========= */
        if (xQueueReceive(button_queue, &btn, 0))
        {
            if (btn == BTN_MENU && menu_irq_enabled)
            {
                /* Disable MENU interrupt immediately */
                gpio_intr_disable(PIN_MENU);
                menu_irq_enabled = false;

                /* Flush any pending MENU events */
                while (xQueueReceive(button_queue, &btn, 0)) {}

                if (!menu_active) {
                    menu_hold_start = now;   // start hold timer
                } else {
                    handle_menu_button(BTN_MENU, rtc); // single step
                }
            }
        }

        /* ========= HOLD TO ENTER MENU ========= */
        if (!menu_active &&
            menu_hold_start &&
            BTN_PRESSED(PIN_MENU) &&
            (now - menu_hold_start) >= pdMS_TO_TICKS(1000))
        {
            handle_menu_button(BTN_MENU, rtc);
            menu_hold_start = 0;
        }

        /* ========= MENU RELEASE ========= */
        if (!BTN_PRESSED(PIN_MENU))
        {
            menu_hold_start = 0;

            if (!menu_irq_enabled) {
                gpio_intr_enable(PIN_MENU);
                menu_irq_enabled = true;
            }
        }

        /* ========= UP / DOWN (POLLING) ========= */
        if (menu_active)
        {
            /* -------- UP -------- */
            if (BTN_PRESSED(PIN_UP))
            {
                if (!up_start) {
                    up_start = now;
                    handle_menu_button(BTN_UP, rtc);
                    up_repeat = now + pdMS_TO_TICKS(REPEAT_DELAY);
                } else if (now >= up_repeat) {
                    handle_menu_button(BTN_UP, rtc);
                    up_repeat = now + pdMS_TO_TICKS(REPEAT_RATE);
                }
            } else {
                up_start = 0;
            }

            /* -------- DOWN -------- */
            if (BTN_PRESSED(PIN_DOWN))
            {
                if (!down_start) {
                    down_start = now;
                    handle_menu_button(BTN_DOWN, rtc);
                    down_repeat = now + pdMS_TO_TICKS(REPEAT_DELAY);
                } else if (now >= down_repeat) {
                    handle_menu_button(BTN_DOWN, rtc);
                    down_repeat = now + pdMS_TO_TICKS(REPEAT_RATE);
                }
            } else {
                down_start = 0;
            }
        }
        
        /* ========= MENU TIMEOUT ========= */
		if (menu_active)
		{
		    int64_t now_us = esp_timer_get_time();
		
		    if ((now_us - last_button_time) > MENU_TIMEOUT_US && menu_state != MENU_YEAR)
		    {
		        menu_active = 0;
		        menu_state  = MENU_IDLE;
		        printf("Menu timeout -> exit\n");
		    }
		}


        vTaskDelay(pdMS_TO_TICKS(20));
    }
}





//=========================================================================================================================//




















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
        
        
        
        
        















        
        
        
        
        
        if (menu_state != MENU_IDLE)
        {
            //clear_back_buffer();
            prepare_frame_back();

            // Draw the menu
            char buf[32];
            switch (menu_state)
            {

                case MENU_HOUR:
                    snprintf(buf, sizeof(buf), "HORA: %02d", tmp_time.hour);
                    //draw_text(0, 8, buf, 255, 0, 0);
        			draw_text_back(2, 5, buf, 255, 0, 0);    

                    break;
                case MENU_MINUTE:
                    snprintf(buf, sizeof(buf), "%02d", tmp_time.minute);
                    //draw_text(1, 8, buf, 255, 0, 0);
                    draw_text_back_2(4, 6, "MINUTO: ", 255, 0, 0);
                    draw_text_back(50, 5, buf, 255, 0, 0);
                    break;
                case MENU_DAY:
                    snprintf(buf, sizeof(buf), "DIA: %02d", tmp_time.day);
                    //draw_text(0, 8, buf, 255, 0, 0);
                    draw_text_back(6, 5, buf, 255, 0, 0);
                    break;
                case MENU_MONTH:
                    snprintf(buf, sizeof(buf), "MES: %02d", tmp_time.month);
                    //draw_text(0, 8, buf, 255, 0, 0);
                    draw_text_back(6, 5, buf, 255, 0, 0);
                    break;
                case MENU_YEAR:
                    snprintf(buf, sizeof(buf), "A|O: %02d", tmp_time.year-2000);
                    //draw_text(0, 8, buf, 255, 0, 0);
                    draw_text_back(6, 5, buf, 255, 0, 0);
                    break;
                default: break;
            }

            //swap_buffers();
            present_frame_back();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue; // skip normal display
        }           
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
                 
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
        vTaskDelay(pdMS_TO_TICKS(5000)); // read every 5s;

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

   	
   	
   	//xTaskCreatePinnedToCore(button_task, "button_task", 2048, NULL, 3, NULL, 0);
   	
	xTaskCreatePinnedToCore(menu_task, "MenuTask", 4096, &rtc, 3, NULL, 0);




    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}



/*
			// Draw TIME AND DATE: 
			draw_text_back(1, 10, "10:25", 255, 255, 255); // white 
			draw_text_back_2(1, 1, "10-02-26", 0, 255, 0); // green 
*/








/*

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


//=========================================================================





*/


