#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mem.h>

#include <target-sys/bf_sal/bf_sys_dma.h>
#include <target-sys/bf_sal/bf_sys_mem.h>

#undef NDEBUG
#define ASSERT(x) assert(x)
#include <assert.h>

#ifdef __x86
#define LG_PAGE_SHIFT 21
#define SM_PAGE_SHIFT 12
#else
#error To support a non-x86 platform, the page sizes must be defined
#endif

#define LG_PAGE_SIZE (1 << LG_PAGE_SHIFT)
#define SM_PAGE_SIZE (1 << SM_PAGE_SHIFT)

#define UNUSED(x) (void)(x)

static bf_dma_bus_map bf_sys_dma_map_fn = NULL;
static bf_dma_bus_unmap bf_sys_dma_unmap_fn = NULL;
static int running_on_model = 1;

void bf_sys_dma_map_fn_register(bf_dma_bus_map fn1, bf_dma_bus_unmap fn2) {
	running_on_model = 0;
	bf_sys_dma_map_fn = fn1;
	bf_sys_dma_unmap_fn = fn2;
}

typedef struct dma_buf_s {
	void			*db_addr;
	struct dma_buf_s	*db_next;
} dma_buf_t;

#define MAX_NAME_LEN 64

// XXX: does this need to be locked, or does something in the upper layer
// handle that?
typedef struct dma_pool_s {
	char		dp_name[MAX_NAME_LEN];

	caddr_t		dp_start;		// start of the pool's VA range
	caddr_t		dp_end;			// end of the pool's VA range
	size_t		dp_pool_size;		// bytes allocated for the pool
	size_t		dp_buf_size;		// size of the per-pool buffers
	int		dp_buf_cnt;		// number of buffers
	size_t		dp_page_cnt;		// pages allocated for the pool
	uint64_t	*dp_phys_pfns;		// physical pages backing the pool
	uint64_t	*dp_dma_pfns;		// DMA pages backing the pool

	uint32_t	dp_pfn_shift;		// for pa->pfn calculation
	uint32_t	dp_page_size;		// size of the pages in the pool

	dma_buf_t *dp_bufs;			// all buffer structures
	dma_buf_t *dp_head;			// head of the freelist
	dma_buf_t *dp_tail;			// tail of the freelist

	int	dp_dev_id;
	int	dp_subdev_id;
} dma_pool_t;

/*
 * Returns 1 if the given virtual address is within the pool, 0 if it isn't.
 */
static int
in_pool(dma_pool_t *pool, caddr_t va) {
	return (va >= pool->dp_start && va < pool->dp_end);
}

/*
 * Given a pool and a virtual address return the index of the physical
 * page in the pool that backs that address
 */
static int
pool_page_idx(dma_pool_t *pool, caddr_t va)
{
	if (!in_pool(pool, va))
		return -1;

	return (va - pool->dp_start) / pool->dp_page_size;
}

/*
 * Given a virtual address return its offset into a page
 */
static size_t
page_offset(dma_pool_t *pool, caddr_t va)
{
	return ((size_t)((uintptr_t)va & (pool->dp_page_size - 1)));
}

static uintptr_t
pfn_to_pa(dma_pool_t *pool, uint64_t pfn)
{
	return ((uintptr_t)(pfn << pool->dp_pfn_shift));
}

static uint64_t
pa_to_pfn(dma_pool_t *pool, void *pa)
{
	return ((uintptr_t)pa >> pool->dp_pfn_shift);
}

int
bf_sys_dma_lib_init(void *x, void *y, void *z)
{
	UNUSED(x);
	UNUSED(y);
	UNUSED(z);

	return 0;
}

/*
 * Translate a VA in the process to physical address for DMA
 *
 * Unsupported on Illumos
 */
bf_dma_addr_t
bf_mem_virt2dma(const void *va)
{
	UNUSED(va);
	ASSERT(0);

	return 0;
}

