/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <gisi/modem.h>
#include <gisi/client.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/phonebook.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/sms.h>
#include <ofono/cbs.h>
#include <ofono/sim.h>
#include <ofono/ussd.h>
#include <ofono/ssn.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/call-meter.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

#include "drivers/isimodem/isimodem.h"
#include "drivers/isimodem/isiutil.h"
#include "drivers/isimodem/infoserver.h"
#include "drivers/isimodem/mtc.h"
#include "drivers/isimodem/debug.h"

#include "n900gpio.h"

struct isi_data {
	struct ofono_modem *modem;
	char const *ifname;
	GIsiModem *idx;
	GIsiClient *client;
	struct n900_gpio *gpio;
	struct isi_infoserver *infoserver;
	int reported;
	enum n900_power_state power_state;
	int mtc_state;
	guint timeout;
	ofono_bool_t online;
	struct isi_cb_data *online_cbd;
};

static void set_power_by_mtc_state(struct isi_data *isi, int state);
static void mtc_power_off(struct isi_data *isi);
static gboolean mtc_power_off_poll(gpointer user);

static void report_powered(struct isi_data *isi, ofono_bool_t powered)
{
	if (powered != isi->reported)
		ofono_modem_set_powered(isi->modem, isi->reported = powered);
}

static void report_online(struct isi_data *isi, ofono_bool_t online)
{
	struct isi_cb_data *cbd = isi->online_cbd;
	ofono_modem_online_cb_t cb = cbd->cb;

	isi->online_cbd = NULL;

	if (isi->online == online)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void set_power_by_mtc_state(struct isi_data *isi, int mtc_state)
{
	isi->mtc_state = mtc_state;

	if (isi->online_cbd)
		report_online(isi, mtc_state == MTC_NORMAL);

	switch (mtc_state) {
	case MTC_STATE_NONE:
	case MTC_POWER_OFF:
	case MTC_CHARGING:
	case MTC_SELFTEST_FAIL:
		report_powered(isi, 0);
		break;

	case MTC_RF_INACTIVE:
	case MTC_NORMAL:
	default:
		report_powered(isi, 1);
	}
}

static void mtc_state_ind_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return;
	}

	if (len < 3 || msg[0] != MTC_STATE_INFO_IND)
		return;

	if (msg[2] == MTC_START) {
		DBG("target modem state: %s (0x%02X)",
			mtc_modem_state_name(msg[1]), msg[1]);
		if (msg[1] == MTC_POWER_OFF) {
			isi->power_state = POWER_OFF_STARTED_STATE;
			mtc_power_off_poll(isi);
		}
	} else if (msg[2] == MTC_READY) {
		DBG("current modem state: %s (0x%02X)",
			mtc_modem_state_name(msg[1]), msg[1]);
		set_power_by_mtc_state(isi, msg[1]);
	}
}

static gboolean mtc_startup_synq_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;

	if (!msg) {
		DBG("%s: %s", "MTC_STARTUP_SYNQ",
			strerror(-g_isi_client_error(client)));
		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_STARTUP_SYNQ_RESP)
		return FALSE;

	return TRUE;
}

static gboolean mtc_state_query_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_data *isi = opaque;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_STATE_QUERY_RESP)
		return FALSE;

	DBG("current modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[1]), msg[1]);
	DBG("target modem state: %s (0x%02X)",
		mtc_modem_state_name(msg[2]), msg[2]);

	set_power_by_mtc_state(isi, msg[1]);

	{
		const unsigned char msg[3] = {
			MTC_STARTUP_SYNQ_REQ,
		};
		g_isi_request_make(client, msg, sizeof(msg), MTC_TIMEOUT,
					mtc_startup_synq_cb, opaque);
	}

	return TRUE;
}

static void reachable_cb(GIsiClient *client, gboolean alive, uint16_t object,
				void *opaque)
{
	const unsigned char msg[] = {
		MTC_STATE_QUERY_REQ,
		0x00, 0x00 /* Filler */
	};

	if (!alive) {
		DBG("MTC client: %s", strerror(-g_isi_client_error(client)));
		/* XXX */
		return;
	}

	DBG("%s (v.%03d.%03d) reachable",
		pn_resource_name(g_isi_client_resource(client)),
		g_isi_version_major(client),
		g_isi_version_minor(client));

	g_isi_subscribe(client, MTC_STATE_INFO_IND, mtc_state_ind_cb, opaque);
	g_isi_request_make(client, msg, sizeof(msg), MTC_TIMEOUT,
				mtc_state_query_cb, opaque);
}

