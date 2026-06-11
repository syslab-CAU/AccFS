#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/async_tx.h>

enum nova_dsa_wait_mode {
        NOVA_DSA_WAIT_POLLING = 0,
        NOVA_DSA_WAIT_INTERRUPT = 1,
};

int nova_dma_memcpy_mode(void *dst, const void *src, size_t size,
                         enum nova_dsa_wait_mode mode);
int nova_dma_memcpy(void *dst, const void *src, size_t size);

struct nova_dma_memcpy_done {
    struct completion done;
    struct dmaengine_result result;
};

// static bool nova_dma_filter(struct dma_chan *chan, void *param);
// static void nova_dma_callback(void *completion);
int nova_dmaengine_init_all(void);
void nova_dmaengine_exit_all(void);
// static int nova_dma_memcpy(void *dst, const void *src, size_t size);
// static inline void nova_memcpy_atomic(void *dst, const void *src, u8 size);
int nova_offload_memcpy_to_nvmm(void *dst, const void *src, size_t size);

int nova_dma_memcpy_mode(void *dst, const void *src, size_t size,
                         enum nova_dsa_wait_mode mode);
int nova_dma_memcpy(void *dst, const void *src, size_t size);

u32 __nova_dma_crc32(u32 crc, const u8 *src, size_t size);
u32 __nova_dma_crc32_phys(u32 crc_seed, phys_addr_t phys, size_t size);
ssize_t nova_offload_dualcast_to_nvmm(void *dst1, void *dst2, const void *src, size_t len);
