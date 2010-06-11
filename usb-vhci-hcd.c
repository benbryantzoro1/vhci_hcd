/*
 * usb-vhci-hcd.c -- VHCI USB host controller driver.
 *
 * Copyright (C) 2007-2008 Conemis AG Karlsruhe Germany
 * Copyright (C) 2007-2010 Michael Singer <michael@a-singer.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/device.h>
#ifdef KBUILD_EXTMOD
#	include "usb-vhci.h"
#	include "conf/usb-vhci.config.h"
#else
#	include <linux/usb-vhci.h>
#	include "usb-vhci.config.h"
#endif

#include <asm/atomic.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>

#ifdef KBUILD_EXTMOD
#	include INCLUDE_CORE_HCD
#else
#	include "../core/hcd.h"
#endif

#define DRIVER_NAME "usb_vhci_hcd"
#define DRIVER_DESC "USB Virtual Host Controller Interface"
#define DRIVER_VERSION USB_VHCI_HCD_VERSION " (" USB_VHCI_HCD_DATE ")"

#ifdef vhci_printk
#	undef vhci_printk
#endif
#define vhci_printk(level, fmt, args...) \
	printk(level DRIVER_NAME ": " fmt, ## args)
#ifdef vhci_dbg
#	undef vhci_dbg
#endif
#ifdef DEBUG
#	warning DEBUG is defined
#	define vhci_dbg(fmt, args...) \
		if(debug_output) vhci_printk(KERN_DEBUG, fmt, ## args)
#else
#	define vhci_dbg(fmt, args...) do {} while(0)
#endif
#ifdef trace_function
#	undef trace_function
#endif
#ifdef DEBUG
#	define trace_function(dev) \
		if(debug_output) dev_dbg((dev), "%s%s\n", \
			in_interrupt() ? "IN_INTERRUPT: " : "", __FUNCTION__)
#else
#	define trace_function(dev) do {} while(0)
#endif

static const char driver_name[] = DRIVER_NAME;
static const char driver_desc[] = DRIVER_DESC;
#ifdef DEBUG
static unsigned int debug_output = 0;
#endif

MODULE_DESCRIPTION(DRIVER_DESC " driver");
MODULE_AUTHOR("Michael Singer <michael@a-singer.de>");
MODULE_LICENSE("GPL");

struct vhci_urb_priv
{
	struct urb *urb;
	struct list_head urbp_list;
	atomic_t status;
};

enum vhci_rh_state
{
	VHCI_RH_RESET     = 0,
	VHCI_RH_SUSPENDED = 1,
	VHCI_RH_RUNNING   = 2
} __attribute__((packed));

struct vhci_port
{
	u16 port_status;
	u16 port_change;
	u8 port_flags;
};

struct vhci_conf
{
	struct platform_device *pdev;

	u8 port_count;
};

struct vhci
{
	spinlock_t lock;

	enum vhci_rh_state rh_state;

	// TODO: implement timer for incrementing frame_num every millisecond
	//struct timer_list timer;

	struct vhci_port *ports;
	u8 port_count;
	u8 port_sched_offset;
	u32 port_update;

	atomic_t frame_num;

	wait_queue_head_t work_event;

	// urbs which are waiting to get fetched by user space are in this list
	struct list_head urbp_list_inbox;

	// urbs which were fetched by user space but not already given back are in this list
	struct list_head urbp_list_fetched;

	// urbs which were fetched by user space and not already given back, and which should be
	// canceled are in this list
	struct list_head urbp_list_cancel;

	// urbs which were fetched by user space and not already given back, and for which the
	// user space already knows about the cancelation state are in this list
	struct list_head urbp_list_canceling;
};

static inline struct vhci *hcd_to_vhci(struct usb_hcd *hcd)
{
	return (struct vhci *)(hcd->hcd_priv);
}

static inline struct usb_hcd *vhci_to_hcd(struct vhci *vhc)
{
	return container_of((void *)vhc, struct usb_hcd, hcd_priv);
}

static inline struct device *vhci_dev(struct vhci *vhc)
{
	return vhci_to_hcd(vhc)->self.controller;
}

static inline struct vhci_conf *dev_to_vhci_conf(struct device *dev)
{
	return (struct vhci_conf *)(*((struct file **)dev->platform_data))->private_data;
}

static inline const char *vhci_dev_name(struct device *dev)
{
#ifdef OLD_DEV_BUS_ID
	return dev->bus_id;
#else
	return dev_name(dev);
#endif
}

static void maybe_set_status(struct vhci_urb_priv *urbp, int status)
{
#ifdef OLD_GIVEBACK_MECH
	struct urb *const urb = urbp->urb;
	unsigned long flags;
	spin_lock_irqsave(&urb->lock, flags);
	if(urb->status == -EINPROGRESS)
		urb->status = status;
	spin_unlock_irqrestore(&urb->lock, flags);
#else
	(void)atomic_cmpxchg(&urbp->status, -EINPROGRESS, status);
#endif
}

static void dump_urb(struct urb *urb);

#ifdef DEBUG
static const char *get_status_str(int status)
{
	switch(status)
	{
	case 0:            return "SUCCESS";
	case -EINPROGRESS: return "-EINPROGRESS";
	case -ECANCELED:   return "-ECANCELED";
	case -EPIPE:       return "-EPIPE";
	default:           return "???";
	}
}
#endif

// gives the urb back to its original owner/creator.
// caller owns vhc->lock and has irq disabled.
static void vhci_urb_giveback(struct vhci *vhc, struct vhci_urb_priv *urbp)
{
	struct urb *const urb = urbp->urb;
	struct usb_device *const udev = urb->dev;
#ifndef OLD_GIVEBACK_MECH
	int status;
#endif
	trace_function(vhci_dev(vhc));
#ifndef OLD_GIVEBACK_MECH
	status = atomic_read(&urbp->status);
#endif
	urb->hcpriv = NULL;
	list_del(&urbp->urbp_list);
#ifndef OLD_GIVEBACK_MECH
	usb_hcd_unlink_urb_from_ep(vhci_to_hcd(vhc), urb);
#endif
	spin_unlock(&vhc->lock);
	kfree(urbp);
	dump_urb(urb);
#ifdef OLD_GIVEBACK_MECH
	usb_hcd_giveback_urb(vhci_to_hcd(vhc), urb);
#else
#	ifdef DEBUG
	if(debug_output) vhci_printk(KERN_DEBUG, "status=%d(%s)\n", status, get_status_str(status));
#	endif
	usb_hcd_giveback_urb(vhci_to_hcd(vhc), urb, status);
#endif
	usb_put_dev(udev);
	spin_lock(&vhc->lock);
}

static inline void trigger_work_event(struct vhci *vhc)
{
	wake_up_interruptible(&vhc->work_event);
}

#ifdef OLD_GIVEBACK_MECH
static int vhci_urb_enqueue(struct usb_hcd *hcd, struct usb_host_endpoint *ep, struct urb *urb, gfp_t mem_flags)
#else
static int vhci_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
#endif
{
	struct vhci *vhc;
	struct vhci_urb_priv *urbp;
	unsigned long flags;
#ifndef OLD_GIVEBACK_MECH
	int retval;
#endif

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(unlikely(!urb->transfer_buffer && urb->transfer_buffer_length))
		return -EINVAL;

	urbp = kzalloc(sizeof(struct vhci_urb_priv), mem_flags);
	if(unlikely(!urbp))
		return -ENOMEM;
	urbp->urb = urb;

	spin_lock_irqsave(&vhc->lock, flags);
#ifndef OLD_GIVEBACK_MECH
	retval = usb_hcd_link_urb_to_ep(hcd, urb);
	if(unlikely(retval))
	{
		kfree(urbp);
		spin_unlock_irqrestore(&vhc->lock, flags);
		return retval;
	}
#endif
	usb_get_dev(urb->dev);
	list_add_tail(&urbp->urbp_list, &vhc->urbp_list_inbox);
	urb->hcpriv = urbp;
	spin_unlock_irqrestore(&vhc->lock, flags);
	trigger_work_event(vhc);
	return 0;
}

#ifdef OLD_GIVEBACK_MECH
static int vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb)
#else
static int vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
#endif
{
	struct vhci *vhc;
	unsigned long flags;
	struct vhci_urb_priv *entry, *urbp = NULL;
#ifndef OLD_GIVEBACK_MECH
	int retval;
#endif

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);
#ifndef OLD_GIVEBACK_MECH
	retval = usb_hcd_check_unlink_urb(hcd, urb, status);
	if(retval)
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return retval;
	}
#endif

	// search the queue of unprocessed urbs (inbox)
	list_for_each_entry(entry, &vhc->urbp_list_inbox, urbp_list)
	{
		if(entry->urb == urb)
		{
			urbp = entry;
			break;
		}
	}

	// if found in inbox
	if(urbp)
		vhci_urb_giveback(vhc, urbp);
	else // if not found...
	{
		// ...then check if the urb is on a vacation through user space
		list_for_each_entry(entry, &vhc->urbp_list_fetched, urbp_list)
		{
			if(entry->urb == urb)
			{
				// move it into the cancel list
				list_move_tail(&entry->urbp_list, &vhc->urbp_list_cancel);
				trigger_work_event(vhc);
				break;
			}
		}
	}

	spin_unlock_irqrestore(&vhc->lock, flags);
	return 0;
}

/*
static void vhci_timer(unsigned long _vhc)
{
	struct vhci *vhc = (struct vhci *)_vhc;
}
*/

