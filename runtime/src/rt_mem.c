/*
 * Guest memory: the 2 GiB guest window, the guest address space manager
 * (VirtualAlloc semantics) and the guest heap.
 *
 * Window layout:
 *   rt_ptrofs            guest address 0. The first 64 KiB are never
 *                        accessible, so guest NULL pointer accesses fault.
 *   image                the program image (translated code and data,
 *                        thunks, runtime data). The linker places it.
 *   free gaps            every free gap in the window is reserved at
 *                        startup (PROT_NONE). VirtualAlloc and the heap use
 *                        only these reserved gaps.
 *
 * The guest address space is managed in 64 KiB granules (Windows allocation
 * granularity). Commit state is kept per 4 KiB guest page. The host page
 * size (16 KiB on Apple silicon) is larger, so a host page is readable and
 * writable when at least one of its guest pages is committed.
 *
 * The guest heap is a TLSF allocator (two-level segregated fit, O(1)) on
 * pools taken from the address space manager. Blocks of 1 MiB or more are
 * separate address space allocations.
 */
#include "rt.h"

#include <errno.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

uint64_t rt_ptrofs;
uint64_t pointer_offset; /* name used by upstream SR support code */

#define NUM_GRANULES ((uint32_t)(RT_WINDOW_SIZE / RT_GRANULE))
#define GUEST_PAGE 0x1000u
#define NUM_PAGES ((uint32_t)(RT_WINDOW_SIZE / GUEST_PAGE))
#define PAGES_PER_GRANULE (RT_GRANULE / GUEST_PAGE)

enum { G_UNAVAILABLE = 0, G_FREE = 1, G_USED = 2 };

static pthread_mutex_t va_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t granule_state[NUM_GRANULES];
/* For the first granule of an allocation: number of granules. 0 elsewhere. */
static uint32_t alloc_granules[NUM_GRANULES];
/* For every granule of an allocation: the first granule. */
static uint32_t alloc_base[NUM_GRANULES];
static uint64_t page_committed[NUM_PAGES / 64];
static uint32_t host_pg_size;
static uint32_t guest_pages_per_host_page;
/* Guest pages [image_page_lo, image_page_hi) hold the program image (the
   translated code and its data, mapped by the loader). They are readable
   for the whole process life, as the image of a Windows module is. */
static uint32_t image_page_lo, image_page_hi;

/* ---------------------------------------------------------------- helpers */

uint32_t rt_h2g_checked(const void *p)
{
    rt_fatal("host pointer %p is outside the guest window [0x%llx, 0x%llx)", p,
             (unsigned long long)rt_ptrofs, (unsigned long long)(rt_ptrofs + RT_WINDOW_SIZE));
}

int rt_gstr(char *dst, size_t max, uint32_t gstr)
{
    const char *s = G2H(gstr);
    size_t i;

    if (max == 0) return -1;
    if (s == NULL) { dst[0] = 0; return -1; }
    for (i = 0; i + 1 < max; i++)
    {
        dst[i] = s[i];
        if (s[i] == 0) return 0;
    }
    dst[max - 1] = 0;
    return (s[max - 1] == 0) ? 0 : -1;
}

/* The bitmap words are changed with atomic operations while va_lock is
   held, so that rt_va_is_committed can read them without the lock. */
static bool page_is_committed(uint32_t page)
{
    return (__atomic_load_n(&page_committed[page >> 6], __ATOMIC_RELAXED) >> (page & 63)) & 1;
}

static void set_page_committed(uint32_t page, bool v)
{
    if (v) __atomic_fetch_or(&page_committed[page >> 6], UINT64_C(1) << (page & 63), __ATOMIC_RELAXED);
    else __atomic_fetch_and(&page_committed[page >> 6], ~(UINT64_C(1) << (page & 63)), __ATOMIC_RELAXED);
}

static void *host_of(uint32_t guest)
{
    return (void *)(uintptr_t)(rt_ptrofs + guest);
}

/* Set the protection of every host page in [first_page, last_page] (guest
   page numbers) from the commit state of its guest pages. */
