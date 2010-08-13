/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef __N900_GPIO_H
#define __N900_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

enum n900_power_state
{
	POWER_NONE_STATE,

	POWER_ON_STARTED_STATE,
	POWER_ON_STATE,
	POWER_ON_RESET_STATE,
	POWER_ON_FAILED_STATE,
	POWER_OFF_STARTED_STATE,
	POWER_OFF_WAITING_STATE,
	POWER_OFF_STATE,
};

typedef void n900_gpio_callback(enum n900_power_state, void *opaque);

int n900_gpio_probe(GIsiModem *idx, n900_gpio_callback *callback, void *data);
int n900_gpio_enable(void *opaque);
int n900_gpio_disable(void *opaque);
int n900_gpio_remove(void *opaque);

char const *n900_power_state_name(enum n900_power_state value);

#ifdef __cplusplus
};
#endif

#endif /* __N900_GPIO_H */
