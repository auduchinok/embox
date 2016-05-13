/**
 * @file imx6_net.c
 * @brief iMX6 MAC-NET driver
 * @author Denis Deryugin <deryugin.denis@gmail.com>
 * @version 0.1
 * @date 2016-05-11
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <hal/reg.h>

#include <kernel/irq.h>
#include <kernel/printk.h>

#include <net/inetdevice.h>
#include <net/l0/net_entry.h>
#include <net/l2/ethernet.h>
#include <net/netdevice.h>
#include <net/skbuff.h>

#include <embox/unit.h>

#include <framework/mod/options.h>

#define NIC_BASE OPTION_GET(NUMBER, base_addr)

#define ENET_EIR  (NIC_BASE + 0x0004)
#define ENET_EIMR (NIC_BASE + 0x0008)
#define ENET_TDAR (NIC_BASE + 0x0014)
#define ENET_ECR  (NIC_BASE + 0x0024)
#define ENET_RCR  (NIC_BASE + 0x0084)
#define ENET_TCR  (NIC_BASE + 0x00C4)
#define MAC_LOW   (NIC_BASE + 0x00E4)
#define MAC_HI    (NIC_BASE + 0x00E8)
#define ENET_RDSR (NIC_BASE + 0x0180)
#define ENET_TDSR (NIC_BASE + 0x0184)
#define ENET_MRBR (NIC_BASE + 0x0188)

/* Various flags */
/* ENET_EIR */
#define EIR_MASK  0x7FFF8000

/* ENET_EIMR */
#define EIMR_RXF  (1 << 25)
#define EIMR_TXF  (1 << 27)

/* ENET_ECR */
#define ETHEREN   (1 << 1) /* Ethernet enable */
#define RESET     (1 << 0)

#define FRAME_LEN     1588
#define TX_BUF_FRAMES 256
#define TX_BUF_LEN    (FRAME_LEN * TX_BUF_FRAMES)
#define RX_BUF_FRAMES 256
#define RX_BUF_LEN    (FRAME_LEN * TX_BUF_FRAMES)

/* Descriptor flags */
/* TX */
/* Flags 1 */
#define FLAG_R      (1 << 15)
#define FLAG_TO1    (1 << 14)
#define FLAG_W      (1 << 13)
#define FLAG_TO2    (1 << 12)
#define FLAG_L      (1 << 11)
#define FLAG_TC     (1 << 10)
/* Flags 2 */
#define FLAG_TSE    (1 << 8)
#define FLAG_OE     (1 << 9)
#define FLAG_LCE    (1 << 10)
#define FLAG_FE     (1 << 11)
#define FLAG_EE     (1 << 12)
#define FLAG_UE     (1 << 13)
#define FLAG_TXE    (1 << 15)
#define FLAG_IINS   (1 << 27)
#define FLAG_PINS   (1 << 28)
#define FLAG_TS     (1 << 29)
#define FLAG_INT_TX (1 << 30)
/* Flags 3 */
#define FLAG_BDU    (1 << 15)

/* RX */
/* Flags 1 */
#define FLAG_TR     (1 << 0)
#define FLAG_OV     (1 << 1)
#define FLAG_CR     (1 << 2)
#define FLAG_NO     (1 << 4)
#define FLAG_LG     (1 << 5)
#define FLAG_MC     (1 << 6)
#define FLAG_BC     (1 << 7)
#define FLAG_M      (1 << 8)
#define FLAG_RO2    (1 << 12)
#define FLAG_RO1    (1 << 14)
#define FLAG_E      (1 << 15)
/* Flags 2 */
#define FLAG_FRAG   (1 << 0)
#define FLAG_IPV6   (1 << 1)
#define FLAG_VLAN   (1 << 2)
#define FLAG_PCR    (1 << 4)
#define FLAG_ICE    (1 << 5)
#define FLAG_INT_RX (1 << 23)
#define FLAG_UC     (1 << 24)
#define FLAG_CE     (1 << 25)
#define FLAG_PE     (1 << 26)
#define FLAG_ME     (1 << 31)

static void _reg_dump(void) {
	printk("ENET_EIR  %10x\n", REG32_LOAD(ENET_EIR ));
	printk("ENET_EIMR %10x\n", REG32_LOAD(ENET_EIMR));
	printk("ENET_TDAR %10x\n", REG32_LOAD(ENET_TDAR));
	printk("ENET_ECR  %10x\n", REG32_LOAD(ENET_ECR ));
	printk("ENET_RCR  %10x\n", REG32_LOAD(ENET_RCR ));
	printk("ENET_TCR  %10x\n", REG32_LOAD(ENET_TCR ));
	printk("MAC_LOW   %10x\n", REG32_LOAD(MAC_LOW  ));
	printk("MAC_HI    %10x\n", REG32_LOAD(MAC_HI   ));
	printk("ENET_RDSR %10x\n", REG32_LOAD(ENET_RDSR));
	printk("ENET_TDSR %10x\n", REG32_LOAD(ENET_TDSR));
	printk("ENET_MRBR %10x\n", REG32_LOAD(ENET_MRBR));
}

static uint8_t _macaddr[6];

static void _write_macaddr(void) {
	uint32_t mac_hi, mac_lo;

	mac_hi  = (_macaddr[5] << 16) |
	          (_macaddr[4] << 24);
	mac_lo  = (_macaddr[3] <<  0) |
	          (_macaddr[2] <<  8) |
	          (_macaddr[1] << 16) |
	          (_macaddr[0] << 24);

	REG32_STORE(MAC_LOW, mac_lo);
	REG32_STORE(MAC_HI, mac_hi);
}

