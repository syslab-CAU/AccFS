#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/async_tx.h>
// #include "nova_def.h"
#include <linux/idxd.h>
#include "internal_idxd.h"
#include "nova_def.h"
#include "nova.h"
#include <linux/bitrev.h>
#include <linux/ktime.h>
#include <linux/math64.h>

#include "offload_rr.h"

#define MAX_CHANNELS 64
static struct dma_chan *nova_dma_chans[MAX_CHANNELS];
static int nova_num_dma_chans = 0;  // The number of acquired dma channels
static atomic_t nova_dma_chan_index = ATOMIC_INIT(0);

// #define pr_info_custom prinfo <- Custom function to toggle printing functionality (while performance evaluation).

/* Filter function for dma_request_channel */
static bool nova_dma_filter(struct dma_chan *chan, void *param)
{
    const char *requested_name = (const char *)param;
    const char *chan_name = dma_chan_name(chan);
    // pr_info("%s(): requested_name: %s", __func__, requested_name);
    // pr_info("%s(): dev_name(chan->device->dev): %s", __func__, dev_name(chan->device->dev)); //0000:6a:01.0
    pr_info("%s(): dma_chan_name(chan): %s", __func__, dma_chan_name(chan)); // dma0chan0

    /* If your DSA driver registers channels with specific names,
     * match them here. Checking dma_chan_name(chan) is common,
     * or check dev_name(chan->device->dev). */
    // if (!strcmp(dev_name(chan->device->dev), requested_name)) {
    if (!strcmp(chan_name, requested_name)) {
        pr_warn("NOVA: Matched DMA channel %s\n", chan_name);
        return true;
    }
    pr_warn("DMA channel %s not matched\n", chan_name);
    return false;
}

static void nova_dma_callback(void *completion)
{
    // pr_info("NOVA: DMA tx completed\n");
    complete(completion);
}

static void nova_dma_memcpy_done_cb(void *param,	// Callback done
                                    const struct dmaengine_result *res)
{
    struct nova_dma_memcpy_done *ctx = param;

    if (res)
        ctx->result = *res;
    else
        ctx->result.result = DMA_TRANS_ABORTED;

    complete(&ctx->done);
}

struct nova_dma_crc32_done {
	struct completion done;
	struct dmaengine_result result;
	struct dsa_completion_record *cr;
	u32 crc32;
	u8 cr_status;
};

static void nova_dma_crc32_done_cb(void *param,
				   const struct dmaengine_result *res)
{
	struct nova_dma_crc32_done *ctx = param;
	u64 crc64;

	if (res)
		ctx->result = *res;
	else
		ctx->result.result = DMA_TRANS_ABORTED;

	if (ctx->cr) {
		dma_rmb();

		ctx->cr_status = READ_ONCE(ctx->cr->status);

		if (ctx->cr_status == DSA_COMP_SUCCESS) {
			crc64 = READ_ONCE(ctx->cr->crc_val);
			ctx->crc32 = (u32)le64_to_cpu((__le64)crc64);
		}
	}

	complete(&ctx->done);
}

/* Init all dmaengines for offloading */
int nova_dmaengine_init_all(void)  // dma_chan_name: dma0chan0
{
    dma_cap_mask_t mask;
    struct dma_chan *nova_dma_chan;
    int i = 0;
    int j = 0;

    pr_info("NOVA: Initializing DMA engine for channel\n");

    /* Initialize DMA capability mask */
    dma_cap_zero(mask);
    dma_cap_set(DMA_MEMCPY, mask);

    /* Repeatedly request channels until we exhaust them or reach MAX_CHANNELS. */
    while (i < MAX_CHANNELS) {
        nova_dma_chan = dma_request_channel(mask, NULL, NULL);
        if (!nova_dma_chan){
            pr_err("NOVA: Failed to request DMA channel\n");    //%s", dma_chan_name(nova_dma_chan)
            break;
        }
        nova_dma_chans[i++] = nova_dma_chan;
    }

    nova_num_dma_chans = i;

    if (nova_num_dma_chans == 0) {
        pr_err("NOVA: No DMA channels available!\n");
        return -ENODEV;
    }

    /* DMA capability check */
    for (j = 0; j < i; j++){  // i == MAX_CHANNELS
        if (nova_dma_chans[j]){
            // if (dma_has_cap(DMA_COMPLETION_NO_ORDER, nova_dma_chans[j]->device->cap_mask) &&
            //     use_polling) {
            //     use_polling= false;
            //     // pr_warn("NOVA: DMA_COMPLETION_NO_ORDER, polling disabled\n");
            // }
            if (!dma_has_cap(DMA_MEMCPY, nova_dma_chans[j]->device->cap_mask)) {
                pr_info("NOVA: dmaengine is NOT capable of DMA_MEMCPY.\n");
            }
        }
    }

    pr_info("NOVA: Acquired %d DMA channels total.\n", nova_num_dma_chans);
    return 0;
}

/* Naive scheduling: simple RR - Just getting next channel from 0 to MAX_CHANNELS in sequence */
// other variants can be added - NUMA node id (Locan-Current/Remote), R/W request type, IO size, ...
// if possible checking SWQ and DWQ might be good approach. 
static inline struct dma_chan *nova_get_next_channel(void)  // *offload_schedule(void)
{
    /* Simple round-robin?:
       1) fetch & increment an atomic
       2) modular by number of DMA channels
       3) return the channel pointer */
    int idx = atomic_fetch_add(1, &nova_dma_chan_index);
    return nova_dma_chans[idx % nova_num_dma_chans];
}

