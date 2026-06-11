// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <uapi/linux/idxd.h>
#include "idxd.h"
#include "registers.h"

static struct idxd_desc *__get_desc(struct idxd_wq *wq, int idx, int cpu)
{
	struct idxd_desc *desc;
	struct idxd_device *idxd = wq->idxd;

	desc = wq->descs[idx];
	memset(desc->hw, 0, sizeof(struct dsa_hw_desc));
	memset(desc->completion, 0, idxd->data->compl_size);
	desc->cpu = cpu;

	if (device_pasid_enabled(idxd))
		desc->hw->pasid = idxd->pasid;

	return desc;
}

struct idxd_desc *idxd_alloc_desc(struct idxd_wq *wq, enum idxd_op_type optype)
{
	int cpu, idx;
	struct idxd_device *idxd = wq->idxd;
	DEFINE_SBQ_WAIT(wait);
	struct sbq_wait_state *ws;
	struct sbitmap_queue *sbq;

	if (idxd->state != IDXD_DEV_ENABLED)
		return ERR_PTR(-EIO);

	sbq = &wq->sbq;
	idx = sbitmap_queue_get(sbq, &cpu);
	if (idx < 0) {
		if (optype == IDXD_OP_NONBLOCK)
			return ERR_PTR(-EAGAIN);
	} else {
		return __get_desc(wq, idx, cpu);
	}

	ws = &sbq->ws[0];
	for (;;) {
		sbitmap_prepare_to_wait(sbq, ws, &wait, TASK_INTERRUPTIBLE);
		if (signal_pending_state(TASK_INTERRUPTIBLE, current))
			break;
		idx = sbitmap_queue_get(sbq, &cpu);
		if (idx >= 0)
			break;
		schedule();
	}

	sbitmap_finish_wait(sbq, ws, &wait);
	if (idx < 0)
		return ERR_PTR(-EAGAIN);

	return __get_desc(wq, idx, cpu);
}

struct idxd_desc *idxd_alloc_desc_nosched(struct idxd_wq *wq, enum idxd_op_type optype)
{
	int cpu, idx;
	struct idxd_device *idxd = wq->idxd;
	DEFINE_SBQ_WAIT(wait);
	struct sbq_wait_state *ws;
	struct sbitmap_queue *sbq;

	if (idxd->state != IDXD_DEV_ENABLED)	// if device not enabled -> EIO
		return ERR_PTR(-EIO);

	sbq = &wq->sbq;
	idx = sbitmap_queue_get(sbq, &cpu);		// bitmap based lf-queue? get 1bit? ret: 성공(idx) 실패(-1)
	if (idx < 0) {							// bitmap get 실패시XXXXXXXXXXXXXXXXXXXX
		if (optype == IDXD_OP_NONBLOCK)		// NONBLOCK 넘어왔으면 재시도하라고 EAGAIN 리턴 
			return ERR_PTR(-EAGAIN);
	} else {
		return __get_desc(wq, idx, cpu);
	}

	ws = &sbq->ws[0];
	for (;;) {	/* polling */
		// pr_info("%s for loop! \n", __func__);	// IDXD_OP_BLOCK으로 하면 나옴.
		idx = sbitmap_queue_get(sbq, &cpu);
		if (idx >= 0)
			break;
		cpu_relax();
	}

	if (idx < 0)
		return ERR_PTR(-EAGAIN);

	return __get_desc(wq, idx, cpu);
}

void idxd_free_desc(struct idxd_wq *wq, struct idxd_desc *desc)
{
	int cpu = desc->cpu;

	desc->cpu = -1;
	sbitmap_queue_clear(&wq->sbq, desc->id, cpu);
}

static struct idxd_desc *list_abort_desc(struct idxd_wq *wq, struct idxd_irq_entry *ie,
					 struct idxd_desc *desc)
{
	struct idxd_desc *d, *n;

	lockdep_assert_held(&ie->list_lock);
	list_for_each_entry_safe(d, n, &ie->work_list, list) {
		if (d == desc) {
			list_del(&d->list);
			return d;
		}
	}

	/*
	 * At this point, the desc needs to be aborted is held by the completion
	 * handler where it has taken it off the pending list but has not added to the
	 * work list. It will be cleaned up by the interrupt handler when it sees the
	 * IDXD_COMP_DESC_ABORT for completion status.
	 */
	return NULL;
}

static void llist_abort_desc(struct idxd_wq *wq, struct idxd_irq_entry *ie,
			     struct idxd_desc *desc)
{
	struct idxd_desc *d, *t, *found = NULL;
	struct llist_node *head;
	LIST_HEAD(flist);

	desc->completion->status = IDXD_COMP_DESC_ABORT;
	/*
	 * Grab the list lock so it will block the irq thread handler. This allows the
	 * abort code to locate the descriptor need to be aborted.
	 */
	spin_lock(&ie->list_lock);
	head = llist_del_all(&ie->pending_llist);
	if (head) {
		llist_for_each_entry_safe(d, t, head, llnode) {
			if (d == desc) {
				found = desc;
				continue;
			}

			if (d->completion->status)
				list_add_tail(&d->list, &flist);
			else
				list_add_tail(&d->list, &ie->work_list);
		}
	}

