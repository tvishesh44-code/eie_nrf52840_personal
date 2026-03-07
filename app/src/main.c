#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys_clock.h>

#include "lvgl.h"

#define SLEEP_MS              1

#define X_A_PIN               7
#define X_B_PIN               8
#define Y_A_PIN               13
#define Y_B_PIN               14

#define X_SIGN                1
#define Y_SIGN                1

#define X_DEBOUNCE_US         3000
#define Y_DEBOUNCE_US         3000
#define CLEAR_HOLD_MS         3000

#define SCREEN_W              320
#define SCREEN_H              240

#define DRAW_X                10
#define DRAW_Y                40
#define DRAW_W                200
#define DRAW_H                180

#define CURSOR_W              8
#define CURSOR_H              8
#define LINE_WIDTH            3

/*
 * Stroke-based storage:
 * One LVGL line object per stroke, many points per stroke.
 * Much more memory-efficient than one object per segment.
 */
#define MAX_STROKES           20
#define MAX_POINTS_PER_STROKE 160

static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *gpio0_dev   = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static const struct gpio_dt_spec button2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static const struct gpio_dt_spec button3 = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);

static lv_obj_t *label;
static lv_obj_t *draw_box;
static lv_obj_t *cursor;
static lv_obj_t *help_label;

typedef struct {
    lv_obj_t *line_obj;
    lv_point_precise_t points[MAX_POINTS_PER_STROKE];
    uint16_t point_count;
    uint32_t color_hex;
    bool in_use;
} stroke_t;

static stroke_t strokes[MAX_STROKES];
static int current_stroke = -1;
static uint16_t stroke_count = 0;

static volatile int x_pos = DRAW_W / 2;
static volatile int y_pos = DRAW_H / 2;

static volatile int x_dir = 0;
static volatile int y_dir = 0;

static volatile uint32_t x_irq_last_us = 0;
static volatile uint32_t y_irq_last_us = 0;

static volatile bool ui_dirty = true;
static volatile bool pos_changed = false;

static struct gpio_callback x_a_cb_data;
static struct gpio_callback y_a_cb_data;

struct button_state {
    bool pressed_now;
    bool pressed_prev;
    bool hold_fired;
    int64_t press_time_ms;
};

static struct button_state b0 = {0};
static struct button_state b1 = {0};
static struct button_state b2 = {0};
static struct button_state b3 = {0};

static int last_draw_x = DRAW_W / 2;
static int last_draw_y = DRAW_H / 2;

static bool pen_down = true;
static bool erase_mode = false;
static uint8_t color_index = 0;

static const uint32_t palette_hex[] = {
    0x000000, /* Black  */
    0xFF0000, /* Red    */
    0x00AA00, /* Green  */
    0x0000FF, /* Blue   */
    0xFFAA00, /* Orange */
    0x8000FF, /* Purple */
    0x00AAAA  /* Teal   */
};

static const char *palette_names[] = {
    "Black",
    "Red",
    "Green",
    "Blue",
    "Orange",
    "Purple",
    "Teal"
};

#define PALETTE_SIZE (sizeof(palette_hex) / sizeof(palette_hex[0]))

static int clamp_int(int value, int low, int high)
{
    if (value < low) {
        return low;
    }

    if (value > high) {
        return high;
    }

    return value;
}

static uint32_t now_us(void)
{
    return k_cyc_to_us_floor32(k_cycle_get_32());
}

static bool button_is_pressed(const struct gpio_dt_spec *button)
{
    int value = gpio_pin_get_dt(button);

    if (value < 0) {
        return false;
    }

    if ((button->dt_flags & GPIO_ACTIVE_LOW) != 0U) {
        return (value == 0);
    }

    return (value != 0);
}

static uint32_t current_draw_color_hex(void)
{
    if (erase_mode) {
        return 0xFFFFFF;
    }

    return palette_hex[color_index];
}

static lv_color_t current_draw_color(void)
{
    return lv_color_hex(current_draw_color_hex());
}

static void update_label(void)
{
    char text[180];

    snprintf(text, sizeof(text),
             "X: %d dir:%d\n"
             "Y: %d dir:%d\n"
             "Pen: %s\n"
             "Erase: %s\n"
             "Color: %s\n"
             "Strokes: %u",
             x_pos, x_dir,
             y_pos, y_dir,
             pen_down ? "DOWN" : "UP",
             erase_mode ? "ON" : "OFF",
             erase_mode ? "White" : palette_names[color_index],
             stroke_count);

    lv_label_set_text(label, text);
}

static void update_cursor(void)
{
    lv_obj_set_pos(cursor,
                   x_pos - (CURSOR_W / 2),
                   y_pos - (CURSOR_H / 2));

    lv_obj_set_style_bg_color(cursor, current_draw_color(), 0);
}

static void clear_strokes(void)
{
    for (int i = 0; i < MAX_STROKES; i++) {
        if (strokes[i].line_obj != NULL) {
            lv_obj_delete(strokes[i].line_obj);
            strokes[i].line_obj = NULL;
        }

        strokes[i].point_count = 0;
        strokes[i].color_hex = 0;
        strokes[i].in_use = false;
    }

    current_stroke = -1;
    stroke_count = 0;
    last_draw_x = x_pos;
    last_draw_y = y_pos;
}

