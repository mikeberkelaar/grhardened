/******************************************************************************
 *
 * Copyright(c) 2003 - 2008 Intel Corporation. All rights reserved.
 *
 * Portions of this file are derived from the ipw3945 project, as well
 * as portions of the ieee80211 subsystem header files.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 * James P. Ketrenos <ipw2100-admin@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 *****************************************************************************/

#include <linux/etherdevice.h>
#include <net/mac80211.h>
#include <asm/unaligned.h>
#include "iwl-eeprom.h"
#include "iwl-dev.h"
#include "iwl-core.h"
#include "iwl-sta.h"
#include "iwl-io.h"
#include "iwl-calib.h"
#include "iwl-helpers.h"
/************************** RX-FUNCTIONS ****************************/
/*
 * Rx theory of operation
 *
 * Driver allocates a circular buffer of Receive Buffer Descriptors (RBDs),
 * each of which point to Receive Buffers to be filled by the NIC.  These get
 * used not only for Rx frames, but for any command response or notification
 * from the NIC.  The driver and NIC manage the Rx buffers by means
 * of indexes into the circular buffer.
 *
 * Rx Queue Indexes
 * The host/firmware share two index registers for managing the Rx buffers.
 *
 * The READ index maps to the first position that the firmware may be writing
 * to -- the driver can read up to (but not including) this position and get
 * good data.
 * The READ index is managed by the firmware once the card is enabled.
 *
 * The WRITE index maps to the last position the driver has read from -- the
 * position preceding WRITE is the last slot the firmware can place a packet.
 *
 * The queue is empty (no good data) if WRITE = READ - 1, and is full if
 * WRITE = READ.
 *
 * During initialization, the host sets up the READ queue position to the first
 * INDEX position, and WRITE to the last (READ - 1 wrapped)
 *
 * When the firmware places a packet in a buffer, it will advance the READ index
 * and fire the RX interrupt.  The driver can then query the READ index and
 * process as many packets as possible, moving the WRITE index forward as it
 * resets the Rx queue buffers with new memory.
 *
 * The management in the driver is as follows:
 * + A list of pre-allocated SKBs is stored in iwl->rxq->rx_free.  When
 *   iwl->rxq->free_count drops to or below RX_LOW_WATERMARK, work is scheduled
 *   to replenish the iwl->rxq->rx_free.
 * + In iwl_rx_replenish (scheduled) if 'processed' != 'read' then the
 *   iwl->rxq is replenished and the READ INDEX is updated (updating the
 *   'processed' and 'read' driver indexes as well)
 * + A received packet is processed and handed to the kernel network stack,
 *   detached from the iwl->rxq.  The driver 'processed' index is updated.
 * + The Host/Firmware iwl->rxq is replenished at tasklet time from the rx_free
 *   list. If there are no allocated buffers in iwl->rxq->rx_free, the READ
 *   INDEX is not incremented and iwl->status(RX_STALLED) is set.  If there
 *   were enough free buffers and RX_STALLED is set it is cleared.
 *
 *
 * Driver sequence:
 *
 * iwl_rx_queue_alloc()   Allocates rx_free
 * iwl_rx_replenish()     Replenishes rx_free list from rx_used, and calls
 *                            iwl_rx_queue_restock
 * iwl_rx_queue_restock() Moves available buffers from rx_free into Rx
 *                            queue, updates firmware pointers, and updates
 *                            the WRITE index.  If insufficient rx_free buffers
 *                            are available, schedules iwl_rx_replenish
 *
 * -- enable interrupts --
 * ISR - iwl_rx()         Detach iwl_rx_mem_buffers from pool up to the
 *                            READ INDEX, detaching the SKB from the pool.
 *                            Moves the packet buffer from queue to rx_used.
 *                            Calls iwl_rx_queue_restock to refill any empty
 *                            slots.
 * ...
 *
 */

/**
 * iwl_rx_queue_space - Return number of free slots available in queue.
 */
int iwl_rx_queue_space(const struct iwl_rx_queue *q)
{
	int s = q->read - q->write;
	if (s <= 0)
		s += RX_QUEUE_SIZE;
	/* keep some buffer to not confuse full and empty queue */
	s -= 2;
	if (s < 0)
		s = 0;
	return s;
}
EXPORT_SYMBOL(iwl_rx_queue_space);

/**
 * iwl_rx_queue_update_write_ptr - Update the write pointer for the RX queue
 */
int iwl_rx_queue_update_write_ptr(struct iwl_priv *priv, struct iwl_rx_queue *q)
{
	u32 reg = 0;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);

	if (q->need_update == 0)
		goto exit_unlock;

	/* If power-saving is in use, make sure device is awake */
	if (test_bit(STATUS_POWER_PMI, &priv->status)) {
		reg = iwl_read32(priv, CSR_UCODE_DRV_GP1);

		if (reg & CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP) {
			iwl_set_bit(priv, CSR_GP_CNTRL,
				    CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
			goto exit_unlock;
		}

		ret = iwl_grab_nic_access(priv);
		if (ret)
			goto exit_unlock;

		/* Device expects a multiple of 8 */
		iwl_write_direct32(priv, FH_RSCSR_CHNL0_WPTR,
				     q->write & ~0x7);
		iwl_release_nic_access(priv);

	/* Else device is assumed to be awake */
	} else
		/* Device expects a multiple of 8 */
		iwl_write32(priv, FH_RSCSR_CHNL0_WPTR, q->write & ~0x7);


	q->need_update = 0;

 exit_unlock:
	spin_unlock_irqrestore(&q->lock, flags);
	return ret;
}
EXPORT_SYMBOL(iwl_rx_queue_update_write_ptr);
/**
 * iwl_dma_addr2rbd_ptr - convert a DMA address to a uCode read buffer ptr
 */
static inline __le32 iwl_dma_addr2rbd_ptr(struct iwl_priv *priv,
					  dma_addr_t dma_addr)
{
	return cpu_to_le32((u32)(dma_addr >> 8));
}

/**
 * iwl_rx_queue_restock - refill RX queue from pre-allocated pool
 *
 * If there are slots in the RX queue that need to be restocked,
 * and we have free pre-allocated buffers, fill the ranks as much
 * as we can, pulling from rx_free.
 *
 * This moves the 'write' index forward to catch up with 'processed', and
 * also updates the memory address in the firmware to reference the new
 * target buffer.
 */
