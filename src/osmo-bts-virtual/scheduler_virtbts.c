/* Scheduler worker functions for Virtua OsmoBTS */

/* (C) 2015-2017 by Harald Welte <laforge@gnumonks.org>
 * (C) 2017 Sebastian Stumpf <sebastian.stumpf87@googlemail.com>
 * Contributions by sysmocom - s.f.m.c. GmbH
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/bits.h>
#include <osmocom/core/gsmtap_util.h>
#include <osmocom/core/gsmtap.h>
#include <osmocom/gsm/rsl.h>

#include <osmo-bts/gsm_data.h>
#include <osmo-bts/phy_link.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/l1sap.h>
#include <osmo-bts/amr.h>
#include <osmo-bts/scheduler.h>
#include <osmo-bts/scheduler_backend.h>
#include "virtual_um.h"
#include "l1_if.h"

#define MODULO_HYPERFRAME 0

/**
 * Send a message over the virtual um interface.
 * This will at first wrap the msg with a GSMTAP header and then write it to the declared multicast socket.
 */
static void _tx_to_virt_um(struct l1sched_ts *l1ts,
			   struct trx_dl_burst_req *br,
			   struct msgb *msg, bool is_voice_frame)
{
	const struct trx_chan_desc *chdesc = &trx_chan_desc[br->chan];
	const struct gsm_bts_trx *trx = l1ts->ts->trx;
	struct msgb *outmsg;			/* msg to send with gsmtap header prepended */
	uint16_t arfcn = trx->arfcn;		/* ARFCN of the transceiver the message is send with */
	uint8_t signal_dbm = 63;		/* signal strength, 63 is best */
	uint8_t snr = 63;			/* signal noise ratio, 63 is best */
	uint8_t *data = msgb_l2(msg);		/* data to transmit (whole message without l1 header) */
	uint8_t data_len = msgb_l2len(msg);	/* length of data */
	uint8_t rsl_chantype;			/* RSL chan type (TS 08.58, 9.3.1) */
	uint8_t subslot;			/* multiframe subslot to send msg in (tch -> 0-26, bcch/ccch -> 0-51) */
	uint8_t timeslot;			/* TDMA timeslot to send in (0-7) */
	uint8_t gsmtap_chantype;		/* the GSMTAP channel */

	rsl_dec_chan_nr(chdesc->chan_nr, &rsl_chantype, &subslot, &timeslot);
	/* the timeslot is not encoded in the chan_nr of the chdesc, and so has to be overwritten */
	timeslot = br->tn;
	/* in Osmocom, AGCH is only sent on ccch block 0. no idea why. this seems to cause false GSMTAP channel
	 * types for agch and pch. */
	if (rsl_chantype == RSL_CHAN_PCH_AGCH &&
	    l1sap_fn2ccch_block(br->fn) >= num_agch(trx, "PH-DATA-REQ"))
		gsmtap_chantype = GSMTAP_CHANNEL_PCH;
	else
		gsmtap_chantype = chantype_rsl2gsmtap2(rsl_chantype, chdesc->link_id, is_voice_frame); /* the logical channel type */

	if (gsmtap_chantype == GSMTAP_CHANNEL_UNKNOWN) {
		LOGL1SB(DL1P, LOGL_ERROR, l1ts, br, "Tx GSMTAP for RSL channel type 0x%02x: cannot send, this"
		       " channel type is unknown in GSMTAP\n", rsl_chantype);
		msgb_free(msg);
		return;
	}

#if MODULO_HYPERFRAME
	/* Restart fn after every superframe (26 * 51 frames) to simulate hyperframe overflow each 6 seconds. */
	br->fn %= 26 * 51;
#endif

	outmsg = gsmtap_makemsg(arfcn, timeslot, gsmtap_chantype, subslot, br->fn, signal_dbm, snr, data, data_len);