/* Pinning thread to DMA channel */
static inline struct dma_chan *nova_link_channel_by_corenum(void)  // *offload_schedule(void)
{
    // int core_id = smp_processor_id();   // Get current CPU cores
    int core_id = task_cpu(current);
    // pr_info("%s(): core_id: %d\n", __func__, core_id);

    /* Compute engine/channel from core_id */
    int engine_id = (core_id >> 3);  // top bits as engine num (0..7)
    int chan_id   = (core_id & 7);   // low 3 bits as channel num (0..7)

    /* Turn into array index, assume 64 channels total */
    int idx = engine_id * 8 + chan_id;
    return nova_dma_chans[idx];
}

/*
 * _nova_dma_crc32 - Offload a crc32 generation to Intel DSA via DMA engine
 *
 * @crc:   Destination buffer (kernel virtual address)
 * @data:   Source buffer (kernel virtual address)
 * @len:  Number of bytes to copy
 *
 * Returns 0 on success, negative on failure.
 */
// Sangjin Luma crc32 최외곽 API
#define DSA_MIN_BYTES     256      /* 그 이하면 바로 CPU-CRC */

u32 __nova_dma_crc32_phys_poll(u32 crc_seed, phys_addr_t phys, size_t size)
{
    // struct dma_device *dma_dev;
    struct device *dev;
    dma_addr_t dma_src;
    struct dma_async_tx_descriptor *tx = NULL;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;  //  0x02 | 0x01
    int ret = 0;
    int err;
    struct dsa_completion_record *cr;
    struct dma_chan *nova_dma_chan = nova_link_channel_by_corenum();
    bool use_polling;
#ifdef ONLY_USE_POLLING_FOR_CHECKSUM
    use_polling = true;		// polling
#elif defined(ONLY_USE_INTERRUPT_FOR_CHECKSUM)
    use_polling = false;	// interrupt
#elif defined(ADAPTIVE_FOR_CHECKSUM_COMPLETION)
    printk("[%s] check\n", __func__);
#else
    printk(KERN_ERR "[%s] add commit.h !\n", __func__);
#endif
    //bool use_polling = false;		// interrupt
    u32 crc32 = 77777;  // crc32: crc32 calc result

	// pr_info("\t\t%s(): start\n", __func__);

    if (!nova_dma_chan){
        pr_err("%s: No channel available\n", __func__);
        return ~EIO; // no channel. 상위 코드에서 CPU-CRC 로 재시도?
    }

    dev = nova_dma_chan->device->dev;
    dma_src = dma_map_resource(dev, phys, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
    if (dma_mapping_error(dev, dma_src)){
        pr_err("%s: DMA mapping error\n", __func__);
        return ~EIO; // 상위 코드에서 CPU-CRC 로 재시도?
    }

    u32 seed = (uint32_t)crc_seed;

    // tx = dmaengine_prep_dma_crc32(nova_dma_chan, crc_seed, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    tx = dmaengine_prep_dma_crc32(nova_dma_chan, NULL, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    if (!tx) {
        pr_err("%s: dmaengine_prep_dma_crc32 failed\n", __func__);
        dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
        // nova_crc32c(dst, src, size);
        return ~EIO; // 상위 코드에서 CPU-CRC 로 재시도?
    }

if (!use_polling) {
	struct nova_dma_crc32_done done;
	struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);

	cr = desc->completion;

	init_completion(&done.done);
	done.result.result = DMA_TRANS_ABORTED;
	done.result.residue = size;
	done.cr = cr;
	done.crc32 = 0;
	done.cr_status = 0;

	tx->callback_result = nova_dma_crc32_done_cb;
	tx->callback_param = &done;

	err = dma_submit_error(dmaengine_submit(tx));
	if (err) {
		pr_err("%s: dma_submit_error in interrupt path\n", __func__);
		dmaengine_desc_free(tx);
		dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
		return ~EIO;
	}

	dma_async_issue_pending(nova_dma_chan);

	ret = wait_for_completion_timeout(&done.done,
					  msecs_to_jiffies(5000));
	if (ret == 0) {
		pr_err("%s: interrupt completion timed out\n", __func__);
		dmaengine_terminate_sync(nova_dma_chan);
		dmaengine_desc_free(tx);
		dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
		return ~ETIMEDOUT;
	}

	if (done.result.result != DMA_TRANS_NOERROR ||
	    done.cr_status != DSA_COMP_SUCCESS) {
		pr_err("%s: interrupt completion error result=%d residue=%u cr_status=%u\n",
		       __func__, done.result.result, done.result.residue,
		       done.cr_status);
		dmaengine_desc_free(tx);
		dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
		return ~EIO;
	}

	crc32 = done.crc32;

	dmaengine_desc_free(tx);
	dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
			   DMA_ATTR_SKIP_CPU_SYNC);

	return crc32;
}
    err = dma_submit_error(dmaengine_submit(tx));
    if (err) {
        pr_err("%s: dma_submit_error\n", __func__);
        dmaengine_desc_free(tx);
        dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
        return ~EIO;
    }

    dma_async_issue_pending(nova_dma_chan); // do nothing
    struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);   // get idxd_desc struct
    cr = desc->completion;

    /* polling */
    while (!READ_ONCE(cr->status)) {
        cpu_relax();
    }

    crc32 = le32_to_cpu(cr->crc_val);
    // pr_info("\t\t%s() cr->crc_val: %d\n", __func__, cr->crc_val);
    // pr_info("\t\t%s() crc32: %d\n", __func__, crc32);
    // pr_info("%s() ############### crc32: %u\n", __func__, crc32);

    dmaengine_desc_free(tx);
    dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);

    return crc32;
}

