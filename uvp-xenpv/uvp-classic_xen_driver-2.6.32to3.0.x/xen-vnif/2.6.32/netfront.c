/******************************************************************************
 * Virtual network driver for conversing with remote driver backends.
 *
 * Copyright (c) 2002-2005, K A Fraser
 * Copyright (c) 2005, XenSource Ltd
 * Copyright (C) 2007 Solarflare Communications, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/ethtool.h>
#include <linux/in.h>
#include <linux/if_ether.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <net/sock.h>
#include <net/pkt_sched.h>
#include <net/arp.h>
#include <net/route.h>
#include <asm/uaccess.h>
#include <xen/evtchn.h>
#include <xen/xenbus.h>
#include <xen/interface/io/netif.h>
#include <xen/interface/memory.h>
#include <xen/balloon.h>
#include <asm/page.h>
#include <asm/maddr.h>
#include <asm/uaccess.h>
#include <xen/interface/grant_table.h>
#include <xen/gnttab.h>
#include <net/ip.h>

#include "acpi_bus.h"
#include "actypes.h"

struct netfront_cb {
	struct page *page;
	unsigned offset;
};

#define NETFRONT_SKB_CB(skb)	((struct netfront_cb *)((skb)->cb))

#include "netfront.h"

/*
 * Mutually-exclusive module options to select receive data path:
 *  rx_copy : Packets are copied by network backend into local memory
 *  rx_flip : Page containing packet data is transferred to our ownership
 * For fully-virtualised guests there is no option - copying must be used.
 * For paravirtualised guests, flipping is the default.
 */
#ifdef CONFIG_XEN
static int MODPARM_rx_copy = 0;
module_param_named(rx_copy, MODPARM_rx_copy, bool, 0);
MODULE_PARM_DESC(rx_copy, "Copy packets from network card (rather than flip)");
static int MODPARM_rx_flip = 0;
module_param_named(rx_flip, MODPARM_rx_flip, bool, 0);
MODULE_PARM_DESC(rx_flip, "Flip packets from network card (rather than copy)");
#else
static const int MODPARM_rx_copy = 1;
static const int MODPARM_rx_flip = 0;
#endif

#define RX_COPY_THRESHOLD 256
#define DEFAULT_DEBUG_LEVEL_SHIFT 3

/* If we don't have GSO, fake things up so that we never try to use it. */
#if defined(NETIF_F_GSO)
#define HAVE_GSO			1
#define HAVE_TSO			1 /* TSO is a subset of GSO */
#define HAVE_CSUM_OFFLOAD		1
static inline void dev_disable_gso_features(struct net_device *dev)
{
	/* Turn off all GSO bits except ROBUST. */
	dev->features &= ~NETIF_F_GSO_MASK;
	dev->features |= NETIF_F_GSO_ROBUST;
}
#elif defined(NETIF_F_TSO)
#define HAVE_GSO		       0
#define HAVE_TSO                       1

/* Some older kernels cannot cope with incorrect checksums,
 * particularly in netfilter. I'm not sure there is 100% correlation
 * with the presence of NETIF_F_TSO but it appears to be a good first
 * approximiation.
 */
#define HAVE_CSUM_OFFLOAD              0

#define gso_size tso_size
#define gso_segs tso_segs
static inline void dev_disable_gso_features(struct net_device *dev)
{
       /* Turn off all TSO bits. */
       dev->features &= ~NETIF_F_TSO;
}
static inline int skb_is_gso(const struct sk_buff *skb)
{
        return skb_shinfo(skb)->tso_size;
}
static inline int skb_gso_ok(struct sk_buff *skb, int features)
{
        return (features & NETIF_F_TSO);
}

static inline int netif_needs_gso(struct net_device *dev, struct sk_buff *skb)
{
        return skb_is_gso(skb) &&
               (!skb_gso_ok(skb, dev->features) ||
                unlikely(skb->ip_summed != CHECKSUM_PARTIAL));
}
#else
#define HAVE_GSO			0
#define HAVE_TSO			0
#define HAVE_CSUM_OFFLOAD		0
#define netif_needs_gso(dev, skb)	0
#define dev_disable_gso_features(dev)	((void)0)
#define ethtool_op_set_tso(dev, data)	(-ENOSYS)
#endif

#define GRANT_INVALID_REF	0

struct netfront_rx_info {
	struct netif_rx_response rx;
	struct netif_extra_info extras[XEN_NETIF_EXTRA_TYPE_MAX - 1];
};

/*
 * Implement our own carrier flag: the network stack's version causes delays
 * when the carrier is re-enabled (in particular, dev_activate() may not
 * immediately be called, which can cause packet loss).
 */
#define netfront_carrier_on(netif)	((netif)->carrier = 1)
#define netfront_carrier_off(netif)	((netif)->carrier = 0)
#define netfront_carrier_ok(netif)	((netif)->carrier)

/*
 * Access macros for acquiring freeing slots in tx_skbs[].
 */

static inline void add_id_to_freelist(struct sk_buff **list, unsigned short id)
{
	list[id] = list[0];
	list[0]  = (void *)(unsigned long)id;
}

static inline unsigned short get_id_from_freelist(struct sk_buff **list)
{
	unsigned int id = (unsigned int)(unsigned long)list[0];
	list[0] = list[id];
	return id;
}

static inline int xennet_rxidx(RING_IDX idx)
{
	return idx & (NET_RX_RING_SIZE - 1);
}

static inline struct sk_buff *xennet_get_rx_skb(struct netfront_info *np,
						RING_IDX ri)
{
	int i = xennet_rxidx(ri);
	struct sk_buff *skb = np->rx_skbs[i];
	np->rx_skbs[i] = NULL;
	return skb;
}

static inline grant_ref_t xennet_get_rx_ref(struct netfront_info *np,
					    RING_IDX ri)
{
	int i = xennet_rxidx(ri);
	grant_ref_t ref = np->grant_rx_ref[i];
	np->grant_rx_ref[i] = GRANT_INVALID_REF;
	return ref;
}