int iwl_rx_queue_restock(struct iwl_priv *priv)
{
	struct iwl_rx_queue *rxq = &priv->rxq;
	struct list_head *element;
	struct iwl_rx_mem_buffer *rxb;
	unsigned long flags;
	int write;
	int ret = 0;

	spin_lock_irqsave(&rxq->lock, flags);
	write = rxq->write & ~0x7;
	while ((iwl_rx_queue_space(rxq) > 0) && (rxq->free_count)) {
		/* Get next free Rx buffer, remove from free list */
		element = rxq->rx_free.next;
		rxb = list_entry(element, struct iwl_rx_mem_buffer, list);
		list_del(element);

		/* Point to Rx buffer via next RBD in circular buffer */
		rxq->bd[rxq->write] = iwl_dma_addr2rbd_ptr(priv, rxb->aligned_dma_addr);
		rxq->queue[rxq->write] = rxb;
		rxq->write = (rxq->write + 1) & RX_QUEUE_MASK;
		rxq->free_count--;
	}
	spin_unlock_irqrestore(&rxq->lock, flags);
	/* If the pre-allocated buffer pool is dropping low, schedule to
	 * refill it */
	if (rxq->free_count <= RX_LOW_WATERMARK)
		queue_work(priv->workqueue, &priv->rx_replenish);


	/* If we've added more space for the firmware to place data, tell it.
	 * Increment device's write pointer in multiples of 8. */
	if ((write != (rxq->write & ~0x7))
	    || (abs(rxq->write - rxq->read) > 7)) {
		spin_lock_irqsave(&rxq->lock, flags);
		rxq->need_update = 1;
		spin_unlock_irqrestore(&rxq->lock, flags);
		ret = iwl_rx_queue_update_write_ptr(priv, rxq);
	}

	return ret;
}
EXPORT_SYMBOL(iwl_rx_queue_restock);


/**
 * iwl_rx_replenish - Move all used packet from rx_used to rx_free
 *
 * When moving to rx_free an SKB is allocated for the slot.
 *
 * Also restock the Rx queue via iwl_rx_queue_restock.
 * This is called as a scheduled work item (except for during initialization)
 */
void iwl_rx_allocate(struct iwl_priv *priv)
{
	struct iwl_rx_queue *rxq = &priv->rxq;
	struct list_head *element;
	struct iwl_rx_mem_buffer *rxb;
	unsigned long flags;
	spin_lock_irqsave(&rxq->lock, flags);
	while (!list_empty(&rxq->rx_used)) {
		element = rxq->rx_used.next;
		rxb = list_entry(element, struct iwl_rx_mem_buffer, list);

		/* Alloc a new receive buffer */
		rxb->skb = alloc_skb(priv->hw_params.rx_buf_size + 256,
				__GFP_NOWARN | GFP_ATOMIC);
		if (!rxb->skb) {
			if (net_ratelimit())
				printk(KERN_CRIT DRV_NAME
				       ": Can not allocate SKB buffers\n");
			/* We don't reschedule replenish work here -- we will
			 * call the restock method and if it still needs
			 * more buffers it will schedule replenish */
			break;
		}
		priv->alloc_rxb_skb++;
		list_del(element);

		/* Get physical address of RB/SKB */
		rxb->real_dma_addr = pci_map_single(
					priv->pci_dev,
					rxb->skb->data,
					priv->hw_params.rx_buf_size + 256,
					PCI_DMA_FROMDEVICE);
		/* dma address must be no more than 36 bits */
		BUG_ON(rxb->real_dma_addr & ~DMA_BIT_MASK(36));
		/* and also 256 byte aligned! */
		rxb->aligned_dma_addr = ALIGN(rxb->real_dma_addr, 256);
		skb_reserve(rxb->skb, rxb->aligned_dma_addr - rxb->real_dma_addr);

		list_add_tail(&rxb->list, &rxq->rx_free);
		rxq->free_count++;
	}
	spin_unlock_irqrestore(&rxq->lock, flags);
}
EXPORT_SYMBOL(iwl_rx_allocate);

void iwl_rx_replenish(struct iwl_priv *priv)
{
	unsigned long flags;

	iwl_rx_allocate(priv);

	spin_lock_irqsave(&priv->lock, flags);
	iwl_rx_queue_restock(priv);
	spin_unlock_irqrestore(&priv->lock, flags);
}
EXPORT_SYMBOL(iwl_rx_replenish);


/* Assumes that the skb field of the buffers in 'pool' is kept accurate.
 * If an SKB has been detached, the POOL needs to have its SKB set to NULL
 * This free routine walks the list of POOL entries and if SKB is set to
 * non NULL it is unmapped and freed
 */
void iwl_rx_queue_free(struct iwl_priv *priv, struct iwl_rx_queue *rxq)
{
	int i;
	for (i = 0; i < RX_QUEUE_SIZE + RX_FREE_BUFFERS; i++) {
		if (rxq->pool[i].skb != NULL) {
			pci_unmap_single(priv->pci_dev,
					 rxq->pool[i].real_dma_addr,
					 priv->hw_params.rx_buf_size + 256,
					 PCI_DMA_FROMDEVICE);
			dev_kfree_skb(rxq->pool[i].skb);
		}
	}

	pci_free_consistent(priv->pci_dev, 4 * RX_QUEUE_SIZE, rxq->bd,
			    rxq->dma_addr);
	rxq->bd = NULL;
}
EXPORT_SYMBOL(iwl_rx_queue_free);

int iwl_rx_queue_alloc(struct iwl_priv *priv)
{
	struct iwl_rx_queue *rxq = &priv->rxq;
	struct pci_dev *dev = priv->pci_dev;
	int i;

	spin_lock_init(&rxq->lock);
	INIT_LIST_HEAD(&rxq->rx_free);
	INIT_LIST_HEAD(&rxq->rx_used);

	/* Alloc the circular buffer of Read Buffer Descriptors (RBDs) */
	rxq->bd = pci_alloc_consistent(dev, 4 * RX_QUEUE_SIZE, &rxq->dma_addr);
	if (!rxq->bd)
		return -ENOMEM;

	/* Fill the rx_used queue with _all_ of the Rx buffers */
	for (i = 0; i < RX_FREE_BUFFERS + RX_QUEUE_SIZE; i++)
		list_add_tail(&rxq->pool[i].list, &rxq->rx_used);

	/* Set us so that we have processed and used all buffers, but have
	 * not restocked the Rx queue with fresh buffers */
	rxq->read = rxq->write = 0;
	rxq->free_count = 0;
	rxq->need_update = 0;
	return 0;
}
EXPORT_SYMBOL(iwl_rx_queue_alloc);

