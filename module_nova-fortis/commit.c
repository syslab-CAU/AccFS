#include "nova.h"
#include "inode.h"

#include <linux/calclock.h>

#include <linux/rbtree.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include "./offload_rr.h"
#include "nova.h"
#include "commit.h"

#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>

//atomic_t cpu_inflight = ATOMIC_INIT(0);
//atomic_t cpu_inflight_data = ATOMIC_INIT(0);
//atomic_t cpu_inflight_meta = ATOMIC_INIT(0);
//atomic_t cpu_inflight_csum = ATOMIC_INIT(0);
//atomic_t cpu_inflight_max = ATOMIC_INIT(0);

atomic64_t cpu_inflight_byte = ATOMIC64_INIT(0);

atomic_t cpu_inflight_data = ATOMIC_INIT(0);
atomic_t cpu_inflight_meta = ATOMIC_INIT(0);
atomic_t cpu_inflight_csum = ATOMIC_INIT(0);

atomic64_t cpu_inflight_bytes = ATOMIC64_INIT(0);
atomic64_t cpu_inflight_bytes_max = ATOMIC64_INIT(0);

//atomic64_t inflight_byte_arr[INFLIGHT_BYTE_BINS];
atomic64_t inflight_byte_sum = ATOMIC64_INIT(0);
atomic64_t inflight_byte_samples = ATOMIC64_INIT(0);
//atomic64_t inflight_byte_max = ATOMIC64_INIT(0);

static inline void nova_atomic64_max_simple(atomic64_t *max, s64 val)
{
        s64 old = atomic64_read(max);

        while (old < val) {
                s64 prev = atomic64_cmpxchg(max, old, val);
                if (prev == old)
                        break;
                old = prev;
        }
}

inline void inc_cpu_inflight_bytes(size_t bytes)
{
        s64 cur;
        int bin;

        cur = atomic64_add_return((s64)bytes, &cpu_inflight_bytes);

        bin = cur >> INFLIGHT_BYTE_BIN_SHIFT;
        if (bin >= INFLIGHT_BYTE_BINS)
                bin = INFLIGHT_BYTE_BINS - 1;

        //atomic64_inc(&inflight_byte_arr[bin]);
        atomic64_add(cur, &inflight_byte_sum);
        atomic64_inc(&inflight_byte_samples);
        //nova_atomic64_max_simple(&inflight_byte_max, cur);
}

inline void dec_cpu_inflight_bytes(size_t bytes)
{
        atomic64_sub((s64)bytes, &cpu_inflight_bytes);
}

static inline void nova_atomic_max(atomic_t *max, int val)
{
        int old;

        old = atomic_read(max);

        while (old < val) {
                int prev = atomic_cmpxchg(max, old, val);

                if (prev == old) {
			printk(KERN_INFO "[NOVA CPU] new max inflight data CPU=%d\n", val);
                        break;
		}

                old = prev;
        }
}

atomic_t inflight_arr[128];
//inline void inc_cpu_inflight(void)
//{
//        int cur;
//
//        cur = atomic_inc_return(&cpu_inflight);
//        //nova_atomic_max(&cpu_inflight_max, cur);
//	atomic_inc(&inflight_arr[cur]);
//}
//
//inline void dec_cpu_inflight(void)
//{
//        atomic_dec(&cpu_inflight);
//}

static inline size_t nova_cpu_inflight_charge(size_t bytes)
{
        if (bytes <= 4UL * 1024UL)
                return CPU_INFLIGHT_CHARGE_4K;

        if (bytes <= 32UL * 1024UL)
                return CPU_INFLIGHT_CHARGE_32K;

        if (bytes <= 64UL * 1024UL)
                return CPU_INFLIGHT_CHARGE_64K;

        if (bytes <= 128UL * 1024UL)
                return CPU_INFLIGHT_CHARGE_128K;

        return CPU_INFLIGHT_CHARGE_256K;
}

inline void inc_cpu_inflight(size_t bytes)
{
        atomic64_add((s64)nova_cpu_inflight_charge(bytes),
                     &cpu_inflight_byte);
}

inline void dec_cpu_inflight(size_t bytes)
{
        atomic64_sub((s64)nova_cpu_inflight_charge(bytes),
                     &cpu_inflight_byte);
}

//static inline bool pick_cpu_or_dsa_for_data(void)
//{
//        int inflight;
//
//        inflight = atomic_read(&cpu_inflight);
//
//        if (inflight >= DATA_INFLIGHT_LIMIT)
//                return USE_DSA;
//
//        return USE_CPU;
//}
//
//static inline bool pick_cpu_or_dsa_for_meta(void)
//{
//        int inflight;
//
//        inflight = atomic_read(&cpu_inflight);
//
//        if (inflight >= META_INFLIGHT_LIMIT)
//                return USE_DSA;
//
//        return USE_CPU;
//}
//
//inline bool pick_cpu_or_dsa_for_csum(void)
//{
//        int inflight;
//
//        inflight = atomic_read(&cpu_inflight);
//
//        if (inflight >= CSUM_INFLIGHT_LIMIT)
//                return USE_DSA;
//
//        return USE_CPU;
//}

static inline bool pick_cpu_or_dsa_by_charge(size_t bytes, size_t limit)
{
        s64 inflight;
        size_t charge;

        inflight = atomic64_read(&cpu_inflight_byte);
        charge = nova_cpu_inflight_charge(bytes);

        if ((u64)inflight + charge > limit)
                return USE_DSA;

        return USE_CPU;
}

static inline bool pick_cpu_or_dsa_for_data(size_t bytes)
{
        return pick_cpu_or_dsa_by_charge(bytes, DATA_INFLIGHT_BYTE_LIMIT);
}

static inline bool pick_cpu_or_dsa_for_meta(size_t bytes)
{
        return pick_cpu_or_dsa_by_charge(bytes, META_INFLIGHT_BYTE_LIMIT);
}

inline bool pick_cpu_or_dsa_for_csum_bytes(size_t bytes)
{
        return pick_cpu_or_dsa_by_charge(bytes, CSUM_INFLIGHT_BYTE_LIMIT);
}

/*
 * Keep the old checksum API for checksum.c.  External metadata entry checksum
 * calls do not pass a basis size, so treat them as one small CPU operation.
 */
inline bool pick_cpu_or_dsa_for_csum(void)
{
        return pick_cpu_or_dsa_for_csum_bytes(4UL * 1024UL);
}

static inline void dbuf_staged_inc(struct super_block *sb, struct nova_inode_info_header *sih, unsigned long pages)
{
	sih->staged_cnt += pages;
	atomic64_add((u64)pages, &NOVA_SB(sb)->staged_data_pages);
}

static inline void dbuf_staged_dec(struct super_block *sb, struct nova_inode_info_header *sih, unsigned long pages)
{
	sih->staged_cnt -= pages;
	atomic64_sub((u64)pages, &NOVA_SB(sb)->staged_data_pages);
}

int commit_inode_for_staged_limit(struct super_block *sb,
        struct inode *inode, unsigned long incoming_pages)
{
        struct nova_sb_info *sbi = NOVA_SB(sb);
        struct nova_inode_info_header *sih = &NOVA_I(inode)->header;
        unsigned long limit_pages;
        s64 staged_pages;

        //limit_pages = totalram_pages() * 4 / 10000;
        limit_pages = totalram_pages() * 40 / 100;
        staged_pages = atomic64_read(&sbi->staged_data_pages);

        if ((u64)staged_pages + incoming_pages < limit_pages)
                return 0;

        if (sih->staged_cnt == 0)
                return 0;

	printk("[%s]: start\n", __func__);
        return commit_process(sb, inode);
}

//inline u32 nova_calc_crc_by_commit_ratio(struct super_block *sb,
//                                          u32 seed,
//                                          const void *buf,
//                                          size_t len,
//                                          const struct cpu_dsa_ctrl *ctrl)
//{
//        if (!pick_cpu_or_dsa_for_data(ctrl))
//                return nova_crc32c(seed, buf, len);
//
//        return nova_dma_crc32c_auto(sb, seed, buf, len);
//}

static inline bool nova_is_pmem_ptr(struct super_block *sb, const void *p)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	return (p >= sbi->virt_addr) && (p < (sbi->virt_addr + sbi->initsize));
}


inline int nova_resolve_runtime_write_entry(struct super_block *sb,
        struct nova_file_write_entry *entry,
        struct nova_file_write_entry **entryc,
        struct nova_file_write_entry *entry_copy)
{
        if (!entry)
                return -EINVAL;

        /* DRAM staged entry */
        if (!nova_is_pmem_ptr(sb, entry)) {
                *entryc = entry;
                return 0;
        }

        /* PMEM entry */
        if (metadata_csum == 0) {
                *entryc = entry;
                return 0;
        }

        if (!nova_verify_entry_csum(sb, entry, entry_copy, FILE_WRITE))
                return -EIO;

        *entryc = entry_copy;
        return 0;
}

static inline bool nova_staged_write_fully_invalid(struct nova_staged_write_node *sw)
{
        return le32_to_cpu(sw->e.invalid_pages) >= le32_to_cpu(sw->e.num_pages);
}

static int discard_data_buffers(struct super_block *sb, struct nova_inode_info_header *sih,
                                unsigned long start, unsigned int num);

static void enqueue_defer_rb_tree(struct super_block *sb,
		struct nova_inode_info_header *sih, unsigned long blocknr, unsigned int num,
		unsigned short btype, int log_page);

static int insert_pmem_reassign_list(struct super_block *sb,
		struct nova_inode_info_header *sih,
		struct nova_file_write_entry *pmem_entry,
		u32 invalid_add);

/*
 * delete / truncate / overwrite로 인해 old run이 사라질 때 처리
 *
 * - PMEM entry:
 *     delete_dead == false : pmem_reassign_list + defer_tree
 *     delete_dead == true  : defer_tree only
 *
 * - DRAM staged entry:
 *     staged entry 내부 invalid_pages/reassigned만 갱신
 *     완전히 invalid 되면 staged_list에서 제거
 */
