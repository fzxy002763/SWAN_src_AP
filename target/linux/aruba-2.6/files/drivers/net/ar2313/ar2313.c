/*
 * ar2313.c: Linux driver for the Atheros AR2313 Ethernet device.
 *
 * Copyright 2004 by Sameer Dekate, <sdekate@arubanetworks.com>.
 * Copyright (C) 2006 Imre Kaloz <kaloz@openwrt.org>
 *
 * Thanks to Atheros for providing hardware and documentation
 * enabling me to write this driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Additional credits:
 * 	This code is taken from John Taylor's Sibyte driver and then 
 * 	modified for the AR2313.
 */

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sockios.h>
#include <linux/pkt_sched.h>
#include <linux/compile.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/ctype.h>

#include <net/sock.h>
#include <net/ip.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>
#include <asm/bootinfo.h>

extern char *getenv(char *e);


#undef INDEX_DEBUG
#define DEBUG     0
#define DEBUG_TX  0
#define DEBUG_RX  0
#define DEBUG_INT 0
#define DEBUG_MC  0
#define DEBUG_ERR 1

#ifndef __exit
#define __exit
#endif

#ifndef min
#define min(a,b)	(((a)<(b))?(a):(b))
#endif

#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES	L1_CACHE_BYTES
#endif

#ifndef SET_MODULE_OWNER
#define SET_MODULE_OWNER(dev)		{do{} while(0);}
#define AR2313_MOD_INC_USE_COUNT	MOD_INC_USE_COUNT
#define AR2313_MOD_DEC_USE_COUNT	MOD_DEC_USE_COUNT
#else
#define AR2313_MOD_INC_USE_COUNT	{do{} while(0);}
#define AR2313_MOD_DEC_USE_COUNT	{do{} while(0);}
#endif

#define PHYSADDR(a)		((_ACAST32_ (a)) & 0x1fffffff)

static char ethaddr[18] = "00:00:00:00:00:00";
static char ifname[5] = "bond";

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,52)
module_param_string(ethaddr, ethaddr, 18, 0);
module_param_string(ifname, ifname, 5, 0);
#else
MODULE_PARM(ethaddr, "c18");
MODULE_PARM(ifname, "c5");
#endif

#define AR2313_MBOX_SET_BIT  0x8

#define BOARD_IDX_STATIC	0
#define BOARD_IDX_OVERFLOW	-1

/* margot includes */
#include <asm/idt-boards/rc32434/rc32434.h>

#include "ar2313_msg.h"
#include "platform.h"
#include "dma.h"
#include "ar2313.h"

/*
 * New interrupt handler strategy:
 *
 * An old interrupt handler worked using the traditional method of
 * replacing an skbuff with a new one when a packet arrives. However
 * the rx rings do not need to contain a static number of buffer
 * descriptors, thus it makes sense to move the memory allocation out
 * of the main interrupt handler and do it in a bottom half handler
 * and only allocate new buffers when the number of buffers in the
 * ring is below a certain threshold. In order to avoid starving the
 * NIC under heavy load it is however necessary to force allocation
 * when hitting a minimum threshold. The strategy for alloction is as
 * follows:
 *
 *     RX_LOW_BUF_THRES    - allocate buffers in the bottom half
 *     RX_PANIC_LOW_THRES  - we are very low on buffers, allocate
 *                           the buffers in the interrupt handler
 *     RX_RING_THRES       - maximum number of buffers in the rx ring
 *
 * One advantagous side effect of this allocation approach is that the
 * entire rx processing can be done without holding any spin lock
 * since the rx rings and registers are totally independent of the tx
 * ring and its registers.  This of course includes the kmalloc's of
 * new skb's. Thus start_xmit can run in parallel with rx processing
 * and the memory allocation on SMP systems.
 *
 * Note that running the skb reallocation in a bottom half opens up
 * another can of races which needs to be handled properly. In
 * particular it can happen that the interrupt handler tries to run
 * the reallocation while the bottom half is either running on another
 * CPU or was interrupted on the same CPU. To get around this the
 * driver uses bitops to prevent the reallocation routines from being
 * reentered.
 *
 * TX handling can also be done without holding any spin lock, wheee
 * this is fun! since tx_csm is only written to by the interrupt
 * handler.
 */

/*
 * Threshold values for RX buffer allocation - the low water marks for
 * when to start refilling the rings are set to 75% of the ring
 * sizes. It seems to make sense to refill the rings entirely from the
 * intrrupt handler once it gets below the panic threshold, that way
 * we don't risk that the refilling is moved to another CPU when the
 * one running the interrupt handler just got the slab code hot in its
 * cache.
 */
#define RX_RING_SIZE		AR2313_DESCR_ENTRIES
#define RX_PANIC_THRES	        (RX_RING_SIZE/4)
#define RX_LOW_THRES	        ((3*RX_RING_SIZE)/4)
#define CRC_LEN                 4
#define RX_OFFSET               2

#define AR2313_BUFSIZE		(AR2313_MTU + ETH_HLEN + CRC_LEN + RX_OFFSET)

#ifdef MODULE
MODULE_AUTHOR("Sameer Dekate<sdekate@arubanetworks.com>");
MODULE_DESCRIPTION("AR2313 Ethernet driver");
#endif

#if DEBUG
static char version[] __initdata = 
  "ar2313.c: v0.02 2006/06/19  sdekate@arubanetworks.com\n";
#endif /* DEBUG */

#define virt_to_phys(x) ((u32)(x) & 0x1fffffff)

// prototypes
static short armiiread(short phy, short reg);
static void armiiwrite(short phy, short reg, short data);
#ifdef TX_TIMEOUT
static void ar2313_tx_timeout(struct net_device *dev);
#endif
static void ar2313_halt(struct net_device *dev);
static void rx_tasklet_func(unsigned long data);
static void ar2313_multicast_list(struct net_device *dev);

static struct net_device *root_dev;
static int probed __initdata = 0;
static unsigned long ar_eth_base;
static unsigned long ar_dma_base;
static unsigned long ar_int_base;
static unsigned long ar_int_mac_mask;
static unsigned long ar_int_phy_mask;

#ifndef ERR
#define ERR(fmt, args...) printk("%s: " fmt, __func__, ##args)
#endif

static int parse_mac_addr(struct net_device *dev, char* macstr){
  int i, j;
  unsigned char result, value;
  
  for (i=0; i<6; i++) {
    result = 0;
    if (i != 5 && *(macstr+2) != ':') {
      ERR("invalid mac address format: %d %c\n",
          i, *(macstr+2));
      return -EINVAL;
    }
    for (j=0; j<2; j++) {
      if (isxdigit(*macstr) && (value = isdigit(*macstr) ? *macstr-'0' :
                                toupper(*macstr)-'A'+10) < 16)
        {
          result = result*16 + value;
          macstr++;
        }
      else {
        ERR("invalid mac address "
            "character: %c\n", *macstr);
        return -EINVAL;
      }
    }
    
    macstr++;
    dev->dev_addr[i] = result;
  }
  
  return 0;
}


