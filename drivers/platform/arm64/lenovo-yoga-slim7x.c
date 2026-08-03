// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Maya Matuszczyk <maya.matuszczyk@gmail.com>
 */
#include <linux/auxiliary_bus.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/irqreturn.h>
#include <linux/leds.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/input.h>
//#include <linux/platform_data/lenovo-yoga-slim7x.h>

// These are the registers that i know about available from SMBUS
#define EC_IRQ_REASON_REG 0x05
#define EC_SUSPEND_RESUME_REG 0x23
#define EC_IRQ_ENABLE_REG 0x35
#define EC_BACKLIGHT_STATUS_REG 0x83
#define EC_MIC_MUTE_LED_REG 0x84
#define EC_AC_STATUS_REG 0x90

// Valid values for EC_SUSPEND_RESUME_REG
#define EC_NOTIFY_SUSPEND_ENTER 0x01
#define EC_NOTIFY_SUSPEND_EXIT 0x00
#define EC_NOTIFY_SCREEN_OFF 0x03
#define EC_NOTIFY_SCREEN_ON 0x04

// These are the values in EC_IRQ_REASON_REG that i could find in DSDT
#define EC_IRQ_MICMUTE_BUTTON 0x04
#define EC_IRQ_FAN1_STATUS_CHANGE 0x30
#define EC_IRQ_FAN2_STATUS_CHANGE 0x31
#define EC_IRQ_FAN1_SPEED_CHANGE 0x32
#define EC_IRQ_FAN2_SPEED_CHANGE 0x33
#define EC_IRQ_COMPLETED_LUT_UPDATE 0x34
#define EC_IRQ_COMPLETED_FAN_PROFILE_SWITCH 0x35
#define EC_IRQ_THERMISTOR_1_TEMP_THRESHOLD_CROSS 0x36
#define EC_IRQ_THERMISTOR_2_TEMP_THRESHOLD_CROSS 0x37
#define EC_IRQ_THERMISTOR_3_TEMP_THRESHOLD_CROSS 0x38
#define EC_IRQ_THERMISTOR_4_TEMP_THRESHOLD_CROSS 0x39
#define EC_IRQ_THERMISTOR_5_TEMP_THRESHOLD_CROSS 0x3a
#define EC_IRQ_THERMISTOR_6_TEMP_THRESHOLD_CROSS 0x3b
#define EC_IRQ_THERMISTOR_7_TEMP_THRESHOLD_CROSS 0x3c
#define EC_IRQ_RECOVERED_FROM_RESET 0x3d
#define EC_IRQ_LENOVO_SUPPORT_KEY 0x90
#define EC_IRQ_FN_Q 0x91
#define EC_IRQ_FN_M 0x92
#define EC_IRQ_FN_SPACE 0x93
#define EC_IRQ_FN_R 0x94
#define EC_IRQ_FNLOCK_ON 0x95
#define EC_IRQ_FNLOCK_OFF 0x96
#define EC_IRQ_FN_N 0x97
#define EC_IRQ_AI 0x9a
#define EC_IRQ_NPU 0x9b

struct yoga_slim7x_ec {
	struct i2c_client *client;
	struct input_dev *idev;
	struct led_classdev kbd_backlight;
	struct led_classdev led_mic_mute;
	struct mutex lock;
	/* Last non-zero value read back from EC_BACKLIGHT_STATUS_REG. */
	u8 saved_kbd_backlight;
};

