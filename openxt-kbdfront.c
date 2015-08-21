/*
 * OpenXT para-virtual input device
 *
 * Copyright (C) 2005 Anthony Liguori <aliguori@us.ibm.com>
 * Copyright (C) 2006-2008 Red Hat, Inc., Markus Armbruster <armbru@redhat.com>
 * Copyright (C) 2015 Assured Information Security, Inc. Kyle J. Temkin <temkink@ainfosec.com>
 *
 *  Based on linux/drivers/input/misc/xen-kbdfront.c
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/slab.h>

#include <asm/xen/hypervisor.h>

#include <xen/xen.h>
#include <xen/events.h>
#include <xen/page.h>
#include <xen/grant_table.h>
#include <xen/interface/grant_table.h>
#include <xen/interface/io/fbif.h>
#include <xen/interface/io/kbdif.h>
#include <xen/xenbus.h>
#include <xen/platform_pci.h>

#include "oxt_kbdif.h"

/**
 * Data structure describing the OXT-KBD device state.
 */
struct openxt_kbd_info {

	//The raw keyboard-- used to send key events.
	struct input_dev *kbd;
	struct input_dev *ptr;
	
	//The input device used to deliver any absolute events.
	struct input_dev *absolute_pointer;

	struct xenkbd_page *page;
	int gref;
	int irq;
	struct xenbus_device *xbdev;
	char phys[32];
};

static int  oxtkbd_remove(struct xenbus_device *);
static int  oxtkbd_connect_backend(struct xenbus_device *, struct openxt_kbd_info *);
static void oxtkbd_disconnect_backend(struct openxt_kbd_info *);

/**
 * Handler for relative motion events.
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The relative motion event to be handled. 
 */
static void __handle_relative_motion(struct openxt_kbd_info *info, 
		union xenkbd_in_event *event)
{
	//Pass the relative movement on to evdev.
	input_report_rel(info->ptr, REL_X, event->motion.rel_x);
	input_report_rel(info->ptr, REL_Y, event->motion.rel_y);

	//If the event has a Z-axis motion (a scroll wheel event),
	//send that as well.
	if (event->motion.rel_z)
		input_report_rel(info->ptr, REL_WHEEL, -event->motion.rel_z);

	input_sync(info->ptr);
}


/**
 * Handler for pure absolute (e.g. touchpad) movements.
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The absolute motion event to be handled. 
 */
static void __handle_absolute_motion(struct openxt_kbd_info *info, 
		union xenkbd_in_event *event) 
{
	//Send the new absolute coordinate...
	input_report_abs(info->absolute_pointer, ABS_X, event->pos.abs_x);
	input_report_abs(info->absolute_pointer, ABS_Y, event->pos.abs_y);

	//... and if we have a scroll wheel event, send that too.
	if (event->pos.rel_z)
		input_report_rel(info->absolute_pointer, REL_WHEEL, -event->pos.rel_z);

	input_sync(info->absolute_pointer);
}


/**
 * Handler for keypress events, including mouse button "keys".
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The keypress event to be handled. 
 */
static void __handle_key_or_button_press(struct openxt_kbd_info *info, 
		union xenkbd_in_event *event)
{
			struct input_dev *dev = NULL;

			//If this event is corresponds to a keyboard event, 
			//send it via the keyboard device.
			if (test_bit(event->key.keycode, info->kbd->keybit))
				dev = info->kbd;

			//If it corresponds to a mouse event, send it via the mouse
			//device. TODO: Possibly differentiate between ABS and REL presses?
			if (test_bit(event->key.keycode, info->ptr->keybit))
				dev = info->ptr;

			//If we found a device to send the key via, send it!
			if (dev) {
				input_report_key(dev, event->key.keycode, event->key.pressed);
				input_sync(dev);
			}
			else {
				pr_warning("unhandled keycode 0x%x\n", event->key.keycode);
			}
}


/**
 * Main handler for OpenXT PV input.
 */
static irqreturn_t input_handler(int rq, void *dev_id)
{
	__u32 cons, prod;

	//Get a reference to the device's information structure...
	struct openxt_kbd_info *info = dev_id;

	//... and get a reference to the shared page used for communications.
	struct xenkbd_page *page = info->page;

	//If we have the latest data from the ringbuffer, we're done!
	prod = page->in_prod;
	if (prod == page->in_cons)
		return IRQ_HANDLED;

	//Ensure that we always see the latest data.
	rmb();

	//For each outstanding event in the ringbuffer...
	for (cons = page->in_cons; cons != prod; cons++) {
		union xenkbd_in_event *event;

		//Get a reference to the current event.
		event = &OXT_KBD_IN_RING_REF(page, cons);

		switch (event->type) {

		case OXT_KBD_TYPE_MOTION:
			__handle_relative_motion(info, event);
			break;

		case OXT_KBD_TYPE_KEY:
			__handle_key_or_button_press(info, event);
			break;

		case OXT_KBD_TYPE_POS:
			__handle_absolute_motion(info, event);
			break;
		}
	}

	//Free the relevant space in the ringbuffer...
	mb();
	page->in_cons = cons;

	//... and signal to the other side that we're ready
	//for more data.
	notify_remote_via_irq(info->irq);
	return IRQ_HANDLED;
}

