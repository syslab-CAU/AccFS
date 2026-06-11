#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/math64.h>
#include <linux/limits.h>

#include <linux/calclock.h>

#include "accfs_sched.h"

/*
 * Conservative lower bound.
 * DSA polling may already help at 1KB in some cases, but 4KB is safer
 * as the first scheduling threshold.
 */
#define ACCFS_DSA_MIN_BYTES       (4UL * 1024)
#define ACCFS_DSA_LARGE_BYTES     (64UL * 1024)
#define ACCFS_DSA_XLARGE_BYTES    (256UL * 1024)
#define ACCFS_DSA_LOW_PRESS_BYTES (1UL * 1024 * 1024)

/*
 * DSA has 64 channels in this setup.
 * LOW_WM tries to keep enough WQs busy.
 * HIGH_WM prevents over-submission to DSA.
 */
#define ACCFS_DSA_LOW_WM          16
#define ACCFS_DSA_THROTTLE_WM     48
#define ACCFS_DSA_HIGH_WM         64

/*
 * EWMA with alpha = 1/8.
 * Stored as fixed-point value scaled by 8.
 */
#define ACCFS_EWMA_SHIFT          3
#define ACCFS_EWMA_SCALE          (1U << ACCFS_EWMA_SHIFT)

/*
 * Stage-1 adaptive metadata scheduling.
 * Only META/CSUM/PARITY use this adaptive ratio.
 */
#define ACCFS_META_DSA_RATIO_INIT      70
#define ACCFS_META_DSA_RATIO_MIN       15
#define ACCFS_META_DSA_RATIO_MAX       90
#define ACCFS_META_DSA_RATIO_STEP      10

/*
 * Measure only 1 out of 128 metadata copies to reduce profiling overhead.
 */
//#define ACCFS_META_COST_SAMPLE_MASK    0x7f	// 128개마다 샘플링
#define ACCFS_META_COST_SAMPLE_MASK    0xfff	// 4096개마다 샘플링

/*
 * Adjust metadata DSA ratio once per 64 sampled metadata copies.
 */
#define ACCFS_META_ADJUST_PERIOD       64

/*
 * 10% hysteresis. Ratio changes only when one path is at least 10% slower.
 */
#define ACCFS_META_COST_HYST_PCT       10

/*
 * DSA completion mode experiment knob.
 *
 * For polling-only experiment:
 *   ACCFS_DSA_MODE_DEFAULT = ACCFS_DSA_MODE_POLLING
 *
 * For interrupt-only experiment:
 *   ACCFS_DSA_MODE_DEFAULT = ACCFS_DSA_MODE_INTERRUPT
 *
 * Later, mixed policy will replace accfs_sched_select_dsa_mode().
 */
#define ACCFS_DSA_MODE_DEFAULT ACCFS_DSA_MODE_POLLING
//#define ACCFS_DSA_MODE_DEFAULT ACCFS_DSA_MODE_INTERRUPT

static atomic_t accfs_active_commits = ATOMIC_INIT(0);
static atomic_t accfs_commit_ewma_x8 = ATOMIC_INIT(0);

static atomic_t accfs_dsa_inflight = ATOMIC_INIT(0);

/*
 * Weighted credit for target-share dispatch.
 * Example: pct=40 means DSA is selected roughly 40% of the time.
 */
static atomic_t accfs_dsa_credit = ATOMIC_INIT(0);

static atomic_t accfs_meta_dsa_pct =
        ATOMIC_INIT(ACCFS_META_DSA_RATIO_INIT);

static atomic_t accfs_meta_sample_seq = ATOMIC_INIT(0);
static atomic_t accfs_meta_adjust_seq = ATOMIC_INIT(0);

static atomic64_t accfs_meta_cpu_cost_ewma_x8 = ATOMIC64_INIT(0);
static atomic64_t accfs_meta_dsa_cost_ewma_x8 = ATOMIC64_INIT(0);

static atomic64_t accfs_meta_cpu_cost_samples = ATOMIC64_INIT(0);
static atomic64_t accfs_meta_dsa_cost_samples = ATOMIC64_INIT(0);

#define ACCFS_SCHED_STATS 1
//#define ACCFS_SCHED_STATS 0

enum accfs_sched_stat_idx {
        ACCFS_STAT_DATA = 0,
        ACCFS_STAT_META_ALL,
        ACCFS_STAT_META_LOG,
        ACCFS_STAT_META_CSUM,
        ACCFS_STAT_META_PARITY,
        ACCFS_STAT_NR,
};

struct accfs_sched_stat {
        atomic64_t samples;

        atomic64_t flush_bytes_sum;
        atomic64_t flush_bytes_min;
        atomic64_t flush_bytes_max;

        atomic64_t active_sum;
        atomic64_t active_min;
        atomic64_t active_max;

