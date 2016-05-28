/**
 * @file
 *
 * @date 14.05.2016
 * @author Anton Bondarev
 */
#include <errno.h>
#include <stdint.h>
#include <util/log.h>

#include <asm/io.h>

#include <drivers/audio/portaudio.h>

#include <kernel/lthread/lthread.h>

#include <drivers/pci/pci.h>
#include <kernel/irq.h>
#include <drivers/pci/pci_driver.h>

#include "es1370.h"

struct es1370_hw_dev {
	uint32_t base_addr;
};

struct es1370_hw_dev es1370_hw_dev;


static int fragment_size = 0x8000;

int es1370_set_int_cnt(int chan) {
	uint32_t base_addr;

	assert(chan == 0);
	base_addr = es1370_hw_dev.base_addr;

	/* Write interrupt count for specified channel.
	   After <DspFragmentSize> bytes, an interrupt will be generated  */
	uint32_t sample_count;
	uint32_t int_cnt_reg;

	switch(chan) {
		case ADC1_CHAN: int_cnt_reg = ES1370_REG_ADC_SCOUNT; break;
		case DAC1_CHAN: int_cnt_reg = ES1370_REG_DAC1_SCOUNT; break;
		case DAC2_CHAN: int_cnt_reg = ES1370_REG_DAC2_SCOUNT; break;
		default: return EINVAL;
	}

	sample_count = fragment_size;

	/* TODO adjust sample count according to sample format */

	/* set the sample count - 1 for the specified channel. */
	out32(sample_count - 1, base_addr + int_cnt_reg);

	return 0;
}


int es1370_setup_dma(void *dma_buff, uint32_t length, int chan) {
	/* dma length in bytes,
	 * max is 64k long words for es1370 = 256k bytes
	 */
	uint32_t page, frame_count_reg, dma_add_reg;
	uint32_t base_addr;

	assert(chan == 0);
	base_addr = es1370_hw_dev.base_addr;

	switch(chan) {
		case ADC1_CHAN:
			page = ADC_MEM_PAGE;
			frame_count_reg = ES1370_REG_ADC_BUFFER_SIZE;
			dma_add_reg = ES1370_REG_ADC_PCI_ADDRESS;
			break;
		case DAC1_CHAN:
			page = DAC_MEM_PAGE;
			frame_count_reg = ES1370_REG_DAC1_BUFFER_SIZE;
			dma_add_reg = ES1370_REG_DAC1_PCI_ADDRESS;
			break;
		case DAC2_CHAN:
			page = DAC_MEM_PAGE;
			frame_count_reg = ES1370_REG_DAC2_BUFFER_SIZE;
			dma_add_reg = ES1370_REG_DAC2_PCI_ADDRESS;
			break;
		default:
			return -EINVAL;
	}
	out8(page, base_addr + ES1370_REG_MEMPAGE);
	out32(dma_buff, base_addr + dma_add_reg);

	/* device expects long word count in stead of bytes */
	length /= 4;

	/* It seems that register _CURRENT_COUNT is overwritten, but this is
	 * the way to go. The register frame_count_reg is only longword
	 * addressable.
	 * It expects length -1
	 */
	out32((uint32_t) (length - 1), base_addr + frame_count_reg);

	return 0;
}


static int es1370_disable_int(int chan) {
	uint16_t ser_interface, int_en_bit;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	switch(chan) {
		case ADC1_CHAN: int_en_bit = SCTRL_R1INTEN; break;
		case DAC1_CHAN: int_en_bit = SCTRL_P1INTEN; break;
		case DAC2_CHAN: int_en_bit = SCTRL_P2INTEN; break;
		default: return EINVAL;
	}
	/* clear the interrupt */
	ser_interface = in16(base_addr + ES1370_REG_SERIAL_CONTROL);

	out16(ser_interface & ~int_en_bit, base_addr + ES1370_REG_SERIAL_CONTROL);

	return 0;
}

int es1370_drv_reenable_int(int chan) {
	uint32_t ser_interface, int_en_bit;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	switch(chan) {
		case ADC1_CHAN: int_en_bit = SCTRL_R1INTEN; break;
		case DAC1_CHAN: int_en_bit = SCTRL_P1INTEN; break;
		case DAC2_CHAN: int_en_bit = SCTRL_P2INTEN; break;
		default: return EINVAL;
	}

	/* clear and reenable an interrupt */
	ser_interface = in32(base_addr + ES1370_REG_SERIAL_CONTROL);
	out32(ser_interface & ~int_en_bit, base_addr + ES1370_REG_SERIAL_CONTROL);
	out32(ser_interface | int_en_bit, base_addr + ES1370_REG_SERIAL_CONTROL);

	return 0;
}