u32 __nova_dma_crc32_poll(u32 crc_seed, const u8 *src, size_t size)
{
    struct dma_device *dma_dev;
    dma_addr_t dma_src;
    struct dma_async_tx_descriptor *tx = NULL;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    dma_cookie_t cookie;
    enum dma_status status;
    struct dmaengine_unmap_data *um;
    struct completion dma_completion;
    int ret = 0;
    struct dsa_completion_record *cr;
    struct dma_chan *nova_dma_chan;
    bool use_polling;
#ifdef ONLY_USE_POLLING_FOR_CHECKSUM
    use_polling = true;		// polling
#elif defined(ONLY_USE_INTERRUPT_FOR_CHECKSUM)
    use_polling = false;	// interrupt
#elif defined(ADAPTIVE_FOR_CHECKSUM_COMPLETION)
    printk("[%s] check\n", __func__);
#else
    printk(KERN_ERR "[%s] add commit.h !\n", __func__);
#endif

    //bool use_polling = false;	// interrupt
    u32 crc32 = crc_seed;

	// pr_info("\t\t%s(): start\n", __func__);

    /* atomic 컨텍스트?, 작은 버퍼 -> 즉시 CPU-CRC - 이후에 적용하기 */
    // if (in_atomic() || irqs_disabled() || size < DSA_MIN_BYTES)
    //     return nova_crc32c(seed, src, size);

    if (use_polling) {  /* my polling and interrupt same flags*/
        // pr_info("Set up flags\n");
        flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;  //  0x02 | 0x01
    }

    nova_dma_chan = nova_link_channel_by_corenum();
    if (!nova_dma_chan) {
        /* If no DMA channel available, fallback to CPU copy */
        pr_info("\t\t%s(): DMA channel not available - fallback to CPU crc32() *\n", __func__);
        // nova_crc32c(dst, src, size);
        return 0;
    }

    // pr_info("NOVA: %s(): DMA channel [%s] acquired\n", __func__, dma_chan_name(nova_dma_chan));

    dma_dev = nova_dma_chan->device;
    um = dmaengine_get_unmap_data(dmaengine_get_dma_device(nova_dma_chan), 2, GFP_KERNEL);
    if (!um) {
        pr_err("%s: failed to allocate unmap data, falling back to CPU memcpy\n", __func__);
        // nova_crc32c(dst, src, size);
        return -ENOMEM;
    }
    um->len = size;

    dma_src = dma_map_single(dma_dev->dev, (void *)src, size, DMA_TO_DEVICE);
    if (dma_mapping_error(dma_dev->dev, dma_src)) {
        pr_err("%s: failed to map source\n", __func__);
        dmaengine_unmap_put(um);
        // nova_crc32c(dst, src, size);
        return -EFAULT;
    }
    um->addr[0] = dma_src;
    um->to_cnt++; /* We have 1 buffer used as DMA_TO_DEVICE */

    u32 seed = (uint32_t)crc_seed;

    // idxd_dma_submit_crc32은 폴링으로 구현 완료됨.
    /* internally polling! */
    // tx = dmaengine_prep_dma_crc32(nova_dma_chan, crc_seed, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    tx = dmaengine_prep_dma_crc32(nova_dma_chan, NULL, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    if (!tx) {
        pr_err("%s: dmaengine_prep_dma_crc32 failed\n", __func__);
        dmaengine_unmap_put(um);
        // nova_crc32c(dst, src, size);
        return -EIO;
    }
    /* Start hardware */
	// pr_info("\t\t%s(): start DSA offloading\n", __func__);

    cookie = dmaengine_submit(tx);
    if (dma_submit_error(cookie)) {
        pr_err("%s: dma_submit_error\n", __func__);
        dmaengine_unmap_put(um);
        // nova_crc32c(dst, src, size); // if failed, perform CPU crc
        return -EIO;
    }
    // pr_info("\t\t%s(): dmaengine_submit done\n", __func__);

    if (use_polling) {  // Wait for completion in a polling manner
        /*
        * 6) Wait for completion in a polling manner
        *    For synchronous use, call dma_sync_wait().
        */
        struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);   // get idxd_desc struct
        // struct dsa_hw_desc* hwd = desc->hw;
        cr = desc->completion; // 여기에서 completion record를 가져옴. // 이건 정상적으로 되네.. 뭐지? cr과 cr2의 관계가 뭐지..
        // struct dsa_completion_record *cr2 = desc->hw->completion_addr; // 여기에서 completion record를 가져옴. // 이건 안됨. dma pinned address (e.g., 00000000fffff040)

        dma_async_issue_pending(nova_dma_chan); // this do nothing
        // pr_info("\t %s(): hw_desc addr: %px, cr addr: %px, cr2 addr: %px\n",__func__ , hwd, cr, cr2);
        // pr_info("\t %s(): cr->status: %u\n", __func__, cr->status);
        // pr_info("DSA POLLING!!\n");

        /* polling */
        while(!READ_ONCE(cr->status)) {
            // device_tx_status // just returning DMA_OUT_OF_ORDER - we cannot use this this do not explicitly show success.
            // pr_info("\t %s(): cr->status: %u\n", __func__, cr->status); // 0
            cpu_relax();
        }
        // pr_info("\t %s(): cr->status: %u\n", __func__, cr->status); // 1
	//
	//dma_rmb(); //dd

	u8 st = READ_ONCE(desc->completion->status);
	u32 hw = (u32)le64_to_cpu(desc->completion->crc_val);

        //if (status != DMA_COMPLETE && status != DMA_OUT_OF_ORDER) {
        if (cr->status != DSA_COMP_SUCCESS) {
            pr_err("%s: DMA transfer error, status=%d\n", __func__, status);
            ret = 0;
        //} else if (status == DMA_OUT_OF_ORDER) {
          //  pr_info("%s(): DMA out of order\n", __func__);
            // pr_info("DMA_OUT_OF_ORDER but hardware is NO_ORDER capable. (DSA do not support order completion)\n");
        } else {    // success
            // pr_info("\t\t%s() SUCCESS\n", __func__);
            // pr_info("\t\tcr pointer: %px\n", cr);
            // pr_info("\t\tcrc_val addr: %px\n", &cr->crc_val);

            // pr_info("\t\t%s(): cr->crc_val: %d\n", __func__, cr->crc_val);
            // pr_info("\t\t%s(): le32_to_cpu(cr->crc_val): %d\n", __func__,  le32_to_cpu(cr->crc_val)); // same
	    crc32 = (u32)READ_ONCE(cr->crc_val);
            crc32 = le32_to_cpu((__le32)crc32);
            ret = crc32;
        }
    }


    // pr_info("%s() ############### crc32: %u\n", __func__, crc32);

    /* 7) Unmap the buffers (puts the unmap data) */
    if (um) {
        dmaengine_unmap_put(um);
        // pr_info("\t\t%s(): dmaengine_unmap_put() done\n", __func__);
    }

    /* 8) If we had a valid descriptor, free it */
    if (tx) {
        dmaengine_desc_free(tx);
        // pr_info("\t\t%s(): dmaengine_desc_free() done\n", __func__);
    }

    /* 9) Release the channel if allocated */
    // nova_dmaengine_exit(nova_dma_chan);
	// pr_info("\t\t%s(): done DSA, ret: %d\n", __func__, ret);
    // pr_info("\t\t%s() cr->crc_val: %llu\n", __func__, cr->crc_val);
    // pr_info("\t\t%s() cr->crc_val: %d\n", __func__, cr->crc_val);

    return ret;
}