        atomic64_t pressure_sum;
        atomic64_t pressure_min;
        atomic64_t pressure_max;

        atomic64_t inflight_sum;
        atomic64_t inflight_min;
        atomic64_t inflight_max;

        atomic64_t cpu_pick;
        atomic64_t dsa_pick;

        atomic64_t cpu_bytes;
        atomic64_t dsa_bytes;

        atomic64_t dsa_fallback;
        atomic64_t dsa_error;

        /* polling / interrupt */
        atomic64_t dsa_mode_pick[ACCFS_DSA_MODE_NR];
        atomic64_t dsa_mode_bytes[ACCFS_DSA_MODE_NR];

        atomic64_t dsa_mode_cost_ewma_x8[ACCFS_DSA_MODE_NR];
        atomic64_t dsa_mode_cost_samples[ACCFS_DSA_MODE_NR];

        atomic64_t dsa_mode_fallback[ACCFS_DSA_MODE_NR];
        atomic64_t dsa_mode_error[ACCFS_DSA_MODE_NR];

        /*
         * Cost EWMA is recorded per split metadata type only when
         * accfs_sched_record_copy_cost() is called by sampled copies.
         */
        atomic64_t cpu_cost_ewma_x8;
        atomic64_t dsa_cost_ewma_x8;
        atomic64_t cpu_cost_samples;
        atomic64_t dsa_cost_samples;
};

static struct accfs_sched_stat accfs_sched_stats[ACCFS_STAT_NR];

static inline bool accfs_kind_is_meta(enum accfs_copy_kind kind)
{
        return kind == ACCFS_COPY_META ||
               kind == ACCFS_COPY_CSUM ||
               kind == ACCFS_COPY_PARITY;
}

static inline int accfs_stat_idx(enum accfs_copy_kind kind)
{
        switch (kind) {
        case ACCFS_COPY_DATA:
                return ACCFS_STAT_DATA;
        case ACCFS_COPY_META:
                return ACCFS_STAT_META_LOG;
        case ACCFS_COPY_CSUM:
                return ACCFS_STAT_META_CSUM;
        case ACCFS_COPY_PARITY:
                return ACCFS_STAT_META_PARITY;
        default:
                return ACCFS_STAT_META_ALL;
        }
}

static inline bool accfs_dsa_mode_valid(enum accfs_dsa_mode mode)
{
        return mode >= 0 && mode < ACCFS_DSA_MODE_NR;
}

static const char *accfs_dsa_mode_name(enum accfs_dsa_mode mode)
{
        switch (mode) {
        case ACCFS_DSA_MODE_POLLING:
                return "polling";
        case ACCFS_DSA_MODE_INTERRUPT:
                return "interrupt";
        default:
                return "unknown";
        }
}

static void accfs_atomic64_min(atomic64_t *v, u64 x)
{
        s64 old;

        do {
                old = atomic64_read(v);
                if ((u64)old <= x)
                        return;
        } while (atomic64_cmpxchg(v, old, (s64)x) != old);
}

static void accfs_atomic64_max(atomic64_t *v, u64 x)
{
        s64 old;

        do {
                old = atomic64_read(v);
                if ((u64)old >= x)
                        return;
        } while (atomic64_cmpxchg(v, old, (s64)x) != old);
}

void accfs_sched_reset_stats(void)
{
        int i, m;

        atomic_set(&accfs_meta_dsa_pct, ACCFS_META_DSA_RATIO_INIT);
        atomic_set(&accfs_meta_sample_seq, 0);
        atomic_set(&accfs_meta_adjust_seq, 0);

        atomic64_set(&accfs_meta_cpu_cost_ewma_x8, 0);
        atomic64_set(&accfs_meta_dsa_cost_ewma_x8, 0);
        atomic64_set(&accfs_meta_cpu_cost_samples, 0);
        atomic64_set(&accfs_meta_dsa_cost_samples, 0);

        for (i = 0; i < ACCFS_STAT_NR; i++) {
                struct accfs_sched_stat *s = &accfs_sched_stats[i];

                atomic64_set(&s->samples, 0);

                atomic64_set(&s->flush_bytes_sum, 0);
                atomic64_set(&s->flush_bytes_min, (s64)U64_MAX);
                atomic64_set(&s->flush_bytes_max, 0);

                atomic64_set(&s->active_sum, 0);
                atomic64_set(&s->active_min, (s64)U64_MAX);
                atomic64_set(&s->active_max, 0);

                atomic64_set(&s->pressure_sum, 0);
                atomic64_set(&s->pressure_min, (s64)U64_MAX);
                atomic64_set(&s->pressure_max, 0);

                atomic64_set(&s->inflight_sum, 0);
                atomic64_set(&s->inflight_min, (s64)U64_MAX);
                atomic64_set(&s->inflight_max, 0);

                atomic64_set(&s->cpu_pick, 0);
                atomic64_set(&s->dsa_pick, 0);

                atomic64_set(&s->cpu_bytes, 0);
                atomic64_set(&s->dsa_bytes, 0);

                atomic64_set(&s->dsa_fallback, 0);
                atomic64_set(&s->dsa_error, 0);

                for (m = 0; m < ACCFS_DSA_MODE_NR; m++) {
                        atomic64_set(&s->dsa_mode_pick[m], 0);
                        atomic64_set(&s->dsa_mode_bytes[m], 0);

                        atomic64_set(&s->dsa_mode_cost_ewma_x8[m], 0);
                        atomic64_set(&s->dsa_mode_cost_samples[m], 0);

                        atomic64_set(&s->dsa_mode_fallback[m], 0);
                        atomic64_set(&s->dsa_mode_error[m], 0);
                }

		atomic64_set(&s->cpu_cost_ewma_x8, 0);
		atomic64_set(&s->dsa_cost_ewma_x8, 0);
		atomic64_set(&s->cpu_cost_samples, 0);
		atomic64_set(&s->dsa_cost_samples, 0);
        }
}