int __init ar2313_probe(void)
{
    struct net_device *dev;
    struct ar2313_private *sp;
    int version_disp;
    char name[64] ;

    if (probed)
        return -ENODEV;
    probed++;

    version_disp = 0;
    sprintf(name, "%s%%d", ifname) ;
    dev = alloc_etherdev(sizeof(struct ar2313_private));

    if (dev == NULL) {
	printk(KERN_ERR "ar2313: Unable to allocate net_device structure!\n");
	return -ENOMEM;
    }

    SET_MODULE_OWNER(dev);

    sp = dev->priv;

    sp->link = 0;
    switch (mips_machtype) {
    case MACH_ARUBA_AP60:
        ar_eth_base = 0xb8100000;
        ar_dma_base = ar_eth_base + 0x1000;
        ar_int_base = 0x1C003020;
        ar_int_mac_mask = RESET_ENET0|RESET_ENET1;
        ar_int_phy_mask = RESET_EPHY0|RESET_EPHY1;
        sp->mac = 1;
    	sp->phy = 1;
        dev->irq = 4;
        break;

    case MACH_ARUBA_AP40:
        ar_eth_base = 0xb0500000;
        ar_dma_base = ar_eth_base + 0x1000;
        ar_int_base = 0x11000004;
        ar_int_mac_mask = 0x800;
        ar_int_phy_mask = 0x400;
        sp->mac = 0;
    	sp->phy = 1;
        dev->irq = 4;
        break;

    case MACH_ARUBA_AP65:
        ar_eth_base = 0xb8100000;
        ar_dma_base = ar_eth_base + 0x1000;
        ar_int_base = 0x1C003020;
        ar_int_mac_mask = RESET_ENET0|RESET_ENET1;
        ar_int_phy_mask = RESET_EPHY0|RESET_EPHY1;
        sp->mac = 0;
#if 0
        // commented out, for now

        if (mips_machtype == MACH_ARUBA_SAMSUNG) {
            sp->phy = 0x1f;
        } else {
            sp->phy = 1;
        }
#else
        sp->phy = 1;
#endif
        dev->irq = 3;
        break;

    default:
        printk("%s: unsupported mips_machtype=0x%lx\n",
               __FUNCTION__, mips_machtype) ;
        return -ENODEV;
    }

    spin_lock_init(&sp->lock);

    /* initialize func pointers */
    dev->open = &ar2313_open;
    dev->stop = &ar2313_close;
    dev->hard_start_xmit = &ar2313_start_xmit;

    dev->get_stats = &ar2313_get_stats;
    dev->set_multicast_list = &ar2313_multicast_list;
#ifdef TX_TIMEOUT
    dev->tx_timeout = ar2313_tx_timeout;
    dev->watchdog_timeo = AR2313_TX_TIMEOUT;
#endif
    dev->do_ioctl = &ar2313_ioctl;

    // SAMEER: do we need this?
    dev->features |= NETIF_F_SG | NETIF_F_HIGHDMA;

    tasklet_init(&sp->rx_tasklet, rx_tasklet_func, (unsigned long) dev);
    tasklet_disable(&sp->rx_tasklet);

    /* display version info if adapter is found */
    if (!version_disp) {
        /* set display flag to TRUE so that */
        /* we only display this string ONCE */
	version_disp = 1;
#if DEBUG
	printk(version);
#endif /* DEBUG */
    }

    request_region(PHYSADDR(ETHERNET_BASE), ETHERNET_SIZE*ETHERNET_MACS,
                   "AR2313ENET");

    sp->eth_regs = ioremap_nocache(PHYSADDR(ETHERNET_BASE + ETHERNET_SIZE*sp->mac),
                                   sizeof(*sp->eth_regs));
    if (!sp->eth_regs) {
	printk("Can't remap eth registers\n");
	return(-ENXIO);
    }

    sp->dma_regs = ioremap_nocache(PHYSADDR(DMA_BASE + DMA_SIZE*sp->mac),
                                   sizeof(*sp->dma_regs));
    dev->base_addr = (unsigned int) sp->dma_regs;
    if (!sp->dma_regs) {
	printk("Can't remap DMA registers\n");
	return(-ENXIO);
    }

    sp->int_regs = ioremap_nocache(PHYSADDR(INTERRUPT_BASE),
                                   sizeof(*sp->int_regs));
    if (!sp->int_regs) {
	printk("Can't remap INTERRUPT registers\n");
	return(-ENXIO);
    }

    strncpy(sp->name, "Atheros AR2313", sizeof (sp->name) - 1);
    sp->name [sizeof (sp->name) - 1] = '\0';

    {
      char mac[32];
      extern char *getenv(char *e);
      unsigned char def_mac[6] = {0, 0x0b, 0x86, 0xba, 0xdb, 0xad};
      memset(mac, 0, 32);
      memcpy(mac, getenv("ethaddr"), 17);
      if (parse_mac_addr(dev, mac)){
        printk("%s: MAC address not found, using default\n", __func__);
        memcpy(dev->dev_addr, def_mac, 6);
      }
    }

    sp->board_idx = BOARD_IDX_STATIC;

    if (ar2313_init(dev)) {
        /*
         * ar2313_init() calls ar2313_init_cleanup() on error.
         */
        kfree(dev);
        return -ENODEV;
    }

    if (register_netdev(dev)){
      printk("%s: register_netdev failed\n", __func__);
      return -1;
    }

    printk("%s: %s: %02x:%02x:%02x:%02x:%02x:%02x, irq %d\n",
           dev->name, sp->name, 
           dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
           dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5],
           dev->irq);

    /* start link poll timer */
    ar2313_setup_timer(dev);

    /*
     * Register the device
     */
    root_dev = dev;

    return 0;
}

