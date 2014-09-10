/**
 * File: gpio.c
 * Date: 12/08/2014
 * Author: Zhiyi Huang
 * Version: 0.1
 *
 * This is a gpio API for the dummy gpio device which
 * generates an interrupt for each half-byte (the most significant
 * bits are generated first.
 *
 * COSC440 assignment 2 in 2014.
 */
/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.2 / 5
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
#include <asm/switch_to.h>
#else
#include <asm/system.h>
#endif
#include <mach/platform.h>
#include <mach/gpio.h>

struct bcm2708_gpio {
    struct list_head list;
    void __iomem *base;
    struct gpio_chip gc;
    unsigned long rising;
    unsigned long falling;
};
struct gpio_chip *gpiochip;
struct bcm2708_gpio *rpi_gpio;
/* Define GPIO pins for the dummy device */
static struct gpio gpio_dummy[] = {
    { 7, GPIOF_IN, "GPIO7"},
    { 8, GPIOF_OUT_INIT_HIGH, "GPIO8"},
    { 17, GPIOF_IN, "GPIO17"},
    { 18, GPIOF_OUT_INIT_HIGH, "GPIO18"},
    { 22, GPIOF_IN, "GPIO22"},
    { 23, GPIOF_OUT_INIT_HIGH, "GPIO23"},
    { 24, GPIOF_IN, "GPIO24"},
    { 25, GPIOF_OUT_INIT_HIGH, "GPIO25"},
    { 4, GPIOF_OUT_INIT_LOW, "GPIO4"},
    { 27, GPIOF_IN, "GPIO27"},
};
static int dummy_irq;
irqreturn_t dummyport_interrupt(int irq, void *dev_id);
u8 read_half_byte(void);
int gpio_dummy_init(void);
void gpio_dummy_exit(void);

static int is_right_chip(struct gpio_chip *chip, void *data) {
    if (strcmp(data, chip->label) == 0)
        return 1;
    return 0;
}

static inline u32
gpio_inw(u32 addr) {
    u32 data;
    asm volatile("ldr %0,[%1]" : "=r"(data) : "r"(addr));
    return data;
}

static inline void
gpio_outw(u32 addr, u32 data) {
    asm volatile("str %1,[%0]" : : "r"(addr), "r"(data));
}

void setgpiofunc(u32 func, u32 alt) {
    u32 sel, data, shift;
    if (func > 53) return;
    sel = 0;
    while (func > 10) {
        func = func - 10;
        sel++;
    }
    sel = (sel << 2) + (u32) rpi_gpio->base;
    data = gpio_inw(sel);
    shift = func + (func << 1);
    data &= ~(7 << shift);
    data |= alt << shift;
    gpio_outw(sel, data);
}

u8 read_half_byte() {
    u32 c;
    u8 r;
    r = 0;
    c = gpio_inw((u32) rpi_gpio->base + 0x34);
    if (c & (1 << 7)) r |= 1;
    if (c & (1 << 17)) r |= 2;
    if (c & (1 << 22)) r |= 4;
    if (c & (1 << 24)) r |= 8;
    return r;
}

int gpio_dummy_init() {
    int ret;
    gpiochip = gpiochip_find("bcm2708_gpio", is_right_chip);
    rpi_gpio = container_of(gpiochip, struct bcm2708_gpio, gc);
    printk(KERN_ERR "GPIO_BASE is %x\n", (u32) rpi_gpio->base);
    ret = gpio_request_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
    if (ret) {
        printk(KERN_ERR "Unable to request GPIOs for the dummy device: %d\n", ret);
        return ret;
    }
    ret = gpio_to_irq(gpio_dummy[ARRAY_SIZE(gpio_dummy) - 1].gpio);
    if (ret < 0) {
        printk(KERN_ERR "Unable to request IRQ for gpio %d: %d\n", gpio_dummy[ARRAY_SIZE(
                gpio_dummy) - 1].gpio, ret);
        goto fail1;
    }
    dummy_irq = ret;
  
    printk(KERN_INFO "Successfully requested IRQ# %d for %s\n", dummy_irq, gpio_dummy[
            ARRAY_SIZE(gpio_dummy) - 1].label);
    ret = request_irq(dummy_irq, dummyport_interrupt, IRQF_TRIGGER_RISING | IRQF_DISABLED, 
            "gpio27", NULL);
    if (ret) {
        printk(KERN_ERR "Unable to request IRQ for dummy device: %d\n", ret);
        goto fail1;
    }
fail1:
    gpio_free_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
    return ret;
}

void gpio_dummy_exit() {
    free_irq(dummy_irq, NULL);
    gpio_free_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
}