int nova_finalize_delete_run(struct super_block *sb,
        struct nova_inode_info_header *sih,
        struct nova_file_write_entry *old,
        unsigned long old_pgoff,
        unsigned int old_len,
        bool delete_dead,
        u64 epoch_id,
        int *freed)
{
        struct nova_file_write_entry *entryc;
        struct nova_file_write_entry entry_copy;
        struct nova_staged_write_node *sw;
        unsigned long old_blk;
        int ret;

        if (!old || old_len == 0)
                return 0;

        ret = nova_resolve_runtime_write_entry(sb, old, &entryc, &entry_copy);
        if (ret)
                return ret;

        old_blk = get_nvmm(sb, sih, entryc, old_pgoff);

        /*
         * staged data buffer가 아직 남아 있으면 제거/split
         * (old physical range와 겹치는 deferred data 제거)
         */
        ret = discard_data_buffers(sb, sih, old_blk, old_len);
        if (ret)
                return ret;

        /*
         * snapshot을 실제로 쓰지 않는 현재 가정에서는
         * old data block은 defer free queue로 넣는다.
         */
        enqueue_defer_rb_tree(sb, sih, old_blk, old_len,
                              sih->i_blk_type, 0);

        if (nova_is_pmem_ptr(sb, old)) {
                /*
                 * PMEM old entry
                 * - live inode overwrite/truncate면 commit 시점에 invalid/reassign 반영
                 * - inode destroy면 어차피 log 자체가 제거되므로 per-entry update 불필요
                 */
                if (!delete_dead) {
                        ret = insert_pmem_reassign_list(sb, sih, old, old_len);
                        if (ret)
                                return ret;
                }
        } else {
                /*
                 * DRAM staged entry
                 * container_of(old, ..., e) 가능
                 */
                sw = container_of(old, struct nova_staged_write_node, e);

                sw->e.reassigned = 1;
                sw->e.invalid_pages = cpu_to_le32(
                        le32_to_cpu(sw->e.invalid_pages) + old_len);

                if (delete_dead || nova_staged_write_fully_invalid(sw)) {
                        list_del_init(&sw->list);
                        kfree(sw);
                }
        }

        sih->i_blocks -= old_len;
        if (freed)
                *freed += old_len;

        return 0;
}

inline void *nova_get_data_block_csum_addr(struct super_block *sb,
        struct nova_inode_info_header *sih,
        unsigned long blk, int replica)
{
        u64 blockoff = nova_get_block_off(sb, blk, sih->i_blk_type);
        unsigned long strp_nr;

        /*
         * 기존 NOVA는 csum address를 stripe 번호 기준으로 찾는다.
         * 4KB block == 512B stripe 8개이므로,
         * 해당 block의 "첫 stripe 위치"를 block csum slot으로 사용한다.
         *
         * 즉, block당 32B 예약 공간이 있던 셈인데,
         * 이제 그 중 첫 4B만 사용한다.
         */
        strp_nr = (unsigned long)(blockoff >> NOVA_STRIPE_SHIFT);

        return nova_get_data_csum_addr(sb, strp_nr, replica);
}



#define NOVA_LOG_WENTRY_PER_PAGE \
        (LOG_BLOCK_TAIL / sizeof(struct nova_file_write_entry))
#define NOVA_LOG_TAIL_CL_OFF   (PAGE_SIZE - CACHELINE_SIZE)

struct log_page_batch {
        struct list_head list;
        u64 block_off;    /* BLOCK_OFF(curr_p) */
        u8 *page_buf;     /* PAGE_SIZE shadow copy */

        /* Number of write entries added to this log page in this commit. */
        unsigned int entry_count;

        /* Offset of the first write entry added to this log page. */
        size_t first_entry_off;
};

static void nova_free_log_page_batch_list(struct list_head *head)
{
        struct log_page_batch *b, *tmp;

        list_for_each_entry_safe(b, tmp, head, list) {
                list_del(&b->list);
                kfree(b->page_buf);
                kfree(b);
        }
}

static struct log_page_batch *
get_or_create_log_batch(struct super_block *sb,
                                  struct list_head *batch_list,
                                  u64 block_off)
{
        struct log_page_batch *b;
        void *src;

        if (!list_empty(batch_list)) {
                b = list_last_entry(batch_list, struct log_page_batch, list);
                if (b->block_off == block_off)
                        return b;
        }

        b = kmalloc(sizeof(*b), GFP_NOFS);
        if (!b)
                return NULL;

        b->page_buf = kmalloc(PAGE_SIZE, GFP_NOFS);
        if (!b->page_buf) {
                kfree(b);
                return NULL;
        }

        /*
         * PMEM log page의 현재 상태를 DRAM shadow page로 복사한다.
         * 이 commit에서 추가하는 write entry와 page tail만 shadow에 반영한 뒤,
         * flush 단계에서 entry_count 기준으로 4KB flush/부분 flush를 고른다.
         */
        src = nova_get_block(sb, block_off);
        memcpy(b->page_buf, src, PAGE_SIZE);

        b->block_off = block_off;
        b->entry_count = 0;
        b->first_entry_off = 0;
        INIT_LIST_HEAD(&b->list);
        list_add_tail(&b->list, batch_list);

        return b;
}

static int from_stage_we_to_shadow(struct super_block *sb,
                struct list_head *batch_list,
                u64 entry_p,
                struct nova_file_write_entry *entry)
{
        struct log_page_batch *batch;
        struct nova_inode_log_page *log_page;
        size_t off_in_page;
        u32 num_entries;

        batch = get_or_create_log_batch(sb, batch_list, BLOCK_OFF(entry_p));
        if (!batch)
                return -ENOMEM;

        off_in_page = ENTRY_LOC(entry_p);
        if (batch->entry_count == 0)
                batch->first_entry_off = off_in_page;

        memcpy(batch->page_buf + off_in_page, entry,
               sizeof(struct nova_file_write_entry));
        batch->entry_count++;

        log_page = (struct nova_inode_log_page *)batch->page_buf;
        num_entries = le32_to_cpu(log_page->page_tail.num_entries);
        log_page->page_tail.num_entries = cpu_to_le32(num_entries + 1);

        return 0;
}

atomic_t cpu_cnt3, dsa_cnt3;
atomic_t low_3, mid_3, high_3, super_high_3;
static int flush_log_pages(struct super_block *sb,
                struct log_page_batch *first,
                unsigned int nr_pages)
{
        struct log_page_batch *b = first;
        size_t total = (size_t)nr_pages * PAGE_SIZE;
        u8 *pack = NULL;
        void *dst;
        unsigned int i;
        bool use_dsa_for_meta;

        if (nr_pages == 0)
                return 0;

#ifdef ONLY_USE_CPU_FOR_META
		use_dsa_for_meta = 0;
#elif defined(ONLY_USE_DSA_FOR_META)
		use_dsa_for_meta = 1;
#elif defined(ADAPTIVE_FOR_META)
		use_dsa_for_meta = pick_cpu_or_dsa_for_meta(total);
#else
		printk(KERN_ERR "[%s] add commit.h\n", __func__);
#endif

        if (use_dsa_for_meta)
                pack = kmalloc(total, GFP_NOFS | __GFP_NOWARN | __GFP_NORETRY);

        if (!pack) {
                use_dsa_for_meta = false;
                pack = kvmalloc(total, GFP_NOFS);
                if (!pack)
                        return -ENOMEM;
        }

        for (i = 0; i < nr_pages; i++) {
                memcpy(pack + ((size_t)i << PAGE_SHIFT),
                       b->page_buf, PAGE_SIZE);
                if (i + 1 < nr_pages)
                        b = list_next_entry(b, list);
        }

        dst = nova_get_block(sb, first->block_off);

        if (!use_dsa_for_meta) {
		inc_cpu_inflight(total);
		inc_cpu_inflight_bytes(total);
		atomic_inc(&cpu_inflight_meta);
                memcpy_to_pmem_nocache(dst, pack, total);
		dec_cpu_inflight_bytes(total);
		dec_cpu_inflight(total);
	} else {
                nova_dma_memcpy(dst, pack, total);
	}

        nova_flush_buffer(dst, total, 0);
        kvfree(pack);
        return 0;
}

static void flush_log_range(struct super_block *sb,
        struct log_page_batch *b, size_t off, size_t len)
{
        void *dst = (u8 *)nova_get_block(sb, b->block_off) + off;
        void *src = b->page_buf + off;
        unsigned long irq_flags = 0;

        nova_memunlock_range(sb, dst, len, &irq_flags);
        inc_cpu_inflight(len);
        inc_cpu_inflight_bytes(len);
        atomic_inc(&cpu_inflight_meta);
        memcpy_to_pmem_nocache(dst, src, len);
        dec_cpu_inflight_bytes(len);
        dec_cpu_inflight(len);
        nova_flush_buffer(dst, len, 0);
        nova_memlock_range(sb, dst, len, &irq_flags);
}

static int flush_shadow_pages(struct super_block *sb,
                struct list_head *batch_list)
{
        struct log_page_batch *b;
        struct log_page_batch *run_first = NULL;
        u64 prev_page = 0;
        unsigned int run_pages = 0;
        size_t len;
        int ret;

        BUILD_BUG_ON(NOVA_LOG_PAGE_FLUSH_ENTRY_THRESHOLD == 0);
        BUILD_BUG_ON(NOVA_LOG_PAGE_FLUSH_ENTRY_THRESHOLD >
                NOVA_LOG_WENTRY_PER_PAGE);

        list_for_each_entry(b, batch_list, list) {
                if (b->entry_count >= NOVA_LOG_PAGE_FLUSH_ENTRY_THRESHOLD) {
                        if (run_first && b->block_off == prev_page + PAGE_SIZE) {
                                prev_page = b->block_off;
                                run_pages++;
                                continue;
                        }

                        if (run_first) {
                                ret = flush_log_pages(sb, run_first, run_pages);
                                if (ret)
                                        return ret;
                        }

                        run_first = b;
                        prev_page = b->block_off;
                        run_pages = 1;
                        continue;
                }

                if (run_first) {
                        ret = flush_log_pages(sb, run_first, run_pages);
                        if (ret)
                                return ret;
                        run_first = NULL;
                        run_pages = 0;
                }

                /*
                 * batch는 from_stage_we_to_shadow()에서만 만들어지므로
                 * entry_count는 항상 1 이상이다. append는 log tail 순서라서
                 * 이번 commit에서 이 page에 추가한 write entry들도 연속이다.
                 */
                len = b->entry_count * sizeof(struct nova_file_write_entry);
                flush_log_range(sb, b, b->first_entry_off, len);

                /* page_tail과 NEXT_PAGE marker가 있는 마지막 cacheline. */
                flush_log_range(sb, b, NOVA_LOG_TAIL_CL_OFF, CACHELINE_SIZE);
        }