u32 __nova_dma_crc32_phys(u32 crc_seed, phys_addr_t phys, size_t size)
{
    // struct dma_device *dma_dev;
    struct device *dev;
    dma_addr_t dma_src;
    struct dma_async_tx_descriptor *tx = NULL;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;  //  0x02 | 0x01
    int ret = 0;
    int err;
    struct dsa_completion_record *cr;
    struct dma_chan *nova_dma_chan = nova_link_channel_by_corenum();
    bool use_polling;		
#ifdef ONLY_USE_POLLING_FOR_CHECKSUM
    use_polling = true;		// polling
#elif defined(ONLY_USE_INTERRUPT_FOR_CHECKSUM)
    use_polling = false;	// interrupt
#elif defined(ADAPTIVE_FOR_CHECKSUM_COMPLETION)
    printk("[%s] check\n", __func__);
#else
    printk(KERN_ERR "[%s] add commit.h !\n", __func__);
#endif

    u32 crc32 = 77777;  // crc32: crc32 calc result

	// pr_info("\t\t%s(): start\n", __func__);

    if (!nova_dma_chan){
        pr_err("%s: No channel available\n", __func__);
        return ~EIO; // no channel. 상위 코드에서 CPU-CRC 로 재시도?
    }

    dev = nova_dma_chan->device->dev;

    dma_src = dma_map_resource(dev, phys, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
    if (dma_mapping_error(dev, dma_src)){
        pr_err("%s: DMA mapping error\n", __func__);
        return ~EIO; // 상위 코드에서 CPU-CRC 로 재시도?
    }
    u32 seed = (uint32_t)crc_seed;

    // tx = dmaengine_prep_dma_crc32(nova_dma_chan, crc_seed, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    tx = dmaengine_prep_dma_crc32(nova_dma_chan, NULL, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    if (!tx) {
        pr_err("%s: dmaengine_prep_dma_crc32 failed\n", __func__);
        dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
        // nova_crc32c(dst, src, size);
        return ~EIO; // 상위 코드에서 CPU-CRC 로 재시도?
    }

if (!use_polling) {
	struct nova_dma_crc32_done done;
	struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);

	cr = desc->completion;

	init_completion(&done.done);
	done.result.result = DMA_TRANS_ABORTED;
	done.result.residue = size;
	done.cr = cr;
	done.crc32 = 0;
	done.cr_status = 0;

	tx->callback_result = nova_dma_crc32_done_cb;
	tx->callback_param = &done;

	err = dma_submit_error(dmaengine_submit(tx));
	if (err) {
		pr_err("%s: dma_submit_error in interrupt path\n", __func__);
		dmaengine_desc_free(tx);
		dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
		return ~EIO;
	}

	dma_async_issue_pending(nova_dma_chan);
	ret = wait_for_completion_timeout(&done.done,
					  msecs_to_jiffies(5000));
	if (ret == 0) {
		pr_err("%s: interrupt completion timed out\n", __func__);
		dmaengine_terminate_sync(nova_dma_chan);
		dmaengine_desc_free(tx);
		dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
		return ~ETIMEDOUT;
	}

	if (done.result.result != DMA_TRANS_NOERROR ||
	    done.cr_status != DSA_COMP_SUCCESS) {
		pr_err("%s: interrupt completion error result=%d residue=%u cr_status=%u\n",
		       __func__, done.result.result, done.result.residue,
		       done.cr_status);
		dmaengine_desc_free(tx);
		dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
		return ~EIO;
	}

	crc32 = done.crc32;

	dmaengine_desc_free(tx);
	dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE,
			   DMA_ATTR_SKIP_CPU_SYNC);

	return crc32;
}
    err = dma_submit_error(dmaengine_submit(tx));
    dma_async_issue_pending(nova_dma_chan); // do nothing
    struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);   // get idxd_desc struct
    cr = desc->completion;

    /* polling */
    while(!cr->status) {
        cpu_relax();
    }

    crc32 = le32_to_cpu(cr->crc_val);
    // pr_info("\t\t%s() cr->crc_val: %d\n", __func__, cr->crc_val);
    // pr_info("\t\t%s() crc32: %d\n", __func__, crc32);
    // pr_info("%s() ############### crc32: %u\n", __func__, crc32);

    dmaengine_desc_free(tx);
    dma_unmap_resource(dev, dma_src, size, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);

    return crc32;
}

