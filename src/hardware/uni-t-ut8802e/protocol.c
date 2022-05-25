/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2012-2013 Uwe Hermann <uwe@hermann-uwe.de>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <string.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"
#include "protocol.h"

/*
 * Driver for the UNI-T UT8802E multimeter.
 *
 * This driver is meant to support Silicon Labs CP2110 HID-to-UART chipset
 * used in the UT8802E multimeter.
 *
 * CP2110 Datasheet:
 * https://www.silabs.com/documents/public/application-notes/an434-cp2110-4-interface-specification.pdf
 *
 * A DMM packet is 8-byte.
 * The data for one DMM packet is spread across multiple HID chunks.
 * A DMM packet is complete once we got 8 bytes of actual data.
 * Every packet starts with a 0xAC byte.
 *
 * A HID data chunk looks like this:
 *
 * - Byte 0: 0x0z, where z is the number of actual data bytes in this chunk.
 * - Bytes 1-z: z data bytes.
 *
 * Example of a complete DMM packet:
 *
 * - 1 byte: 0xAC magic marker
 * - 1 byte: the selected mode and range
 * - 1 byte: ---XX digits
 * - 1 byte: -XX-- digits
 * - 1 byte: X---- digit (5 digits max. total)
 * - 1 byte: 0x30 & number of digits after decimal point
 * - 1 byte: flags (min, max, hold rel, OL, sign)
 * - 1 byte: checksum (probably - TODO)
 */

static void decode_packet(struct sr_dev_inst *sdi, const uint8_t *buf)
{
	struct dev_context *devc;
	struct dmm_info *dmm;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_analog analog;
	struct sr_analog_encoding encoding;
	struct sr_analog_meaning meaning;
	struct sr_analog_spec spec;
	float floatval;
	void *info;
	int ret;

	devc = sdi->priv;
	dmm = (struct dmm_info *)sdi->driver;
	/* Note: digits/spec_digits will be overridden by the DMM parsers. */
	sr_analog_init(&analog, &encoding, &meaning, &spec, 0);
	info = g_malloc(dmm->info_size);

	/* Parse the protocol packet. */
	ret = dmm->packet_parse(buf, &floatval, &analog, info);
	if (ret != SR_OK) {
		sr_dbg("Invalid DMM packet, ignoring.");
		g_free(info);
		return;
	}

	/* If this DMM needs additional handling, call the resp. function. */
	if (dmm->dmm_details)
		dmm->dmm_details(&analog, info);

	g_free(info);

	/* Send a sample packet with one analog value. */
	analog.meaning->channels = sdi->channels;
	analog.num_samples = 1;
	analog.data = &floatval;
	packet.type = SR_DF_ANALOG;
	packet.payload = &analog;
	sr_session_send(sdi, &packet);

	sr_sw_limits_update_samples_read(&devc->limits, 1);
}

static int hid_chip_init(struct sr_dev_inst *sdi)
{
	int ret;
	uint8_t buf[64];
	struct sr_usb_dev_inst *usb;

	usb = sdi->conn;

	sr_dbg("Initializing UART...");

	if (libusb_kernel_driver_active(usb->devhdl, 0) == 1) {
		ret = libusb_detach_kernel_driver(usb->devhdl, 0);
		if (ret < 0) {
			sr_err("Failed to detach kernel driver: %s.",
			       libusb_error_name(ret));
			return SR_ERR;
		}
	}

	if ((ret = libusb_claim_interface(usb->devhdl, 0)) < 0) {
		sr_err("Failed to claim interface 0: %s.",
		       libusb_error_name(ret));
		return SR_ERR;
	}

	/* UART Enable */
	buf[0] = 0x41; /* Report ID */
	buf[1] = 0x01; /* Argument: 0x1 - ON */
	ret = libusb_control_transfer(
		usb->devhdl,
		LIBUSB_ENDPOINT_OUT | 0x21, /* Endpoint (should be 0x21 - FIXME: why?) */
		0x09,                       /* Set Report */
		(0x3 << 8) | 0x41,          /* UART Enable */
		0,                          /* wIndex */
		(unsigned char*)&buf,       /* payload */
		2,                          /* wLength */
		1000);

	if (ret < 0) {
		sr_err("Failed to enable the UART (%s)", libusb_error_name(ret));
		return SR_ERR;
	}

	/* Set UART Config */
	buf[0] = 0x50; /* Report ID */

	/* baud rate, MSB first */
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x25;
	buf[4] = 0x80;

	/* Parity (fucked up ?) */
	buf[5] = 0x00;

	/* Flow Control */
	buf[6] = 0x00;

	/* Data Bits (8 data bits) */
	buf[7] = 0x03;

	/* Stop Bits */
	buf[8] = 0x0;

	ret = libusb_control_transfer(
		usb->devhdl,
		LIBUSB_ENDPOINT_OUT | 0x21, /* Endpoint (0x21 - FIXME: why?) */
		0x09,                       /* Set Report */
		(0x3 << 8) | 0x50,          /* Set UART Config */
		0,                          /* wIndex */
		(unsigned char *)&buf,      /* payload */
		9,                          /* wLength */
		1000);

	if (ret < 0) {
		sr_err("Set UART Config Failed (%s)", libusb_error_name(ret));
		return SR_ERR;
	}

	/* Purge FIFOs */
	buf[0] = 0x43;
	buf[1] = 0x03;

	ret = libusb_control_transfer(
		usb->devhdl,
		LIBUSB_ENDPOINT_OUT | 0x21, /* Endpoint (0x21 - FIXME: why?) */
		0x09,                       /* Set Report */
		(0x3 << 8) | 0x43,          /* Purge FIFOs */
		0,                          /* wIndex */
		(unsigned char *)&buf,      /* payload */
		2,                          /* wLength */
		1000);

	if (ret < 0) {
		sr_err("Purge FIFOs Failed");
		return SR_ERR;
	}

	return SR_OK;
}