static int vhci_hub_status(struct usb_hcd *hcd, char *buf)
{
	struct vhci *vhc;
	unsigned long flags;
	u8 port;
	int retval = 0;
	int idx, rel_bit, abs_bit;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	memset(buf, 0, 1 + vhc->port_count / 8);

	spin_lock_irqsave(&vhc->lock, flags);
	if(!test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return 0;
	}

	for(port = 0; port < vhc->port_count; port++)
	{
		if(vhc->ports[port].port_change)
		{
			abs_bit = port + 1;
			idx     = abs_bit / (sizeof *buf * 8);
			rel_bit = abs_bit % (sizeof *buf * 8);
			buf[idx] |= (1 << rel_bit);
			retval = 1;
		}
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "port %d status 0x%04x has changes at 0x%04x\n", (int)(port + 1), (int)vhc->ports[port].port_status, (int)vhc->ports[port].port_change);
#endif
	}

	if(vhc->rh_state == VHCI_RH_SUSPENDED)
		usb_hcd_resume_root_hub(hcd);

	spin_unlock_irqrestore(&vhc->lock, flags);
	return retval;
}

// caller has lock
// called in vhci_hub_control only
static inline void hub_descriptor(const struct vhci *vhc, char *buf, u16 len)
{
	struct usb_hub_descriptor desc;
	int portArrLen = vhc->port_count / 8 + 1; // length of one port bit-array in bytes
	u16 l = USB_DT_HUB_NONVAR_SIZE + 2 * portArrLen; // length of our hub descriptor
	memset(&desc, 0, USB_DT_HUB_NONVAR_SIZE);

	if(likely(len > USB_DT_HUB_NONVAR_SIZE))
	{
		if(unlikely(len < l)) l = len;
		if(likely(l > USB_DT_HUB_NONVAR_SIZE))
		{
			memset(buf + USB_DT_HUB_NONVAR_SIZE, 0, l - USB_DT_HUB_NONVAR_SIZE);
			if(likely(l > USB_DT_HUB_NONVAR_SIZE + portArrLen))
				memset(buf + USB_DT_HUB_NONVAR_SIZE + portArrLen, 0xff, l - (USB_DT_HUB_NONVAR_SIZE + portArrLen));
		}
	}
	else l = len;

	desc.bDescLength = l;
	desc.bDescriptorType = 0x29;
	desc.bNbrPorts = vhc->port_count;
	desc.wHubCharacteristics = __constant_cpu_to_le16(0x0009); // Per port power and overcurrent
	memcpy(buf, &desc, l);
}

// caller has lock
// first port is port# 1 (not 0)
static inline void userspace_needs_port_update(struct vhci *vhc, u8 port)
{
	vhc->port_update |= 1 << port;
	trigger_work_event(vhc);
}

static int vhci_hub_control(struct usb_hcd *hcd,
                            u16 typeReq,
                            u16 wValue,
                            u16 wIndex,
                            char *buf,
                            u16 wLength)
{
	struct vhci *vhc;
	int retval = 0;
	unsigned long flags;
	u16 *ps, *pc;
	u8 *pf;
	u8 port, has_changes = 0;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(unlikely(!test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags)))
		return -ETIMEDOUT;

	spin_lock_irqsave(&vhc->lock, flags);

	switch(typeReq)
	{
	case ClearHubFeature:
	case SetHubFeature:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: %sHubFeature [wValue=0x%04x]\n", __FUNCTION__, (typeReq == ClearHubFeature) ? "Clear" : "Set", (int)wValue);
#endif
		if(unlikely(wIndex || wLength || (wValue != C_HUB_LOCAL_POWER && wValue != C_HUB_OVER_CURRENT)))
			goto err;
		break;
	case ClearPortFeature:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: ClearPortFeature [wValue=0x%04x, wIndex=%d]\n", __FUNCTION__, (int)wValue, (int)wIndex);
#endif
		if(unlikely(!wIndex || wIndex > vhc->port_count || wLength))
			goto err;
		ps = &vhc->ports[wIndex - 1].port_status;
		pc = &vhc->ports[wIndex - 1].port_change;
		pf = &vhc->ports[wIndex - 1].port_flags;
		switch(wValue)
		{
		case USB_PORT_FEAT_SUSPEND:
			// (see USB 2.0 spec section 11.5 and 11.24.2.7.1.3)
			if(*ps & USB_PORT_STAT_SUSPEND)
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d resuming\n", (int)wIndex);
#endif
				*pf |= USB_VHCI_PORT_STAT_FLAG_RESUMING;
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_POWER:
			// (see USB 2.0 spec section 11.11 and 11.24.2.7.1.6)
			if(*ps & USB_PORT_STAT_POWER)
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d power-off\n", (int)wIndex);
#endif
				// clear all status bits except overcurrent (see USB 2.0 spec section 11.24.2.7.1)
				*ps &= USB_PORT_STAT_OVERCURRENT;
				// clear all change bits except overcurrent (see USB 2.0 spec section 11.24.2.7.2)
				*pc &= USB_PORT_STAT_C_OVERCURRENT;
				// clear resuming flag
				*pf &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_ENABLE:
			// (see USB 2.0 spec section 11.5.1.4 and 11.24.2.7.{1,2}.2)
			if(*ps & USB_PORT_STAT_ENABLE)
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d disabled\n", (int)wIndex);
#endif
				// clear enable and suspend bits (see section 11.24.2.7.1.{2,3})
				*ps &= ~(USB_PORT_STAT_ENABLE | USB_PORT_STAT_SUSPEND);
				// i'm not quite sure if the suspend change bit should be cleared too (see section 11.24.2.7.2.{2,3})
				*pc &= ~(USB_PORT_STAT_C_ENABLE | USB_PORT_STAT_C_SUSPEND);
				// clear resuming flag
				*pf &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;
				// TODO: maybe we should clear the low/high speed bits here (section 11.24.2.7.1.{7,8})
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_CONNECTION:
		case USB_PORT_FEAT_OVER_CURRENT:
		case USB_PORT_FEAT_RESET:
		case USB_PORT_FEAT_LOWSPEED:
		case USB_PORT_FEAT_HIGHSPEED:
		case USB_PORT_FEAT_INDICATOR:
			break; // no-op
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			if(*pc & (1 << (wValue - 16)))
			{
				*pc &= ~(1 << (wValue - 16));
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		//case USB_PORT_FEAT_TEST:
		default:
			goto err;
		}
		break;
	case GetHubDescriptor:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: GetHubDescriptor [wValue=0x%04x, wLength=%d]\n", __FUNCTION__, (int)wValue, (int)wLength);
#endif
		if(unlikely(wIndex))
			goto err;
		hub_descriptor(vhc, buf, wLength);
		break;
	case GetHubStatus:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: GetHubStatus\n", __FUNCTION__);
#endif
		if(unlikely(wValue || wIndex || wLength != 4))
			goto err;
		buf[0] = buf[1] = buf[2] = buf[3] = 0;
		break;
	case GetPortStatus:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: GetPortStatus [wIndex=%d]\n", __FUNCTION__, (int)wIndex);
#endif
		if(unlikely(wValue || !wIndex || wIndex > vhc->port_count || wLength != 4))
			goto err;
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: ==> [port_status=0x%04x] [port_change=0x%04x]\n", __FUNCTION__, (int)vhc->ports[wIndex - 1].port_status, (int)vhc->ports[wIndex - 1].port_change);
#endif
		buf[0] = (u8)vhc->ports[wIndex - 1].port_status;
		buf[1] = (u8)(vhc->ports[wIndex - 1].port_status >> 8);
		buf[2] = (u8)vhc->ports[wIndex - 1].port_change;
		buf[3] = (u8)(vhc->ports[wIndex - 1].port_change >> 8);
		break;
	case SetPortFeature:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: SetPortFeature [wValue=0x%04x, wIndex=%d]\n", __FUNCTION__, (int)wValue, (int)wIndex);
#endif
		if(unlikely(!wIndex || wIndex > vhc->port_count || wLength))
			goto err;
		ps = &vhc->ports[wIndex - 1].port_status;
		pc = &vhc->ports[wIndex - 1].port_change;
		pf = &vhc->ports[wIndex - 1].port_flags;
		switch(wValue)
		{
		case USB_PORT_FEAT_SUSPEND:
			// USB 2.0 spec section 11.24.2.7.1.3:
			//  "This bit can be set only if the port’s PORT_ENABLE bit is set and the hub receives
			//  a SetPortFeature(PORT_SUSPEND) request."
			// The spec also says that the suspend bit has to be cleared whenever the enable bit is cleared.
			// (see also section 11.5)
			if((*ps & USB_PORT_STAT_ENABLE) && !(*ps & USB_PORT_STAT_SUSPEND))
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d suspended\n", (int)wIndex);
#endif
				*ps |= USB_PORT_STAT_SUSPEND;
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_POWER:
			// (see USB 2.0 spec section 11.11 and 11.24.2.7.1.6)
			if(!(*ps & USB_PORT_STAT_POWER))
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d power-on\n", (int)wIndex);
#endif
				*ps |= USB_PORT_STAT_POWER;
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_RESET:
			// (see USB 2.0 spec section 11.24.2.7.1.5)
			// initiate reset only if there is a device plugged into the port and if there isn't already a reset pending
			if((*ps & USB_PORT_STAT_CONNECTION) && !(*ps & USB_PORT_STAT_RESET))
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d resetting\n", (int)wIndex);
#endif

				// keep the state of these bits and clear all others
				*ps &= USB_PORT_STAT_POWER
				     | USB_PORT_STAT_CONNECTION
				     | USB_PORT_STAT_LOW_SPEED
				     | USB_PORT_STAT_HIGH_SPEED
				     | USB_PORT_STAT_OVERCURRENT;

				*ps |= USB_PORT_STAT_RESET; // reset initiated

				// clear resuming flag
				*pf &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;

				userspace_needs_port_update(vhc, wIndex);
			}
#ifdef DEBUG
			else if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d reset not possible because of port_state=%04x\n", (int)wIndex, (int)*ps);
#endif
			break;
		case USB_PORT_FEAT_CONNECTION:
		case USB_PORT_FEAT_OVER_CURRENT:
		case USB_PORT_FEAT_LOWSPEED:
		case USB_PORT_FEAT_HIGHSPEED:
		case USB_PORT_FEAT_INDICATOR:
			break; // no-op
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			if(!(*pc & (1 << (wValue - 16))))
			{
				*pc |= 1 << (wValue - 16);
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		//case USB_PORT_FEAT_ENABLE: // port can't be enabled without reseting (USB 2.0 spec section 11.24.2.7.1.2)
		//case USB_PORT_FEAT_TEST:
		default:
			goto err;
		}
		break;
	default:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: +++UNHANDLED_REQUEST+++ [req=0x%04x, v=0x%04x, i=0x%04x, l=%d]\n", __FUNCTION__, (int)typeReq, (int)wValue, (int)wIndex, (int)wLength);
#endif
err:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: STALL\n", __FUNCTION__);
#endif
		// "protocol stall" on error
		retval = -EPIPE;
	}

	for(port = 0; port < vhc->port_count; port++)
		if(vhc->ports[port].port_change)
			has_changes = 1;

	spin_unlock_irqrestore(&vhc->lock, flags);

	if(has_changes)
		usb_hcd_poll_rh_status(hcd);
	return retval;
}