        if (run_first)
                return flush_log_pages(sb, run_first, run_pages);

        return 0;
}

static inline unsigned long dbuf_end(const struct data_buf_node *n)
{
	return n->blocknr + n->blk_cnt; /* end-exclusive */
}


static struct data_buf_node *
dbuf_lookup_node(struct nova_inode_info_header *sih, unsigned long blk)
{
	struct rb_node *n = sih->dbuf_rb.rb_node;

	while (n) {
		struct data_buf_node *cur =
			rb_entry(n, struct data_buf_node, rb);

		if (blk < cur->blocknr)
			n = n->rb_left;
		else if (blk >= dbuf_end(cur))
			n = n->rb_right;
		else
			return cur;
	}
	return NULL;
}

/* nvmm physical blocknr(4KB) -> staged DRAM page ptr (or NULL) */
u8 *dbuf_lookup_page(struct inode *inode,
			  unsigned long nvmm_blk)
{
	struct nova_inode_info_header *sih = &NOVA_I(inode)->header;
	struct data_buf_node *n;

	n = dbuf_lookup_node(sih, nvmm_blk);
	if (!n)
		return NULL;

	return n->buf + ((nvmm_blk - n->blocknr) << PAGE_SHIFT);
}

/* ===== staged data buffer (no refs/flushing/wq; inode lock serializes) ===== */
static int load_old_block_or_zero(struct super_block *sb,
        struct inode *inode, unsigned long lblk, u8 *blockbuf,
        size_t new_off, size_t new_len)
{
        struct nova_inode_info *si = NOVA_I(inode);
        struct nova_inode_info_header *sih = &si->header;
        struct nova_file_write_entry *entry, *entryc, entry_copy;
        unsigned long nvmm, nvmmoff;
        u8 *blockptr;
        size_t blocksize = sb->s_blocksize;
        size_t head_len, tail_off, tail_len;
        int ret;

        head_len = new_off;
        tail_off = new_off + new_len;
        tail_len = (tail_off < blocksize) ? (blocksize - tail_off) : 0;

        entry = nova_get_write_entry(sb, sih, lblk);
        if (!entry) {
                if (head_len)
                        memset(blockbuf, 0, head_len);
                if (tail_len)
                        memset(blockbuf + tail_off, 0, tail_len);
                return 0;
        }

        ret = nova_resolve_runtime_write_entry(sb, entry, &entryc, &entry_copy);
        if (ret)
                return ret;

        nvmm = get_nvmm(sb, sih, entryc, lblk);

        /* staged data buffer가 있으면 그걸 old source로 사용 */
        {
                u8 *pending = dbuf_lookup_page(inode, nvmm);
                if (pending) {
                        if (head_len)
                                memcpy(blockbuf, pending, head_len);
                        if (tail_len)
                                memcpy(blockbuf + tail_off,
                                       pending + tail_off, tail_len);
                        return 0;
                }
        }

        nvmmoff = nova_get_block_off(sb, nvmm, sih->i_blk_type);
        blockptr = (u8 *)nova_get_block(sb, nvmmoff);

        if (head_len) {
                ret = copy_mc_to_kernel(blockbuf, blockptr, head_len);
                if (ret < 0)
                        return ret;
        }
        if (tail_len) {
                ret = copy_mc_to_kernel(blockbuf + tail_off,
                                        blockptr + tail_off, tail_len);
                if (ret < 0)
                        return ret;
        }

        return 0;
}

static int dbuf_append_pages(struct super_block *sb, struct nova_inode_info_header* sih, struct data_buf_node *n,
                             const u8 *src, unsigned long src_pages,
                             unsigned long *appended_pages)
{
        unsigned long cap_left, do_pages;
        u8 *base;
        size_t new_size;

        *appended_pages = 0;

        if (src_pages == 0)
                return 0;

        if (n->blk_cnt >= NOVA_DBUF_MAX_PAGES)
                return 0; /* full */

        cap_left = NOVA_DBUF_MAX_PAGES - n->blk_cnt;
        do_pages = (src_pages < cap_left) ? src_pages : cap_left;
        if (do_pages == 0)
                return 0;

        new_size = (n->blk_cnt + do_pages) << PAGE_SHIFT;

        base = krealloc(n->buf, new_size, GFP_NOFS);
        if (!base)
                return -ENOMEM;

        n->buf = base;
        memcpy(n->buf + (n->blk_cnt << PAGE_SHIFT),
               src, do_pages << PAGE_SHIFT);

        n->blk_cnt += do_pages;
	dbuf_staged_inc(sb, sih, do_pages);
        *appended_pages = do_pages;
        return 0;
}

/* ===================== staged data buffer helpers ===================== */

/* smallest node with cur->blocknr >= key */
static struct data_buf_node *
dbuf_find_ge(struct nova_inode_info_header *sih, unsigned long key)
{
        struct rb_node *n = sih->dbuf_rb.rb_node;
        struct data_buf_node *res = NULL;

        while (n) {
            struct data_buf_node *cur = rb_entry(n, struct data_buf_node, rb);
            if (cur->blocknr >= key) {
                    res = cur;
                    n = n->rb_left;
            } else {
                    n = n->rb_right;
            }
        }
        return res;
}

/* greatest node with cur->blocknr < key */
static struct data_buf_node *
dbuf_find_prev(struct nova_inode_info_header *sih, unsigned long key)
{
        struct rb_node *n = sih->dbuf_rb.rb_node;
        struct data_buf_node *res = NULL;

        while (n) {
                struct data_buf_node *cur = rb_entry(n, struct data_buf_node, rb);
                if (cur->blocknr < key) {
                        res = cur;
                        n = n->rb_right;
                } else {
                        n = n->rb_left;
                }
        }
        return res;
}


/* predecessor: end <= blk 인 것 중 가장 오른쪽 */
static struct data_buf_node *
dbuf_prev_by_blk(struct nova_inode_info_header *sih, unsigned long blk)
{
	struct rb_node *n = sih->dbuf_rb.rb_node;
	struct data_buf_node *best = NULL;

	while (n) {
		struct data_buf_node *cur =
			rb_entry(n, struct data_buf_node, rb);

		if (dbuf_end(cur) <= blk) {
			best = cur;
			n = n->rb_right;
		} else {
			n = n->rb_left;
		}
	}
	return best;
}

static int dbuf_rb_insert(struct nova_inode_info_header *sih,
			       struct data_buf_node *newn)
{
	struct rb_node **link = &sih->dbuf_rb.rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		struct data_buf_node *cur;
		parent = *link;
		cur = rb_entry(parent, struct data_buf_node, rb);

		if (dbuf_end(newn) <= cur->blocknr)
			link = &(*link)->rb_left;
		else if (newn->blocknr >= dbuf_end(cur))
			link = &(*link)->rb_right;
		else
			return -EEXIST; /* overlap: should not happen */
	}

	rb_link_node(&newn->rb, parent, link);
	rb_insert_color(&newn->rb, &sih->dbuf_rb);
	return 0;
}

static void dbuf_remove_node(struct super_block *sb, struct nova_inode_info_header *sih,
				  struct data_buf_node *n)
{
	dbuf_staged_dec(sb, sih, n->blk_cnt);
	rb_erase(&n->rb, &sih->dbuf_rb);
	list_del(&n->list);
	kfree(n->buf);
	kfree(n);
}

/* ===================== discard/split on overwrite(old physical) ===================== */

/* Find any node overlapping [start,end) */
static struct data_buf_node *
dbuf_find_overlap(struct nova_inode_info_header *sih,
                       unsigned long start, unsigned long end)
{
        struct rb_node *n = sih->dbuf_rb.rb_node;
        struct data_buf_node *res = NULL;

        while (n) {
                struct data_buf_node *cur = rb_entry(n, struct data_buf_node, rb);
                unsigned long cur_start = cur->blocknr;
                unsigned long cur_end = dbuf_end(cur);

                if (end <= cur_start) {
                        n = n->rb_left;
                } else if (start >= cur_end) {
                        n = n->rb_right;
                } else {
                        res = cur;
                        //n = n->rb_left; /* keep searching left overlaps */
			break;
                }
        }
        return res;
}

static int discard_data_buffers(struct super_block *sb, struct nova_inode_info_header *sih,
                                unsigned long start, unsigned int num)
{
        unsigned long end = start + num;

        while (1) {
                struct data_buf_node *cur;
                unsigned long cur_start, cur_end;

                cur = dbuf_find_overlap(sih, start, end);
                if (!cur)
                        break;

                cur_start = cur->blocknr;
                cur_end = dbuf_end(cur);

                /* case 1) fully covered -> delete */
                if (start <= cur_start && end >= cur_end) {
                        dbuf_remove_node(sb, sih, cur);
                        continue;
                }

                /* case 2) overlap head: trim head (need re-key) */
                if (start <= cur_start && end < cur_end) {
                        unsigned long cut = end - cur_start;      /* pages */
                        unsigned long remain = cur_end - end;     /* pages */
                        size_t remain_bytes = remain << PAGE_SHIFT;
                        u8 *newbuf;

                        rb_erase(&cur->rb, &sih->dbuf_rb);

                        memmove(cur->buf,
                                cur->buf + (cut << PAGE_SHIFT),
                                remain_bytes);

                        newbuf = krealloc(cur->buf, remain_bytes, GFP_NOFS);
                        if (newbuf)
                                cur->buf = newbuf;
                        /* shrink 실패는 무시 가능: 앞쪽 데이터는 이미 compact됨 */

			dbuf_staged_dec(sb, sih, cut);
                        cur->blocknr = end;
                        cur->blk_cnt = remain;

                        if (dbuf_rb_insert(sih, cur)) {
                                list_del(&cur->list);
                                kfree(cur->buf);
                                kfree(cur);
                                return -EFAULT;
                        }
                        continue;
                }

                /* case 3) overlap tail: trim tail */
                if (start > cur_start && end >= cur_end) {
                        unsigned long remain = start - cur_start; /* pages */
                        size_t remain_bytes = remain << PAGE_SHIFT;
                        u8 *newbuf;

                        newbuf = krealloc(cur->buf, remain_bytes, GFP_NOFS);
                        if (newbuf)
                                cur->buf = newbuf;
                        /* shrink 실패 무시 가능 */

			dbuf_staged_dec(sb, sih, cur->blk_cnt - remain);
                        cur->blk_cnt = remain;
                        continue;
                }

                /* case 4) middle overlap: split into left + right */
                if (start > cur_start && end < cur_end) {
                        struct data_buf_node *right;
                        unsigned long left_cnt  = start - cur_start;
                        unsigned long right_cnt = cur_end - end;
                        unsigned long right_src_off = end - cur_start;
                        size_t left_bytes  = left_cnt  << PAGE_SHIFT;
                        size_t right_bytes = right_cnt << PAGE_SHIFT;
                        u8 *newleft;

                        right = kmalloc(sizeof(*right), GFP_NOFS);
                        if (!right)
                                return -ENOMEM;

                        right->buf = kmalloc(right_bytes, GFP_NOFS);
                        if (!right->buf) {
                                kfree(right);
                                return -ENOMEM;
                        }

                        memcpy(right->buf,
                               cur->buf + (right_src_off << PAGE_SHIFT),
                               right_bytes);

                        right->blocknr = end;
                        right->blk_cnt = right_cnt;
                        INIT_LIST_HEAD(&right->list);

                        newleft = krealloc(cur->buf, left_bytes, GFP_NOFS);
                        if (newleft)
                                cur->buf = newleft;
                        /* shrink 실패 무시 가능 */

			dbuf_staged_dec(sb, sih, end - start);
                        cur->blk_cnt = left_cnt;

                        if (dbuf_rb_insert(sih, right)) {
                                kfree(right->buf);
                                kfree(right);
                                return -EFAULT;
                        }

                        list_add(&right->list, &cur->list);
                        continue;
                }
        }

        return 0;
}

