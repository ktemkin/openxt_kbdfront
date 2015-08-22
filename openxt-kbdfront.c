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


#include <linux/input/mt.h>

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

//Forward declarations.
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
		union oxtkbd_in_event *event)
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
		union oxtkbd_in_event *event) 
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
 * Handler for multitouch touch events.
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The touch event to be handled.
 */
static void __handle_touch_down(struct openxt_kbd_info *info, 
		union oxtkbd_in_event *event) 
{
	//Send an indication that the given finger has been pressed...
	input_mt_slot(info->absolute_pointer, event->touch_move.id);
	input_mt_report_slot_state(info->absolute_pointer, MT_TOOL_FINGER, 1);
}


/**
 * Handler for multitouch touch events.
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The touch event to be handled.
 */
static void __handle_touch_movement(struct openxt_kbd_info *info, 
		union oxtkbd_in_event *event, int report_slot, int send_abs_event) 
{
	//Send the slot number, which determines which "finger" is providing
	//the touch event.
	if(report_slot)
		input_mt_slot(info->absolute_pointer, event->touch_move.id);

	//... the multi-touch coordinates...
	input_report_abs(info->absolute_pointer, ABS_MT_POSITION_X, event->touch_move.abs_x);
	input_report_abs(info->absolute_pointer, ABS_MT_POSITION_Y, event->touch_move.abs_y);

  //... and absolute touch points, if desired.
  //Note that we only send the absolute touch events for slot zero-- the other "fingers"
  //only send multi-touch events!
  if(send_abs_event && (event->touch_move.id == 0)) {
    input_report_abs(info->absolute_pointer, ABS_X, event->touch_move.abs_x);
    input_report_abs(info->absolute_pointer, ABS_Y, event->touch_move.abs_y);
  }
}


/**
 * Handler for multitouch touch events.
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The touch event to be handled.
 */
static void __handle_touch_up(struct openxt_kbd_info *info, 
		union oxtkbd_in_event *event) 
{
	//Send an indication that the given finger has been pressed...
	input_mt_slot(info->absolute_pointer, event->touch_move.id);
	input_mt_report_slot_state(info->absolute_pointer, MT_TOOL_FINGER, 0);
}


/**
 * Handle touch framing events.
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The touch event to be handled.
 */
static void __handle_touch_framing(struct openxt_kbd_info *info, 
		union oxtkbd_in_event *event) 
{
  input_mt_sync_frame(info->absolute_pointer);
	input_sync(info->absolute_pointer);;
}

/**
 * Handler for keypress events, including mouse button "keys".
 *
 * @param info The device information structure for the relevant PV input
 *		device.
 * @param event The keypress event to be handled. 
 */
static void __handle_key_or_button_press(struct openxt_kbd_info *info, 
		union oxtkbd_in_event *event)
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
		union oxtkbd_in_event *event;

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

		case OXT_KBD_TYPE_TOUCH_DOWN:
			__handle_touch_down(info, event);
      __handle_touch_movement(info, event, false, false);
			break;

		case OXT_KBD_TYPE_TOUCH_UP:
			__handle_touch_up(info, event);
			break;

		case OXT_KBD_TYPE_TOUCH_MOVE:
			__handle_touch_movement(info, event, true, false);
			break;

		case OXT_KBD_TYPE_TOUCH_FRAME:
			__handle_touch_framing(info, event);
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
 * Registers a new Xen Virtual Keyboard Device.
 *
 * @param info The information structure for the combined input device
 *		for which this device should belong to.
 * @param name The new device's name, as reported to the user.
 */