static gboolean
mtc_power_off_poll(gpointer user)
{
	struct isi_data *isi = user;

	const unsigned char req[] = {
		MTC_SHUTDOWN_SYNC_REQ,
		0x00, 0x00 /* Filler */
	};

	isi->timeout = 0;

	if (isi->power_state == POWER_ON_STARTED_STATE ||
		isi->power_state == POWER_OFF_STATE ||
		isi->power_state == POWER_OFF_WAITING_STATE)
		return FALSE;

	g_isi_request_make(isi->client, req, sizeof(req), MTC_TIMEOUT,
				NULL, NULL);

	isi->timeout = g_timeout_add(200, mtc_power_off_poll, user);

	return FALSE;
}

static gboolean mtc_power_off_cb(GIsiClient *client,
					const void *restrict data, size_t len,
					uint16_t object, void *opaque)
{
	struct isi_data *isi = opaque;
	const unsigned char *msg = data;

	if (!msg) {
		DBG("%s: %s", "MTC_POWER_OFF_RESP",
			strerror(-g_isi_client_error(client)));
		if (isi->power_state == POWER_OFF_STARTED_STATE)
			mtc_power_off(isi);
		return TRUE;
	}

	if (len < 3 || msg[0] != MTC_POWER_OFF_RESP)
		return FALSE;

	return TRUE;
}

static void mtc_power_off(struct isi_data *isi)
{
	const unsigned char req[] = {
		MTC_POWER_OFF_REQ,
		0x00, 0x00 /* Filler */
	};

	g_isi_request_make(isi->client, req, sizeof(req), MTC_TIMEOUT,
				mtc_power_off_cb, isi);
}

static void n900_modem_power_cb(enum n900_power_state state,
				void *data)
{
	struct ofono_modem *modem = data;
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("power state %s", n900_power_state_name(state));

	isi->power_state = state;

	if (state == POWER_OFF_STARTED_STATE)
		mtc_power_off(isi);
	else if (isi->timeout)
		g_source_remove(isi->timeout);

	if (state == POWER_ON_STATE)
		g_isi_verify(isi->client, reachable_cb, isi);
	else
		set_power_by_mtc_state(isi, MTC_STATE_NONE);
}

static int n900_modem_probe(struct ofono_modem *modem)
{
	char const *ifname = ofono_modem_get_string(modem, "Interface");
	GIsiModem *idx;
	struct isi_data *isi;

	if (ifname == NULL)
		ifname = "phonet0";

	DBG("(%p) with %s", modem, ifname);

	idx = g_isi_modem_by_name(ifname);
	if (idx == NULL) {
		DBG("Interface=%s: %s", ifname, strerror(errno));
		return -errno;
	}

	if (n900_gpio_probe(idx, n900_modem_power_cb, modem) != 0) {
		DBG("gpio for %s: %s", ifname, strerror(errno));
		return -errno;
	}

	isi = g_new0(struct isi_data, 1);
	if (isi == NULL) {
		n900_gpio_remove(modem);
		return -ENOMEM;
	}

	ofono_modem_set_data(isi->modem = modem, isi);

	isi->idx = idx;
	isi->ifname = ifname;
	isi->client = g_isi_client_create(isi->idx, PN_MTC);

	return 0;
}

static void n900_modem_remove(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("");

	if (isi == NULL)
		return;

	n900_gpio_remove(modem);
	if (isi->timeout)
		g_source_remove(isi->timeout);
	g_isi_client_destroy(isi->client);
	g_free(isi);
}

static gboolean mtc_state_cb(GIsiClient *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	struct ofono_modem *modem = cbd->user;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct isi_data *isi = ofono_modem_get_data(modem);
	const unsigned char *msg = data;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto err;
	}

	if (len < 3 || msg[0] != MTC_STATE_RESP)
		return FALSE;

	DBG("cause: %s (0x%02X)", mtc_isi_cause_name(msg[1]), msg[1]);

	if (msg[1] == MTC_OK) {
		isi->online_cbd = cbd;
		return TRUE;
	}