#define DPRINTK(fmt, args...)				\
	pr_debug("netfront (%s:%d) " fmt,		\
		 __FUNCTION__, __LINE__, ##args)
#define IPRINTK(fmt, args...)				\
	printk(KERN_INFO "netfront: " fmt, ##args)
#define WPRINTK(fmt, args...)				\
	printk(KERN_WARNING "netfront: " fmt, ##args)

static int setup_device(struct xenbus_device *, struct netfront_info *);
static struct net_device *create_netdev(struct xenbus_device *);

static void end_access(int, void *);
static void netif_disconnect_backend(struct netfront_info *);

static int network_connect(struct net_device *);
static void network_tx_buf_gc(struct net_device *);
static void network_alloc_rx_buffers(struct net_device *);
static void send_fake_arp(struct net_device *, int arpType);

static irqreturn_t netif_int(int irq, void *dev_id);

#ifdef CONFIG_SYSFS
static int xennet_sysfs_addif(struct net_device *netdev);
static void xennet_sysfs_delif(struct net_device *netdev);
#else /* !CONFIG_SYSFS */
#define xennet_sysfs_addif(dev) (0)
#define xennet_sysfs_delif(dev) do { } while(0)
#endif

static inline int xennet_can_sg(struct net_device *dev)
{
	return dev->features & NETIF_F_SG;
}

/**
 * remove pci device of nic8139
 */
static int rtl8139_acpi_bus_trim(acpi_handle handle)
{
	struct acpi_device *device;
	int retval;

	retval = acpi_bus_get_device(handle, &device);
	if (retval) {
		printk(KERN_INFO "acpi_device not found\n");
		return retval;
	}

	retval = acpi_bus_trim(device, 1);
	if (retval)
		printk(KERN_INFO "cannot remove from acpi list\n");

	return retval;
}

static void remove_rtl8139(void)
{
	struct pci_dev *pdev = NULL;
	acpi_handle phandle = NULL;

	pdev = pci_get_device(0x10ec, 0x8139, pdev);

	if (pdev) {
		phandle = DEVICE_ACPI_HANDLE(&pdev->dev);
		printk(KERN_INFO "remove_rtl8139 : rtl8139 of subsystem(0x%2x,0x%2x) removed.\n", 
						pdev->subsystem_vendor, pdev->subsystem_device);
		pci_disable_device(pdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19) /* check kernel version */
		pci_remove_bus_device(pdev);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
		pci_stop_bus_device(pdev);
		pci_remove_bus_device(pdev);
#else
		pci_stop_and_remove_bus_device(pdev);
#endif /* end check kernel version */
		pci_dev_put(pdev);
	}

	if (phandle) {
		rtl8139_acpi_bus_trim(phandle);
	}

	return;
}

static int xen_net_read_mac(struct xenbus_device *dev, u8 mac[]);

/**
 * Entry point to this code when a new device is created.  Allocate the basic
 * structures and the ring buffers for communication with the backend, and
 * inform the backend of the appropriate details for those.
 */
static int __devinit netfront_probe(struct xenbus_device *dev,
				    const struct xenbus_device_id *id)
{
	int err;
	struct net_device *netdev;
	struct netfront_info *info;
    
	/* remove rtl8139 when xen nic devices appear */
	remove_rtl8139();

	netdev = create_netdev(dev);
	if (IS_ERR(netdev)) {
		err = PTR_ERR(netdev);
		xenbus_dev_fatal(dev, err, "creating netdev");
		return err;
	}

	info = netdev_priv(netdev);
	dev_set_drvdata(&dev->dev, info);

	err = xen_net_read_mac(dev, info->netdev->dev_addr);
	if (err) {
		printk(KERN_ERR "%s: xen-netfront read nic hwaddr error.\n", __FUNCTION__);
	}

	err = register_netdev(info->netdev);
	if (err) {
		printk(KERN_WARNING "%s: register_netdev err=%d\n",
		       __FUNCTION__, err);
		goto fail;
	}

	err = xennet_sysfs_addif(info->netdev);
	if (err) {
		unregister_netdev(info->netdev);
		printk(KERN_WARNING "%s: add sysfs failed err=%d\n",
		       __FUNCTION__, err);
		goto fail;
	}

	return 0;

 fail:
	free_netdev(netdev);
	dev_set_drvdata(&dev->dev, NULL);
	return err;
}

static int __devexit netfront_remove(struct xenbus_device *dev)
{
	struct netfront_info *info = dev_get_drvdata(&dev->dev);

	DPRINTK("%s\n", dev->nodename);

	netfront_accelerator_call_remove(info, dev);

	netif_disconnect_backend(info);

	del_timer_sync(&info->rx_refill_timer);

	xennet_sysfs_delif(info->netdev);

	unregister_netdev(info->netdev);

	free_netdev(info->netdev);

	return 0;
}


static int netfront_suspend(struct xenbus_device *dev)
{
	struct netfront_info *info = dev_get_drvdata(&dev->dev);
	return netfront_accelerator_suspend(info, dev);
}


static int netfront_suspend_cancel(struct xenbus_device *dev)
{
	struct netfront_info *info = dev_get_drvdata(&dev->dev);
	return netfront_accelerator_suspend_cancel(info, dev);
}


/**
 * We are reconnecting to the backend, due to a suspend/resume, or a backend
 * driver restart.  We tear down our netif structure and recreate it, but
 * leave the device-layer structures intact so that this is transparent to the
 * rest of the kernel.
 */
static int netfront_resume(struct xenbus_device *dev)
{
	struct netfront_info *info = dev_get_drvdata(&dev->dev);

	DPRINTK("%s\n", dev->nodename);

	netfront_accelerator_resume(info, dev);

	netif_disconnect_backend(info);
	return 0;
}

static int xen_net_read_mac(struct xenbus_device *dev, u8 mac[])
{
	char *s, *e, *macstr;
	int i;

	macstr = s = xenbus_read(XBT_NIL, dev->nodename, "mac", NULL);
	if (IS_ERR(macstr))
		return PTR_ERR(macstr);

	for (i = 0; i < ETH_ALEN; i++) {
		mac[i] = simple_strtoul(s, &e, 16);
		if ((s == e) || (*e != ((i == ETH_ALEN-1) ? '\0' : ':'))) {
			kfree(macstr);
			return -ENOENT;
		}
		s = e+1;
	}

	kfree(macstr);
	return 0;
}

/* Common code used when first setting up, and when resuming. */
static int talk_to_backend(struct xenbus_device *dev,
			   struct netfront_info *info)
{
	const char *message;
	struct xenbus_transaction xbt;
	int err;

	/* Read mac only in the first setup. */
	if (!is_valid_ether_addr(info->mac)) {
		err = xen_net_read_mac(dev, info->mac);
		if (err) {
			xenbus_dev_fatal(dev, err, "parsing %s/mac",
					 dev->nodename);
			goto out;
		}
	}

	/* Create shared ring, alloc event channel. */
	err = setup_device(dev, info);
	if (err)
		goto out;

	/* This will load an accelerator if one is configured when the
	 * watch fires */
	netfront_accelerator_add_watch(info);

again:
	err = xenbus_transaction_start(&xbt);
	if (err) {
		xenbus_dev_fatal(dev, err, "starting transaction");
		goto destroy_ring;
	}

	err = xenbus_printf(xbt, dev->nodename, "tx-ring-ref","%u",
			    info->tx_ring_ref);
	if (err) {
		message = "writing tx ring-ref";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, dev->nodename, "rx-ring-ref","%u",
			    info->rx_ring_ref);
	if (err) {
		message = "writing rx ring-ref";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, dev->nodename,
			    "event-channel", "%u",
			    irq_to_evtchn_port(info->irq));
	if (err) {
		message = "writing event-channel";
		goto abort_transaction;
	}

	err = xenbus_printf(xbt, dev->nodename, "request-rx-copy", "%u",
			    info->copying_receiver);
	if (err) {
		message = "writing request-rx-copy";
		goto abort_transaction;
	}

	err = xenbus_printf(xbt, dev->nodename, "feature-rx-notify", "%d", 1);
	if (err) {
		message = "writing feature-rx-notify";
		goto abort_transaction;
	}

	err = xenbus_printf(xbt, dev->nodename, "feature-no-csum-offload",
			    "%d", !HAVE_CSUM_OFFLOAD);
	if (err) {
		message = "writing feature-no-csum-offload";
		goto abort_transaction;
	}

	err = xenbus_printf(xbt, dev->nodename, "feature-sg", "%d", 1);
	if (err) {
		message = "writing feature-sg";
		goto abort_transaction;
	}

	err = xenbus_printf(xbt, dev->nodename, "feature-gso-tcpv4", "%d",
			    HAVE_TSO);
	if (err) {
		message = "writing feature-gso-tcpv4";
		goto abort_transaction;
	}

	err = xenbus_transaction_end(xbt, 0);
	if (err) {
		if (err == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, err, "completing transaction");
		goto destroy_ring;
	}

	return 0;

 abort_transaction:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, err, "%s", message);
 destroy_ring:
	netfront_accelerator_call_remove(info, dev);
	netif_disconnect_backend(info);
 out:
	return err;
}

static int setup_device(struct xenbus_device *dev, struct netfront_info *info)
{
	struct netif_tx_sring *txs;
	struct netif_rx_sring *rxs;
	int err;
	struct net_device *netdev = info->netdev;

	info->tx_ring_ref = GRANT_INVALID_REF;
	info->rx_ring_ref = GRANT_INVALID_REF;
	info->rx.sring = NULL;
	info->tx.sring = NULL;
	info->irq = 0;

	txs = (struct netif_tx_sring *)get_zeroed_page(GFP_NOIO | __GFP_HIGH);
	if (!txs) {
		err = -ENOMEM;
		xenbus_dev_fatal(dev, err, "allocating tx ring page");
		goto fail;
	}
	SHARED_RING_INIT(txs);
	FRONT_RING_INIT(&info->tx, txs, PAGE_SIZE);

	err = xenbus_grant_ring(dev, virt_to_mfn(txs));
	if (err < 0) {
		free_page((unsigned long)txs);
		goto fail;
	}
	info->tx_ring_ref = err;

	rxs = (struct netif_rx_sring *)get_zeroed_page(GFP_NOIO | __GFP_HIGH);
	if (!rxs) {
		err = -ENOMEM;
		xenbus_dev_fatal(dev, err, "allocating rx ring page");
		goto fail;
	}
	SHARED_RING_INIT(rxs);
	FRONT_RING_INIT(&info->rx, rxs, PAGE_SIZE);

	err = xenbus_grant_ring(dev, virt_to_mfn(rxs));
	if (err < 0) {
		free_page((unsigned long)rxs);
		goto fail;
	}
	info->rx_ring_ref = err;

	memcpy(netdev->dev_addr, info->mac, ETH_ALEN);

	err = bind_listening_port_to_irqhandler(
		dev->otherend_id, netif_int, IRQF_SAMPLE_RANDOM, netdev->name,
		netdev);
	if (err < 0)
		goto fail;
	info->irq = err;

	return 0;

 fail:
	return err;
}

/**
 * Callback received when the backend's state changes.
 */
static void backend_changed(struct xenbus_device *dev,
			    enum xenbus_state backend_state)
{
	struct netfront_info *np = dev_get_drvdata(&dev->dev);
	struct net_device *netdev = np->netdev;

	DPRINTK("%s\n", xenbus_strstate(backend_state));

	switch (backend_state) {
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateUnknown:
	case XenbusStateClosed:
		break;

	case XenbusStateInitWait:
		if (dev->state != XenbusStateInitialising)
			break;
		if (network_connect(netdev) != 0)
			break;
		xenbus_switch_state(dev, XenbusStateConnected);
		break;

	case XenbusStateConnected:
	    (void)send_fake_arp(netdev, ARPOP_REQUEST);
	    (void)send_fake_arp(netdev, ARPOP_REPLY);
		break;

	case XenbusStateClosing:
		xenbus_frontend_closed(dev);
		break;
	}
}

/** Send a packet on a net device to encourage switches to learn the
 * MAC. We send a fake ARP request.
 *
 * @param dev device
 * @return 0 on success, error code otherwise
 */
static void send_fake_arp(struct net_device *dev, int arpType)
{
#ifdef CONFIG_INET
	struct sk_buff *skb;
	u32             src_ip, dst_ip;

	dst_ip = INADDR_BROADCAST;
	src_ip = inet_select_addr(dev, dst_ip, RT_SCOPE_LINK);

	/* No IP? Then nothing to do. */
	if (src_ip == 0)
		return;

	// creat GARP
	if ( ARPOP_REQUEST == arpType )
	{
		skb = arp_create(ARPOP_REQUEST, ETH_P_ARP,
			 src_ip, dev, src_ip,
			 /*dst_hw*/ NULL, /*src_hw*/ NULL,
			 /*target_hw*/ NULL);	
	}
	else
	{
		skb = arp_create(ARPOP_REPLY, ETH_P_ARP,
			 dst_ip, dev, src_ip,
			 /*dst_hw*/ NULL, /*src_hw*/ NULL,
			 /*target_hw*/ dev->dev_addr);
	}
	
	if (skb == NULL)
		return;

	dev_queue_xmit(skb);
#endif
}

static inline int netfront_tx_slot_available(struct netfront_info *np)
{
	return ((np->tx.req_prod_pvt - np->tx.rsp_cons) <
		(TX_MAX_TARGET - MAX_SKB_FRAGS - 2));
}


static inline void network_maybe_wake_tx(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);

	if (unlikely(netif_queue_stopped(dev)) &&
	    netfront_tx_slot_available(np) &&
	    likely(netif_running(dev)) &&
	    netfront_check_accelerator_queue_ready(dev, np))
		netif_wake_queue(dev);
}


int netfront_check_queue_ready(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);

	return unlikely(netif_queue_stopped(dev)) &&
		netfront_tx_slot_available(np) &&
		likely(netif_running(dev));
}
EXPORT_SYMBOL(netfront_check_queue_ready);


