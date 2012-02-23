/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>

#include <asm/uaccess.h>

#include <asm/gpio.h>
#include <plat/gpio-cfg.h>

struct gpio_button_data {
	struct gpio_keys_button *button;
	struct input_dev *input;
	struct timer_list timer;
};

struct gpio_keys_drvdata {
	struct input_dev *input;
	struct gpio_button_data data[0];
};

struct gpio_keys_drvdata *ddata;

#define HOME_KEYCODE		158
#define VOLUME_DOWN_KEYCODE	102
#define VOLUME_UP_KEYCODE	108
#define POWER_KEYCODE		139
#define COMBINE_KEY		HOME_KEYCODE

static int can_combine = 0;
static int has_combine = 0;
static int downkey_times = 0;
static int powerkey_times = 0;

static void add_fake_keys_capability(struct input_dev *input)
{
	input_set_capability(input, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(input, EV_KEY, KEY_SEARCH);
}

static void fake_fast_restart(void)
{
	struct task_struct *p;
	struct file *filp;

	write_lock(&tasklist_lock);	/* block fork */
	for_each_process(p) {
		if (p->mm && !is_global_init(p))
			/* Not swapper, init nor kernel thread */
			force_sig(SIGTERM, p);
	}
	write_unlock(&tasklist_lock);

	/* play a vibrator */
	filp = filp_open("/sys/class/timed_output/vibrator/enable", O_WRONLY, 0);
	if (!IS_ERR(filp) && filp) {
		vfs_write(filp, (char __user *)"1000", 4, &filp->f_pos); /* 1000ms */
		filp_close(filp, NULL);
	}
}

static void gpio_keys_report_event(struct gpio_button_data *bdata)
{
	struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned int type = button->type ?button->type: EV_KEY;
	int state = (gpio_get_value(button->gpio) ? 1 : 0) ^ button->active_low;
	int keycode = button->code;

	switch (keycode) {
	case COMBINE_KEY:
		if (state == 1) { // press down
			can_combine++;
			if (can_combine <= 2) // pending 2 times (about 1s), waitting for combine key
				return;
			else
				can_combine--; // waitting for combine key timeout, sending normal

			if (has_combine)
				return;
		} else { // state == 0, key up
			if (has_combine) { // combine end
				can_combine = 0;
				has_combine = 0;
				return;
			}

			while (can_combine > 0) { // send pending keys
				pr_debug("gpio_keys_report_event(send pending):type=%d;state:%d,raw_keycode:%d,keycode:%d\n",type,!state,button->code,keycode);
				input_event(input, type, keycode, !state);//state��ʵ��ʾ�ľ���press��1���£�0��ʾ̧��,���ϱ�1�����ϱ�0(����������ɣ���������¼�)��Linux�涨��, modified by hui
				input_sync(input);
				mdelay(10);
				can_combine--;
			}
		}
		break;
	case VOLUME_DOWN_KEYCODE:
	case VOLUME_UP_KEYCODE:
	case POWER_KEYCODE:
		if (state == 1) {
			if (keycode == VOLUME_DOWN_KEYCODE)
				downkey_times++;
			else if (keycode == POWER_KEYCODE)
				powerkey_times++;
			else
				downkey_times = powerkey_times = 0;

			if ((downkey_times > 10) && (powerkey_times > 10)) { /* about 5(+)s */
				printk("fast restart: force to kill all processes, trick keys times %d, %d\n", downkey_times, powerkey_times);
				fake_fast_restart();
				downkey_times = powerkey_times = 0;
			}
		} else {
			if (keycode == VOLUME_DOWN_KEYCODE)
				downkey_times--;
			else if (keycode == POWER_KEYCODE)
				powerkey_times--;
			else
				downkey_times = powerkey_times = 0;
		}

		if (can_combine > 0 || has_combine) {
			if (keycode == VOLUME_DOWN_KEYCODE)
				keycode = KEY_VOLUMEDOWN;
			else if (keycode == VOLUME_UP_KEYCODE)
				keycode = KEY_VOLUMEUP;
			else if (keycode == POWER_KEYCODE)
				keycode = KEY_SEARCH;	/* M + Power -> SEARCH */

			if (state == 1)
				has_combine = 1;
		} else 
			has_combine = 0;
		break;
	default:
		break;
	}

	pr_debug("gpio_keys_report_event:type=%d;state:%d,raw_keycode:%d,keycode:%d\n",type,state,button->code,keycode);
	input_event(input, type, keycode, !!state);//state��ʵ��ʾ�ľ���press��1���£�0��ʾ̧��,���ϱ�1�����ϱ�0(����������ɣ���������¼�)��Linux�涨��, modified by hui
	input_sync(input);
}

static void gpio_check_button(unsigned long _data)
{
	struct gpio_button_data *data = (struct gpio_button_data *)_data;
	struct gpio_keys_button *button=data->button;
	int down;

	pr_debug("gpio_check_button:gpio=%d;desc:%s\n",button->gpio,button->desc);

	down = gpio_get_value(button->gpio);

	/*
	*������,˵����ΰ�������Ч��,�ϱ������ֵ
	*/
	if(button->last_state ==down)
		gpio_keys_report_event(data);

	if(!down) 
		mod_timer(&data->timer,jiffies + msecs_to_jiffies(500));
}

static irqreturn_t gpio_keys_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
	struct gpio_keys_button *button = bdata->button;
	int down;

	if(!button)
		return IRQ_HANDLED;
	
	BUG_ON(irq != gpio_to_irq(button->gpio));

	down = gpio_get_value(button->gpio);
	
	pr_debug("\ngpio_keys_isr:gpio=%d;desc:%s\n",button->gpio,button->desc);
	
	/* the power button of the ipaq are tricky. They send 'released' events even
	 * when the button are already released. The work-around is to proceed only
	 * if the state changed.
	 **/

	if (button->last_state == down)
		return IRQ_HANDLED;

	pr_debug("handle key\n");
	
	button->last_state = down;

	if (button->debounce_interval)
		mod_timer(&bdata->timer,
			jiffies + msecs_to_jiffies(button->debounce_interval));
	else
		gpio_keys_report_event(bdata);
		
	return IRQ_HANDLED;
}