#if 0
static void ar2313_dump_regs(struct net_device *dev)
{
    unsigned int *ptr, i;
    struct ar2313_private *sp = (struct ar2313_private *)dev->priv;

    ptr = (unsigned int *)sp->eth_regs;
    for(i=0; i< (sizeof(ETHERNET_STRUCT)/ sizeof(unsigned int)); i++, ptr++) {
	    printk("ENET: %08x = %08x\n", (int)ptr, *ptr);
    }

    ptr = (unsigned int *)sp->dma_regs;
    for(i=0; i< (sizeof(DMA)/ sizeof(unsigned int)); i++, ptr++) {
	    printk("DMA: %08x = %08x\n", (int)ptr, *ptr);
    }

    ptr = (unsigned int *)sp->int_regs;
    for(i=0; i< (sizeof(INTERRUPT)/ sizeof(unsigned int)); i++, ptr++){
	    printk("INT: %08x = %08x\n", (int)ptr, *ptr);
    }

    for (i = 0; i < AR2313_DESCR_ENTRIES; i++) {
	ar2313_descr_t *td = &sp->tx_ring[i];
        printk("Tx desc %2d: %08x %08x %08x %08x\n", i,
               td->status, td->devcs, td->addr, td->descr);
    }
}
#endif

#ifdef TX_TIMEOUT
static void
ar2313_tx_timeout(struct net_device *dev)
{
    struct ar2313_private *sp = (struct ar2313_private *)dev->priv;
    unsigned long flags;
	
#if DEBUG_TX
    printk("Tx timeout\n");
#endif
    spin_lock_irqsave(&sp->lock, flags);
    ar2313_restart(dev);
    spin_unlock_irqrestore(&sp->lock, flags);
}
#endif

#if DEBUG_MC
static void
printMcList(struct net_device *dev)
{
    struct dev_mc_list *list = dev->mc_list;
    int num=0, i;
    while(list){
    	printk("%d MC ADDR ", num);
    	for(i=0;i<list->dmi_addrlen;i++) {
    	    printk(":%02x", list->dmi_addr[i]);
    	}
	list = list->next;
    	printk("\n");
    }
}
#endif

/*
 * Set or clear the multicast filter for this adaptor.
 * THIS IS ABSOLUTE CRAP, disabled
 */
static void
ar2313_multicast_list(struct net_device *dev)
{   
    /* 
     * Always listen to broadcasts and 
     * treat IFF bits independently 
     */
    struct ar2313_private *sp = (struct ar2313_private *)dev->priv;
    unsigned int recognise;

    recognise = sp->eth_regs->mac_control;

    if (dev->flags & IFF_PROMISC) { /* set promiscuous mode */
    	recognise |= MAC_CONTROL_PR;
    } else {
    	recognise &= ~MAC_CONTROL_PR;
    }

    if ((dev->flags & IFF_ALLMULTI) || (dev->mc_count > 15)) {
#if DEBUG_MC
	printMcList(dev);
    	printk("%s: all MULTICAST mc_count %d\n", __FUNCTION__, dev->mc_count);
#endif
    	recognise |= MAC_CONTROL_PM;/* all multicast */
    } else if (dev->mc_count > 0) {
#if DEBUG_MC
	printMcList(dev);
    	printk("%s: mc_count %d\n", __FUNCTION__, dev->mc_count);
#endif
    	recognise |= MAC_CONTROL_PM; /* for the time being */
    }
#if DEBUG_MC
    printk("%s: setting %08x to %08x\n", __FUNCTION__, (int)sp->eth_regs, recognise);
#endif
	
    sp->eth_regs->mac_control = recognise;
}

static void rx_tasklet_cleanup(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;

    /*
     * Tasklet may be scheduled. Need to get it removed from the list
     * since we're about to free the struct.
     */

    sp->unloading = 1;
    tasklet_enable(&sp->rx_tasklet);
    tasklet_kill(&sp->rx_tasklet);
}

static void __exit ar2313_module_cleanup(void)
{
    rx_tasklet_cleanup(root_dev);
    ar2313_init_cleanup(root_dev);
    unregister_netdev(root_dev);
    kfree(root_dev);
    release_region(PHYSADDR(ETHERNET_BASE), ETHERNET_SIZE*ETHERNET_MACS);
}


/*
 * Restart the AR2313 ethernet controller. 
 */
static int ar2313_restart(struct net_device *dev)
{
    /* disable interrupts */
    disable_irq(dev->irq);

    /* stop mac */
    ar2313_halt(dev);
    
    /* initialize */
    ar2313_init(dev);
    
    /* enable interrupts */
    enable_irq(dev->irq);
    
    return 0;
}

extern unsigned long mips_machtype;

int __init ar2313_module_init(void)
{
    int status=-1;
    switch (mips_machtype){
    case MACH_ARUBA_AP60:
    case MACH_ARUBA_AP65:
    case MACH_ARUBA_AP40:
    	root_dev = NULL;
    	status = ar2313_probe();
        break;
    }
    return status;
}


module_init(ar2313_module_init);
module_exit(ar2313_module_cleanup);


static void ar2313_free_descriptors(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    if (sp->rx_ring != NULL) {
	kfree((void*)KSEG0ADDR(sp->rx_ring));
	sp->rx_ring = NULL;
	sp->tx_ring = NULL;
    }
}


static int ar2313_allocate_descriptors(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    int size;
    int j;
    ar2313_descr_t *space;

    if(sp->rx_ring != NULL){
	printk("%s: already done.\n", __FUNCTION__);
	return 0;
    }

    size = (sizeof(ar2313_descr_t) * (AR2313_DESCR_ENTRIES * AR2313_QUEUES));
    space = kmalloc(size, GFP_KERNEL);
    if (space == NULL)
        return 1;

    /* invalidate caches */
    dma_cache_inv((unsigned int)space, size);

    /* now convert pointer to KSEG1 */
    space = (ar2313_descr_t *)KSEG1ADDR(space);

    memset((void *)space, 0, size);

    sp->rx_ring = space;
    space += AR2313_DESCR_ENTRIES;

    sp->tx_ring = space;
    space += AR2313_DESCR_ENTRIES;

    /* Initialize the transmit Descriptors */
    for (j = 0; j < AR2313_DESCR_ENTRIES; j++) {
	ar2313_descr_t *td = &sp->tx_ring[j];
	td->status = 0;
	td->devcs  = DMA_TX1_CHAINED;
	td->addr   = 0;
	td->descr  = K1_TO_PHYS(&sp->tx_ring[(j+1) & (AR2313_DESCR_ENTRIES-1)]);
    }

    return 0;
}


/*
 * Generic cleanup handling data allocated during init. Used when the
 * module is unloaded or if an error occurs during initialization
 */
static void ar2313_init_cleanup(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    struct sk_buff *skb;
    int j;

    ar2313_free_descriptors(dev);

    if (sp->eth_regs) iounmap((void*)sp->eth_regs);
    if (sp->dma_regs) iounmap((void*)sp->dma_regs);

    if (sp->rx_skb) {
	for (j = 0; j < AR2313_DESCR_ENTRIES; j++) {
	    skb = sp->rx_skb[j];
	    if (skb) {
		sp->rx_skb[j] = NULL;
		dev_kfree_skb(skb);
	    }
	}
	kfree(sp->rx_skb);
	sp->rx_skb = NULL;
    }

    if (sp->tx_skb) {
	for (j = 0; j < AR2313_DESCR_ENTRIES; j++) {
	    skb = sp->tx_skb[j];
	    if (skb) {
		sp->tx_skb[j] = NULL;
		dev_kfree_skb(skb);
	    }
	}
	kfree(sp->tx_skb);
	sp->tx_skb = NULL;
    }
}