static void nova_drop_all_inode_staged_writes(struct nova_inode_info_header *sih)
{
        while (!list_empty(&sih->staged_list)) {
                struct nova_staged_write_node *n;
                n = list_first_entry(&sih->staged_list,
                                     struct nova_staged_write_node, list);
                list_del(&n->list);
                kfree(n);
        }
}

static void nova_clear_pmem_reassign_pending(struct nova_inode_info_header *sih)
{
        struct pmem_reassign_node *pr, *tmp;

        list_for_each_entry_safe(pr, tmp, &sih->pmem_reassign_list, list) {
                if (pr->pmem_entry)
                        xa_erase(&sih->pmem_reassign_xa, (unsigned long)pr->pmem_entry);
                list_del(&pr->list);
                kfree(pr);
        }
        xa_destroy(&sih->pmem_reassign_xa);
        xa_init(&sih->pmem_reassign_xa);
}

static void nova_drop_all_defer_free_ranges(struct nova_inode_info_header *sih)
{
        unsigned int i;

        if (!sih->defer_tree || sih->nr_defer == 0)
                return;

        for (i = 0; i < sih->nr_defer; i++) {
                struct rb_root local;
                struct rb_node *node;

                spin_lock(&sih->defer_tree[i].lock);
                local = sih->defer_tree[i].root;
                sih->defer_tree[i].root = RB_ROOT;
                spin_unlock(&sih->defer_tree[i].lock);

                node = rb_first(&local);
                while (node) {
                        struct defer_free_range *r;
                        struct rb_node *next = rb_next(node);

                        r = rb_entry(node, struct defer_free_range, node);
                        rb_erase(&r->node, &local);
                        kfree(r);

                        node = next;
                }
        }
}

void nova_drop_all_inode_data_buffers(struct inode *inode)
{
	struct nova_inode_info_header *sih = &NOVA_I(inode)->header;
	struct data_buf_node *n, *tmp;
	struct super_block *sb = inode->i_sb;

	list_for_each_entry_safe(n, tmp, &sih->dbuf_list, list)
		dbuf_remove_node(sb, sih, n);
}



void nova_abort_deferred_inode_state(struct inode *inode)
{
        struct nova_inode_info_header *sih = &NOVA_I(inode)->header;

        nova_drop_all_inode_staged_writes(sih);
        nova_drop_all_inode_data_buffers(inode);
        nova_clear_pmem_reassign_pending(sih);
        nova_drop_all_defer_free_ranges(sih);
}

void nova_abort_deleted_inode_state(struct inode *inode)
{
        struct nova_inode_info_header *sih = &NOVA_I(inode)->header;

        /*
         * delete inode에서는 defer free ranges를 이미 처리했거나
         * 곧 처리해야 하므로 여기서 drop하면 안 된다.
         */
        nova_drop_all_inode_staged_writes(sih);
        nova_drop_all_inode_data_buffers(inode);
        nova_clear_pmem_reassign_pending(sih);
}

/* ===================== stage with merge ===================== */
int nova_stage_data_buffer(struct inode *inode,
                           unsigned long blocknr, unsigned long blk_cnt,
                           u8 *tmp_buf)
{
        struct nova_inode_info_header *sih = &NOVA_I(inode)->header;
        unsigned long done = 0;
        int ret = 0;

        if (!tmp_buf || blk_cnt == 0)
                return -EINVAL;

        while (done < blk_cnt) {
                unsigned long cur_blk = blocknr + done;
                u8 *cur_ptr = tmp_buf + (done << PAGE_SHIFT);
                unsigned long left_appended = 0;
                struct data_buf_node *left;

                /* 1) left 인접 node에 append 시도 */
                left = dbuf_prev_by_blk(sih, cur_blk);
                if (left && dbuf_end(left) == cur_blk) {
                        ret = dbuf_append_pages(inode->i_sb, sih, left, cur_ptr,
                                                blk_cnt - done,
                                                &left_appended);
                        if (ret)
                                goto err;

                        if (left_appended > 0) {
                                done += left_appended;
                                continue;
                        }
                }

                /* 2) 새 node 생성 */
                {
                        unsigned long chunk = blk_cnt - done;
                        struct data_buf_node *n;

                        if (chunk > NOVA_DBUF_MAX_PAGES)
                                chunk = NOVA_DBUF_MAX_PAGES;

                        n = kmalloc(sizeof(*n), GFP_NOFS);
                        if (!n) {
                                ret = -ENOMEM;
                                goto err;
                        }

                        n->blocknr = cur_blk;
                        n->blk_cnt = chunk;
                        INIT_LIST_HEAD(&n->list);

                        /*
                         * 최적화:
                         * - 남은 전체가 한 개 node로 들어가는 경우는 tmp_buf를 그대로 소유
                         * - 그 외에는 node private buffer를 새로 만들어 복사
                         */
                        if (done == 0 && chunk == blk_cnt) {
                                n->buf = tmp_buf;
                                tmp_buf = NULL;
                        } else {
                                n->buf = kmalloc(chunk << PAGE_SHIFT, GFP_NOFS);
                                if (!n->buf) {
                                        kfree(n);
                                        ret = -ENOMEM;
                                        goto err;
                                }

                                memcpy(n->buf,
                                       tmp_buf + (done << PAGE_SHIFT),
                                       chunk << PAGE_SHIFT);
                        }

                        ret = dbuf_rb_insert(sih, n);
                        if (ret) {
                                kfree(n->buf);
                                kfree(n);
                                goto err;
                        }

                        list_add_tail(&n->list, &sih->dbuf_list);
			dbuf_staged_inc(inode->i_sb, sih, chunk);
                        done += chunk;
                }
        }

        if (tmp_buf)
                kfree(tmp_buf);
        return 0;

err:
        if (tmp_buf)
                kfree(tmp_buf);
        return ret;
}

/* compute-only: full-block (offset=0, bytes=blocksize) 기준 */
#define CSUM0 NOVA_INIT_CSUM
static inline void nova_calc_block_csum_parity_full(struct super_block *sb,
        const u8 *block,
        size_t basis_bytes,
        __le32 *crc_out,      /* 4KB block당 1개 crc (4 bytes) */
        u8 *parity_out)       /* parity는 기존처럼 512 bytes */
{
        size_t blocksize = sb->s_blocksize;   /* 지금 가정: 4KB */
        size_t strp_size = NOVA_STRIPE_SIZE;  /* 512B */
        const u8 *p = block;
        u64 *par64 = (u64 *)parity_out;
        unsigned int i;

        if (crc_out) { //nova_should_use_dma_meta
//                u32 crc = nova_calc_crc_by_commit_ratio(sb, CSUM0, block,
//                                                         blocksize, ctrl);
#ifdef ONLY_USE_CPU_FOR_CHECKSUM
		inc_cpu_inflight(basis_bytes);
		inc_cpu_inflight_bytes(blocksize);
		atomic_inc(&cpu_inflight_csum);
                u32 crc = nova_crc32c(CSUM0, block, blocksize);
		dec_cpu_inflight_bytes(blocksize);
		dec_cpu_inflight(basis_bytes);
#elif defined(ONLY_USE_DSA_FOR_CHECKSUM)
        	u32 crc = nova_dma_crc32c_auto(sb, CSUM0, block, blocksize);
#elif defined(ADAPTIVE_FOR_CHECKSUM)
		bool use_dsa = pick_cpu_or_dsa_for_csum_bytes(basis_bytes);
		u32 crc;
		if (!use_dsa) {
			inc_cpu_inflight(basis_bytes);
			inc_cpu_inflight_bytes(blocksize);
			atomic_inc(&cpu_inflight_csum);
			crc = nova_crc32c(CSUM0, block, blocksize);
			dec_cpu_inflight_bytes(blocksize);
			dec_cpu_inflight(basis_bytes);
		} else {
			crc = nova_dma_crc32c_auto(sb, CSUM0, block, blocksize);
		}
#else
			printk(KERN_ERR "[%s] add commit.h\n", __func__);
#endif
                *crc_out = cpu_to_le32(crc);
        }

        if (parity_out) {
                for (i = 0; i < strp_size / sizeof(u64); i++) {
                        u64 q0 = *((u64 *)(p));
                        u64 q1 = *((u64 *)(p + 1 * strp_size));
                        u64 q2 = *((u64 *)(p + 2 * strp_size));
                        u64 q3 = *((u64 *)(p + 3 * strp_size));
                        u64 q4 = *((u64 *)(p + 4 * strp_size));
                        u64 q5 = *((u64 *)(p + 5 * strp_size));
                        u64 q6 = *((u64 *)(p + 6 * strp_size));
                        u64 q7 = *((u64 *)(p + 7 * strp_size));

                        par64[i] = q0 ^ q1 ^ q2 ^ q3 ^ q4 ^ q5 ^ q6 ^ q7;
                        p += sizeof(u64);
                }
        }
}

struct nova_pmem_seg {
        void *dst;
        const u8 *src;
        size_t len;
};