void iwl_rx_queue_reset(struct iwl_priv *priv, struct iwl_rx_queue *rxq)
{
	unsigned long flags;
	int i;
	spin_lock_irqsave(&rxq->lock, flags);
	INIT_LIST_HEAD(&rxq->rx_free);
	INIT_LIST_HEAD(&rxq->rx_used);
	/* Fill the rx_used queue with _all_ of the Rx buffers */
	for (i = 0; i < RX_FREE_BUFFERS + RX_QUEUE_SIZE; i++) {
		/* In the reset function, these buffers may have been allocated
		 * to an SKB, so we need to unmap and free potential storage */
		if (rxq->pool[i].skb != NULL) {
			pci_unmap_single(priv->pci_dev,
					 rxq->pool[i].real_dma_addr,
					 priv->hw_params.rx_buf_size + 256,
					 PCI_DMA_FROMDEVICE);
			priv->alloc_rxb_skb--;
			dev_kfree_skb(rxq->pool[i].skb);
			rxq->pool[i].skb = NULL;
		}
		list_add_tail(&rxq->pool[i].list, &rxq->rx_used);
	}

	/* Set us so that we have processed and used all buffers, but have
	 * not restocked the Rx queue with fresh buffers */
	rxq->read = rxq->write = 0;
	rxq->free_count = 0;
	spin_unlock_irqrestore(&rxq->lock, flags);
}
EXPORT_SYMBOL(iwl_rx_queue_reset);

int iwl_rx_init(struct iwl_priv *priv, struct iwl_rx_queue *rxq)
{
	int ret;
	unsigned long flags;
	unsigned int rb_size;

	spin_lock_irqsave(&priv->lock, flags);
	ret = iwl_grab_nic_access(priv);
	if (ret) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return ret;
	}

	if (priv->cfg->mod_params->amsdu_size_8K)
		rb_size = FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_8K;
	else
		rb_size = FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K;

	/* Stop Rx DMA */
	iwl_write_direct32(priv, FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);

	/* Reset driver's Rx queue write index */
	iwl_write_direct32(priv, FH_RSCSR_CHNL0_RBDCB_WPTR_REG, 0);

	/* Tell device where to find RBD circular buffer in DRAM */
	iwl_write_direct32(priv, FH_RSCSR_CHNL0_RBDCB_BASE_REG,
			   rxq->dma_addr >> 8);

	/* Tell device where in DRAM to update its Rx status */
	iwl_write_direct32(priv, FH_RSCSR_CHNL0_STTS_WPTR_REG,
			   (priv->shared_phys + priv->rb_closed_offset) >> 4);

	/* Enable Rx DMA, enable host interrupt, Rx buffer size 4k, 256 RBDs */
	iwl_write_direct32(priv, FH_MEM_RCSR_CHNL0_CONFIG_REG,
			   FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL |
			   FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL |
			   rb_size |
			     /* 0x10 << 4 | */
			   (RX_QUEUE_SIZE_LOG <<
			      FH_RCSR_RX_CONFIG_RBDCB_SIZE_BITSHIFT));

	/*
	 * iwl_write32(priv,CSR_INT_COAL_REG,0);
	 */

	iwl_release_nic_access(priv);
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

int iwl_rxq_stop(struct iwl_priv *priv)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	ret = iwl_grab_nic_access(priv);
	if (unlikely(ret)) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return ret;
	}

	/* stop Rx DMA */
	iwl_write_direct32(priv, FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
	ret = iwl_poll_direct_bit(priv, FH_MEM_RSSR_RX_STATUS_REG,
				     (1 << 24), 1000);
	if (ret < 0)
		IWL_ERROR("Can't stop Rx DMA.\n");

	iwl_release_nic_access(priv);
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}
EXPORT_SYMBOL(iwl_rxq_stop);

void iwl_rx_missed_beacon_notif(struct iwl_priv *priv,
				struct iwl_rx_mem_buffer *rxb)

{
	struct iwl_rx_packet *pkt = (struct iwl_rx_packet *)rxb->skb->data;
	struct iwl4965_missed_beacon_notif *missed_beacon;

	missed_beacon = &pkt->u.missed_beacon;
	if (le32_to_cpu(missed_beacon->consequtive_missed_beacons) > 5) {
		IWL_DEBUG_CALIB("missed bcn cnsq %d totl %d rcd %d expctd %d\n",
		    le32_to_cpu(missed_beacon->consequtive_missed_beacons),
		    le32_to_cpu(missed_beacon->total_missed_becons),
		    le32_to_cpu(missed_beacon->num_recvd_beacons),
		    le32_to_cpu(missed_beacon->num_expected_beacons));
		if (!test_bit(STATUS_SCANNING, &priv->status))
			iwl_init_sensitivity(priv);
	}
}
EXPORT_SYMBOL(iwl_rx_missed_beacon_notif);

int iwl_rx_agg_start(struct iwl_priv *priv, const u8 *addr, int tid, u16 ssn)
{
	unsigned long flags;
	int sta_id;

	sta_id = iwl_find_station(priv, addr);
	if (sta_id == IWL_INVALID_STATION)
		return -ENXIO;

	spin_lock_irqsave(&priv->sta_lock, flags);
	priv->stations[sta_id].sta.station_flags_msk = 0;
	priv->stations[sta_id].sta.sta.modify_mask = STA_MODIFY_ADDBA_TID_MSK;
	priv->stations[sta_id].sta.add_immediate_ba_tid = (u8)tid;
	priv->stations[sta_id].sta.add_immediate_ba_ssn = cpu_to_le16(ssn);
	priv->stations[sta_id].sta.mode = STA_CONTROL_MODIFY_MSK;
	spin_unlock_irqrestore(&priv->sta_lock, flags);

	return iwl_send_add_sta(priv, &priv->stations[sta_id].sta,
					CMD_ASYNC);
}
EXPORT_SYMBOL(iwl_rx_agg_start);

int iwl_rx_agg_stop(struct iwl_priv *priv, const u8 *addr, int tid)
{
	unsigned long flags;
	int sta_id;

	sta_id = iwl_find_station(priv, addr);
	if (sta_id == IWL_INVALID_STATION)
		return -ENXIO;

	spin_lock_irqsave(&priv->sta_lock, flags);
	priv->stations[sta_id].sta.station_flags_msk = 0;
	priv->stations[sta_id].sta.sta.modify_mask = STA_MODIFY_DELBA_TID_MSK;
	priv->stations[sta_id].sta.remove_immediate_ba_tid = (u8)tid;
	priv->stations[sta_id].sta.mode = STA_CONTROL_MODIFY_MSK;
	spin_unlock_irqrestore(&priv->sta_lock, flags);

	return iwl_send_add_sta(priv, &priv->stations[sta_id].sta,
					CMD_ASYNC);
}
EXPORT_SYMBOL(iwl_rx_agg_stop);