static int ar2313_setup_timer(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv; 

    init_timer(&sp->link_timer);

    sp->link_timer.function = ar2313_link_timer_fn;
    sp->link_timer.data = (int) dev;
    sp->link_timer.expires = jiffies + HZ;

    add_timer(&sp->link_timer);
    return 0;

}

static void ar2313_link_timer_fn(unsigned long data)
{
    struct net_device *dev = (struct net_device *) data;
    struct ar2313_private *sp = dev->priv; 

    // see if the link status changed
    // This was needed to make sure we set the PHY to the
    // autonegotiated value of half or full duplex.
    ar2313_check_link(dev);
    
    // Loop faster when we don't have link. 
    // This was needed to speed up the AP bootstrap time.
    if(sp->link == 0) {
    	mod_timer(&sp->link_timer, jiffies + HZ/2);
    } else {
    	mod_timer(&sp->link_timer, jiffies + LINK_TIMER);
    }
}

static void ar2313_check_link(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    u16 phyData;

    phyData = armiiread(sp->phy, MII_BMSR);
    if (sp->phyData != phyData) {
	if (phyData & BMSR_LSTATUS) {
            /* link is present, ready link partner ability to deterine duplexity */
            int duplex = 0;
            u16 reg;

            sp->link = 1;
            reg = armiiread(sp->phy, MII_BMCR);
            if (reg & BMCR_ANENABLE) {
                /* auto neg enabled */
                reg = armiiread(sp->phy, MII_LPA);
                duplex = (reg & (LPA_100FULL|LPA_10FULL))? 1:0;
            } else {
                /* no auto neg, just read duplex config */
                duplex = (reg & BMCR_FULLDPLX)? 1:0;
            }

            printk(KERN_INFO "%s: Configuring MAC for %s duplex\n", dev->name,
                   (duplex)? "full":"half");

            if (duplex) {
                /* full duplex */
                sp->eth_regs->mac_control = ((sp->eth_regs->mac_control | MAC_CONTROL_F) &
                                             ~MAC_CONTROL_DRO);
            } else {
                /* half duplex */
                sp->eth_regs->mac_control = ((sp->eth_regs->mac_control | MAC_CONTROL_DRO) &
                                             ~MAC_CONTROL_F);
            }
    	} else {
            /* no link */
            sp->link = 0;
    	}
        sp->phyData = phyData;
    }
}
  
static int
ar2313_reset_reg(struct net_device *dev)
{
    struct ar2313_private *sp = (struct ar2313_private *)dev->priv;
    unsigned int ethsal, ethsah;
    unsigned int flags;

    *sp->int_regs |= ar_int_mac_mask;
    mdelay(10);
    *sp->int_regs &= ~ar_int_mac_mask;
    mdelay(10);
    *sp->int_regs |= ar_int_phy_mask;
    mdelay(10);
    *sp->int_regs &= ~ar_int_phy_mask;
    mdelay(10);

    sp->dma_regs->bus_mode = (DMA_BUS_MODE_SWR);
    mdelay(10);
    sp->dma_regs->bus_mode = ((32 << DMA_BUS_MODE_PBL_SHIFT) | DMA_BUS_MODE_BLE);

    /* enable interrupts */
    sp->dma_regs->intr_ena = (DMA_STATUS_AIS |
			      DMA_STATUS_NIS |
			      DMA_STATUS_RI  |
			      DMA_STATUS_TI  |
			      DMA_STATUS_FBE);
    sp->dma_regs->xmt_base = K1_TO_PHYS(sp->tx_ring);
    sp->dma_regs->rcv_base = K1_TO_PHYS(sp->rx_ring);
    sp->dma_regs->control = (DMA_CONTROL_SR | DMA_CONTROL_ST | DMA_CONTROL_SF);
    
    sp->eth_regs->flow_control = (FLOW_CONTROL_FCE);
    sp->eth_regs->vlan_tag = (0x8100);

    /* Enable Ethernet Interface */
    flags = (MAC_CONTROL_TE  | /* transmit enable */
	     MAC_CONTROL_PM  | /* pass mcast */
	     MAC_CONTROL_F   | /* full duplex */
	     MAC_CONTROL_HBD); /* heart beat disabled */

    if (dev->flags & IFF_PROMISC) { /* set promiscuous mode */
	flags |= MAC_CONTROL_PR;
    }
    sp->eth_regs->mac_control = flags;

    /* Set all Ethernet station address registers to their initial values */
    ethsah = ((((u_int)(dev->dev_addr[5]) << 8) & (u_int)0x0000FF00) |
	      (((u_int)(dev->dev_addr[4]) << 0) & (u_int)0x000000FF));

    ethsal = ((((u_int)(dev->dev_addr[3]) << 24) & (u_int)0xFF000000) |
              (((u_int)(dev->dev_addr[2]) << 16) & (u_int)0x00FF0000) |
              (((u_int)(dev->dev_addr[1]) <<  8) & (u_int)0x0000FF00) |
              (((u_int)(dev->dev_addr[0]) <<  0) & (u_int)0x000000FF) );

    sp->eth_regs->mac_addr[0] = ethsah;
    sp->eth_regs->mac_addr[1] = ethsal;

    mdelay(10);

    return(0);
}