static bool start_new_stroke(int start_x, int start_y)
{
    if (stroke_count >= MAX_STROKES) {
        return false;
    }

    int idx = -1;

    for (int i = 0; i < MAX_STROKES; i++) {
        if (!strokes[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        return false;
    }

    strokes[idx].line_obj = lv_line_create(draw_box);
    strokes[idx].point_count = 1;
    strokes[idx].color_hex = current_draw_color_hex();
    strokes[idx].in_use = true;

    strokes[idx].points[0].x = start_x;
    strokes[idx].points[0].y = start_y;

    lv_line_set_points(strokes[idx].line_obj, strokes[idx].points, strokes[idx].point_count);
    lv_obj_set_style_line_width(strokes[idx].line_obj, LINE_WIDTH, 0);
    lv_obj_set_style_line_color(strokes[idx].line_obj, lv_color_hex(strokes[idx].color_hex), 0);
    lv_obj_set_style_line_opa(strokes[idx].line_obj, LV_OPA_COVER, 0);

    current_stroke = idx;
    stroke_count++;

    lv_obj_move_foreground(cursor);
    return true;
}

static bool append_point_to_current_stroke(int x, int y)
{
    if (current_stroke < 0) {
        return false;
    }

    stroke_t *s = &strokes[current_stroke];

    if (!s->in_use || s->line_obj == NULL) {
        return false;
    }

    if (s->point_count >= MAX_POINTS_PER_STROKE) {
        return false;
    }

    if (s->point_count > 0) {
        uint16_t last = s->point_count - 1U;

        if (s->points[last].x == x && s->points[last].y == y) {
            return true;
        }
    }

    s->points[s->point_count].x = x;
    s->points[s->point_count].y = y;
    s->point_count++;

    lv_line_set_points(s->line_obj, s->points, s->point_count);
    lv_obj_move_foreground(cursor);

    return true;
}

static void ensure_stroke_active(int start_x, int start_y)
{
    bool need_new = false;

    if (current_stroke < 0) {
        need_new = true;
    } else {
        stroke_t *s = &strokes[current_stroke];

        if (!s->in_use || s->line_obj == NULL) {
            need_new = true;
        } else if (s->color_hex != current_draw_color_hex()) {
            need_new = true;
        } else if (s->point_count >= MAX_POINTS_PER_STROKE) {
            need_new = true;
        }
    }

    if (need_new) {
        start_new_stroke(start_x, start_y);
    }
}

/* ---- EXACT known-good encoder logic kept ---- */

static void x_a_falling_isr(const struct device *port,
                            struct gpio_callback *cb,
                            gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    uint32_t t = now_us();

    if ((t - x_irq_last_us) < X_DEBOUNCE_US) {
        return;
    }
    x_irq_last_us = t;

    int b = gpio_pin_get(gpio0_dev, X_B_PIN);

    if (b < 0) {
        return;
    }

    if (b) {
        x_dir = 1 * X_SIGN;
    } else {
        x_dir = -1 * X_SIGN;
    }

    x_pos = clamp_int(x_pos + x_dir, 0, DRAW_W - 1);
    ui_dirty = true;
    pos_changed = true;
}

static void y_a_falling_isr(const struct device *port,
                            struct gpio_callback *cb,
                            gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    uint32_t t = now_us();

    if ((t - y_irq_last_us) < Y_DEBOUNCE_US) {
        return;
    }
    y_irq_last_us = t;

    int b = gpio_pin_get(gpio0_dev, Y_B_PIN);

    if (b < 0) {
        return;
    }

    if (b) {
        y_dir = 1 * Y_SIGN;
    } else {
        y_dir = -1 * Y_SIGN;
    }

    y_pos = clamp_int(y_pos + y_dir, 0, DRAW_H - 1);
    ui_dirty = true;
    pos_changed = true;
}

static int setup_buttons(void)
{
    if (!gpio_is_ready_dt(&button0) ||
        !gpio_is_ready_dt(&button1) ||
        !gpio_is_ready_dt(&button2) ||
        !gpio_is_ready_dt(&button3)) {
        return -1;
    }

    if (gpio_pin_configure_dt(&button0, GPIO_INPUT) < 0) {
        return -1;
    }

    if (gpio_pin_configure_dt(&button1, GPIO_INPUT) < 0) {
        return -1;
    }

    if (gpio_pin_configure_dt(&button2, GPIO_INPUT) < 0) {
        return -1;
    }

    if (gpio_pin_configure_dt(&button3, GPIO_INPUT) < 0) {
        return -1;
    }

    return 0;
}

static int setup_encoders(void)
{
    if (!device_is_ready(gpio0_dev)) {
        return -1;
    }

    if (gpio_pin_configure(gpio0_dev, X_A_PIN, GPIO_INPUT | GPIO_PULL_UP) < 0) {
        return -1;
    }

    if (gpio_pin_configure(gpio0_dev, X_B_PIN, GPIO_INPUT | GPIO_PULL_UP) < 0) {
        return -1;
    }

    if (gpio_pin_configure(gpio0_dev, Y_A_PIN, GPIO_INPUT | GPIO_PULL_UP) < 0) {
        return -1;
    }

    if (gpio_pin_configure(gpio0_dev, Y_B_PIN, GPIO_INPUT | GPIO_PULL_UP) < 0) {
        return -1;
    }

    if (gpio_pin_interrupt_configure(gpio0_dev, X_A_PIN, GPIO_INT_EDGE_FALLING) < 0) {
        return -1;
    }

    if (gpio_pin_interrupt_configure(gpio0_dev, Y_A_PIN, GPIO_INT_EDGE_FALLING) < 0) {
        return -1;
    }

    gpio_init_callback(&x_a_cb_data, x_a_falling_isr, BIT(X_A_PIN));
    gpio_add_callback(gpio0_dev, &x_a_cb_data);

    gpio_init_callback(&y_a_cb_data, y_a_falling_isr, BIT(Y_A_PIN));
    gpio_add_callback(gpio0_dev, &y_a_cb_data);

    return 0;
}

static void handle_button_press(uint8_t index)
{
    switch (index) {
    case 0:
        pen_down = !pen_down;
        if (!pen_down) {
            current_stroke = -1;
        } else {
            last_draw_x = x_pos;
            last_draw_y = y_pos;
        }
        ui_dirty = true;
        break;

    case 1:
        erase_mode = !erase_mode;
        current_stroke = -1;
        ui_dirty = true;
        break;

    case 2:
        color_index++;
        if (color_index >= PALETTE_SIZE) {
            color_index = 0;
        }
        current_stroke = -1;
        ui_dirty = true;
        break;

    case 3:
        if (color_index == 0U) {
            color_index = PALETTE_SIZE - 1U;
        } else {
            color_index--;
        }
        current_stroke = -1;
        ui_dirty = true;
        break;

    default:
        break;
    }
}

static void handle_button_hold(struct button_state *btn, uint8_t index)
{
    if (index == 1U) {
        clear_strokes();
        btn->hold_fired = true;
        ui_dirty = true;
    }
}

static void update_one_button(struct button_state *btn,
                              const struct gpio_dt_spec *spec,
                              uint8_t index)
{
    int64_t now = k_uptime_get();

    btn->pressed_now = button_is_pressed(spec);

    if (btn->pressed_now && !btn->pressed_prev) {
        btn->press_time_ms = now;
        btn->hold_fired = false;
        handle_button_press(index);
    }

    if (btn->pressed_now && !btn->hold_fired) {
        if ((now - btn->press_time_ms) >= CLEAR_HOLD_MS) {
            handle_button_hold(btn, index);
        }
    }

    if (!btn->pressed_now && btn->pressed_prev) {
        btn->hold_fired = false;
    }

    btn->pressed_prev = btn->pressed_now;
}

int main(void)
{
    if (!device_is_ready(display_dev)) {
        return 0;
    }

    if (setup_buttons() < 0) {
        return 0;
    }

    if (setup_encoders() < 0) {
        return 0;
    }

    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        return 0;
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    label = lv_label_create(screen);
    lv_obj_set_pos(label, 220, 20);

    draw_box = lv_obj_create(screen);
    lv_obj_set_pos(draw_box, DRAW_X, DRAW_Y);
    lv_obj_set_size(draw_box, DRAW_W, DRAW_H);
    lv_obj_set_style_bg_color(draw_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(draw_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(draw_box, 4, 0);
    lv_obj_set_style_border_color(draw_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(draw_box, 0, 0);
    lv_obj_set_style_pad_all(draw_box, 0, 0);
    lv_obj_clear_flag(draw_box, LV_OBJ_FLAG_SCROLLABLE);

    cursor = lv_obj_create(draw_box);
    lv_obj_set_size(cursor, CURSOR_W, CURSOR_H);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cursor, 2, 0);
    lv_obj_set_style_border_color(cursor, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_radius(cursor, 0, 0);

    help_label = lv_label_create(screen);
    lv_label_set_text(help_label,
                      "SW0 Pen\n"
                      "SW1 Erase\n"
                      "Hold SW1 Clear\n"
                      "SW2 Next\n"
                      "SW3 Prev");
    lv_obj_set_pos(help_label, 220, 140);

    update_cursor();
    update_label();

    display_blanking_off(display_dev);

    while (1) {
        if (pos_changed) {
            if (pen_down) {
                ensure_stroke_active(last_draw_x, last_draw_y);
                append_point_to_current_stroke(x_pos, y_pos);
            }

            last_draw_x = x_pos;
            last_draw_y = y_pos;
            pos_changed = false;
        }

        update_one_button(&b0, &button0, 0);
        update_one_button(&b1, &button1, 1);
        update_one_button(&b2, &button2, 2);
        update_one_button(&b3, &button3, 3);

        if (ui_dirty) {
            update_cursor();
            update_label();
            ui_dirty = false;
        }

        lv_timer_handler();
        k_msleep(SLEEP_MS);
    }

    return 0;
}