static irqreturn_t yoga_slim7x_ec_irq(int irq, void *data)
{
	struct yoga_slim7x_ec *ec = data;
	struct device *dev = &ec->client->dev;
	int val;

	guard(mutex)(&ec->lock);

	val = i2c_smbus_read_byte_data(ec->client, EC_IRQ_REASON_REG);
	if (val < 0) {
		dev_err(dev, "Failed to get EC IRQ reason: %d\n", val);
		return IRQ_HANDLED;
	}

	switch (val) {
	case EC_IRQ_MICMUTE_BUTTON:
		input_report_key(ec->idev, KEY_MICMUTE, 1);
		input_sync(ec->idev);
		input_report_key(ec->idev, KEY_MICMUTE, 0);
		input_sync(ec->idev);

		/*
		 * This platform has no ALSA capture-mute control for
		 * SND_CTL_LED to follow (mute is handled in userspace audio
		 * routing), and unlike the backlight the EC does not toggle
		 * this LED itself. Toggle it here in lockstep with the
		 * button that userspace's own mute handling responds to.
		 */
		led_set_brightness(&ec->led_mic_mute,
				   ec->led_mic_mute.brightness ? LED_OFF : LED_ON);
		break;
	case EC_IRQ_FN_SPACE:
		/* Fn+Space: Keyboard backlight toggle */
		input_report_key(ec->idev, KEY_KBDILLUMTOGGLE, 1);
		input_sync(ec->idev);
		input_report_key(ec->idev, KEY_KBDILLUMTOGGLE, 0);
		input_sync(ec->idev);

		/*
		 * The EC toggles the backlight itself; read the resulting
		 * state back so the LED classdev (and userspace watching it)
		 * stays in sync with the hardware-driven change.
		 */
		val = i2c_smbus_read_byte_data(ec->client, EC_BACKLIGHT_STATUS_REG);
		if (val >= 0) {
			if (val)
				ec->saved_kbd_backlight = val;
			led_classdev_notify_brightness_hw_changed(&ec->kbd_backlight,
								  val ? LED_ON : LED_OFF);
		}
		break;
	default:
		dev_dbg(dev, "Unhandled EC IRQ reason: 0x%02x\n", val);
	}

	return IRQ_HANDLED;
}

static enum led_brightness yoga_slim7x_kbd_bl_get(struct led_classdev *led_cdev)
{
	struct yoga_slim7x_ec *ec = container_of(led_cdev, struct yoga_slim7x_ec,
						 kbd_backlight);
	int val;

	guard(mutex)(&ec->lock);

	val = i2c_smbus_read_byte_data(ec->client, EC_BACKLIGHT_STATUS_REG);
	if (val < 0)
		return LED_OFF;

	if (val)
		ec->saved_kbd_backlight = val;

	return val ? LED_ON : LED_OFF;
}

static int yoga_slim7x_kbd_bl_set(struct led_classdev *led_cdev,
				  enum led_brightness brightness)
{
	struct yoga_slim7x_ec *ec = container_of(led_cdev, struct yoga_slim7x_ec,
						 kbd_backlight);
	u8 val = brightness ? (ec->saved_kbd_backlight ?: 0x01) : 0x00;

	guard(mutex)(&ec->lock);

	return i2c_smbus_write_byte_data(ec->client, EC_BACKLIGHT_STATUS_REG, val);
}

static int yoga_slim7x_kbd_backlight_probe(struct yoga_slim7x_ec *ec)
{
	struct device *dev = &ec->client->dev;

	ec->kbd_backlight.name = "platform::kbd_backlight";
	ec->kbd_backlight.flags = LED_BRIGHT_HW_CHANGED;
	ec->kbd_backlight.max_brightness = 1;
	ec->kbd_backlight.brightness_set_blocking = yoga_slim7x_kbd_bl_set;
	ec->kbd_backlight.brightness_get = yoga_slim7x_kbd_bl_get;

	return devm_led_classdev_register(dev, &ec->kbd_backlight);
}

static enum led_brightness yoga_slim7x_mic_mute_led_get(struct led_classdev *led_cdev)
{
	struct yoga_slim7x_ec *ec = container_of(led_cdev, struct yoga_slim7x_ec,
						 led_mic_mute);
	int val;

	guard(mutex)(&ec->lock);

	val = i2c_smbus_read_byte_data(ec->client, EC_MIC_MUTE_LED_REG);
	if (val < 0)
		return LED_OFF;

	return val ? LED_ON : LED_OFF;
}

static int yoga_slim7x_mic_mute_led_set(struct led_classdev *led_cdev,
					enum led_brightness brightness)
{
	struct yoga_slim7x_ec *ec = container_of(led_cdev, struct yoga_slim7x_ec,
						 led_mic_mute);

	guard(mutex)(&ec->lock);

	return i2c_smbus_write_byte_data(ec->client, EC_MIC_MUTE_LED_REG,
					 brightness ? 0x01 : 0x00);
}

static int yoga_slim7x_mic_mute_led_probe(struct yoga_slim7x_ec *ec)
{
	struct device *dev = &ec->client->dev;

	/*
	 * No default_trigger: this platform exposes no ALSA capture-mute
	 * control for SND_CTL_LED to follow (verified against amixer -c0
	 * controls; mute is handled in userspace audio routing). The
	 * EC_IRQ_MICMUTE_BUTTON case above toggles this LED directly
	 * instead, in lockstep with the Fn+F4 button press.
	 */
	ec->led_mic_mute.name = "platform::micmute";
	ec->led_mic_mute.max_brightness = 1;
	ec->led_mic_mute.brightness_set_blocking = yoga_slim7x_mic_mute_led_set;
	ec->led_mic_mute.brightness_get = yoga_slim7x_mic_mute_led_get;

	return devm_led_classdev_register(dev, &ec->led_mic_mute);
}

