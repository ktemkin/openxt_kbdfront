/*
 * kbdif.h -- Xen virtual keyboard/mouse
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (C) 2005 Anthony Liguori <aliguori@us.ibm.com>
 * Copyright (C) 2006 Red Hat, Inc., Markus Armbruster <armbru@redhat.com>
 */

#ifndef __OPENXTFB_KBDIF_H__
#define __OPENXTFB_KBDIF_H__

/*
 * Frontends should ignore unknown in events.
 */

/* Pointer movement event */
#define OXT_KBD_TYPE_MOTION  1

/* Event type 2 currently not used */
/* Key event (includes pointer buttons) */
#define OXT_KBD_TYPE_KEY     3

/*
 * Pointer position event
 */
#define OXT_KBD_TYPE_POS     4

/*
 * OpenXT Multitouch events
 */
#define OXT_KBD_TYPE_TOUCH_DOWN   5
#define OXT_KBD_TYPE_TOUCH_UP     6
#define OXT_KBD_TYPE_TOUCH_MOVE   7
#define OXT_KBD_TYPE_TOUCH_FRAME  8

/**
 * Packet describing a touch "press" event.
 * Careful not to change the ordering-- id must be the second element!
 */
struct oxtkbd_touch_down {
        uint8_t type;   /* OXT_KBD_TYPE_TOUCH_DOWN */
        int32_t id;     /* the finger identifier for a touch event */
    int32_t abs_x;  /* absolute X position (in FB pixels) */
    int32_t abs_y;  /* absolute Y position (in FB pixels) */
};

/**
 * Packet describing a touch release event.
 * Careful not to change the ordering-- id must be the second element!
 */
struct oxtkbd_touch_up {
        uint8_t type;   /* OXT_KBD_TYPE_TOUCH_UP */
        int32_t id;     /* the finger identifier for a touch event */
};

/**
 * Packet describing a touch movement event.
 * Careful not to change the ordering-- id must be the second element!
 */
struct oxtkbd_touch_move {
        uint8_t type;   /* OXT_KBD_TYPE_TOUCH_MOVE */
        int32_t id;     /* the finger identifier for a touch event */
    int32_t abs_x;  /* absolute X position (in FB pixels) */
    int32_t abs_y;  /* absolute Y position (in FB pixels) */
};


/**
 * Packet describing a touch movement event.
 */
struct oxtkbd_touch_frame {
        uint8_t type;   /* OXT_KBD_TYPE_TOUCH_FRAME */
};

#define OXT_KBD_IN_EVENT_SIZE 40

/**
 * Union representing an OpenXT keyboard event.
 *
 */
union oxtkbd_in_event {

    //Do not edit the entries below;
    //they're necessary for backwards compatibility.
    uint8_t type;
    struct xenkbd_motion motion;
    struct xenkbd_key key;
    struct xenkbd_position pos;

    //New event types for OpenXT.
    //Multitouch:
    struct oxtkbd_touch_down   touch_down;
    struct oxtkbd_touch_move   touch_move;
    struct oxtkbd_touch_up     touch_up;
    struct oxtkbd_touch_frame  touch_frame;

    char pad[OXT_KBD_IN_EVENT_SIZE];
};

/* shared page */

#define OXT_KBD_IN_RING_SIZE 2048
#define OXT_KBD_IN_RING_LEN (XENKBD_IN_RING_SIZE / XENKBD_IN_EVENT_SIZE)
#define OXT_KBD_IN_RING_OFFS 1024
#define OXT_KBD_IN_RING(page) \
    ((union oxtkbd_in_event *)((char *)(page) + OXT_KBD_IN_RING_OFFS))
#define OXT_KBD_IN_RING_REF(page, idx) \
    (OXT_KBD_IN_RING((page))[(idx) % XENKBD_IN_RING_LEN])

#endif