struct imx6_buf_desc {
	uint16_t len;
	uint16_t flags1;
	uint32_t data_pointer;
	uint32_t flags2;
	uint16_t payload_checksum; /* Used only for RX */
	uint8_t  protocol;         /* Used only for RX */
	uint8_t  header_len;       /* Used only for RX */
	uint16_t pad1;             /* Unused */
	uint16_t flags3;           /* Used only for RX */
	uint32_t timestamp;
	uint16_t pad2[4];          /* Unused */
} __attribute__ ((aligned(16)));

static struct imx6_buf_desc _tx_desc_ring[TX_BUF_FRAMES];
static struct imx6_buf_desc _rx_desc_ring[RX_BUF_FRAMES];

static uint8_t _tx_buf[TX_BUF_LEN] __attribute__ ((aligned(16)));
static uint8_t _rx_buf[RX_BUF_LEN] __attribute__ ((aligned(16)));

//extern void dcache_inval(const void *p, size_t size);
//extern void dcache_flush(const void *p, size_t size);

#define dcache_flush(x, y) ;
#define dcache_inval(x, y) ;

static int imx6_net_xmit(struct net_device *dev, struct sk_buff *skb) {
	uint8_t *data;
	int desc_num;
	ipl_t sp;

	assert(dev);
	assert(skb);

	sp = ipl_save();
	{
		data = (uint8_t*) skb_data_cast_in(skb->data);

		if (!data)
			return -1;

		skb_free(skb);

		desc_num = 0;
		memcpy(&_tx_buf[0], data, skb->len);
		dcache_flush(&_tx_buf[0], skb->len);

		memset(&_tx_desc_ring[desc_num], 0, sizeof(struct imx6_buf_desc));
		_tx_desc_ring[desc_num] = (struct imx6_buf_desc) {
			.len          = skb->len,
			.flags1       = FLAG_R | FLAG_L | FLAG_TC,
			.data_pointer = (uint32_t) &_tx_buf[0]
		};
		dcache_flush(&_tx_desc_ring[desc_num], sizeof(struct imx6_buf_desc));

		REG32_STORE(ENET_TDAR, 0);
	}
	ipl_restore(sp);

	int t = 0xFFFF;
	while (t--);
	printk("Errcode %#x\n", REG32_LOAD(ENET_EIR));

	return 0;
}

static void _init_buffers() {
	struct imx6_buf_desc *desc;

	memset(&_tx_desc_ring[0], 0,
	        TX_BUF_FRAMES * sizeof(struct imx6_buf_desc));
	memset(&_rx_desc_ring[0], 0,
	        RX_BUF_FRAMES * sizeof(struct imx6_buf_desc));

	/* Mark last buffer (i.e. set wrap flag) */
	_rx_desc_ring[RX_BUF_FRAMES - 1].flags1 = FLAG_W;
	_tx_desc_ring[TX_BUF_FRAMES - 1].flags1 = FLAG_W;

	for (int i = 0; i < RX_BUF_FRAMES; i++) {
		desc = &_rx_desc_ring[i];
		desc->data_pointer = (uint32_t) _rx_buf[i * FRAME_LEN];
		desc->flags1 |= FLAG_E;
		desc->flags2 |= FLAG_INT_RX;
	}

	for (int i = 0; i < TX_BUF_FRAMES; i++) {
		_tx_desc_ring[i].flags2 |= FLAG_INT_TX;
	}

	dcache_flush(&_tx_desc_ring[0],
	              TX_BUF_FRAMES * sizeof(struct imx6_buf_desc));
	dcache_flush(&_rx_desc_ring[0],
	              RX_BUF_FRAMES * sizeof(struct imx6_buf_desc));
}

static void _reset(void) {
	REG32_STORE(ENET_ECR, RESET);

	/* Clear pending interrupts */
	REG32_STORE(ENET_EIR, EIR_MASK);

	assert((RX_BUF_LEN & 0xF) == 0);
	REG32_STORE(ENET_MRBR, RX_BUF_LEN);

	assert((((uint32_t) &_tx_desc_ring[0]) & 0xF) == 0);
	REG32_STORE(ENET_TDSR, ((uint32_t) &_tx_desc_ring[0]));

	assert((((uint32_t) &_rx_desc_ring[0]) & 0xF) == 0);
	REG32_STORE(ENET_RDSR, ((uint32_t) &_rx_desc_ring[0]));

	_init_buffers();

	REG32_STORE(ENET_EIMR, EIMR_TXF | EIMR_RXF | EIR_MASK);

	_write_macaddr();

	REG32_STORE(ENET_ECR, ETHEREN); /* Note: should be last init step */

	_reg_dump();
}

static int imx6_net_open(struct net_device *dev) {
	_reset();

	return 0;
}



static int imx6_net_set_macaddr(struct net_device *dev, const void *addr) {
	assert(dev);
	assert(addr);

	memcpy(&_macaddr, (uint8_t *) addr, 6);
	_write_macaddr();

	_reg_dump();
	return 0;
}

static const struct net_driver imx6_net_drv_ops = {
	.xmit = imx6_net_xmit,
	.start = imx6_net_open,
	.set_macaddr = imx6_net_set_macaddr
};

EMBOX_UNIT_INIT(imx6_net_init);
static int imx6_net_init(void) {
	struct net_device *nic;

	if (NULL == (nic = etherdev_alloc(0))) {
                return -ENOMEM;
        }

	nic->drv_ops = &imx6_net_drv_ops;

	_reset();

	return inetdev_register_dev(nic);
}