static inline void nova_seg_add_merge(struct nova_pmem_seg *segs, int *nr,
        void *dst, const u8 *src, size_t len)
{
        if (*nr > 0) {
                struct nova_pmem_seg *p = &segs[*nr - 1];
                char *p_dst_end = (char *)p->dst + p->len;
                const u8 *p_src_end = p->src + p->len;

                if (p_dst_end == (char *)dst && p_src_end == src) {
                        p->len += len;
                        return;
                }
        }
        segs[*nr].dst = dst;
        segs[*nr].src = src;
        segs[*nr].len = len;
        (*nr)++;
}

atomic_t cpu_cnt2, dsa_cnt2;
atomic_t low_2, mid_2, high_2, super_high_2;
atomic_t max_2;
static inline void nova_flush_segs(struct super_block *sb,
        struct nova_pmem_seg *segs, int nr,
        size_t basis_bytes,
        unsigned long *irq_flags)
{
        int i;

        for (i = 0; i < nr; i++) {
                bool use_dsa_for_meta;

#ifdef ONLY_USE_CPU_FOR_META
		use_dsa_for_meta = 0;
#elif defined(ONLY_USE_DSA_FOR_META)
		use_dsa_for_meta = 1;
#elif defined(ADAPTIVE_FOR_META)
		use_dsa_for_meta = pick_cpu_or_dsa_for_meta(basis_bytes);
#else
		printk(KERN_ERR "[%s] add commit.h\n", __func__);
#endif

                nova_memunlock_range(sb, segs[i].dst, segs[i].len, irq_flags);

                if (!use_dsa_for_meta) {
			inc_cpu_inflight(basis_bytes);
			inc_cpu_inflight_bytes(segs[i].len);
			atomic_inc(&cpu_inflight_meta);
                        memcpy_to_pmem_nocache(segs[i].dst, segs[i].src,
                                               segs[i].len);
			dec_cpu_inflight_bytes(segs[i].len);
			dec_cpu_inflight(basis_bytes);
		} else {
                        nova_dma_memcpy(segs[i].dst, segs[i].src,
                                        segs[i].len);
		}

                nova_memlock_range(sb, segs[i].dst, segs[i].len, irq_flags);
        }
}

KTDEF(crc_parity_alloc_2);
KTDEF(crc_parity_calc_2);
KTDEF(crc_parity_flush_2);
KTDEF(tmp_cnt2);
extern atomic_t tmp_cnt;
static int nova_flush_node_csum_parity_batch(struct super_block *sb,
        struct inode *inode,
        struct data_buf_node *n,
        const u8 *src_base)
{
        struct nova_inode_info_header *sih = &NOVA_I(inode)->header;
        size_t csum_bytes = NOVA_BLOCK_CSUM_SIZE;   /* 4 */
        size_t strp_size = NOVA_STRIPE_SIZE;        /* 512 */

        u8 *crc_buf = NULL;
        u8 *parity_buf = NULL;

        struct nova_pmem_seg *csum0 = NULL, *csum1 = NULL, *par = NULL;
        int nr0 = 0, nr1 = 0, nrp = 0;

        unsigned long irq_flags = 0;
        unsigned int blocksize_bits = sb->s_blocksize_bits;
        unsigned long i;
        int ret = 0;

        if (data_csum > 0) {
                crc_buf = kmalloc(n->blk_cnt * csum_bytes, GFP_NOFS);
                csum0 = kmalloc_array(n->blk_cnt, sizeof(*csum0), GFP_NOFS);
                csum1 = kmalloc_array(n->blk_cnt, sizeof(*csum1), GFP_NOFS);
                if (!crc_buf || !csum0 || !csum1) {
                        ret = -ENOMEM;
                        goto out;
                }
        }

        if (data_parity > 0) {
                parity_buf = kmalloc(n->blk_cnt * strp_size, GFP_NOFS);
                par = kmalloc_array(n->blk_cnt, sizeof(*par), GFP_NOFS);
                if (!parity_buf || !par) {
                        ret = -ENOMEM;
                        goto out;
                }
        }

        for (i = 0; i < n->blk_cnt; i++) {
                unsigned long blk = n->blocknr + i;
                const u8 *blk_src = src_base + (i << blocksize_bits);

                __le32 *crc = NULL;
                u8 *parity = NULL;

                if (data_csum > 0)
                        crc = (__le32 *)(crc_buf + i * csum_bytes);
                if (data_parity > 0)
                        parity = parity_buf + i * strp_size;

                nova_calc_block_csum_parity_full(sb, blk_src, n->blk_cnt * sb->s_blocksize, crc, parity);

                if (data_csum > 0) {
                        void *dst0 = nova_get_data_block_csum_addr(sb, sih, blk, 0);
                        void *dst1 = nova_get_data_block_csum_addr(sb, sih, blk, 1);

                        nova_seg_add_merge(csum0, &nr0, dst0,
                                (const u8 *)crc, csum_bytes);
                        nova_seg_add_merge(csum1, &nr1, dst1,
                                (const u8 *)crc, csum_bytes);
                }

                if (data_parity > 0) {
                        void *dstp = nova_get_parity_addr(sb, blk);

                        nova_seg_add_merge(par, &nrp, dstp,
                                (const u8 *)parity, strp_size);
                }
        }

        if (data_csum > 0) {
                nova_flush_segs(sb, csum0, nr0, n->blk_cnt * sb->s_blocksize, &irq_flags);
                nova_flush_segs(sb, csum1, nr1, n->blk_cnt * sb->s_blocksize, &irq_flags);
        }

        if (data_parity > 0)
                nova_flush_segs(sb, par, nrp, n->blk_cnt * sb->s_blocksize, &irq_flags);

out:
        kfree(csum0);
        kfree(csum1);
        kfree(par);
        kfree(crc_buf);
        kfree(parity_buf);
        return ret;
}

/* flush는 기존처럼 rb 순회로 해도 되고 list 순회로 해도 됨 */
KTDEF(memcpy_to_pmem_nocache_dbuf);
KTDEF(tmp_dbuf);
KTDEF(dbuf_1);
atomic_t tmp_dbuf;
atomic_t node_blk1, node_blk1024, node_blk_else;
atomic_t cpu_cnt, dsa_cnt;
int flush_data_buffers(struct super_block *sb, struct inode *inode)
{
	ktime_t stopwatch[2];
	struct nova_inode_info_header *sih = &NOVA_I(inode)->header;
	struct rb_node *node;
	unsigned long irq_flags = 0;
	size_t blocksize = sb->s_blocksize;

	node = rb_first(&sih->dbuf_rb);
	while (node) {
		struct data_buf_node *n =
			rb_entry(node, struct data_buf_node, rb);
		struct rb_node *next = rb_next(node);

		void *pmem = nova_get_block(sb,
			nova_get_block_off(sb, n->blocknr, sih->i_blk_type));

		u8 *src = n->buf;
		size_t total = n->blk_cnt * blocksize;
		bool use_dsa_for_data;

#ifdef ONLY_USE_CPU_FOR_DATA
		use_dsa_for_data = 0;
#elif defined(ONLY_USE_DSA_FOR_DATA)
		use_dsa_for_data = 1;
#elif defined(ADAPTIVE_FOR_DATA)
		use_dsa_for_data = pick_cpu_or_dsa_for_data(total);
#else
		printk(KERN_ERR "[%s] add commit.h!!!!\n", __func__);
#endif

		nova_memunlock_range(sb, pmem, total, &irq_flags);

		ktget(&stopwatch[0]);
		if (!use_dsa_for_data) {
			inc_cpu_inflight(total);
			inc_cpu_inflight_bytes(total);
			atomic_inc(&cpu_inflight_data);
			memcpy_to_pmem_nocache(pmem, src, total);
			dec_cpu_inflight_bytes(total);
			dec_cpu_inflight(total);
		} else {
			nova_dma_memcpy(pmem, src, total);
		}
		ktget(&stopwatch[1]);
		ktput(stopwatch, memcpy_to_pmem_nocache_dbuf);

		nova_memlock_range(sb, pmem, total, &irq_flags);

		ktget(&stopwatch[0]);
		if (data_csum > 0 || data_parity > 0) {
//			for (i = 0; i < n->blk_cnt; i++) {
//				u8 *blk = src + (i << blocksize_bits);
//				nova_update_block_csum_parity(sb, sih, blk,
//					n->blocknr + i, 0, blocksize);
//			}
			int r = nova_flush_node_csum_parity_batch(sb, inode, n, src);
			if (r) {
				/* 실패 처리 정책: 여기서는 그냥 에러 리턴 */
				return r;
			}
		}
		ktget(&stopwatch[1]);
		ktput(stopwatch, dbuf_1);

		dbuf_remove_node(sb, sih, n);
		node = next;
	}

	return 0;
}

KTDEF(kmalloc_21);
KTDEF(new_if_2);
KTDEF(copy_from_user_21);

