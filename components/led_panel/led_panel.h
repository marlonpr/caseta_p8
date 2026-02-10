#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// virtual grid: N = panels across; M = panels down
#define VIRTUAL_NUM_PANEL_HORIZONTAL 2
#define VIRTUAL_NUM_PANEL_VERTICAL   1

/* -------------------- PANEL MAP TYPE -------------------- */
typedef struct {
    int panel_w, panel_h, N, M;
    uint32_t recip_pw, recip_ph, SHIFT;
    int total_src_w, total_src_h;
} PanelMapFast;

/* -------------------- GLOBAL PANEL MAP INSTANCE -------------------- */
extern PanelMapFast pm;

/* -------------------- PUBLIC API -------------------- */

// brightness / registers
void configure_min(void);
void configure_max(void);
void init_max_brightness(void);
void set_gains_and_brightness(float Rg, float Gg, float Bg, uint8_t brightness);

// drawing / frame control
void prepare_frame_back(void);
void present_frame_back(void);
void set_pixel_rgb_back(int sx, int sy, uint8_t r, uint8_t g, uint8_t b);
void draw_char_back(int sx, int sy, char c, uint8_t r, uint8_t g, uint8_t b);
void draw_text_back(int sx, int sy, const char *str,
                    uint8_t r, uint8_t g, uint8_t b);
void draw_text_back_gap(int sx, int sy, const char *str,
                        uint8_t r, uint8_t g, uint8_t b);
                        
                        
                        
                        
void draw_text_back_2(int sx, int sy, const char *str,
                    uint8_t r, uint8_t g, uint8_t b);                        
                        
                        
                        
                        
                        
void clear_region_back(int sx, int sy, int w, int h);

// scrolling
void long_scroll_start(const char *txt, int y, int speed_px_sec);
void long_scroll_update(void);

// tasks
void refresh_task(void *arg);
void drawing_task(void *arg);

// initialization helpers
void init_pins(void);

void map_src_to_flat_row_init(PanelMapFast *pm_in,
                              int panel_w, int panel_h,
                              int N, int M);
