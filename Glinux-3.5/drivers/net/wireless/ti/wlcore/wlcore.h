/*
 * This file is part of wlcore
 *
 * Copyright (C) 2011 Texas Instruments Inc.
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

#ifndef __WLCORE_H__
#define __WLCORE_H__

#include <linux/platform_device.h>

#include "wl12xx.h"
#include "event.h"

/* The maximum number of Tx descriptors in all chip families */
#define WLCORE_MAX_TX_DESCRIPTORS 32

/* forward declaration */
struct wl1271_tx_hw_descr;
enum wl_rx_buf_align;

struct wlcore_ops {
	int (*identify_chip)(struct wl1271 *wl);
	int (*identify_fw)(struct wl1271 *wl);
	int (*boot)(struct wl1271 *wl);
	void (*trigger_cmd)(struct wl1271 *wl, int cmd_box_addr,
			    void *buf, size_t len);
	void (*ack_event)(struct wl1271 *wl);
	u32 (*calc_tx_blocks)(struct wl1271 *wl, u32 len, u32 spare_blks);
	void (*set_tx_desc_blocks)(struct wl1271 *wl,
				   struct wl1271_tx_hw_descr *desc,
				   u32 blks, u32 spare_blks);
	void (*set_tx_desc_data_len)(struct wl1271 *wl,
				     struct wl1271_tx_hw_descr *desc,
				     struct sk_buff *skb);
	enum wl_rx_buf_align (*get_rx_buf_align)(struct wl1271 *wl,
						 u32 rx_desc);
	void (*prepare_read)(struct wl1271 *wl, u32 rx_desc, u32 len);
	u32 (*get_rx_packet_len)(struct wl1271 *wl, void *rx_data,
				 u32 data_len);
	void (*tx_delayed_compl)(struct wl1271 *wl);
	void (*tx_immediate_compl)(struct wl1271 *wl);
	int (*hw_init)(struct wl1271 *wl);
	int (*init_vif)(struct wl1271 *wl, struct wl12xx_vif *wlvif);
	u32 (*sta_get_ap_rate_mask)(struct wl1271 *wl,
				    struct wl12xx_vif *wlvif);
	s8 (*get_pg_ver)(struct wl1271 *wl);
	void (*get_mac)(struct wl1271 *wl);
} __no_const;

enum wlcore_partitions {
	PART_DOWN,
	PART_WORK,
	PART_BOOT,
	PART_DRPW,
	PART_TOP_PRCM_ELP_SOC,
	PART_PHY_INIT,

	PART_TABLE_LEN,
};

struct wlcore_partition {
	u32 size;
	u32 start;
};

struct wlcore_partition_set {
	struct wlcore_partition mem;
	struct wlcore_partition reg;
	struct wlcore_partition mem2;
	struct wlcore_partition mem3;
};

enum wlcore_registers {
	/* register addresses, used with partition translation */
	REG_ECPU_CONTROL,
	REG_INTERRUPT_NO_CLEAR,
	REG_INTERRUPT_ACK,
	REG_COMMAND_MAILBOX_PTR,
	REG_EVENT_MAILBOX_PTR,
	REG_INTERRUPT_TRIG,
	REG_INTERRUPT_MASK,
	REG_PC_ON_RECOVERY,
	REG_CHIP_ID_B,
	REG_CMD_MBOX_ADDRESS,

	/* data access memory addresses, used with partition translation */
	REG_SLV_MEM_DATA,
	REG_SLV_REG_DATA,

	/* raw data access memory addresses */
	REG_RAW_FW_STATUS_ADDR,

	REG_TABLE_LEN,
};

struct wl1271 {
	struct ieee80211_hw *hw;
	bool mac80211_registered;

	struct device *dev;

	void *if_priv;

	struct wl1271_if_operations *if_ops;

	void (*set_power)(bool enable);
	int irq;
	int ref_clock;

	spinlock_t wl_lock;

	enum wl1271_state state;
	enum wl12xx_fw_type fw_type;
	bool plt;
	u8 last_vif_count;
	struct mutex mutex;

	unsigned long flags;

	struct wlcore_partition_set curr_part;

	struct wl1271_chip chip;

	int cmd_box_addr;

	u8 *fw;
	size_t fw_len;
	void *nvs;
	size_t nvs_len;

	s8 hw_pg_ver;

	/* address read from the fuse ROM */
	u32 fuse_oui_addr;
	u32 fuse_nic_addr;

	/* we have up to 2 MAC addresses */
	struct mac_address addresses[2];
	int channel;
	u8 system_hlid;