static int ar2313_init(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    int ecode=0;

    /*
     * Allocate descriptors
     */
    if (ar2313_allocate_descriptors(dev)) {
	printk("%s: %s: ar2313_allocate_descriptors failed\n", 
			dev->name, __FUNCTION__);
	ecode = -EAGAIN;
        goto init_error;
    }

    /*
     * Get the memory for the skb rings.
     */
    if(sp->rx_skb == NULL) {
	sp->rx_skb = kmalloc(sizeof(struct sk_buff *) * AR2313_DESCR_ENTRIES, GFP_KERNEL);
	if (!(sp->rx_skb)) {
	    printk("%s: %s: rx_skb kmalloc failed\n", 
			    dev->name, __FUNCTION__);
	    ecode = -EAGAIN;
	    goto init_error;
	}
    }
    memset(sp->rx_skb, 0, sizeof(struct sk_buff *) * AR2313_DESCR_ENTRIES);

    if(sp->tx_skb == NULL) {
	sp->tx_skb = kmalloc(sizeof(struct sk_buff *) * AR2313_DESCR_ENTRIES, GFP_KERNEL);
	if (!(sp->tx_skb)) {
	    printk("%s: %s: tx_skb kmalloc failed\n", 
			    dev->name, __FUNCTION__);
	    ecode = -EAGAIN;
	    goto init_error;
	}
    }
    memset(sp->tx_skb, 0, sizeof(struct sk_buff *) * AR2313_DESCR_ENTRIES);

    /*
     * Set tx_csm before we start receiving interrupts, otherwise
     * the interrupt handler might think it is supposed to process
     * tx ints before we are up and running, which may cause a null
     * pointer access in the int handler.
     */
    sp->rx_skbprd = 0;
    sp->cur_rx = 0;
    sp->tx_prd = 0;
    sp->tx_csm = 0;

    /*
     * Zero the stats before starting the interface
     */
    memset(&sp->stats, 0, sizeof(sp->stats));

    /*
     * We load the ring here as there seem to be no way to tell the
     * firmware to wipe the ring without re-initializing it.
     */
    ar2313_load_rx_ring(dev, RX_RING_SIZE);

    /* 
     * Init hardware
     */
    ar2313_reset_reg(dev);

    /*
     * Get the IRQ
     */
    ecode = request_irq(dev->irq, &ar2313_interrupt, SA_SHIRQ | SA_INTERRUPT, dev->name, dev);
    if (ecode) {
        printk(KERN_WARNING "%s: %s: Requested IRQ %d is busy\n",
	       dev->name, __FUNCTION__, dev->irq);
	goto init_error;
    }

#if 0
    // commented out, for now

    if(mips_machtype == MACH_ARUBA_SAMSUNG) {
      int i;
        /* configure Marvell 88E6060 */
        /* reset chip */
        armiiwrite(0x1f, 0xa, 0xa130);
        do {
            udelay(1000);
            i = armiiread(sp->phy, 0xa);
        } while (i & 0x8000);
      
        /* configure MAC address */
        armiiwrite(sp->phy, 0x1, dev->dev_addr[0] << 8 | dev->dev_addr[1]);
        armiiwrite(sp->phy, 0x2, dev->dev_addr[2] << 8 | dev->dev_addr[3]);
        armiiwrite(sp->phy, 0x3, dev->dev_addr[4] << 8 | dev->dev_addr[5]);
      
        /* set ports to forwarding */
        armiiwrite(0x18, 0x4, 0x3);
        armiiwrite(0x1c, 0x4, 0x3);
        armiiwrite(0x1d, 0x4, 0x3);
    }
#endif

    tasklet_enable(&sp->rx_tasklet);

    return 0;

 init_error:
    ar2313_init_cleanup(dev);
    return ecode;
}

/*
 * Load the rx ring.
 *
 * Loading rings is safe without holding the spin lock since this is
 * done only before the device is enabled, thus no interrupts are
 * generated and by the interrupt handler/tasklet handler.
 */
static void ar2313_load_rx_ring(struct net_device *dev, int nr_bufs)
{

    struct ar2313_private *sp = ((struct net_device *)dev)->priv;
    short i, idx;

    idx = sp->rx_skbprd;

    for (i = 0; i < nr_bufs; i++) {
        struct sk_buff *skb;
	ar2313_descr_t *rd;

	if (sp->rx_skb[idx]) {
#if DEBUG_RX
	    printk(KERN_INFO "ar2313 rx refill full\n");
#endif /* DEBUG */
	    break;
	}

        // partha: create additional room for the second GRE fragment
	skb = alloc_skb(AR2313_BUFSIZE+128, GFP_ATOMIC);
	if (!skb) {
	    printk("\n\n\n\n %s: No memory in system\n\n\n\n", __FUNCTION__);
	    break;
	}
        // partha: create additional room in the front for tx pkt capture
        skb_reserve(skb, 32);

	/*
	 * Make sure IP header starts on a fresh cache line.
	 */
	skb->dev = dev;
	skb_reserve(skb, RX_OFFSET);
	sp->rx_skb[idx] = skb;

	rd = (ar2313_descr_t *) &sp->rx_ring[idx];

	/* initialize dma descriptor */
	rd->devcs  = ((AR2313_BUFSIZE << DMA_RX1_BSIZE_SHIFT) |
                      DMA_RX1_CHAINED);
	rd->addr   = virt_to_phys(skb->data);
	rd->descr  = virt_to_phys(&sp->rx_ring[(idx+1) & (AR2313_DESCR_ENTRIES-1)]);
	rd->status = DMA_RX_OWN;

	idx = DSC_NEXT(idx);
    }

    if (!i) {
#if DEBUG_ERR
        printk(KERN_INFO "Out of memory when allocating standard receive buffers\n");
#endif /* DEBUG */
    } else {
	sp->rx_skbprd = idx;
    }

    return;
}

#define AR2313_MAX_PKTS_PER_CALL        64

static int ar2313_rx_int(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    struct sk_buff *skb, *skb_new;
    ar2313_descr_t *rxdesc;
    unsigned int status;
    u32 idx;
    int pkts = 0;
    int rval;

    idx = sp->cur_rx;

    /* process at most the entire ring and then wait for another interrupt */
    while(1) {

	rxdesc = &sp->rx_ring[idx];
	status = rxdesc->status;
	if (status & DMA_RX_OWN) {
	    /* SiByte owns descriptor or descr not yet filled in */
            rval = 0;
	    break;
	}

        if (++pkts > AR2313_MAX_PKTS_PER_CALL) {
            rval = 1;
            break;
        }

#if DEBUG_RX
	printk("index %d\n", idx);
	printk("RX status %08x\n", rxdesc->status);
	printk("RX devcs  %08x\n", rxdesc->devcs );
	printk("RX addr   %08x\n", rxdesc->addr  );
	printk("RX descr  %08x\n", rxdesc->descr );
#endif

        if ((status & (DMA_RX_ERROR|DMA_RX_ERR_LENGTH)) &&
	    (!(status & DMA_RX_LONG))){
#if DEBUG_RX
	    printk("%s: rx ERROR %08x\n", __FUNCTION__, status);
#endif
	    sp->stats.rx_errors++;
	    sp->stats.rx_dropped++;

	    /* add statistics counters */
	    if (status & DMA_RX_ERR_CRC)    sp->stats.rx_crc_errors++;
	    if (status & DMA_RX_ERR_COL)    sp->stats.rx_over_errors++;
	    if (status & DMA_RX_ERR_LENGTH)
		    sp->stats.rx_length_errors++;
	    if (status & DMA_RX_ERR_RUNT)   sp->stats.rx_over_errors++;
	    if (status & DMA_RX_ERR_DESC)   sp->stats.rx_over_errors++;

	} else {
	    /* alloc new buffer. */
	    skb_new = dev_alloc_skb(AR2313_BUFSIZE + RX_OFFSET + 128);
	    if (skb_new != NULL) {

                skb = sp->rx_skb[idx];
		/* set skb */
                skb_put(skb, ((status >> DMA_RX_LEN_SHIFT) & 0x3fff) - CRC_LEN);

#ifdef CONFIG_MERLOT
		if ((dev->am_pkt_handler == NULL) || 
                    (dev->am_pkt_handler(skb, dev) == 0)) {
#endif
                  sp->stats.rx_bytes += skb->len;
                  skb->protocol = eth_type_trans(skb, dev);
                  /* pass the packet to upper layers */

#ifdef CONFIG_MERLOT
                  if (dev->asap_netif_rx)
                      dev->asap_netif_rx(skb);
                  else
#endif
                  netif_rx(skb);
#ifdef CONFIG_MERLOT
		}
#endif
		skb_new->dev = dev;
		/* 16 bit align */
		skb_reserve(skb_new, RX_OFFSET+32);
		/* reset descriptor's curr_addr */
		rxdesc->addr = virt_to_phys(skb_new->data); 

		sp->stats.rx_packets++;
		sp->rx_skb[idx] = skb_new;

	    } else {
		sp->stats.rx_dropped++;
	    }
	}

	rxdesc->devcs = ((AR2313_BUFSIZE << DMA_RX1_BSIZE_SHIFT) | 
                         DMA_RX1_CHAINED);
	rxdesc->status = DMA_RX_OWN;

	idx = DSC_NEXT(idx);
    }

    sp->cur_rx = idx;

    return rval;
}


