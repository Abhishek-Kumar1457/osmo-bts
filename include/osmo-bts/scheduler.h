#ifndef TRX_SCHEDULER_H
#define TRX_SCHEDULER_H

#include <osmocom/core/utils.h>
#include <osmocom/core/rate_ctr.h>

#include <osmo-bts/gsm_data.h>

/* Whether a logical channel must be activated automatically */
#define TRX_CHAN_FLAG_AUTO_ACTIVE	(1 << 0)
/* Whether a logical channel belongs to PDCH (packet switched data) */
#define TRX_CHAN_FLAG_PDCH		(1 << 1)

/* FIXME: we should actually activate 'auto-active' channels */
#define TRX_CHAN_IS_ACTIVE(state, chan) \
	(trx_chan_desc[chan].flags & TRX_CHAN_FLAG_AUTO_ACTIVE || (state)->active)

/* These types define the different channels on a multiframe.
 * Each channel has queues and can be activated individually.
 */
enum trx_chan_type {
	TRXC_IDLE = 0,
	TRXC_FCCH,
	TRXC_SCH,
	TRXC_BCCH,
	TRXC_RACH,
	TRXC_CCCH,
	TRXC_TCHF,
	TRXC_TCHH_0,
	TRXC_TCHH_1,
	TRXC_SDCCH4_0,
	TRXC_SDCCH4_1,
	TRXC_SDCCH4_2,
	TRXC_SDCCH4_3,
	TRXC_SDCCH8_0,
	TRXC_SDCCH8_1,
	TRXC_SDCCH8_2,
	TRXC_SDCCH8_3,
	TRXC_SDCCH8_4,
	TRXC_SDCCH8_5,
	TRXC_SDCCH8_6,
	TRXC_SDCCH8_7,
	TRXC_SACCHTF,
	TRXC_SACCHTH_0,
	TRXC_SACCHTH_1,
	TRXC_SACCH4_0,
	TRXC_SACCH4_1,
	TRXC_SACCH4_2,
	TRXC_SACCH4_3,
	TRXC_SACCH8_0,
	TRXC_SACCH8_1,
	TRXC_SACCH8_2,
	TRXC_SACCH8_3,
	TRXC_SACCH8_4,
	TRXC_SACCH8_5,
	TRXC_SACCH8_6,
	TRXC_SACCH8_7,
	TRXC_PDTCH,
	TRXC_PTCCH,
	TRXC_CBCH,
	_TRX_CHAN_MAX
};

#define GSM_BURST_LEN		148
#define GPRS_BURST_LEN		GSM_BURST_LEN
#define EGPRS_BURST_LEN		444

enum trx_burst_type {
	TRX_BURST_GMSK,
	TRX_BURST_8PSK,
};

/* States each channel on a multiframe */
struct l1sched_chan_state {
	/* Pointer to the associated logical channel state from gsm_data_shared.
	 * Initialized during channel activation, thus may be NULL for inactive
	 * or auto-active channels. Always check before dereferencing! */
	struct gsm_lchan	*lchan;

	/* scheduler */
	bool			active;		/* Channel is active */
	ubit_t			*dl_bursts;	/* burst buffer for TX */
	enum trx_burst_type	dl_burst_type;  /* GMSK or 8PSK burst type */
	sbit_t			*ul_bursts;	/* burst buffer for RX */
	uint32_t		ul_first_fn;	/* fn of first burst */
	uint8_t			ul_mask;	/* mask of received bursts */

	/* measurements */
	uint8_t			rssi_num;	/* number of RSSI values */
	float			rssi_sum;	/* sum of RSSI values */
	uint8_t			toa_num;	/* number of TOA values */
	int32_t			toa256_sum;	/* sum of TOA values (1/256 symbol) */
	uint8_t			ci_cb_num;	/* number of C/I values */
	int32_t			ci_cb_sum;	/* sum of C/I values (in centiBels) */