static struct input_dev * __allocate_keyboard_device(struct openxt_kbd_info *info, 
		char * name)
{
	int i, ret;

	//Allocate a new input device.
	struct input_dev *kbd = input_allocate_device();
	if (!kbd)
		return NULL;

	//Initailize its basic device information...
	kbd->name       = name;
	kbd->phys       = info->phys;
	kbd->id.bustype = BUS_PCI;
	kbd->id.vendor  = 0x5853;
	kbd->id.product = 0xffff;

	//Register all of the keys we'll want the keyboard device to handle.
	__set_bit(EV_KEY, kbd->evbit);
	for (i = KEY_ESC; i < KEY_UNKNOWN; i++)
		__set_bit(i, kbd->keybit);
	for (i = KEY_OK; i < KEY_MAX; i++)
		__set_bit(i, kbd->keybit);

	//And finally, register the device with the input subsystem.
	ret = input_register_device(kbd);
	if (ret) {
		input_free_device(kbd);
		return NULL;
	}

	return kbd;
}

/**
 * Creates a new Xen Virtrual Pointer Device.
 *
 * @param info The information structure for the combined input device
 *		for which this device should belong to.
 * @param name The new device's name, as reported to the user.
 */ 
static struct input_dev * __allocate_pointer_device(struct openxt_kbd_info *info, 
		char * name, int is_absolute, int is_multitouch)
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

    if(is_multitouch) {
      input_set_abs_params(ptr, ABS_MT_POSITION_X, 0, XENFB_WIDTH, 0, 0);
      input_set_abs_params(ptr, ABS_MT_POSITION_Y, 0, XENFB_HEIGHT, 0, 0);
      
      //Accept touches, as well.
      input_set_capability(ptr, EV_KEY, BTN_TOUCH);

      //And allow up to ten fingers of touch.
      input_mt_init_slots(ptr, 10, INPUT_MT_DIRECT);
    }
	} 
	//Otherwise, register it as providing relative ones.
	else {
		input_set_capability(ptr, EV_REL, REL_X);
		input_set_capability(ptr, EV_REL, REL_Y);
	}

	//Either way, allow wheel input to be provided.
	input_set_capability(ptr, EV_REL, REL_WHEEL);

	//Mark this device as providing the typical mouse keys.
  if(!is_multitouch) {
    __set_bit(EV_KEY, ptr->evbit);

    for (i = BTN_LEFT; i <= BTN_TASK; i++)
      __set_bit(i, ptr->keybit);
  }

	//Finally, register the new input device with evdev.
	ret = input_register_device(ptr);
	if (ret) {
		input_free_device(ptr);
		return NULL;
	}
	
	return ptr;
}

/**
 * Creates a new OpenXT combined input device, if possible.
 *
 * @param dev The XenBus device to which the new combined input device will belong.
 * @param id Information regarding the relevant xenbus device.
 * 
 * @return 0 on success, or an error code on failure.
 */
static int oxtkbd_probe(struct xenbus_device *dev, const struct xenbus_device_id *id)
{
	int ret;
	struct openxt_kbd_info *info;

	//Create a new information structure for our new combined input device.
	//This will represent the device "object", and store all of the device's
	//state.
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		xenbus_dev_fatal(dev, -ENOMEM, "allocating info structure");
		return -ENOMEM;
	}

	//Associate the new information structure with the core XenBus device.
	dev_set_drvdata(&dev->dev, info);

	//Initialize the information structure.
	info->xbdev = dev;
	info->irq   = -1;
	info->gref  = -1;
	snprintf(info->phys, sizeof(info->phys), "xenbus/%s", dev->nodename);

	//To communicate with the backend, we'll use a small "shared page"
	//as a ring buffer. We'll allocate that page now, and share it with
	//the when it tries to connect.
	info->page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!info->page)
		goto error_nomem;

	//Allocate a new keyboard device, which will handle all keypresses and
	//button presses.
	info->kbd = __allocate_keyboard_device(info, "Xen Virtual Keyboard");
	if(!info->kbd)
		goto error_nomem;

	//Allocate a new relative pointer, for our relative events...
	info->ptr = __allocate_pointer_device(info, "Xen Relative Pointer", false, false);
	if(!info->ptr)
		goto error_nomem;

	//...and an absolute pointer for our absolute ones.
	info->absolute_pointer = __allocate_pointer_device(info, "Xen Absolute Pointer", true, true);
	if(!info->absolute_pointer)
		goto error_nomem;

	//Finally, connect to the backend.
	ret = oxtkbd_connect_backend(dev, info);
	if (ret < 0)
		goto error;

	//Indicate success.
	return 0;

 error_nomem:
	ret = -ENOMEM;
	xenbus_dev_fatal(dev, ret, "allocating device memory");
 error:
	oxtkbd_remove(dev);
	return ret;
}