/*
 * Translate a DMA address to a VA within this process.
 */
void *
bf_mem_dma2virt(bf_sys_dma_pool_handle_t hdl, bf_dma_addr_t dma_addr)
{
	dma_pool_t *pool = (dma_pool_t *)hdl;
	caddr_t pa;
	size_t idx;

	pa = (caddr_t) dma_addr;
	for (idx = 0; idx < pool->dp_page_cnt; idx++) {
		uint64_t pfn = pool->dp_dma_pfns[idx];
		caddr_t page_base = (caddr_t)pfn_to_pa(pool, pfn);
		if (pa >= page_base && pa < page_base + pool->dp_page_size) {
			break;
		}
	}

	if (idx == pool->dp_page_cnt) {
		return NULL;
	}

	uintptr_t offset = idx * pool->dp_page_size | page_offset(pool, pa);
	caddr_t va = pool->dp_start + offset;
	return (va);
}

/*
 * Allocate a single buffer from the pool's freelist.  Returns NULL if there are
 * no buffers available.
 */
static dma_buf_t *
dma_buf_alloc(dma_pool_t *pool)
{
	dma_buf_t *buf;

	if ((buf = pool->dp_head) != NULL) {
		pool->dp_head = buf->db_next;
		if (pool->dp_head == NULL) {
			pool->dp_tail = NULL;
		}
	}

	return buf;
}

/*
 * Return a buffer to the pool's freelist.
 */
static void
dma_buf_free(dma_pool_t *pool, dma_buf_t *buf)
{
	ASSERT(in_pool(pool, buf->db_addr));

	buf->db_next = NULL;
	if (pool->dp_tail == NULL) {
		pool->dp_head = buf;
	} else {
		pool->dp_tail->db_next = buf;
	};
	pool->dp_tail = buf;
}

static uint64_t
get_pfn(int fd, caddr_t va) {
	mem_vtop_t vtop;
	vtop.m_as = NULL;
	vtop.m_va = va;

	if (ioctl(fd, MEM_VTOP, &vtop) != 0) {
		fprintf(stderr, "failed to get pfn for %p: %s\n", va,
		    strerror(errno));
		return UINT64_MAX;
	}

	return vtop.m_pfn;
}

/*
 * Query the kernel for the physical pages backing the pool's VA range.
 */
static int
get_phys_pfns(dma_pool_t *hdl)
{
	caddr_t va;

	hdl->dp_phys_pfns = malloc(sizeof(uint64_t) * hdl->dp_page_cnt);
	bzero(hdl->dp_phys_pfns, sizeof(uint64_t) * hdl->dp_page_cnt);
	if (hdl->dp_phys_pfns == NULL) {
		fprintf(stderr, "couldn't allocate pfn array\n");
		return -1;
	}

	int fd = open("/dev/kmem", O_RDONLY);
	if (fd < 0) {
		/*
		 * The SDE wants to know the mapping between the virtual and
		 * physical addresses.  I _think_ it only needs that info when
		 * using a user-space packet driver.  To get that information
		 * into an omicron service zone, we would need to make /dev/kmem
		 * available inside that zone.  Alternatively, we could add
		 * another ioctl to the tofino driver to get the mappings.
		 * It's not worth doing either until we find a reason we
		 * actually need it.
		 */
		perror("unable to get va->pfn mappings: "
		    "failed to open /dev/kmem");
			
		return 0;
	}

	va = hdl->dp_start;
	for (size_t i = 0; i < hdl->dp_page_cnt; i++) {
		uint64_t pfn;

		if (running_on_model) {
			// On the simulator, all "dma" is done to/from virtual
			// addresses.
			pfn = pa_to_pfn(hdl, va);
		} else {
			pfn = get_pfn(fd, va);
		}
		if (pfn == UINT64_MAX)
			return -1;

		hdl->dp_phys_pfns[i] = pfn;
		va += hdl->dp_page_size;
	}

	return 0;
}