static void ar2313_tx_int(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    u32 idx;
    struct sk_buff *skb;
    ar2313_descr_t *txdesc;
    unsigned int status=0;

    idx = sp->tx_csm;

    while (idx != sp->tx_prd) {

	txdesc = &sp->tx_ring[idx];

#if DEBUG_TX
	printk("%s: TXINT: csm=%d idx=%d prd=%d status=%x devcs=%x addr=%08x descr=%x\n", 
		dev->name, sp->tx_csm, idx, sp->tx_prd,
	       	txdesc->status, txdesc->devcs, txdesc->addr, txdesc->descr);
#endif /* DEBUG */

	if ((status = txdesc->status) & DMA_TX_OWN) {
	    /* ar2313 dma still owns descr */
	    break;
	}
	/* done with this descriptor */
	txdesc->status = 0;

	if (status & DMA_TX_ERROR){
		sp->stats.tx_errors++;
		sp->stats.tx_dropped++;
		if(status & DMA_TX_ERR_UNDER)
                    sp->stats.tx_fifo_errors++;
		if(status & DMA_TX_ERR_HB)
                    sp->stats.tx_heartbeat_errors++;
		if(status & (DMA_TX_ERR_LOSS |
                             DMA_TX_ERR_LINK))
                    sp->stats.tx_carrier_errors++;
                if (status & (DMA_TX_ERR_LATE|
                              DMA_TX_ERR_COL |
                              DMA_TX_ERR_JABBER |
                              DMA_TX_ERR_DEFER))
                    sp->stats.tx_aborted_errors++;
	} else {
		/* transmit OK */
		sp->stats.tx_packets++;
	}

	skb = sp->tx_skb[idx];
	sp->tx_skb[idx] = NULL;
	idx = DSC_NEXT(idx);
	sp->stats.tx_bytes += skb->len;
	dev_kfree_skb_irq(skb);
    }

    sp->tx_csm = idx;

    return;
}


static void
rx_tasklet_func(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;
	struct ar2313_private *sp = dev->priv;

	if (sp->unloading) {
	    return;
	}

        if (ar2313_rx_int(dev)) {
            tasklet_hi_schedule(&sp->rx_tasklet);
        }
        else {
            unsigned long flags;
            spin_lock_irqsave(&sp->lock, flags);
            sp->dma_regs->intr_ena |= DMA_STATUS_RI;
            spin_unlock_irqrestore(&sp->lock, flags);
        }
}

static void
rx_schedule(struct net_device *dev)
{
	struct ar2313_private *sp = dev->priv;

	sp->dma_regs->intr_ena &= ~DMA_STATUS_RI;

	tasklet_hi_schedule(&sp->rx_tasklet);
}

static irqreturn_t ar2313_interrupt(int irq, void *dev_id)
{
    struct net_device *dev = (struct net_device *)dev_id;
    struct ar2313_private *sp = dev->priv;
    unsigned int status, enabled;

    /* clear interrupt */
    /*
     * Don't clear RI bit if currently disabled.
     */
    status = sp->dma_regs->status;
    enabled = sp->dma_regs->intr_ena;
    sp->dma_regs->status = status & enabled;

    if (status & DMA_STATUS_NIS) {
	/* normal status */
        /*
         * Don't schedule rx processing if interrupt
         * is already disabled.
         */
	if (status & enabled & DMA_STATUS_RI) {
	    /* receive interrupt */
	    rx_schedule(dev);
	}
	if (status & DMA_STATUS_TI) {
	   /* transmit interrupt */
	    ar2313_tx_int(dev);
	}
    }

    if (status & DMA_STATUS_AIS) {
#if DEBUG_INT
        printk("%s: AIS set %08x & %x\n", __FUNCTION__,
               status, (DMA_STATUS_FBE | DMA_STATUS_TPS));
#endif
	/* abnormal status */
	if (status & (DMA_STATUS_FBE | DMA_STATUS_TPS)) {
	    ar2313_restart(dev);
	}
    }
    return IRQ_HANDLED;
}


static int ar2313_open(struct net_device *dev)
{
    struct ar2313_private *sp;

    sp = dev->priv;

    dev->mtu = 1500;
    netif_start_queue(dev);

    sp->eth_regs->mac_control |= MAC_CONTROL_RE;

    AR2313_MOD_INC_USE_COUNT;

    return 0;
}

static void ar2313_halt(struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    int j;

    tasklet_disable(&sp->rx_tasklet);

    /* kill the MAC */
    sp->eth_regs->mac_control &= ~(MAC_CONTROL_RE | /* disable Receives */
                                   MAC_CONTROL_TE); /* disable Transmits */
    /* stop dma */
    sp->dma_regs->control = 0;
    sp->dma_regs->bus_mode = DMA_BUS_MODE_SWR;

    /* place phy and MAC in reset */
    *sp->int_regs |= (ar_int_mac_mask | ar_int_phy_mask);

    /* free buffers on tx ring */
    for (j = 0; j < AR2313_DESCR_ENTRIES; j++) {
	struct sk_buff *skb;
	ar2313_descr_t *txdesc;

	txdesc = &sp->tx_ring[j];
	txdesc->descr = 0;

	skb = sp->tx_skb[j];
	if (skb) {
	    dev_kfree_skb(skb);
	    sp->tx_skb[j] = NULL;
	}
    }
}