u32 __nova_dma_crc32(u32 crc_seed, const u8 *src, size_t size)
{
    struct dma_device *dma_dev;
    dma_addr_t dma_src;
    struct dma_async_tx_descriptor *tx = NULL;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    dma_cookie_t cookie;
    enum dma_status status;
    struct dmaengine_unmap_data *um;
    struct completion dma_completion;
    int ret = 0;
    struct dsa_completion_record *cr;
    struct dma_chan *nova_dma_chan;
    bool use_polling;		
#ifdef ONLY_USE_POLLING_FOR_CHECKSUM
    use_polling = true;		// polling
#elif defined(ONLY_USE_INTERRUPT_FOR_CHECKSUM)
    use_polling = false;	// interrupt
#elif defined(ADAPTIVE_FOR_CHECKSUM_COMPLETION)
    printk("[%s] check\n", __func__);
#else
    printk(KERN_ERR "[%s] add commit.h !\n", __func__);
#endif

    u32 crc32 = crc_seed;

	// pr_info("\t\t%s(): start\n", __func__);

    /* atomic 컨텍스트?, 작은 버퍼 -> 즉시 CPU-CRC - 이후에 적용하기 */
    // if (in_atomic() || irqs_disabled() || size < DSA_MIN_BYTES)
    //     return nova_crc32c(seed, src, size);

    if (use_polling) {  /* my polling and interrupt same flags*/
        // pr_info("Set up flags\n");
        flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;  //  0x02 | 0x01
    }

    nova_dma_chan = nova_link_channel_by_corenum();
    if (!nova_dma_chan) {
        /* If no DMA channel available, fallback to CPU copy */
        pr_info("\t\t%s(): DMA channel not available - fallback to CPU crc32() *\n", __func__);
        // nova_crc32c(dst, src, size);
        return 0;
    }

    // pr_info("NOVA: %s(): DMA channel [%s] acquired\n", __func__, dma_chan_name(nova_dma_chan));

    dma_dev = nova_dma_chan->device;
    um = dmaengine_get_unmap_data(dmaengine_get_dma_device(nova_dma_chan), 2, GFP_KERNEL);
    if (!um) {
        pr_err("%s: failed to allocate unmap data, falling back to CPU memcpy\n", __func__);
        // nova_crc32c(dst, src, size);
        return -ENOMEM;
    }
    um->len = size;

    dma_src = dma_map_single(dma_dev->dev, (void *)src, size, DMA_TO_DEVICE);
    if (dma_mapping_error(dma_dev->dev, dma_src)) {
        pr_err("%s: failed to map source\n", __func__);
        dmaengine_unmap_put(um);
        // nova_crc32c(dst, src, size);
        return -EFAULT;
    }
    um->addr[0] = dma_src;
    um->to_cnt++; /* We have 1 buffer used as DMA_TO_DEVICE */

    u32 seed = (uint32_t)crc_seed;

    // idxd_dma_submit_crc32은 폴링으로 구현 완료됨.
    /* internally polling! */
    // tx = dmaengine_prep_dma_crc32(nova_dma_chan, crc_seed, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    tx = dmaengine_prep_dma_crc32(nova_dma_chan, NULL, dma_src, size, seed, flags); // idxd_dma_submit_crc32로 가는 함수임.
    if (!tx) {
        pr_err("%s: dmaengine_prep_dma_crc32 failed\n", __func__);
        dmaengine_unmap_put(um);
        // nova_crc32c(dst, src, size);
        return -EIO;
    }

    /* Start hardware */
	// pr_info("\t\t%s(): start DSA offloading\n", __func__);

if (!use_polling) {
	struct nova_dma_crc32_done done;
	struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);

	cr = desc->completion;

	init_completion(&done.done);
	done.result.result = DMA_TRANS_ABORTED;
	done.result.residue = size;
	done.cr = cr;
	done.crc32 = 0;
	done.cr_status = 0;

	tx->callback_result = nova_dma_crc32_done_cb;
	tx->callback_param = &done;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		pr_err("%s: dma_submit_error in interrupt path\n", __func__);
		dmaengine_unmap_put(um);
		dmaengine_desc_free(tx);
		return -EIO;
	}

	dma_async_issue_pending(nova_dma_chan);
	ret = wait_for_completion_timeout(&done.done,
					  msecs_to_jiffies(5000));
	if (ret == 0) {
		pr_err("%s: interrupt completion timed out\n", __func__);
		dmaengine_terminate_sync(nova_dma_chan);
		dmaengine_unmap_put(um);
		dmaengine_desc_free(tx);
		return -ETIMEDOUT;
	}

	if (done.result.result != DMA_TRANS_NOERROR ||
	    done.cr_status != DSA_COMP_SUCCESS) {
		pr_err("%s: interrupt completion error result=%d residue=%u cr_status=%u\n",
		       __func__, done.result.result, done.result.residue,
		       done.cr_status);
		dmaengine_unmap_put(um);
		dmaengine_desc_free(tx);
		return -EIO;
	}

	crc32 = done.crc32;

// debugging
#if 0
	if (unlikely(size <= 128)) {
		static atomic_t dbg_cnt = ATOMIC_INIT(0);
		int n = atomic_inc_return(&dbg_cnt);

		if (n <= 80) {
			pr_err("[DSA_CRC_INT_DONE] n=%d size=%zu cpu=%d pid=%d "
			       "chan=%px cookie=%d cr_status=%u crc=%x src=%px\n",
			       n, size, raw_smp_processor_id(), current->pid,
			       nova_dma_chan, cookie, done.cr_status, crc32, src);
		}
	}
