/*
 * main.c
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(LED3_NODE, gpios);


int main(void) {
    int ret, ret1, ret2, ret3;

    if(!gpio_is_ready_dt(&led0)){
        return -1;
    }

    if(!gpio_is_ready_dt(&led1)){
        return -1;
    }

    if(!gpio_is_ready_dt(&led2)){
        return -1;
    }

    if(!gpio_is_ready_dt(&led3)){
        return -1;
    }

    ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret1 = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
    if (ret1 < 0) {
        return ret1;
    }

    ret2 = gpio_pin_configure_dt(&led2, GPIO_OUTPUT_ACTIVE);
    if (ret1 < 0) {
        return ret2;
    }

    ret3 = gpio_pin_configure_dt(&led3, GPIO_OUTPUT_ACTIVE);
    if (ret1 < 0) {
        return ret3;
    }

    int index = 0; 

    while (1) {
    /* Turn all LEDs off */
    gpio_pin_set_dt(&led0, 0);
    gpio_pin_set_dt(&led1, 0);
    gpio_pin_set_dt(&led2, 0);
    gpio_pin_set_dt(&led3, 0);

    /* Turn on exactly one LED */
    switch (index) {
    case 0:
        gpio_pin_set_dt(&led0, 1);
        break;
    case 1:
        gpio_pin_set_dt(&led1, 1);
        break;
    case 2:
        gpio_pin_set_dt(&led2, 1);
        break;
    case 3:
        gpio_pin_set_dt(&led3, 1);
        break;
    }

    index = (index + 1) % 4;
    k_msleep(500);
    }

    return 0;
}