static int network_open(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);

	memset(&np->stats, 0, sizeof(np->stats));
	napi_enable(&np->napi);

	spin_lock_bh(&np->rx_lock);
	if (netfront_carrier_ok(np)) {
		network_alloc_rx_buffers(dev);
		np->rx.sring->rsp_event = np->rx.rsp_cons + 1;
		if (RING_HAS_UNCONSUMED_RESPONSES(&np->rx)){
			netfront_accelerator_call_stop_napi_irq(np, dev);

			napi_schedule(&np->napi);
		}
	}

	/**
	 * Description: 
	 *         When NIC device has been actived successfully, operstate which is 
	 *         member of netdev structure should be set to UP.
	 */
	np->netdev->operstate = (IF_OPER_DOWN != np->netdev->operstate) ?
                            IF_OPER_UP : IF_OPER_DOWN;

	spin_unlock_bh(&np->rx_lock);

	netif_start_queue(dev);

	return 0;
}

static void network_tx_buf_gc(struct net_device *dev)
{
	RING_IDX cons, prod;
	unsigned short id;
	struct netfront_info *np = netdev_priv(dev);
	struct sk_buff *skb;

	BUG_ON(!netfront_carrier_ok(np));

	do {
		prod = np->tx.sring->rsp_prod;
		rmb(); /* Ensure we see responses up to 'rp'. */

		for (cons = np->tx.rsp_cons; cons != prod; cons++) {
			struct netif_tx_response *txrsp;

			txrsp = RING_GET_RESPONSE(&np->tx, cons);
			if (txrsp->status == NETIF_RSP_NULL)
				continue;

			id  = txrsp->id;
			skb = np->tx_skbs[id];
			if (unlikely(gnttab_query_foreign_access(
				np->grant_tx_ref[id]) != 0)) {
				printk(KERN_ALERT "network_tx_buf_gc: warning "
				       "-- grant still in use by backend "
				       "domain.\n");
				BUG();
			}
			gnttab_end_foreign_access_ref(np->grant_tx_ref[id]);
			gnttab_release_grant_reference(
				&np->gref_tx_head, np->grant_tx_ref[id]);
			np->grant_tx_ref[id] = GRANT_INVALID_REF;
			add_id_to_freelist(np->tx_skbs, id);
			dev_kfree_skb_irq(skb);
		}

		np->tx.rsp_cons = prod;

		/*
		 * Set a new event, then check for race with update of tx_cons.
		 * Note that it is essential to schedule a callback, no matter
		 * how few buffers are pending. Even if there is space in the
		 * transmit ring, higher layers may be blocked because too much
		 * data is outstanding: in such cases notification from Xen is
		 * likely to be the only kick that we'll get.
		 */
		np->tx.sring->rsp_event =
			prod + ((np->tx.sring->req_prod - prod) >> 1) + 1;
		mb();
	} while ((cons == prod) && (prod != np->tx.sring->rsp_prod));

	network_maybe_wake_tx(dev);
}

static void rx_refill_timeout(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct netfront_info *np = netdev_priv(dev);

	netfront_accelerator_call_stop_napi_irq(np, dev);

	napi_schedule(&np->napi);
}

static void network_alloc_rx_buffers(struct net_device *dev)
{
	unsigned short id;
	struct netfront_info *np = netdev_priv(dev);
	struct sk_buff *skb;
	struct page *page;
	int i, batch_target, notify;
	RING_IDX req_prod = np->rx.req_prod_pvt;
	struct xen_memory_reservation reservation;
	grant_ref_t ref;
 	unsigned long pfn;
 	void *vaddr;
	int nr_flips;
	netif_rx_request_t *req;

	if (unlikely(!netfront_carrier_ok(np)))
		return;

	/*
	 * Allocate skbuffs greedily, even though we batch updates to the
	 * receive ring. This creates a less bursty demand on the memory
	 * allocator, so should reduce the chance of failed allocation requests
	 * both for ourself and for other kernel subsystems.
	 */
	batch_target = np->rx_target - (req_prod - np->rx.rsp_cons);
	for (i = skb_queue_len(&np->rx_batch); i < batch_target; i++) {
		/*
		 * Allocate an skb and a page. Do not use __dev_alloc_skb as
		 * that will allocate page-sized buffers which is not
		 * necessary here.
		 * 16 bytes added as necessary headroom for netif_receive_skb.
		 */
		skb = alloc_skb(RX_COPY_THRESHOLD + 16 + NET_IP_ALIGN,
				GFP_ATOMIC | __GFP_NOWARN);
		if (unlikely(!skb))
			goto no_skb;

		page = alloc_page(GFP_ATOMIC | __GFP_NOWARN);
		if (!page) {
			kfree_skb(skb);
no_skb:
			/* Could not allocate any skbuffs. Try again later. */
			mod_timer(&np->rx_refill_timer,
				  jiffies + (HZ/10));
			/* Any skbuffs queued for refill? Force them out. */
			if (i != 0)
				goto refill;
			break;
		}

		skb_reserve(skb, 16 + NET_IP_ALIGN); /* mimic dev_alloc_skb() */
		skb_shinfo(skb)->frags[0].page = page;
		skb_shinfo(skb)->nr_frags = 1;
		__skb_queue_tail(&np->rx_batch, skb);
	}

	/* Is the batch large enough to be worthwhile? */
	if (i < (np->rx_target/2)) {
		if (req_prod > np->rx.sring->req_prod)
			goto push;
		return;
	}

	/* Adjust our fill target if we risked running out of buffers. */
	if (((req_prod - np->rx.sring->rsp_prod) < (np->rx_target / 4)) &&
	    ((np->rx_target *= 2) > np->rx_max_target))
		np->rx_target = np->rx_max_target;

 refill:
	for (nr_flips = i = 0; ; i++) {
		if ((skb = __skb_dequeue(&np->rx_batch)) == NULL)
			break;

		skb->dev = dev;

		id = xennet_rxidx(req_prod + i);

		BUG_ON(np->rx_skbs[id]);
		np->rx_skbs[id] = skb;

		ref = gnttab_claim_grant_reference(&np->gref_rx_head);
		BUG_ON((signed short)ref < 0);
		np->grant_rx_ref[id] = ref;

		pfn = page_to_pfn(skb_shinfo(skb)->frags[0].page);
		vaddr = page_address(skb_shinfo(skb)->frags[0].page);

		req = RING_GET_REQUEST(&np->rx, req_prod + i);
		if (!np->copying_receiver) {
			gnttab_grant_foreign_transfer_ref(ref,
							  np->xbdev->otherend_id,
							  pfn);
			np->rx_pfn_array[nr_flips] = pfn_to_mfn(pfn);
			if (!xen_feature(XENFEAT_auto_translated_physmap)) {
				/* Remove this page before passing
				 * back to Xen. */
				set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
				MULTI_update_va_mapping(np->rx_mcl+i,
							(unsigned long)vaddr,
							__pte(0), 0);
			}
			nr_flips++;
		} else {
			gnttab_grant_foreign_access_ref(ref,
							np->xbdev->otherend_id,
							pfn_to_mfn(pfn),
							0);
		}

		req->id = id;
		req->gref = ref;
	}

	if ( nr_flips != 0 ) {
		/* Tell the ballon driver what is going on. */
		balloon_update_driver_allowance(i);

		set_xen_guest_handle(reservation.extent_start,
				     np->rx_pfn_array);
		reservation.nr_extents   = nr_flips;
		reservation.extent_order = 0;
		reservation.address_bits = 0;
		reservation.domid        = DOMID_SELF;

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			/* After all PTEs have been zapped, flush the TLB. */
			np->rx_mcl[i-1].args[MULTI_UVMFLAGS_INDEX] =
				UVMF_TLB_FLUSH|UVMF_ALL;

			/* Give away a batch of pages. */
			np->rx_mcl[i].op = __HYPERVISOR_memory_op;
			np->rx_mcl[i].args[0] = XENMEM_decrease_reservation;
			np->rx_mcl[i].args[1] = (unsigned long)&reservation;

			/* Zap PTEs and give away pages in one big
			 * multicall. */
			if (unlikely(HYPERVISOR_multicall(np->rx_mcl, i+1)))
				BUG();

			/* Check return status of HYPERVISOR_memory_op(). */
			if (unlikely(np->rx_mcl[i].result != i))
				panic("Unable to reduce memory reservation\n");
			while (nr_flips--)
				BUG_ON(np->rx_mcl[nr_flips].result);
		} else {
			if (HYPERVISOR_memory_op(XENMEM_decrease_reservation,
						 &reservation) != i)
				panic("Unable to reduce memory reservation\n");
		}
	} else {
		wmb();
	}

	/* Above is a suitable barrier to ensure backend will see requests. */
	np->rx.req_prod_pvt = req_prod + i;
 push:
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&np->rx, notify);
	if (notify)
		notify_remote_via_irq(np->irq);
}