static int vhci_bus_suspend(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	unsigned long flags;
	u8 port;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);

	// suspend ports
	for(port = 0; port < vhc->port_count; port++)
	{
		if((vhc->ports[port].port_status & USB_PORT_STAT_ENABLE) &&
			!(vhc->ports[port].port_status & USB_PORT_STAT_SUSPEND))
		{
			dev_dbg(vhci_dev(vhc), "Port %d suspended\n", (int)port + 1);
			vhc->ports[port].port_status |= USB_PORT_STAT_SUSPEND;
			vhc->ports[port].port_flags &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;
			userspace_needs_port_update(vhc, port + 1);
		}
	}

	// TODO: somehow we have to suppress the resuming of ports while the bus is suspended

	vhc->rh_state = VHCI_RH_SUSPENDED;
	hcd->state = HC_STATE_SUSPENDED;

	spin_unlock_irqrestore(&vhc->lock, flags);

	return 0;
}

static int vhci_bus_resume(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	int rc = 0;
	unsigned long flags;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags)))
	{
		dev_warn(&hcd->self.root_hub->dev, "HC isn't running! You have to resume the host controller device before you resume the root hub.\n");
		rc = -ENODEV;
	}
	else
	{
		vhc->rh_state = VHCI_RH_RUNNING;
		//set_link_state(vhc);
		hcd->state = HC_STATE_RUNNING;
	}
	spin_unlock_irqrestore(&vhc->lock, flags);

	return rc;
}

static inline ssize_t show_urb(char *buf, size_t size, struct urb *urb)
{
	int ep = usb_pipeendpoint(urb->pipe);

	return snprintf(buf, size,
		"urb/%p %s ep%d%s%s len %d/%d\n",
		urb,
		({
			char *s;
			switch(urb->dev->speed)
			{
			case USB_SPEED_LOW:  s = "ls"; break;
			case USB_SPEED_FULL: s = "fs"; break;
			case USB_SPEED_HIGH: s = "hs"; break;
			default:             s = "?";  break;
			};
			s;
		}),
		ep, ep ? (usb_pipein(urb->pipe) ? "in" : "out") : "",
		({
			char *s;
			switch(usb_pipetype(urb->pipe))
			{
			case PIPE_CONTROL:   s = "";      break;
			case PIPE_BULK:      s = "-bulk"; break;
			case PIPE_INTERRUPT: s = "-int";  break;
			default:             s = "-iso";  break;
			};
			s;
		}),
		urb->actual_length, urb->transfer_buffer_length);
}

#ifdef DEBUG
static void dump_urb(struct urb *urb)
{
	int i, j;
	int max = urb->transfer_buffer_length;
	int in = usb_pipein(urb->pipe);
	if(!debug_output) return;
	vhci_printk(KERN_DEBUG, "dump_urb 0x%016llx:\n", (u64)(unsigned long)urb);
	vhci_printk(KERN_DEBUG, "dvadr=0x%02x epnum=%d epdir=%s eptpe=%s\n", (int)usb_pipedevice(urb->pipe), (int)usb_pipeendpoint(urb->pipe), (in ? "IN" : "OUT"), (usb_pipecontrol(urb->pipe) ? "CTRL" : (usb_pipebulk(urb->pipe) ? "BULK" : (usb_pipeint(urb->pipe) ? "INT" : (usb_pipeisoc(urb->pipe) ? "ISO" : "INV!")))));
#ifdef OLD_GIVEBACK_MECH
	vhci_printk(KERN_DEBUG, "status=%d(%s) flags=0x%08x buflen=%d/%d\n", urb->status, get_status_str(urb->status), urb->transfer_flags, urb->actual_length, max);
#else
	vhci_printk(KERN_DEBUG, "flags=0x%08x buflen=%d/%d\n", urb->transfer_flags, urb->actual_length, max);
#endif
	vhci_printk(KERN_DEBUG, "tbuf=0x%p tdma=0x%016llx sbuf=0x%p sdma=0x%016llx\n", urb->transfer_buffer, (u64)urb->transfer_dma, urb->setup_packet, (u64)urb->setup_dma);
	if(usb_pipeint(urb->pipe))
		vhci_printk(KERN_DEBUG, "interval=%d\n", urb->interval);
	else if(usb_pipeisoc(urb->pipe))
		vhci_printk(KERN_DEBUG, "interval=%d err=%d packets=%d startfrm=%d\n", urb->interval, urb->error_count, urb->number_of_packets, urb->start_frame);
	else if(usb_pipecontrol(urb->pipe))
	{
		const char *const sr[13] =
		{
			"GET_STATUS",
			"CLEAR_FEATURE",
			"reserved",
			"SET_FEATURE",
			"reserved",
			"SET_ADDRESS",
			"GET_DESCRIPTOR",
			"SET_DESCRIPTOR",
			"GET_CONFIGURATION",
			"SET_CONFIGURATION",
			"GET_INTERFACE",
			"SET_INTERFACE",
			"SYNCH_FRAME"
		};
		const char *const sd[9] =
		{
			"invalid",
			"DEVICE",
			"CONFIGURATION",
			"STRING",
			"INTERFACE",
			"ENDPOINT",
			"DEVICE_QUALIFIER",
			"OTHER_SPEED_CONFIGURATION",
			"INTERFACE_POWER"
		};
		const char *const sf[3] =
		{
			"ENDPOINT_HALT",
			"DEVICE_REMOTE_WAKEUP",
			"TEST_MODE"
		};
		max = urb->setup_packet[6] | (urb->setup_packet[7] << 8);
		in = urb->setup_packet[0] & 0x80;
		if(urb->setup_packet == NULL)
			vhci_printk(KERN_DEBUG, "(!!!) setup_packet is NULL\n");
		else
		{
			unsigned int val = urb->setup_packet[2] | (urb->setup_packet[3] << 8);
			vhci_printk(KERN_DEBUG, "bRequestType=0x%02x(%s,%s,%s) bRequest=0x%02x(%s)\n",
				(int)urb->setup_packet[0],
				in ? "IN" : "OUT",
				(((urb->setup_packet[0] >> 5) & 0x03) == 0) ? "STD" : ((((urb->setup_packet[0] >> 5) & 0x03) == 1) ? "CLS" : ((((urb->setup_packet[0] >> 5) & 0x03) == 2) ? "VDR" : "???")),
				((urb->setup_packet[0] & 0x1f) == 0) ? "DV" : (((urb->setup_packet[0] & 0x1f) == 1) ? "IF" : (((urb->setup_packet[0] & 0x1f) == 2) ? "EP" : (((urb->setup_packet[0] & 0x1f) == 3) ? "OT" : "??"))),
				(int)urb->setup_packet[1],
				(((urb->setup_packet[0] >> 5) & 0x03) == 0 && urb->setup_packet[1] < 13) ? sr[urb->setup_packet[1]] : "???");
			vhci_printk(KERN_DEBUG, "wValue=0x%04x", val);
			if(((urb->setup_packet[0] >> 5) & 0x03) == 0)
			{
				if(urb->setup_packet[1] == 1 || urb->setup_packet[1] == 3)
					printk("(%s)", (val < 3) ? sf[val] : "???");
				else if(urb->setup_packet[1] == 6 || urb->setup_packet[1] == 7)
					printk("(%s)", (urb->setup_packet[3] < 9) ? sd[urb->setup_packet[3]] : "???");
			}
			printk(" wIndex=0x%04x wLength=0x%04x\n", urb->setup_packet[4] | (urb->setup_packet[5] << 8), max);
		}
	}
	if(usb_pipeisoc(urb->pipe))
	{
		for(j = 0; j < urb->number_of_packets; j++)
		{
			vhci_printk(KERN_DEBUG, "PACKET%d: offset=%d pktlen=%d/%d status=%d(%s)\n", j, urb->iso_frame_desc[j].offset, urb->iso_frame_desc[j].actual_length, urb->iso_frame_desc[j].length, urb->iso_frame_desc[j].status, get_status_str(urb->iso_frame_desc[j].status));
			if(debug_output >= 2)
			{
				vhci_printk(KERN_DEBUG, "PACKET%d: data stage (%d/%d bytes %s):\n", j, urb->iso_frame_desc[j].actual_length, urb->iso_frame_desc[j].length, in ? "received" : "transmitted");
				vhci_printk(KERN_DEBUG, "PACKET%d: ", j);
				max = in ? urb->iso_frame_desc[j].actual_length : urb->iso_frame_desc[j].length;
				if(debug_output > 2 || max <= 16)
					for(i = urb->iso_frame_desc[j].offset; i < max + urb->iso_frame_desc[j].offset; i++)
						printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
				else
				{
					for(i = urb->iso_frame_desc[j].offset; i < 8 + urb->iso_frame_desc[j].offset; i++)
						printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
					printk("... ");
					for(i = max + urb->iso_frame_desc[j].offset - 8; i < max + urb->iso_frame_desc[j].offset; i++)
						printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
				}
				printk("\n");
			}
		}
	}
	else if(debug_output >= 2)
	{
		vhci_printk(KERN_DEBUG, "data stage (%d/%d bytes %s):\n", urb->actual_length, max, in ? "received" : "transmitted");
		vhci_printk(KERN_DEBUG, "");
		if(in) max = urb->actual_length;
		if(debug_output > 2 || max <= 16)
			for(i = 0; i < max; i++)
				printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
		else
		{
			for(i = 0; i < 8; i++)
				printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
			printk("... ");
			for(i = max - 8; i < max; i++)
				printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
		}
		printk("\n");
	}
}
#else
static inline void dump_urb(struct urb *urb) {/* do nothing */}
#endif