int copy_buf_from_user(struct super_block *sb,
        struct inode *inode, unsigned long start_blk,
        size_t offset, size_t bytes, const char __user *buf,
        u8 **tmp_buf_out, unsigned long *blk_cnt_out)
{
	ktime_t stopwatch[2];
        u8 *tmp_buf;
        size_t blocksize = sb->s_blocksize;
        unsigned int blocksize_bits = sb->s_blocksize_bits;
        unsigned long blk_cnt;
        size_t end_off;
        int ret = 0;

        blk_cnt = ((offset + bytes - 1) >> blocksize_bits) + 1;

        ktget(&stopwatch[0]);
        tmp_buf = kmalloc(blk_cnt * blocksize, GFP_KERNEL);
        ktget(&stopwatch[1]);
        ktput(stopwatch, kmalloc_21);

        if (unlikely(!tmp_buf))
                return -ENOMEM;

        /* 1) blk_cnt == 1: head+tail이 같은 블록 */
        if (blk_cnt == 1) {
                bool full = (offset == 0 && bytes == blocksize);

                ktget(&stopwatch[0]);
                if (!full) {
                        ret = load_old_block_or_zero(sb, inode, start_blk,
                                                     tmp_buf, offset, bytes);
                        if (ret) {
                                ktget(&stopwatch[1]);
                                ktput(stopwatch, new_if_2);
                                goto out_free;
                        }
                }
                ktget(&stopwatch[1]);
                ktput(stopwatch, new_if_2);

		goto copy_and_out;
        }

        /* 2) blk_cnt >= 2: head/tail만 partial 가능 */

        /* head partial? (첫 블록이 중간부터 시작하면 보존 필요) */
        if (offset != 0) {
                size_t head_write = blocksize - offset; /* 첫 블록에서 덮는 길이 */

                ktget(&stopwatch[0]);
                ret = load_old_block_or_zero(sb, inode, start_blk,
                                             tmp_buf, offset, head_write);
                ktget(&stopwatch[1]);
                ktput(stopwatch, new_if_2);

                if (ret)
                        goto out_free;
        }

        /* tail partial? (마지막 블록이 끝까지 안 덮이면 보존 필요) */
        end_off = (offset + bytes) & (blocksize - 1);
        if (end_off != 0) {
                u8 *tail_blk = tmp_buf + ((blk_cnt - 1) << blocksize_bits);

                ktget(&stopwatch[0]);
                ret = load_old_block_or_zero(sb, inode, start_blk + (blk_cnt - 1),
                                             tail_blk, 0, end_off);
                ktget(&stopwatch[1]);
                ktput(stopwatch, new_if_2);

                if (ret)
                        goto out_free;
        }

copy_and_out:
        /* 3) user -> tmp_buf는 한 번에 (연속 범위) */
        ktget(&stopwatch[0]);
        if (unlikely(copy_from_user(tmp_buf + offset, buf, bytes) != 0)) {
                ret = -EFAULT;
                ktget(&stopwatch[1]);
                ktput(stopwatch, copy_from_user_21);
                goto out_free;
        }
        ktget(&stopwatch[1]);
        ktput(stopwatch, copy_from_user_21);

        *tmp_buf_out = tmp_buf;
        *blk_cnt_out = blk_cnt;
        return 0;

out_free:
        kfree(tmp_buf);
        return ret;
}

int nova_free_blocks(struct super_block *sb, unsigned long blocknr,
		int num, unsigned short btype, int log_page);


inline bool nova_defer_free_empty(struct super_block *sb,
                                         struct nova_inode_info_header *sih)
{
    unsigned int i, nr;

    if (!sih || !sih->defer_tree)
        return true;

    nr = NOVA_SB(sb)->cpus;   /* dfb를 만들 때 쓴 값과 동일해야 함 */

    for (i = 0; i < nr; i++) {
        if (READ_ONCE(sih->defer_tree[i].root.rb_node))
            return false;
    }
    return true;
}


void nova_destroy_defer_buckets(struct super_block *sb,
		struct nova_inode_info_header *sih)
{
	unsigned int i;

	if (!sih || !sih->defer_tree || sih->nr_defer == 0)
		return;

	for (i = 0; i < sih->nr_defer; i++) {
		struct rb_root local_root;
		struct rb_node *node;

		/* bucket i tree를 통째로 local로 옮기고 bucket 비움 */
		spin_lock(&sih->defer_tree[i].lock);
		local_root = sih->defer_tree[i].root;
		sih->defer_tree[i].root = RB_ROOT;
		spin_unlock(&sih->defer_tree[i].lock);

		/* lock 없이 local_root 비우기 */
		node = rb_first(&local_root);
		while (node) {
			struct defer_free_range *r;
			struct rb_node *next;
			unsigned long blocknr;
			unsigned int num;

			r = rb_entry(node, struct defer_free_range, node);
			next = rb_next(node);

			rb_erase(&r->node, &local_root);

			/* 남아있던 range는 여기서도 안전하게 free 처리 */
			blocknr = r->range_low;
			num = (unsigned int)(r->range_high - r->range_low + 1);
			nova_free_blocks(sb, blocknr, num, r->btype, r->log_page);

			kfree(r);
			node = next;
		}
	}

	kfree(sih->defer_tree);
	sih->defer_tree = NULL;
	sih->nr_defer = 0;
}


static struct defer_free_range *
defer_find_prev(struct rb_root *root, unsigned long low)
{
	struct rb_node *n = root->rb_node;
	struct defer_free_range *prev = NULL;

	while (n) {
		struct defer_free_range *cur = rb_entry(n, typeof(*cur), node);
		if (cur->range_low < low) {
			prev = cur;
			n = n->rb_right;
		} else {
			n = n->rb_left;
		}
	}
	return prev;
}

static struct defer_free_range *
defer_find_next(struct rb_root *root, unsigned long low)
{
	struct rb_node *n = root->rb_node;
	struct defer_free_range *next = NULL;

	while (n) {
		struct defer_free_range *cur = rb_entry(n, typeof(*cur), node);
		if (cur->range_low >= low) {
			next = cur;
			n = n->rb_left;
		} else {
			n = n->rb_right;
		}
	}
	return next;
}

static int defer_insert_range(struct defer_rb_tree *tree,
		unsigned long low, unsigned long high,
		unsigned short btype, int log_page)
{
	struct rb_root *root = &tree->root;
	struct defer_free_range *prev, *next, *r;
	struct rb_node **link, *parent = NULL;

	/* 1) 인접 후보 찾기 */
	prev = defer_find_prev(root, low);
	next = defer_find_next(root, low);

	/* 2) prev와 병합 가능? (겹치거나 붙거나) */
	if (prev && prev->btype == btype && prev->log_page == log_page &&
			prev->range_high + 1 >= low) {
		if (high > prev->range_high)
			prev->range_high = high;

		/* prev 확장 후 next도 연쇄 병합 */
		while (next && next->btype == btype && next->log_page == log_page &&
				prev->range_high + 1 >= next->range_low) {
			if (next->range_high > prev->range_high)
				prev->range_high = next->range_high;

			/* next 삭제 */
			{
				struct rb_node *tmp = rb_next(&next->node);
				rb_erase(&next->node, root);
				kfree(next);
				next = tmp ? rb_entry(tmp, typeof(*next), node) : NULL;
			}
		}
		return 0;
	}

	/* 3) next와 병합 가능? (새 range가 next 앞에 붙거나 겹침) */
	if (next && next->btype == btype && next->log_page == log_page &&
			high + 1 >= next->range_low) {
		if (low < next->range_low)
			next->range_low = low;
		if (high > next->range_high)
			next->range_high = high;
		return 0;
	}

	/* 4) 병합 불가 -> 새 노드 삽입 */
	r = kmalloc(sizeof(*r), GFP_NOFS);
	if (!r)
		return -ENOMEM;

	r->range_low = low;
	r->range_high = high;
	r->btype = btype;
	r->log_page = log_page;

	link = &root->rb_node;
	while (*link) {
		struct defer_free_range *cur;
		parent = *link;
		cur = rb_entry(parent, typeof(*cur), node);
		if (r->range_low < cur->range_low)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&r->node, parent, link);
	rb_insert_color(&r->node, root);
	return 0;
}


inline int nova_invalidate_write_entry(struct super_block *sb,
		struct nova_file_write_entry *entry, int reassign,
		unsigned int num_free, u8 e_type);

static void enqueue_defer_rb_tree(struct super_block *sb,
		struct nova_inode_info_header *sih, unsigned long blocknr, unsigned int num,
		unsigned short btype, int log_page)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	unsigned long per = sbi->per_list_blocks;

	while (num) {
		unsigned long bucket = blocknr / per;
		unsigned long bucket_end = (bucket + 1) * per;
		unsigned int n = num;

		if (blocknr + n > bucket_end)
			n = (unsigned int)(bucket_end - blocknr);

		if (bucket >= sih->nr_defer) {
			pr_err("[%s] bucket OOB\n", __func__);
			return;
		}

		spin_lock(&sih->defer_tree[bucket].lock);
		defer_insert_range(&sih->defer_tree[bucket], blocknr, blocknr + n - 1, btype, log_page);
		spin_unlock(&sih->defer_tree[bucket].lock);

		blocknr += n;
		num -= n;
	}
}


/* PMEM 엔트리에 대해 overwrite된 페이지 수를 누적 기록 */
static int insert_pmem_reassign_list(struct super_block *sb,
		struct nova_inode_info_header *sih,
		struct nova_file_write_entry *pmem_entry,
		u32 invalid_add)
{
	struct pmem_reassign_node *pr_node;
	unsigned long key;

	if (!pmem_entry || invalid_add == 0)
		return 0;
	if (!nova_is_pmem_ptr(sb, pmem_entry))
		return 0;

	key = (unsigned long)pmem_entry;

	spin_lock(&sih->pmem_reassign_lock);

	pr_node = xa_load(&sih->pmem_reassign_xa, key);
	if (pr_node) {
		if (!pr_node->inmem_reassign)
			printk(KERN_ALERT "[%s] bug\n", __func__);
		pr_node->invalid_add += invalid_add;
		spin_unlock(&sih->pmem_reassign_lock);
		return 0;
	}

	spin_unlock(&sih->pmem_reassign_lock);

	pr_node = kmalloc(sizeof(*pr_node), GFP_NOFS);
	if (!pr_node)
		return -ENOMEM;

	INIT_LIST_HEAD(&pr_node->list);
	pr_node->pmem_entry = pmem_entry;
	pr_node->invalid_add = invalid_add;
	pr_node->inmem_reassign = true;

	spin_lock(&sih->pmem_reassign_lock);

	xa_store(&sih->pmem_reassign_xa, key, pr_node, GFP_ATOMIC);
	list_add_tail(&pr_node->list, &sih->pmem_reassign_list);

	spin_unlock(&sih->pmem_reassign_lock);
	return 0;
}

/* read에서: entry->reassigned(영속 필드) + (PMEM pending update)까지 고려 */
inline bool nova_runtime_reassigned(struct super_block *sb,
		struct nova_inode_info_header *sih,
		struct nova_file_write_entry *entry)
{
	struct pmem_reassign_node *pr_node;

	if (!entry)
		return false;

	/* PMEM에 이미 reassigned 찍힌 경우 */
	if (READ_ONCE(entry->reassigned))
		return true;

	if (!nova_is_pmem_ptr(sb, entry))
		return false;

	/* pending 조회: 리스트 순회 대신 xarray */
	rcu_read_lock();
	pr_node = xa_load(&sih->pmem_reassign_xa, (unsigned long)entry);
	rcu_read_unlock();
	if (pr_node && pr_node->inmem_reassign)
		return true;

	return false;
}


bool nova_verify_entry_csum(struct super_block *sb, void *entry, void *entryc, u8 e_type);

/* fsync에서 PMEM old entry에 invalid_pages/reassigned를 원본 NOVA 방식으로 반영 */
/* fsync에서 PMEM old entry에 invalid_pages/reassigned를 원본 NOVA 방식으로 반영
 * - clamp 없음: invalid_add를 그대로 적용
 * - after-check: 적용 후 num/invalid를 읽어 검증
 * - invalid > num 발생 시 즉시 에러 처리
 */