	unsigned long links_map[BITS_TO_LONGS(WL12XX_MAX_LINKS)];
	unsigned long roles_map[BITS_TO_LONGS(WL12XX_MAX_ROLES)];
	unsigned long roc_map[BITS_TO_LONGS(WL12XX_MAX_ROLES)];
	unsigned long rate_policies_map[
			BITS_TO_LONGS(WL12XX_MAX_RATE_POLICIES)];

	struct list_head wlvif_list;

	u8 sta_count;
	u8 ap_count;

	struct wl1271_acx_mem_map *target_mem_map;

	/* Accounting for allocated / available TX blocks on HW */
	u32 tx_blocks_freed;
	u32 tx_blocks_available;
	u32 tx_allocated_blocks;
	u32 tx_results_count;

	/* Accounting for allocated / available Tx packets in HW */
	u32 tx_pkts_freed[NUM_TX_QUEUES];
	u32 tx_allocated_pkts[NUM_TX_QUEUES];

	/* Transmitted TX packets counter for chipset interface */
	u32 tx_packets_count;

	/* Time-offset between host and chipset clocks */
	s64 time_offset;

	/* Frames scheduled for transmission, not handled yet */
	int tx_queue_count[NUM_TX_QUEUES];
	long stopped_queues_map;

	/* Frames received, not handled yet by mac80211 */
	struct sk_buff_head deferred_rx_queue;

	/* Frames sent, not returned yet to mac80211 */
	struct sk_buff_head deferred_tx_queue;

	struct work_struct tx_work;
	struct workqueue_struct *freezable_wq;

	/* Pending TX frames */
	unsigned long tx_frames_map[BITS_TO_LONGS(WLCORE_MAX_TX_DESCRIPTORS)];
	struct sk_buff *tx_frames[WLCORE_MAX_TX_DESCRIPTORS];
	int tx_frames_cnt;

	/* FW Rx counter */
	u32 rx_counter;

	/* Rx memory pool address */
	struct wl1271_rx_mem_pool_addr rx_mem_pool_addr;

	/* Intermediate buffer, used for packet aggregation */
	u8 *aggr_buf;

	/* Reusable dummy packet template */
	struct sk_buff *dummy_packet;

	/* Network stack work  */
	struct work_struct netstack_work;

	/* FW log buffer */
	u8 *fwlog;

	/* Number of valid bytes in the FW log buffer */
	ssize_t fwlog_size;

	/* Sysfs FW log entry readers wait queue */
	wait_queue_head_t fwlog_waitq;

	/* Hardware recovery work */
	struct work_struct recovery_work;

	/* Pointer that holds DMA-friendly block for the mailbox */
	struct event_mailbox *mbox;

	/* The mbox event mask */
	u32 event_mask;

	/* Mailbox pointers */
	u32 mbox_ptr[2];

	/* Are we currently scanning */
	struct ieee80211_vif *scan_vif;
	struct wl1271_scan scan;
	struct delayed_work scan_complete_work;

	/* Connection loss work */
	struct delayed_work connection_loss_work;

	bool sched_scanning;

	/* The current band */
	enum ieee80211_band band;

	struct completion *elp_compl;
	struct delayed_work elp_work;

	/* in dBm */
	int power_level;

	struct wl1271_stats stats;

	__le32 buffer_32;
	u32 buffer_cmd;
	u32 buffer_busyword[WL1271_BUSY_WORD_CNT];

	struct wl_fw_status *fw_status;
	struct wl1271_tx_hw_res_if *tx_res_if;

	/* Current chipset configuration */
	struct wlcore_conf conf;

	bool sg_enabled;

	bool enable_11a;

	/* Most recently reported noise in dBm */
	s8 noise;

	/* bands supported by this instance of wl12xx */
	struct ieee80211_supported_band bands[IEEE80211_NUM_BANDS];

	int tcxo_clock;

	/*
	 * wowlan trigger was configured during suspend.
	 * (currently, only "ANY" trigger is supported)
	 */
	bool wow_enabled;
	bool irq_wake_enabled;

	/*
	 * AP-mode - links indexed by HLID. The global and broadcast links
	 * are always active.
	 */
	struct wl1271_link links[WL12XX_MAX_LINKS];

	/* AP-mode - a bitmap of links currently in PS mode according to FW */
	u32 ap_fw_ps_map;

	/* AP-mode - a bitmap of links currently in PS mode in mac80211 */
	unsigned long ap_ps_map;

	/* Quirks of specific hardware revisions */
	unsigned int quirks;

	/* Platform limitations */
	unsigned int platform_quirks;

	/* number of currently active RX BA sessions */
	int ba_rx_session_count;

	/* AP-mode - number of currently connected stations */
	int active_sta_count;

	/* last wlvif we transmitted from */
	struct wl12xx_vif *last_wlvif;

	/* work to fire when Tx is stuck */
	struct delayed_work tx_watchdog_work;