static ssize_t show_urbs(struct device *dev, struct device_attribute *attr, char *buf);
static DEVICE_ATTR(urbs_inbox,     S_IRUSR, show_urbs, NULL);
static DEVICE_ATTR(urbs_fetched,   S_IRUSR, show_urbs, NULL);
static DEVICE_ATTR(urbs_cancel,    S_IRUSR, show_urbs, NULL);
static DEVICE_ATTR(urbs_canceling, S_IRUSR, show_urbs, NULL);

static ssize_t show_urbs(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct usb_hcd *hcd;
	struct vhci *vhc;
	struct vhci_urb_priv *urbp;
	size_t size = 0;
	unsigned long flags;
	struct list_head *list;

	hcd = dev_get_drvdata(dev);
	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(attr == &dev_attr_urbs_inbox)
		list = &vhc->urbp_list_inbox;
	else if(attr == &dev_attr_urbs_fetched)
		list = &vhc->urbp_list_fetched;
	else if(attr == &dev_attr_urbs_cancel)
		list = &vhc->urbp_list_cancel;
	else if(attr == &dev_attr_urbs_canceling)
		list = &vhc->urbp_list_canceling;
	else
	{
		dev_err(vhci_dev(vhc), "unreachable code reached... wtf?\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&vhc->lock, flags);
	list_for_each_entry(urbp, list, urbp_list)
	{
		size_t temp;

		temp = PAGE_SIZE - size;
		if(unlikely(temp <= 0)) break;

		temp = show_urb(buf, temp, urbp->urb);
		buf += temp;
		size += temp;
	}
	spin_unlock_irqrestore(&vhc->lock, flags);

	return size;
}

static int vhci_start(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	int retval;
	struct vhci_port *ports;
	struct vhci_conf *conf;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	conf = dev_to_vhci_conf(vhci_dev(vhc));

	ports = kzalloc(conf->port_count * sizeof(struct vhci_port), GFP_KERNEL);
	if(unlikely(ports == NULL)) return -ENOMEM;

	spin_lock_init(&vhc->lock);
	//init_timer(&vhc->timer);
	//vhc->timer.function = vhci_timer;
	//vhc->timer.data = (unsigned long)vhc;
	vhc->ports = ports;
	vhc->port_count = conf->port_count;
	vhc->port_sched_offset = 0;
	vhc->port_update = 0;
	atomic_set(&vhc->frame_num, 0);
	init_waitqueue_head(&vhc->work_event);
	INIT_LIST_HEAD(&vhc->urbp_list_inbox);
	INIT_LIST_HEAD(&vhc->urbp_list_fetched);
	INIT_LIST_HEAD(&vhc->urbp_list_cancel);
	INIT_LIST_HEAD(&vhc->urbp_list_canceling);
	vhc->rh_state = VHCI_RH_RUNNING;

	hcd->power_budget = 30000; // practically we have unlimited power because this is a virtual device with... err... virtual power!
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_inbox);
	if(unlikely(retval != 0)) goto kfree_port_arr;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_fetched);
	if(unlikely(retval != 0)) goto rem_file_inbox;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_cancel);
	if(unlikely(retval != 0)) goto rem_file_fetched;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_canceling);
	if(unlikely(retval != 0)) goto rem_file_cancel;

	return 0;

rem_file_cancel:
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_cancel);

rem_file_fetched:
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_fetched);

rem_file_inbox:
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_inbox);

kfree_port_arr:
	kfree(ports);
	vhc->ports = NULL;
	vhc->port_count = 0;
	return retval;
}

static void vhci_stop(struct usb_hcd *hcd)
{
	struct vhci *vhc;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_canceling);
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_cancel);
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_fetched);
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_inbox);

	if(likely(vhc->ports))
	{
		kfree(vhc->ports);
		vhc->ports = NULL;
		vhc->port_count = 0;
	}

	vhc->rh_state = VHCI_RH_RESET;

	dev_info(vhci_dev(vhc), "stopped\n");
}

static int vhci_get_frame(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	vhc = hcd_to_vhci(hcd);
	trace_function(vhci_dev(vhc));
	return atomic_read(&vhc->frame_num);
}

static const struct hc_driver vhci_hcd = {
	.description      = driver_name,
	.product_desc     = "VHCI Host Controller",
	.hcd_priv_size    = sizeof(struct vhci),

	.flags            = HCD_USB2,

	.start            = vhci_start,
	.stop             = vhci_stop,

	.urb_enqueue      = vhci_urb_enqueue,
	.urb_dequeue      = vhci_urb_dequeue,

	.get_frame_number = vhci_get_frame,

	.hub_status_data  = vhci_hub_status,
	.hub_control      = vhci_hub_control,
	.bus_suspend      = vhci_bus_suspend,
	.bus_resume       = vhci_bus_resume
};

static int vhci_hcd_probe(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	int retval;

#ifdef DEBUG
	if(debug_output) dev_dbg(&pdev->dev, "%s\n", __FUNCTION__);
#endif
	dev_info(&pdev->dev, DRIVER_DESC " -- Version " DRIVER_VERSION "\n");

	hcd = usb_create_hcd(&vhci_hcd, &pdev->dev, vhci_dev_name(&pdev->dev));
	if(unlikely(!hcd)) return -ENOMEM;

	retval = usb_add_hcd(hcd, 0, 0);
	if(unlikely(retval)) usb_put_hcd(hcd);

	return retval;
}