	if (outmsg) {
		struct phy_instance *pinst = trx_phy_instance(trx);
		int rc;

		rc = virt_um_write_msg(pinst->phy_link->u.virt.virt_um, outmsg);
		if (rc < 0)
			LOGL1SB(DL1P, LOGL_ERROR, l1ts, br,
			       "GSMTAP msg could not send to virtual Um: %s\n", strerror(-rc));
		else if (rc == 0)
			bts_shutdown(trx->bts, "VirtPHY write socket died\n");
		else
			LOGL1SB(DL1P, LOGL_DEBUG, l1ts, br,
			       "Sending GSMTAP message to virtual Um\n");
	} else
		LOGL1SB(DL1P, LOGL_ERROR, l1ts, br, "GSMTAP msg could not be created!\n");

	/* free incoming message */
	msgb_free(msg);
}

static void tx_to_virt_um(struct l1sched_ts *l1ts,
			  struct trx_dl_burst_req *br,
			  struct msgb *msg)
{
	_tx_to_virt_um(l1ts, br, msg, false);
}


static struct gsm_lchan *lchan_from_l1t(const struct l1sched_ts *l1ts,
					const enum trx_chan_type chan)
{
	struct gsm_bts_trx_ts *ts = l1ts->ts;
	uint8_t subslot = 0;

	if (chan == TRXC_TCHH_1)
		subslot = 1;

	return &ts->lchan[subslot];
}

/* Determine the gsmtap_um_voice_type of a gsm_lchan */
static int get_um_voice_type(const struct gsm_lchan *lchan)
{
	switch (lchan->tch_mode) {
	case GSM48_CMODE_SPEECH_V1:
		if (lchan->type == GSM_LCHAN_TCH_H)
			return GSMTAP_UM_VOICE_HR;
		else
			return GSMTAP_UM_VOICE_FR;
	case GSM48_CMODE_SPEECH_EFR:
		return GSMTAP_UM_VOICE_EFR;
	case GSM48_CMODE_SPEECH_AMR:
		return GSMTAP_UM_VOICE_AMR;
	default:
		return -1;
	}
}

static void tx_to_virt_um_voice_frame(struct l1sched_ts *l1ts,
				      struct trx_dl_burst_req *br,
				      struct msgb *msg)
{
	struct gsm_lchan *lchan = lchan_from_l1t(l1ts, br->chan);
	int um_voice_type;

	OSMO_ASSERT(lchan);
	um_voice_type = get_um_voice_type(lchan);
	if (um_voice_type < 0) {
		LOGPLCHAN(lchan, DL1P, LOGL_ERROR, "Cannot determine Um voice type from lchan\n");
		um_voice_type = 0xff;
	}

	/* the first byte indicates the type of voice codec (gsmtap_um_voice_type) */
	msgb_pull_to_l2(msg);
	msgb_push_u8(msg, um_voice_type);
	msg->l2h = msg->data;
	_tx_to_virt_um(l1ts, br, msg, true);
}

/*
 * TX on downlink
 */

int tx_fcch_fn(struct l1sched_ts *l1ts, struct trx_dl_burst_req *br)
{
	return 0;
}

int tx_sch_fn(struct l1sched_ts *l1ts, struct trx_dl_burst_req *br)
{
	return 0;
}

int tx_data_fn(struct l1sched_ts *l1ts, struct trx_dl_burst_req *br)
{
	struct msgb *msg;

	if (br->bid > 0)
		return 0;

	/* get mac block from queue */
	msg = _sched_dequeue_prim(l1ts, br);
	if (!msg) {
		LOGL1SB(DL1P, LOGL_INFO, l1ts, br, "has not been served !! No prim\n");
		return -ENODEV;
	}

	/* check validity of message */
	if (msgb_l2len(msg) != GSM_MACBLOCK_LEN) {
		LOGL1SB(DL1P, LOGL_FATAL, l1ts, br, "Prim not 23 bytes, please FIX! (len=%d)\n",
			msgb_l2len(msg));
		/* free message */
		msgb_free(msg);
		return -EINVAL;
	}

	/* transmit the msg received on dl from bsc to layer1 (virt Um) */
	tx_to_virt_um(l1ts, br, msg);

	return 0;
}

