// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for onboard USB hubs
 *
 * Copyright (c) 2022, Google LLC
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/of.h>
#include <linux/usb/onboard_hub.h>

static struct usb_device_driver onboard_hub_usbdev_driver;

/************************** Platform driver **************************/

struct usbdev_node {
	struct usb_device *udev;
	struct list_head list;
};

struct onboard_hub {
	struct regulator *vdd;
	struct device *dev;
	bool always_powered_in_suspend;
	bool is_powered_on;
	bool going_away;
	struct list_head udev_list;
	struct mutex lock;
};

static int onboard_hub_power_on(struct onboard_hub *hub)
{
	int err;

	err = regulator_enable(hub->vdd);
	if (err) {
		dev_err(hub->dev, "failed to enable regulator: %d\n", err);
		return err;
	}

	hub->is_powered_on = true;

	return 0;
}

static int onboard_hub_power_off(struct onboard_hub *hub)
{
	int err;

	err = regulator_disable(hub->vdd);
	if (err) {
		dev_err(hub->dev, "failed to disable regulator: %d\n", err);
		return err;
	}

	hub->is_powered_on = false;

	return 0;
}

static int __maybe_unused onboard_hub_suspend(struct device *dev)
{
	struct onboard_hub *hub = dev_get_drvdata(dev);
	struct usbdev_node *node;
	bool power_off = true;

	if (hub->always_powered_in_suspend)
		return 0;

	mutex_lock(&hub->lock);

	list_for_each_entry(node, &hub->udev_list, list) {
		if (!device_may_wakeup(node->udev->bus->controller))
			continue;

		if (usb_wakeup_enabled_descendants(node->udev)) {
			power_off = false;
			break;
		}
	}

	mutex_unlock(&hub->lock);

	if (!power_off)
		return 0;

	return onboard_hub_power_off(hub);
}

static int __maybe_unused onboard_hub_resume(struct device *dev)
{
	struct onboard_hub *hub = dev_get_drvdata(dev);

	if (hub->is_powered_on)
		return 0;

	return onboard_hub_power_on(hub);
}

static inline void get_udev_link_name(const struct usb_device *udev, char *buf, size_t size)
{
	snprintf(buf, size, "usb_dev.%s", dev_name(&udev->dev));
}

static int onboard_hub_add_usbdev(struct onboard_hub *hub, struct usb_device *udev)
{
	struct usbdev_node *node;
	char link_name[64];
	int err;

	mutex_lock(&hub->lock);

	if (hub->going_away) {
		err = -EINVAL;
		goto error;
	}

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		err = -ENOMEM;
		goto error;
	}

	node->udev = udev;

	list_add(&node->list, &hub->udev_list);

	mutex_unlock(&hub->lock);

	get_udev_link_name(udev, link_name, sizeof(link_name));
	WARN_ON(sysfs_create_link(&hub->dev->kobj, &udev->dev.kobj, link_name));

	return 0;

error:
	mutex_unlock(&hub->lock);

	return err;
}

static void onboard_hub_remove_usbdev(struct onboard_hub *hub, const struct usb_device *udev)
{
	struct usbdev_node *node;
	char link_name[64];

	get_udev_link_name(udev, link_name, sizeof(link_name));
	sysfs_remove_link(&hub->dev->kobj, link_name);

	mutex_lock(&hub->lock);

	list_for_each_entry(node, &hub->udev_list, list) {
		if (node->udev == udev) {
			list_del(&node->list);
			kfree(node);
			break;
		}
	}

	mutex_unlock(&hub->lock);
}

static ssize_t always_powered_in_suspend_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	const struct onboard_hub *hub = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", hub->always_powered_in_suspend);
}

static ssize_t always_powered_in_suspend_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct onboard_hub *hub = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0)
		return ret;

	hub->always_powered_in_suspend = val;

	return count;
}
static DEVICE_ATTR_RW(always_powered_in_suspend);

static struct attribute *onboard_hub_attrs[] = {
	&dev_attr_always_powered_in_suspend.attr,
	NULL,
};
ATTRIBUTE_GROUPS(onboard_hub);