static int __devinit gpio_keys_probe(struct platform_device *pdev)
{
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	struct input_dev *input;
	int i, error;
	int wakeup = 0;

	ddata = kzalloc(sizeof(struct gpio_keys_drvdata) +
			pdata->nbuttons * sizeof(struct gpio_button_data),
			GFP_KERNEL);
	input = input_allocate_device();
	if (!ddata || !input) {
		error = -ENOMEM;
		goto fail1;
	}

	platform_set_drvdata(pdev, ddata);

	input->name = pdev->name;
	input->phys = "gpio-keys/input0";
	input->dev.parent = &pdev->dev;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	ddata->input = input;

	for (i = 0; i < pdata->nbuttons; i++) {
		struct gpio_keys_button *button = &pdata->buttons[i];
		struct gpio_button_data *bdata = &ddata->data[i];
		int irq;
		unsigned int type = button->type ?: EV_KEY;

		bdata->input = input;
		bdata->button = button;
		setup_timer(&bdata->timer,gpio_check_button, (unsigned long)bdata);

		error = gpio_request(button->gpio, button->desc ?: "gpio_keys");
		if (error < 0) {
			pr_err("gpio-keys: failed to request GPIO %d,"
				" error %d\n", button->gpio, error);
			goto fail2;
		}

		error = gpio_direction_input(button->gpio);
		if (error < 0) {
			pr_err("gpio-keys: failed to configure input"
				" direction for GPIO %d, error %d\n",
				button->gpio, error);
			gpio_free(button->gpio);
			goto fail2;
		}

		s3c_gpio_setpull(button->gpio, S3C_GPIO_PULL_NONE);
		button->last_state = gpio_get_value(button->gpio);
		
		irq = gpio_to_irq(button->gpio);
		if (irq < 0) {
			error = irq;
			pr_err("gpio-keys: Unable to get irq number"
				" for GPIO %d, error %d\n",
				button->gpio, error);
			gpio_free(button->gpio);
			goto fail2;
		}

		error = request_irq(irq, gpio_keys_isr,IRQF_SAMPLE_RANDOM | 
			IRQF_TRIGGER_RISING |IRQF_TRIGGER_FALLING,
			button->desc ? button->desc : "gpio_keys",bdata);
		if (error) {
			pr_err("gpio-keys: Unable to claim irq %d; error %d\n",
				irq, error);
			gpio_free(button->gpio);
			goto fail2;
		}
		
		if (button->wakeup)
		{
			wakeup = 1;
		}

		input_set_capability(input, type, button->code);
	}

	add_fake_keys_capability(input);

	error = input_register_device(input);
	if (error) {
		pr_err("gpio-keys: Unable to register input device, "
			"error: %d\n", error);
		goto fail2;
	}

	device_init_wakeup(&pdev->dev, wakeup);

	return 0;

 fail2:
	while (--i >= 0) {
		free_irq(gpio_to_irq(pdata->buttons[i].gpio), &ddata->data[i]);
		if (pdata->buttons[i].debounce_interval)
			del_timer_sync(&ddata->data[i].timer);
		gpio_free(pdata->buttons[i].gpio);
	}

	platform_set_drvdata(pdev, NULL);
 fail1:
	input_free_device(input);
	kfree(ddata);

	return error;
}