int tx_pdtch_fn(struct l1sched_ts *l1ts, struct trx_dl_burst_req *br)
{
	struct msgb *msg = NULL; /* make GCC happy */

	if (br->bid > 0)
		return 0;

	/* get mac block from queue */
	msg = _sched_dequeue_prim(l1ts, br);
	if (!msg) {
		LOGL1SB(DL1P, LOGL_INFO, l1ts, br, "has not been served !! No prim\n");
		return -ENODEV;
	}

	tx_to_virt_um(l1ts, br, msg);

	return 0;
}

static void tx_tch_common(struct l1sched_ts *l1ts,
			  const struct trx_dl_burst_req *br,
			  struct msgb **_msg_tch, struct msgb **_msg_facch,
			  int codec_mode_request)
{
	struct msgb *msg1, *msg2, *msg_tch = NULL, *msg_facch = NULL;
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[br->chan];
	uint8_t rsl_cmode = chan_state->rsl_cmode;
	uint8_t tch_mode = chan_state->tch_mode;
	struct osmo_phsap_prim *l1sap;
#if 0
	/* handle loss detection of received TCH frames */
	if (rsl_cmode == RSL_CMOD_SPD_SPEECH
	 && ++(chan_state->lost_frames) > 5) {
		uint8_t tch_data[GSM_FR_BYTES];
		int len;

		LOGL1SB(DL1P, LOGL_NOTICE, l1ts, br, "Missing TCH bursts detected, sending "
			"BFI for %s\n", trx_chan_desc[br->chan].name);

		/* indicate bad frame */
		switch (tch_mode) {
		case GSM48_CMODE_SPEECH_V1: /* FR / HR */
			if (br->chan != TRXC_TCHF) { /* HR */
				tch_data[0] = 0x70; /* F = 0, FT = 111 */
				memset(tch_data + 1, 0, 14);
				len = 15;
				break;
			}
			memset(tch_data, 0, GSM_FR_BYTES);
			len = GSM_FR_BYTES;
			break;
		case GSM48_CMODE_SPEECH_EFR: /* EFR */
			if (br->chan != TRXC_TCHF)
				goto inval_mode1;
			memset(tch_data, 0, GSM_EFR_BYTES);
			len = GSM_EFR_BYTES;
			break;
		case GSM48_CMODE_SPEECH_AMR: /* AMR */
			len = amr_compose_payload(tch_data,
				chan_state->codec[chan_state->dl_cmr],
				chan_state->codec[chan_state->dl_ft], 1);
			if (len < 2)
				break;
			memset(tch_data + 2, 0, len - 2);
			_sched_compose_tch_ind(l1ts, 0, br->chan, tch_data, len);
			break;
		default:
inval_mode1:
			LOGP(DL1P, LOGL_ERROR, "TCH mode invalid, please "
				"fix!\n");
			len = 0;
		}
		if (len)
			_sched_compose_tch_ind(l1ts, 0, br->chan, tch_data, len);
	}
#endif

	/* get frame and unlink from queue */
	msg1 = _sched_dequeue_prim(l1ts, br);
	msg2 = _sched_dequeue_prim(l1ts, br);
	if (msg1) {
		l1sap = msgb_l1sap_prim(msg1);
		if (l1sap->oph.primitive == PRIM_TCH) {
			msg_tch = msg1;
			if (msg2) {
				l1sap = msgb_l1sap_prim(msg2);
				if (l1sap->oph.primitive == PRIM_TCH) {
					LOGL1SB(DL1P, LOGL_FATAL, l1ts, br,
						"TCH twice, please FIX!\n");
					msgb_free(msg2);
				} else
					msg_facch = msg2;
			}
		} else {
			msg_facch = msg1;
			if (msg2) {
				l1sap = msgb_l1sap_prim(msg2);
				if (l1sap->oph.primitive != PRIM_TCH) {
					LOGL1SB(DL1P, LOGL_FATAL, l1ts, br,
						"FACCH twice, please FIX!\n");
					msgb_free(msg2);
				} else
					msg_tch = msg2;
			}
		}
	} else if (msg2) {
		l1sap = msgb_l1sap_prim(msg2);
		if (l1sap->oph.primitive == PRIM_TCH)
			msg_tch = msg2;
		else
			msg_facch = msg2;
	}

	/* check validity of message */
	if (msg_facch && msgb_l2len(msg_facch) != GSM_MACBLOCK_LEN) {
		LOGL1SB(DL1P, LOGL_FATAL, l1ts, br, "Prim has odd len=%u != %u\n",
			msgb_l2len(msg_facch), GSM_MACBLOCK_LEN);
		/* free message */
		msgb_free(msg_facch);
		msg_facch = NULL;
	}

	/* check validity of message, get AMR ft and cmr */
	if (!msg_facch && msg_tch) {
		int len;
#if 0
		uint8_t bfi, cmr_codec, ft_codec;
		int cmr, ft, i;
#endif

		if (rsl_cmode != RSL_CMOD_SPD_SPEECH) {
			LOGL1SB(DL1P, LOGL_NOTICE, l1ts, br, "Dropping speech frame, "
				"because we are not in speech mode\n");
			goto free_bad_msg;
		}

		switch (tch_mode) {
		case GSM48_CMODE_SPEECH_V1: /* FR / HR */
			if (br->chan != TRXC_TCHF) { /* HR */
				len = 15;
				if (msgb_l2len(msg_tch) >= 1
				 && (msg_tch->l2h[0] & 0xf0) != 0x00) {
					LOGL1SB(DL1P, LOGL_NOTICE, l1ts, br,
						"Transmitting 'bad HR frame'\n");
					goto free_bad_msg;
				}
				break;
			}
			len = GSM_FR_BYTES;
			if (msgb_l2len(msg_tch) >= 1
			 && (msg_tch->l2h[0] >> 4) != 0xd) {
				LOGL1SB(DL1P, LOGL_NOTICE, l1ts, br,
					"Transmitting 'bad FR frame'\n");
				goto free_bad_msg;
			}
			break;
		case GSM48_CMODE_SPEECH_EFR: /* EFR */
			if (br->chan != TRXC_TCHF)
				goto inval_mode2;
			len = GSM_EFR_BYTES;
			if (msgb_l2len(msg_tch) >= 1
			 && (msg_tch->l2h[0] >> 4) != 0xc) {
				LOGL1SB(DL1P, LOGL_NOTICE, l1ts, br,
					"Transmitting 'bad EFR frame'\n");
				goto free_bad_msg;
			}
			break;
		case GSM48_CMODE_SPEECH_AMR: /* AMR */
			/* TODO: check length for consistency */
			goto send_frame;
			break;
		default:
inval_mode2:
			LOGL1SB(DL1P, LOGL_ERROR, l1ts, br, "TCH mode invalid, please fix!\n");
			goto free_bad_msg;
		}
		if (len < 0) {
			LOGL1SB(DL1P, LOGL_ERROR, l1ts, br, "Cannot send invalid AMR payload\n");
			goto free_bad_msg;
		}
		if (msgb_l2len(msg_tch) != len) {
			LOGL1SB(DL1P, LOGL_ERROR, l1ts, br, "Cannot send payload with "
				"invalid length! (expecing %d, received %d)\n", len, msgb_l2len(msg_tch));
free_bad_msg:
			/* free message */
			msgb_free(msg_tch);
			msg_tch = NULL;
			goto send_frame;
		}
	}

send_frame:
	*_msg_tch = msg_tch;
	*_msg_facch = msg_facch;
}

