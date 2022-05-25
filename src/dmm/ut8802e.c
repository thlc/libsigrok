/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2014 Uwe Hermann <uwe@hermann-uwe.de>
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

/*
 * UNI-T UT8802e protocol parser.
 */

#include <config.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "ut8802e"

static uint8_t bcd_to_dec(uint8_t byte) {
	return (byte >> 4) * 10 + (byte & 0xf);
}

static int parse_value(const uint8_t *buf, struct ut8802e_info *info,
		       float *floatval, int *exponent)
{
	int intval;

	intval = bcd_to_dec(buf[4]) * 10000 +
		 bcd_to_dec(buf[3]) * 100 +
		 bcd_to_dec(buf[2]);
	*exponent = buf[5] & 0xf;
	*floatval = intval / powf(10, *exponent);

	if (info->is_ol) {
		*floatval = INFINITY;
	}

	*floatval *= info->is_sign ? -1 : 1;

	/* TODO: some modes are using an automatic range:
	 *  - frequency
	 *  - capacitance
	 */
	if (info->is_frequency && buf[1] == 0x2c)
		*floatval *= 1000;

	if (info->is_capacitance) {
		switch (buf[1]) {
		case 0x27:
			*floatval /= 1000;
			break;
		case 0x28:
			*floatval /= 1; /* FIXME */
			break;
		}
	}

	return SR_OK;
}

static void parse_flags(const uint8_t *buf, struct ut8802e_info *info)
{
	sr_dbg("Mode: %02x", buf[1]);
	/* Function byte */
	switch (buf[1]) {
	case 0x0c: /* AC 750V */
	case 0x0b: /* AC 200V */
	case 0x0a: /* AC 20V */
	case 0x09: /* AC 2v */
		info->is_voltage = info->is_ac = TRUE;
		break;
	case 0x01: /* DC 200mV */
	case 0x03: /* DC 2V */
	case 0x04: /* DC 20V */
	case 0x05: /* DC 200V */
	case 0x06: /* DC 1000V */
		info->is_voltage = info->is_dc = TRUE;
		break;
	case 0x1F: /* 200M ohm */
	case 0x1D: /* 2M */
	case 0x1C: /* 200K */
	case 0x1B: /* 20K */
	case 0x1A: /* 2K */
	case 0x19: /* 200 */
		info->is_resistance = TRUE;
		break;
	case 0x28: /* Capacitance */
	case 0x27:
		info->is_capacitance = TRUE;
		break;
	case 0x0d: /* 200uA DC */
	case 0x0e: /* 2mA DC */
	case 0x11: /* 20mA DC */
	case 0x12: /* 200mA DC */
	case 0x16: /* 20A DC */
		info->is_current = info->is_dc = TRUE;
		break;
	case 0x10: /* 2mA AC */
	case 0x13: /* 20mA AC */
	case 0x14: /* 200mA AC */
	case 0x18: /* 20A AC */
		info->is_current = info->is_ac = TRUE;
		break;
	case 0x24: /* Continuity */
		info->is_continuity = TRUE;
		break;
	case 0x23: /* Diode */
		info->is_diode = TRUE;
		break;
	case 0x2b: /* Frequency */
	case 0x2c:
		info->is_frequency = TRUE;
		break;
	case 0x22: /* Duty Cycle */
		info->is_duty_cycle = TRUE;
		break;
	default:
		sr_dbg("Invalid function byte: 0x%02x.", buf[1]);
		break;
	}

	/* bit 2 and 5 are still unknown. bit 2 seems always set. */
	info->is_min    = (buf[6] & (1 << 0)) != 0;
	info->is_max    = (buf[6] & (1 << 1)) != 0;
	info->is_rel    = (buf[6] & (1 << 3)) != 0;
	info->is_hold   = (buf[6] & (1 << 4)) != 0;
	info->is_ol     = (buf[6] & (1 << 6)) != 0;
	info->is_sign   = (buf[6] & (1 << 7)) != 0;
}