static void accfs_sched_record_decision_one(int idx, size_t bytes,
                                            bool use_dsa)
{
#if ACCFS_SCHED_STATS
        struct accfs_sched_stat *s;
        u64 active;
        u64 pressure;
        u64 inflight;

        s = &accfs_sched_stats[idx];

        active = accfs_sched_active_commits();
        pressure = accfs_sched_pressure();
        inflight = accfs_sched_dsa_inflight();

        atomic64_inc(&s->samples);

        atomic64_add((s64)bytes, &s->flush_bytes_sum);
        accfs_atomic64_min(&s->flush_bytes_min, (u64)bytes);
        accfs_atomic64_max(&s->flush_bytes_max, (u64)bytes);

        atomic64_add((s64)active, &s->active_sum);
        accfs_atomic64_min(&s->active_min, active);
        accfs_atomic64_max(&s->active_max, active);

        atomic64_add((s64)pressure, &s->pressure_sum);
        accfs_atomic64_min(&s->pressure_min, pressure);
        accfs_atomic64_max(&s->pressure_max, pressure);

        atomic64_add((s64)inflight, &s->inflight_sum);
        accfs_atomic64_min(&s->inflight_min, inflight);
        accfs_atomic64_max(&s->inflight_max, inflight);

        if (use_dsa) {
                atomic64_inc(&s->dsa_pick);
                atomic64_add((s64)bytes, &s->dsa_bytes);
        } else {
                atomic64_inc(&s->cpu_pick);
                atomic64_add((s64)bytes, &s->cpu_bytes);
        }
#endif
}

static void accfs_sched_record_decision(size_t bytes,
                                        enum accfs_copy_kind kind,
                                        bool use_dsa)
{
#if ACCFS_SCHED_STATS
        int idx = accfs_stat_idx(kind);

        accfs_sched_record_decision_one(idx, bytes, use_dsa);

        if (idx != ACCFS_STAT_DATA)
                accfs_sched_record_decision_one(ACCFS_STAT_META_ALL,
                                                bytes, use_dsa);
#endif
}

static void accfs_sched_record_fallback_one(int idx, int ret)
{
#if ACCFS_SCHED_STATS
        struct accfs_sched_stat *s = &accfs_sched_stats[idx];

        atomic64_inc(&s->dsa_fallback);

        if (ret)
                atomic64_inc(&s->dsa_error);
#endif
}

void accfs_sched_record_dsa_fallback(size_t bytes,
                                     enum accfs_copy_kind kind,
                                     int ret)
{
#if ACCFS_SCHED_STATS
        int idx = accfs_stat_idx(kind);

        accfs_sched_record_fallback_one(idx, ret);

        if (idx != ACCFS_STAT_DATA)
                accfs_sched_record_fallback_one(ACCFS_STAT_META_ALL, ret);
#endif
}

static void accfs_sched_record_dsa_mode_decision_one(int idx, size_t bytes,
                                                     enum accfs_dsa_mode mode)
{
#if ACCFS_SCHED_STATS
        struct accfs_sched_stat *s;

        if (!accfs_dsa_mode_valid(mode))
                return;

        s = &accfs_sched_stats[idx];

        atomic64_inc(&s->dsa_mode_pick[mode]);
        atomic64_add((s64)bytes, &s->dsa_mode_bytes[mode]);
#endif
}

void accfs_sched_record_dsa_mode_decision(size_t bytes,
                                          enum accfs_copy_kind kind,
                                          enum accfs_dsa_mode mode)
{
#if ACCFS_SCHED_STATS
        int idx = accfs_stat_idx(kind);