static int yoga_slim7x_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct yoga_slim7x_ec *ec;
	int ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	mutex_init(&ec->lock);
	ec->client = client;

	ec->idev = devm_input_allocate_device(dev);
	if (!ec->idev)
		return -ENOMEM;
	ec->idev->name = "yoga-slim7x-ec";
	ec->idev->phys = "yoga-slim7x-ec/input0";
	input_set_capability(ec->idev, EV_KEY, KEY_MICMUTE);
	input_set_capability(ec->idev, EV_KEY, KEY_KBDILLUMTOGGLE);

	ret = input_register_device(ec->idev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register input device\n");

	ret = yoga_slim7x_kbd_backlight_probe(ec);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register keyboard backlight LED\n");

	ret = yoga_slim7x_mic_mute_led_probe(ec);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register mic mute LED\n");

	ret = devm_request_threaded_irq(dev, client->irq,
					NULL, yoga_slim7x_ec_irq,
					IRQF_ONESHOT, "yoga_slim7x_ec", ec);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Unable to request irq\n");

	i2c_set_clientdata(client, ec);

	ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x01);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to enable interrupts\n");

	return 0;
}

static void yoga_slim7x_ec_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int ret;

	ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x00);
	if (ret < 0)
		dev_err(dev, "Failed to disable interrupts: %d\n", ret);
}

static int yoga_slim7x_ec_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct yoga_slim7x_ec *ec = i2c_get_clientdata(client);
	int ret;

	/* Turn off keyboard backlight to save power; brightness_set_blocking
	 * caches the on-value in ec->saved_kbd_backlight for resume.
	 */
	led_classdev_suspend(&ec->kbd_backlight);

	ret = i2c_smbus_write_byte_data(client, EC_SUSPEND_RESUME_REG, EC_NOTIFY_SCREEN_OFF);
	if (ret)
		return ret;

	ret = i2c_smbus_write_byte_data(client, EC_SUSPEND_RESUME_REG, EC_NOTIFY_SUSPEND_ENTER);
	if (ret)
		return ret;

	return 0;
}

static int yoga_slim7x_ec_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct yoga_slim7x_ec *ec = i2c_get_clientdata(client);
	int ret;

	ret = i2c_smbus_write_byte_data(client, EC_SUSPEND_RESUME_REG, EC_NOTIFY_SUSPEND_EXIT);
	if (ret)
		return ret;

	ret = i2c_smbus_write_byte_data(client, EC_SUSPEND_RESUME_REG, EC_NOTIFY_SCREEN_ON);
	if (ret)
		return ret;

	/* Restore keyboard backlight state */
	led_classdev_resume(&ec->kbd_backlight);

	return 0;
}

static const struct of_device_id yoga_slim7x_ec_of_match[] = {
	{ .compatible = "lenovo,yoga-slim7x-ec" },
	{}
};
MODULE_DEVICE_TABLE(of, yoga_slim7x_ec_of_match);

static const struct i2c_device_id yoga_slim7x_ec_i2c_id_table[] = {
	{ "yoga-slim7x-ec", },
	{}
};
MODULE_DEVICE_TABLE(i2c, yoga_slim7x_ec_i2c_id_table);

static DEFINE_SIMPLE_DEV_PM_OPS(yoga_slim7x_ec_pm_ops,
		yoga_slim7x_ec_suspend,
		yoga_slim7x_ec_resume);

static struct i2c_driver yoga_slim7x_ec_i2c_driver = {
	.driver = {
		.name = "yoga-slim7x-ec",
		.of_match_table = yoga_slim7x_ec_of_match,
		.pm = &yoga_slim7x_ec_pm_ops
	},
	.probe = yoga_slim7x_ec_probe,
	.remove = yoga_slim7x_ec_remove,
	.id_table = yoga_slim7x_ec_i2c_id_table,
};
module_i2c_driver(yoga_slim7x_ec_i2c_driver);

MODULE_DESCRIPTION("Lenovo Yoga Slim 7x Embedded Controller");
MODULE_LICENSE("GPL");