/* Calculate noise level, based on measurements during network silence just
 *   before arriving beacon.  This measurement can be done only if we know
 *   exactly when to expect beacons, therefore only when we're associated. */
static void iwl_rx_calc_noise(struct iwl_priv *priv)
{
	struct statistics_rx_non_phy *rx_info
				= &(priv->statistics.rx.general);
	int num_active_rx = 0;
	int total_silence = 0;
	int bcn_silence_a =
		le32_to_cpu(rx_info->beacon_silence_rssi_a) & IN_BAND_FILTER;
	int bcn_silence_b =
		le32_to_cpu(rx_info->beacon_silence_rssi_b) & IN_BAND_FILTER;
	int bcn_silence_c =
		le32_to_cpu(rx_info->beacon_silence_rssi_c) & IN_BAND_FILTER;

	if (bcn_silence_a) {
		total_silence += bcn_silence_a;
		num_active_rx++;
	}
	if (bcn_silence_b) {
		total_silence += bcn_silence_b;
		num_active_rx++;
	}
	if (bcn_silence_c) {
		total_silence += bcn_silence_c;
		num_active_rx++;
	}

	/* Average among active antennas */
	if (num_active_rx)
		priv->last_rx_noise = (total_silence / num_active_rx) - 107;
	else
		priv->last_rx_noise = IWL_NOISE_MEAS_NOT_AVAILABLE;

	IWL_DEBUG_CALIB("inband silence a %u, b %u, c %u, dBm %d\n",
			bcn_silence_a, bcn_silence_b, bcn_silence_c,
			priv->last_rx_noise);
}

#define REG_RECALIB_PERIOD (60)

void iwl_rx_statistics(struct iwl_priv *priv,
			      struct iwl_rx_mem_buffer *rxb)
{
	int change;
	struct iwl_rx_packet *pkt = (struct iwl_rx_packet *)rxb->skb->data;

	IWL_DEBUG_RX("Statistics notification received (%d vs %d).\n",
		     (int)sizeof(priv->statistics), pkt->len);

	change = ((priv->statistics.general.temperature !=
		   pkt->u.stats.general.temperature) ||
		  ((priv->statistics.flag &
		    STATISTICS_REPLY_FLG_FAT_MODE_MSK) !=
		   (pkt->u.stats.flag & STATISTICS_REPLY_FLG_FAT_MODE_MSK)));

	memcpy(&priv->statistics, &pkt->u.stats, sizeof(priv->statistics));

	set_bit(STATUS_STATISTICS, &priv->status);

	/* Reschedule the statistics timer to occur in
	 * REG_RECALIB_PERIOD seconds to ensure we get a
	 * thermal update even if the uCode doesn't give
	 * us one */
	mod_timer(&priv->statistics_periodic, jiffies +
		  msecs_to_jiffies(REG_RECALIB_PERIOD * 1000));

	if (unlikely(!test_bit(STATUS_SCANNING, &priv->status)) &&
	    (pkt->hdr.cmd == STATISTICS_NOTIFICATION)) {
		iwl_rx_calc_noise(priv);
		queue_work(priv->workqueue, &priv->run_time_calib_work);
	}

	iwl_leds_background(priv);

	if (priv->cfg->ops->lib->temperature && change)
		priv->cfg->ops->lib->temperature(priv);
}
EXPORT_SYMBOL(iwl_rx_statistics);

#define PERFECT_RSSI (-20) /* dBm */
#define WORST_RSSI (-95)   /* dBm */
#define RSSI_RANGE (PERFECT_RSSI - WORST_RSSI)

/* Calculate an indication of rx signal quality (a percentage, not dBm!).
 * See http://www.ces.clemson.edu/linux/signal_quality.shtml for info
 *   about formulas used below. */
static int iwl_calc_sig_qual(int rssi_dbm, int noise_dbm)
{
	int sig_qual;
	int degradation = PERFECT_RSSI - rssi_dbm;

	/* If we get a noise measurement, use signal-to-noise ratio (SNR)
	 * as indicator; formula is (signal dbm - noise dbm).
	 * SNR at or above 40 is a great signal (100%).
	 * Below that, scale to fit SNR of 0 - 40 dB within 0 - 100% indicator.
	 * Weakest usable signal is usually 10 - 15 dB SNR. */
	if (noise_dbm) {
		if (rssi_dbm - noise_dbm >= 40)
			return 100;
		else if (rssi_dbm < noise_dbm)
			return 0;
		sig_qual = ((rssi_dbm - noise_dbm) * 5) / 2;

	/* Else use just the signal level.
	 * This formula is a least squares fit of data points collected and
	 *   compared with a reference system that had a percentage (%) display
	 *   for signal quality. */
	} else
		sig_qual = (100 * (RSSI_RANGE * RSSI_RANGE) - degradation *
			    (15 * RSSI_RANGE + 62 * degradation)) /
			   (RSSI_RANGE * RSSI_RANGE);

	if (sig_qual > 100)
		sig_qual = 100;
	else if (sig_qual < 1)
		sig_qual = 0;

	return sig_qual;
}

#ifdef CONFIG_IWLWIFI_DEBUG

/**
 * iwl_dbg_report_frame - dump frame to syslog during debug sessions
 *
 * You may hack this function to show different aspects of received frames,
 * including selective frame dumps.
 * group100 parameter selects whether to show 1 out of 100 good frames.
 *
 * TODO:  This was originally written for 3945, need to audit for
 *        proper operation with 4965.
 */
static void iwl_dbg_report_frame(struct iwl_priv *priv,
		      struct iwl_rx_packet *pkt,
		      struct ieee80211_hdr *header, int group100)
{
	u32 to_us;
	u32 print_summary = 0;
	u32 print_dump = 0;	/* set to 1 to dump all frames' contents */
	u32 hundred = 0;
	u32 dataframe = 0;
	__le16 fc;
	u16 seq_ctl;
	u16 channel;
	u16 phy_flags;
	int rate_sym;
	u16 length;
	u16 status;
	u16 bcn_tmr;
	u32 tsf_low;
	u64 tsf;
	u8 rssi;
	u8 agc;
	u16 sig_avg;
	u16 noise_diff;
	struct iwl4965_rx_frame_stats *rx_stats = IWL_RX_STATS(pkt);
	struct iwl4965_rx_frame_hdr *rx_hdr = IWL_RX_HDR(pkt);
	struct iwl4965_rx_frame_end *rx_end = IWL_RX_END(pkt);
	u8 *data = IWL_RX_DATA(pkt);

	if (likely(!(priv->debug_level & IWL_DL_RX)))
		return;