/**
 * Creates a new Xen Virtrual Pointer Device.
 */ 
static struct input_dev * __allocate_pointer_device(struct openxt_kbd_info *info, 
		char * name, int is_absolute)
{
	int i, ret;
	struct input_dev *ptr = input_allocate_device();

	//If we weren't able to allocate a new input device, fail out!
	if (!ptr)
		return NULL;

	//Otherwise, set up some of the device's defaults.
	ptr->name       = name;
	ptr->phys       = info->phys;
	ptr->id.bustype = BUS_PCI;
	ptr->id.vendor  = 0x5853;
	ptr->id.product = 0xfffe;

	//If we're creating an absolute device, register it as a provider
	//of absolute events.
	if (is_absolute) {
		__set_bit(EV_ABS, ptr->evbit);
		input_set_abs_params(ptr, ABS_X, 0, XENFB_WIDTH, 0, 0);
		input_set_abs_params(ptr, ABS_Y, 0, XENFB_HEIGHT, 0, 0);
	} 
	//Otherwise, register it as providing relative ones.
	else {
		input_set_capability(ptr, EV_REL, REL_X);
		input_set_capability(ptr, EV_REL, REL_Y);
	}

	//Either way, allow wheel input to be provided.
	input_set_capability(ptr, EV_REL, REL_WHEEL);

	//Mark this device as providing the typical mouse keys.
	__set_bit(EV_KEY, ptr->evbit);
	for (i = BTN_LEFT; i <= BTN_TASK; i++)
		__set_bit(i, ptr->keybit);

	//Finally, register the new input device with evdev.
	ret = input_register_device(ptr);
	if (ret) {
		input_free_device(ptr);
		return NULL;
	}
	
	return ptr;
}

static int xenkbd_probe(struct xenbus_device *dev,
				  const struct xenbus_device_id *id)
{
	int ret, i;
	struct openxt_kbd_info *info;
	struct input_dev *kbd;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		xenbus_dev_fatal(dev, -ENOMEM, "allocating info structure");
		return -ENOMEM;
	}
	dev_set_drvdata(&dev->dev, info);
	info->xbdev = dev;
	info->irq = -1;
	info->gref = -1;
	snprintf(info->phys, sizeof(info->phys), "xenbus/%s", dev->nodename);

	info->page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!info->page)
		goto error_nomem;

	/* keyboard */
	kbd = input_allocate_device();
	if (!kbd)
		goto error_nomem;
	kbd->name = "Xen Virtual Keyboard";
	kbd->phys = info->phys;
	kbd->id.bustype = BUS_PCI;
	kbd->id.vendor = 0x5853;
	kbd->id.product = 0xffff;

	__set_bit(EV_KEY, kbd->evbit);
	for (i = KEY_ESC; i < KEY_UNKNOWN; i++)
		__set_bit(i, kbd->keybit);
	for (i = KEY_OK; i < KEY_MAX; i++)
		__set_bit(i, kbd->keybit);

	ret = input_register_device(kbd);
	if (ret) {
		input_free_device(kbd);
		xenbus_dev_fatal(dev, ret, "input_register_device(kbd)");
		goto error;
	}
	info->kbd = kbd;

	//Allocate a new relative pointer, for our relative events...
	info->ptr = __allocate_pointer_device(info, "Xen Relative Pointer", false);
	if(!info->ptr)
		goto error_nomem;

	//...and an absolute pointer for our absolute ones.
	info->absolute_pointer = __allocate_pointer_device(info, "Xen Absolute Pointer", true);
	if(!info->absolute_pointer)
		goto error_nomem;

	//Finally, connect to the backend.
	ret = oxtkbd_connect_backend(dev, info);
	if (ret < 0)
		goto error;

	return 0;

 error_nomem:
	ret = -ENOMEM;
	xenbus_dev_fatal(dev, ret, "allocating device memory");
 error:
	oxtkbd_remove(dev);
	return ret;
}

static int xenkbd_resume(struct xenbus_device *dev)
{
	struct openxt_kbd_info *info = dev_get_drvdata(&dev->dev);

	oxtkbd_disconnect_backend(info);
	memset(info->page, 0, PAGE_SIZE);
	return oxtkbd_connect_backend(dev, info);
}

static int oxtkbd_remove(struct xenbus_device *dev)
{
	struct openxt_kbd_info *info = dev_get_drvdata(&dev->dev);

	oxtkbd_disconnect_backend(info);
	if (info->kbd)
		input_unregister_device(info->kbd);
	if (info->ptr)
		input_unregister_device(info->ptr);
	free_page((unsigned long)info->page);
	kfree(info);
	return 0;
}