static void xennet_make_frags(struct sk_buff *skb, struct net_device *dev,
			      struct netif_tx_request *tx)
{
	struct netfront_info *np = netdev_priv(dev);
	char *data = skb->data;
	unsigned long mfn;
	RING_IDX prod = np->tx.req_prod_pvt;
	int frags = skb_shinfo(skb)->nr_frags;
	unsigned int offset = offset_in_page(data);
	unsigned int len = skb_headlen(skb);
	unsigned int id;
	grant_ref_t ref;
	int i;

	while (len > PAGE_SIZE - offset) {
		tx->size = PAGE_SIZE - offset;
		tx->flags |= NETTXF_more_data;
		len -= tx->size;
		data += tx->size;
		offset = 0;

		id = get_id_from_freelist(np->tx_skbs);
		np->tx_skbs[id] = skb_get(skb);
		tx = RING_GET_REQUEST(&np->tx, prod++);
		tx->id = id;
		ref = gnttab_claim_grant_reference(&np->gref_tx_head);
		BUG_ON((signed short)ref < 0);

		mfn = virt_to_mfn(data);
		gnttab_grant_foreign_access_ref(ref, np->xbdev->otherend_id,
						mfn, GTF_readonly);

		tx->gref = np->grant_tx_ref[id] = ref;
		tx->offset = offset;
		tx->size = len;
		tx->flags = 0;
	}

	for (i = 0; i < frags; i++) {
		skb_frag_t *frag = skb_shinfo(skb)->frags + i;

		tx->flags |= NETTXF_more_data;

		id = get_id_from_freelist(np->tx_skbs);
		np->tx_skbs[id] = skb_get(skb);
		tx = RING_GET_REQUEST(&np->tx, prod++);
		tx->id = id;
		ref = gnttab_claim_grant_reference(&np->gref_tx_head);
		BUG_ON((signed short)ref < 0);

		mfn = pfn_to_mfn(page_to_pfn(frag->page));
		gnttab_grant_foreign_access_ref(ref, np->xbdev->otherend_id,
						mfn, GTF_readonly);

		tx->gref = np->grant_tx_ref[id] = ref;
		tx->offset = frag->page_offset;
		tx->size = frag->size;
		tx->flags = 0;
	}

	np->tx.req_prod_pvt = prod;
}

static int network_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	unsigned short id;
	struct netfront_info *np = netdev_priv(dev);
	struct netif_tx_request *tx;
	struct netif_extra_info *extra;
	char *data = skb->data;
	RING_IDX i;
	grant_ref_t ref;
	unsigned long mfn;
	int notify;
	int frags = skb_shinfo(skb)->nr_frags;
	unsigned int offset = offset_in_page(data);
	unsigned int len = skb_headlen(skb);

	/* Check the fast path, if hooks are available */
 	if (np->accel_vif_state.hooks && 
 	    np->accel_vif_state.hooks->start_xmit(skb, dev)) { 
 		/* Fast path has sent this packet */ 
 		return NETDEV_TX_OK;
 	} 

	frags += DIV_ROUND_UP(offset + len, PAGE_SIZE);
	if (unlikely(frags > MAX_SKB_FRAGS + 1)) {
		printk(KERN_ALERT "xennet: skb rides the rocket: %d frags\n",
		       frags);
		dump_stack();
		goto drop;
	}

	spin_lock_irq(&np->tx_lock);

	if (unlikely(!netfront_carrier_ok(np) ||
		     (frags > 1 && !xennet_can_sg(dev)) ||
		     netif_needs_gso(dev, skb))) {
		spin_unlock_irq(&np->tx_lock);
		goto drop;
	}

	i = np->tx.req_prod_pvt;

	id = get_id_from_freelist(np->tx_skbs);
	np->tx_skbs[id] = skb;

	tx = RING_GET_REQUEST(&np->tx, i);

	tx->id   = id;
	ref = gnttab_claim_grant_reference(&np->gref_tx_head);
	BUG_ON((signed short)ref < 0);
	mfn = virt_to_mfn(data);
	gnttab_grant_foreign_access_ref(
		ref, np->xbdev->otherend_id, mfn, GTF_readonly);
	tx->gref = np->grant_tx_ref[id] = ref;
	tx->offset = offset;
	tx->size = len;

	tx->flags = 0;
	extra = NULL;

	if (skb->ip_summed == CHECKSUM_PARTIAL) /* local packet? */
		tx->flags |= NETTXF_csum_blank | NETTXF_data_validated;
#ifdef CONFIG_XEN
	if (skb->proto_data_valid) /* remote but checksummed? */
		tx->flags |= NETTXF_data_validated;
#endif

#if HAVE_TSO
	if (skb_shinfo(skb)->gso_size) {
		struct netif_extra_info *gso = (struct netif_extra_info *)
			RING_GET_REQUEST(&np->tx, ++i);

		if (extra)
			extra->flags |= XEN_NETIF_EXTRA_FLAG_MORE;
		else
			tx->flags |= NETTXF_extra_info;

		gso->u.gso.size = skb_shinfo(skb)->gso_size;
		gso->u.gso.type = XEN_NETIF_GSO_TYPE_TCPV4;
		gso->u.gso.pad = 0;
		gso->u.gso.features = 0;

		gso->type = XEN_NETIF_EXTRA_TYPE_GSO;
		gso->flags = 0;
		extra = gso;
	}
#endif

	np->tx.req_prod_pvt = i + 1;

	xennet_make_frags(skb, dev, tx);
	tx->size = skb->len;

	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&np->tx, notify);
	if (notify)
		notify_remote_via_irq(np->irq);

	np->stats.tx_bytes += skb->len;
	np->stats.tx_packets++;
	dev->trans_start = jiffies;

	/* Note: It is not safe to access skb after network_tx_buf_gc()! */
	network_tx_buf_gc(dev);

	if (!netfront_tx_slot_available(np))
		netif_stop_queue(dev);

	spin_unlock_irq(&np->tx_lock);

	return NETDEV_TX_OK;

 drop:
	np->stats.tx_dropped++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static irqreturn_t netif_int(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct netfront_info *np = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&np->tx_lock, flags);

	if (likely(netfront_carrier_ok(np))) {
		network_tx_buf_gc(dev);
		/* Under tx_lock: protects access to rx shared-ring indexes. */
		if (RING_HAS_UNCONSUMED_RESPONSES(&np->rx)) {
			netfront_accelerator_call_stop_napi_irq(np, dev);

			napi_schedule(&np->napi);
		}
	}

	spin_unlock_irqrestore(&np->tx_lock, flags);

	return IRQ_HANDLED;
}

static void xennet_move_rx_slot(struct netfront_info *np, struct sk_buff *skb,
				grant_ref_t ref)
{
	int new = xennet_rxidx(np->rx.req_prod_pvt);

	BUG_ON(np->rx_skbs[new]);
	np->rx_skbs[new] = skb;
	np->grant_rx_ref[new] = ref;
	RING_GET_REQUEST(&np->rx, np->rx.req_prod_pvt)->id = new;
	RING_GET_REQUEST(&np->rx, np->rx.req_prod_pvt)->gref = ref;
	np->rx.req_prod_pvt++;
}

int xennet_get_extras(struct netfront_info *np,
		      struct netif_extra_info *extras, RING_IDX rp)

{
	struct netif_extra_info *extra;
	RING_IDX cons = np->rx.rsp_cons;
	int err = 0;

	do {
		struct sk_buff *skb;
		grant_ref_t ref;

		if (unlikely(cons + 1 == rp)) {
			if (net_ratelimit())
				WPRINTK("Missing extra info\n");
			err = -EBADR;
			break;
		}

		extra = (struct netif_extra_info *)
			RING_GET_RESPONSE(&np->rx, ++cons);

		if (unlikely(!extra->type ||
			     extra->type >= XEN_NETIF_EXTRA_TYPE_MAX)) {
			if (net_ratelimit())
				WPRINTK("Invalid extra type: %d\n",
					extra->type);
			err = -EINVAL;
		} else {
			memcpy(&extras[extra->type - 1], extra,
			       sizeof(*extra));
		}

		skb = xennet_get_rx_skb(np, cons);
		ref = xennet_get_rx_ref(np, cons);
		xennet_move_rx_slot(np, skb, ref);
	} while (extra->flags & XEN_NETIF_EXTRA_FLAG_MORE);

	np->rx.rsp_cons = cons;
	return err;
}