static int vhci_hcd_remove(struct platform_device *pdev)
{
	unsigned long flags;
	struct usb_hcd *hcd;
	struct vhci *vhc;
	struct vhci_urb_priv *urbp;

    hcd = platform_get_drvdata(pdev);
	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);
	while(!list_empty(&vhc->urbp_list_inbox))
	{
		urbp = list_entry(vhc->urbp_list_inbox.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	while(!list_empty(&vhc->urbp_list_fetched))
	{
		urbp = list_entry(vhc->urbp_list_fetched.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	while(!list_empty(&vhc->urbp_list_cancel))
	{
		urbp = list_entry(vhc->urbp_list_cancel.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	while(!list_empty(&vhc->urbp_list_canceling))
	{
		urbp = list_entry(vhc->urbp_list_canceling.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	spin_unlock_irqrestore(&vhc->lock, flags);

    usb_remove_hcd(hcd);
    usb_put_hcd(hcd);

	return 0;
}

static int vhci_hcd_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct usb_hcd *hcd;
	struct vhci *vhc;
	int rc = 0;

	hcd = platform_get_drvdata(pdev);
	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(unlikely(vhc->rh_state == VHCI_RH_RUNNING))
	{
		dev_warn(&pdev->dev, "Root hub isn't suspended! You have to suspend the root hub before you suspend the host controller device.\n");
		rc = -EBUSY;
	}
	else
		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);

	return rc;
}

static int vhci_hcd_resume(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	struct vhci *vhc;

	hcd = platform_get_drvdata(pdev);
	vhc = hcd_to_vhci(hcd);
	trace_function(vhci_dev(vhc));

	set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	usb_hcd_poll_rh_status(hcd);
	return 0;
}

static struct platform_driver vhci_hcd_driver = {
	.probe      = vhci_hcd_probe,
	.remove     = vhci_hcd_remove,
	.suspend    = vhci_hcd_suspend,
	.resume     = vhci_hcd_resume,
	.driver     = {
		.name   = driver_name,
		.owner  = THIS_MODULE
	}
};

// Callback function for driver_for_each_device(..) in ioc_register(...).
// Data points to the device-id we're looking for.
// This funktion returns an error (-EINVAL), if the device has the given id assigned to it.
// (Enumeration stops/finishes on errors.)
static int device_enum(struct device *dev, void *data)
{
	struct platform_device *pdev;
	pdev = to_platform_device(dev);
	return unlikely(*((const int *)data) == pdev->id) ? -EINVAL : 0;
}

static spinlock_t dev_enum_lock = SPIN_LOCK_UNLOCKED;

static int device_open(struct inode *inode, struct file *file)
{
	vhci_dbg("%s(inode=%p, file=%p)\n", __FUNCTION__, inode, file);

	if(unlikely(file->private_data != NULL))
	{
		vhci_printk(KERN_ERR, "file->private_data != NULL (Da is schon vor mir einer drueber grutscht.)\n");
		return -EINVAL;
	}

	try_module_get(THIS_MODULE);
	return 0;
}

// called in device_ioctl only
static inline int ioc_register(struct file *file, struct usb_vhci_ioc_register __user *arg)
{
	const char *dname;
	int retval, i, usbbusnum;
	struct platform_device *pdev;
	struct vhci_conf *conf;
	struct usb_hcd *hcd;
	u8 pc;

	vhci_dbg("cmd=USB_VHCI_HCD_IOCREGISTER\n");

	if(unlikely(file->private_data))
	{
		vhci_printk(KERN_ERR, "file->private_data != NULL (USB_VHCI_HCD_IOCREGISTER already done?)\n");
		return -EPROTO;
	}

	__get_user(pc, &arg->port_count);
	if(pc > 31)
		return -EINVAL;

	// search for free device-id
	spin_lock(&dev_enum_lock);
	for(i = 0; i < 10000; i++)
	{
		retval = driver_for_each_device(&vhci_hcd_driver.driver, NULL, &i, device_enum);
		if(unlikely(!retval)) break;
	}
	if(unlikely(i >= 10000))
	{
		spin_unlock(&dev_enum_lock);
		vhci_printk(KERN_ERR, "there are too much devices!\n");
		return -EBUSY;
	}

	vhci_dbg("allocate platform_device %s.%d\n", driver_name, i);
	pdev = platform_device_alloc(driver_name, i);
	if(unlikely(!pdev))
	{
		spin_unlock(&dev_enum_lock);
		return -ENOMEM;
	}

	vhci_dbg("associate ptr to file structure with platform_device\n");
	retval = platform_device_add_data(pdev, &file, sizeof(struct file *));
	if(unlikely(retval < 0))
	{
		spin_unlock(&dev_enum_lock);
		goto pdev_put;
	}

	vhci_dbg("allocate and associate vhci_conf structure with file->private_data\n");
	conf = kmalloc(sizeof(struct vhci_conf), GFP_KERNEL);
	if(unlikely(!conf))
	{
		spin_unlock(&dev_enum_lock);
		retval = -ENOMEM;
		goto pdev_put;
	}
	conf->pdev = pdev;
	conf->port_count = pc;
	file->private_data = conf;

	vhci_dbg("add platform_device %s.%d\n", pdev->name, pdev->id);
	retval = platform_device_add(pdev);
	spin_unlock(&dev_enum_lock);
	if(unlikely(retval < 0))
	{
		vhci_printk(KERN_ERR, "add platform_device %s.%d failed\n", pdev->name, pdev->id);
		kfree(conf);
		file->private_data = NULL;
		goto pdev_put;
	}

	// copy id to user space
	__put_user(pdev->id, &arg->id);

	// copy bus-id to user space
	dname = vhci_dev_name(&pdev->dev);
	i = strlen(dname);
	i = (i < sizeof(arg->bus_id)) ? i : sizeof(arg->bus_id) - 1;
	if(copy_to_user(arg->bus_id, dname, i))
	{
		vhci_printk(KERN_WARNING, "Failed to copy bus_id to userspace.\n");
		__put_user('\0', arg->bus_id);
	}
	// make sure the last character is null
	__put_user('\0', arg->bus_id + i);

	hcd = platform_get_drvdata(pdev);
	usbbusnum = hcd->self.busnum;
	vhci_printk(KERN_INFO, "Usb bus #%d\n", usbbusnum);
	__put_user(usbbusnum, &arg->usb_busnum);

	return 0;

pdev_put:
	platform_device_put(pdev);
	return retval;
}

static int device_release(struct inode *inode, struct file *file)
{
	struct vhci_conf *conf;

	vhci_dbg("%s(inode=%p, file=%p)\n", __FUNCTION__, inode, file);

	conf = file->private_data;
	file->private_data = NULL;

	if(likely(conf))
	{
		vhci_dbg("unregister platform_device %s\n", vhci_dev_name(&conf->pdev->dev));
		platform_device_unregister(conf->pdev);

		kfree(conf);
	}
	else
	{
		vhci_dbg("was not configured\n");
	}

	module_put(THIS_MODULE);
	return 0;
}

static ssize_t device_read(struct file *file,
                           char __user *buffer,
                           size_t length,
                           loff_t *offset)
{
	vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);
	return -ENODEV;
}

static ssize_t device_write(struct file *file,
                            const char __user *buffer,
                            size_t length,
                            loff_t *offset)
{
	vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);
	return -ENODEV;
}

// called in device_ioctl only
static inline int ioc_port_stat(struct vhci *vhc, struct usb_vhci_ioc_port_stat __user *arg)
{
	unsigned long flags;
	u8 index;
	u16 status, change, overcurrent;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCPORTSTAT\n");
#endif

	__get_user(status, &arg->status);
	__get_user(change, &arg->change);
	__get_user(index, &arg->index);
	if(unlikely(!index || index > vhc->port_count))
		return -EINVAL;

	if(unlikely(change != USB_PORT_STAT_C_CONNECTION &&
	            change != USB_PORT_STAT_C_ENABLE &&
	            change != USB_PORT_STAT_C_SUSPEND &&
	            change != USB_PORT_STAT_C_OVERCURRENT &&
	            change != USB_PORT_STAT_C_RESET &&
	            change != (USB_PORT_STAT_C_RESET | USB_PORT_STAT_C_ENABLE)))
		return -EINVAL;

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_POWER)))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -EPROTO;
	}

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "performing PORT_STAT [port=%d ~status=0x%04x ~change=0x%04x]\n", (int)index, (int)status, (int)change);
#endif

	switch(change)
	{
	case USB_PORT_STAT_C_CONNECTION:
		overcurrent = vhc->ports[index - 1].port_status & USB_PORT_STAT_OVERCURRENT;
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_CONNECTION;
		if(status & USB_PORT_STAT_CONNECTION)
			vhc->ports[index - 1].port_status = USB_PORT_STAT_POWER | USB_PORT_STAT_CONNECTION |
				((status & USB_PORT_STAT_LOW_SPEED) ? USB_PORT_STAT_LOW_SPEED :
				((status & USB_PORT_STAT_HIGH_SPEED) ? USB_PORT_STAT_HIGH_SPEED : 0)) |
				overcurrent;
		else
			vhc->ports[index - 1].port_status = USB_PORT_STAT_POWER | overcurrent;
		vhc->ports[index - 1].port_flags &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;
		break;

	case USB_PORT_STAT_C_ENABLE:
		if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_CONNECTION) ||
			(vhc->ports[index - 1].port_status & USB_PORT_STAT_RESET) ||
			(status & USB_PORT_STAT_ENABLE)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EPROTO;
		}
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_ENABLE;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_ENABLE;
		vhc->ports[index - 1].port_flags &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_SUSPEND;
		break;

	case USB_PORT_STAT_C_SUSPEND:
		if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_CONNECTION) ||
			!(vhc->ports[index - 1].port_status & USB_PORT_STAT_ENABLE) ||
			(vhc->ports[index - 1].port_status & USB_PORT_STAT_RESET) ||
			(status & USB_PORT_STAT_SUSPEND)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EPROTO;
		}
		vhc->ports[index - 1].port_flags &= ~USB_VHCI_PORT_STAT_FLAG_RESUMING;
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_SUSPEND;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_SUSPEND;
		break;

	case USB_PORT_STAT_C_OVERCURRENT:
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_OVERCURRENT;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_OVERCURRENT;
		vhc->ports[index - 1].port_status |= status & USB_PORT_STAT_OVERCURRENT;
		break;

	default: // USB_PORT_STAT_C_RESET [| USB_PORT_STAT_C_ENABLE]
		if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_CONNECTION) ||
			!(vhc->ports[index - 1].port_status & USB_PORT_STAT_RESET) ||
			(status & USB_PORT_STAT_RESET)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EPROTO;
		}
		if(change & USB_PORT_STAT_C_ENABLE)
		{
			if(status & USB_PORT_STAT_ENABLE)
			{
				spin_unlock_irqrestore(&vhc->lock, flags);
				return -EPROTO;
			}
			vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_ENABLE;
		}
		else
			vhc->ports[index - 1].port_status |= status & USB_PORT_STAT_ENABLE;
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_RESET;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_RESET;
		break;
	}

	userspace_needs_port_update(vhc, index);
	spin_unlock_irqrestore(&vhc->lock, flags);

	usb_hcd_poll_rh_status(vhci_to_hcd(vhc));
	return 0;
}