        accfs_sched_record_dsa_mode_decision_one(idx, bytes, mode);

        if (idx != ACCFS_STAT_DATA)
                accfs_sched_record_dsa_mode_decision_one(ACCFS_STAT_META_ALL,
                                                         bytes, mode);
#endif
}

static u64 accfs_update_cost_ewma(atomic64_t *ewma_x8, u64 sample_ns);

static void accfs_sched_record_dsa_copy_cost_one(int idx,
                                                 enum accfs_dsa_mode mode,
                                                 u64 cost_ns)
{
#if ACCFS_SCHED_STATS
        struct accfs_sched_stat *s;

        if (!accfs_dsa_mode_valid(mode))
                return;

        s = &accfs_sched_stats[idx];

        atomic64_inc(&s->dsa_mode_cost_samples[mode]);
        accfs_update_cost_ewma(&s->dsa_mode_cost_ewma_x8[mode], cost_ns);
#endif
}

void accfs_sched_record_dsa_copy_cost(enum accfs_copy_kind kind,
                                      enum accfs_dsa_mode mode,
                                      u64 cost_ns)
{
#if ACCFS_SCHED_STATS
        int idx = accfs_stat_idx(kind);

        accfs_sched_record_dsa_copy_cost_one(idx, mode, cost_ns);

        if (idx != ACCFS_STAT_DATA)
                accfs_sched_record_dsa_copy_cost_one(ACCFS_STAT_META_ALL,
                                                     mode, cost_ns);
#endif
}

static void accfs_sched_record_dsa_fallback_mode_one(int idx,
                                                     enum accfs_dsa_mode mode,
                                                     int ret)
{
#if ACCFS_SCHED_STATS
        struct accfs_sched_stat *s;

        if (!accfs_dsa_mode_valid(mode))
                return;

        s = &accfs_sched_stats[idx];

        atomic64_inc(&s->dsa_mode_fallback[mode]);

        if (ret)
                atomic64_inc(&s->dsa_mode_error[mode]);
#endif
}

void accfs_sched_record_dsa_fallback_mode(size_t bytes,
                                          enum accfs_copy_kind kind,
                                          enum accfs_dsa_mode mode,
                                          int ret)
{
#if ACCFS_SCHED_STATS
        int idx = accfs_stat_idx(kind);

        accfs_sched_record_dsa_fallback_mode_one(idx, mode, ret);

        if (idx != ACCFS_STAT_DATA)
                accfs_sched_record_dsa_fallback_mode_one(ACCFS_STAT_META_ALL,
                                                         mode, ret);
#endif
}

static u64 accfs_div(u64 a, u64 b)
{
        if (b == 0)
                return 0;

        return div64_u64(a, b);
}

static void accfs_sched_print_one(const char *name,
                                  struct accfs_sched_stat *s)
{
        u64 samples;
        u64 cpu_pick;
        u64 dsa_pick;
        u64 cpu_bytes;
        u64 dsa_bytes;
        u64 total_bytes;

        samples = atomic64_read(&s->samples);
        if (samples == 0) {
                printk(KERN_ERR "[accfs_sched] %s: no samples\n", name);
                return;
        }

        cpu_pick = atomic64_read(&s->cpu_pick);
        dsa_pick = atomic64_read(&s->dsa_pick);
        cpu_bytes = atomic64_read(&s->cpu_bytes);
        dsa_bytes = atomic64_read(&s->dsa_bytes);
        total_bytes = cpu_bytes + dsa_bytes;

        printk(KERN_ERR "================= accfs scheduler stats: %s\n", name);

        printk(KERN_ERR
               "[accfs_sched] %s samples=%llu cpu_pick=%llu dsa_pick=%llu dsa_pick_ratio=%llu%%\n",
               name,
               samples,
               cpu_pick,
               dsa_pick,
               accfs_div(dsa_pick * 100, samples));

        printk(KERN_ERR
               "[accfs_sched] %s cpu_bytes=%llu dsa_bytes=%llu dsa_byte_ratio=%llu%% total_bytes=%llu\n",
               name,
               cpu_bytes,
               dsa_bytes,
               accfs_div(dsa_bytes * 100, total_bytes),
               total_bytes);

        printk(KERN_ERR
               "[accfs_sched] %s flush_size min=%llu avg=%llu max=%llu bytes\n",
               name,
               (u64)atomic64_read(&s->flush_bytes_min),
               accfs_div((u64)atomic64_read(&s->flush_bytes_sum), samples),
               (u64)atomic64_read(&s->flush_bytes_max));

        printk(KERN_ERR
               "[accfs_sched] %s active_commit_threads min=%llu avg=%llu max=%llu\n",
               name,
               (u64)atomic64_read(&s->active_min),
               accfs_div((u64)atomic64_read(&s->active_sum), samples),
               (u64)atomic64_read(&s->active_max));