#endif
	dmaengine_unmap_put(um);
	dmaengine_desc_free(tx);

	return crc32;
}

    cookie = dmaengine_submit(tx);
    if (dma_submit_error(cookie)) {
        pr_err("%s: dma_submit_error\n", __func__);
        dmaengine_unmap_put(um);
        // nova_crc32c(dst, src, size); // if failed, perform CPU crc
        return -EIO;
    }
    // pr_info("\t\t%s(): dmaengine_submit done\n", __func__);

    if (use_polling) {  // Wait for completion in a polling manner
        /*
        * 6) Wait for completion in a polling manner
        *    For synchronous use, call dma_sync_wait().
        */
        struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);   // get idxd_desc struct
        // struct dsa_hw_desc* hwd = desc->hw;
        cr = desc->completion; // 여기에서 completion record를 가져옴. // 이건 정상적으로 되네.. 뭐지? cr과 cr2의 관계가 뭐지..
        // struct dsa_completion_record *cr2 = desc->hw->completion_addr; // 여기에서 completion record를 가져옴. // 이건 안됨. dma pinned address (e.g., 00000000fffff040)

        dma_async_issue_pending(nova_dma_chan); // this do nothing
        // pr_info("\t %s(): hw_desc addr: %px, cr addr: %px, cr2 addr: %px\n",__func__ , hwd, cr, cr2);
        // pr_info("\t %s(): cr->status: %u\n", __func__, cr->status);
        // pr_info("DSA POLLING!!\n");

        /* polling */
        while(!READ_ONCE(cr->status)) {
            // device_tx_status // just returning DMA_OUT_OF_ORDER - we cannot use this this do not explicitly show success.
            // pr_info("\t %s(): cr->status: %u\n", __func__, cr->status); // 0
            cpu_relax();
        }
        // pr_info("\t %s(): cr->status: %u\n", __func__, cr->status); // 1
	//
	//dma_rmb(); //dd

	u8 st = READ_ONCE(desc->completion->status);
	u32 hw = (u32)le64_to_cpu(desc->completion->crc_val);

        //if (status != DMA_COMPLETE && status != DMA_OUT_OF_ORDER) {
        if (cr->status != DSA_COMP_SUCCESS) {
            pr_err("%s: DMA transfer error, status=%d\n", __func__, status);
            ret = 0;
        //} else if (status == DMA_OUT_OF_ORDER) {
          //  pr_info("%s(): DMA out of order\n", __func__);
            // pr_info("DMA_OUT_OF_ORDER but hardware is NO_ORDER capable. (DSA do not support order completion)\n");
        } else {    // success
            // pr_info("\t\t%s() SUCCESS\n", __func__);
            // pr_info("\t\tcr pointer: %px\n", cr);
            // pr_info("\t\tcrc_val addr: %px\n", &cr->crc_val);

            // pr_info("\t\t%s(): cr->crc_val: %d\n", __func__, cr->crc_val);
            // pr_info("\t\t%s(): le32_to_cpu(cr->crc_val): %d\n", __func__,  le32_to_cpu(cr->crc_val)); // same
	    crc32 = (u32)READ_ONCE(cr->crc_val);
            crc32 = le32_to_cpu((__le32)crc32);
            ret = crc32;
        }
    }


    // pr_info("%s() ############### crc32: %u\n", __func__, crc32);

    /* 7) Unmap the buffers (puts the unmap data) */
    if (um) {
        dmaengine_unmap_put(um);
        // pr_info("\t\t%s(): dmaengine_unmap_put() done\n", __func__);
    }

    /* 8) If we had a valid descriptor, free it */
    if (tx) {
        dmaengine_desc_free(tx);
        // pr_info("\t\t%s(): dmaengine_desc_free() done\n", __func__);
    }

    /* 9) Release the channel if allocated */
    // nova_dmaengine_exit(nova_dma_chan);
	// pr_info("\t\t%s(): done DSA, ret: %d\n", __func__, ret);
    // pr_info("\t\t%s() cr->crc_val: %llu\n", __func__, cr->crc_val);
    // pr_info("\t\t%s() cr->crc_val: %d\n", __func__, cr->crc_val);

    return ret;
}

/*
 * nova_dma_memcpy_mode - Offload a copy to Intel DSA via DMA engine
 *
 * This function only performs DSA memcpy.
 * It does NOT fallback to CPU memcpy internally.
 *
 * @dst:   Destination buffer (kernel virtual address)
 * @src:   Source buffer (kernel virtual address)
 * @size:  Number of bytes to copy
 * @mode:  completion mode (npolling / interrupt)
 *
 * Return:
 *   0        on DSA success
 *   -errno   on DSA setup/submission/completion failure
 */
int nova_dma_memcpy_mode(void *dst, const void *src, size_t size,
                         enum nova_dsa_wait_mode mode)
{
        struct dma_device *dma_dev;
        struct dma_chan *nova_dma_chan;
        struct dma_async_tx_descriptor *tx = NULL;
        struct dmaengine_unmap_data *um = NULL;
        dma_addr_t dma_src, dma_dst;
        dma_cookie_t cookie;
        enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
        int ret = 0;

        if (!dst || !src || size == 0)
                return -EINVAL;

        nova_dma_chan = nova_link_channel_by_corenum();
        if (!nova_dma_chan) {
                pr_err("%s: DMA channel not available\n", __func__);
                return -ENODEV;
        }

        dma_dev = nova_dma_chan->device;

        um = dmaengine_get_unmap_data(dmaengine_get_dma_device(nova_dma_chan),
                                      2, GFP_KERNEL);
        if (!um) {
                pr_err("%s: failed to allocate unmap data\n", __func__);
                return -ENOMEM;
        }

