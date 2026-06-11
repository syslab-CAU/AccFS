#ifndef _ACCFS_SCHED_H_
#define _ACCFS_SCHED_H_

#include <linux/types.h>
#include <linux/atomic.h>

enum accfs_copy_kind {
        ACCFS_COPY_DATA = 0,
        ACCFS_COPY_META = 1,
        ACCFS_COPY_CSUM = 2,
        ACCFS_COPY_PARITY = 3,
};

enum accfs_dsa_mode {
        ACCFS_DSA_MODE_POLLING = 0,
        ACCFS_DSA_MODE_INTERRUPT,
        ACCFS_DSA_MODE_NR,
};

void accfs_sched_commit_enter(void);
void accfs_sched_commit_exit(void);

unsigned int accfs_sched_active_commits(void);
unsigned int accfs_sched_pressure(void);
unsigned int accfs_sched_dsa_inflight(void);

/*
 * Pure candidate check.
 * This does not reserve DSA in-flight slots.
 * Use this for old boolean paths such as checksum selection.
 */
bool accfs_sched_candidate(size_t bytes, enum accfs_copy_kind kind);

/*
 * Scheduling decision + DSA slot reservation.
 * If this returns true, caller must call accfs_sched_end_dsa()
 * after nova_dma_memcpy() finishes.
 */
bool accfs_sched_try_begin_dsa(size_t bytes, enum accfs_copy_kind kind);
void accfs_sched_end_dsa(void);

bool accfs_sched_should_sample_copy(enum accfs_copy_kind kind);
void accfs_sched_record_copy_cost(enum accfs_copy_kind kind,
                                  bool use_dsa,
                                  u64 cost_ns);

/* polling, interrupt completion decision */
enum accfs_dsa_mode accfs_sched_select_dsa_mode(size_t bytes,
                                                enum accfs_copy_kind kind);

void accfs_sched_record_dsa_mode_decision(size_t bytes,
                                          enum accfs_copy_kind kind,
                                          enum accfs_dsa_mode mode);

void accfs_sched_record_dsa_copy_cost(enum accfs_copy_kind kind,
                                      enum accfs_dsa_mode mode,
                                      u64 cost_ns);

void accfs_sched_record_dsa_fallback_mode(size_t bytes,
                                          enum accfs_copy_kind kind,
                                          enum accfs_dsa_mode mode,
                                          int ret);

/* scheduling stats */
unsigned int accfs_sched_meta_dsa_pct(void);
void accfs_sched_reset_stats(void);
void accfs_sched_print_stats(void);
void accfs_sched_record_dsa_fallback(size_t bytes,
                                     enum accfs_copy_kind kind,
                                     int ret);

#endif /* _ACCFS_SCHED_H_ */