static int xennet_get_responses(struct netfront_info *np,
				struct netfront_rx_info *rinfo, RING_IDX rp,
				struct sk_buff_head *list,
				int *pages_flipped_p)
{
	int pages_flipped = *pages_flipped_p;
	struct mmu_update *mmu;
	struct multicall_entry *mcl;
	struct netif_rx_response *rx = &rinfo->rx;
	struct netif_extra_info *extras = rinfo->extras;
	RING_IDX cons = np->rx.rsp_cons;
	struct sk_buff *skb = xennet_get_rx_skb(np, cons);
	grant_ref_t ref = xennet_get_rx_ref(np, cons);
	int max = MAX_SKB_FRAGS + (rx->status <= RX_COPY_THRESHOLD);
	int frags = 1;
	int err = 0;
	unsigned long ret;

	if (rx->flags & NETRXF_extra_info) {
		err = xennet_get_extras(np, extras, rp);
		cons = np->rx.rsp_cons;
	}

	for (;;) {
		unsigned long mfn;

		if (unlikely(rx->status < 0 ||
			     rx->offset + rx->status > PAGE_SIZE)) {
			if (net_ratelimit())
				WPRINTK("rx->offset: %x, size: %u\n",
					rx->offset, rx->status);
			xennet_move_rx_slot(np, skb, ref);
			err = -EINVAL;
			goto next;
		}

		/*
		 * This definitely indicates a bug, either in this driver or in
		 * the backend driver. In future this should flag the bad
		 * situation to the system controller to reboot the backed.
		 */
		if (ref == GRANT_INVALID_REF) {
			if (net_ratelimit())
				WPRINTK("Bad rx response id %d.\n", rx->id);
			err = -EINVAL;
			goto next;
		}

		if (!np->copying_receiver) {
			/* Memory pressure, insufficient buffer
			 * headroom, ... */
			if (!(mfn = gnttab_end_foreign_transfer_ref(ref))) {
				if (net_ratelimit())
					WPRINTK("Unfulfilled rx req "
						"(id=%d, st=%d).\n",
						rx->id, rx->status);
				xennet_move_rx_slot(np, skb, ref);
				err = -ENOMEM;
				goto next;
			}

			if (!xen_feature(XENFEAT_auto_translated_physmap)) {
				/* Remap the page. */
				struct page *page =
					skb_shinfo(skb)->frags[0].page;
				unsigned long pfn = page_to_pfn(page);
				void *vaddr = page_address(page);

				mcl = np->rx_mcl + pages_flipped;
				mmu = np->rx_mmu + pages_flipped;

				MULTI_update_va_mapping(mcl,
							(unsigned long)vaddr,
							pfn_pte_ma(mfn,
								   PAGE_KERNEL),
							0);
				mmu->ptr = ((maddr_t)mfn << PAGE_SHIFT)
					| MMU_MACHPHYS_UPDATE;
				mmu->val = pfn;

				set_phys_to_machine(pfn, mfn);
			}
			pages_flipped++;
		} else {
			ret = gnttab_end_foreign_access_ref(ref);
			BUG_ON(!ret);
		}

		gnttab_release_grant_reference(&np->gref_rx_head, ref);

		__skb_queue_tail(list, skb);

next:
		if (!(rx->flags & NETRXF_more_data))
			break;

		if (cons + frags == rp) {
			if (net_ratelimit())
				WPRINTK("Need more frags\n");
			err = -ENOENT;
			break;
		}

		rx = RING_GET_RESPONSE(&np->rx, cons + frags);
		skb = xennet_get_rx_skb(np, cons + frags);
		ref = xennet_get_rx_ref(np, cons + frags);
		frags++;
	}

	if (unlikely(frags > max)) {
		if (net_ratelimit())
			WPRINTK("Too many frags\n");
		err = -E2BIG;
	}

	if (unlikely(err))
		np->rx.rsp_cons = cons + frags;

	*pages_flipped_p = pages_flipped;

	return err;
}

static RING_IDX xennet_fill_frags(struct netfront_info *np,
				  struct sk_buff *skb,
				  struct sk_buff_head *list)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	RING_IDX cons = np->rx.rsp_cons;
	skb_frag_t *frag = shinfo->frags + nr_frags;
	struct sk_buff *nskb;

	while ((nskb = __skb_dequeue(list))) {
		struct netif_rx_response *rx =
			RING_GET_RESPONSE(&np->rx, ++cons);

		frag->page = skb_shinfo(nskb)->frags[0].page;
		frag->page_offset = rx->offset;
		frag->size = rx->status;

		skb->data_len += rx->status;

		skb_shinfo(nskb)->nr_frags = 0;
		kfree_skb(nskb);

		frag++;
		nr_frags++;
	}

	shinfo->nr_frags = nr_frags;
	return cons;
}

static int xennet_set_skb_gso(struct sk_buff *skb,
			      struct netif_extra_info *gso)
{
	if (!gso->u.gso.size) {
		if (net_ratelimit())
			WPRINTK("GSO size must not be zero.\n");
		return -EINVAL;
	}

	/* Currently only TCPv4 S.O. is supported. */
	if (gso->u.gso.type != XEN_NETIF_GSO_TYPE_TCPV4) {
		if (net_ratelimit())
			WPRINTK("Bad GSO type %d.\n", gso->u.gso.type);
		return -EINVAL;
	}

#if HAVE_TSO
	skb_shinfo(skb)->gso_size = gso->u.gso.size;
#if HAVE_GSO
	skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;

	/* Header must be checked, and gso_segs computed. */
	skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;
#endif
	skb_shinfo(skb)->gso_segs = 0;

	return 0;
#else
	if (net_ratelimit())
		WPRINTK("GSO unsupported by this kernel.\n");
	return -EINVAL;
#endif
}

static int xen_skb_checksum_setup(struct sk_buff *skb)
{
	struct iphdr *iph;
	unsigned char *th;
	int err = -EPROTO;

	if (skb->protocol != htons(ETH_P_IP))
		goto out;

	iph = (void *)skb->data;
	th = skb->data + 4 * iph->ihl;
	if (th >= skb_tail_pointer(skb))
		goto out;

	skb->csum_start = th - skb->head;
	switch (iph->protocol) {
	case IPPROTO_TCP:
		skb->csum_offset = offsetof(struct tcphdr, check);
		break;
	case IPPROTO_UDP:
		skb->csum_offset = offsetof(struct udphdr, check);
		break;
	default:
		if (net_ratelimit())
			printk(KERN_ERR "Attempting to checksum a non-"
			       "TCP/UDP packet, dropping a protocol"
			       " %d packet", iph->protocol);
		goto out;
	}

	if ((th + skb->csum_offset + 2) > skb_tail_pointer(skb))
		goto out;

	err = 0;

out:
	return err;
}