int tx_tchf_fn(struct l1sched_ts *l1ts, struct trx_dl_burst_req *br)
{
	struct msgb *msg_tch = NULL, *msg_facch = NULL;

	if (br->bid > 0)
		return 0;

	tx_tch_common(l1ts, br, &msg_tch, &msg_facch, (((br->fn + 4) % 26) >> 2) & 1);

	/* no message at all */
	if (!msg_tch && !msg_facch) {
		LOGL1SB(DL1P, LOGL_INFO, l1ts, br, "has not been served !! No prim\n");
		return -ENODEV;
	}

	if (msg_facch) {
		tx_to_virt_um(l1ts, br, msg_facch);
		msgb_free(msg_tch);
	} else if (msg_tch)
		tx_to_virt_um_voice_frame(l1ts, br, msg_tch);

	return 0;
}

int tx_tchh_fn(struct l1sched_ts *l1ts, struct trx_dl_burst_req *br)
{
	struct msgb *msg_tch = NULL, *msg_facch = NULL;
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[br->chan];
	//uint8_t tch_mode = chan_state->tch_mode;

	/* send burst, if we already got a frame */
	if (br->bid > 0)
		return 0;

	/* get TCH and/or FACCH */
	tx_tch_common(l1ts, br, &msg_tch, &msg_facch, (((br->fn + 4) % 26) >> 2) & 1);

	/* check for FACCH alignment */
	if (msg_facch && ((((br->fn + 4) % 26) >> 2) & 1)) {
		LOGL1SB(DL1P, LOGL_ERROR, l1ts, br, "Cannot transmit FACCH starting on "
			"even frames, please fix RTS!\n");
		msgb_free(msg_facch);
		msg_facch = NULL;
	}

	/* no message at all */
	if (!msg_tch && !msg_facch && !chan_state->dl_ongoing_facch) {
		LOGL1SB(DL1P, LOGL_INFO, l1ts, br, "has not been served !! No prim\n");
		return -ENODEV;
	}

	if (msg_facch) {
		tx_to_virt_um(l1ts, br, msg_facch);
		msgb_free(msg_tch);
	} else if (msg_tch)
		tx_to_virt_um_voice_frame(l1ts, br, msg_tch);

	return 0;
}