/**
 * Resumes use of the input device after sleep/S3.
 *
 * @param dev The device to be resumed.
 */
static int oxtkbd_resume(struct xenbus_device *dev)
{
	//Get a reference to the connection's information structure.
	struct openxt_kbd_info *info = dev_get_drvdata(&dev->dev);

	//Upon resume, we'll want to tear down our existing connection
	//and reconnect.
	oxtkbd_disconnect_backend(info);

	//Ensure that no events survive past S3.
	memset(info->page, 0, PAGE_SIZE);

	return oxtkbd_connect_backend(dev, info);
}


/**
 * Removes the provided OpenXT input connection from the system.
 * 
 * @param dev There device to be removed.
 */
static int oxtkbd_remove(struct xenbus_device *dev)
{
	//Get a reference to the connection's information structure.
	struct openxt_kbd_info *info = dev_get_drvdata(&dev->dev);

	//Disconnect ourself from the backend...
	oxtkbd_disconnect_backend(info);

	//...tear down each of our actual input devices...
	if (info->kbd)
		input_unregister_device(info->kbd);
	if (info->ptr)
		input_unregister_device(info->ptr);
	if (info->absolute_pointer) {
    input_mt_destroy_slots(info->absolute_pointer);
		input_unregister_device(info->absolute_pointer);
  }

	//... free our shared page...
	free_page((unsigned long)info->page);

	//... finally free our information structure.
	kfree(info);
	return 0;
}


/**
 * Connect the OpenXT input device to the corresponding backend.
 *
 * @param dev The device to be connected.
 * @param info The information structure that corresponds to the given device.
 *
 * @return int Zero on success, or an error code on failure.
 */
static int oxtkbd_connect_backend(struct xenbus_device *dev,
				  struct openxt_kbd_info *info)
{
	int ret, evtchn;
	struct xenbus_transaction xbt;

	//To communicate with the backend, we'll share a single page of memory
	//We'll start this process by granting out our "shared page".
	ret = gnttab_grant_foreign_access(dev->otherend_id, virt_to_mfn(info->page), 0);
	if (ret < 0)
		return ret;
	info->gref = ret;

	//Next, we'll need to create an event channel we can use to signal that data
	//has changed in our shared page.
	ret = xenbus_alloc_evtchn(dev, &evtchn);
	if (ret)
		goto error_grant;

	//Bind our input handler to our event channel-- ensuring we're recieve any
	//"new data" notifications.
	ret = bind_evtchn_to_irqhandler(evtchn, input_handler, 0, dev->devicetype, info);
	if (ret < 0) {
		xenbus_dev_fatal(dev, ret, "bind_evtchn_to_irqhandler");
		goto error_evtchan;
	}
	info->irq = ret;

 again:

	//Now that we've set up our shared assets, we'll need to communicate them
	//to the backend. First, we'll start a xenbus transaction, so we can dump
	//all of our data into the XenStore simultaneously.
	ret = xenbus_transaction_start(&xbt);
	if (ret) {
		xenbus_dev_fatal(dev, ret, "starting transaction");
		goto error_irqh;
	}