static int netif_poll(struct napi_struct *napi, int budget)
{
	struct netfront_info *np = container_of(napi, struct netfront_info, napi);
	struct net_device *dev = np->netdev;
	struct sk_buff *skb;
	struct netfront_rx_info rinfo;
	struct netif_rx_response *rx = &rinfo.rx;
	struct netif_extra_info *extras = rinfo.extras;
	RING_IDX i, rp;
	struct multicall_entry *mcl;
	int work_done, more_to_do = 1, accel_more_to_do = 1;
	struct sk_buff_head rxq;
	struct sk_buff_head errq;
	struct sk_buff_head tmpq;
	unsigned long flags;
	unsigned int len;
	int pages_flipped = 0;
	int err;

	spin_lock(&np->rx_lock); /* no need for spin_lock_bh() in ->poll() */

	if (unlikely(!netfront_carrier_ok(np))) {
		spin_unlock(&np->rx_lock);
		return 0;
	}

	skb_queue_head_init(&rxq);
	skb_queue_head_init(&errq);
	skb_queue_head_init(&tmpq);

	rp = np->rx.sring->rsp_prod;
	rmb(); /* Ensure we see queued responses up to 'rp'. */

	i = np->rx.rsp_cons;
	work_done = 0;
	while ((i != rp) && (work_done < budget)) {
		memcpy(rx, RING_GET_RESPONSE(&np->rx, i), sizeof(*rx));
		memset(extras, 0, sizeof(rinfo.extras));

		err = xennet_get_responses(np, &rinfo, rp, &tmpq,
					   &pages_flipped);

		if (unlikely(err)) {
err:	
			while ((skb = __skb_dequeue(&tmpq)))
				__skb_queue_tail(&errq, skb);
			np->stats.rx_errors++;
			i = np->rx.rsp_cons;
			continue;
		}

		skb = __skb_dequeue(&tmpq);

		if (extras[XEN_NETIF_EXTRA_TYPE_GSO - 1].type) {
			struct netif_extra_info *gso;
			gso = &extras[XEN_NETIF_EXTRA_TYPE_GSO - 1];

			if (unlikely(xennet_set_skb_gso(skb, gso))) {
				__skb_queue_head(&tmpq, skb);
				np->rx.rsp_cons += skb_queue_len(&tmpq);
				goto err;
			}
		}

		NETFRONT_SKB_CB(skb)->page = skb_shinfo(skb)->frags[0].page;
		NETFRONT_SKB_CB(skb)->offset = rx->offset;

		len = rx->status;
		if (len > RX_COPY_THRESHOLD)
			len = RX_COPY_THRESHOLD;
		skb_put(skb, len);

		if (rx->status > len) {
			skb_shinfo(skb)->frags[0].page_offset =
				rx->offset + len;
			skb_shinfo(skb)->frags[0].size = rx->status - len;
			skb->data_len = rx->status - len;
		} else {
			skb_shinfo(skb)->frags[0].page = NULL;
			skb_shinfo(skb)->nr_frags = 0;
		}

		i = xennet_fill_frags(np, skb, &tmpq);

		/*
		 * Truesize must approximates the size of true data plus
		 * any supervisor overheads. Adding hypervisor overheads
		 * has been shown to significantly reduce achievable
		 * bandwidth with the default receive buffer size. It is
		 * therefore not wise to account for it here.
		 *
		 * After alloc_skb(RX_COPY_THRESHOLD), truesize is set to
		 * RX_COPY_THRESHOLD + the supervisor overheads. Here, we
		 * add the size of the data pulled in xennet_fill_frags().
		 *
		 * We also adjust for any unused space in the main data
		 * area by subtracting (RX_COPY_THRESHOLD - len). This is
		 * especially important with drivers which split incoming
		 * packets into header and data, using only 66 bytes of
		 * the main data area (see the e1000 driver for example.)
		 * On such systems, without this last adjustement, our
		 * achievable receive throughout using the standard receive
		 * buffer size was cut by 25%(!!!).
		 */
		skb->truesize += skb->data_len - (RX_COPY_THRESHOLD - len);
		skb->len += skb->data_len;

		/*
		 * Old backends do not assert data_validated but we
		 * can infer it from csum_blank so test both flags.
		 */

		if (rx->flags & NETRXF_csum_blank)
			skb->ip_summed = CHECKSUM_PARTIAL;
		else if (rx->flags & NETRXF_data_validated)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb->ip_summed = CHECKSUM_NONE;
#ifdef CONFIG_XEN
		skb->proto_data_valid = (skb->ip_summed != CHECKSUM_NONE);
		skb->proto_csum_blank = !!(rx->flags & NETRXF_csum_blank);
#endif
		np->stats.rx_packets++;
		np->stats.rx_bytes += skb->len;

		__skb_queue_tail(&rxq, skb);

		np->rx.rsp_cons = ++i;
		work_done++;
	}

	if (pages_flipped) {
		/* Some pages are no longer absent... */
		balloon_update_driver_allowance(-pages_flipped);

		/* Do all the remapping work and M2P updates. */
		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			mcl = np->rx_mcl + pages_flipped;
			mcl->op = __HYPERVISOR_mmu_update;
			mcl->args[0] = (unsigned long)np->rx_mmu;
			mcl->args[1] = pages_flipped;
			mcl->args[2] = 0;
			mcl->args[3] = DOMID_SELF;
			err = HYPERVISOR_multicall_check(np->rx_mcl,
							 pages_flipped + 1,
							 NULL);
			BUG_ON(err);
		}
	}

	__skb_queue_purge(&errq);

	while ((skb = __skb_dequeue(&rxq)) != NULL) {
		struct page *page = NETFRONT_SKB_CB(skb)->page;
		void *vaddr = page_address(page);
		unsigned offset = NETFRONT_SKB_CB(skb)->offset;

		memcpy(skb->data, vaddr + offset, skb_headlen(skb));

		if (page != skb_shinfo(skb)->frags[0].page)
			__free_page(page);

		/* Ethernet work: Delayed to here as it peeks the header. */
		skb->protocol = eth_type_trans(skb, dev);

		if (skb->ip_summed == CHECKSUM_PARTIAL) {
			if (xen_skb_checksum_setup(skb)) {
				kfree_skb(skb);
				np->stats.rx_errors++;
				continue;
			}
		}

		/* Pass it up. */
		netif_receive_skb(skb);
	}

	/* If we get a callback with very few responses, reduce fill target. */
	/* NB. Note exponential increase, linear decrease. */
	if (((np->rx.req_prod_pvt - np->rx.sring->rsp_prod) >
	     ((3*np->rx_target) / 4)) &&
	    (--np->rx_target < np->rx_min_target))
		np->rx_target = np->rx_min_target;

	network_alloc_rx_buffers(dev);

	if (work_done < budget) {
		/* there's some spare capacity, try the accelerated path */
		int accel_budget = budget - work_done;
		int accel_budget_start = accel_budget;

 		if (np->accel_vif_state.hooks) { 
 			accel_more_to_do =  
 				np->accel_vif_state.hooks->netdev_poll 
 				(dev, &accel_budget); 
 			work_done += (accel_budget_start - accel_budget); 
 		} else
			accel_more_to_do = 0;
	}

	if (work_done < budget) {
		local_irq_save(flags);

		RING_FINAL_CHECK_FOR_RESPONSES(&np->rx, more_to_do);

		if (!more_to_do && !accel_more_to_do && 
		    np->accel_vif_state.hooks) {
			/* 
			 *  Slow path has nothing more to do, see if
			 *  fast path is likewise
			 */
			accel_more_to_do = 
				np->accel_vif_state.hooks->start_napi_irq(dev);
		}

		if (!more_to_do && !accel_more_to_do)
			__napi_complete(napi);

		local_irq_restore(flags);
	}

	spin_unlock(&np->rx_lock);
	
	return work_done;
}

static void netif_release_tx_bufs(struct netfront_info *np)
{
	struct sk_buff *skb;
	int i;

	for (i = 1; i <= NET_TX_RING_SIZE; i++) {
		if ((unsigned long)np->tx_skbs[i] < PAGE_OFFSET)
			continue;

		skb = np->tx_skbs[i];
		gnttab_end_foreign_access_ref(np->grant_tx_ref[i]);
		gnttab_release_grant_reference(
			&np->gref_tx_head, np->grant_tx_ref[i]);
		np->grant_tx_ref[i] = GRANT_INVALID_REF;
		add_id_to_freelist(np->tx_skbs, i);
		dev_kfree_skb_irq(skb);
	}
}

static void netif_release_rx_bufs_flip(struct netfront_info *np)
{
	struct mmu_update      *mmu = np->rx_mmu;
	struct multicall_entry *mcl = np->rx_mcl;
	struct sk_buff_head free_list;
	struct sk_buff *skb;
	unsigned long mfn;
	int xfer = 0, noxfer = 0, unused = 0;
	int id, ref, rc;

	skb_queue_head_init(&free_list);

	spin_lock_bh(&np->rx_lock);

	for (id = 0; id < NET_RX_RING_SIZE; id++) {
		if ((ref = np->grant_rx_ref[id]) == GRANT_INVALID_REF) {
			unused++;
			continue;
		}

		skb = np->rx_skbs[id];
		mfn = gnttab_end_foreign_transfer_ref(ref);
		gnttab_release_grant_reference(&np->gref_rx_head, ref);
		np->grant_rx_ref[id] = GRANT_INVALID_REF;
		add_id_to_freelist(np->rx_skbs, id);

		if (0 == mfn) {
			struct page *page = skb_shinfo(skb)->frags[0].page;
			balloon_release_driver_page(page);
			skb_shinfo(skb)->nr_frags = 0;
			dev_kfree_skb(skb);
			noxfer++;
			continue;
		}

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			/* Remap the page. */
			struct page *page = skb_shinfo(skb)->frags[0].page;
			unsigned long pfn = page_to_pfn(page);
			void *vaddr = page_address(page);

			MULTI_update_va_mapping(mcl, (unsigned long)vaddr,
						pfn_pte_ma(mfn, PAGE_KERNEL),
						0);
			mcl++;
			mmu->ptr = ((maddr_t)mfn << PAGE_SHIFT)
				| MMU_MACHPHYS_UPDATE;
			mmu->val = pfn;
			mmu++;

			set_phys_to_machine(pfn, mfn);
		}
		__skb_queue_tail(&free_list, skb);
		xfer++;
	}

	DPRINTK("%s: %d xfer, %d noxfer, %d unused\n",
		__FUNCTION__, xfer, noxfer, unused);

	if (xfer) {
		/* Some pages are no longer absent... */
		balloon_update_driver_allowance(-xfer);

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			/* Do all the remapping work and M2P updates. */
			mcl->op = __HYPERVISOR_mmu_update;
			mcl->args[0] = (unsigned long)np->rx_mmu;
			mcl->args[1] = mmu - np->rx_mmu;
			mcl->args[2] = 0;
			mcl->args[3] = DOMID_SELF;
			mcl++;
			rc = HYPERVISOR_multicall_check(
				np->rx_mcl, mcl - np->rx_mcl, NULL);
			BUG_ON(rc);
		}
	}

	__skb_queue_purge(&free_list);

	spin_unlock_bh(&np->rx_lock);
}

static void netif_release_rx_bufs_copy(struct netfront_info *np)
{
	struct sk_buff *skb;
	int i, ref;
	int busy = 0, inuse = 0;

	spin_lock_bh(&np->rx_lock);

	for (i = 0; i < NET_RX_RING_SIZE; i++) {
		ref = np->grant_rx_ref[i];

		if (ref == GRANT_INVALID_REF)
			continue;

		inuse++;

		skb = np->rx_skbs[i];

		if (!gnttab_end_foreign_access_ref(ref))
		{
			busy++;
			continue;
		}

		gnttab_release_grant_reference(&np->gref_rx_head, ref);
		np->grant_rx_ref[i] = GRANT_INVALID_REF;
		add_id_to_freelist(np->rx_skbs, i);

		dev_kfree_skb(skb);
	}

	if (busy)
		DPRINTK("%s: Unable to release %d of %d inuse grant references out of %ld total.\n",
			__FUNCTION__, busy, inuse, NET_RX_RING_SIZE);

	spin_unlock_bh(&np->rx_lock);
}

static int network_close(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);
	netif_stop_queue(np->netdev);
	napi_disable(&np->napi);
	return 0;
}


static struct net_device_stats *network_get_stats(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);

	netfront_accelerator_call_get_stats(np, dev);
	return &np->stats;
}

static int xennet_set_mac_address(struct net_device *dev, void *p)
{
	struct netfront_info *np = netdev_priv(dev);
	struct sockaddr *addr = p;

	if (netif_running(dev))
		return -EBUSY;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	memcpy(np->mac, addr->sa_data, ETH_ALEN);

	return 0;
}