        um->len = size;

        dma_src = dma_map_single(dma_dev->dev, (void *)src, size,
                                 DMA_TO_DEVICE);
        if (dma_mapping_error(dma_dev->dev, dma_src)) {
                pr_err("%s: failed to map source\n", __func__);
                ret = -EFAULT;
                goto out_unmap;
        }

        um->addr[0] = dma_src;
        um->to_cnt = 1;

        dma_dst = dma_map_single(dma_dev->dev, dst, size,
                                 DMA_BIDIRECTIONAL);
        if (dma_mapping_error(dma_dev->dev, dma_dst)) {
                pr_err("%s: failed to map destination\n", __func__);
                ret = -EFAULT;
                goto out_unmap;
        }

        um->addr[1] = dma_dst;
        um->bidi_cnt = 1;

        tx = dmaengine_prep_dma_memcpy(nova_dma_chan, dma_dst, dma_src,
                                       size, flags);
        if (!tx) {
                pr_err("%s: dmaengine_prep_dma_memcpy failed\n", __func__);
                ret = -EIO;
                goto out_unmap;
        }

        if (mode == NOVA_DSA_WAIT_INTERRUPT) {
                struct nova_dma_memcpy_done done;

                init_completion(&done.done);
                done.result.result = DMA_TRANS_ABORTED;
                done.result.residue = size;

                tx->callback_result = nova_dma_memcpy_done_cb;
                tx->callback_param = &done;

                cookie = dmaengine_submit(tx);
                if (dma_submit_error(cookie)) {
                        pr_err("%s: dma_submit_error\n", __func__);
                        ret = -EIO;
                        goto out_unmap;
                }

                dma_async_issue_pending(nova_dma_chan);

                ret = wait_for_completion_timeout(&done.done,
                                                  msecs_to_jiffies(5000));
                if (ret == 0) {
                        pr_err("%s: interrupt completion timed out\n",
                               __func__);
                        dmaengine_terminate_sync(nova_dma_chan);
                        ret = -ETIMEDOUT;
                        goto out_unmap;
                }

                if (done.result.result != DMA_TRANS_NOERROR) {
                        pr_err("%s: interrupt completion error result=%d residue=%u\n",
                               __func__, done.result.result,
                               done.result.residue);
                        ret = -EIO;
                        goto out_unmap;
                }

                ret = 0;
        } else {
                struct idxd_desc *desc;
                struct dsa_completion_record *cr;
                u8 cr_status;
                u8 comp_status;

                cookie = dmaengine_submit(tx);
                if (dma_submit_error(cookie)) {
                        pr_err("%s: dma_submit_error\n", __func__);
                        ret = -EIO;
                        goto out_unmap;
                }

                desc = container_of(tx, struct idxd_desc, txd);
                cr = desc->completion;

                dma_async_issue_pending(nova_dma_chan);

                do {
                        cr_status = READ_ONCE(cr->status);
                        if (!cr_status)
                                cpu_relax();
                } while (!cr_status);

                dma_rmb();

                comp_status = cr_status & DSA_COMP_STATUS_MASK;

                if (comp_status != DSA_COMP_SUCCESS) {
                        pr_err("%s: polling completion error cr_status=%u comp_status=%u\n",
                               __func__, cr_status, comp_status);
                        ret = -EIO;
                } else {
                        ret = 0;
                }
        }

out_unmap:
        if (um)
                dmaengine_unmap_put(um);

        if (tx)
                dmaengine_desc_free(tx);

        return ret;
}

/*
 * Default path keeps the previous behavior: polling DSA memcpy.
 * For interrupt-only experiment, temporarily change this wrapper to
 * NOVA_DSA_WAIT_INTERRUPT.
 */
int nova_dma_memcpy(void *dst, const void *src, size_t size)
{
#ifdef ONLY_USE_POLLING_FOR_MEMCPY
        return nova_dma_memcpy_mode(dst, src, size, NOVA_DSA_WAIT_POLLING);
#elif defined(ONLY_USE_INTERRUPT_FOR_MEMCPY)
        return nova_dma_memcpy_mode(dst, src, size, NOVA_DSA_WAIT_INTERRUPT);
#elif defined(ADAPTIVE_FOR_MEMCPY_COMPLETION)
	printk("[%s] check\n", __func__);
	return -1;
#endif
	printk(KERN_ERR "[%s] add commit\n", __func__);
}

// Sangjin Luma memcpy 최외곽 API
int nova_offload_memcpy_to_nvmm(void *dst, const void *src,
	// unsigned int size)
    size_t size)
{
	void *tmp_buf;
	size_t copied = 0;
	int ret = 0;

	/* Simple approach: allocate a temporary kernel buffer */
	tmp_buf = kmalloc(size, GFP_KERNEL);
	if (!tmp_buf){
	    pr_err("%s: tmp_buf allocation failed\n", __func__);
	    return -1;
	}

	/* 1) Copy user → tmp_buf (regular CPU) */
	copied = size - __copy_from_user(tmp_buf, src, size);
	if (copied == 0) {
	    pr_err("%s: copy_from_user to tmp_buf failed\n", __func__);
	    kfree(tmp_buf);
	    return -1;
	}
	// pr_info("## size: %d, copied: %d\n", size, copied); // SNAPSHOT -> size:40? copied: 40?

	ret = nova_dma_memcpy(dst, tmp_buf, copied);   // returns negative on failure
	if (ret < 0) {  // offloading failed
		pr_err("%s: nova_dma_memcpy failed, fallback to CPU copy\n", __func__);
		ret = __copy_from_user_inatomic_nocache(dst, src, size);
	}
	// pr_info("## performed nova_dma_memcpy! ret: %d\n", __func__, ret); // ret 값이 왜 -1055754240 나오지
	ret = 0;

	return ret;
}

