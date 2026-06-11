#ifndef __COMMIT_H
#define __COMMIT_H

//#define DATA_INFLIGHT_LIMIT	6
//#define META_INFLIGHT_LIMIT	6
//#define CSUM_INFLIGHT_LIMIT	6

#define CPU_INFLIGHT_BYTE_LIMIT	(60UL * 1024UL)

/*
 * cpu_inflight_byte is a virtual CPU budget, not raw bytes.
 * These charges reproduce the measured optimal count-based limits:
 *   4K   -> about 11 CPU operations
 *   32K  -> about 6 CPU operations
 *   64K  -> about 4 CPU operations
 *   128K -> about 3 CPU operations
 *   256K -> about 2 CPU operations
 */
#define CPU_INFLIGHT_CHARGE_4K		(4608UL)
#define CPU_INFLIGHT_CHARGE_32K		(9UL * 1024UL)
#define CPU_INFLIGHT_CHARGE_64K		(12UL * 1024UL)
#define CPU_INFLIGHT_CHARGE_128K		(16UL * 1024UL)
#define CPU_INFLIGHT_CHARGE_256K		(24UL * 1024UL)

#define DATA_INFLIGHT_BYTE_LIMIT	CPU_INFLIGHT_BYTE_LIMIT
#define META_INFLIGHT_BYTE_LIMIT	CPU_INFLIGHT_BYTE_LIMIT
#define CSUM_INFLIGHT_BYTE_LIMIT	CPU_INFLIGHT_BYTE_LIMIT

/* for data memcpy */
//#define ONLY_USE_CPU_FOR_DATA
//#define ONLY_USE_DSA_FOR_DATA
#define ADAPTIVE_FOR_DATA


/* for metadata memcpy (data parity, we, ) */
//#define ONLY_USE_CPU_FOR_META
//#define ONLY_USE_DSA_FOR_META
#define ADAPTIVE_FOR_META


/* for checksum */
//#define ONLY_USE_CPU_FOR_CHECKSUM 
//#define ONLY_USE_DSA_FOR_CHECKSUM 
#define ADAPTIVE_FOR_CHECKSUM


/* polling/interrupt for memcpy */
//#define ONLY_USE_POLLING_FOR_MEMCPY
#define ONLY_USE_INTERRUPT_FOR_MEMCPY
//#define ADAPTIVE_FOR_MEMCPY_COMPLETION

/* polling/interrupt for checksumy */
//#define ONLY_USE_POLLING_FOR_CHECKSUM
#define ONLY_USE_INTERRUPT_FOR_CHECKSUM
//#define ADAPTIVE_FOR_CHECKSUM_COMPLETION


#ifndef NOVA_DBUF_MAX_PAGES
/* node 하나가 관리하는 최대 4KB 페이지 수 (1MB) */
//#define NOVA_DBUF_MAX_PAGES 256UL
#define NOVA_DBUF_MAX_PAGES 1024UL //(4MB)
#endif

#define NOVA_BLOCK_CSUM_SIZE   sizeof(__le32)

#ifndef NOVA_LOG_PAGE_FLUSH_ENTRY_THRESHOLD
/*
 * commit_process()에서 한 log page batch에 이번 commit으로 추가된
 * write entry 수가 이 값 이상이면 4KB log page 전체를 flush한다.
 *
 * 1  : 기존 구현처럼 batch가 생기면 항상 page 단위 flush
 * 10 : write entry 10개 이상부터 page 단위 flush
 * 63 : write entry가 한 page를 모두 채울 때만 page 단위 flush
 */
#define NOVA_LOG_PAGE_FLUSH_ENTRY_THRESHOLD 63U
#endif

#define CPU_DSA_SPLIT        10

#define USE_CPU 0
#define USE_DSA 1

#define CPU_ONLY (-1)
#define DSA_ONLY (-2)

//#define NOVA_CPU_INFLIGHT_TIME

//extern atomic_t dsa_inflight_for_memcpy;
//extern atomic_t dsa_inflight_for_memcpy_max;
//
//inline void inc_dsa_inflight_for_memcpy(void);
//inline void dec_dsa_inflight_for_memcpy(void);
//

#define INFLIGHT_BYTE_BIN_SHIFT 12
#define INFLIGHT_BYTE_BINS 256

extern atomic64_t cpu_inflight_byte;

inline void inc_cpu_inflight(size_t bytes);
inline void dec_cpu_inflight(size_t bytes);
inline bool pick_cpu_or_dsa_for_csum(void);
inline bool pick_cpu_or_dsa_for_csum_bytes(size_t bytes);

//inline void inc_cpu_inflight(void);
//inline void dec_cpu_inflight(void);
//inline bool pick_cpu_or_dsa_for_csum(void);

inline void nova_cpu_inflight_time_begin(size_t bytes);
inline void nova_cpu_inflight_time_end(size_t bytes);
void nova_cpu_inflight_time_print_top(void);
void nova_cpu_inflight_time_reset(void);
int commit_inode_for_staged_limit(struct super_block *sb,
        struct inode *inode, unsigned long incoming_pages);

inline void inc_cpu_inflight_bytes(size_t bytes);

inline void dec_cpu_inflight_bytes(size_t bytes);

#endif /* __COMMIT_H */