KTDEF(nova_invalidate_write_entry_41);
KTDEF(pmem_reassign_lock);
KTDEF(pmem_reassign_lock_2);
static int invalidate_pr_node(struct super_block *sb,
                              struct nova_inode_info_header *sih)
{
	ktime_t stopwatch[2];
	LIST_HEAD(local);
	struct pmem_reassign_node *pr_node, *tmp;
	int ret = 0;

	ktget(&stopwatch[0]);
	spin_lock(&sih->pmem_reassign_lock);
	ktget(&stopwatch[1]);
	ktput(stopwatch, pmem_reassign_lock);
	list_splice_init(&sih->pmem_reassign_list, &local);
	spin_unlock(&sih->pmem_reassign_lock);

	list_for_each_entry_safe(pr_node, tmp, &local, list) {
		struct nova_file_write_entry *entry = pr_node->pmem_entry;
		u32 num, invalid;

		if (!entry || !nova_is_pmem_ptr(sb, entry)) {
			/* 잘못된 노드는 그냥 버림 */
			xa_erase(&sih->pmem_reassign_xa, (unsigned long)entry);
			list_del(&pr_node->list);
			kfree(pr_node);
			printk(KERN_ERR "[%s] need check\n", __func__);
			continue;
		}

		ktget(&stopwatch[0]);
		ret = nova_invalidate_write_entry(sb, entry, 1, pr_node->invalid_add, FILE_WRITE);
		ktget(&stopwatch[1]);
		ktput(stopwatch, nova_invalidate_write_entry_41);
		if (ret)
			break;

		num     = le32_to_cpu(entry->num_pages);
		invalid = le32_to_cpu(entry->invalid_pages);

		if (unlikely(num == 0)) {
			pr_err("[%s] after-check num_pages==0 entry=%px\n", __func__, entry);
			ret = -EIO;
			break;
		}

		if (unlikely(invalid > num)) {
			/* 여기 오면 상태가 이미 깨진 것: 즉시 실패 처리 */
			pr_err("[%s] CORRUPTION: invalid(%u) > num(%u) entry=%px add=%u\n",
					__func__, invalid, num, entry, pr_node->invalid_add);
			ret = -EIO;   /* 또는 -EIO */
			break;
		}

		/* 3) 정상 처리된 pr_node는 항상 정리 (중복 방지 핵심) */
		ktget(&stopwatch[0]);
		spin_lock(&sih->pmem_reassign_lock);
		ktget(&stopwatch[1]);
		ktput(stopwatch, pmem_reassign_lock_2);

		xa_erase(&sih->pmem_reassign_xa, (unsigned long)entry);

		list_del(&pr_node->list);
		spin_unlock(&sih->pmem_reassign_lock);
		kfree(pr_node);
	}

	if (ret) {
		/* 실패: 아직 처리 안 된(pr_node 포함) 것들을 다시 되돌려 재시도 가능하게 */
		printk("[%s] faile\n", __func__);
		spin_lock(&sih->pmem_reassign_lock);
		list_splice(&local, &sih->pmem_reassign_list);
		spin_unlock(&sih->pmem_reassign_lock);
		return ret;
	}

	return 0;
}


static int old_entry_reassign(struct super_block *sb,
                struct nova_inode_info_header *sih,
                struct nova_file_write_entry *old,
                unsigned long old_pgoff,
                unsigned int old_len)
{
        int freed = 0;

        /*
         * overwrite/truncate에서 old run을 밀어낼 때는
         * inode는 아직 살아 있으므로 delete_dead=false
         */
        return nova_finalize_delete_run(sb, sih, old, old_pgoff, old_len,
                                        false, 0, &freed);
}

int nova_assign_write_entry_staged(struct super_block *sb,
		struct nova_inode_info_header *sih,
		struct nova_file_write_entry *new_entry)
{
	unsigned long new_start = (unsigned long)le64_to_cpu(new_entry->pgoff);
	unsigned int  new_num   = (unsigned int)le32_to_cpu(new_entry->num_pages);
	unsigned int i;
	int ret = 0;

	/* old run tracking (original nova 스타일) */
	struct nova_file_write_entry *start_old_entry = NULL;
	unsigned long old_pgoff = 0;
	unsigned int old_len = 0;

	for (i = 0; i < new_num; i++) {
		unsigned long p = new_start + i;
		void **slot = radix_tree_lookup_slot(&sih->tree, p);
		struct nova_file_write_entry *old = NULL;

		if (slot)
			old = radix_tree_deref_slot(slot);

		/* 자기 자신이면 old 처리 X */
		if (old == new_entry) { //안타는 듯 없어도될듯?
			printk(KERN_ALERT "[%s] need check1\n", __func__);
			old = NULL;
		}

		/* run이 바뀌면 이전 run finalize */
		if (old != start_old_entry) {
			ret = old_entry_reassign(sb, sih, start_old_entry, old_pgoff, old_len);
			if (ret)
				return ret;

			start_old_entry = old;
			old_pgoff = p;
			old_len = (old ? 1 : 0);
		} else {
			if (old)
				old_len++;
		}

		/* radix tree를 new_entry로 설치 */
		if (slot) {
			radix_tree_replace_slot(&sih->tree, slot, new_entry);
		} else {
			ret = radix_tree_insert(&sih->tree, p, new_entry);
			if (ret)
				return ret;
		}
	}

	/* 마지막 run 처리 */
	ret = old_entry_reassign(sb, sih, start_old_entry, old_pgoff, old_len);
	return ret;
}


u32 nova_calc_entry_csum(void *entry, u8 e_type);
u32 my_nova_calc_entry_csum(struct super_block *sb, void *entry, u8 e_type);

	struct nova_staged_write_node *
nova_stage_file_write(struct inode *inode, struct nova_file_write_entry *data)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_staged_write_node *n;

	n = kmalloc(sizeof(*n), GFP_NOFS);
	if (!n)
		return NULL;

	INIT_LIST_HEAD(&n->list);
	memcpy(&n->e, data, sizeof(*data));
	n->pmem_curr_p = 0;
	n->pmem_alter_p = 0;
	//	n->dead = 0;

	list_add_tail(&n->list, &sih->staged_list);
	//	atomic64_inc(&sih->staged_cnt);

	return n;
}



int nova_process_deferred_free_ranges(struct super_block *sb,
                struct nova_inode_info_header *sih)
{
    unsigned int i;

    if (!sih || !sih->defer_tree || sih->nr_defer == 0)
        return 0;

    for (i = 0; i < sih->nr_defer; i++) {
        struct rb_root local_root;
        struct rb_node *node;

        /*
         * bucket i의 rb-tree를 통째로 local_root로 옮겨오고,
         * bucket은 비워둔다. (lock은 짧게)
         */
        spin_lock(&sih->defer_tree[i].lock);
        local_root = sih->defer_tree[i].root;
        sih->defer_tree[i].root = RB_ROOT;
        spin_unlock(&sih->defer_tree[i].lock);

        /*
         * lock 없이 local_root를 순회하며 free 수행
         * (range_low/high는 inclusive)
         */
        node = rb_first(&local_root);
        while (node) {
            struct defer_free_range *r;
            struct rb_node *next;
            unsigned long blocknr;
            unsigned int num;

            r = rb_entry(node, struct defer_free_range, node);
            next = rb_next(node);

            rb_erase(&r->node, &local_root);

            blocknr = r->range_low;
            num = (unsigned int)(r->range_high - r->range_low + 1);

            nova_free_blocks(sb, blocknr, num, r->btype, r->log_page);

            kfree(r);
            node = next;
        }
    }

    return 0;
}


struct nova_commit_write_node {
	struct list_head list;

	struct nova_staged_write_node *staged;

	unsigned long commit_pgoff;
	unsigned int commit_len;

	u64 curr_p;
	u64 alter_p;

	struct nova_file_write_entry e;
};

static struct log_page_batch *find_log_batch(struct list_head *batch_list,
        u64 block_off)
{
        struct log_page_batch *b;

        list_for_each_entry(b, batch_list, list) {
                if (b->block_off == block_off)
                        return b;
        }

        return NULL;
}

static struct nova_inode_log_page *commit_get_log_page(struct super_block *sb,
        struct list_head *batch_list, u64 block_off, bool *shadow)
{
        struct log_page_batch *b;

        b = find_log_batch(batch_list, BLOCK_OFF(block_off));
        if (b) {
                if (shadow)
                        *shadow = true;
                return (struct nova_inode_log_page *)b->page_buf;
        }

        if (shadow)
                *shadow = false;
        return (struct nova_inode_log_page *)nova_get_block(sb, BLOCK_OFF(block_off));
}

static u64 commit_next_log_page(struct super_block *sb,
        struct list_head *batch_list, u64 curr)
{
        struct nova_inode_log_page *page;

        page = commit_get_log_page(sb, batch_list, curr, NULL);
        return page->page_tail.next_page;
}

static void commit_set_next_page_address(struct super_block *sb,
        struct list_head *batch_list, u64 curr, u64 next_page, int fence)
{
        struct nova_inode_log_page *page;
        bool shadow;

        page = commit_get_log_page(sb, batch_list, curr, &shadow);
        if (shadow) {
                page->page_tail.next_page = next_page;
                return;
        }

        nova_set_next_page_address(sb, page, next_page, fence);
}

static void commit_set_next_page_flag(struct super_block *sb,
        struct list_head *batch_list, u64 curr_p)
{
        struct log_page_batch *b;
        void *entry;

        if (ENTRY_LOC(curr_p) >= LOG_BLOCK_TAIL)
                return;

        b = find_log_batch(batch_list, BLOCK_OFF(curr_p));
        if (b) {
                entry = b->page_buf + ENTRY_LOC(curr_p);
                nova_set_entry_type(entry, NEXT_PAGE);
                return;
        }

        nova_set_next_page_flag(sb, curr_p);
}

static void commit_set_alter_page_one(struct super_block *sb,
        struct list_head *batch_list, u64 curr, u64 alter_curr)
{
        struct nova_inode_log_page *page;
        bool shadow;

        page = commit_get_log_page(sb, batch_list, curr, &shadow);
        page->page_tail.alter_page = alter_curr;

        if (!shadow)
                nova_flush_buffer(&page->page_tail,
                        sizeof(struct nova_inode_page_tail), 0);
}