static void update_host_protection(uint32_t first_page, uint32_t last_page)
{
    uint32_t hp_first = first_page / guest_pages_per_host_page;
    uint32_t hp_last = last_page / guest_pages_per_host_page;
    uint32_t hp;

    for (hp = hp_first; hp <= hp_last; hp++)
    {
        uint32_t p0 = hp * guest_pages_per_host_page;
        bool any = false;
        uint32_t i;

        for (i = 0; i < guest_pages_per_host_page; i++)
        {
            if (page_is_committed(p0 + i)) { any = true; break; }
        }
        if (mprotect(host_of(p0 * GUEST_PAGE), host_pg_size, any ? (PROT_READ | PROT_WRITE) : PROT_NONE) != 0)
        {
            rt_fatal("mprotect of guest 0x%08x failed: %s", p0 * GUEST_PAGE, strerror(errno));
        }
    }
}

/* ---------------------------------------------------------------- window */

/* The host address range of the image that holds the runtime and the
   translated code (the game bundle that the loader opens). The guest
   window must hold this image, because guest addresses of the translated
   code, the import thunks and the COM vtables are offsets in the window. */
static void find_image_range(uint64_t *lo, uint64_t *hi)
{
    Dl_info info;
    const struct mach_header_64 *mh;
    const uint8_t *cmd;
    uint32_t i;
    intptr_t slide = 0;
    bool have_text = false;

    if (dladdr((const void *)&find_image_range, &info) == 0 || info.dli_fbase == NULL)
        rt_fatal("the runtime image header was not found");
    mh = (const struct mach_header_64 *)info.dli_fbase;
    if (mh->magic != MH_MAGIC_64) rt_fatal("the runtime image is not a 64-bit Mach-O image");
    /* The slide is the distance between the loaded header and the address
       of __TEXT (the segment that starts with the header). */
    cmd = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++)
    {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, SEG_TEXT) == 0)
        {
            slide = (intptr_t)((uintptr_t)mh - (uintptr_t)((const struct segment_command_64 *)lc)->vmaddr);
            have_text = true;
        }
        cmd += lc->cmdsize;
    }
    if (!have_text) rt_fatal("the runtime image has no __TEXT segment");
    *lo = UINT64_MAX;
    *hi = 0;
    cmd = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++)
    {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64)
        {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            if (strcmp(seg->segname, SEG_PAGEZERO) != 0 && seg->vmsize != 0)
            {
                uint64_t s = seg->vmaddr + (uint64_t)slide;
                uint64_t e = s + seg->vmsize;
                if (s < *lo) *lo = s;
                if (e > *hi) *hi = e;
            }
        }
        cmd += lc->cmdsize;
    }
    if (*lo == UINT64_MAX) rt_fatal("the runtime image has no segments");
}