static inline u8 conv_urb_type(u8 type)
{
	switch(type & 0x3)
	{
	case PIPE_ISOCHRONOUS: return USB_VHCI_URB_TYPE_ISO;
	case PIPE_INTERRUPT:   return USB_VHCI_URB_TYPE_INT;
	case PIPE_BULK:        return USB_VHCI_URB_TYPE_BULK;
	default:               return USB_VHCI_URB_TYPE_CONTROL;
	}
}

static inline u16 conv_urb_flags(unsigned int flags)
{
	return ((flags & URB_SHORT_NOT_OK) ? USB_VHCI_URB_FLAGS_SHORT_NOT_OK : 0) |
	       ((flags & URB_ISO_ASAP)     ? USB_VHCI_URB_FLAGS_ISO_ASAP     : 0) |
	       ((flags & URB_ZERO_PACKET)  ? USB_VHCI_URB_FLAGS_ZERO_PACKET  : 0);
}

static int has_work(struct vhci *vhc)
{
	unsigned long flags;
	int y = 0;
	spin_lock_irqsave(&vhc->lock, flags);
	if(vhc->port_update ||
		!list_empty(&vhc->urbp_list_cancel) ||
		!list_empty(&vhc->urbp_list_inbox))
		y = 1;
	spin_unlock_irqrestore(&vhc->lock, flags);
	return y;
}

// called in device_ioctl only
static inline int ioc_fetch_work(struct vhci *vhc, struct usb_vhci_ioc_work __user *arg, s16 timeout)
{
	unsigned long flags;
	u8 _port, port;
	struct vhci_urb_priv *urbp;
	long wret;

#ifdef DEBUG
	// Floods the logs
	//if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHWORK\n");
#endif

	if(timeout)
	{
		if(timeout > 1000)
			timeout = 1000;
		if(timeout > 0)
			wret = wait_event_interruptible_timeout(vhc->work_event, has_work(vhc), msecs_to_jiffies(timeout));
		else
			wret = wait_event_interruptible(vhc->work_event, has_work(vhc));
		if(unlikely(wret < 0))
		{
			if(likely(wret == -ERESTARTSYS))
				return -EINTR;
			return wret;
		}
		else if(!wret)
			return -ETIMEDOUT;
	}
	else
	{
		if(!has_work(vhc))
			return -ETIMEDOUT;
	}

	spin_lock_irqsave(&vhc->lock, flags);
	if(!list_empty(&vhc->urbp_list_cancel))
	{
		urbp = list_entry(vhc->urbp_list_cancel.next, struct vhci_urb_priv, urbp_list);
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHWORK [work=CANCEL_URB handle=0x%016llx]\n", (u64)(unsigned long)urbp->urb);
#endif
		__put_user(USB_VHCI_WORK_TYPE_CANCEL_URB, &arg->type);
		__put_user((u64)(unsigned long)urbp->urb, &arg->handle);
		list_move_tail(&urbp->urbp_list, &vhc->urbp_list_canceling);
		spin_unlock_irqrestore(&vhc->lock, flags);
		return 0;
	}

	if(vhc->port_update)
	{
		if(vhc->port_sched_offset >= vhc->port_count)
			vhc->port_sched_offset = 0;
		for(_port = 0; _port < vhc->port_count; _port++)
		{
			// The port which will be checked first, is rotated by port_sched_offset, so that every port
			// has its chance to be reported to user space, even if the hcd is under heavy load.
			port = (_port + vhc->port_sched_offset) % vhc->port_count;
			if(vhc->port_update & (1 << (port + 1))) // test bit
			{
				vhc->port_update &= ~(1 << (port + 1)); // clear bit
				vhc->port_sched_offset = port + 1;
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHWORK [work=PORT_STAT port=%d status=0x%04x change=0x%04x]\n", (int)(port + 1), (int)vhc->ports[port].port_status, (int)vhc->ports[port].port_change);
#endif
				__put_user(USB_VHCI_WORK_TYPE_PORT_STAT, &arg->type);
				__put_user(port + 1, &arg->work.port.index);
				__put_user(vhc->ports[port].port_status, &arg->work.port.status);
				__put_user(vhc->ports[port].port_change, &arg->work.port.change);
				__put_user(vhc->ports[port].port_flags, &arg->work.port.flags);
				spin_unlock_irqrestore(&vhc->lock, flags);
				return 0;
			}
		}
	}

repeat:
	if(!list_empty(&vhc->urbp_list_inbox))
	{
		urbp = list_entry(vhc->urbp_list_inbox.next, struct vhci_urb_priv, urbp_list);
		__put_user(USB_VHCI_WORK_TYPE_PROCESS_URB, &arg->type);
		__put_user((u64)(unsigned long)urbp->urb, &arg->handle);
		__put_user(usb_pipedevice(urbp->urb->pipe), &arg->work.urb.address);
		__put_user(usb_pipeendpoint(urbp->urb->pipe) | (usb_pipein(urbp->urb->pipe) ? 0x80 : 0x00), &arg->work.urb.endpoint);
		__put_user(conv_urb_type(usb_pipetype(urbp->urb->pipe)), &arg->work.urb.type);
		__put_user(conv_urb_flags(urbp->urb->transfer_flags), &arg->work.urb.flags);
		if(usb_pipecontrol(urbp->urb->pipe))
		{
			const struct usb_ctrlrequest *cmd;
			u16 wValue, wIndex, wLength;
			if(unlikely(!urbp->urb->setup_packet))
				goto invalid_urb;
			cmd = (struct usb_ctrlrequest *)urbp->urb->setup_packet;
			wValue = le16_to_cpu(cmd->wValue);
			wIndex = le16_to_cpu(cmd->wIndex);
			wLength = le16_to_cpu(cmd->wLength);
			if(unlikely(wLength > urbp->urb->transfer_buffer_length))
				goto invalid_urb;
			if(cmd->bRequestType & 0x80)
			{
				if(unlikely(!wLength || !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			else
			{
				if(unlikely(wLength && !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			__put_user(wLength, &arg->work.urb.buffer_length);
			__put_user(cmd->bRequestType, &arg->work.urb.setup_packet.bmRequestType);
			__put_user(cmd->bRequest, &arg->work.urb.setup_packet.bRequest);
			__put_user(wValue, &arg->work.urb.setup_packet.wValue);
			__put_user(wIndex, &arg->work.urb.setup_packet.wIndex);
			__put_user(wLength, &arg->work.urb.setup_packet.wLength);
		}
		else
		{
			if(usb_pipein(urbp->urb->pipe))
			{
				if(unlikely(!urbp->urb->transfer_buffer_length || !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			else
			{
				if(unlikely(urbp->urb->transfer_buffer_length && !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			__put_user(urbp->urb->transfer_buffer_length, &arg->work.urb.buffer_length);
		}
		__put_user(urbp->urb->interval, &arg->work.urb.interval);
		__put_user(urbp->urb->number_of_packets, &arg->work.urb.packet_count);

#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHWORK [work=PROCESS_URB handle=0x%016llx]\n", (u64)(unsigned long)urbp->urb);
#endif
		dump_urb(urbp->urb);
		list_move_tail(&urbp->urbp_list, &vhc->urbp_list_fetched);
		spin_unlock_irqrestore(&vhc->lock, flags);
		return 0;

	invalid_urb:
		// reject invalid urbs immediately
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHWORK  <<< THROWING AWAY INVALID URB >>>  [handle=0x%016llx]\n", (u64)(unsigned long)urbp->urb);
#endif
		maybe_set_status(urbp, -EPIPE);
		vhci_urb_giveback(vhc, urbp);
		goto repeat;
	}

	spin_unlock_irqrestore(&vhc->lock, flags);
	return -ENODATA;
}

// caller has lock
static inline struct vhci_urb_priv *urbp_from_handle(struct vhci *vhc, const void *handle)
{
	struct vhci_urb_priv *entry;
	list_for_each_entry(entry, &vhc->urbp_list_fetched, urbp_list)
		if(entry->urb == handle)
			return entry;
	return NULL;
}

// caller has lock
static inline struct vhci_urb_priv *urbp_from_handle_in_cancel(struct vhci *vhc, const void *handle)
{
	struct vhci_urb_priv *entry;
	list_for_each_entry(entry, &vhc->urbp_list_cancel, urbp_list)
		if(entry->urb == handle)
			return entry;
	return NULL;
}

// caller has lock
static inline struct vhci_urb_priv *urbp_from_handle_in_canceling(struct vhci *vhc, const void *handle)
{
	struct vhci_urb_priv *entry;
	list_for_each_entry(entry, &vhc->urbp_list_canceling, urbp_list)
		if(entry->urb == handle)
			return entry;
	return NULL;
}

// caller has lock
static inline int is_urb_dir_in(const struct urb *urb)
{
	if(unlikely(usb_pipecontrol(urb->pipe)))
	{
		const struct usb_ctrlrequest *cmd = (struct usb_ctrlrequest *)urb->setup_packet;
		return cmd->bRequestType & 0x80;
	}
	else
		return usb_pipein(urb->pipe);
}

// -ECANCELED doesn't report an error, but it indicates that the urb was in the "cancel"
// list or in the "canceling" list.
// If this function reports an error, then the urb will be given back to its creator anyway,
// if its handle was found.
// called in ioc_giveback{,32} only
static inline int ioc_giveback_common(struct vhci *vhc, const void *handle, int status, int act, int iso_count, int err_count, const void __user *buf, const struct usb_vhci_ioc_iso_packet_giveback __user *iso)
{
	unsigned long flags;
	int retval = 0, is_in, is_iso, i;
	struct vhci_urb_priv *urbp;

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!(urbp = urbp_from_handle(vhc, handle))))
	{
		// if not found, check the cancel{,ing} list
		if(likely((urbp = urbp_from_handle_in_canceling(vhc, handle)) ||
			(urbp = urbp_from_handle_in_cancel(vhc, handle))))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: urb was canceled\n");
#endif
			retval = -ECANCELED;
		}
		else
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: handle not found\n");
#endif
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -ENOENT;
		}
	}

	is_in = is_urb_dir_in(urbp->urb);
	is_iso = usb_pipeisoc(urbp->urb->pipe);

	if(likely(is_iso))
	{
		if(unlikely(is_in && act != urbp->urb->transfer_buffer_length))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK(ISO): invalid: buffer_actual != buffer_length\n");
#endif
			retval = -ENOBUFS;
			goto done_with_errors;
		}
		if(unlikely(iso_count != urbp->urb->number_of_packets))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK(ISO): invalid: number_of_packets missmatch\n");
#endif
			retval = -EINVAL;
			goto done_with_errors;
		}
		if(unlikely(iso_count && !iso))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK(ISO): invalid: iso_packets must not be zero\n");
#endif
			retval = -EINVAL;
			goto done_with_errors;
		}
		if(likely(iso_count))
		{
			if(!access_ok(VERIFY_READ, (void *)iso, iso_count * sizeof(struct usb_vhci_ioc_iso_packet_giveback)))
			{
				retval = -EFAULT;
				goto done_with_errors;
			}
		}
	}
	else if(unlikely(act > urbp->urb->transfer_buffer_length))
	{
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: invalid: buffer_actual > buffer_length\n");
#endif
		retval = is_in ? -ENOBUFS : -EINVAL;
		goto done_with_errors;
	}
	if(is_in)
	{
		if(unlikely(act && !buf))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: buf must not be zero\n");
#endif
			retval = -EINVAL;
			goto done_with_errors;
		}
		if(unlikely(copy_from_user(urbp->urb->transfer_buffer, buf, act)))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: copy_from_user(buf) failed\n");
#endif
			retval = -EFAULT;
			goto done_with_errors;
		}
	}
	else if(unlikely(buf))
	{
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: invalid: buf should be NULL\n");
#endif
		// no data expected, so buf should be NULL
		retval = -EINVAL;
		goto done_with_errors;
	}
	if(likely(is_iso && iso_count))
	{
		for(i = 0; i < iso_count; i++)
		{
			__get_user(urbp->urb->iso_frame_desc[i].status, &iso[i].status);
			__get_user(urbp->urb->iso_frame_desc[i].actual_length, &iso[i].packet_actual);
		}
	}
	urbp->urb->actual_length = act;
	urbp->urb->error_count = err_count;

	// now we are done with this urb and it can return to its creator
	maybe_set_status(urbp, status);
	vhci_urb_giveback(vhc, urbp);
	spin_unlock_irqrestore(&vhc->lock, flags);
#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: done\n");
#endif
	return retval;

done_with_errors:
	vhci_urb_giveback(vhc, urbp);
	spin_unlock_irqrestore(&vhc->lock, flags);
#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: done (with errors)\n");
#endif
	return retval;
}

// called in device_ioctl only
static inline int ioc_giveback(struct vhci *vhc, const struct usb_vhci_ioc_giveback __user *arg)
{
	u64 handle64;
	const void *handle;
	const void __user *buf;
	const struct usb_vhci_ioc_iso_packet_giveback __user *iso;
	int status, act, iso_count, err_count;

#ifdef DEBUF
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCGIVEBACK\n");
#endif

	if(sizeof(void *) > 4)
		__get_user(handle64, &arg->handle);
	else
	{
		u32 handle1, handle2;
		__get_user(handle1, (u32 __user *)&arg->handle);
		__get_user(handle2, (u32 __user *)&arg->handle + 1);
		*((u32 *)&handle64) = handle1;
		*((u32 *)&handle64 + 1) = handle2;
		if(handle64 >> 32)
			return -EINVAL;
	}
	__get_user(status, &arg->status);
	__get_user(act, &arg->buffer_actual);
	__get_user(iso_count, &arg->packet_count);
	__get_user(err_count, &arg->error_count);
	__get_user(buf, &arg->buffer);
	__get_user(iso, &arg->iso_packets);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	return ioc_giveback_common(vhc, handle, status, act, iso_count, err_count, buf, iso);
}

// called in ioc_fetch_data{,32} only
static inline int ioc_fetch_data_common(struct vhci *vhc, const void *handle, void __user *user_buf, int user_len, struct usb_vhci_ioc_iso_packet_data __user *iso, int iso_count)
{
	unsigned long flags;
	int tb_len, is_in, is_iso, i;
	struct vhci_urb_priv *urbp;

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!(urbp = urbp_from_handle(vhc, handle))))
	{
		// if not found, check the cancel{,ing} list
		if(likely((urbp = urbp_from_handle_in_cancel(vhc, handle)) ||
			(urbp = urbp_from_handle_in_canceling(vhc, handle))))
		{
			// we can give the urb back to its creator now, because the user space is informed about
			// its cancelation
			vhci_urb_giveback(vhc, urbp);
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -ECANCELED;
		}
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -ENOENT;
	}

	tb_len = urbp->urb->transfer_buffer_length;
	if(unlikely(usb_pipecontrol(urbp->urb->pipe)))
	{
		const struct usb_ctrlrequest *cmd = (struct usb_ctrlrequest *)urbp->urb->setup_packet;
		tb_len = le16_to_cpu(cmd->wLength);
	}

	is_in = is_urb_dir_in(urbp->urb);
	is_iso = usb_pipeisoc(urbp->urb->pipe);

	if(likely(is_iso))
	{
		if(unlikely(iso_count != urbp->urb->number_of_packets))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EINVAL;
		}
		if(likely(iso_count))
		{
			if(unlikely(!iso))
			{
				spin_unlock_irqrestore(&vhc->lock, flags);
				return -EINVAL;
			}
			if(!access_ok(VERIFY_WRITE, (void *)iso, iso_count * sizeof(struct usb_vhci_ioc_iso_packet_data)))
			{
				spin_unlock_irqrestore(&vhc->lock, flags);
				return -EFAULT;
			}
			for(i = 0; i < iso_count; i++)
			{
				__put_user(urbp->urb->iso_frame_desc[i].offset, &iso[i].offset);
				__put_user(urbp->urb->iso_frame_desc[i].length, &iso[i].packet_length);
			}
		}
	}
	else if(unlikely(is_in || !tb_len || !urbp->urb->transfer_buffer))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -ENODATA;
	}

	if(likely(!is_in && tb_len))
	{
		if(unlikely(!user_buf || user_len < tb_len))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EINVAL;
		}
		if(unlikely(copy_to_user(user_buf, urbp->urb->transfer_buffer, tb_len)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EFAULT;
		}
	}
	spin_unlock_irqrestore(&vhc->lock, flags);
	return 0;
}