	/* loss detection */
	uint8_t			lost_frames;	/* how many L2 frames were lost */
	uint32_t		last_tdma_fn;	/* last processed TDMA frame number */
	uint32_t		proc_tdma_fs;	/* how many TDMA frames were processed */
	uint32_t		lost_tdma_fs;	/* how many TDMA frames were lost */

	/* mode */
	uint8_t			rsl_cmode, tch_mode; /* mode for TCH channels */

	/* AMR */
	uint8_t			codec[4];	/* 4 possible codecs for amr */
	int			codecs;		/* number of possible codecs */
	float			ber_sum;	/* sum of bit error rates */
	int			ber_num;	/* number of bit error rates */
	uint8_t			ul_ft;		/* current uplink FT index */
	uint8_t			dl_ft;		/* current downlink FT index */
	uint8_t			ul_cmr;		/* current uplink CMR index */
	uint8_t			dl_cmr;		/* current downlink CMR index */
	uint8_t			amr_loop;	/* if AMR loop is enabled */
	uint8_t			amr_last_dtx;	/* last received dtx frame type */

	/* TCH/H */
	uint8_t			dl_ongoing_facch; /* FACCH/H on downlink */
	uint8_t			ul_ongoing_facch; /* FACCH/H on uplink */

	/* encryption */
	int			ul_encr_algo;	/* A5/x encry algo downlink */
	int			dl_encr_algo;	/* A5/x encry algo uplink */
	int			ul_encr_key_len;
	int			dl_encr_key_len;
	uint8_t			ul_encr_key[MAX_A5_KEY_LEN];
	uint8_t			dl_encr_key[MAX_A5_KEY_LEN];

	/* measurements */
	/* TODO: measurement history (ring buffer) will be added here */

	/* handover */
	bool			ho_rach_detect;	/* if rach detection is on */
};

struct l1sched_ts {
	uint8_t 		mf_index;	/* selected multiframe index */
	uint8_t			mf_period;	/* period of multiframe */
	const struct trx_sched_frame *mf_frames; /* pointer to frame layout */

	struct llist_head	dl_prims;	/* Queue primitives for TX */

	struct rate_ctr_group	*ctrs;		/* rate counters */

	/* Channel states for all logical channels */
	struct l1sched_chan_state chan_state[_TRX_CHAN_MAX];
};

struct l1sched_trx {
	struct gsm_bts_trx	*trx;
	struct l1sched_ts       ts[TRX_NR_TS];
};

struct l1sched_ts *l1sched_trx_get_ts(struct l1sched_trx *l1t, uint8_t tn);


/*! \brief Initialize the scheduler data structures */
int trx_sched_init(struct l1sched_trx *l1t, struct gsm_bts_trx *trx);

/*! \brief De-initialize the scheduler data structures */
void trx_sched_exit(struct l1sched_trx *l1t);

/*! \brief Handle a PH-DATA.req from L2 down to L1 */
int trx_sched_ph_data_req(struct l1sched_trx *l1t, struct osmo_phsap_prim *l1sap);

/*! \brief Handle a PH-TCH.req from L2 down to L1 */
int trx_sched_tch_req(struct l1sched_trx *l1t, struct osmo_phsap_prim *l1sap);

/*! \brief PHY informs us of new (current) GSM frame number */
int trx_sched_clock(struct gsm_bts *bts, uint32_t fn);

/*! \brief PHY informs us clock indications should start to be received */
int trx_sched_clock_started(struct gsm_bts *bts);

/*! \brief PHY informs us no more clock indications should be received anymore */
int trx_sched_clock_stopped(struct gsm_bts *bts);

/*! \brief set multiframe scheduler to given physical channel config */
int trx_sched_set_pchan(struct l1sched_trx *l1t, uint8_t tn,
        enum gsm_phys_chan_config pchan);