        printk(KERN_ERR
               "[accfs_sched] %s ewma_commit_pressure min=%llu avg=%llu max=%llu\n",
               name,
               (u64)atomic64_read(&s->pressure_min),
               accfs_div((u64)atomic64_read(&s->pressure_sum), samples),
               (u64)atomic64_read(&s->pressure_max));

        printk(KERN_ERR
               "[accfs_sched] %s dsa_inflight min=%llu avg=%llu max=%llu\n",
               name,
               (u64)atomic64_read(&s->inflight_min),
               accfs_div((u64)atomic64_read(&s->inflight_sum), samples),
               (u64)atomic64_read(&s->inflight_max));

        printk(KERN_ERR
               "[accfs_sched] %s dsa_fallback=%llu dsa_error=%llu\n",
               name,
               (u64)atomic64_read(&s->dsa_fallback),
               (u64)atomic64_read(&s->dsa_error));

        printk(KERN_ERR
               "[accfs_sched] %s cost_ewma cpu=%lluns dsa=%lluns cpu_samples=%llu dsa_samples=%llu\n",
               name,
               ((u64)atomic64_read(&s->cpu_cost_ewma_x8)) >> ACCFS_EWMA_SHIFT,
               ((u64)atomic64_read(&s->dsa_cost_ewma_x8)) >> ACCFS_EWMA_SHIFT,
               (u64)atomic64_read(&s->cpu_cost_samples),
               (u64)atomic64_read(&s->dsa_cost_samples));

        int m;
        u64 total_mode_pick = 0;
        u64 total_mode_bytes = 0;

        for (m = 0; m < ACCFS_DSA_MODE_NR; m++) {
                total_mode_pick += (u64)atomic64_read(&s->dsa_mode_pick[m]);
                total_mode_bytes += (u64)atomic64_read(&s->dsa_mode_bytes[m]);
        }

        for (m = 0; m < ACCFS_DSA_MODE_NR; m++) {
                u64 mode_pick = (u64)atomic64_read(&s->dsa_mode_pick[m]);
                u64 mode_bytes = (u64)atomic64_read(&s->dsa_mode_bytes[m]);

                printk(KERN_ERR
                       "[accfs_sched] %s dsa_mode=%s pick=%llu pick_ratio=%llu%% bytes=%llu byte_ratio=%llu%% cost_ewma=%lluns cost_samples=%llu fallback=%llu error=%llu\n",
                       name,
                       accfs_dsa_mode_name((enum accfs_dsa_mode)m),
                       mode_pick,
                       accfs_div(mode_pick * 100, total_mode_pick),
                       mode_bytes,
                       accfs_div(mode_bytes * 100, total_mode_bytes),
                       ((u64)atomic64_read(&s->dsa_mode_cost_ewma_x8[m])) >> ACCFS_EWMA_SHIFT,
                       (u64)atomic64_read(&s->dsa_mode_cost_samples[m]),
                       (u64)atomic64_read(&s->dsa_mode_fallback[m]),
                       (u64)atomic64_read(&s->dsa_mode_error[m]));
        }

}

void accfs_sched_print_stats(void)
{
        u64 cpu_x8 = (u64)atomic64_read(&accfs_meta_cpu_cost_ewma_x8);
        u64 dsa_x8 = (u64)atomic64_read(&accfs_meta_dsa_cost_ewma_x8);

        printk(KERN_ERR "================= accfs scheduler stats summary\n");

        printk(KERN_ERR
               "[accfs_sched] META adaptive pct=%u cpu_cost_ewma=%lluns dsa_cost_ewma=%lluns cpu_cost_samples=%llu dsa_cost_samples=%llu\n",
               accfs_sched_meta_dsa_pct(),
               cpu_x8 >> ACCFS_EWMA_SHIFT,
               dsa_x8 >> ACCFS_EWMA_SHIFT,
               (u64)atomic64_read(&accfs_meta_cpu_cost_samples),
               (u64)atomic64_read(&accfs_meta_dsa_cost_samples));

        accfs_sched_print_one("DATA", &accfs_sched_stats[ACCFS_STAT_DATA]);
        accfs_sched_print_one("META_ALL", &accfs_sched_stats[ACCFS_STAT_META_ALL]);
        accfs_sched_print_one("META_LOG", &accfs_sched_stats[ACCFS_STAT_META_LOG]);
        accfs_sched_print_one("META_CSUM", &accfs_sched_stats[ACCFS_STAT_META_CSUM]);
        accfs_sched_print_one("META_PARITY", &accfs_sched_stats[ACCFS_STAT_META_PARITY]);
}