err:
	if (msg && msg[1] == MTC_ALREADY_ACTIVE)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
	return TRUE;
}

static void n900_modem_set_online(struct ofono_modem *modem,
					ofono_bool_t online,
					ofono_modem_online_cb_t cb, void *data)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	const unsigned char req[] = {
		MTC_STATE_REQ, online ? MTC_NORMAL : MTC_RF_INACTIVE, 0x00
	};
	struct isi_cb_data *cbd;

	DBG("(%p) with %s", modem, isi->ifname);

	if (isi->power_state != POWER_ON_STATE)
		goto error;
	if (isi->mtc_state == MTC_SELFTEST_FAIL)
		goto error;

	cbd = isi_cb_data_new(modem, cb, data);
	if (!cbd)
		goto error;

	isi->online = online;

	if (g_isi_request_make(isi->client, req, sizeof(req), MTC_TIMEOUT,
				mtc_state_cb, cbd))
		return;

	g_free(cbd);
error:
	CALLBACK_WITH_FAILURE(cb, data);
}

static void n900_modem_pre_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("");

	isi->infoserver = isi_infoserver_create(isi->modem, isi->idx);

	ofono_sim_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_devinfo_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_voicecall_create(isi->modem, 0, "isimodem", isi->idx);
}

static void n900_modem_post_sim(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);

	DBG("");

	ofono_phonebook_create(isi->modem, 0, "isimodem", isi->idx);
}

static void n900_modem_post_online(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("");

	ofono_netreg_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_sms_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_cbs_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_ssn_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_ussd_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_forwarding_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_settings_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_barring_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_call_meter_create(isi->modem, 0, "isimodem", isi->idx);
	ofono_radio_settings_create(isi->modem, 0, "isimodem", isi->idx);
	gprs = ofono_gprs_create(isi->modem, 0, "isimodem", isi->idx);
	gc = ofono_gprs_context_create(isi->modem, 0, "isimodem", isi->idx);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
	else
		DBG("Failed to add context");
}

static int n900_modem_enable(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	DBG("modem=%p with %p", modem, isi ? isi->ifname : NULL);
	return n900_gpio_enable(modem);
}

static int n900_modem_disable(struct ofono_modem *modem)
{
	struct isi_data *isi = ofono_modem_get_data(modem);
	DBG("modem=%p with %p", modem, isi ? isi->ifname : NULL);
	return n900_gpio_disable(modem);
}

static struct ofono_modem_driver driver = {
	.name = "n900modem",
	.probe = n900_modem_probe,
	.remove = n900_modem_remove,
	.enable = n900_modem_enable,
	.disable = n900_modem_disable,
	.set_online = n900_modem_set_online,
	.pre_sim = n900_modem_pre_sim,
	.post_sim = n900_modem_post_sim,
	.post_online = n900_modem_post_online,
};

static int n900modem_init(void)
{
	isi_devinfo_init();
	isi_phonebook_init();
	isi_netreg_init();
	isi_voicecall_init();
	isi_sms_init();
	isi_cbs_init();
	isi_sim_init();
	isi_ssn_init();
	isi_ussd_init();
	isi_call_forwarding_init();
	isi_call_settings_init();
	isi_call_barring_init();
	isi_call_meter_init();
	isi_radio_settings_init();

	ofono_modem_driver_register(&driver);

	return 0;
}

static void n900modem_exit(void)
{
	ofono_modem_driver_unregister(&driver);

	isi_devinfo_exit();
	isi_phonebook_exit();
	isi_netreg_exit();
	isi_voicecall_exit();
	isi_sms_exit();
	isi_cbs_exit();
	isi_sim_exit();
	isi_ssn_exit();
	isi_ussd_exit();
	isi_call_forwarding_exit();
	isi_call_settings_exit();
	isi_call_barring_exit();
	isi_call_meter_exit();
	isi_radio_settings_exit();
}

OFONO_PLUGIN_DEFINE(n900modem, "Nokia N900 modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_HIGH, n900modem_init, n900modem_exit)
