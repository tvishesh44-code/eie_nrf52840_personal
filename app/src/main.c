/*
 * main.c
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);


int main(void) {
    int ret, ret1;

    if(!gpio_is_ready_dt(&led0)){
        return -1;
    }

    if(!gpio_is_ready_dt(&led1)){
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

    while (1) {
        gpio_pin_toggle_dt(&led0);

        k_msleep(1000);

        gpio_pin_toggle_dt(&led1);

        k_msleep(1000);
    }

    return 0;
}