static unsigned int accfs_update_commit_ewma(unsigned int cur)
{
        int old, new;
        int cur_x8;

        if (cur < 1)
                cur = 1;

        cur_x8 = (int)(cur * ACCFS_EWMA_SCALE);

        do {
                old = atomic_read(&accfs_commit_ewma_x8);

                if (old <= 0) {
                        new = cur_x8;
                } else {
                        /*
                         * new = old * 7/8 + cur_x8 * 1/8
                         * Since cur_x8 / 8 == cur, this becomes:
                         * new = old - old/8 + cur
                         */
                        new = old - (old >> ACCFS_EWMA_SHIFT) + cur;
                }
        } while (atomic_cmpxchg(&accfs_commit_ewma_x8, old, new) != old);

        return (unsigned int)((new + (ACCFS_EWMA_SCALE / 2)) >>
                              ACCFS_EWMA_SHIFT);
}

void accfs_sched_commit_enter(void)
{
        int cur = atomic_inc_return(&accfs_active_commits);

        if (cur < 1)
                cur = 1;

        accfs_update_commit_ewma((unsigned int)cur);
}

void accfs_sched_commit_exit(void)
{
        int cur = atomic_dec_return(&accfs_active_commits);

        if (unlikely(cur < 0)) {
                atomic_set(&accfs_active_commits, 0);
                cur = 0;
        }

        accfs_update_commit_ewma((unsigned int)max(cur, 1));
}

unsigned int accfs_sched_active_commits(void)
{
        int cur = atomic_read(&accfs_active_commits);

        return (cur > 0) ? (unsigned int)cur : 1U;
}

unsigned int accfs_sched_pressure(void)
{
        unsigned int cur = accfs_sched_active_commits();
        unsigned int ewma;
        int raw = atomic_read(&accfs_commit_ewma_x8);

        if (raw <= 0)
                return cur;

        ewma = (unsigned int)((raw + (ACCFS_EWMA_SCALE / 2)) >>
                              ACCFS_EWMA_SHIFT);

        /*
         * Fast scale-up, slow scale-down.
         * If many commit threads suddenly enter, react immediately.
         * If pressure drops, EWMA prevents abrupt policy oscillation.
         */
        return max(cur, ewma);
}

unsigned int accfs_sched_dsa_inflight(void)
{
        int cur = atomic_read(&accfs_dsa_inflight);

        return (cur > 0) ? (unsigned int)cur : 0U;
}

static unsigned int accfs_base_dsa_pct(unsigned int pressure, size_t bytes)
{
        /*
         * Low pressure: CPU is usually better for throughput.
         * Still allow a small DSA share for very large flushes.
         */
        if (pressure < 16)
                return (bytes >= ACCFS_DSA_LOW_PRESS_BYTES) ? 10U : 0U;

        if (pressure < 32)
                return 20U;

        if (pressure < 48)
                return 40U;

        if (pressure < 64)
                return 55U;

        return 70U;
}

static inline bool accfs_is_meta_kind(enum accfs_copy_kind kind)
{
        return kind == ACCFS_COPY_META ||
               kind == ACCFS_COPY_CSUM ||
               kind == ACCFS_COPY_PARITY;
}

unsigned int accfs_sched_meta_dsa_pct(void)
{
        int pct = atomic_read(&accfs_meta_dsa_pct);

        if (pct < ACCFS_META_DSA_RATIO_MIN)
                return ACCFS_META_DSA_RATIO_MIN;

        if (pct > ACCFS_META_DSA_RATIO_MAX)
                return ACCFS_META_DSA_RATIO_MAX;

        return (unsigned int)pct;
}

bool accfs_sched_should_sample_copy(enum accfs_copy_kind kind)
{
        if (!accfs_is_meta_kind(kind))
                return false;

        /*
         * Sampling is intentionally limited to metadata copies only.
         * This avoids putting ktime_get_ns() on every data copy.
         */
        return ((atomic_inc_return(&accfs_meta_sample_seq) &
                 ACCFS_META_COST_SAMPLE_MASK) == 0);
}

static u64 accfs_update_cost_ewma(atomic64_t *ewma_x8, u64 sample_ns)
{
        s64 old, new;
        s64 sample_x8;

        if (sample_ns == 0)
                sample_ns = 1;

        sample_x8 = (s64)(sample_ns << ACCFS_EWMA_SHIFT);

        do {
                old = atomic64_read(ewma_x8);

                if (old <= 0) {
                        new = sample_x8;
                } else {
                        /*
                         * EWMA alpha = 1/8:
                         * new = old * 7/8 + sample * 1/8
                         *
                         * Since old is already scaled by 8:
                         * new = old - old/8 + sample
                         */
                        new = old - (old >> ACCFS_EWMA_SHIFT) +
                              (s64)sample_ns;
                }
        } while (atomic64_cmpxchg(ewma_x8, old, new) != old);

        return (u64)(new >> ACCFS_EWMA_SHIFT);
}