int es1370_drv_pause(int sub_dev) {
	uint32_t pause_bit;
	uint32_t sctrl;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	es1370_disable_int(sub_dev); /* don't send interrupts */

	switch(sub_dev) {
		case DAC1_CHAN: pause_bit = SCTRL_P1PAUSE;break;
		case DAC2_CHAN: pause_bit = SCTRL_P2PAUSE;break;
		default: return EINVAL;
	}

	sctrl = in32(base_addr + ES1370_REG_SERIAL_CONTROL);
	/* pause */
	out32(sctrl | pause_bit, base_addr + ES1370_REG_SERIAL_CONTROL);

	return 0;
}


int es1370_drv_resume(int sub_dev) {
	uint32_t pause_bit = 0;
	uint32_t sctrl;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	es1370_drv_reenable_int(sub_dev); /* enable interrupts */

	switch(sub_dev) {
		case DAC1_CHAN:
			pause_bit = SCTRL_P1LOOPSEL | SCTRL_P1PAUSE | SCTRL_P1SCTRLD ;
			break;
		case DAC2_CHAN:
			pause_bit = SCTRL_P2LOOPSEL | SCTRL_P2PAUSE | SCTRL_P2DACSEN | SCTRL_P2ENDINC | SCTRL_P2STINC;
			break;
		default: return EINVAL;
	}

	sctrl = in32(base_addr + ES1370_REG_SERIAL_CONTROL);
	/* clear pause bit */
	out32(sctrl & ~pause_bit, base_addr + ES1370_REG_SERIAL_CONTROL);

	return 0;
}

static int set_sample_rate(uint32_t rate, int sub_dev) {
	/* currently only 44.1kHz */
	uint32_t controlRegister;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	controlRegister = in32(base_addr + ES1370_REG_CONTROL);
	controlRegister |= CTRL_WTSRSEL | CTRL_DAC1_EN;
	out32(controlRegister, base_addr + ES1370_REG_CONTROL);

	return 0;
}

static int set_bits(uint32_t nr_of_bits, int sub_dev) {
	/* set format bits for specified channel. */
	uint16_t size_16_bit, ser_interface;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	switch(sub_dev) {
		case ADC1_CHAN: size_16_bit = SCTRL_R1SEB; break;
		case DAC1_CHAN: size_16_bit = SCTRL_P1SEB; break;
		case DAC2_CHAN: size_16_bit = SCTRL_P2SEB; break;
		default: return EINVAL;
	}

	ser_interface = in32(base_addr + ES1370_REG_SERIAL_CONTROL);
	ser_interface &= ~size_16_bit;
	switch(nr_of_bits) {
		case 16: ser_interface |= size_16_bit;break;
		case  8: break;
		default: return EINVAL;
	}
	out32(ser_interface, base_addr + ES1370_REG_SERIAL_CONTROL);

	return 0;
}

static int set_stereo(uint32_t stereo, int sub_dev) {
	/* set format bits for specified channel. */
	uint32_t stereo_bit, ser_interface;
	uint32_t base_addr;

	base_addr = es1370_hw_dev.base_addr;

	switch(sub_dev) {
		case ADC1_CHAN: stereo_bit = SCTRL_R1SMB; break;
		case DAC1_CHAN: stereo_bit = SCTRL_P1SMB; break;
		case DAC2_CHAN: stereo_bit = SCTRL_P2SMB; break;
		default: return EINVAL;
	}
	ser_interface = in32(base_addr + ES1370_REG_SERIAL_CONTROL);
	ser_interface &= ~stereo_bit;
	if (stereo) {
		ser_interface |= stereo_bit;
	}
	out32(ser_interface, base_addr + ES1370_REG_SERIAL_CONTROL);

	return 0;
}