/* Reserve one free host range [s, e) (both granule aligned) for the guest. */
static void reserve_gap(uint64_t s, uint64_t e)
{
    void *want = (void *)(uintptr_t)s;
    void *got;
    uint32_t g;

    if (e <= s) return;
    got = mmap(want, (size_t)(e - s), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (got == MAP_FAILED) rt_fatal("mmap of guest gap 0x%llx failed: %s", (unsigned long long)s, strerror(errno));
    if (got != want)
    {
        munmap(got, (size_t)(e - s));
        rt_fatal("guest gap 0x%llx..0x%llx could not be reserved", (unsigned long long)s, (unsigned long long)e);
    }
    for (g = (uint32_t)((s - rt_ptrofs) / RT_GRANULE); g < (uint32_t)((e - rt_ptrofs) / RT_GRANULE); g++)
    {
        granule_state[g] = G_FREE;
    }
}

void rt_mem_init(void)
{
    uint64_t img_lo, img_hi, win_lo, win_hi, addr;
    long ps = sysconf(_SC_PAGESIZE);

    if (ps <= 0 || ps > (long)RT_GRANULE || ((uint32_t)ps & ((uint32_t)ps - 1)) != 0)
    {
        rt_fatal("unsupported host page size %ld", ps);
    }
    host_pg_size = (uint32_t)ps;
    guest_pages_per_host_page = host_pg_size / GUEST_PAGE;

    find_image_range(&img_lo, &img_hi);
    /* Guest address 0 is 2 MiB below the image. */
    if (img_lo < UINT64_C(0x200000)) rt_fatal("program image is too low in memory");
    win_lo = (img_lo - UINT64_C(0x200000)) & ~(uint64_t)(RT_GRANULE - 1);
    win_hi = win_lo + RT_WINDOW_SIZE;
    if (img_hi + UINT64_C(0x40000000) > win_hi)
    {
        rt_fatal("program image (%llu MiB) leaves less than 1 GiB of guest memory",
                 (unsigned long long)((img_hi - img_lo) >> 20));
    }
    rt_ptrofs = win_lo;
    pointer_offset = win_lo;
    image_page_lo = (uint32_t)((img_lo - win_lo) / GUEST_PAGE);
    image_page_hi = (uint32_t)((img_hi - win_lo + GUEST_PAGE - 1) / GUEST_PAGE);

    /* Reserve every free gap above the image. The range below the image
       (guest NULL region and up to the image) stays unavailable. */
    addr = (img_hi + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1);
    while (addr < win_hi)
    {
        mach_vm_address_t raddr = addr;
        mach_vm_size_t rsize = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr;
        uint64_t gap_end;

        kr = mach_vm_region(mach_task_self(), &raddr, &rsize, VM_REGION_BASIC_INFO_64,
                            (vm_region_info_t)&info, &count, &object);
        if (kr == KERN_INVALID_ADDRESS)
        {
            raddr = win_hi; /* nothing mapped above addr */
            rsize = 0;
        }
        else if (kr != KERN_SUCCESS)
        {
            rt_fatal("mach_vm_region failed: %d", kr);
        }
        gap_end = (raddr < win_hi ? (uint64_t)raddr : win_hi) & ~(uint64_t)(RT_GRANULE - 1);
        if (gap_end > addr) reserve_gap(addr, gap_end);
        if (raddr >= win_hi) break;
        addr = ((uint64_t)raddr + rsize + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1);
    }
    rt_log("guest window 0x%llx, image %llu MiB, %llu MiB free for the guest",
           (unsigned long long)rt_ptrofs, (unsigned long long)((img_hi - img_lo) >> 20),
           (unsigned long long)(rt_va_free_bytes() >> 20));
}

/* ---------------------------------------------------------------- address space */

uint64_t rt_va_free_bytes(void)
{
    uint64_t n = 0;
    uint32_t g;
    for (g = 0; g < NUM_GRANULES; g++) if (granule_state[g] == G_FREE) n += RT_GRANULE;
    return n;
}

static uint32_t va_reserve_locked(uint32_t addr, uint32_t count)
{
    uint32_t g, run = 0, start = 0;

    if (count == 0 || count > NUM_GRANULES) return 0;
    if (addr != 0)
    {
        uint32_t g0 = addr / RT_GRANULE;
        if ((uint64_t)g0 + count > NUM_GRANULES) return 0;
        for (g = g0; g < g0 + count; g++) if (granule_state[g] != G_FREE) return 0;
        start = g0;
    }
    else
    {
        for (g = 0; g < NUM_GRANULES; g++)
        {
            if (granule_state[g] == G_FREE)
            {
                if (run == 0) start = g;
                if (++run == count) break;
            }
            else run = 0;
        }
        if (run != count) return 0;
    }
    for (g = start; g < start + count; g++)
    {
        granule_state[g] = G_USED;
        alloc_base[g] = start;
    }
    alloc_granules[start] = count;
    return start * RT_GRANULE;
}

uint32_t rt_va_reserve(uint32_t addr, uint32_t size)
{
    uint32_t count, base;

    if (size == 0) return 0;
    count = (uint32_t)(((uint64_t)size + RT_GRANULE - 1) / RT_GRANULE);
    pthread_mutex_lock(&va_lock);
    base = va_reserve_locked(addr & ~(RT_GRANULE - 1), count);
    pthread_mutex_unlock(&va_lock);
    return base;
}

/* Base granule of the allocation that contains granule g, or UINT32_MAX. */
static uint32_t alloc_base_of(uint32_t g)
{
    if (g >= NUM_GRANULES || granule_state[g] != G_USED) return UINT32_MAX;
    return alloc_base[g];
}

static int range_in_one_alloc(uint32_t addr, uint32_t size)
{
    uint64_t end = (uint64_t)addr + size;
    uint32_t b;
    if (size == 0 || end > RT_WINDOW_SIZE) return 0;
    b = alloc_base_of(addr / RT_GRANULE);
    if (b == UINT32_MAX) return 0;
    return end <= (uint64_t)(b + alloc_granules[b]) * RT_GRANULE;
}

int rt_va_commit(uint32_t addr, uint32_t size)
{
    uint32_t p0, p1, p;

    pthread_mutex_lock(&va_lock);
    if (!range_in_one_alloc(addr, size)) { pthread_mutex_unlock(&va_lock); return -1; }
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    for (p = p0; p <= p1; p++) set_page_committed(p, true);
    update_host_protection(p0, p1);
    pthread_mutex_unlock(&va_lock);
    return 0;
}

/* Make the host pages of guest pages [p0, p1] that have no committed guest
   page zero and inaccessible, and zero decommitted guest pages that share a
   host page with a committed page. */
static void discard_pages(uint32_t p0, uint32_t p1)
{
    uint32_t hp;
    for (hp = p0 / guest_pages_per_host_page; hp <= p1 / guest_pages_per_host_page; hp++)
    {
        uint32_t first = hp * guest_pages_per_host_page, i;
        bool any = false;
        for (i = 0; i < guest_pages_per_host_page; i++) if (page_is_committed(first + i)) { any = true; break; }
        if (!any)
        {
            void *h = host_of(first * GUEST_PAGE);
            if (mmap(h, host_pg_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) != h)
            {
                rt_fatal("mmap reset of guest 0x%08x failed: %s", first * GUEST_PAGE, strerror(errno));
            }
        }
        else
        {
            for (i = 0; i < guest_pages_per_host_page; i++)
            {
                uint32_t p = first + i;
                if (p >= p0 && p <= p1 && !page_is_committed(p)) memset(host_of(p * GUEST_PAGE), 0, GUEST_PAGE);
            }
        }
    }
}

int rt_va_decommit(uint32_t addr, uint32_t size)
{
    uint32_t p0, p1, p;

    pthread_mutex_lock(&va_lock);
    if (!range_in_one_alloc(addr, size)) { pthread_mutex_unlock(&va_lock); return -1; }
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    for (p = p0; p <= p1; p++) set_page_committed(p, false);
    discard_pages(p0, p1);
    pthread_mutex_unlock(&va_lock);
    return 0;
}

int rt_va_release(uint32_t base)
{
    uint32_t g0, count, g, p0, p1, p;

    pthread_mutex_lock(&va_lock);
    g0 = base / RT_GRANULE;
    if ((base & (RT_GRANULE - 1)) != 0 || g0 >= NUM_GRANULES || granule_state[g0] != G_USED || alloc_granules[g0] == 0)
    {
        pthread_mutex_unlock(&va_lock);
        return -1;
    }
    count = alloc_granules[g0];
    p0 = g0 * PAGES_PER_GRANULE;
    p1 = (g0 + count) * PAGES_PER_GRANULE - 1;
    for (p = p0; p <= p1; p++) set_page_committed(p, false);
    /* The allocation is granule aligned, so it covers whole host pages. */
    if (mmap(host_of(base), (size_t)count * RT_GRANULE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) != host_of(base))
    {
        rt_fatal("mmap reset of guest 0x%08x failed: %s", base, strerror(errno));
    }
    for (g = g0; g < g0 + count; g++) granule_state[g] = G_FREE;
    alloc_granules[g0] = 0;
    pthread_mutex_unlock(&va_lock);
    return 0;
}

int rt_va_is_committed(uint32_t addr, uint32_t size)
{
    uint32_t p0, p1, p;
    int ok = 1;
    if (size == 0) return 1;
    if ((uint64_t)addr + size > RT_WINDOW_SIZE) return 0;
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    /* No lock: the bitmap reads are atomic, and the image range does not
       change after start-up. A result is only valid until the next commit
       or decommit, as with the lock. */
    for (p = p0; p <= p1; p++)
    {
        if (!page_is_committed(p) && (p < image_page_lo || p >= image_page_hi)) { ok = 0; break; }
    }
    return ok;
}

/* Reserve and commit a whole allocation. Returns guest base or 0. */
static uint32_t va_alloc_committed(uint32_t size)
{
    uint32_t base = rt_va_reserve(0, size);
    if (base == 0) return 0;
    if (rt_va_commit(base, size) != 0) rt_fatal("commit of new allocation 0x%08x failed", base);
    return base;
}

/* ---------------------------------------------------------------- heap (TLSF) */

/*
 * Block layout (all sizes are multiples of 16):
 *   +0  prev_phys  host pointer to the physically previous block
 *   +8  size       block size including this 16-byte header; bit 0: free,
 *                  bit 1: last block of a pool
 *   +16 payload    for free blocks: next_free, prev_free (host pointers)
 */
typedef struct tblock {
    struct tblock *prev_phys;
    uint64_t size;
    struct tblock *next_free;
    struct tblock *prev_free;
} tblock;

#define TB_FREE UINT64_C(1)
#define TB_LAST UINT64_C(2)
#define TB_FLAGS UINT64_C(15)
#define TB_HDR 16u
#define TB_MIN 32u
#define SL_LOG 4
#define SL_COUNT (1 << SL_LOG)
#define FL_COUNT 32
#define LARGE_LIMIT 0x100000u
#define POOL_SIZE 0x1000000u
#define LARGE_MAGIC 0x4c524148u /* "HARL" */

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t fl_bitmap;
static uint32_t sl_bitmap[FL_COUNT];
static tblock *free_heads[FL_COUNT][SL_COUNT];
/* Kind of each granule for rt_heap_free: 1 = TLSF pool, 2 = large block. */
static uint8_t heap_granule_kind[NUM_GRANULES];

static uint64_t tb_size(const tblock *b) { return b->size & ~TB_FLAGS; }
static tblock *tb_next(tblock *b) { return (tblock *)((uint8_t *)b + tb_size(b)); }

static void mapping(uint64_t size, int *fl, int *sl)
{
    if (size < (UINT64_C(1) << (SL_LOG + 4)))
    {
        *fl = 0;
        *sl = (int)(size >> 4) & (SL_COUNT - 1);
    }
    else
    {
        int f = 63 - __builtin_clzll(size);
        *sl = (int)(size >> (f - SL_LOG)) & (SL_COUNT - 1);
        *fl = f - (SL_LOG + 4) + 1;
    }
}

static void remove_free(tblock *b)
{
    int fl, sl;
    mapping(tb_size(b), &fl, &sl);
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else
    {
        free_heads[fl][sl] = b->next_free;
        if (b->next_free == NULL)
        {
            sl_bitmap[fl] &= ~(1u << sl);
            if (sl_bitmap[fl] == 0) fl_bitmap &= ~(1u << fl);
        }
    }
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->next_free = b->prev_free = NULL;
}

static void insert_free(tblock *b)
{
    int fl, sl;
    mapping(tb_size(b), &fl, &sl);
    b->prev_free = NULL;
    b->next_free = free_heads[fl][sl];
    if (b->next_free) b->next_free->prev_free = b;
    free_heads[fl][sl] = b;
    fl_bitmap |= 1u << fl;
    sl_bitmap[fl] |= 1u << sl;
}

static tblock *find_free(uint64_t size)
{
    int fl, sl;
    uint32_t slmap, flmap;

    /* round the request up to the next list so every block in it fits */
    if (size >= (UINT64_C(1) << (SL_LOG + 4)))
    {
        size += (UINT64_C(1) << (63 - __builtin_clzll(size) - SL_LOG)) - 1;
    }
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) return NULL;
    slmap = sl_bitmap[fl] & (~0u << sl);
    if (slmap == 0)
    {
        flmap = (fl + 1 < FL_COUNT) ? (fl_bitmap & (~0u << (fl + 1))) : 0;
        if (flmap == 0) return NULL;
        fl = __builtin_ctz(flmap);
        slmap = sl_bitmap[fl];
    }
    sl = __builtin_ctz(slmap);
    return free_heads[fl][sl];
}