/*
 * close should do nothing. Here's why. It's called when
 * 'ifconfig bond0 down' is run. If it calls free_irq then
 * the irq is gone forever ! When bond0 is made 'up' again,
 * the ar2313_open () does not call request_irq (). Worse,
 * the call to ar2313_halt() generates a WDOG reset due to
 * the write to 'sp->int_regs' and the box reboots.
 * Commenting this out is good since it allows the
 * system to resume when bond0 is made up again.
 */
static int ar2313_close(struct net_device *dev)
{
#if 0
    /*
     * Disable interrupts
     */
    disable_irq(dev->irq);
    	
    /*
     * Without (or before) releasing irq and stopping hardware, this
     * is an absolute non-sense, by the way. It will be reset instantly
     * by the first irq.
     */
    netif_stop_queue(dev);

    /* stop the MAC and DMA engines */
    ar2313_halt(dev);

    /* release the interrupt */
    free_irq(dev->irq, dev);

#endif
    AR2313_MOD_DEC_USE_COUNT;
    return 0;
}

static int ar2313_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct ar2313_private *sp = dev->priv;
    ar2313_descr_t *td;
    u32 idx;

    idx = sp->tx_prd;
    td = &sp->tx_ring[idx];

    if (td->status & DMA_TX_OWN) {
#if DEBUG_TX
	printk("%s: No space left to Tx\n", __FUNCTION__);
#endif
        /* free skbuf and lie to the caller that we sent it out */
        sp->stats.tx_dropped++;
	dev_kfree_skb(skb);

        /* restart transmitter in case locked */
        sp->dma_regs->xmt_poll = 0;
	return 0;
    }

    /* Setup the transmit descriptor. */
    td->devcs = ((skb->len << DMA_TX1_BSIZE_SHIFT) | 
                 (DMA_TX1_LS|DMA_TX1_IC|DMA_TX1_CHAINED));
    td->addr = virt_to_phys(skb->data);
    td->status = DMA_TX_OWN;

    /* kick transmitter last */
    sp->dma_regs->xmt_poll = 0;

#if DEBUG_TX
    printk("index %d\n", idx);
    printk("TX status %08x\n", td->status);
    printk("TX devcs  %08x\n", td->devcs );
    printk("TX addr   %08x\n", td->addr  );
    printk("TX descr  %08x\n", td->descr );
#endif

    sp->tx_skb[idx] = skb;
    idx = DSC_NEXT(idx);
    sp->tx_prd = idx;

    //dev->trans_start = jiffies;

    return 0;
}

static int netdev_get_ecmd(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	struct ar2313_private *np = dev->priv;
	u32 tmp;

	ecmd->supported = 
		(SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
		SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
		SUPPORTED_Autoneg | SUPPORTED_TP | SUPPORTED_MII);
	
        ecmd->port = PORT_TP;
	/* only supports internal transceiver */
	ecmd->transceiver = XCVR_INTERNAL;
	/* not sure what this is for */
	ecmd->phy_address = 1;

	ecmd->advertising = ADVERTISED_MII;
	tmp = armiiread(np->phy, MII_ADVERTISE);
	if (tmp & ADVERTISE_10HALF)
		ecmd->advertising |= ADVERTISED_10baseT_Half;
	if (tmp & ADVERTISE_10FULL)
		ecmd->advertising |= ADVERTISED_10baseT_Full;
	if (tmp & ADVERTISE_100HALF)
		ecmd->advertising |= ADVERTISED_100baseT_Half;
	if (tmp & ADVERTISE_100FULL)
		ecmd->advertising |= ADVERTISED_100baseT_Full;

	tmp = armiiread(np->phy, MII_BMCR);
	if (tmp & BMCR_ANENABLE) {
		ecmd->advertising |= ADVERTISED_Autoneg;
		ecmd->autoneg = AUTONEG_ENABLE;
	} else {
		ecmd->autoneg = AUTONEG_DISABLE;
	}

        if (ecmd->autoneg == AUTONEG_ENABLE) {
            tmp = armiiread(np->phy, MII_LPA);
            if (tmp & (LPA_100FULL|LPA_10FULL)) {
                ecmd->duplex = DUPLEX_FULL;
            } else {
                ecmd->duplex = DUPLEX_HALF;
            }
            if (tmp & (LPA_100FULL|LPA_100HALF)) {
		ecmd->speed = SPEED_100;
            } else {
		ecmd->speed = SPEED_10;
            }
        } else {
            if (tmp & BMCR_FULLDPLX) {
                ecmd->duplex = DUPLEX_FULL;
            } else {
                ecmd->duplex = DUPLEX_HALF;
            }
            if (tmp & BMCR_SPEED100) {
		ecmd->speed = SPEED_100;
            } else {
		ecmd->speed = SPEED_10;
            }
	}

	/* ignore maxtxpkt, maxrxpkt for now */

	return 0;
}

static int netdev_set_ecmd(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	struct ar2313_private *np = dev->priv;
	u32 tmp;

	if (ecmd->speed != SPEED_10 && ecmd->speed != SPEED_100)
		return -EINVAL;
	if (ecmd->duplex != DUPLEX_HALF && ecmd->duplex != DUPLEX_FULL)
		return -EINVAL;
	if (ecmd->port != PORT_TP)
		return -EINVAL;
	if (ecmd->transceiver != XCVR_INTERNAL)
		return -EINVAL;
	if (ecmd->autoneg != AUTONEG_DISABLE && ecmd->autoneg != AUTONEG_ENABLE)
		return -EINVAL;
	/* ignore phy_address, maxtxpkt, maxrxpkt for now */
	
	/* WHEW! now lets bang some bits */
	
	tmp = armiiread(np->phy, MII_BMCR);
	if (ecmd->autoneg == AUTONEG_ENABLE) {
		/* turn on autonegotiation */
		tmp |= BMCR_ANENABLE;
                printk("%s: Enabling auto-neg\n", dev->name);
	} else {
		/* turn off auto negotiation, set speed and duplexity */
		tmp &= ~(BMCR_ANENABLE | BMCR_SPEED100 | BMCR_FULLDPLX);
		if (ecmd->speed == SPEED_100)
			tmp |= BMCR_SPEED100;
		if (ecmd->duplex == DUPLEX_FULL)
			tmp |= BMCR_FULLDPLX;
                printk("%s: Hard coding %d/%s\n", dev->name, 
                       (ecmd->speed == SPEED_100)? 100:10,
                       (ecmd->duplex == DUPLEX_FULL)? "full":"half");
	}
	armiiwrite(np->phy, MII_BMCR, tmp);
        np->phyData = 0;
	return 0;
}

