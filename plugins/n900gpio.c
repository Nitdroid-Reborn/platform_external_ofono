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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <gisi/netlink.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

#include <drivers/isimodem/debug.h>
#include "n900gpio.h"

#define GPIO_SWITCH	"/sys/devices/platform/gpio-switch"
#define DEV_CMT		"/dev/cmt"

enum rapu_type {
	RAPU1, RAPU2
};

enum {
	RESET_RETRIES = 5,
	POWER_ON_RETRIES = 10
};

enum phonet_state {
	PHONET_LINK_NONE = 0,
	PHONET_LINK_DOWN,
	PHONET_LINK_UP,
};

enum gpio_power_event {
	PHONET_LINK_UP_EVENT = 1,
	PHONET_LINK_DOWN_EVENT,

	POWER_ON_EVENT,

	POWER_ON_TIMEOUT_EVENT,
	POWER_REBOOT_TIMEOUT_EVENT,

	POWER_OFF_EVENT,
	POWER_OFF_IMMEDIATELY_EVENT,

	POWER_OFF_TIMEOUT_EVENT,
	POWER_OFF_COMPLETE_EVENT,
};

struct n900_gpio {
	GPhonetNetlink *link;

	n900_gpio_callback *callback;
	void *data;

	enum n900_power_state state;

	enum phonet_state current, target;

	unsigned retries;

	enum gpio_power_event timer_event;
	guint timeout;
	guint pollout;

	enum rapu_type rapu;

	unsigned have_gpio_switch:1;
	unsigned have_cmt_en:1;
	unsigned have_cmt_rst_rq:1;
	unsigned have_cmt_rst:1;
	unsigned have_cmt_bsi:1;
	unsigned have_cmt_apeslpx:1;

	unsigned reset_in_progress:1;
	unsigned startup_in_progress:1;
} self;

#define _(X) case X: return #X

static inline char const *gpio_power_event_name(enum gpio_power_event value)
{
	switch (value) {
		_(PHONET_LINK_UP_EVENT);
		_(PHONET_LINK_DOWN_EVENT);
		_(POWER_ON_EVENT);
		_(POWER_ON_TIMEOUT_EVENT);
		_(POWER_REBOOT_TIMEOUT_EVENT);
		_(POWER_OFF_EVENT);
		_(POWER_OFF_IMMEDIATELY_EVENT);
		_(POWER_OFF_TIMEOUT_EVENT);
		_(POWER_OFF_COMPLETE_EVENT);
	}
	return "<UNKNOWN>";
}

char const *n900_power_state_name(enum n900_power_state value)
{
	switch (value) {
		_(POWER_NONE_STATE);
		_(POWER_ON_STARTED_STATE);
		_(POWER_ON_STATE);
		_(POWER_ON_RESET_STATE);
		_(POWER_ON_FAILED_STATE);
		_(POWER_OFF_STARTED_STATE);
		_(POWER_OFF_WAITING_STATE);
		_(POWER_OFF_STATE);
	}
	return "<UNKNOWN>";
}

#undef _

static void gpio_power_state_machine(enum gpio_power_event event);
static void gpio_power_set_state(enum n900_power_state new_state);

static int file_exists(char const *filename)
{
	struct stat st;

	return stat(filename, &st) == 0;
}

static int file_write(char const *filename, char const *output)
{
	FILE *f;

	f = fopen(filename, "r+");
	if (!f) {
		DBG("%s: %s (%d)", filename, strerror(errno), errno);
		return -1;
	}

	fputs(output, f);

	return fclose(f);
}

static int gpio_write(char *line, int value)
{
	char filename[256];

	DBG("(\"%s\", \"%s\")", line, value ? "active" : "inactive");

	if (self.have_gpio_switch) {
		snprintf(filename, sizeof filename, "%s/%s/%s",
				GPIO_SWITCH, line, "state");
		return file_write(filename, value ? "active" : "inactive");
	} else {
		snprintf(filename, sizeof filename, "%s/%s/%s",
				DEV_CMT, line, "value");
		return file_write(filename, value ? "1" : "0");
	}
}