	/* MAC header */
	fc = header->frame_control;
	seq_ctl = le16_to_cpu(header->seq_ctrl);

	/* metadata */
	channel = le16_to_cpu(rx_hdr->channel);
	phy_flags = le16_to_cpu(rx_hdr->phy_flags);
	rate_sym = rx_hdr->rate;
	length = le16_to_cpu(rx_hdr->len);

	/* end-of-frame status and timestamp */
	status = le32_to_cpu(rx_end->status);
	bcn_tmr = le32_to_cpu(rx_end->beacon_timestamp);
	tsf_low = le64_to_cpu(rx_end->timestamp) & 0x0ffffffff;
	tsf = le64_to_cpu(rx_end->timestamp);

	/* signal statistics */
	rssi = rx_stats->rssi;
	agc = rx_stats->agc;
	sig_avg = le16_to_cpu(rx_stats->sig_avg);
	noise_diff = le16_to_cpu(rx_stats->noise_diff);

	to_us = !compare_ether_addr(header->addr1, priv->mac_addr);

	/* if data frame is to us and all is good,
	 *   (optionally) print summary for only 1 out of every 100 */
	if (to_us && (fc & ~cpu_to_le16(IEEE80211_FCTL_PROTECTED)) ==
	    cpu_to_le16(IEEE80211_FCTL_FROMDS | IEEE80211_FTYPE_DATA)) {
		dataframe = 1;
		if (!group100)
			print_summary = 1;	/* print each frame */
		else if (priv->framecnt_to_us < 100) {
			priv->framecnt_to_us++;
			print_summary = 0;
		} else {
			priv->framecnt_to_us = 0;
			print_summary = 1;
			hundred = 1;
		}
	} else {
		/* print summary for all other frames */
		print_summary = 1;
	}

	if (print_summary) {
		char *title;
		int rate_idx;
		u32 bitrate;

		if (hundred)
			title = "100Frames";
		else if (ieee80211_has_retry(fc))
			title = "Retry";
		else if (ieee80211_is_assoc_resp(fc))
			title = "AscRsp";
		else if (ieee80211_is_reassoc_resp(fc))
			title = "RasRsp";
		else if (ieee80211_is_probe_resp(fc)) {
			title = "PrbRsp";
			print_dump = 1;	/* dump frame contents */
		} else if (ieee80211_is_beacon(fc)) {
			title = "Beacon";
			print_dump = 1;	/* dump frame contents */
		} else if (ieee80211_is_atim(fc))
			title = "ATIM";
		else if (ieee80211_is_auth(fc))
			title = "Auth";
		else if (ieee80211_is_deauth(fc))
			title = "DeAuth";
		else if (ieee80211_is_disassoc(fc))
			title = "DisAssoc";
		else
			title = "Frame";

		rate_idx = iwl_hwrate_to_plcp_idx(rate_sym);
		if (unlikely(rate_idx == -1))
			bitrate = 0;
		else
			bitrate = iwl_rates[rate_idx].ieee / 2;

		/* print frame summary.
		 * MAC addresses show just the last byte (for brevity),
		 *    but you can hack it to show more, if you'd like to. */
		if (dataframe)
			IWL_DEBUG_RX("%s: mhd=0x%04x, dst=0x%02x, "
				     "len=%u, rssi=%d, chnl=%d, rate=%u, \n",
				     title, le16_to_cpu(fc), header->addr1[5],
				     length, rssi, channel, bitrate);
		else {
			/* src/dst addresses assume managed mode */
			IWL_DEBUG_RX("%s: 0x%04x, dst=0x%02x, "
				     "src=0x%02x, rssi=%u, tim=%lu usec, "
				     "phy=0x%02x, chnl=%d\n",
				     title, le16_to_cpu(fc), header->addr1[5],
				     header->addr3[5], rssi,
				     tsf_low - priv->scan_start_tsf,
				     phy_flags, channel);
		}
	}
	if (print_dump)
		iwl_print_hex_dump(priv, IWL_DL_RX, data, length);
}
#else
static inline void iwl_dbg_report_frame(struct iwl_priv *priv,
					    struct iwl_rx_packet *pkt,
					    struct ieee80211_hdr *header,
					    int group100)
{
}
#endif