/*
 * Query the kernel for the addresses to use when DMAing to/from
 * this pool for this device.
 */
static int
get_dma_pfns(dma_pool_t *hdl, int dev_id, uint32_t subdev_id)
{
	hdl->dp_dma_pfns = malloc(sizeof(uint64_t) * hdl->dp_page_cnt);
	if (hdl->dp_dma_pfns == NULL) {
		fprintf(stderr, "couldn't allocate dma array\n");
		return -1;
	}

	caddr_t va = hdl->dp_start;
	for (size_t i = 0; i < hdl->dp_page_cnt; i++) {
		void *dma;

		if (running_on_model) {
			// On the simulator, all "dma" is done to/from virtual
			// addresses.
			hdl->dp_dma_pfns[i] = hdl->dp_phys_pfns[i];
		} else {
			// bf_switchd.c defines the interface in terms of
			// physical addrs, but Illumos wants to lock the range
			// using the VAs for the calling process.  Since this is
			// internal to the ioctl() implementation and
			// bf_sys_dma_map_fn() is just passing the value
			// through, we can just call it using the VA regardless
			// of the arg name.
			if (bf_sys_dma_map_fn(dev_id, subdev_id, va, hdl->dp_page_size,
			    &dma) != 0) {
				printf("unable to get dma addr for 0x%p\n", va);
				return -1;
			}
			hdl->dp_dma_pfns[i] = (uint64_t)pa_to_pfn(hdl, dma);
		}
		va += hdl->dp_page_size;
	}

	return 0;
}

static size_t
get_pagesize(uint64_t addr)
{
	uint64_t page_size;
	uint_t req, valid;

	req = MEMINFO_VPAGESIZE;
	if (meminfo(&addr, 1, &req, 1, &page_size, &valid) < 0) {
		fprintf(stderr, "failed to get meminfo for 0x%lx\n", addr);
		return (0);
	}

	if (!valid) {
		fprintf(stderr, "failed to get physmem for 0x%lx\n", addr);
		return (0);
	}

	return ((size_t) page_size);
}

/*
 * Allocate a range of memory with the given number of bytes using the requested
 * page size, zero it out, and lock it into physical memory.
 *
 * The memcntl() that requests a specific pagesize is advisory.  If the call
 * succeeds, that only means that the kernel has agreed to try to use the
 * specified page size at allocation time - not that it guarantees that it will.
 * The function will return the page size we actually allocated in the *page_sz
 * parameter.
 */