static void handle_flags(struct sr_datafeed_analog *analog,
			 float *floatval, const struct ut8802e_info *info)
{
	/* Measurement modes */
	if (info->is_voltage) {
		analog->meaning->mq = SR_MQ_VOLTAGE;
		analog->meaning->unit = SR_UNIT_VOLT;
	}
	if (info->is_current) {
		analog->meaning->mq = SR_MQ_CURRENT;
		analog->meaning->unit = SR_UNIT_AMPERE;
	}
	if (info->is_resistance) {
		analog->meaning->mq = SR_MQ_RESISTANCE;
		analog->meaning->unit = SR_UNIT_OHM;
	}
	if (info->is_frequency) {
		analog->meaning->mq = SR_MQ_FREQUENCY;
		analog->meaning->unit = SR_UNIT_HERTZ;
	}
	if (info->is_capacitance) {
		analog->meaning->mq = SR_MQ_CAPACITANCE;
		analog->meaning->unit = SR_UNIT_FARAD;
	}
	if (info->is_temperature && info->is_celsius) {
		analog->meaning->mq = SR_MQ_TEMPERATURE;
		analog->meaning->unit = SR_UNIT_CELSIUS;
	}
	if (info->is_temperature && info->is_fahrenheit) {
		analog->meaning->mq = SR_MQ_TEMPERATURE;
		analog->meaning->unit = SR_UNIT_FAHRENHEIT;
	}
	if (info->is_continuity) {
		analog->meaning->mq = SR_MQ_CONTINUITY;
		analog->meaning->unit = SR_UNIT_BOOLEAN;
		*floatval = (*floatval < 0.0 || *floatval > 60.0) ? 0.0 : 1.0;
	}
	if (info->is_diode) {
		analog->meaning->mq = SR_MQ_VOLTAGE;
		analog->meaning->unit = SR_UNIT_VOLT;
	}
	if (info->is_duty_cycle) {
		analog->meaning->mq = SR_MQ_DUTY_CYCLE;
		analog->meaning->unit = SR_UNIT_PERCENTAGE;
	}
	if (info->is_power) {
		analog->meaning->mq = SR_MQ_POWER;
		analog->meaning->unit = SR_UNIT_WATT;
	}
	if (info->is_loop_current) {
		/* 4mA = 0%, 20mA = 100% */
		analog->meaning->mq = SR_MQ_CURRENT;
		analog->meaning->unit = SR_UNIT_PERCENTAGE;
	}

	/* Measurement related flags */
	if (info->is_ac)
		analog->meaning->mqflags |= SR_MQFLAG_AC;
	if (info->is_dc)
		analog->meaning->mqflags |= SR_MQFLAG_DC;
	if (info->is_ac)
		/* All AC modes do True-RMS measurements. */
		analog->meaning->mqflags |= SR_MQFLAG_RMS;
	if (info->is_diode)
		analog->meaning->mqflags |= SR_MQFLAG_DIODE | SR_MQFLAG_DC;

	/* Special modes */
	if (info->is_min)
		analog->meaning->mqflags |= SR_MQFLAG_MIN;
	if (info->is_max)
		analog->meaning->mqflags |= SR_MQFLAG_MAX;
	if (info->is_rel)
		analog->meaning->mqflags |= SR_MQFLAG_RELATIVE;
	if (info->is_hold)
		analog->meaning->mqflags |= SR_MQFLAG_HOLD;
}

static gboolean flags_valid(const struct ut8802e_info *info)
{
	int count;

	/* Does the packet "measure" more than one type of value? */
	count  = (info->is_voltage) ? 1 : 0;
	count += (info->is_current) ? 1 : 0;
	count += (info->is_resistance) ? 1 : 0;
	count += (info->is_capacitance) ? 1 : 0;
	count += (info->is_frequency) ? 1 : 0;
	count += (info->is_temperature) ? 1 : 0;
	count += (info->is_continuity) ? 1 : 0;
	count += (info->is_diode) ? 1 : 0;
	count += (info->is_power) ? 1 : 0;
	count += (info->is_loop_current) ? 1 : 0;
	if (count > 1) {
		sr_dbg("More than one measurement type detected in packet.");
		return FALSE;
	}

	return TRUE;
}

SR_PRIV gboolean sr_ut8802e_packet_valid(const uint8_t *buf)
{
	struct ut8802e_info info;

	memset(&info, 0, sizeof(struct ut8802e_info));

	if (buf[0] != 0xAC)
		return FALSE;

	/* TODO: checksum at the end of the packet? */

	parse_flags(buf, &info);

	return flags_valid(&info);
}

SR_PRIV int sr_ut8802e_parse(const uint8_t *buf, float *floatval,
			     struct sr_datafeed_analog *analog, void *info)
{
	int ret, exponent = 0;
	struct ut8802e_info *info_local;

	info_local = info;
	memset(info_local, 0, sizeof(struct ut8802e_info));

	if (!sr_ut8802e_packet_valid(buf))
		return SR_ERR;

	parse_flags(buf, info_local);

	if ((ret = parse_value(buf, info, floatval, &exponent)) != SR_OK) {
		sr_dbg("Error parsing value: %d.", ret);
		return ret;
	}

	handle_flags(analog, floatval, info);

	/* FIXME: is that right? */
	analog->encoding->digits = exponent;
	analog->spec->spec_digits = exponent;

	return SR_OK;
}