static void iwl_add_radiotap(struct iwl_priv *priv,
				 struct sk_buff *skb,
				 struct iwl_rx_phy_res *rx_start,
				 struct ieee80211_rx_status *stats,
				 u32 ampdu_status)
{
	s8 signal = stats->signal;
	s8 noise = 0;
	int rate = stats->rate_idx;
	u64 tsf = stats->mactime;
	__le16 antenna;
	__le16 phy_flags_hw = rx_start->phy_flags;
	struct iwl4965_rt_rx_hdr {
		struct ieee80211_radiotap_header rt_hdr;
		__le64 rt_tsf;		/* TSF */
		u8 rt_flags;		/* radiotap packet flags */
		u8 rt_rate;		/* rate in 500kb/s */
		__le16 rt_channelMHz;	/* channel in MHz */
		__le16 rt_chbitmask;	/* channel bitfield */
		s8 rt_dbmsignal;	/* signal in dBm, kluged to signed */
		s8 rt_dbmnoise;
		u8 rt_antenna;		/* antenna number */
	} __attribute__ ((packed)) *iwl4965_rt;

	/* TODO: We won't have enough headroom for HT frames. Fix it later. */
	if (skb_headroom(skb) < sizeof(*iwl4965_rt)) {
		if (net_ratelimit())
			printk(KERN_ERR "not enough headroom [%d] for "
			       "radiotap head [%zd]\n",
			       skb_headroom(skb), sizeof(*iwl4965_rt));
		return;
	}

	/* put radiotap header in front of 802.11 header and data */
	iwl4965_rt = (void *)skb_push(skb, sizeof(*iwl4965_rt));

	/* initialise radiotap header */
	iwl4965_rt->rt_hdr.it_version = PKTHDR_RADIOTAP_VERSION;
	iwl4965_rt->rt_hdr.it_pad = 0;

	/* total header + data */
	put_unaligned_le16(sizeof(*iwl4965_rt), &iwl4965_rt->rt_hdr.it_len);

	/* Indicate all the fields we add to the radiotap header */
	put_unaligned_le32((1 << IEEE80211_RADIOTAP_TSFT) |
			   (1 << IEEE80211_RADIOTAP_FLAGS) |
			   (1 << IEEE80211_RADIOTAP_RATE) |
			   (1 << IEEE80211_RADIOTAP_CHANNEL) |
			   (1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL) |
			   (1 << IEEE80211_RADIOTAP_DBM_ANTNOISE) |
			   (1 << IEEE80211_RADIOTAP_ANTENNA),
			   &(iwl4965_rt->rt_hdr.it_present));

	/* Zero the flags, we'll add to them as we go */
	iwl4965_rt->rt_flags = 0;

	put_unaligned_le64(tsf, &iwl4965_rt->rt_tsf);

	iwl4965_rt->rt_dbmsignal = signal;
	iwl4965_rt->rt_dbmnoise = noise;

	/* Convert the channel frequency and set the flags */
	put_unaligned(cpu_to_le16(stats->freq), &iwl4965_rt->rt_channelMHz);
	if (!(phy_flags_hw & RX_RES_PHY_FLAGS_BAND_24_MSK))
		put_unaligned_le16(IEEE80211_CHAN_OFDM | IEEE80211_CHAN_5GHZ,
				   &iwl4965_rt->rt_chbitmask);
	else if (phy_flags_hw & RX_RES_PHY_FLAGS_MOD_CCK_MSK)
		put_unaligned_le16(IEEE80211_CHAN_CCK | IEEE80211_CHAN_2GHZ,
				   &iwl4965_rt->rt_chbitmask);
	else	/* 802.11g */
		put_unaligned_le16(IEEE80211_CHAN_OFDM | IEEE80211_CHAN_2GHZ,
				   &iwl4965_rt->rt_chbitmask);

	if (rate == -1)
		iwl4965_rt->rt_rate = 0;
	else
		iwl4965_rt->rt_rate = iwl_rates[rate].ieee;

	/*
	 * "antenna number"
	 *
	 * It seems that the antenna field in the phy flags value
	 * is actually a bitfield. This is undefined by radiotap,
	 * it wants an actual antenna number but I always get "7"
	 * for most legacy frames I receive indicating that the
	 * same frame was received on all three RX chains.
	 *
	 * I think this field should be removed in favour of a
	 * new 802.11n radiotap field "RX chains" that is defined
	 * as a bitmask.
	 */
	antenna = phy_flags_hw & RX_RES_PHY_FLAGS_ANTENNA_MSK;
	iwl4965_rt->rt_antenna = le16_to_cpu(antenna) >> 4;

	/* set the preamble flag if appropriate */
	if (phy_flags_hw & RX_RES_PHY_FLAGS_SHORT_PREAMBLE_MSK)
		iwl4965_rt->rt_flags |= IEEE80211_RADIOTAP_F_SHORTPRE;

	stats->flag |= RX_FLAG_RADIOTAP;
}

static void iwl_update_rx_stats(struct iwl_priv *priv, u16 fc, u16 len)
{
	/* 0 - mgmt, 1 - cnt, 2 - data */
	int idx = (fc & IEEE80211_FCTL_FTYPE) >> 2;
	priv->rx_stats[idx].cnt++;
	priv->rx_stats[idx].bytes += len;
}

/*
 * returns non-zero if packet should be dropped
 */
static int iwl_set_decrypted_flag(struct iwl_priv *priv,
				      struct ieee80211_hdr *hdr,
				      u32 decrypt_res,
				      struct ieee80211_rx_status *stats)
{
	u16 fc = le16_to_cpu(hdr->frame_control);

	if (priv->active_rxon.filter_flags & RXON_FILTER_DIS_DECRYPT_MSK)
		return 0;

	if (!(fc & IEEE80211_FCTL_PROTECTED))
		return 0;

	IWL_DEBUG_RX("decrypt_res:0x%x\n", decrypt_res);
	switch (decrypt_res & RX_RES_STATUS_SEC_TYPE_MSK) {
	case RX_RES_STATUS_SEC_TYPE_TKIP:
		/* The uCode has got a bad phase 1 Key, pushes the packet.
		 * Decryption will be done in SW. */
		if ((decrypt_res & RX_RES_STATUS_DECRYPT_TYPE_MSK) ==
		    RX_RES_STATUS_BAD_KEY_TTAK)
			break;

	case RX_RES_STATUS_SEC_TYPE_WEP:
		if ((decrypt_res & RX_RES_STATUS_DECRYPT_TYPE_MSK) ==
		    RX_RES_STATUS_BAD_ICV_MIC) {
			/* bad ICV, the packet is destroyed since the
			 * decryption is inplace, drop it */
			IWL_DEBUG_RX("Packet destroyed\n");
			return -1;
		}
	case RX_RES_STATUS_SEC_TYPE_CCMP:
		if ((decrypt_res & RX_RES_STATUS_DECRYPT_TYPE_MSK) ==
		    RX_RES_STATUS_DECRYPT_OK) {
			IWL_DEBUG_RX("hw decrypt successfully!!!\n");
			stats->flag |= RX_FLAG_DECRYPTED;
		}
		break;

	default:
		break;
	}
	return 0;
}

static u32 iwl_translate_rx_status(struct iwl_priv *priv, u32 decrypt_in)
{
	u32 decrypt_out = 0;

	if ((decrypt_in & RX_RES_STATUS_STATION_FOUND) ==
					RX_RES_STATUS_STATION_FOUND)
		decrypt_out |= (RX_RES_STATUS_STATION_FOUND |
				RX_RES_STATUS_NO_STATION_INFO_MISMATCH);

	decrypt_out |= (decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK);

	/* packet was not encrypted */
	if ((decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK) ==
					RX_RES_STATUS_SEC_TYPE_NONE)
		return decrypt_out;

	/* packet was encrypted with unknown alg */
	if ((decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK) ==
					RX_RES_STATUS_SEC_TYPE_ERR)
		return decrypt_out;

	/* decryption was not done in HW */
	if ((decrypt_in & RX_MPDU_RES_STATUS_DEC_DONE_MSK) !=
					RX_MPDU_RES_STATUS_DEC_DONE_MSK)
		return decrypt_out;

	switch (decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK) {

	case RX_RES_STATUS_SEC_TYPE_CCMP:
		/* alg is CCM: check MIC only */
		if (!(decrypt_in & RX_MPDU_RES_STATUS_MIC_OK))
			/* Bad MIC */
			decrypt_out |= RX_RES_STATUS_BAD_ICV_MIC;
		else
			decrypt_out |= RX_RES_STATUS_DECRYPT_OK;

		break;

	case RX_RES_STATUS_SEC_TYPE_TKIP:
		if (!(decrypt_in & RX_MPDU_RES_STATUS_TTAK_OK)) {
			/* Bad TTAK */
			decrypt_out |= RX_RES_STATUS_BAD_KEY_TTAK;
			break;
		}
		/* fall through if TTAK OK */
	default:
		if (!(decrypt_in & RX_MPDU_RES_STATUS_ICV_OK))
			decrypt_out |= RX_RES_STATUS_BAD_ICV_MIC;
		else
			decrypt_out |= RX_RES_STATUS_DECRYPT_OK;
		break;
	};

	IWL_DEBUG_RX("decrypt_in:0x%x  decrypt_out = 0x%x\n",
					decrypt_in, decrypt_out);

	return decrypt_out;
}