static void *
alloc_pool(size_t pool_sz, size_t *page_sz)
{
	struct memcntl_mha ma;
	size_t page_mask = *page_sz - 1;
	size_t actual_size;
	char *m;

	if ((pool_sz & page_mask) != 0) {
		fprintf(stderr, "alloc size of %lx is not %lx aligned\n",
		    pool_sz, *page_sz);
		return (NULL);
	}

	/* Allocate the VA space */
	m = mmap(0, pool_sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (m == MAP_FAILED) {
		perror("mmap failed");
		return NULL;
	}

	ma.mha_cmd = MHA_MAPSIZE_VA;
	ma.mha_flags = 0;
	ma.mha_pagesize = *page_sz;
	if (memcntl(m, pool_sz, MC_HAT_ADVISE, (caddr_t) &ma, 0, 0) < 0) {
		perror("memcntl failed");
		goto fail;
	}

	/* Fault in the zero-filled range */
	bzero(m, pool_sz);

	/* Lock it in place */
	if (memcntl(m, pool_sz, MC_LOCK, 0, 0, 0) != 0) {
		perror("failed to lock DMA memory");
		goto fail;
	}

	/*
	 * Iterate over all the pages allocated to back the pool, looking for
	 * the page size we actually allocated and verifying that all the pages
	 * have the same size.
	 */
	uint64_t addr = (uint64_t)m;
	uint64_t offset = 0;
	if ((actual_size = get_pagesize(addr)) == 0)
		goto fail;
	offset += actual_size;
	while (offset < pool_sz) {
		size_t t = get_pagesize(addr + offset);
		if (t == actual_size) {
			offset += t;
			continue;
		}

		if (t != 0) {
			fprintf(stderr, "multiple page sizes: %ld and %ld\n",
			    t, actual_size);
		}
		goto fail;
	}

	*page_sz = actual_size;
	return m;

fail:
	munmap(m, pool_sz);
	return NULL;
}

/*
 * Create a DMA memory pool, containing 'cnt' buffers of 'size' bytes, on an
 * 'align' byte boundary.
 *
 * The alignment must be a power of 2 and the buffer size must be a multiple of
 * the alignement.
 */
int
bf_sys_dma_pool_create(char *pool_name,
                           bf_sys_dma_pool_handle_t *hdl,
                           int dev_id,
                           uint32_t subdev_id,
                           size_t size,
                           int cnt,
                           unsigned align)
{
	dma_pool_t *pool_hdl = NULL;
	caddr_t pool = NULL;
	size_t page_sz, alloc_sz;

	/* Verify power-of-2 alignment */
	if (align & (align - 1)) {
		return -1;
	}

	/* Verify that the buffer size is a multiple of the alignment */
	if (size & (align - 1)) {
		return -1;
	}
	alloc_sz = size * cnt;

	pool_hdl = (dma_pool_t *)malloc(sizeof(*pool_hdl));
	if (pool_hdl == NULL) {
		fprintf(stderr, "unable to allocate dma_pool struct\n");
		return -1;
	}
	*hdl = (void *)pool_hdl;

	bzero(pool_hdl, sizeof(*pool_hdl));
	strlcpy(pool_hdl->dp_name, pool_name, MAX_NAME_LEN);
	pool_hdl->dp_buf_size = size;
	pool_hdl->dp_buf_cnt = cnt;
	pool_hdl->dp_dev_id = dev_id;
	pool_hdl->dp_subdev_id = subdev_id;

	if (alloc_sz == 0)
		return 0;

	pool_hdl->dp_bufs = malloc(sizeof (dma_buf_t) * cnt);
	if (pool_hdl->dp_bufs == NULL) {
		fprintf(stderr, "unable to allocate dma_buf structs\n");
		goto fail;
	}

	/*
	 * We use large pages to map the DMA pools, unless it's a small pool and
	 * using a large page would waste more than 256k.
	 */
#define MAX_WASTE (256 * 1024)
	if (alloc_sz < LG_PAGE_SIZE && (LG_PAGE_SIZE - alloc_sz) > MAX_WASTE) {
		page_sz = SM_PAGE_SIZE;
	} else {
		page_sz = LG_PAGE_SIZE;
	}

	size_t gap = alloc_sz % page_sz;
	if (gap != 0) {
		alloc_sz += (page_sz - gap);
	}

	pool = alloc_pool(alloc_sz, &page_sz);
	if (pool == NULL) {
		fprintf(stderr, "unable to allocate %lx bytes\n", alloc_sz);
		goto fail;
	}
	printf("DMA pool %s: va range: 0x%p -> 0x%p  size=0x%lx  page_size: 0x%lx\n",
	    pool_name, pool, pool + alloc_sz, alloc_sz, page_sz);

	pool_hdl->dp_start = pool;
	pool_hdl->dp_end = pool + alloc_sz;
	pool_hdl->dp_pool_size = alloc_sz;
	pool_hdl->dp_page_size = page_sz;
	pool_hdl->dp_pfn_shift =
	    (page_sz == SM_PAGE_SIZE) ? SM_PAGE_SHIFT : LG_PAGE_SHIFT;
	pool_hdl->dp_page_cnt = alloc_sz / pool_hdl->dp_page_size;
	if (get_phys_pfns(pool_hdl) < 0)
		goto fail;
	if (get_dma_pfns(pool_hdl, dev_id, subdev_id) < 0)
		goto fail;

	dma_buf_t *buf = pool_hdl->dp_bufs;
	for (int i = 0; i < cnt; i++) {
		buf->db_addr = (pool + i * size);
		dma_buf_free(pool_hdl, buf);
		buf++;
	}

	return 0;

fail:
	bf_sys_dma_pool_destroy(*hdl);
	return -1;
}

void
bf_sys_dma_pool_destroy(bf_sys_dma_pool_handle_t hdl)
{
	dma_pool_t *pool = (dma_pool_t *)hdl;

	if (pool != NULL) {
		if (pool->dp_phys_pfns != NULL)
			free(pool->dp_phys_pfns);
		if (pool->dp_dma_pfns != NULL)
			free(pool->dp_dma_pfns);
		if (pool->dp_bufs != NULL)
			free(pool->dp_bufs);
		if (pool->dp_start != NULL)
			munmap(pool->dp_start, pool->dp_pool_size);
		free(pool);
	}
}

int
_bf_sys_dma_alloc(bf_sys_dma_pool_handle_t hdl, size_t size, void **va,
	bf_phys_addr_t *pa, const char *file, int line)
{
	dma_buf_t *buf;

	dma_pool_t *pool = (dma_pool_t *)hdl;
	if (size > pool->dp_buf_size) {
		fprintf(stderr, "%s:%d allocating %ld bytes from pool %s "
				"with %ld bufs\n", file, line, size,
				 pool->dp_name, pool->dp_buf_size);
		return -1;
	}

	if ((buf = dma_buf_alloc(pool)) == NULL) {
		fprintf(stderr, "%s:%d pool %s is empty\n", file, line,
				 pool->dp_name);
		return -1;
	}

	*va = buf->db_addr;
	return bf_sys_dma_get_phy_addr_from_pool(hdl, *va, pa);
}

void
bf_sys_dma_free(bf_sys_dma_pool_handle_t hdl, void *va)
{
	dma_pool_t *pool = (dma_pool_t *)hdl;

	ASSERT(in_pool(pool, va));
	int idx = bf_sys_dma_buffer_index(hdl, va);
	ASSERT(idx >= 0);
	dma_buf_free(pool, &pool->dp_bufs[idx]);
}

int
bf_sys_dma_buffer_index(bf_sys_dma_pool_handle_t hdl, void *va)
{
	dma_pool_t *pool = (dma_pool_t *)hdl;

	if (!in_pool(pool, va)) {
		fprintf(stderr, "va outside of pool\n");
		return -1;
	}

	return ((caddr_t)va - pool->dp_start) / pool->dp_buf_size;
}

int
bf_sys_dma_get_phy_addr_from_pool(bf_sys_dma_pool_handle_t hdl, void *va,
    bf_phys_addr_t *pa)
{
	dma_pool_t *pool = (dma_pool_t *)hdl;
	uint64_t pfn;
	int idx;

	if ((idx = pool_page_idx(pool, (caddr_t)va)) < 0) {
		printf("out of pool\n");
		return -1;
	}

	pfn = pool->dp_dma_pfns[idx];
	*pa = pfn_to_pa(pool, pfn) | page_offset(pool, va);

	return (0);
}

int
bf_sys_dma_map(bf_sys_dma_pool_handle_t hdl, const void *va,
    const bf_phys_addr_t pa, size_t size, bf_dma_addr_t *dma_addr,
    bf_sys_dma_dir_t direction)
{
	UNUSED(pa);
	UNUSED(hdl);
	UNUSED(va);
	UNUSED(size);
	UNUSED(direction);

	*dma_addr = pa;
	return 0;
}

int
bf_sys_dma_unmap(bf_sys_dma_pool_handle_t hdl, const void *va, size_t size,
    bf_sys_dma_dir_t direction)
{
	UNUSED(hdl);
	UNUSED(va);
	UNUSED(size);
	UNUSED(direction);
	return 0;
}