static int netdev_ethtool_ioctl(struct net_device *dev, void *useraddr)
{
	struct ar2313_private *np = dev->priv;
	u32 cmd;
	
	if (get_user(cmd, (u32 *)useraddr))
		return -EFAULT;

        switch (cmd) {
	/* get settings */
	case ETHTOOL_GSET: {
		struct ethtool_cmd ecmd = { ETHTOOL_GSET };
		spin_lock_irq(&np->lock);
		netdev_get_ecmd(dev, &ecmd);
		spin_unlock_irq(&np->lock);
		if (copy_to_user(useraddr, &ecmd, sizeof(ecmd)))
			return -EFAULT;
		return 0;
	}
	/* set settings */
	case ETHTOOL_SSET: {
		struct ethtool_cmd ecmd;
		int r;
		if (copy_from_user(&ecmd, useraddr, sizeof(ecmd)))
			return -EFAULT;
		spin_lock_irq(&np->lock);
		r = netdev_set_ecmd(dev, &ecmd);
		spin_unlock_irq(&np->lock);
		return r;
	}
	/* restart autonegotiation */
	case ETHTOOL_NWAY_RST: {
		int tmp;
		int r = -EINVAL;
		/* if autoneg is off, it's an error */
		tmp = armiiread(np->phy, MII_BMCR);
		if (tmp & BMCR_ANENABLE) {
			tmp |= (BMCR_ANRESTART);
			armiiwrite(np->phy, MII_BMCR, tmp);
			r = 0;
		}
		return r;
	}
	/* get link status */
	case ETHTOOL_GLINK: {
		struct ethtool_value edata = {ETHTOOL_GLINK};
		edata.data = (armiiread(np->phy, MII_BMSR)&BMSR_LSTATUS) ? 1:0;
		if (copy_to_user(useraddr, &edata, sizeof(edata)))
			return -EFAULT;
		return 0;
	}
        }
	
	return -EOPNOTSUPP;
}

static int ar2313_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
    struct mii_ioctl_data *data = (struct mii_ioctl_data *)&ifr->ifr_data;

    switch (cmd) {
    case SIOCDEVPRIVATE: {
        struct ar2313_cmd scmd;

        if (copy_from_user(&scmd, ifr->ifr_data, sizeof(scmd)))
	    return -EFAULT;

#if DEBUG
	printk("%s: ioctl devprivate c=%d a=%x l=%d m=%d d=%x,%x\n",
	       dev->name, scmd.cmd,
	       scmd.address, scmd.length,
	       scmd.mailbox, scmd.data[0], scmd.data[1]);
#endif /* DEBUG */

	switch (scmd.cmd) {
	case AR2313_READ_DATA:
	    if(scmd.length==4){
		scmd.data[0] = *((u32*)scmd.address);
	    } else if(scmd.length==2) {
		scmd.data[0] = *((u16*)scmd.address);
	    } else if (scmd.length==1) {
		scmd.data[0] = *((u8*)scmd.address);
	    } else {
		return -EOPNOTSUPP;
	    }
	    if(copy_to_user(ifr->ifr_data, &scmd, sizeof(scmd)))
	        return -EFAULT;
	    break;

	case AR2313_WRITE_DATA:
	    if(scmd.length==4){
		*((u32*)scmd.address) = scmd.data[0];
	    } else if(scmd.length==2) {
		*((u16*)scmd.address) = scmd.data[0];
	    } else if (scmd.length==1) {
		*((u8*)scmd.address) = scmd.data[0];
	    } else {
		return -EOPNOTSUPP;
	    }
	    break;

	case AR2313_GET_VERSION:
	    // SAMEER: sprintf((char*) &scmd, "%s", ARUBA_VERSION);
	    if(copy_to_user(ifr->ifr_data, &scmd, sizeof(scmd)))
	        return -EFAULT;
	    break;

	default:
	    return -EOPNOTSUPP;
	}
	return 0;
    }
      
    case SIOCETHTOOL:
        return netdev_ethtool_ioctl(dev, (void *) ifr->ifr_data);

    case SIOCGMIIPHY:		/* Get address of MII PHY in use. */
        data->phy_id = 1;
        /* Fall Through */

    case SIOCGMIIREG:		/* Read MII PHY register. */
    case SIOCDEVPRIVATE+1:	/* for binary compat, remove in 2.5 */
        data->val_out = armiiread(data->phy_id & 0x1f, 
                                  data->reg_num & 0x1f);
        return 0;
    case SIOCSMIIREG:		/* Write MII PHY register. */
    case SIOCDEVPRIVATE+2:	/* for binary compat, remove in 2.5 */
        if (!capable(CAP_NET_ADMIN))
            return -EPERM;
        armiiwrite(data->phy_id & 0x1f, 
                   data->reg_num & 0x1f, data->val_in);
        return 0;

    case SIOCSIFHWADDR:
        if (copy_from_user(dev->dev_addr, ifr->ifr_data, sizeof(dev->dev_addr)))
            return -EFAULT;
        return 0;

    case SIOCGIFHWADDR:
        if (copy_to_user(ifr->ifr_data, dev->dev_addr, sizeof(dev->dev_addr)))
            return -EFAULT;
        return 0;

    default:
        break;
    }

    return -EOPNOTSUPP;
}

static struct net_device_stats *ar2313_get_stats(struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
    return &sp->stats;
}

static short
armiiread(short phy, short reg)
{
  volatile ETHERNET_STRUCT * ethernet;

  ethernet = (volatile ETHERNET_STRUCT *)ETHERNET_BASE; /* always MAC 0 */
  ethernet->mii_addr = ((reg << MII_ADDR_REG_SHIFT) |
                        (phy << MII_ADDR_PHY_SHIFT));
  while (ethernet->mii_addr & MII_ADDR_BUSY);
  return (ethernet->mii_data >> MII_DATA_SHIFT);
}

static void
armiiwrite(short phy, short reg, short data)
{
  volatile ETHERNET_STRUCT * ethernet;

  ethernet = (volatile ETHERNET_STRUCT *)ETHERNET_BASE; /* always MAC 0 */
  while (ethernet->mii_addr & MII_ADDR_BUSY);
  ethernet->mii_data = data << MII_DATA_SHIFT;
  ethernet->mii_addr = ((reg << MII_ADDR_REG_SHIFT) |
                        (phy << MII_ADDR_PHY_SHIFT) |
                        MII_ADDR_WRITE);
}