static int add_pool(uint32_t min_payload)
{
    uint64_t want = (uint64_t)min_payload + 2 * TB_HDR + TB_MIN;
    uint32_t size, base, g;
    tblock *b, *end;

    if (want < POOL_SIZE) want = POOL_SIZE;
    if (want > UINT32_MAX - RT_GRANULE) return -1;
    size = (uint32_t)((want + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1));
    base = va_alloc_committed(size);
    if (base == 0) return -1;
    for (g = base / RT_GRANULE; g < (base + size) / RT_GRANULE; g++) heap_granule_kind[g] = 1;
    b = (tblock *)G2H(base);
    b->prev_phys = NULL;
    b->size = (uint64_t)(size - TB_HDR) | TB_FREE;
    end = tb_next(b);
    end->prev_phys = b;
    end->size = TB_LAST; /* size 0, used */
    insert_free(b);
    return 0;
}

static uint32_t tlsf_alloc_locked(uint64_t need)
{
    tblock *b = find_free(need);
    uint64_t bs;

    if (b == NULL)
    {
        if (add_pool((uint32_t)need) != 0) return 0;
        b = find_free(need);
        if (b == NULL) rt_fatal("heap pool added but no block of %llu bytes found", (unsigned long long)need);
    }
    remove_free(b);
    bs = tb_size(b);
    if (bs - need >= TB_MIN)
    {
        tblock *rest = (tblock *)((uint8_t *)b + need);
        tblock *after = tb_next(b);
        rest->prev_phys = b;
        rest->size = (bs - need) | TB_FREE;
        after->prev_phys = rest;
        b->size = need;
        insert_free(rest);
    }
    else
    {
        b->size = bs;
    }
    return H2G((uint8_t *)b + TB_HDR);
}