/*! \brief set all matching logical channels active/inactive */
int trx_sched_set_lchan(struct l1sched_trx *l1t, uint8_t chan_nr, uint8_t link_id, bool active);

/*! \brief set mode of all matching logical channels to given mode(s) */
int trx_sched_set_mode(struct l1sched_trx *l1t, uint8_t chan_nr, uint8_t rsl_cmode,
	uint8_t tch_mode, int codecs, uint8_t codec0, uint8_t codec1,
	uint8_t codec2, uint8_t codec3, uint8_t initial_codec,
	uint8_t handover);

/*! \brief set ciphering on given logical channels */
int trx_sched_set_cipher(struct l1sched_trx *l1t, uint8_t chan_nr, int downlink,
        int algo, uint8_t *key, int key_len);

/* frame structures */
struct trx_sched_frame {
	/*! \brief downlink TRX channel type */
	enum trx_chan_type		dl_chan;
	/*! \brief downlink block ID */
	uint8_t				dl_bid;
	/*! \brief uplink TRX channel type */
	enum trx_chan_type		ul_chan;
	/*! \brief uplink block ID */
	uint8_t				ul_bid;
};

/* multiframe structure */
struct trx_sched_multiframe {
	/*! \brief physical channel config (channel combination) */
	enum gsm_phys_chan_config	pchan;
	/*! \brief applies to which timeslots? */
	uint8_t				slotmask;
	/*! \brief repeats how many frames */
	uint8_t				period;
	/*! \brief pointer to scheduling structure */
	const struct trx_sched_frame	*frames;
	/*! \brief human-readable name */
	const char 			*name;
};

int find_sched_mframe_idx(enum gsm_phys_chan_config pchan, uint8_t tn);

/*! Determine if given frame number contains SACCH (true) or other (false) burst */
bool trx_sched_is_sacch_fn(struct gsm_bts_trx_ts *ts, uint32_t fn, bool uplink);
extern const struct trx_sched_multiframe trx_sched_multiframes[];

#define TRX_BI_F_NOPE_IND	(1 << 0)
#define TRX_BI_F_MOD_TYPE	(1 << 1)
#define TRX_BI_F_TS_INFO	(1 << 2)
#define TRX_BI_F_CI_CB		(1 << 3)

/*! UL burst indication with the corresponding meta info */
struct trx_ul_burst_ind {
	/* Field presence bitmask (see TRX_BI_F_*) */
	uint8_t flags;

	/* Mandatory fields */
	uint32_t fn;		/*!< TDMA frame number */
	uint8_t tn;		/*!< TDMA time-slot number */
	int16_t toa256;		/*!< Timing of Arrival in units of 1/256 of symbol */
	int8_t rssi;		/*!< Received Signal Strength Indication */

	/* Optional fields (defined by flags) */
	enum trx_burst_type bt;	/*!< Modulation type */
	uint8_t tsc_set;	/*!< Training Sequence Set */
	uint8_t tsc;		/*!< Training Sequence Code */
	int16_t ci_cb;		/*!< Carrier-to-Interference ratio (in centiBels) */

	/*! Burst soft-bits buffer */
	sbit_t burst[EGPRS_BURST_LEN];
	size_t burst_len;
};

/*! DL burst request with the corresponding meta info */
struct trx_dl_burst_req {
	uint32_t fn;		/*!< TDMA frame number */
	uint8_t tn;		/*!< TDMA timeslot number */
	uint8_t att;		/*!< Tx power attenuation */

	/*! Burst hard-bits buffer */
	ubit_t burst[EGPRS_BURST_LEN];
	size_t burst_len;
};

/*! Handle an UL burst received by PHY */
int trx_sched_route_burst_ind(struct trx_ul_burst_ind *bi, struct l1sched_trx *l1t);
int trx_sched_ul_burst(struct l1sched_trx *l1t, struct trx_ul_burst_ind *bi);

#endif /* TRX_SCHEDULER_H */