static void iwl_pass_packet_to_mac80211(struct iwl_priv *priv,
				       int include_phy,
				       struct iwl_rx_mem_buffer *rxb,
				       struct ieee80211_rx_status *stats)
{
	struct iwl_rx_packet *pkt = (struct iwl_rx_packet *)rxb->skb->data;
	struct iwl_rx_phy_res *rx_start = (include_phy) ?
	    (struct iwl_rx_phy_res *)&(pkt->u.raw[0]) : NULL;
	struct ieee80211_hdr *hdr;
	u16 len;
	__le32 *rx_end;
	unsigned int skblen;
	u32 ampdu_status;
	u32 ampdu_status_legacy;

	if (!include_phy && priv->last_phy_res[0])
		rx_start = (struct iwl_rx_phy_res *)&priv->last_phy_res[1];

	if (!rx_start) {
		IWL_ERROR("MPDU frame without a PHY data\n");
		return;
	}
	if (include_phy) {
		hdr = (struct ieee80211_hdr *)((u8 *) &rx_start[1] +
					       rx_start->cfg_phy_cnt);

		len = le16_to_cpu(rx_start->byte_count);

		rx_end = (__le32 *)((u8 *) &pkt->u.raw[0] +
				  sizeof(struct iwl_rx_phy_res) +
				  rx_start->cfg_phy_cnt + len);

	} else {
		struct iwl4965_rx_mpdu_res_start *amsdu =
		    (struct iwl4965_rx_mpdu_res_start *)pkt->u.raw;

		hdr = (struct ieee80211_hdr *)(pkt->u.raw +
			       sizeof(struct iwl4965_rx_mpdu_res_start));
		len =  le16_to_cpu(amsdu->byte_count);
		rx_start->byte_count = amsdu->byte_count;
		rx_end = (__le32 *) (((u8 *) hdr) + len);
	}

	ampdu_status = le32_to_cpu(*rx_end);
	skblen = ((u8 *) rx_end - (u8 *) &pkt->u.raw[0]) + sizeof(u32);

	if (!include_phy) {
		/* New status scheme, need to translate */
		ampdu_status_legacy = ampdu_status;
		ampdu_status = iwl_translate_rx_status(priv, ampdu_status);
	}

	/* start from MAC */
	skb_reserve(rxb->skb, (void *)hdr - (void *)pkt);
	skb_put(rxb->skb, len);	/* end where data ends */

	/* We only process data packets if the interface is open */
	if (unlikely(!priv->is_open)) {
		IWL_DEBUG_DROP_LIMIT
		    ("Dropping packet while interface is not open.\n");
		return;
	}

	hdr = (struct ieee80211_hdr *)rxb->skb->data;

	/*  in case of HW accelerated crypto and bad decryption, drop */
	if (!priv->hw_params.sw_crypto &&
	    iwl_set_decrypted_flag(priv, hdr, ampdu_status, stats))
		return;

	if (priv->add_radiotap)
		iwl_add_radiotap(priv, rxb->skb, rx_start, stats, ampdu_status);

	iwl_update_rx_stats(priv, le16_to_cpu(hdr->frame_control), len);
	ieee80211_rx_irqsafe(priv->hw, rxb->skb, stats);
	priv->alloc_rxb_skb--;
	rxb->skb = NULL;
}

/* Calc max signal level (dBm) among 3 possible receivers */
static inline int iwl_calc_rssi(struct iwl_priv *priv,
				struct iwl_rx_phy_res *rx_resp)
{
	return priv->cfg->ops->utils->calc_rssi(priv, rx_resp);
}


static void iwl_sta_modify_ps_wake(struct iwl_priv *priv, int sta_id)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->sta_lock, flags);
	priv->stations[sta_id].sta.station_flags &= ~STA_FLG_PWR_SAVE_MSK;
	priv->stations[sta_id].sta.station_flags_msk = STA_FLG_PWR_SAVE_MSK;
	priv->stations[sta_id].sta.sta.modify_mask = 0;
	priv->stations[sta_id].sta.mode = STA_CONTROL_MODIFY_MSK;
	spin_unlock_irqrestore(&priv->sta_lock, flags);

	iwl_send_add_sta(priv, &priv->stations[sta_id].sta, CMD_ASYNC);
}

static void iwl_update_ps_mode(struct iwl_priv *priv, u16 ps_bit, u8 *addr)
{
	/* FIXME: need locking over ps_status ??? */
	u8 sta_id = iwl_find_station(priv, addr);

	if (sta_id != IWL_INVALID_STATION) {
		u8 sta_awake = priv->stations[sta_id].
				ps_status == STA_PS_STATUS_WAKE;

		if (sta_awake && ps_bit)
			priv->stations[sta_id].ps_status = STA_PS_STATUS_SLEEP;
		else if (!sta_awake && !ps_bit) {
			iwl_sta_modify_ps_wake(priv, sta_id);
			priv->stations[sta_id].ps_status = STA_PS_STATUS_WAKE;
		}
	}
}

/* This is necessary only for a number of statistics, see the caller. */
static int iwl_is_network_packet(struct iwl_priv *priv,
		struct ieee80211_hdr *header)
{
	/* Filter incoming packets to determine if they are targeted toward
	 * this network, discarding packets coming from ourselves */
	switch (priv->iw_mode) {
	case IEEE80211_IF_TYPE_IBSS: /* Header: Dest. | Source    | BSSID */
		/* packets to our IBSS update information */
		return !compare_ether_addr(header->addr3, priv->bssid);
	case IEEE80211_IF_TYPE_STA: /* Header: Dest. | AP{BSSID} | Source */
		/* packets to our IBSS update information */
		return !compare_ether_addr(header->addr2, priv->bssid);
	default:
		return 1;
	}
}

/* Called for REPLY_RX (legacy ABG frames), or
 * REPLY_RX_MPDU_CMD (HT high-throughput N frames). */