static void tlsf_free_locked(tblock *b)
{
    tblock *next = tb_next(b);
    if (b->size & TB_FREE) rt_fatal("double free of guest heap block 0x%08x", H2G((uint8_t *)b + TB_HDR));
    b->size |= TB_FREE;
    if (next->size & TB_FREE)
    {
        remove_free(next);
        b->size = (tb_size(b) + tb_size(next)) | TB_FREE;
        tb_next(b)->prev_phys = b;
    }
    if (b->prev_phys != NULL && (b->prev_phys->size & TB_FREE))
    {
        tblock *prev = b->prev_phys;
        remove_free(prev);
        prev->size = (tb_size(prev) + tb_size(b)) | TB_FREE;
        tb_next(prev)->prev_phys = prev;
        b = prev;
    }
    insert_free(b);
}

static uint64_t round_request(uint32_t size)
{
    uint64_t need = ((uint64_t)size + TB_HDR + 15) & ~UINT64_C(15);
    return need < TB_MIN ? TB_MIN : need;
}

uint32_t rt_heap_alloc(uint32_t size)
{
    uint32_t r;

    if (size >= LARGE_LIMIT)
    {
        uint64_t total = (uint64_t)size + 16;
        uint32_t base, g;
        if (total > UINT32_MAX - RT_GRANULE) return 0;
        base = va_alloc_committed((uint32_t)total);
        if (base == 0) return 0;
        pthread_mutex_lock(&heap_lock);
        for (g = base / RT_GRANULE; g < base / RT_GRANULE + alloc_granules[base / RT_GRANULE]; g++) heap_granule_kind[g] = 2;
        pthread_mutex_unlock(&heap_lock);
        wr32(base, LARGE_MAGIC);
        wr32(base + 4, size);
        return base + 16;
    }
    pthread_mutex_lock(&heap_lock);
    r = tlsf_alloc_locked(round_request(size));
    pthread_mutex_unlock(&heap_lock);
    return r;
}