#define GPIO_WRITE(line, value) \
	(self.have_ ## line ? gpio_write(#line, value) : 0)

static int gpio_probe(char const *line)
{
	char filename[256];
	int result;

	if (self.have_gpio_switch)
		snprintf(filename, sizeof filename,
				"%s/%s/state", GPIO_SWITCH, line);
	else
		snprintf(filename, sizeof filename,
				"%s/%s/value", DEV_CMT, line);

	result = file_exists(filename);

	DBG("%s: %s", line, result ? "found" : "not found");

	return result;
}

/*
 * Modem start up function
 * Sets all lines down and leaves "power key" pressed
 * (power key must be released after some time)
 */
static void
gpio_start_modem_power_on(void)
{
	DBG("");

	if (self.startup_in_progress)
		return;
	self.startup_in_progress = 1;

	GPIO_WRITE(cmt_apeslpx, 0);	/* skip flash mode */
	GPIO_WRITE(cmt_rst_rq, 0);	/* prevent current drain */

	switch (self.rapu) {
	case RAPU2:
		GPIO_WRITE(cmt_en, 0);
		/* 15 ms needed for ASIC poweroff */
		usleep(20000);
		GPIO_WRITE(cmt_en, 1);
		break;

	case RAPU1:
		/* toggle BSI visible to CMT */
		GPIO_WRITE(cmt_bsi, 0);
		GPIO_WRITE(cmt_rst, 0);	/* Assert PURX */
		GPIO_WRITE(cmt_en, 1);	/* Press "power key" */
		GPIO_WRITE(cmt_rst, 1);	/* Release CMT to boot */
		break;
	}

	GPIO_WRITE(cmt_rst_rq, 1);
}

static void
gpio_finish_modem_power_on(void)
{
	DBG("");

	if (!self.startup_in_progress)
		return;
	self.startup_in_progress = 0;

	switch (self.rapu) {
	case RAPU2:
		break;

	case RAPU1:
		GPIO_WRITE(cmt_en, 0);	/* release "power key" */
		break;
	}
}

static void
gpio_start_modem_reset(void)
{
	DBG("");

	if (self.reset_in_progress)
		return;
	self.reset_in_progress = 1;

	if (self.have_cmt_rst_rq) {
		GPIO_WRITE(cmt_rst_rq, 0); /* Just in case */
		GPIO_WRITE(cmt_rst_rq, 1);
	} else
		gpio_start_modem_power_on();
}

static void
gpio_finish_modem_reset(void)
{
	DBG("");

	if (!self.reset_in_progress)
		return;
	self.reset_in_progress = 0;
	gpio_finish_modem_power_on();
}

static void
gpio_finish_modem_power_off(void)
{
	DBG("");

	if (self.reset_in_progress)
		gpio_finish_modem_reset();
	if (self.startup_in_progress)
		gpio_finish_modem_power_on();

	GPIO_WRITE(cmt_apeslpx, 0);	/* skip flash mode */
	GPIO_WRITE(cmt_rst_rq, 0);	/* prevent current drain */

	switch (self.rapu) {
	case RAPU2:
		GPIO_WRITE(cmt_en, 0);	/* Power off */
		break;

	case RAPU1:
		GPIO_WRITE(cmt_rst, 0);	/* force CMT to reset state */
		GPIO_WRITE(cmt_en, 0);	/* release "power key" */
		GPIO_WRITE(cmt_rst, 1);	/* release CMT to be powered
							off by bootloader */
		break;
	}
}

static gboolean
gpio_power_timer_cb(gpointer user)
{
	self.timeout = 0;

	if (self.timer_event)
		gpio_power_state_machine(self.timer_event);

	return FALSE;
}


static void
gpio_power_state_machine(enum gpio_power_event event)
{
	enum n900_power_state new_state;

	DBG("(%s) @ state %s",
		gpio_power_event_name(event),
		n900_power_state_name(self.state));

	switch (event) {
	case POWER_ON_EVENT:
		self.target = PHONET_LINK_UP;

		if (self.current == PHONET_LINK_NONE)
			return;

		switch (self.state) {
		case POWER_ON_STARTED_STATE:
		case POWER_ON_RESET_STATE:
		case POWER_ON_STATE:
			/* Do nothing */
			break;
		case POWER_OFF_STARTED_STATE:
			/* Do nothing */
			break;
		case POWER_NONE_STATE:
		case POWER_OFF_WAITING_STATE:
		case POWER_OFF_STATE:
		case POWER_ON_FAILED_STATE:
			gpio_power_set_state(POWER_ON_STARTED_STATE);
			break;
		}
		return;

	case PHONET_LINK_DOWN_EVENT:
		switch (self.target) {
		case PHONET_LINK_DOWN:
		case PHONET_LINK_NONE:
		default:
			if (self.state == POWER_OFF_STATE ||
				self.state == POWER_NONE_STATE)
				new_state = POWER_OFF_STATE;
			else
				new_state = POWER_OFF_WAITING_STATE;
			gpio_power_set_state(new_state);
			return;
		case PHONET_LINK_UP:
			break;
		}

		switch (self.state) {
		case POWER_NONE_STATE:
			/* first connection down event => start modem */
			gpio_power_set_state(POWER_ON_STARTED_STATE);
			break;

		case POWER_ON_STARTED_STATE:
		case POWER_ON_RESET_STATE:
			break;

		default:
			self.retries = 0;
			gpio_power_set_state(POWER_ON_RESET_STATE);
			break;
		}
		return;

	case POWER_ON_TIMEOUT_EVENT:
		if (self.target == PHONET_LINK_DOWN)
			new_state = POWER_OFF_STARTED_STATE;
		else if (self.retries <= POWER_ON_RETRIES)
			new_state = POWER_ON_STARTED_STATE;
		else
			new_state = POWER_ON_FAILED_STATE;
		gpio_power_set_state(new_state);
		return;

	case POWER_REBOOT_TIMEOUT_EVENT:
		/* Modem is not rebooting itself - try to powercycle it */
		if (self.target == PHONET_LINK_DOWN)
			new_state = POWER_OFF_STARTED_STATE;
		else if (self.retries <= RESET_RETRIES)
			new_state = POWER_ON_RESET_STATE;
		else
			new_state = POWER_ON_STARTED_STATE;
		gpio_power_set_state(new_state);
		return;

	case PHONET_LINK_UP_EVENT:
		switch (self.state) {
		case POWER_NONE_STATE:
			return;

		case POWER_ON_STARTED_STATE:
		case POWER_ON_RESET_STATE:
			break;

		case POWER_ON_STATE:
			return;

		case POWER_OFF_STARTED_STATE:
		case POWER_OFF_WAITING_STATE:
		case POWER_OFF_STATE:
		case POWER_ON_FAILED_STATE:
			DBG("LINK_UP event while "
				"modem should be powered off");
			/* should never come here */
			break;
		}
		if (self.target == PHONET_LINK_DOWN)
			gpio_power_set_state(POWER_OFF_STARTED_STATE);
		else
			gpio_power_set_state(POWER_ON_STATE);
		return;

	case POWER_OFF_EVENT:
		self.target = PHONET_LINK_DOWN;

		switch (self.state) {
		case POWER_ON_STARTED_STATE:
		case POWER_ON_RESET_STATE:
			/* Do nothing until a timer expires */
			break;
		case POWER_ON_STATE:
			gpio_power_set_state(POWER_OFF_STARTED_STATE);
			break;
		case POWER_OFF_STARTED_STATE:
		case POWER_OFF_WAITING_STATE:
		case POWER_OFF_STATE:
			/* Do nothing */
			break;
		case POWER_NONE_STATE:
		case POWER_ON_FAILED_STATE:
			gpio_power_set_state(POWER_OFF_STATE);
			break;
		}
		return;

	case POWER_OFF_IMMEDIATELY_EVENT:
		gpio_power_set_state(POWER_OFF_STATE);
		return;

	case POWER_OFF_TIMEOUT_EVENT:
		DBG("CMT power off timed out");
		gpio_power_set_state(POWER_OFF_STATE);
		return;

	case POWER_OFF_COMPLETE_EVENT:
		if (self.state == POWER_OFF_WAITING_STATE) {
			DBG("Modem shutdown complete");
			gpio_power_set_state(POWER_OFF_STATE);
		}
		return;
	}

	DBG("Event %s (%d) not handled", gpio_power_event_name(event), event);
}


static void
gpio_power_set_state(enum n900_power_state new_state)
{
	enum n900_power_state old_state = self.state;
	unsigned timeout = 0;
	enum gpio_power_event timer_event;

	DBG("(%s) at (%s)%s",
		n900_power_state_name(new_state),
		n900_power_state_name(old_state),
		new_state == old_state ? " - already" : "");

	switch (old_state) {
	case POWER_ON_STARTED_STATE:
		gpio_finish_modem_power_on();
		break;
	case POWER_ON_RESET_STATE:
		gpio_finish_modem_reset();
		break;
	default:
		break;
	}

	if (self.timeout) {
		g_source_remove(self.timeout), self.timeout = 0;
		self.timer_event = 0;
	}

	if (old_state == new_state &&
		new_state != POWER_ON_STARTED_STATE &&
		new_state != POWER_ON_RESET_STATE)
		return;

	switch (self.state = new_state) {
	case POWER_NONE_STATE:
		break;

	case POWER_ON_STARTED_STATE:
		self.retries++;
		/* Maximum time modem power on procedure on can take */
		timeout = 5000;
		timer_event = POWER_ON_TIMEOUT_EVENT;
		gpio_start_modem_power_on();
		break;

	case POWER_ON_RESET_STATE:
		DBG("Starting modem restart timeout");
		/* Time allowed for CMT to restart after crash */
		timeout = 5000;
		timer_event = POWER_REBOOT_TIMEOUT_EVENT;
		if (self.retries++ > 0)
			gpio_start_modem_reset();
		break;

	case POWER_ON_STATE:
		DBG("Power on");
		self.retries = 0;
		break;

	case POWER_OFF_STARTED_STATE:
		DBG("Starting power off");
		/* Maximum time modem power_off can take */
		timeout = 6150;
		timer_event = POWER_OFF_TIMEOUT_EVENT;
		break;

	case POWER_OFF_WAITING_STATE:
		gpio_finish_modem_power_off();
		DBG("Waiting to modem to settle down");
		/* Cooling time after power off */
		timeout = 1000;
		timer_event = POWER_OFF_COMPLETE_EVENT;
		break;

	case POWER_OFF_STATE:
		if (old_state != POWER_OFF_WAITING_STATE &&
			old_state != POWER_ON_FAILED_STATE)
			gpio_finish_modem_power_off();
		break;

	case POWER_ON_FAILED_STATE:
		DBG("Link to mdoem cannot be established, giving up");
		gpio_finish_modem_power_off();
		break;
	}

	if (timeout) {
		self.timer_event = timer_event;
		self.timeout = g_timeout_add(timeout,
					gpio_power_timer_cb, NULL);
	}

	self.callback(new_state, self.data);
}

static void phonet_status_cb(GIsiModem *idx,
				GPhonetLinkState st,
				char const *ifname,
				void *dummy)
{
	DBG("Link %s (%u) is %s",
		ifname, g_isi_modem_index(idx),
		st == PN_LINK_REMOVED ? "removed" :
		st == PN_LINK_DOWN ? "down" : "up");

	(void)dummy;

	if (st == PN_LINK_UP) {
		if (self.current == PHONET_LINK_UP)
			return;
		self.current = PHONET_LINK_UP;

		/* link is up - we can lower cmt_rst_rq */
		GPIO_WRITE(cmt_rst_rq, 0);

		gpio_power_state_machine(PHONET_LINK_UP_EVENT);
	} else {
		if (self.current == PHONET_LINK_DOWN)
			return;
		self.current = PHONET_LINK_DOWN;

		gpio_power_state_machine(PHONET_LINK_DOWN_EVENT);
	}
}

int n900_gpio_probe(GIsiModem *idx, n900_gpio_callback *callback, void *data)
{
	int error;

	if (callback == NULL) {
		DBG("n900_gpio: %s", "no callback");
		return -(errno = EFAULT);
	}

	if (self.callback) {
		DBG("n900_gpio: %s", strerror(EBUSY));
		return -(errno = EBUSY);
	}

	if (g_pn_netlink_by_modem(idx)) {
		DBG("Phonet link %p: %s", idx, strerror(EBUSY));
		return -(errno = EBUSY);
	}

	self.callback = callback;
	self.data = data;
	self.target = PHONET_LINK_NONE;
	self.have_gpio_switch = file_exists(GPIO_SWITCH);

	if (self.have_gpio_switch)
		DBG("Using GPIO switch");
	else
		DBG("Using /dev/cmt");

	/* GPIO lines availability depends on HW and SW versions */
	self.have_cmt_en = gpio_probe("cmt_en");
	self.have_cmt_rst_rq = gpio_probe("cmt_rst_rq");
	self.have_cmt_rst = gpio_probe("cmt_rst");
	self.have_cmt_bsi = gpio_probe("cmt_bsi");
	self.have_cmt_apeslpx = gpio_probe("cmt_apeslpx");

	if (!self.have_cmt_en) {
		DBG("Modem control GPIO lines are not available");
		memset(&self, 0, sizeof self);
		return -(errno = ENODEV);
	}

	if (self.have_cmt_bsi)
		self.rapu = RAPU1;
	else
		self.rapu = RAPU2;

	self.link = g_pn_netlink_start(idx, phonet_status_cb, NULL);
	if (!self.link) {
		memset(&self, 0, sizeof self);
		return -errno;
	}

	error = g_pn_netlink_set_address(idx, PN_DEV_SOS);
	if (error && error != -EEXIST)
		DBG("g_pn_netlink_set_address: %s\n", strerror(-error));

	error = g_pn_netlink_add_route(idx, PN_DEV_HOST);
	if (error && error != -ENOTSUP)	/* We get ENOTSUP on Maemo 5 kernel */
		DBG("g_pn_netlink_add_route: %s\n", strerror(-error));

	return 0;
}

int n900_gpio_remove(void *data)
{
	if (self.data != data)
		return -EINVAL;

	if (self.link)
		g_pn_netlink_stop(self.link);
	if (self.timeout)
		g_source_remove(self.timeout), self.timeout = 0;

	memset(&self, 0, sizeof self);

	return 0;
}

int n900_gpio_enable(void *data)
{
	if (self.data != data)
		return -EINVAL;

	if (self.state == POWER_ON_STATE)
		return 0;

	gpio_power_state_machine(POWER_ON_EVENT);

	return -EINPROGRESS;
}

int n900_gpio_disable(void *data)
{
	if (self.data != data)
		return -EINVAL;

	if (self.state == POWER_OFF_STATE ||
		self.state == POWER_ON_FAILED_STATE)
		return 0;

	gpio_power_state_machine(POWER_OFF_EVENT);

	return -EINPROGRESS;
}