// called in device_ioctl only
static inline int ioc_fetch_data(struct vhci *vhc, struct usb_vhci_ioc_urb_data __user *arg)
{
	u64 handle64;
	const void *handle;
	void __user *user_buf;
	struct usb_vhci_ioc_iso_packet_data __user *iso;
	int user_len, iso_count;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHDATA\n");
#endif
	if(sizeof(void *) > 4)
		__get_user(handle64, &arg->handle);
	else
	{
		u32 handle1, handle2;
		__get_user(handle1, (u32 __user *)&arg->handle);
		__get_user(handle2, (u32 __user *)&arg->handle + 1);
		*((u32 *)&handle64) = handle1;
		*((u32 *)&handle64 + 1) = handle2;
		if(handle64 >> 32)
			return -EINVAL;
	}
	__get_user(user_len, &arg->buffer_length);
	__get_user(iso_count, &arg->packet_count);
	__get_user(user_buf, &arg->buffer);
	__get_user(iso, &arg->iso_packets);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	return ioc_fetch_data_common(vhc, handle, user_buf, user_len, iso, iso_count);
}

#ifdef CONFIG_COMPAT
// called in device_ioctl only
static inline int ioc_giveback32(struct vhci *vhc, const struct usb_vhci_ioc_giveback32 __user *arg)
{
	u64 handle64;
	const void *handle;
	u32 buf32, iso32;
	const void __user *buf;
	const struct usb_vhci_ioc_iso_packet_giveback __user *iso;
	int status, act, iso_count, err_count;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCGIVEBACK32\n");
#endif
	__get_user(handle64, &arg->handle);
	__get_user(status, &arg->status);
	__get_user(act, &arg->buffer_actual);
	__get_user(iso_count, &arg->packet_count);
	__get_user(err_count, &arg->error_count);
	__get_user(buf32, &arg->buffer);
	__get_user(iso32, &arg->iso_packets);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	buf = compat_ptr(buf32);
	iso = compat_ptr(iso32);
	return ioc_giveback_common(vhc, handle, status, act, iso_count, err_count, buf, iso);
}