static void accfs_adjust_meta_dsa_pct(void)
{
        u64 cpu_x8, dsa_x8;
        u64 cpu_ns, dsa_ns;
        int seq;
        int pct, new_pct;

        if (atomic64_read(&accfs_meta_cpu_cost_samples) == 0 ||
            atomic64_read(&accfs_meta_dsa_cost_samples) == 0)
                return;

        seq = atomic_inc_return(&accfs_meta_adjust_seq);
        if (seq % ACCFS_META_ADJUST_PERIOD)
                return;

        cpu_x8 = (u64)atomic64_read(&accfs_meta_cpu_cost_ewma_x8);
        dsa_x8 = (u64)atomic64_read(&accfs_meta_dsa_cost_ewma_x8);

        if (cpu_x8 == 0 || dsa_x8 == 0)
                return;

        cpu_ns = cpu_x8 >> ACCFS_EWMA_SHIFT;
        dsa_ns = dsa_x8 >> ACCFS_EWMA_SHIFT;

        pct = atomic_read(&accfs_meta_dsa_pct);
        new_pct = pct;

        /*
         * If CPU metadata copy is at least 10% slower than DSA polling,
         * increase metadata DSA ratio. If DSA is at least 10% slower,
         * decrease metadata DSA ratio.
         */
        if (cpu_ns > dsa_ns + div_u64(dsa_ns * ACCFS_META_COST_HYST_PCT, 100)) {
                new_pct = pct + ACCFS_META_DSA_RATIO_STEP;
        } else if (dsa_ns > cpu_ns + div_u64(cpu_ns * ACCFS_META_COST_HYST_PCT, 100)) {
                new_pct = pct - ACCFS_META_DSA_RATIO_STEP;
        }

        if (new_pct > ACCFS_META_DSA_RATIO_MAX)
                new_pct = ACCFS_META_DSA_RATIO_MAX;

        if (new_pct < ACCFS_META_DSA_RATIO_MIN)
                new_pct = ACCFS_META_DSA_RATIO_MIN;

        if (new_pct != pct)
                atomic_set(&accfs_meta_dsa_pct, new_pct);
}

static void accfs_sched_record_cost_one(int idx, bool use_dsa, u64 cost_ns)
{
#if ACCFS_SCHED_STATS
        struct accfs_sched_stat *s = &accfs_sched_stats[idx];

        if (use_dsa) {
                atomic64_inc(&s->dsa_cost_samples);
                accfs_update_cost_ewma(&s->dsa_cost_ewma_x8, cost_ns);
        } else {
                atomic64_inc(&s->cpu_cost_samples);
                accfs_update_cost_ewma(&s->cpu_cost_ewma_x8, cost_ns);
        }
#endif
}

void accfs_sched_record_copy_cost(enum accfs_copy_kind kind,
                                  bool use_dsa,
                                  u64 cost_ns)
{
        int idx;

        if (!accfs_kind_is_meta(kind))
                return;

        /*
         * Global metadata cost EWMA.
         * This is still used by the adaptive metadata ratio controller.
         */
        if (use_dsa) {
                atomic64_inc(&accfs_meta_dsa_cost_samples);
                accfs_update_cost_ewma(&accfs_meta_dsa_cost_ewma_x8,
                                       cost_ns);
        } else {
                atomic64_inc(&accfs_meta_cpu_cost_samples);
                accfs_update_cost_ewma(&accfs_meta_cpu_cost_ewma_x8,
                                       cost_ns);
        }

        /*
         * Split stats.
         */
        idx = accfs_stat_idx(kind);
        accfs_sched_record_cost_one(idx, use_dsa, cost_ns);
        accfs_sched_record_cost_one(ACCFS_STAT_META_ALL, use_dsa, cost_ns);

        accfs_adjust_meta_dsa_pct();
}

static unsigned int accfs_target_dsa_pct(size_t bytes,
                                         enum accfs_copy_kind kind)
{
        unsigned int pressure;
        unsigned int pct;
        unsigned int inflight;

        if (bytes == 0)
                return 0;

        pressure = accfs_sched_pressure();

        /*
         * Stage-1 policy:
         * DATA keeps the original size/pressure-based policy.
         * META/CSUM/PARITY bypass the 4KB minimum and use an adaptive
         * metadata DSA ratio learned from CPU-vs-DSA cost EWMA.
         */
        if (accfs_is_meta_kind(kind)) {
                pct = accfs_sched_meta_dsa_pct();

                /*
                 * Under low commit pressure, avoid being too aggressive.
                 * Under high pressure, use the adaptive ratio directly.
                 */
                if (pressure < 16)
                        pct = min(pct, 20U);

                return pct;
        }

        if (bytes < ACCFS_DSA_MIN_BYTES)
                return 0;