static int __devexit gpio_keys_remove(struct platform_device *pdev)
{
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
	struct input_dev *input = ddata->input;
	int i;

	device_init_wakeup(&pdev->dev, 0);

	for (i = 0; i < pdata->nbuttons; i++) {
		int irq = gpio_to_irq(pdata->buttons[i].gpio);
		free_irq(irq, &ddata->data[i]);
		if (pdata->buttons[i].debounce_interval)
			del_timer_sync(&ddata->data[i].timer);
		gpio_free(pdata->buttons[i].gpio);
	}

	input_unregister_device(input);

	return 0;
}


#ifdef CONFIG_PM
static int gpio_keys_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int gpio_keys_resume(struct platform_device *pdev)
{
#if 0
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	int i;

	if (device_may_wakeup(&pdev->dev)) {
		for (i = 0; i < pdata->nbuttons; i++) {
			struct gpio_keys_button *button = &pdata->buttons[i];
			if (button->wakeup) {
				int irq = gpio_to_irq(button->gpio);
				disable_irq_wake(irq);
			}
		}
	}
#endif
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	int i;

	for (i=0; i<pdata->nbuttons; i++) {
		struct gpio_keys_button *button = &pdata->buttons[i];
		int irq = gpio_to_irq(button->gpio);
		gpio_keys_isr(irq, &ddata->data[i]);
	}

	return 0;
}
#else
#define gpio_keys_suspend	NULL
#define gpio_keys_resume	NULL
#endif

static struct platform_driver gpio_keys_device_driver = {
	.probe		= gpio_keys_probe,
	.remove		= __devexit_p(gpio_keys_remove),
	.suspend		= gpio_keys_suspend,
	.resume		= gpio_keys_resume,
	.driver		= {
		.name	= "meizum8-buttons",
		.owner	= THIS_MODULE,
	}
};

static int __init gpio_keys_init(void)
{
	return platform_driver_register(&gpio_keys_device_driver);
}

static void __exit gpio_keys_exit(void)
{
	platform_driver_unregister(&gpio_keys_device_driver);
}

module_init(gpio_keys_init);
module_exit(gpio_keys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Blundell <pb@handhelds.org>");
MODULE_DESCRIPTION("Keyboard driver for CPU GPIOs");
MODULE_ALIAS("platform:gpio-keys");

