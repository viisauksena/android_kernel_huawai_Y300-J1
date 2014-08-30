/*
 * drivers/input/accelerometer/accelerometer_common.h
 */

#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/gpio.h>
#include <linux/hardware_self_adapt.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sensors.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <mach/vreg.h>

#ifdef CONFIG_HUAWEI_HW_DEV_DCT
#include <linux/hw_dev_dec.h>
#endif

static struct input_dev *sensor_dev;