static int xennet_change_mtu(struct net_device *dev, int mtu)
{
	int max = xennet_can_sg(dev) ? 65535 - ETH_HLEN : ETH_DATA_LEN;

	if (mtu > max)
		return -EINVAL;
	dev->mtu = mtu;
	return 0;
}

static int xennet_set_sg(struct net_device *dev, u32 data)
{
	if (data) {
		struct netfront_info *np = netdev_priv(dev);
		int val;

		if (xenbus_scanf(XBT_NIL, np->xbdev->otherend, "feature-sg",
				 "%d", &val) < 0)
			val = 0;
		if (!val)
			return -ENOSYS;
	} else if (dev->mtu > ETH_DATA_LEN)
		dev->mtu = ETH_DATA_LEN;

	return ethtool_op_set_sg(dev, data);
}

static int xennet_set_tso(struct net_device *dev, u32 data)
{
	if (data) {
		struct netfront_info *np = netdev_priv(dev);
		int val;

		if (xenbus_scanf(XBT_NIL, np->xbdev->otherend,
				 "feature-gso-tcpv4", "%d", &val) < 0)
			val = 0;
		if (!val)
			return -ENOSYS;
	}

	return ethtool_op_set_tso(dev, data);
}

static void xennet_set_features(struct net_device *dev)
{
	dev_disable_gso_features(dev);
	xennet_set_sg(dev, 0);

	/* We need checksum offload to enable scatter/gather and TSO. */
	if (!(dev->features & NETIF_F_IP_CSUM))
		return;

	if (xennet_set_sg(dev, 1))
		return;

	/* Before 2.6.9 TSO seems to be unreliable so do not enable it
	 * on older kernels.
	 */
	if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,9))
		xennet_set_tso(dev, 1);
}

/**
 * Description: 
 *         Get device-specific settings
 * Version: 
 */
static int netfront_get_settings(struct net_device *dev,
	                         struct ethtool_cmd *ecmd)
{
    struct netfront_info *adapter = netdev_priv(dev);
    struct netfront_hw   *hw = &adapter->hw;
    u32    link_speed = 0;
    bool   link_up    = 1;

    ecmd->supported = SUPPORTED_1000baseT_Full;
    ecmd->transceiver = XCVR_EXTERNAL;
    ecmd->supported |= SUPPORTED_FIBRE;
    ecmd->advertising = (ADVERTISED_1000baseT_Full |
                                 ADVERTISED_FIBRE);
    ecmd->port = PORT_FIBRE;
    ecmd->autoneg = AUTONEG_DISABLE;

    if (link_up) {
        ecmd->speed = (link_speed == NETFRONT_LINK_SPEED_10GB_FULL) ?
                              SPEED_10000 : SPEED_1000;
                              ecmd->duplex = DUPLEX_FULL;
    } else {
        ecmd->speed = -1;
        ecmd->duplex = -1;
    }

    return 0;
}

/**
 * Description: 
 *         Report whether Wake-on-Lan is enabled
 */
static void netfront_get_wol(struct net_device *dev,
	                     struct ethtool_wolinfo *wol)
{
    struct netfront_info *adapter = netdev_priv(dev);

    wol->supported = WAKE_UCAST | WAKE_MCAST |
	             WAKE_BCAST | WAKE_MAGIC;
    wol->wolopts   = 0;

    if (adapter->wol & NETFRONT_WUFC_EX)
	wol->wolopts |= WAKE_UCAST;
    if (adapter->wol & NETFRONT_WUFC_MC)
	wol->wolopts |= WAKE_MCAST;
    if (adapter->wol & NETFRONT_WUFC_BC)
	wol->wolopts |= WAKE_BCAST;
    if (adapter->wol & NETFRONT_WUFC_MAG)
	wol->wolopts |= WAKE_MAGIC;

    return;
}

/**
 * Description: 
 *         Report driver message level
 */
static u32 netfront_get_msglevel(struct net_device *dev)
{
    struct netfront_info *adapter = netdev_priv(dev);

    adapter->msg_enable = (1 << DEFAULT_DEBUG_LEVEL_SHIFT) - 1;

    return adapter->msg_enable;
}

/* to support "ethtool -i" for getting driver information*/
static void netfront_get_drvinfo(struct net_device *dev,
				 struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "netfront");
	strcpy(info->version, "1.0");
	strcpy(info->bus_info, dev_name(dev->dev.parent));
}

static int network_connect(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);
	int i, requeue_idx, err;
	struct sk_buff *skb;
	grant_ref_t ref;
	netif_rx_request_t *req;
	unsigned int feature_rx_copy, feature_rx_flip;

	err = xenbus_scanf(XBT_NIL, np->xbdev->otherend,
			   "feature-rx-copy", "%u", &feature_rx_copy);
	if (err != 1)
		feature_rx_copy = 0;
	err = xenbus_scanf(XBT_NIL, np->xbdev->otherend,
			   "feature-rx-flip", "%u", &feature_rx_flip);
	if (err != 1)
		feature_rx_flip = 1;

	/*
	 * Copy packets on receive path if:
	 *  (a) This was requested by user, and the backend supports it; or
	 *  (b) Flipping was requested, but this is unsupported by the backend.
	 */
	np->copying_receiver = ((MODPARM_rx_copy && feature_rx_copy) ||
				(MODPARM_rx_flip && !feature_rx_flip));

	err = talk_to_backend(np->xbdev, np);
	if (err)
		return err;

	xennet_set_features(dev);

	DPRINTK("device %s has %sing receive path.\n",
		dev->name, np->copying_receiver ? "copy" : "flipp");

	spin_lock_bh(&np->rx_lock);
	spin_lock_irq(&np->tx_lock);

	/*
	 * Recovery procedure:
	 *  NB. Freelist index entries are always going to be less than
	 *  PAGE_OFFSET, whereas pointers to skbs will always be equal or
	 *  greater than PAGE_OFFSET: we use this property to distinguish
	 *  them.
	 */

	/* Step 1: Discard all pending TX packet fragments. */
	netif_release_tx_bufs(np);

	/* Step 2: Rebuild the RX buffer freelist and the RX ring itself. */
	for (requeue_idx = 0, i = 0; i < NET_RX_RING_SIZE; i++) {
		if (!np->rx_skbs[i])
			continue;

		skb = np->rx_skbs[requeue_idx] = xennet_get_rx_skb(np, i);
		ref = np->grant_rx_ref[requeue_idx] = xennet_get_rx_ref(np, i);
		req = RING_GET_REQUEST(&np->rx, requeue_idx);

		if (!np->copying_receiver) {
			gnttab_grant_foreign_transfer_ref(
				ref, np->xbdev->otherend_id,
				page_to_pfn(skb_shinfo(skb)->frags->page));
		} else {
			gnttab_grant_foreign_access_ref(
				ref, np->xbdev->otherend_id,
				pfn_to_mfn(page_to_pfn(skb_shinfo(skb)->
						       frags->page)),
				0);
		}
		req->gref = ref;
		req->id   = requeue_idx;

		requeue_idx++;
	}

	np->rx.req_prod_pvt = requeue_idx;

	/*
	 * Step 3: All public and private state should now be sane.  Get
	 * ready to start sending and receiving packets and give the driver
	 * domain a kick because we've probably just requeued some
	 * packets.
	 */
	netfront_carrier_on(np);
	notify_remote_via_irq(np->irq);
	network_tx_buf_gc(dev);
	network_alloc_rx_buffers(dev);

	spin_unlock_irq(&np->tx_lock);
	spin_unlock_bh(&np->rx_lock);

	return 0;
}

static void netif_uninit(struct net_device *dev)
{
	struct netfront_info *np = netdev_priv(dev);
	netif_release_tx_bufs(np);
	if (np->copying_receiver)
		netif_release_rx_bufs_copy(np);
	else
		netif_release_rx_bufs_flip(np);
	gnttab_free_grant_references(np->gref_tx_head);
	gnttab_free_grant_references(np->gref_rx_head);
}

static const struct ethtool_ops network_ethtool_ops =
{
	/**
	 * Description: 
	 *         Added some information for ethtool.
	 */
	.get_settings = netfront_get_settings,
	.get_wol      = netfront_get_wol,
	.get_msglevel = netfront_get_msglevel,

	.get_drvinfo = netfront_get_drvinfo,
	.get_tx_csum = ethtool_op_get_tx_csum,
	.set_tx_csum = ethtool_op_set_tx_csum,
	.get_sg = ethtool_op_get_sg,
	.set_sg = xennet_set_sg,
#if HAVE_TSO
	.get_tso = ethtool_op_get_tso,
	.set_tso = xennet_set_tso,
#endif
	.get_link = ethtool_op_get_link,
};

#ifdef CONFIG_SYSFS
static ssize_t show_rxbuf_min(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct netfront_info *info = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%u\n", info->rx_min_target);
}

static ssize_t store_rxbuf_min(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct net_device *netdev = to_net_dev(dev);
	struct netfront_info *np = netdev_priv(netdev);
	char *endp;
	unsigned long target;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	target = simple_strtoul(buf, &endp, 0);
	if (endp == buf)
		return -EBADMSG;

	if (target < RX_MIN_TARGET)
		target = RX_MIN_TARGET;
	if (target > RX_MAX_TARGET)
		target = RX_MAX_TARGET;

	spin_lock_bh(&np->rx_lock);
	if (target > np->rx_max_target)
		np->rx_max_target = target;
	np->rx_min_target = target;
	if (target > np->rx_target)
		np->rx_target = target;

	network_alloc_rx_buffers(netdev);

	spin_unlock_bh(&np->rx_lock);
	return len;
}

