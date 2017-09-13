/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * drivers/audio/es8388char.c
 *
 * This is a simple character driver for testing I2C.  It is not an audio
 * driver but does conform to some of the buffer management heuristics of an
 * audio driver.  It is not suitable for use in any real driver application
 * in its current form.
 *
 *   Copyright (C) 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <sys/types.h>

#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <debug.h>
#include <errno.h>

#include <tinyara/kmalloc.h>
#include <tinyara/fs/fs.h>
#include <tinyara/audio/audio.h>
#include <tinyara/audio/i2s.h>
#include <tinyara/i2c.h>

#include <tinyara/clock.h>

#include "es8388char.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* Configuration ************************************************************/

#ifndef CONFIG_AUDIO_ES8388CHAR_RXTIMEOUT
#define CONFIG_AUDIO_ES8388CHAR_RXTIMEOUT	0
#endif

#ifndef CONFIG_AUDIO_ES8388CHAR_TXTIMEOUT
#define CONFIG_AUDIO_ES8388CHAR_TXTIMEOUT	0
#endif

#define ES8388CHAR_RXBUF_CNT	8
#define ES8388CHAR_TXBUF_CNT	8

#define FAIL	0xFFFF

/* Device naming ************************************************************/
#define DEVNAME_FMT    "/dev/es8388char%d"
#define DEVNAME_FMTLEN (16 + 3 + 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct es8388char_dev_s {
	FAR struct i2s_dev_s *i2s;	/* The lower half i2s driver */
	FAR struct i2c_dev_s *i2c;	/* i2c bus access ops */
	FAR struct i2c_config_s *i2c_config;

#ifdef AUDIO_BUFFERS_REUSE
	/*If we decide to reuse bufferes use next queues*/
	struct dq_queue_s doneq;	/* Queue of sent buffers to be reused */
	sem_t donesem;				/* Protect dq */
#endif

	struct dq_queue_s rxedq;	/* Queue of received audio IN buffers */
	sem_t rxsem;				/* Protect RX Q and rx_cnt */
	sem_t cnt_rxsem;			/* Counting semaphore to block receiving thread if no data is received yet */
	int rx_cnt;					/* Count allocated RX buffers */

	sem_t alloc;				/* Counting semaphore for Alloc TX buffers, and block allocator if needed */

	sem_t exclsem;				/* Assures mutually exclusive access */

	uint32_t samprate;			/* 8000, 44100, ... */
	uint8_t bpsamp;				/* Bits per sample: 8 bits = 8, 16 bits = 16 */
	uint8_t nchannels;			/* Mono=1, Stereo=2 */

	uint8_t volume;
	int16_t balance;			/* Current balance level */
	int16_t micgain;			/* Current mic gain level */

	bool running;				/* True: Worker thread is running */
	bool paused;				/* True: Playing is paused */
	bool mute;					/* True: Output is muted */
	bool terminating;			/* True: Stop requested */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/
/* I2S callback function */

static void es8388char_rxcallback(FAR struct i2s_dev_s *dev, FAR struct ap_buffer_s *apb, FAR void *arg, int result);
static void es8388char_txcallback(FAR struct i2s_dev_s *dev, FAR struct ap_buffer_s *apb, FAR void *arg, int result);

/* Character driver methods */

static ssize_t es8388char_read(FAR struct file *filep, FAR char *buffer, size_t buflen);
static ssize_t es8388char_write(FAR struct file *filep, FAR const char *buffer, size_t buflen);