static int onboard_hub_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct onboard_hub *hub;
	int err;

	hub = devm_kzalloc(dev, sizeof(*hub), GFP_KERNEL);
	if (!hub)
		return -ENOMEM;

	hub->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(hub->vdd))
		return PTR_ERR(hub->vdd);

	hub->dev = dev;
	mutex_init(&hub->lock);
	INIT_LIST_HEAD(&hub->udev_list);

	dev_set_drvdata(dev, hub);

	err = onboard_hub_power_on(hub);
	if (err)
		return err;

	/*
	 * The USB driver might have been detached from the USB devices by
	 * onboard_hub_remove(), make sure to re-attach it if needed.
	 */
	err = driver_attach(&onboard_hub_usbdev_driver.drvwrap.driver);
	if (err) {
		onboard_hub_power_off(hub);
		return err;
	}

	return 0;
}

static int onboard_hub_remove(struct platform_device *pdev)
{
	struct onboard_hub *hub = dev_get_drvdata(&pdev->dev);
	struct usbdev_node *node;
	struct usb_device *udev;

	hub->going_away = true;

	mutex_lock(&hub->lock);

	/* unbind the USB devices to avoid dangling references to this device */
	while (!list_empty(&hub->udev_list)) {
		node = list_first_entry(&hub->udev_list, struct usbdev_node, list);
		udev = node->udev;

		/*
		 * Unbinding the driver will call onboard_hub_remove_usbdev(),
		 * which acquires hub->lock.  We must release the lock first.
		 */
		get_device(&udev->dev);
		mutex_unlock(&hub->lock);
		device_release_driver(&udev->dev);
		put_device(&udev->dev);
		mutex_lock(&hub->lock);
	}

	mutex_unlock(&hub->lock);

	return onboard_hub_power_off(hub);
}

static const struct of_device_id onboard_hub_match[] = {
	{ .compatible = "usbbda,411" },
	{ .compatible = "usbbda,5411" },
	{ .compatible = "usbbda,414" },
	{ .compatible = "usbbda,5414" },
	{}
};
MODULE_DEVICE_TABLE(of, onboard_hub_match);

static bool of_is_onboard_usb_hub(const struct device_node *np)
{
	return !!of_match_node(onboard_hub_match, np);
}

static const struct dev_pm_ops __maybe_unused onboard_hub_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(onboard_hub_suspend, onboard_hub_resume)
};

static struct platform_driver onboard_hub_driver = {
	.probe = onboard_hub_probe,
	.remove = onboard_hub_remove,

	.driver = {
		.name = "onboard-usb-hub",
		.of_match_table = onboard_hub_match,
		.pm = pm_ptr(&onboard_hub_pm_ops),
		.dev_groups = onboard_hub_groups,
	},
};

/************************** USB driver **************************/

#define VENDOR_ID_REALTEK	0x0bda

/*
 * Returns the onboard_hub platform device that is associated with the USB
 * device passed as parameter.
 */
static struct onboard_hub *_find_onboard_hub(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct onboard_hub *hub;

	pdev = of_find_device_by_node(dev->of_node);
	if (!pdev) {
		np = of_parse_phandle(dev->of_node, "companion-hub", 0);
		if (!np) {
			dev_err(dev, "failed to find device node for companion hub\n");
			return ERR_PTR(-EINVAL);
		}

		pdev = of_find_device_by_node(np);
		of_node_put(np);

		if (!pdev)
			return ERR_PTR(-ENODEV);
	}

	hub = dev_get_drvdata(&pdev->dev);
	put_device(&pdev->dev);

	/*
	 * The presence of drvdata ('hub') indicates that the platform driver
	 * finished probing. This handles the case where (conceivably) we could
	 * be running at the exact same time as the platform driver's probe. If
	 * we detect the race we request probe deferral and we'll come back and
	 * try again.
	 */
	if (!hub)
		return ERR_PTR(-EPROBE_DEFER);

	return hub;
}

static int onboard_hub_usbdev_probe(struct usb_device *udev)
{
	struct device *dev = &udev->dev;
	struct onboard_hub *hub;
	int err;

	/* ignore supported hubs without device tree node */
	if (!dev->of_node)
		return -ENODEV;

	hub = _find_onboard_hub(dev);
	if (IS_ERR(hub))
		return PTR_ERR(hub);

	dev_set_drvdata(dev, hub);

	err = onboard_hub_add_usbdev(hub, udev);
	if (err)
		return err;

	return 0;
}

static void onboard_hub_usbdev_disconnect(struct usb_device *udev)
{
	struct onboard_hub *hub = dev_get_drvdata(&udev->dev);

	onboard_hub_remove_usbdev(hub, udev);
}