static int oxtkbd_connect_backend(struct xenbus_device *dev,
				  struct openxt_kbd_info *info)
{
	int ret, evtchn;
	struct xenbus_transaction xbt;

	ret = gnttab_grant_foreign_access(dev->otherend_id,
	                                  virt_to_mfn(info->page), 0);
	if (ret < 0)
		return ret;
	info->gref = ret;

	ret = xenbus_alloc_evtchn(dev, &evtchn);
	if (ret)
		goto error_grant;
	ret = bind_evtchn_to_irqhandler(evtchn, input_handler,
					0, dev->devicetype, info);
	if (ret < 0) {
		xenbus_dev_fatal(dev, ret, "bind_evtchn_to_irqhandler");
		goto error_evtchan;
	}
	info->irq = ret;

 again:
	ret = xenbus_transaction_start(&xbt);
	if (ret) {
		xenbus_dev_fatal(dev, ret, "starting transaction");
		goto error_irqh;
	}
	ret = xenbus_printf(xbt, dev->nodename, "page-ref", "%lu",
			    virt_to_mfn(info->page));
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, dev->nodename, "page-gref", "%u", info->gref);
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, dev->nodename, "event-channel", "%u",
			    evtchn);
	if (ret)
		goto error_xenbus;
	ret = xenbus_transaction_end(xbt, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, ret, "completing transaction");
		goto error_irqh;
	}

	xenbus_switch_state(dev, XenbusStateInitialised);
	return 0;

 error_xenbus:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, ret, "writing xenstore");
 error_irqh:
	unbind_from_irqhandler(info->irq, info);
	info->irq = -1;
 error_evtchan:
	xenbus_free_evtchn(dev, evtchn);
 error_grant:
	gnttab_end_foreign_access(info->gref, 0, 0UL);
	info->gref = -1;
	return ret;
}

static void oxtkbd_disconnect_backend(struct openxt_kbd_info *info)
{
	if (info->irq >= 0)
		unbind_from_irqhandler(info->irq, info);
	info->irq = -1;
	if (info->gref >= 0)
		gnttab_end_foreign_access(info->gref, 0, 0UL);
	info->gref = -1;
}

static void xenkbd_backend_changed(struct xenbus_device *dev,
				   enum xenbus_state backend_state)
{
	struct openxt_kbd_info *info = dev_get_drvdata(&dev->dev);
	int ret, val;

	switch (backend_state) {
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitWait:
InitWait:
		ret = xenbus_scanf(XBT_NIL, info->xbdev->otherend,
				   "feature-abs-pointer", "%d", &val);
		if (ret < 0)
			val = 0;
		if (val) {
			ret = xenbus_printf(XBT_NIL, info->xbdev->nodename,
					    "request-abs-pointer", "1");
			if (ret)
				pr_warning("xenkbd: can't request abs-pointer");
		}

		xenbus_switch_state(dev, XenbusStateConnected);
		break;

	case XenbusStateConnected:
		/*
		 * Work around xenbus race condition: If backend goes
		 * through InitWait to Connected fast enough, we can
		 * get Connected twice here.
		 */
		if (dev->state != XenbusStateConnected)
			goto InitWait; /* no InitWait seen yet, fudge it */

		/* Set input abs params to match backend screen res */
		if (xenbus_scanf(XBT_NIL, info->xbdev->otherend,
				 "width", "%d", &val) > 0)
			input_set_abs_params(info->ptr, ABS_X, 0, val, 0, 0);

		if (xenbus_scanf(XBT_NIL, info->xbdev->otherend,
				 "height", "%d", &val) > 0)
			input_set_abs_params(info->ptr, ABS_Y, 0, val, 0, 0);

		break;

	case XenbusStateClosed:
		if (dev->state == XenbusStateClosed)
			break;
		/* Missed the backend's CLOSING state -- fallthrough */
	case XenbusStateClosing:
		xenbus_frontend_closed(dev);
		break;
	}
}

static const struct xenbus_device_id xenkbd_ids[] = {
	{ "vkbd" },
	{ "" }
};

static struct xenbus_driver xenkbd_driver = {
	.ids = xenkbd_ids,
	.probe = xenkbd_probe,
	.remove = oxtkbd_remove,
	.resume = xenkbd_resume,
	.otherend_changed = xenkbd_backend_changed,
};

static int __init xenkbd_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	/* Nothing to do if running in dom0. */
	if (xen_initial_domain())
		return -ENODEV;

	if (!xen_has_pv_devices())
		return -ENODEV;

	return xenbus_register_frontend(&xenkbd_driver);
}

static void __exit xenkbd_cleanup(void)
{
	xenbus_unregister_driver(&xenkbd_driver);
}

module_init(xenkbd_init);
module_exit(xenkbd_cleanup);

MODULE_DESCRIPTION("Xen virtual keyboard/pointer device frontend with touch support");
MODULE_LICENSE("GPL");