        pct = accfs_base_dsa_pct(pressure, bytes);
        if (pct == 0)
                return 0;

        /*
         * Size adjustment for DATA.
         * Larger actual flush ranges are better candidates for DSA.
         */
        if (pressure >= 16) {
                if (bytes < (16UL * 1024)) {			// lt 16K -> -10%
                        if (pct > 10)
                                pct -= 10;
                } else if (bytes >= ACCFS_DSA_XLARGE_BYTES) {	// ge 256K -> +20%
                        pct += 20;
                } else if (bytes >= ACCFS_DSA_LARGE_BYTES) {	// ge 64k -> +10%
                        pct += 10;
                }
        }

        inflight = accfs_sched_dsa_inflight();

        if (inflight >= ACCFS_DSA_HIGH_WM)
                return 0;

        /*
         * Admission control.
         * - If DSA is already close to full, reduce DSA share.
         * - If pressure is high and DSA is under-filled, boost DSA share
         *   to utilize at least around 16 WQs/channels.
         */
        if (inflight >= ACCFS_DSA_THROTTLE_WM) {
                if (pct > 20)
                        pct -= 20;
                else
                        pct = 0;
        } else if (pressure >= 32 && inflight < ACCFS_DSA_LOW_WM) {
                pct = max(pct, 60U);
        }

        if (pct > 90)
                pct = 90;

        return pct;
}

#define ACCFS_DSA_INT_HIGH_PRESSURE    32
//#define ACCFS_DSA_INT_MID_PRESSURE     16
#define ACCFS_DSA_INT_MID_PRESSURE     8
//#define ACCFS_DSA_INT_SIZE_THRESHOLD   (16UL * 1024)
#define ACCFS_DSA_INT_SIZE_THRESHOLD   (8UL * 1024)

enum accfs_dsa_mode accfs_sched_select_dsa_mode(size_t bytes,
                                                enum accfs_copy_kind kind)
{
        /*
         * Stage-1: fixed mode for clean polling-only / interrupt-only
         * experiments.
         */
#if 0
        (void)bytes;
        (void)kind;

        return ACCFS_DSA_MODE_DEFAULT;
#endif

        /* Stage-2 mixed policy */
        unsigned int commit_threads;
        (void)kind;

        commit_threads = accfs_sched_pressure();

        if (commit_threads >= ACCFS_DSA_INT_HIGH_PRESSURE)	// thr >= 32
                return ACCFS_DSA_MODE_INTERRUPT;

        if (commit_threads >= ACCFS_DSA_INT_MID_PRESSURE &&	// 8 <= thr < 32 && flush size > 8K
            bytes > ACCFS_DSA_INT_SIZE_THRESHOLD)
                return ACCFS_DSA_MODE_INTERRUPT;

        return ACCFS_DSA_MODE_POLLING;
}

bool accfs_sched_candidate(size_t bytes, enum accfs_copy_kind kind)
{
        return accfs_target_dsa_pct(bytes, kind) > 0;
}

static bool accfs_weighted_pick(unsigned int pct)
{
        int credit;

        if (pct >= 100)
                return true;

        if (pct == 0)
                return false;

        credit = atomic_add_return((int)pct, &accfs_dsa_credit);

        if (credit >= 100) {
                atomic_sub(100, &accfs_dsa_credit);
                return true;
        }

        return false;
}

static bool accfs_try_reserve_dsa(void)
{
        int old, new;

        do {
                old = atomic_read(&accfs_dsa_inflight);

                if (old >= ACCFS_DSA_HIGH_WM)
                        return false;

                new = old + 1;
        } while (atomic_cmpxchg(&accfs_dsa_inflight, old, new) != old);	// accfs_dsa_inflight + 1

        return true;
}

KTDEF(pick_and_try_reserve_dsa);
KTDEF(accfs_sched_record_decision);
bool accfs_sched_try_begin_dsa(size_t bytes, enum accfs_copy_kind kind)
{
        unsigned int pct = accfs_target_dsa_pct(bytes, kind);
        bool use_dsa = false;
	ktime_t stopwatch[2] = {0, 0};

	ktget(&stopwatch[0]);
        if (accfs_weighted_pick(pct))
                use_dsa = accfs_try_reserve_dsa();
	ktget(&stopwatch[1]);
	ktput(stopwatch, pick_and_try_reserve_dsa); 

	ktget(&stopwatch[0]);
        accfs_sched_record_decision(bytes, kind, use_dsa);
	ktget(&stopwatch[1]);
	ktput(stopwatch, accfs_sched_record_decision); 

        return use_dsa;
}

void accfs_sched_end_dsa(void)
{
        int cur = atomic_dec_return(&accfs_dsa_inflight);

        if (unlikely(cur < 0))
                atomic_set(&accfs_dsa_inflight, 0);
}