uint32_t rt_heap_calloc(uint32_t count, uint32_t size)
{
    uint64_t total = (uint64_t)count * size;
    uint32_t p;
    if (total > UINT32_MAX) return 0;
    p = rt_heap_alloc((uint32_t)total);
    if (p != 0) memset(G2H(p), 0, (size_t)total);
    return p;
}

static int is_large(uint32_t ptr)
{
    uint32_t g;
    if (ptr < 16) return 0;
    g = (ptr - 16) / RT_GRANULE;
    return heap_granule_kind[g] == 2 && ((ptr - 16) & (RT_GRANULE - 1)) == 0 && rd32(ptr - 16) == LARGE_MAGIC;
}

static void check_heap_ptr(uint32_t ptr)
{
    uint32_t g = ptr / RT_GRANULE;
    if ((ptr & 15) != 0 || g >= NUM_GRANULES || heap_granule_kind[g] == 0)
    {
        rt_fatal("pointer 0x%08x was not allocated by the guest heap", ptr);
    }
}

uint32_t rt_heap_size(uint32_t ptr)
{
    uint32_t r;
    if (ptr == 0) return 0;
    check_heap_ptr(ptr);
    if (is_large(ptr)) return rd32(ptr - 12);
    pthread_mutex_lock(&heap_lock);
    r = (uint32_t)(tb_size((tblock *)((uint8_t *)G2H(ptr) - TB_HDR)) - TB_HDR);
    pthread_mutex_unlock(&heap_lock);
    return r;
}