static ssize_t es8388char_ioctl(FAR struct file *filep, int cmd, unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations es8388char_fops = {
	NULL,						/* open  */
	NULL,						/* close */
	es8388char_read,			/* read  */
	es8388char_write,			/* write */
	NULL,						/* seek  */
	es8388char_ioctl,			/* ioctl */
#ifndef CONFIG_DISABLE_POLL
	NULL,						/* poll  */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void delay(unsigned int mS)
{
	systime_t start = clock_systimer();

	mS = mS / MSEC_PER_TICK + 1;

	while (1) {
		if ((start + mS) < clock_systimer()) {
			return;
		}
	}
}

/****************************************************************************
 * Name: es8388char_readreg
 *
 * Description
 *    Read the specified 16-bit register from the es8388 device.
 *
 ****************************************************************************/
static uint8_t es8388char_readreg(FAR struct es8388char_dev_s *priv, uint8_t regaddr)
{

	int32_t ret;
	FAR struct i2c_dev_s *dev = priv->i2c;
	FAR struct i2c_config_s *es8388char_i2c_config = priv->i2c_config;


	ret = i2c_write(dev, es8388char_i2c_config, &regaddr, 1);
	if (ret < 0) {
		auddbg("Error, cannot read reg %x\n", regaddr);
		return 0;
	}

	ret = i2c_read(dev, es8388char_i2c_config, &regaddr, 1);
	if (ret < 0) {
		auddbg("Error, cannot read reg %x\n", regaddr);
		return 0;
	}

	return ret;
}

/************************************************************************************
 * Name: es8388char_writereg
 *
 * Description:
 *   Write the specified 16-bit register to the es8388 device.
 *
 ************************************************************************************/

static void es8388char_writereg(FAR struct es8388char_dev_s *priv, uint8_t regaddr, uint8_t regval)
{
	uint8_t reg[2];
	int32_t ret;
	FAR struct i2c_dev_s *dev = priv->i2c;
	FAR struct i2c_config_s *es8388char_i2c_config = priv->i2c_config;

	reg[0] = regaddr;
	reg[1] = regval;

	ret = i2c_write(dev, es8388char_i2c_config, reg, 2);
	if (ret < 0) {
		auddbg("Error, cannot write reg %x\n", regaddr);
	}
}

static uint8_t es8388char_modifyreg(FAR struct es8388char_dev_s *priv, uint8_t regaddr, uint8_t clear, uint8_t set)
{
	uint8_t data;

	data = es8388char_readreg(priv, regaddr);
	data &= ~clear;
	data |= set;

	es8388char_writereg(priv, regaddr, data);
	return es8388char_readreg(priv, regaddr);
}

static void es8388char_i2c_script(FAR struct es8388char_dev_s *priv, t_codec_init_script_entry *script, uint32_t size)
{
	uint32_t i;
	uint16_t ret;

	for (i = 0; i < size; i++) {
		ret = es8388char_modifyreg(priv, script[i].addr, 0xFF, script[i].val);
		delay(script[i].delay);
	}
}


/****************************************************************************
 * Name: es8388char_setdatawidth
 *
 * Description:
 *   Set the 8- 16- 24- bit data modes
 *
 ****************************************************************************/

static void es8388char_setdatawidth(FAR struct es8388char_dev_s *priv)
{
	I2S_TXDATAWIDTH(priv->i2s, priv->bpsamp);
}

/****************************************************************************
 * Name: es8388char_setbitrate
 *
 * Description:
 *
 ****************************************************************************/

static void es8388char_setbitrate(FAR struct es8388char_dev_s *priv)
{
	int FsRatio;	

 	auddbg("Dummy Try to set SampleRate: %d\n", priv->samprate);
	
	switch(priv->samprate) {

		case AUDIO_SAMP_RATE_8K:  
			FsRatio = 0x0A;	
			break;		
		case AUDIO_SAMP_RATE_12K: 
			FsRatio = 0x07;	
			break;		
		case AUDIO_SAMP_RATE_16K: 
			FsRatio = 0x06;	
			break;		
		case AUDIO_SAMP_RATE_24K: 
			FsRatio = 0x04;	
			break;		
		case AUDIO_SAMP_RATE_32K: 
			FsRatio = 0x03;	
			break;		
		case AUDIO_SAMP_RATE_48K: 
			FsRatio = 0x02;	
			break;		
		case AUDIO_SAMP_RATE_96K: 
			FsRatio = 0x00;	
			break;		
		default:
	 		auddbg("Not supported sample rate");
			return;
	}


	/* Init codec here */
	es8388char_i2c_script(priv, codec_init_script, sizeof(codec_init_script) / sizeof(t_codec_init_script_entry));
	es8388char_modifyreg(priv, ES8388_ADCCONTROL5, 0xff, ADCFsRatio(FsRatio));
	es8388char_modifyreg(priv, ES8388_DACCONTROL2, 0xff, DACFsRatio(FsRatio));
	es8388char_modifyreg(priv, ES8388_CHIPPOWER, 0xff, 0);
}

/************************************************************************************
 * Name: es8388char_setvolume
 *
 * Description:
 *   Set the right and left volume values in the es8388 device based on the current
 *   volume and balance settings.
 *
 ************************************************************************************/

static void es8388char_setvolume(FAR struct es8388char_dev_s *priv)
{
	uint16_t vol_l, vol_r;
	int16_t bal = priv->balance;

	/* ADD VOLUME CODE HERE */
	vol_l = priv->volume;
	vol_r = priv->volume;

	if (bal > 0) {
		vol_l = vol_l - vol_l * abs(bal) / 1000;
	}

	if (bal < 0) {
		vol_r = vol_r - vol_l * abs(bal) / 1000;
	}

/*	if (priv->mute) {
		es8388char_writereg(priv, ES8388_HPOUT_MUTE, 0x8080);
	} else {
		es8388char_writereg(priv, ES8388_HPOUT_MUTE, 0x0000);
	}

	es8388char_writereg(priv, ES8388_HPOUT_VLML, vol_l << 8);
	es8388char_writereg(priv, ES8388_HPOUT_VLMR, vol_r << 8);
	audvdbg("MUTE %x, VOLL %x VOLR %x\n", (uint32_t) es8388char_readreg(priv, ES8388_HPOUT_MUTE), (uint32_t) es8388char_readreg(priv, ES8388_HPOUT_VLML), (uint32_t) es8388char_readreg(priv, ES8388_HPOUT_VLMR));
*/
}

/************************************************************************************
 * Name: es8388char_setmic
 *
 * Description:
 *   Set Mic gain and some other related options if needed
 *
 ************************************************************************************/

static void es8388char_setmic(FAR struct es8388char_dev_s *priv)
{
//	es8388char_writereg(priv, ES8388_IN1_CTRL, (priv->micgain + 16) << 8);
//	audvdbg("MIC GAIN 0x%x\n", (uint32_t) es8388char_readreg(priv, ES8388_IN1_CTRL));
}

/****************************************************************************
 * Name: es8388char_configure
 *
 * Description:
 *   Configure the audio device for the specified  mode of operation.
 *
 ****************************************************************************/

static int es8388char_configure(FAR struct es8388char_dev_s *priv, FAR const struct audio_caps_s *caps)
{
	int ret = OK;

	DEBUGASSERT(priv && caps);
	audvdbg("ac_type: %d\n", caps->ac_type);

	/* Process the configure operation */

	switch (caps->ac_type) {
	case AUDIO_TYPE_FEATURE:
		audvdbg("  AUDIO_TYPE_FEATURE\n");

		/* Process based on Feature Unit */

		switch (caps->ac_format.hw) {
		case AUDIO_FU_VOLUME: {
			/* Set the volume 0 ~ 1000 */
			if (caps->ac_controls.hw[0] <= 1000) {
				priv->volume = 63 - 63 * caps->ac_controls.hw[0] / 1000;
			}
			es8388char_setvolume(priv);
			audvdbg("    Volume: %d\n", priv->volume);
		}
		break;
		case AUDIO_FU_MUTE: {
			/* Set the mute true/false */
			priv->mute = !!(caps->ac_controls.b[0]);
			audvdbg("    Mute: %d\n", priv->mute);
			es8388char_setvolume(priv);
		}
		break;
		case AUDIO_FU_BALANCE: {
			/* Set the balamce -1000 ~ 0 ~ 1000 */
			int16_t bal = caps->ac_controls.hw[0];

			if (abs(bal) <= 1000) {
				priv->balance = bal;
			}
			audvdbg("    Balance: %d\n", priv->balance);
			es8388char_setvolume(priv);
		}
		break;
		case AUDIO_FU_MICGAIN: {
			/* Set MIC gain */
			/* Values -16 ~ 0 ~ 53 equal to -12dB ~ 0dB ~ 39.75 */
			int16_t gain = caps->ac_controls.hw[0];

			if ((gain >= -16) && (gain <= 53)) {
				priv->micgain = gain;
				audvdbg("    Mic Gain: %d\n", priv->micgain);
				es8388char_setmic(priv);
			} else {
				audvdbg("    Mic Gain request is out of range, unchanged: %d\n", priv->micgain);
			}
		}
		break;

		default:
			auddbg("    ERROR: Unrecognized feature unit\n");
			ret = -ENOTTY;
			break;
		}
		break;

	case AUDIO_TYPE_OUTPUT: {
		audvdbg("  AUDIO_TYPE_OUTPUT:\n");
		audvdbg("    Number of channels: %u\n", caps->ac_channels);
		audvdbg("    Sample rate:        %u\n", caps->ac_controls.hw[0]);
		audvdbg("    Sample width:       %u\n", caps->ac_controls.b[2]);

		/* Verify that all of the requested values are supported */

		ret = -ERANGE;
		if (caps->ac_channels != 2) {
			auddbg("ERROR: Unsupported number of channels: %d\n", caps->ac_channels);
			break;
		}


		/* Save the current stream configuration */

		priv->samprate = caps->ac_controls.hw[0];
		priv->nchannels = caps->ac_channels;
		priv->bpsamp = caps->ac_controls.b[2];

		es8388char_setdatawidth(priv);
		es8388char_setbitrate(priv);

		ret = OK;
	}
	break;

	case AUDIO_TYPE_PROCESSING:
		break;
	}

	return ret;
}

/****************************************************************************
 * Name: es8388char_start
 *
 * Description:
 *   Configure the audio device for the specified  mode of operation.
 *
 ****************************************************************************/

static int es8388char_start(FAR struct es8388char_dev_s *priv, FAR const struct audio_caps_s *caps)
{
	int ret = OK;

//	es8388char_setdatawidth(priv);

	/* Init codec here only for debugging purposes. Remove later. */
//	es8388char_i2c_script(priv, codec_init_script, sizeof(codec_init_script) / sizeof(t_codec_init_script_entry));


	return ret;
}

static int es8388char_stop(FAR struct es8388char_dev_s *priv, FAR const struct audio_caps_s *caps)
{
	int ret = OK;
	/* Init codec here only for debugging purposes. Remove later. */

	return ret;
}

/****************************************************************************
 * Name: es8388char_rxcallback
 *
 * Description:
 *   I2S RX transfer complete callback.
 *
 *   NOTE: In this test driver, the RX is simply dumped in the bit bucket.
 *   You would not do this in a real application. You would return the
 *   received data to the caller via some IPC.
 *
 *   Also, the test buffer is simply freed.  This will work if this driver
 *   has the sole reference to buffer; in that case the buffer will be freed.
 *   Otherwise -- memory leak!  A more efficient design would recycle the
 *   audio buffers.
 *
 ****************************************************************************/

static void es8388char_rxcallback(FAR struct i2s_dev_s *dev, FAR struct ap_buffer_s *apb, FAR void *arg, int result)
{
	FAR struct es8388char_dev_s *priv = (FAR struct es8388char_dev_s *)arg;

	DEBUGASSERT(priv && apb);
	audvdbg("apb=%p nbytes=%d result=%d crefs=%d\n", apb, apb->nbytes, result, apb->crefs);

	sem_wait(&priv->rxsem);
	/* we have some data, read ... */
	dq_addlast((FAR dq_entry_t *) apb, &priv->rxedq);
	sem_post(&priv->rxsem);

	/* Let the waiting thread to capture received data */
	sem_post(&priv->cnt_rxsem);
}

/****************************************************************************
 * Name: es8388char_txcallback
 *
 * Description:
 *   I2S TX transfer complete callback
 *
 *   NOTE: The test buffer is simply freed.  This will work if this driver
 *   has the sole reference to buffer; in that case the buffer will be freed.
 *   Otherwise -- memory leak!  A more efficient design would recycle the
 *   audio buffers.
 *
 ****************************************************************************/

static void es8388char_txcallback(FAR struct i2s_dev_s *dev, FAR struct ap_buffer_s *apb, FAR void *arg, int result)
{
	FAR struct es8388char_dev_s *priv = (FAR struct es8388char_dev_s *)arg;

	DEBUGASSERT(priv && apb);
	audvdbg("apb=%p nbytes=%d result=%d crefs=%d\n", apb, apb->nbytes, result, apb->crefs);

	/* Free buffer here ... completety, if not reused */
	apb_free(apb);

	/* Let to alloc next buffer if pull was full */
	sem_post(&priv->alloc);
}

/****************************************************************************
 * Name: es8388char_read
 *
 * Description:
 *   Standard character driver read method
 *
 ****************************************************************************/

static ssize_t es8388char_read(FAR struct file *filep, FAR char *buffer, size_t buflen)
{
	return 0;
}

/****************************************************************************
 * Name: es8388char_write
 *
 * Description:
 *   Standard character driver write method
 *
 ****************************************************************************/

static ssize_t es8388char_write(FAR struct file *filep, FAR const char *buffer, size_t buflen)
{
	return 0;
}

/************************************************************************************
 * Name: es8388char_ioctl
 *
 * Description:
 *   The standard ioctl method.  This is where ALL of the Audio work is done.
 *
 ************************************************************************************/

static ssize_t es8388char_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
	FAR struct inode *inode = filep->f_inode;
	FAR struct es8388char_dev_s *priv;
	FAR struct audio_buf_desc_s *bufdesc;
	struct ap_buffer_s *apb;
	int ret;

	/* Get our private data structure */
	DEBUGASSERT(filep);

	inode = filep->f_inode;
	DEBUGASSERT(inode);

	priv = (FAR struct es8388char_dev_s *)inode->i_private;
	DEBUGASSERT(priv);

	ret = 0;

	switch (cmd) {

	/* Alloc buffer for further TX operation. Will be filled by user APP. */
	case AUDIOIOC_ALLOCBUFFER: {
		audvdbg("AUDIOIOC_ALLOCBUFFER, arg - %ld\n", arg);

		bufdesc = (FAR struct audio_buf_desc_s *)arg;
		/* We will be waiting here is if we reached max number of allocated TX buffers */
		sem_wait(&priv->alloc);
		ret = apb_alloc(bufdesc);
	}
	break;

	/* Free buffer we received at DEQUEUE with TXed data. */
	case AUDIOIOC_FREEBUFFER: {
		audvdbg("AUDIOIOC_FREEBUFFER, arg - %ld\n", arg);

		bufdesc = (FAR struct audio_buf_desc_s *)arg;
		DEBUGASSERT(bufdesc->u.pBuffer != NULL);

		apb_free(bufdesc->u.pBuffer);

		/* decrement number of allocated rx buffers */
		DEBUGASSERT(priv->rx_cnt > 0);

		sem_wait(&priv->rxsem);
		priv->rx_cnt--;
		sem_post(&priv->rxsem);

		ret = sizeof(struct audio_buf_desc_s);
	}
	break;

	/* Put buffer into TX queue. callback will be called after buffer is transferred */
	case AUDIOIOC_ENQUEUEBUFFER: {
		audvdbg("AUDIOIOC_ENQUEUEBUFFER, arg - %ld\n", arg);

		bufdesc = (FAR struct audio_buf_desc_s *)arg;
		ret = I2S_SEND(priv->i2s, bufdesc->u.pBuffer, es8388char_txcallback, priv, CONFIG_AUDIO_ES8388CHAR_TXTIMEOUT);

	}
	break;

	/* Request RXed data. Before receive anything we allocate and enqueue empty RX buffer for receiving */
	case AUDIOIOC_DEQUEUEBUFFER: {
		audvdbg("AUDIOIOC_DEQUEUEBUFFER, arg - %ld\n", arg);
		bufdesc = (FAR struct audio_buf_desc_s *)arg;

		/* Try to allocate needed buffers and enqueue in receive queue */
		while (priv->rx_cnt < ES8388CHAR_RXBUF_CNT) {

			if (bufdesc->numbytes == 0) {
				bufdesc->numbytes = 1024 * 16;
			}

			bufdesc->u.ppBuffer = &apb;
			ret = apb_alloc(bufdesc);

			if (ret < 0) {
				break;
			}

			ret = I2S_RECEIVE(priv->i2s, apb, es8388char_rxcallback, priv, CONFIG_AUDIO_ES8388CHAR_RXTIMEOUT);

			if (ret < 0) {
				apb_free(apb);
				break;
			}
			/* Increment counter of allocated RX buffers */
			sem_wait(&priv->rxsem);
			priv->rx_cnt++;
			sem_post(&priv->rxsem);
		}

		/* Clean buffer pointer to prevent wrong accesses */
		bufdesc->u.ppBuffer = NULL;

		/* We didn't hit any error before, so try to read any data from rx queue */
		if (ret >= 0) {
			/* Will be blocked here if no data is available to read yet */
			sem_wait(&priv->cnt_rxsem);

			/* we have some data, read ... */
			sem_wait(&priv->rxsem);
			apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->rxedq);
			sem_post(&priv->rxsem);

			bufdesc->u.pBuffer = apb;
			ret = 0;
		}
		break;
	}
	break;

	case AUDIOIOC_CONFIGURE: {
		audvdbg("AUDIOIOC_CONFIGURE, arg - %ld\n", arg);
		ret = es8388char_configure(priv, (FAR const struct audio_caps_s *)arg);
	}
	break;

	case AUDIOIOC_START: {
		audvdbg("AUDIOIOC_START, arg - %ld\n", arg);
		ret = es8388char_start(priv, (FAR const struct audio_caps_s *)arg);
	}
	break;

	case AUDIOIOC_STOP: {
		audvdbg("AUDIOIOC_STOP, arg - %ld\n", arg);
		ret = es8388char_stop(priv, (FAR const struct audio_caps_s *)arg);
	}
	break;

	default:
		audvdbg("NOT SUPPORTED COMMAND %X, arg - %ld\n", cmd, arg);
	}

	return ret;

}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: es8388char_register
 *
 * Description:
 *   Create and register the I2S character driver.
 *
 *   The I2S character driver is a simple character driver that supports I2S
 *   transfers via a read() and write().  The intent of this driver is to
 *   support I2S testing.  It is not an audio driver but does conform to some
 *   of the buffer management heuristics of an audio driver.  It is not
 *   suitable for use in any real driver application in its current form.
 *
 * Input Parameters:
 *   i2s - An instance of the lower half I2S driver
 *   minor - The device minor number.  The I2S character device will be
 *     registers as /dev/es8388charN where N is the minor number
 *
 * Returned Value:
 *   OK if the driver was successfully register; A negated errno value is
 *   returned on any failure.
 *
 ****************************************************************************/