static const struct usb_device_id onboard_hub_id_table[] = {
	{ USB_DEVICE(VENDOR_ID_REALTEK, 0x0411) }, /* RTS5411 USB 3.1 */
	{ USB_DEVICE(VENDOR_ID_REALTEK, 0x5411) }, /* RTS5411 USB 2.1 */
	{ USB_DEVICE(VENDOR_ID_REALTEK, 0x0414) }, /* RTS5414 USB 3.2 */
	{ USB_DEVICE(VENDOR_ID_REALTEK, 0x5414) }, /* RTS5414 USB 2.1 */
	{}
};
MODULE_DEVICE_TABLE(usb, onboard_hub_id_table);

static struct usb_device_driver onboard_hub_usbdev_driver = {
	.name = "onboard-usb-hub",
	.probe = onboard_hub_usbdev_probe,
	.disconnect = onboard_hub_usbdev_disconnect,
	.generic_subclass = 1,
	.supports_autosuspend =	1,
	.id_table = onboard_hub_id_table,
};

/*** Helpers for creating/destroying platform devices for onboard hubs ***/

struct pdev_list_entry {
	struct platform_device *pdev;
	struct list_head node;
};

/**
 * onboard_hub_create_pdevs -- create platform devices for onboard USB hubs
 * @parent_hub	: parent hub to scan for connected onboard hubs
 * @pdev_list	: list of onboard hub platform devices owned by the parent hub
 *
 * Creates a platform device for each supported onboard hub that is connected to
 * the given parent hub. To keep track of the platform devices they are added to
 * a list that is owned by the parent hub.
 */
void onboard_hub_create_pdevs(struct usb_device *parent_hub, struct list_head *pdev_list)
{
	int i;
	struct device_node *np, *npc;
	struct platform_device *pdev = NULL;
	struct pdev_list_entry *pdle;

	INIT_LIST_HEAD(pdev_list);

	for (i = 1; i <= parent_hub->maxchild; i++) {
		np = usb_of_get_device_node(parent_hub, i);
		if (!np)
			continue;

		if (!of_is_onboard_usb_hub(np))
			goto node_put;

		npc = of_parse_phandle(np, "companion-hub", 0);
		if (npc) {
			pdev = of_find_device_by_node(npc);
			of_node_put(npc);
		}

		if (pdev) {
			/* the companion hub already has a platform device, nothing to do here */
			put_device(&pdev->dev);
			goto node_put;
		}

		pdev = of_platform_device_create(np, NULL, &parent_hub->dev);
		if (!pdev) {
			dev_err(&parent_hub->dev,
				"failed to create platform device for onboard hub '%pOF'\n", np);
			goto node_put;
		}

		pdle = devm_kzalloc(&pdev->dev, sizeof(*pdle), GFP_KERNEL);
		if (!pdle) {
			of_platform_device_destroy(&pdev->dev, NULL);
			goto node_put;
		}

		pdle->pdev = pdev;
		list_add(&pdle->node, pdev_list);

node_put:
		of_node_put(np);
	}
}
EXPORT_SYMBOL_GPL(onboard_hub_create_pdevs);

/**
 * onboard_hub_destroy_pdevs -- free resources of onboard hub platform devices
 * @pdev_list	: list of onboard hub platform devices
 *
 * Destroys the platform devices in the given list and frees the memory associated
 * with the list entry.
 */
void onboard_hub_destroy_pdevs(struct list_head *pdev_list)
{
	struct pdev_list_entry *pdle, *tmp;

	list_for_each_entry_safe(pdle, tmp, pdev_list, node) {
		list_del(&pdle->node);
		of_platform_device_destroy(&pdle->pdev->dev, NULL);
	}
}
EXPORT_SYMBOL_GPL(onboard_hub_destroy_pdevs);

/************************** Driver (de)registration **************************/

static int __init onboard_hub_init(void)
{
	int ret;

	ret = platform_driver_register(&onboard_hub_driver);
	if (ret)
		return ret;

	ret = usb_register_device_driver(&onboard_hub_usbdev_driver, THIS_MODULE);
	if (ret)
		platform_driver_unregister(&onboard_hub_driver);

	return ret;
}
module_init(onboard_hub_init);

static void __exit onboard_hub_exit(void)
{
	usb_deregister_device_driver(&onboard_hub_usbdev_driver);
	platform_driver_unregister(&onboard_hub_driver);
}
module_exit(onboard_hub_exit);

MODULE_AUTHOR("Matthias Kaehlcke <mka@chromium.org>");
MODULE_DESCRIPTION("Driver for discrete onboard USB hubs");
MODULE_LICENSE("GPL v2");