static void commit_set_alter_page_address(struct super_block *sb,
        struct list_head *main_batches, struct list_head *alter_batches,
        u64 curr, u64 alter_curr)
{
        if (metadata_csum == 0)
                return;

        commit_set_alter_page_one(sb, main_batches, curr, alter_curr);
        commit_set_alter_page_one(sb, alter_batches, alter_curr, curr);
}

static void commit_update_alter_pages(struct super_block *sb,
        struct list_head *main_batches, struct list_head *alter_batches,
        u64 curr, u64 alter_curr)
{
        if (curr == 0 || alter_curr == 0 || metadata_csum == 0)
                return;

        while (curr && alter_curr) {
                commit_set_alter_page_address(sb, main_batches, alter_batches,
                        curr, alter_curr);
                curr = commit_next_log_page(sb, main_batches, curr);
                alter_curr = commit_next_log_page(sb, alter_batches, alter_curr);
        }

        if (curr || alter_curr)
                nova_dbg("%s: curr 0x%llx, alter_curr 0x%llx\n",
                        __func__, curr, alter_curr);
}

static u64 commit_extend_inode_log(struct super_block *sb,
        struct nova_inode *pi, struct nova_inode_info_header *sih,
        u64 curr_p, u64 alter_tail, struct list_head *main_batches,
        struct list_head *alter_batches, int *extended)
{
        u64 new_block, alter_new_block = 0;
        unsigned long num_pages;
        int allocated_main;
        int allocated_alter = 0;
        int target_cpu;
        int num_logs = metadata_csum ? 2 : 1;

        if (curr_p == 0)
                return nova_get_append_head(sb, pi, sih, curr_p,
                        sizeof(struct nova_file_write_entry), MAIN_LOG,
                        0, extended);

        num_pages = sih->log_pages >= EXTEND_THRESHOLD ?
                        EXTEND_THRESHOLD : sih->log_pages;
        target_cpu = sih->ino % NOVA_SB(sb)->cpus;

        allocated_main = nova_allocate_inode_log_pages(sb, sih, num_pages,
                        &new_block, target_cpu, 0);
        if (allocated_main <= 0) {
                nova_err(sb, "%s ERROR: no inode log page available\n",
                        __func__);
                return 0;
        }

        if (metadata_csum) {
                allocated_alter = nova_allocate_inode_log_pages(sb, sih,
                                num_pages, &alter_new_block, target_cpu, 1);
                if (allocated_alter <= 0) {
                        nova_err(sb, "%s ERROR: no alter log page available\n",
                                __func__);
                        return 0;
                }

                commit_update_alter_pages(sb, main_batches, alter_batches,
                        new_block, alter_new_block);
        }

        commit_set_next_page_address(sb, main_batches, curr_p, new_block, 1);

        if (metadata_csum) {
                u64 alter_connect = alter_tail ? alter_tail : sih->alter_log_tail;

                commit_set_next_page_address(sb, alter_batches,
                        alter_connect, alter_new_block, 1);
        }

        sih->fast_gc_pending = 1;
        sih->log_pages += allocated_main * num_logs;

        if (extended)
                *extended = 1;

        return new_block;
}

static u64 commit_get_append_head(struct super_block *sb, struct nova_inode *pi,
        struct nova_inode_info_header *sih, u64 tail, u64 alter_tail,
        size_t size, int log_id, struct list_head *main_batches,
        struct list_head *alter_batches, int *extended)
{
        struct list_head *batch_list;
        u64 curr_p;
        u64 next;

        batch_list = (log_id == MAIN_LOG) ? main_batches : alter_batches;
        curr_p = tail;

        if (!curr_p) {
                curr_p = nova_get_append_head(sb, pi, sih, tail, size,
                        log_id, 0, extended);
                return curr_p;
        }

        next = commit_next_log_page(sb, batch_list, curr_p);

        if (is_last_entry(curr_p, size) && next == 0) {
                commit_set_next_page_flag(sb, batch_list, curr_p);

                if (log_id != MAIN_LOG)
                        return 0;

                curr_p = commit_extend_inode_log(sb, pi, sih, curr_p,
                        alter_tail, main_batches, alter_batches, extended);
                if (!curr_p)
                        return 0;
        }

        if (is_last_entry(curr_p, size)) {
                commit_set_next_page_flag(sb, batch_list, curr_p);
                curr_p = commit_next_log_page(sb, batch_list, curr_p);
        }

        return curr_p;
}




KTDEF(nova_calc_entry_csum_31);
KTDEF(memcpy_to_pmem_nocache_31);
KTDEF(nova_flush_buffer_31);
KTDEF(nova_update_inode_31);
KTDEF(nova_update_alter_inode_31);
KTDEF(commit_tmp_1);
KTDEF(commit_tmp_2);
KTDEF(commit_tmp_3);
KTDEF(flush_data_buffers);
int commit_process(struct super_block *sb, struct inode *inode)
{
        ktime_t stopwatch[2];
        struct nova_sb_info *sbi = NOVA_SB(sb);
        struct nova_inode_info *si = NOVA_I(inode);
        struct nova_inode_info_header *sih = &si->header;
        struct nova_inode *pi;
        LIST_HEAD(local_staged);
        LIST_HEAD(main_batches);
        LIST_HEAD(alter_batches);
        struct nova_staged_write_node *staged, *staged_tmp;
        u64 tail, alter_tail;
        size_t sz = sizeof(struct nova_file_write_entry);
        unsigned long irq_flags = 0;
        int ret = 0;
//        struct cpu_dsa_ctrl ctrl;

//        cpu_dsa_ctrl_init(&ctrl);

        /* staged_list -> local */
        list_splice_init(&sih->staged_list, &local_staged);

        pi = nova_get_block(sb, sih->pi_addr);
        tail = sih->log_tail;
        alter_tail = sih->alter_log_tail;

        /* 0) data buffer 먼저 PMEM에 flush */
        ktget(&stopwatch[0]);
        ret = flush_data_buffers(sb, inode);
        ktget(&stopwatch[1]);
        ktput(stopwatch, flush_data_buffers);
        if (ret)
                goto fail;

        ktget(&stopwatch[0]);
        list_for_each_entry(staged, &local_staged, list) {
                u64 curr_p, alter_p = 0;
                int extended = 0;

                if (metadata_csum) {
                        u32 csum = my_nova_calc_entry_csum(sb, &staged->e, FILE_WRITE);
                        staged->e.csum = cpu_to_le32(csum);
                }

//                curr_p = nova_get_append_head(sb, pi, sih, tail, sz,
//                                             MAIN_LOG, 0, &extended);
                curr_p = commit_get_append_head(sb, pi, sih, tail, alter_tail, sz,
				MAIN_LOG, &main_batches, &alter_batches, &extended);

                if (!curr_p) {
                        ret = -ENOSPC;
                        goto fail;
                }

                staged->pmem_curr_p = curr_p;
                ret = from_stage_we_to_shadow(sb, &main_batches,
                                              curr_p, &staged->e);
                if (ret)
                        goto fail;

                tail = curr_p + sz;

                if (metadata_csum) {
//                        alter_p = nova_get_append_head(sb, pi, sih, alter_tail, sz,
//                                                       ALTER_LOG, 0, &extended);
			alter_p = commit_get_append_head(sb, pi, sih, alter_tail, tail, sz,
					ALTER_LOG, &main_batches, &alter_batches, &extended);

                        if (!alter_p) {
                                ret = -ENOSPC;
                                goto fail;
                        }

                        staged->pmem_alter_p = alter_p;
                        ret = from_stage_we_to_shadow(sb, &alter_batches,
                                                      alter_p, &staged->e);
                        if (ret)
                                goto fail;

                        alter_tail = alter_p + sz;
                }
        }
        ktget(&stopwatch[1]);
        ktput(stopwatch, commit_tmp_1);

        ktget(&stopwatch[0]);
        ret = flush_shadow_pages(sb, &main_batches);
        if (ret)
                goto fail;

        if (metadata_csum) {
                ret = flush_shadow_pages(sb, &alter_batches);
                if (ret)
                        goto fail;
        }
        ktget(&stopwatch[1]);
        ktput(stopwatch, commit_tmp_2);

        PERSISTENT_BARRIER();

        {
                struct nova_inode_update upd;

                memset(&upd, 0, sizeof(upd));
                upd.tail = tail;
                upd.alter_tail = alter_tail;

                nova_memunlock_inode(sb, pi, &irq_flags);
                nova_update_inode(sb, inode, pi, &upd, 1);
                nova_update_alter_inode(sb, inode, pi);
                nova_memlock_inode(sb, pi, &irq_flags);
        }

        sih->log_tail = tail;
        sih->alter_log_tail = alter_tail;

        list_for_each_entry(staged, &local_staged, list) {
                struct nova_file_write_entry *pmem;
                unsigned long base;
                unsigned int n, i;

                if (!staged->pmem_curr_p)
                        continue;

                pmem = (struct nova_file_write_entry *)
                        nova_get_block(sb, staged->pmem_curr_p);
                base = (unsigned long)le64_to_cpu(staged->e.pgoff);
                n    = (unsigned int)le32_to_cpu(staged->e.num_pages);

                for (i = 0; i < n; i++) {
                        unsigned long p = base + i;
                        void **slot = radix_tree_lookup_slot(&sih->tree, p);
                        struct nova_file_write_entry *cur;

                        if (!slot)
                                continue;

                        cur = radix_tree_deref_slot(slot);
                        if (cur == &staged->e)
                                radix_tree_replace_slot(&sih->tree, slot, pmem);
                }
        }

        nova_free_log_page_batch_list(&main_batches);
        nova_free_log_page_batch_list(&alter_batches);

        list_for_each_entry_safe(staged, staged_tmp, &local_staged, list) {
                list_del(&staged->list);
                kfree(staged);
        }

        ret = invalidate_pr_node(sb, sih);
        if (ret)
                goto out;

        nova_process_deferred_free_ranges(sb, sih);

	if (sih->fast_gc_pending) {
		ret = nova_inode_log_fast_gc(sb, pi, sih, 0, 0, 0, 0, 0);
		if (ret)
			goto out;
		sih->fast_gc_pending = 0;
	}	

        goto out;

fail:
        nova_free_log_page_batch_list(&main_batches);
        nova_free_log_page_batch_list(&alter_batches);
        list_splice(&local_staged, &sih->staged_list);

out:
        return ret;
}