	//Provide a direct reference to the page. This allows backends that want
	//to use foreign mappings (i.e. legacy backends) to map in the shared page
	//without touching grants.
	ret = xenbus_printf(xbt, dev->nodename, "page-ref", "%lu", virt_to_mfn(info->page));
	if (ret)
		goto error_xenbus;

	//And provide our grant reference. This is the preferred way of getting the
	//shared page.
	ret = xenbus_printf(xbt, dev->nodename, "page-gref", "%u", info->gref);
	if (ret)
		goto error_xenbus;

	//Provide the number for our event channel, so the backend can signal
	//new informatino to us.
	ret = xenbus_printf(xbt, dev->nodename, "event-channel", "%u", evtchn);
	if (ret)
		goto error_xenbus;

	//Attempt to apply all of our changes at once.
	ret = xenbus_transaction_end(xbt, 0);

	//If our transaction failed...
	if (ret) {

		//... it may have been because the XenStore was busy. If this is the case,
		//repeat out transaction until we succeed, or hit an error.
		if (ret == -EAGAIN)
			goto again;

		//Otherwise, we couldn't connect. Bail out!
		xenbus_dev_fatal(dev, ret, "completing transaction");
		goto error_irqh;
	}

	//Finally, switch our state to "intialized", hopefully cueing the backend
	//to connect.
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


/**
 * Handles a backend disconnect.
 *
 * @param info The information structure for the device to be disconnected.
 */
static void oxtkbd_disconnect_backend(struct openxt_kbd_info *info)
{

	//If we had an input IRQ registered, tear it down.
	if (info->irq >= 0)
		unbind_from_irqhandler(info->irq, info);
	info->irq = -1;

	//... and if we have a shared page for our ring, tear it down.
	if (info->gref >= 0)
		gnttab_end_foreign_access(info->gref, 0, 0UL);
	info->gref = -1;
}


/**
 * Handle a change to the keyboard device's backend. 
 * This effectively moves us through the XenBus negotiation FSM.
 */
static void oxtkbd_backend_changed(struct xenbus_device *dev,
				   enum xenbus_state backend_state)
{
	int val;
	struct openxt_kbd_info *info = dev_get_drvdata(&dev->dev);

	switch (backend_state) {
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitWait:
InitWait:
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

		//Once we connect, try to adjust the screen width and height
		//to match the width and height stored in the XenStore.
		
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

/**
 * For now, we'll provide a backwards-compatible alternative to
 * xen_kbdfront that can support additional frontend capabilities.
 * This may change in the future.
 */ 
static const struct xenbus_device_id oxtkbd_ids[] = {
	{ "vkbd" },
	{ "" }
};


/**
 * Specify each of the XenBus handlers for our new input driver.
 */
static struct xenbus_driver oxtkbd_driver = {
	.ids              = oxtkbd_ids,
	.probe            = oxtkbd_probe,
	.remove           = oxtkbd_remove,
	.resume           = oxtkbd_resume,
	.otherend_changed = oxtkbd_backend_changed,
};


/**
 * Initializes the OpenXT input module.
 */
static int __init oxtkbd_init(void)
{
	//If we're not on Xen, we definitely don't apply.
	if (!xen_domain())
		return -ENODEV;

	//For now, there's no sense in running this from dom0,
	//as it has direct access to the keyboard. This may change,
	//depending on disaggregation specifics.
	if (xen_initial_domain())
		return -ENODEV;

	//If we can't use the XenBus, fail out.
	if (!xen_has_pv_devices())
		return -ENODEV;

	//Otheriwse, register our driver!
	return xenbus_register_frontend(&oxtkbd_driver);
}


/**
 * Tears down the OpenXT input module.
 */
static void __exit oxtkbd_cleanup(void)
{
	xenbus_unregister_driver(&oxtkbd_driver);
}

module_init(oxtkbd_init);
module_exit(oxtkbd_cleanup);

MODULE_DESCRIPTION("OpenXT Paravirtual Input Device");
MODULE_LICENSE("GPL");