struct nova_dma_done {
	struct completion done;
	struct dmaengine_result result;
};

static void nova_dma_done_cb(void *param, const struct dmaengine_result *res)
{
	struct nova_dma_done *ctx = param;

	if (res)
		ctx->result = *res;
	else
		ctx->result.result = DMA_TRANS_ABORTED;

	complete(&ctx->done);
}

static inline bool nova_ranges_overlap(const void *a, const void *b, size_t len)
{
	unsigned long a0 = (unsigned long)a;
	unsigned long b0 = (unsigned long)b;

	return (a0 < b0 + len) && (b0 < a0 + len);
}

//#define NOVA_DUALCAST_MIN_LEN 256
bool nova_dsa_dualcast_possible(const void *dst1, const void *dst2, size_t len)
{
	if (!dst1 || !dst2 || !len)
		return false;

//	if (len < NOVA_DUALCAST_MIN_LEN)
//		return false;

	/* DSA dualcast requirement: bits 11:0 must match */
	if ((((unsigned long)dst1 ^ (unsigned long)dst2) & (PAGE_SIZE - 1)) != 0)
		return false;

	if (nova_ranges_overlap(dst1, dst2, len))
		return false;

	return true;
}

ssize_t nova_offload_dualcast_to_nvmm(void *dst1, void *dst2,
				const void *src, size_t len)
{
	struct dma_chan *chan;
	struct device *dev;
	struct dma_async_tx_descriptor *tx;
	struct nova_dma_done ctx;
	dma_cookie_t cookie;
	unsigned long tmo;

	dma_addr_t src_dma;
	dma_addr_t dst1_dma;
	dma_addr_t dst2_dma;
	bool src_mapped = false, dst1_mapped = false, dst2_mapped = false;

	ssize_t ret = 0;

	if (!src || !dst1 || !dst2 || !len)
		return -EINVAL;

	/*
	 * fast reject -> fallback
	 */
	if (!nova_dsa_dualcast_possible(dst1, dst2, len))
		goto fallback;

	if (nova_ranges_overlap(src, dst1, len) ||
	    nova_ranges_overlap(src, dst2, len))
		goto fallback;

	chan = nova_link_channel_by_corenum();
	if (!chan || !chan->device || !chan->device->dev)
		goto fallback;

	if (!chan->device->device_prep_dma_dualcast)
		goto fallback;

	dev = chan->device->dev;

	init_completion(&ctx.done);
	ctx.result.result = DMA_TRANS_ABORTED;
	ctx.result.residue = len;

	src_dma = dma_map_single(dev, (void *)src, len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, src_dma))
		goto fallback;
	src_mapped = true;

	dst1_dma = dma_map_single(dev, dst1, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dst1_dma)) {
		ret = -EIO;
		goto out_unmap;
	}
	dst1_mapped = true;

	dst2_dma = dma_map_single(dev, dst2, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dst2_dma)) {
		ret = -EIO;
		goto out_unmap;
	}
	dst2_mapped = true;

	//printk("[%s] dualcast submit!\n", __func__);
	tx = dmaengine_prep_dma_dualcast(chan, dst1_dma, dst2_dma, src_dma, len,
					 DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!tx) {
		ret = -EIO;
		goto out_unmap;
	}

	tx->callback_result = nova_dma_done_cb;
	tx->callback_param = &ctx;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		ret = -EIO;
		goto out_unmap;
	}

	dma_async_issue_pending(chan);

	tmo = wait_for_completion_timeout(&ctx.done, msecs_to_jiffies(5000));
	if (!tmo) {
		ret = -ETIMEDOUT;
		goto out_unmap;
	}

	if (ctx.result.result != DMA_TRANS_NOERROR) {
		ret = -EIO;
		goto out_unmap;
	}

	ret = len;

out_unmap:
	if (src_mapped)
		dma_unmap_single(dev, src_dma, len, DMA_TO_DEVICE);
	if (dst1_mapped)
		dma_unmap_single(dev, dst1_dma, len, DMA_FROM_DEVICE);
	if (dst2_mapped)
		dma_unmap_single(dev, dst2_dma, len, DMA_FROM_DEVICE);

	if (ret >= 0)
		return ret;

fallback:
	printk("[%s] Fallback path!\n", __func__);
	/*
	 * 기존 NOVA 경로로 fallback.
	 * 여기서는 "persistent single-copy helper"를 두 번 호출.
	 */
	/* fallback에서는 DSA를 쓰지 않고 기존 CPU memcpy 경로 사용 */
	ret = memcpy_to_pmem_nocache(dst1, src, len);
	if (ret)
		return -EFAULT;
	nova_flush_buffer(dst1, len, 0);
	
	ret = memcpy_to_pmem_nocache(dst2, src, len);
	if (ret)
		return -EFAULT;
	nova_flush_buffer(dst2, len, 0);

	return len;
}

/* Cleanup and release all dma channels on exit NOVA module */
void nova_dmaengine_exit_all(void)
{
    int i;
    /* Releasing unmap data, tx descriptor should be done before calling this function */
    /* Check list
     * 1. dmaengine_unmap_put(um) once.
     * 2. dmaengine_desc_free(tx) once.
     */

    for (i = 0; i < nova_num_dma_chans; i++) {
        if (nova_dma_chans[i]) {
            dma_release_channel(nova_dma_chans[i]);
            // pr_info("NOVA: DMA channel [%s] released\n",dma_chan_name(nova_dma_chans[i]));
            nova_dma_chans[i] = NULL;
        }
    }
    pr_info("NOVA: All x%d dmaengines released\n", nova_num_dma_chans);
    nova_num_dma_chans = 0;
}