void iwl_rx_reply_rx(struct iwl_priv *priv,
				struct iwl_rx_mem_buffer *rxb)
{
	struct ieee80211_hdr *header;
	struct ieee80211_rx_status rx_status;
	struct iwl_rx_packet *pkt = (struct iwl_rx_packet *)rxb->skb->data;
	/* Use phy data (Rx signal strength, etc.) contained within
	 *   this rx packet for legacy frames,
	 *   or phy data cached from REPLY_RX_PHY_CMD for HT frames. */
	int include_phy = (pkt->hdr.cmd == REPLY_RX);
	struct iwl_rx_phy_res *rx_start = (include_phy) ?
		(struct iwl_rx_phy_res *)&(pkt->u.raw[0]) :
		(struct iwl_rx_phy_res *)&priv->last_phy_res[1];
	__le32 *rx_end;
	unsigned int len = 0;
	u16 fc;
	u8 network_packet;

	rx_status.mactime = le64_to_cpu(rx_start->timestamp);
	rx_status.freq =
		ieee80211_channel_to_frequency(le16_to_cpu(rx_start->channel));
	rx_status.band = (rx_start->phy_flags & RX_RES_PHY_FLAGS_BAND_24_MSK) ?
				IEEE80211_BAND_2GHZ : IEEE80211_BAND_5GHZ;
	rx_status.rate_idx =
		iwl_hwrate_to_plcp_idx(le32_to_cpu(rx_start->rate_n_flags));
	if (rx_status.band == IEEE80211_BAND_5GHZ)
		rx_status.rate_idx -= IWL_FIRST_OFDM_RATE;

	rx_status.antenna = 0;
	rx_status.flag = 0;

	/* TSF isn't reliable. In order to allow smooth user experience,
	 * this W/A doesn't propagate it to the mac80211 */
	/*rx_status.flag |= RX_FLAG_TSFT;*/

	if ((unlikely(rx_start->cfg_phy_cnt > 20))) {
		IWL_DEBUG_DROP("dsp size out of range [0,20]: %d/n",
				rx_start->cfg_phy_cnt);
		return;
	}

	if (!include_phy) {
		if (priv->last_phy_res[0])
			rx_start = (struct iwl_rx_phy_res *)
				&priv->last_phy_res[1];
		else
			rx_start = NULL;
	}

	if (!rx_start) {
		IWL_ERROR("MPDU frame without a PHY data\n");
		return;
	}

	if (include_phy) {
		header = (struct ieee80211_hdr *)((u8 *) &rx_start[1]
						  + rx_start->cfg_phy_cnt);

		len = le16_to_cpu(rx_start->byte_count);
		rx_end = (__le32 *)(pkt->u.raw + rx_start->cfg_phy_cnt +
				  sizeof(struct iwl_rx_phy_res) + len);
	} else {
		struct iwl4965_rx_mpdu_res_start *amsdu =
			(struct iwl4965_rx_mpdu_res_start *)pkt->u.raw;

		header = (void *)(pkt->u.raw +
			sizeof(struct iwl4965_rx_mpdu_res_start));
		len = le16_to_cpu(amsdu->byte_count);
		rx_end = (__le32 *) (pkt->u.raw +
			sizeof(struct iwl4965_rx_mpdu_res_start) + len);
	}

	if (!(*rx_end & RX_RES_STATUS_NO_CRC32_ERROR) ||
	    !(*rx_end & RX_RES_STATUS_NO_RXE_OVERFLOW)) {
		IWL_DEBUG_RX("Bad CRC or FIFO: 0x%08X.\n",
				le32_to_cpu(*rx_end));
		return;
	}

	priv->ucode_beacon_time = le32_to_cpu(rx_start->beacon_time_stamp);

	/* Find max signal strength (dBm) among 3 antenna/receiver chains */
	rx_status.signal = iwl_calc_rssi(priv, rx_start);

	/* Meaningful noise values are available only from beacon statistics,
	 *   which are gathered only when associated, and indicate noise
	 *   only for the associated network channel ...
	 * Ignore these noise values while scanning (other channels) */
	if (iwl_is_associated(priv) &&
	    !test_bit(STATUS_SCANNING, &priv->status)) {
		rx_status.noise = priv->last_rx_noise;
		rx_status.qual = iwl_calc_sig_qual(rx_status.signal,
							 rx_status.noise);
	} else {
		rx_status.noise = IWL_NOISE_MEAS_NOT_AVAILABLE;
		rx_status.qual = iwl_calc_sig_qual(rx_status.signal, 0);
	}

	/* Reset beacon noise level if not associated. */
	if (!iwl_is_associated(priv))
		priv->last_rx_noise = IWL_NOISE_MEAS_NOT_AVAILABLE;

	/* Set "1" to report good data frames in groups of 100 */
	/* FIXME: need to optimze the call: */
	iwl_dbg_report_frame(priv, pkt, header, 1);

	IWL_DEBUG_STATS_LIMIT("Rssi %d, noise %d, qual %d, TSF %llu\n",
		rx_status.signal, rx_status.noise, rx_status.signal,
		(unsigned long long)rx_status.mactime);

	/* Take shortcut when only in monitor mode */
	if (priv->iw_mode == IEEE80211_IF_TYPE_MNTR) {
		iwl_pass_packet_to_mac80211(priv, include_phy,
						 rxb, &rx_status);
		return;
	}

	network_packet = iwl_is_network_packet(priv, header);
	if (network_packet) {
		priv->last_rx_rssi = rx_status.signal;
		priv->last_beacon_time =  priv->ucode_beacon_time;
		priv->last_tsf = le64_to_cpu(rx_start->timestamp);
	}

	fc = le16_to_cpu(header->frame_control);
	switch (fc & IEEE80211_FCTL_FTYPE) {
	case IEEE80211_FTYPE_MGMT:
	case IEEE80211_FTYPE_DATA:
		if (priv->iw_mode == IEEE80211_IF_TYPE_AP)
			iwl_update_ps_mode(priv, fc  & IEEE80211_FCTL_PM,
						header->addr2);
		/* fall through */
	default:
			iwl_pass_packet_to_mac80211(priv, include_phy, rxb,
				   &rx_status);
		break;

	}
}
EXPORT_SYMBOL(iwl_rx_reply_rx);

/* Cache phy data (Rx signal strength, etc) for HT frame (REPLY_RX_PHY_CMD).
 * This will be used later in iwl_rx_reply_rx() for REPLY_RX_MPDU_CMD. */
void iwl_rx_reply_rx_phy(struct iwl_priv *priv,
				    struct iwl_rx_mem_buffer *rxb)
{
	struct iwl_rx_packet *pkt = (struct iwl_rx_packet *)rxb->skb->data;
	priv->last_phy_res[0] = 1;
	memcpy(&priv->last_phy_res[1], &(pkt->u.raw[0]),
	       sizeof(struct iwl_rx_phy_res));
}
EXPORT_SYMBOL(iwl_rx_reply_rx_phy);