	struct wlcore_ops *ops;
	/* pointer to the lower driver partition table */
	const struct wlcore_partition_set *ptable;
	/* pointer to the lower driver register table */
	const int *rtable;
	/* name of the firmwares to load - for PLT, single role, multi-role */
	const char *plt_fw_name;
	const char *sr_fw_name;
	const char *mr_fw_name;

	/* per-chip-family private structure */
	void *priv;

	/* number of TX descriptors the HW supports. */
	u32 num_tx_desc;

	/* spare Tx blocks for normal/GEM operating modes */
	u32 normal_tx_spare;
	u32 gem_tx_spare;

	/* translate HW Tx rates to standard rate-indices */
	const u8 **band_rate_to_idx;

	/* size of table for HW rates that can be received from chip */
	u8 hw_tx_rate_tbl_size;

	/* this HW rate and below are considered HT rates for this chip */
	u8 hw_min_ht_rate;

	/* HW HT (11n) capabilities */
	struct ieee80211_sta_ht_cap ht_cap;

	/* size of the private FW status data */
	size_t fw_status_priv_len;

	/* RX Data filter rule state - enabled/disabled */
	bool rx_filter_enabled[WL1271_MAX_RX_FILTERS];
};

int __devinit wlcore_probe(struct wl1271 *wl, struct platform_device *pdev);
int __devexit wlcore_remove(struct platform_device *pdev);
struct ieee80211_hw *wlcore_alloc_hw(size_t priv_size);
int wlcore_free_hw(struct wl1271 *wl);

/* Firmware image load chunk size */
#define CHUNK_SIZE	16384

/* Quirks */

/* Each RX/TX transaction requires an end-of-transaction transfer */
#define WLCORE_QUIRK_END_OF_TRANSACTION		BIT(0)

/* wl127x and SPI don't support SDIO block size alignment */
#define WLCORE_QUIRK_TX_BLOCKSIZE_ALIGN		BIT(2)

/* means aggregated Rx packets are aligned to a SDIO block */
#define WLCORE_QUIRK_RX_BLOCKSIZE_ALIGN		BIT(3)

/* Older firmwares did not implement the FW logger over bus feature */
#define WLCORE_QUIRK_FWLOG_NOT_IMPLEMENTED	BIT(4)

/* Older firmwares use an old NVS format */
#define WLCORE_QUIRK_LEGACY_NVS			BIT(5)

/* Some firmwares may not support ELP */
#define WLCORE_QUIRK_NO_ELP			BIT(6)

/* TODO: move to the lower drivers when all usages are abstracted */
#define CHIP_ID_1271_PG10              (0x4030101)
#define CHIP_ID_1271_PG20              (0x4030111)
#define CHIP_ID_1283_PG10              (0x05030101)
#define CHIP_ID_1283_PG20              (0x05030111)

/* TODO: move all these common registers and values elsewhere */
#define HW_ACCESS_ELP_CTRL_REG		0x1FFFC

/* ELP register commands */
#define ELPCTRL_WAKE_UP             0x1
#define ELPCTRL_WAKE_UP_WLAN_READY  0x5
#define ELPCTRL_SLEEP               0x0
/* ELP WLAN_READY bit */
#define ELPCTRL_WLAN_READY          0x2

/*************************************************************************

    Interrupt Trigger Register (Host -> WiLink)

**************************************************************************/

/* Hardware to Embedded CPU Interrupts - first 32-bit register set */

/*
 * The host sets this bit to inform the Wlan
 * FW that a TX packet is in the XFER
 * Buffer #0.
 */
#define INTR_TRIG_TX_PROC0 BIT(2)

/*
 * The host sets this bit to inform the FW
 * that it read a packet from RX XFER
 * Buffer #0.
 */
#define INTR_TRIG_RX_PROC0 BIT(3)

#define INTR_TRIG_DEBUG_ACK BIT(4)

#define INTR_TRIG_STATE_CHANGED BIT(5)

/* Hardware to Embedded CPU Interrupts - second 32-bit register set */

/*
 * The host sets this bit to inform the FW
 * that it read a packet from RX XFER
 * Buffer #1.
 */
#define INTR_TRIG_RX_PROC1 BIT(17)

/*
 * The host sets this bit to inform the Wlan
 * hardware that a TX packet is in the XFER
 * Buffer #1.
 */
#define INTR_TRIG_TX_PROC1 BIT(18)

#define ACX_SLV_SOFT_RESET_BIT	BIT(1)
#define SOFT_RESET_MAX_TIME	1000000
#define SOFT_RESET_STALL_TIME	1000

#define ECPU_CONTROL_HALT	0x00000101

#define WELP_ARM_COMMAND_VAL	0x4

#endif /* __WLCORE_H__ */