void rt_heap_free(uint32_t ptr)
{
    if (ptr == 0) return;
    check_heap_ptr(ptr);
    if (is_large(ptr))
    {
        uint32_t base = ptr - 16, g, n;
        pthread_mutex_lock(&heap_lock);
        n = alloc_granules[base / RT_GRANULE];
        for (g = base / RT_GRANULE; g < base / RT_GRANULE + n; g++) heap_granule_kind[g] = 0;
        pthread_mutex_unlock(&heap_lock);
        wr32(base, 0);
        if (rt_va_release(base) != 0) rt_fatal("release of large heap block 0x%08x failed", ptr);
        return;
    }
    pthread_mutex_lock(&heap_lock);
    tlsf_free_locked((tblock *)((uint8_t *)G2H(ptr) - TB_HDR));
    pthread_mutex_unlock(&heap_lock);
}

uint32_t rt_heap_realloc(uint32_t ptr, uint32_t size)
{
    uint32_t old_size, np;

    if (ptr == 0) return rt_heap_alloc(size);
    if (size == 0) { rt_heap_free(ptr); return 0; }
    check_heap_ptr(ptr);
    if (!is_large(ptr) && size < LARGE_LIMIT)
    {
        uint64_t need = round_request(size);
        tblock *b;
        pthread_mutex_lock(&heap_lock);
        b = (tblock *)((uint8_t *)G2H(ptr) - TB_HDR);
        if (tb_size(b) >= need)
        {
            pthread_mutex_unlock(&heap_lock);
            return ptr;
        }
        {
            tblock *next = tb_next(b);
            if ((next->size & TB_FREE) && tb_size(b) + tb_size(next) >= need)
            {
                uint64_t total = tb_size(b) + tb_size(next);
                remove_free(next);
                if (total - need >= TB_MIN)
                {
                    tblock *rest = (tblock *)((uint8_t *)b + need);
                    rest->prev_phys = b;
                    rest->size = (total - need) | TB_FREE;
                    b->size = need;
                    tb_next(rest)->prev_phys = rest;
                    insert_free(rest);
                }
                else
                {
                    b->size = total;
                    tb_next(b)->prev_phys = b;
                }
                pthread_mutex_unlock(&heap_lock);
                return ptr;
            }
        }
        pthread_mutex_unlock(&heap_lock);
    }
    old_size = rt_heap_size(ptr);
    np = rt_heap_alloc(size);
    if (np == 0) return 0;
    memcpy(G2H(np), G2H(ptr), old_size < size ? old_size : size);
    rt_heap_free(ptr);
    return np;
}