	if (!found)
		found = list_abort_desc(wq, ie, desc);
	spin_unlock(&ie->list_lock);

	if (found)
		idxd_dma_complete_txd(found, IDXD_COMPLETE_ABORT, false);

	/*
	 * completing the descriptor will return desc to allocator and
	 * the desc can be acquired by a different process and the
	 * desc->list can be modified.  Delete desc from list so the
	 * list trasversing does not get corrupted by the other process.
	 */
	list_for_each_entry_safe(d, t, &flist, list) {
		list_del_init(&d->list);
		idxd_dma_complete_txd(found, IDXD_COMPLETE_ABORT, true);
	}
}

/*
 * ENQCMDS typically fail when the WQ is inactive or busy. On host submission, the driver
 * has better control of number of descriptors being submitted to a shared wq by limiting
 * the number of driver allocated descriptors to the wq size. However, when the swq is
 * exported to a guest kernel, it may be shared with multiple guest kernels. This means
 * the likelihood of getting busy returned on the swq when submitting goes significantly up.
 * Having a tunable retry mechanism allows the driver to keep trying for a bit before giving
 * up. The sysfs knob can be tuned by the system administrator.
 */
int idxd_enqcmds(struct idxd_wq *wq, void __iomem *portal, const void *desc)
{
	unsigned int retries = wq->enqcmds_retries;
	int rc;

	do {
		rc = enqcmds(portal, desc);
		if (rc == 0)
			break;
		cpu_relax();
	} while (retries--);

	return rc;
}
//EXPORT_SYMBOL(idxd_enqcmds);	// Sangjin Luma


static void print_dsa_hw_desc(const struct dsa_hw_desc *desc)	// Sangjin Luma
{
	/* Top-level fields */
	pr_info("=== dsa_hw_desc ===\n");
	pr_info("pasid             : %u\n", desc->pasid);
	pr_info("rsvd (11 bits)    : %u\n", desc->rsvd);
	pr_info("priv              : %u\n", desc->priv);
	pr_info("flags             : 0x%06x\n", desc->flags);
	pr_info("opcode            : 0x%02x\n", desc->opcode);
	pr_info("completion_addr   : 0x%llx\n",
		(unsigned long long)desc->completion_addr);

	/* First union (src_addr, rdback_addr, pattern, desc_list_addr) */
	pr_info("src_addr          : 0x%llx\n",
		(unsigned long long)desc->src_addr);
	pr_info("rdback_addr       : 0x%llx\n",
		(unsigned long long)desc->rdback_addr);
	pr_info("pattern           : 0x%llx\n",
		(unsigned long long)desc->pattern);
	pr_info("desc_list_addr    : 0x%llx\n",
		(unsigned long long)desc->desc_list_addr);

	/* Second union (dst_addr, rdback_addr2, src2_addr, comp_pattern) */
	pr_info("dst_addr          : 0x%llx\n",
		(unsigned long long)desc->dst_addr);
	pr_info("rdback_addr2      : 0x%llx\n",
		(unsigned long long)desc->rdback_addr2);
	pr_info("src2_addr         : 0x%llx\n",
		(unsigned long long)desc->src2_addr);
	pr_info("comp_pattern      : 0x%llx\n",
		(unsigned long long)desc->comp_pattern);

	/* xfer_size / desc_count union */
	pr_info("xfer_size         : %u\n", desc->xfer_size);
	pr_info("desc_count        : %u\n", desc->desc_count);

	pr_info("int_handle        : %u\n", desc->int_handle);
	pr_info("rsvd1 (16 bits)   : %u\n", desc->rsvd1);

	/* Now for the larger op-specific union. Many fields overlap.
	 * Typically you select which ones to print depending on opcode. */
	pr_info("expected_res      : %u\n", desc->expected_res);

	pr_info("delta_addr        : 0x%llx\n",
		(unsigned long long)desc->delta_addr);
	pr_info("max_delta_size    : %u\n", desc->max_delta_size);
	pr_info("delt_rsvd         : %u\n", desc->delt_rsvd);
	pr_info("expected_res_mask : %u\n", desc->expected_res_mask);

	pr_info("delta_rec_size    : %u\n", desc->delta_rec_size);
	pr_info("dest2             : 0x%llx\n",
		(unsigned long long)desc->dest2);

	/* CRC fields */
	pr_info("crc_seed          : %u\n", desc->crc_seed);
	pr_info("crc_rsvd          : %u\n", desc->crc_rsvd);
	pr_info("seed_addr         : 0x%llx\n",
		(unsigned long long)desc->seed_addr);

	/* DIF check/strip */
	pr_info("src_dif_flags     : 0x%x\n", desc->src_dif_flags);
	pr_info("dif_chk_res       : 0x%x\n", desc->dif_chk_res);
	pr_info("dif_chk_flags     : 0x%x\n", desc->dif_chk_flags);

	/* Note: dif_chk_res2 is 5 bytes. Print them as a small hex dump. */
	pr_info("dif_chk_res2[5]   : %02x %02x %02x %02x %02x\n",
		desc->dif_chk_res2[0], desc->dif_chk_res2[1],
		desc->dif_chk_res2[2], desc->dif_chk_res2[3],
		desc->dif_chk_res2[4]);

	pr_info("chk_ref_tag_seed  : 0x%x\n", desc->chk_ref_tag_seed);
	pr_info("chk_app_tag_mask  : 0x%x\n", desc->chk_app_tag_mask);
	pr_info("chk_app_tag_seed  : 0x%x\n", desc->chk_app_tag_seed);

	/* DIF insert */
	pr_info("dif_ins_res       : 0x%x\n", desc->dif_ins_res);
	pr_info("dest_dif_flag     : 0x%x\n", desc->dest_dif_flag);
	pr_info("dif_ins_flags     : 0x%x\n", desc->dif_ins_flags);

	pr_info("dif_ins_res2[13]  : ");
	/* 13-byte array */
	{
		int i;
		for (i = 0; i < 13; i++)
			pr_cont("%02x ", desc->dif_ins_res2[i]);
		pr_cont("\n");
	}

	pr_info("ins_ref_tag_seed  : 0x%x\n", desc->ins_ref_tag_seed);
	pr_info("ins_app_tag_mask  : 0x%x\n", desc->ins_app_tag_mask);
	pr_info("ins_app_tag_seed  : 0x%x\n", desc->ins_app_tag_seed);

	/* DIF update */
	pr_info("src_upd_flags     : 0x%x\n", desc->src_upd_flags);
	pr_info("upd_dest_flags    : 0x%x\n", desc->upd_dest_flags);
	pr_info("dif_upd_flags     : 0x%x\n", desc->dif_upd_flags);

	pr_info("dif_upd_res[5]    : %02x %02x %02x %02x %02x\n",
		desc->dif_upd_res[0], desc->dif_upd_res[1],
		desc->dif_upd_res[2], desc->dif_upd_res[3],
		desc->dif_upd_res[4]);

	pr_info("src_ref_tag_seed  : 0x%x\n", desc->src_ref_tag_seed);
	pr_info("src_app_tag_mask  : 0x%x\n", desc->src_app_tag_mask);
	pr_info("src_app_tag_seed  : 0x%x\n", desc->src_app_tag_seed);
	pr_info("dest_ref_tag_seed : 0x%x\n", desc->dest_ref_tag_seed);
	pr_info("dest_app_tag_mask : 0x%x\n", desc->dest_app_tag_mask);
	pr_info("dest_app_tag_seed : 0x%x\n", desc->dest_app_tag_seed);

	/* If none of the above fields apply, you might want to
	 * dump the raw op_specific[24] bytes as a fallback: */
	pr_info("op_specific (raw) :");
	{
		const uint8_t *raw = desc->op_specific;
		int i;
		for (i = 0; i < 24; i++)
			pr_cont(" %02x", raw[i]);
		pr_cont("\n");
	}

	pr_info("=== end dsa_hw_desc ===\n");
}