int es1370_drv_start(int sub_dev) {
	//uint32_t enable_bit;
	int result = 0;
	//uint32_t status;
	//uint32_t base_addr;

	//base_addr = es1370_hw_dev.base_addr;

	assert(sub_dev == DAC1_CHAN);

	/* Write default values to device in case user failed to configure.
	   If user did configure properly, everything is written twice.
	   please raise your hand if you object against to this strategy...*/
	//result |= set_sample_rate(44100, sub_dev);
	result |= set_stereo(1, sub_dev);
	result |= set_bits(16, sub_dev);

	/* set the interrupt count */
	result |= es1370_set_int_cnt(sub_dev);

	if (result) {
		return EIO;
	}

	/* if device currently paused, resume */
	es1370_drv_resume(sub_dev);
/*	switch(sub_dev) {
		case ADC1_CHAN: enable_bit = CTRL_ADC_EN;break;
		case DAC1_CHAN: enable_bit = CTRL_DAC1_EN;break;
		case DAC2_CHAN: enable_bit = CTRL_DAC2_EN;break;
		default: return EINVAL;
	}
*/
	/* enable interrupts from 'sub device' */
	es1370_drv_reenable_int(sub_dev);
	result |= set_sample_rate(44100, sub_dev);

	//status = in32(base_addr + ES1370_REG_STATUS);
	//log_debug("\n******************************");
	//log_debug("START en_bit = enable_bit %x", enable_bit);
	//log_debug("*******************************\n");
	/* this means play!!! */
	//out32(status | enable_bit, base_addr + ES1370_REG_STATUS);


	return 0;
}

static irq_return_t es1370_interrupt(unsigned int irq_num, void *dev_id) {
	struct es1370_hw_dev *hw_dev;
	uint32_t base_addr;
	uint32_t status, sctl;

	hw_dev = dev_id;
	base_addr = hw_dev->base_addr;

	status = in32(base_addr + ES1370_REG_STATUS);
	if (!(status & STAT_INTR)) {
		return IRQ_NONE;
	}
	sctl = in32(base_addr + ES1370_REG_SERIAL_CONTROL);

	if (status & STAT_ADC)
		sctl &= ~SCTRL_R1INTEN;
	if (status & STAT_DAC1)
		sctl &= ~SCTRL_P1INTEN;
	if (status & STAT_DAC2)
		sctl &= ~SCTRL_P2INTEN;

	out32(sctl, base_addr + ES1370_REG_SERIAL_CONTROL);
	//outl(s->sctrl, s->io+ES1370_REG_SERIAL_CONTROL);
	//lthread_launch(&es1370_lthread);
	Pa_StartStream(NULL);

	log_debug("irq status #%X\n\n", status);

	return IRQ_HANDLED;
}

extern int ak4531_init(uint32_t base_addr, uint32_t status_addr, uint32_t poll_addr);
static int es1370_hw_init(uint32_t base_addr) {
	uint32_t chip_sel_ctrl_reg;
	int i,j;

	/* turn everything off */
	out32(0, base_addr + ES1370_REG_CONTROL);

	/* turn off legacy (legacy control is undocumented) */
	out32(0, base_addr + ES1370_REG_LEGACY);
	out32(0, base_addr + ES1370_REG_LEGACY + 4);

	/* turn off serial interface */
	out32(0, base_addr + ES1370_REG_SERIAL_CONTROL);

	/* enable the codec */
	chip_sel_ctrl_reg = in32(base_addr + ES1370_REG_CONTROL);
	chip_sel_ctrl_reg |= CTRL_XCTL0 | CTRL_CDC_EN;
	out32(chip_sel_ctrl_reg, base_addr + ES1370_REG_CONTROL);

	if (ak4531_init(base_addr + ES1370_REG_CODEC, base_addr + ES1370_REG_STATUS, base_addr + ES1370_REG_STATUS)) {
		log_error("Could not init ak4531\n");
	}

	/* clear all the memory */
	for (i = 0; i < 0x10; ++i) {
		out8(i, base_addr + ES1370_REG_MEMPAGE);
		for (j = 0; j < 0x10; j += 4) {
			out32(0x0UL, base_addr + ES1370_REG_MEMORY + j);
		}
	}

	return 0;
}

static int es1370_init(struct pci_slot_dev *pci_dev) {
	int err;

	pci_set_master(pci_dev);

	es1370_hw_dev.base_addr = pci_dev->bar[0] & 0xFF00;

	if ((err = irq_attach(pci_dev->irq, es1370_interrupt, IF_SHARESUP, &es1370_hw_dev, "iac"))) {
		log_error("can't alloc irq");
		return err;
	}

	es1370_hw_init(es1370_hw_dev.base_addr);

	return 0;
}

PCI_DRIVER("es1370 Audio Controller", es1370_init, 0x1274, 0x5000);