// called in device_ioctl only
static inline int ioc_fetch_data32(struct vhci *vhc, struct usb_vhci_ioc_urb_data32 __user *arg)
{
	u64 handle64;
	const void *handle;
	u32 user_buf32, iso32;
	void __user *user_buf;
	struct usb_vhci_ioc_iso_packet_data __user *iso;
	int user_len, iso_count;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=USB_VHCI_HCD_IOCFETCHDATA32\n");
#endif

	__get_user(handle64, &arg->handle);
	__get_user(user_len, &arg->buffer_length);
	__get_user(iso_count, &arg->packet_count);
	__get_user(user_buf32, &arg->buffer);
	__get_user(iso32, &arg->iso_packets);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	user_buf = compat_ptr(user_buf32);
	iso = compat_ptr(iso32);
	return ioc_fetch_data_common(vhc, handle, user_buf, user_len, iso, iso_count);
}
#endif

static long device_do_ioctl(struct file *file,
                           unsigned int cmd,
                           void __user *arg)
{
	struct vhci_conf *conf;
	struct usb_hcd *hcd;
	struct vhci *vhc;
	long ret = 0;
	s16 timeout;

	// Floods the logs
	//vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);

	if(unlikely(_IOC_TYPE(cmd) != USB_VHCI_HCD_IOC_MAGIC)) return -ENOTTY;
	if(unlikely(_IOC_NR(cmd) > USB_VHCI_HCD_IOC_MAXNR)) return -ENOTTY;

	if(unlikely((_IOC_DIR(cmd) & _IOC_READ) && !access_ok(VERIFY_WRITE, arg, _IOC_SIZE(cmd))))
		return -EFAULT;
	if(unlikely((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok(VERIFY_READ, arg, _IOC_SIZE(cmd))))
		return -EFAULT;

	if(unlikely(cmd == USB_VHCI_HCD_IOCREGISTER))
		return ioc_register(file, (struct usb_vhci_ioc_register __user *)arg);

	conf = file->private_data;

	if(unlikely(!conf))
		return -EPROTO;

	hcd = platform_get_drvdata(conf->pdev);
	vhc = hcd_to_vhci(hcd);

	switch(__builtin_expect(cmd, USB_VHCI_HCD_IOCFETCHWORK))
	{
	case USB_VHCI_HCD_IOCPORTSTAT:
		ret = ioc_port_stat(vhc, (struct usb_vhci_ioc_port_stat __user *)arg);
		break;

	case USB_VHCI_HCD_IOCFETCHWORK_RO:
		ret = ioc_fetch_work(vhc, (struct usb_vhci_ioc_work __user *)arg, 100);
		break;

	case USB_VHCI_HCD_IOCFETCHWORK:
		__get_user(timeout, &((struct usb_vhci_ioc_work __user *)arg)->timeout);
		ret = ioc_fetch_work(vhc, (struct usb_vhci_ioc_work __user *)arg, timeout);
		break;

	case USB_VHCI_HCD_IOCGIVEBACK:
		ret = ioc_giveback(vhc, (struct usb_vhci_ioc_giveback __user *)arg);
		break;

	case USB_VHCI_HCD_IOCFETCHDATA:
		ret = ioc_fetch_data(vhc, (struct usb_vhci_ioc_urb_data __user *)arg);
		break;

#ifdef CONFIG_COMPAT
	case USB_VHCI_HCD_IOCGIVEBACK32:
		ret = ioc_giveback32(vhc, (struct usb_vhci_ioc_giveback32 __user *)arg);
		break;

	case USB_VHCI_HCD_IOCFETCHDATA32:
		ret = ioc_fetch_data32(vhc, (struct usb_vhci_ioc_urb_data32 __user *)arg);
		break;
#endif

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static long device_ioctl(struct file *file,
                         unsigned int cmd,
                         unsigned long arg)
{
	return device_do_ioctl(file, cmd, (void __user *)arg);
}

#ifdef CONFIG_COMPAT
static long device_ioctl32(struct file *file,
                           unsigned int cmd,
                           unsigned long arg)
{
	return device_do_ioctl(file, cmd, compat_ptr(arg));
}
#endif

static loff_t device_llseek(struct file *file, loff_t offset, int origin)
{
	vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);
	return -ESPIPE;
}

static struct file_operations fops = {
	.owner          = THIS_MODULE,
	.llseek         = device_llseek,
	.read           = device_read,
	.write          = device_write,
	.unlocked_ioctl = device_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = device_ioctl32,
#endif
	.open           = device_open,
	.release        = device_release // a.k.a. close
};

#ifdef DEBUG
static ssize_t show_debug_output(struct device_driver *drv, char *buf)
{
	if(buf != NULL)
	{
		switch(debug_output)
		{
		case 0:  *buf = '0'; break; // No debug output
		case 1:  *buf = '1'; break; // Debug output without data buffer dumps
		case 2:  *buf = '2'; break; // Debug output with short data buffer dumps
		default: *buf = '3'; break; // Debug output with full data buffer dumps
		}
	}
	return 1;
}

static ssize_t store_debug_output(struct device_driver *drv, const char *buf, size_t count)
{
	if(count != 1 || buf == NULL) return -EINVAL;
	switch(*buf)
	{
	case '0': debug_output = 0; return 1;
	case '1': debug_output = 1; return 1;
	case '2': debug_output = 2; return 1;
	case '3': debug_output = 3; return 1;
	}
	return -EINVAL;
}

static DRIVER_ATTR(debug_output, S_IRUSR | S_IWUSR, show_debug_output, store_debug_output);
#endif

static void vhci_device_release(struct device *dev)
{
}

static int vhci_major;

static struct class vhci_class = {
	.owner = THIS_MODULE,
	.name = driver_name
};

static struct device vhci_device = {
	.class = &vhci_class,
	.release = vhci_device_release,
#ifndef NO_DEV_INIT_NAME
	.init_name = "vhci-ctrl",
#endif
	.driver = &vhci_hcd_driver.driver
};

static int __init init(void)
{
	int	retval;

	if(usb_disabled()) return -ENODEV;

	vhci_printk(KERN_INFO, DRIVER_DESC " -- Version " DRIVER_VERSION "\n");

#ifdef DEBUG
	vhci_printk(KERN_DEBUG, "register platform_driver %s\n", driver_name);
#endif
	retval = platform_driver_register(&vhci_hcd_driver);
	if(unlikely(retval < 0))
	{
		vhci_printk(KERN_ERR, "register platform_driver failed\n");
		return retval;
	}

	vhci_major = register_chrdev(0, driver_name, &fops);
	if(unlikely(vhci_major < 0))
	{
		vhci_printk(KERN_ERR, "Sorry, registering the character device failed with %d.\n", retval);
#ifdef DEBUG
		vhci_printk(KERN_DEBUG, "unregister platform_driver %s\n", driver_name);
#endif
		platform_driver_unregister(&vhci_hcd_driver);
		return retval;
	}

	vhci_printk(KERN_INFO, "Successfully registered the character device.\n");
	vhci_printk(KERN_INFO, "The major device number is %d.\n", vhci_major);

	if(unlikely(class_register(&vhci_class)))
	{
		vhci_printk(KERN_WARNING, "failed to register class %s\n", driver_name);
	}
	else
	{
#ifdef NO_DEV_INIT_NAME
#ifdef OLD_DEV_BUS_ID
		strlcpy((void *)&vhci_device.bus_id, "vhci-ctrl", 10);
#else
		dev_set_name(&vhci_device, "vhci-ctrl");
#endif
#endif
		vhci_device.devt = MKDEV(vhci_major, 0);
		if(unlikely(device_register(&vhci_device)))
		{
			vhci_printk(KERN_WARNING, "failed to register device %s\n", "vhci-ctrl");
		}
	}

#ifdef DEBUG
	retval = driver_create_file(&vhci_hcd_driver.driver, &driver_attr_debug_output);
	if(unlikely(retval != 0))
	{
		vhci_printk(KERN_DEBUG, "driver_create_file(&vhci_hcd_driver, &driver_attr_debug_output) failed\n");
		vhci_printk(KERN_DEBUG, "==> ignoring\n");
	}
#endif

#ifdef DEBUG
	vhci_printk(KERN_DEBUG, "USB_VHCI_HCD_IOCREGISTER     = %08x\n", (unsigned int)USB_VHCI_HCD_IOCREGISTER);
    vhci_printk(KERN_DEBUG, "USB_VHCI_HCD_IOCPORTSTAT     = %08x\n", (unsigned int)USB_VHCI_HCD_IOCPORTSTAT);
	vhci_printk(KERN_DEBUG, "USB_VHCI_HCD_IOCFETCHWORK_RO = %08x\n", (unsigned int)USB_VHCI_HCD_IOCFETCHWORK_RO);
    vhci_printk(KERN_DEBUG, "USB_VHCI_HCD_IOCFETCHWORK    = %08x\n", (unsigned int)USB_VHCI_HCD_IOCFETCHWORK);
    vhci_printk(KERN_DEBUG, "USB_VHCI_HCD_IOCGIVEBACK     = %08x\n", (unsigned int)USB_VHCI_HCD_IOCGIVEBACK);
    vhci_printk(KERN_DEBUG, "USB_VHCI_HCD_IOCFETCHDATA    = %08x\n", (unsigned int)USB_VHCI_HCD_IOCFETCHDATA);
#endif

	return 0;
}
module_init(init);

static void __exit cleanup(void)
{
#ifdef DEBUG
	driver_remove_file(&vhci_hcd_driver.driver, &driver_attr_debug_output);
#endif
	device_unregister(&vhci_device);
	class_unregister(&vhci_class);
	unregister_chrdev(vhci_major, driver_name);
	vhci_dbg("unregister platform_driver %s\n", driver_name);
	platform_driver_unregister(&vhci_hcd_driver);
	vhci_dbg("bin weg\n");
}
module_exit(cleanup);