int idxd_submit_desc(struct idxd_wq *wq, struct idxd_desc *desc)
{
	struct idxd_device *idxd = wq->idxd;
	struct idxd_irq_entry *ie = NULL;
	u32 desc_flags = desc->hw->flags;
	void __iomem *portal;
	int rc;

	//print_dsa_hw_desc(desc->hw);	// Sangjin Luma

	if (idxd->state != IDXD_DEV_ENABLED)
		return -EIO;

	if (!percpu_ref_tryget_live(&wq->wq_active)) {
		wait_for_completion(&wq->wq_resurrect);
		if (!percpu_ref_tryget_live(&wq->wq_active))
			return -ENXIO;
	}

	portal = idxd_wq_portal_addr(wq);

	/*
	 * Pending the descriptor to the lockless list for the irq_entry
	 * that we designated the descriptor to.
	 */
	if (desc_flags & IDXD_OP_FLAG_RCI) {
		ie = &wq->ie;
		desc->hw->int_handle = ie->int_handle;
		llist_add(&desc->llnode, &ie->pending_llist);
	}

	/*
	 * The wmb() flushes writes to coherent DMA data before
	 * possibly triggering a DMA read. The wmb() is necessary
	 * even on UP because the recipient is a device.
	 */
	wmb();

	if (wq_dedicated(wq)) {
		iosubmit_cmds512(portal, desc->hw, 1);
	} else {
		rc = idxd_enqcmds(wq, portal, desc->hw);
		if (rc < 0) {
			percpu_ref_put(&wq->wq_active);
			/* abort operation frees the descriptor */
			if (ie)
				llist_abort_desc(wq, ie, desc);
			return rc;
		}
	}

	percpu_ref_put(&wq->wq_active);
	return 0;
}
//EXPORT_SYMBOL(idxd_submit_desc);	// Sangjin Luma