int es8388char_register(FAR struct i2s_dev_s *i2s, FAR struct i2c_dev_s *i2c, FAR struct i2c_config_s *i2c_config, int minor)
{
	FAR struct es8388char_dev_s *priv;
	char devname[DEVNAME_FMTLEN];
	int ret;
//	uint16_t regval;

	/* Sanity check */

	DEBUGASSERT(i2s != NULL && i2c != NULL && (unsigned)minor < 1000);

	/* Allocate a I2S character device structure */

	priv = (FAR struct es8388char_dev_s *)kmm_zalloc(sizeof(struct es8388char_dev_s));
	if (priv) {
		/* Initialize the I2S character device structure */

		priv->i2s = i2s;
		priv->i2c = i2c;
		priv->i2c_config = i2c_config;

		sem_init(&priv->exclsem, 0, 1);

		sem_init(&priv->alloc, 0, ES8388CHAR_TXBUF_CNT);

		dq_init(&priv->rxedq);	/* Queue of received audio IN buffers */
		sem_init(&priv->rxsem, 0, 1);	/* Protect dq */
		sem_init(&priv->cnt_rxsem, 0, 0);	/* Assume we did not receive anything yet!!! */
		priv->rx_cnt = 0;		/* Count allocated RX buffers */

//		regval = es8388char_readreg(priv, ES8388_RESET);
//		if (regval != 0) {
//			auddbg("ERROR: es8388 not found: ID=%04x\n", regval);
//			return -ENODEV;
//		}

		/* Create the character device name */
		snprintf(devname, DEVNAME_FMTLEN, DEVNAME_FMT, minor);
		ret = register_driver(devname, &es8388char_fops, 0666, priv);
		if (ret < 0) {
			/* Free the device structure if we failed to create the character
			 * device.
			 */

			kmm_free(priv);
		}

		/* Init codec by IOCTL call later, with required options */

		/* Return the result of the registration */

		return OK;
	}

	return -ENOMEM;
}