static ssize_t show_rxbuf_max(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct netfront_info *info = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%u\n", info->rx_max_target);
}

static ssize_t store_rxbuf_max(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct net_device *netdev = to_net_dev(dev);
	struct netfront_info *np = netdev_priv(netdev);
	char *endp;
	unsigned long target;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	target = simple_strtoul(buf, &endp, 0);
	if (endp == buf)
		return -EBADMSG;

	if (target < RX_MIN_TARGET)
		target = RX_MIN_TARGET;
	if (target > RX_MAX_TARGET)
		target = RX_MAX_TARGET;

	spin_lock_bh(&np->rx_lock);
	if (target < np->rx_min_target)
		np->rx_min_target = target;
	np->rx_max_target = target;
	if (target < np->rx_target)
		np->rx_target = target;

	network_alloc_rx_buffers(netdev);

	spin_unlock_bh(&np->rx_lock);
	return len;
}

static ssize_t show_rxbuf_cur(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct netfront_info *info = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%u\n", info->rx_target);
}

static struct device_attribute xennet_attrs[] = {
	__ATTR(rxbuf_min, S_IRUGO|S_IWUSR, show_rxbuf_min, store_rxbuf_min),
	__ATTR(rxbuf_max, S_IRUGO|S_IWUSR, show_rxbuf_max, store_rxbuf_max),
	__ATTR(rxbuf_cur, S_IRUGO, show_rxbuf_cur, NULL),
};

static int xennet_sysfs_addif(struct net_device *netdev)
{
	int i;
	int error = 0;

	for (i = 0; i < ARRAY_SIZE(xennet_attrs); i++) {
		error = device_create_file(&netdev->dev,
					   &xennet_attrs[i]);
		if (error)
			goto fail;
	}
	return 0;

 fail:
	while (--i >= 0)
		device_remove_file(&netdev->dev, &xennet_attrs[i]);
	return error;
}

static void xennet_sysfs_delif(struct net_device *netdev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xennet_attrs); i++)
		device_remove_file(&netdev->dev, &xennet_attrs[i]);
}

#endif /* CONFIG_SYSFS */


/*
 * Nothing to do here. Virtual interface is point-to-point and the
 * physical interface is probably promiscuous anyway.
 */
static void network_set_multicast_list(struct net_device *dev)
{
}

static const struct net_device_ops xennet_netdev_ops = {
	.ndo_uninit             = netif_uninit,
	.ndo_open               = network_open,
	.ndo_stop               = network_close,
	.ndo_start_xmit         = network_start_xmit,
	.ndo_set_multicast_list = network_set_multicast_list,
	.ndo_set_mac_address    = xennet_set_mac_address,
	.ndo_validate_addr      = eth_validate_addr,
	.ndo_change_mtu	        = xennet_change_mtu,
	.ndo_get_stats          = network_get_stats,
};

static struct net_device * __devinit create_netdev(struct xenbus_device *dev)
{
	int i, err = 0;
	struct net_device *netdev = NULL;
	struct netfront_info *np = NULL;

	netdev = alloc_etherdev(sizeof(struct netfront_info));
	if (!netdev) {
		printk(KERN_WARNING "%s> alloc_etherdev failed.\n",
		       __FUNCTION__);
		return ERR_PTR(-ENOMEM);
	}

	np                   = netdev_priv(netdev);
	np->xbdev            = dev;

	spin_lock_init(&np->tx_lock);
	spin_lock_init(&np->rx_lock);

	init_accelerator_vif(np, dev);

	skb_queue_head_init(&np->rx_batch);
	np->rx_target     = RX_DFL_MIN_TARGET;
	np->rx_min_target = RX_DFL_MIN_TARGET;
	np->rx_max_target = RX_MAX_TARGET;

	init_timer(&np->rx_refill_timer);
	np->rx_refill_timer.data = (unsigned long)netdev;
	np->rx_refill_timer.function = rx_refill_timeout;

	/* Initialise {tx,rx}_skbs as a free chain containing every entry. */
	for (i = 0; i <= NET_TX_RING_SIZE; i++) {
		np->tx_skbs[i] = (void *)((unsigned long) i+1);
		np->grant_tx_ref[i] = GRANT_INVALID_REF;
	}

	for (i = 0; i < NET_RX_RING_SIZE; i++) {
		np->rx_skbs[i] = NULL;
		np->grant_rx_ref[i] = GRANT_INVALID_REF;
	}

	/* A grant for every tx ring slot */
	if (gnttab_alloc_grant_references(TX_MAX_TARGET,
					  &np->gref_tx_head) < 0) {
		printk(KERN_ALERT "#### netfront can't alloc tx grant refs\n");
		err = -ENOMEM;
		goto exit;
	}
	/* A grant for every rx ring slot */
	if (gnttab_alloc_grant_references(RX_MAX_TARGET,
					  &np->gref_rx_head) < 0) {
		printk(KERN_ALERT "#### netfront can't alloc rx grant refs\n");
		err = -ENOMEM;
		goto exit_free_tx;
	}

	netdev->netdev_ops	= &xennet_netdev_ops;
	netif_napi_add(netdev, &np->napi, netif_poll, 64);
	netdev->features        = NETIF_F_IP_CSUM;

	SET_ETHTOOL_OPS(netdev, &network_ethtool_ops);
	SET_NETDEV_DEV(netdev, &dev->dev);

	np->netdev = netdev;

	netfront_carrier_off(np);

	return netdev;

 exit_free_tx:
	gnttab_free_grant_references(np->gref_tx_head);
 exit:
	free_netdev(netdev);
	return ERR_PTR(err);
}

#ifdef CONFIG_INET
/*
 * We use this notifier to send out a fake ARP reply to reset switches and
 * router ARP caches when an IP interface is brought up on a VIF.
 */
static int
inetdev_notify(struct notifier_block *this, unsigned long event, void *ptr)
{
	int count = 0;
	struct in_ifaddr  *ifa = (struct in_ifaddr *)ptr;
	struct net_device *dev = ifa->ifa_dev->dev;

	/* UP event and is it one of our devices? */
	if (event == NETDEV_UP && dev->netdev_ops->ndo_open == network_open)
	{
		for (count=0; count<3; count++)
		{
			(void)send_fake_arp(dev, ARPOP_REQUEST);
		}
		
		for (count=0; count<3; count++)
		{
			(void)send_fake_arp(dev, ARPOP_REPLY);
		}
	}

	return NOTIFY_DONE;
}

static struct notifier_block notifier_inetdev = {
	.notifier_call  = inetdev_notify,
	.next           = NULL,
	.priority       = 0
};
#endif


static void netif_disconnect_backend(struct netfront_info *info)
{
	/* Stop old i/f to prevent errors whilst we rebuild the state. */
	spin_lock_bh(&info->rx_lock);
	spin_lock_irq(&info->tx_lock);
	netfront_carrier_off(info);
	spin_unlock_irq(&info->tx_lock);
	spin_unlock_bh(&info->rx_lock);

	if (info->irq)
		unbind_from_irqhandler(info->irq, info->netdev);
	info->irq = 0;

	end_access(info->tx_ring_ref, info->tx.sring);
	end_access(info->rx_ring_ref, info->rx.sring);
	info->tx_ring_ref = GRANT_INVALID_REF;
	info->rx_ring_ref = GRANT_INVALID_REF;
	info->tx.sring = NULL;
	info->rx.sring = NULL;
}


static void end_access(int ref, void *page)
{
	if (ref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(ref, (unsigned long)page);
}


/* ** Driver registration ** */


static const struct xenbus_device_id netfront_ids[] = {
	{ "vif" },
	{ "" }
};
MODULE_ALIAS("xen:vif");


static struct xenbus_driver netfront_driver = {
	.name = "vif",
	.ids = netfront_ids,
	.probe = netfront_probe,
	.remove = __devexit_p(netfront_remove),
	.suspend = netfront_suspend,
	.suspend_cancel = netfront_suspend_cancel,
	.resume = netfront_resume,
	.otherend_changed = backend_changed,
};


static int __init netif_init(void)
{
	int err;

	if (!is_running_on_xen())
		return -ENODEV;

#ifdef CONFIG_XEN
	if (MODPARM_rx_flip && MODPARM_rx_copy) {
		WPRINTK("Cannot specify both rx_copy and rx_flip.\n");
		return -EINVAL;
	}

	if (!MODPARM_rx_flip && !MODPARM_rx_copy)
		MODPARM_rx_flip = 1; /* Default is to flip. */
#endif

	netif_init_accel();

	IPRINTK("Initialising virtual ethernet driver.\n");

#ifdef CONFIG_INET
	(void)register_inetaddr_notifier(&notifier_inetdev);
#endif

	err = xenbus_register_frontend(&netfront_driver);
	if (err) {
#ifdef CONFIG_INET
		unregister_inetaddr_notifier(&notifier_inetdev);
#endif
	}
	return err;
}
module_init(netif_init);


static void __exit netif_exit(void)
{
#ifdef CONFIG_INET
	unregister_inetaddr_notifier(&notifier_inetdev);
#endif
	xenbus_unregister_driver(&netfront_driver);

	netif_exit_accel();
}
module_exit(netif_exit);

MODULE_LICENSE("Dual BSD/GPL");
