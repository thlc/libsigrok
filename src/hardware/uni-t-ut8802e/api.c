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
#include <stdlib.h>
#include <string.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"
#include "protocol.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_MULTIMETER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_SET | SR_CONF_GET,
	SR_CONF_LIMIT_MSEC | SR_CONF_SET | SR_CONF_GET,
};

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	GSList *usb_devices, *devices, *l;
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct drv_context *drvc;
	struct dmm_info *dmm;
	struct sr_usb_dev_inst *usb;
	struct sr_config *src;
	const char *conn;

	drvc = di->context;
	dmm = (struct dmm_info *)di;

	conn = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	if (!conn)
		return NULL;

	devices = NULL;
	if (!(usb_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn))) {
		g_slist_free_full(usb_devices, g_free);
		return NULL;
	}

	for (l = usb_devices; l; l = l->next) {
		usb = l->data;
		devc = g_malloc0(sizeof(struct dev_context));
		devc->first_run = TRUE;
		sdi = g_malloc0(sizeof(struct sr_dev_inst));
		sdi->status = SR_ST_INACTIVE;
		sdi->vendor = g_strdup(dmm->vendor);
		sdi->model = g_strdup(dmm->device);
		sdi->priv = devc;
		sr_channel_new(sdi, 0, SR_CHANNEL_ANALOG, TRUE, "P1");
		sdi->inst_type = SR_INST_USB;
		sdi->conn = usb;
		devices = g_slist_append(devices, sdi);
	}

	return std_scan_complete(di, devices);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct sr_dev_driver *di;
	struct drv_context *drvc;
	struct sr_usb_dev_inst *usb;

	di = sdi->driver;
	drvc = di->context;
	usb = sdi->conn;

	return sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	(void)cg;

	devc = sdi->priv;

	return sr_sw_limits_config_set(&devc->limits, key, data);
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	devc = sdi->priv;

	sr_sw_limits_acquisition_start(&devc->limits);

	std_session_send_df_header(sdi);

	sr_session_source_add(sdi->session, -1, 0, 10,
			uni_t_ut8802e_receive_data, (void *)sdi);

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	std_session_send_df_end(sdi);
	sr_session_source_remove(sdi->session, -1);

	return SR_OK;
}

#define DMM(ID, CHIPSET, VENDOR, MODEL, BAUDRATE, PACKETSIZE, \
			VALID, PARSE, DETAILS) \
	&((struct dmm_info) { \
		{ \
			.name = ID, \
			.longname = VENDOR " " MODEL, \
			.api_version = 1, \
			.init = std_init, \
			.cleanup = std_cleanup, \
			.scan = scan, \
			.dev_list = std_dev_list, \
			.dev_clear = std_dev_clear, \
			.config_get = NULL, \
			.config_set = config_set, \
			.config_list = config_list, \
			.dev_open = dev_open, \
			.dev_close = std_dummy_dev_close /* TODO */, \
			.dev_acquisition_start = dev_acquisition_start, \
			.dev_acquisition_stop = dev_acquisition_stop, \
			.context = NULL, \
		}, \
		VENDOR, MODEL, BAUDRATE, PACKETSIZE, \
		VALID, PARSE, DETAILS, sizeof(struct CHIPSET##_info) \
	}).di

SR_REGISTER_DEV_DRIVER_LIST(uni_t_ut8802e_drivers,
	DMM(
		"uni-t-ut8802e", ut71x,
		"UNI-T", "UT8802E", 2400, UT8802E_PACKET_SIZE,
		sr_ut8802e_packet_valid, sr_ut8802e_parse, NULL
	),
);