/***********************************************************************
 * RX on uplink (indication to upper layer)
 ***********************************************************************/

/* we don't use those functions, as we feed the MAC frames from GSMTAP
 * directly into the L1SAP, bypassing the TDMA multiplex logic oriented
 * towards receiving bursts */

int rx_rach_fn(struct l1sched_ts *l1ts, const struct trx_ul_burst_ind *bi)
{
	return 0;
}

/*! \brief a single burst was received by the PHY, process it */
int rx_data_fn(struct l1sched_ts *l1ts, const struct trx_ul_burst_ind *bi)
{
	return 0;
}

int rx_pdtch_fn(struct l1sched_ts *l1ts, const struct trx_ul_burst_ind *bi)
{
	return 0;
}

int rx_tchf_fn(struct l1sched_ts *l1ts, const struct trx_ul_burst_ind *bi)
{
	return 0;
}

int rx_tchh_fn(struct l1sched_ts *l1ts, const struct trx_ul_burst_ind *bi)
{
	return 0;
}

void _sched_act_rach_det(struct gsm_bts_trx *trx, uint8_t tn, uint8_t ss, int activate)
{
}

/***********************************************************************
 * main scheduler function
 ***********************************************************************/

#define RTS_ADVANCE		5	/* about 20ms */

static int vbts_sched_fn(struct gsm_bts *bts, uint32_t fn)
{
	struct gsm_bts_trx *trx;

	/* send time indication */
	/* update model with new frame number, lot of stuff happening, measurements of timeslots */
	/* saving GSM time in BTS model, and more */
	l1if_mph_time_ind(bts, fn);

	/* advance the frame number? */
	llist_for_each_entry(trx, &bts->trx_list, list) {
		struct trx_dl_burst_req br = { .fn = fn };

		/* do for each of the 8 timeslots */
		for (br.tn = 0; br.tn < ARRAY_SIZE(trx->ts); br.tn++) {
			struct l1sched_ts *l1ts = trx->ts[br.tn].priv;
			/* Generate RTS indication to higher layers */
			/* This will basically do 2 things (check l1_if:bts_model_l1sap_down):
			 * 1) Get pending messages from layer 2 (from the lapdm queue)
			 * 2) Process the messages
			 *    --> Handle and process non-transparent RSL-Messages (activate channel, )
			 *    --> Forward transparent RSL-DATA-Messages to the ms by appending them to
			 *        the l1-dl-queue */
			_sched_rts(l1ts, GSM_TDMA_FN_SUM(fn, RTS_ADVANCE));
			/* schedule transmit backend functions */
			/* Process data in the l1-dlqueue and forward it
			 * to MS */
			/* the returned bits are not used here, the routines called will directly forward their
			 * bits to the virt Um */
			_sched_dl_burst(l1ts, &br);
		}
	}

	return 0;
}