static void log_chunk(const uint8_t *buf, int len)
{
	sr_dbg("HID chunk data:");
	for (int i = 0; i < len; i++) {
		sr_spew("[%i]: %02x", i, buf[i]);
	}
}

static void log_dmm_packet(const uint8_t *buf)
{
	GString *text;

	text = sr_hexdump_new(buf, UT8802E_PACKET_SIZE);
	sr_dbg("DMM packet:   %s", text->str);
	sr_hexdump_free(text);
}

static int get_and_handle_data(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct dmm_info *dmm;
	uint8_t buf[CHUNK_SIZE], *pbuf;
	int i, ret, len, num_databytes_in_chunk;
	struct sr_usb_dev_inst *usb;

	devc = sdi->priv;
	dmm = (struct dmm_info *)sdi->driver;
	usb = sdi->conn;
	pbuf = devc->protocol_buf;

	/* On the first run, we need to init the HID chip. */
	if (devc->first_run) {
		if ((ret = hid_chip_init(sdi)) != SR_OK) {
			sr_err("HID chip init failed: %d.", ret);
			return SR_ERR;
		}
		memset(pbuf, 0x00, DMM_BUFSIZE);
		devc->first_run = FALSE;
	}

	memset(&buf, 0x00, CHUNK_SIZE);

	ret = libusb_interrupt_transfer(
		usb->devhdl,
		LIBUSB_ENDPOINT_IN | 0x1,
		(unsigned char *)&buf,
		CHUNK_SIZE,
		&len,
		1000);

	if (ret < 0) {
		sr_err("USB receive error: %s.", libusb_error_name(ret));
		return SR_ERR;
	}

	log_chunk((const uint8_t *)&buf, len);

	/* If there are no data bytes just return (without error). */
	if (buf[0] == 0x0)
		return SR_OK;

	devc->bufoffset = 0;

	/*
	 * Append the 1-7 data bytes of this chunk to pbuf.
	 *
	 */
	num_databytes_in_chunk = buf[0];
	for (i = 0; i < num_databytes_in_chunk; i++, devc->buflen++) {
		pbuf[devc->buflen] = buf[1 + i];
	}

	/* Now look for packets in that data. */
	while ((devc->buflen - devc->bufoffset) >= dmm->packet_size) {
		if (dmm->packet_valid(pbuf + devc->bufoffset)) {
			log_dmm_packet(pbuf + devc->bufoffset);
			decode_packet(sdi, pbuf + devc->bufoffset);
			devc->bufoffset += dmm->packet_size;
		} else {
			devc->bufoffset++;
		}
	}

	/* Move remaining bytes to beginning of buffer. */
	if (devc->bufoffset < devc->buflen)
		memmove(pbuf, pbuf + devc->bufoffset, devc->buflen - devc->bufoffset);
	devc->buflen -= devc->bufoffset;

	return SR_OK;
}

SR_PRIV int uni_t_ut8802e_receive_data(int fd, int revents, void *cb_data)
{
	int ret;
	struct sr_dev_inst *sdi;
	struct dev_context *devc;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	devc = sdi->priv;

	if ((ret = get_and_handle_data(sdi)) != SR_OK)
		return FALSE;

	/* Abort acquisition if we acquired enough samples. */
	if (sr_sw_limits_check(&devc->limits))
		sr_dev_acquisition_stop(sdi);

	return TRUE;
}