static void vbts_fn_timer_cb(void *data)
{
	struct gsm_bts *bts = data;
	struct bts_virt_priv *bts_virt = (struct bts_virt_priv *)bts->model_priv;
	struct timeval tv_now;
	struct timeval *tv_clock = &bts_virt->tv_clock;
	int32_t elapsed_us;

	gettimeofday(&tv_now, NULL);

	/* check how much time elapsed till the last timer callback call.
	 * this value should be about 4.615 ms (a bit greater) as this is the scheduling interval */
	elapsed_us = (tv_now.tv_sec - tv_clock->tv_sec) * 1000000
	                + (tv_now.tv_usec - tv_clock->tv_usec);

	/* not so good somehow a lot of time passed between two timer callbacks */
	if (elapsed_us > 2 *GSM_TDMA_FN_DURATION_uS)
		LOGP(DL1P, LOGL_NOTICE, "vbts_fn_timer_cb after %d us\n", elapsed_us);

	/* schedule the current frame/s (fn = frame number)
	 * this loop will be called at least once, but can also be executed
	 * multiple times if more than one frame duration (4615us) passed till the last callback */
	while (elapsed_us > GSM_TDMA_FN_DURATION_uS / 2) {
		const struct timeval tv_frame = {
			.tv_sec = 0,
			.tv_usec = GSM_TDMA_FN_DURATION_uS,
		};
		timeradd(tv_clock, &tv_frame, tv_clock);
		/* increment the frame number in the BTS model instance */
		vbts_sched_fn(bts, GSM_TDMA_FN_INC(bts_virt->last_fn));
		elapsed_us -= GSM_TDMA_FN_DURATION_uS;
	}

	/* re-schedule the timer */
	/* timer is set to frame duration - elapsed time to guarantee that this cb method will be
	 * periodically executed every 4.615ms */
	osmo_timer_schedule(&bts_virt->fn_timer, 0, GSM_TDMA_FN_DURATION_uS - elapsed_us);
}

int vbts_sched_start(struct gsm_bts *bts)
{
	struct bts_virt_priv *bts_virt = (struct bts_virt_priv *)bts->model_priv;
	LOGP(DL1P, LOGL_NOTICE, "starting VBTS scheduler\n");

	memset(&bts_virt->fn_timer, 0, sizeof(bts_virt->fn_timer));
	bts_virt->fn_timer.cb = vbts_fn_timer_cb;
	bts_virt->fn_timer.data = bts;

	gettimeofday(&bts_virt->tv_clock, NULL);
	/* trigger the first timer after 4615us (a frame duration) */
	osmo_timer_schedule(&bts_virt->fn_timer, 0, GSM_TDMA_FN_DURATION_uS);

	return 0;
}
