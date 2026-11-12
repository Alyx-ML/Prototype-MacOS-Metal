/*
 * MSVCR80 C++ run-time type information: __RTDynamicCast, __RTtypeid and
 * the type_info members that the game imports.
 *
 * The algorithms follow rtti.cpp and typinfo.cpp of the Visual C++ 2005
 * run-time library. The data structures (x86):
 *   vftable[-1]        RTTICompleteObjectLocator *
 *   CompleteObjectLocator  +0 signature, +4 offset, +8 cdOffset,
 *                      +12 TypeDescriptor *, +16 ClassHierarchyDescriptor *
 *   ClassHierarchyDescriptor  +0 signature, +4 attributes (1 multiple,
 *                      2 virtual inheritance), +8 numBaseClasses,
 *                      +12 BaseClassArray * (array of descriptor pointers)
 *   BaseClassDescriptor  +0 TypeDescriptor *, +4 numContainedBases,
 *                      +8 mdisp, +12 pdisp, +16 vdisp, +20 attributes
 *   TypeDescriptor / type_info  +0 vftable, +4 _M_data (name cache),
 *                      +8 decorated name (".?AV...")
 *
 * The game has no C++ exception handlers (prototypeenginef.dll does not
 * import __CxxFrameHandler3), so a std::bad_cast, std::bad_typeid or
 * std::__non_rtti_object exception would end the process. The runtime
 * stops the process with a message that names the exception instead.
 */
#include "rt_crt.h"
#include "rt_thread.h"

#include <pthread.h>

#define CHD_MULTINH 1u
#define CHD_VIRTINH 2u
#define BCD_NOTVISIBLE 1u
#define BCD_AMBIGUOUS 2u

static pthread_mutex_t typeinfo_lock = PTHREAD_MUTEX_INITIALIZER;

static void need_readable(uint32_t g, uint32_t n, const char *what)
{
    if (g == 0 || !rt_va_is_committed(g, n))
        rt_fatal("%s: no RTTI data at 0x%08x; the C++ runtime would throw std::__non_rtti_object", what, g);
}

static uint32_t col_of(uint32_t obj, const char *what)
{
    uint32_t vft, col;
    need_readable(obj, 4, what);
    vft = rd32(obj);
    need_readable(vft - 4u, 4, what);
    col = rd32(vft - 4u);
    need_readable(col, 20, what);
    return col;
}

static bool typeids_equal(uint32_t a, uint32_t b)
{
    return a == b || strcmp(G2H(a + 8u), G2H(b + 8u)) == 0;
}

/* Offset of a base class in the complete object (PMD: mdisp, pdisp,
   vdisp at bcd+8). */
static int32_t pmd_offset(uint32_t complete, uint32_t bcd)
{
    int32_t mdisp = (int32_t)rd32(bcd + 8), pdisp = (int32_t)rd32(bcd + 12), vdisp = (int32_t)rd32(bcd + 16);
    int32_t off = 0;
    if (pdisp >= 0)
    {
        uint32_t vbtable = rd32(complete + (uint32_t)pdisp);
        off = pdisp + (int32_t)rd32(vbtable + (uint32_t)vdisp);
    }
    return off + mdisp;
}

/* Single inheritance: the first visible base class of the target type.
   UNCONFIRMED: the visibility test is from the Visual C++ 2008 source;
   the 2005 source is not available here. */
static uint32_t find_si(uint32_t chd, uint32_t target)
{
    uint32_t n = rd32(chd + 8), arr = rd32(chd + 12), i;
    for (i = 0; i < n; i++)
    {
        uint32_t bcd = rd32(arr + 4u * i);
        if (typeids_equal(rd32(bcd), target) && !(rd32(bcd + 20) & BCD_NOTVISIBLE)) return bcd;
    }
    return 0;
}

/* Multiple and virtual inheritance: first a down-cast (a base of the
   target type that contains the source instance at src_offset), then a
   cross-cast (a visible, unambiguous base of the target type). */
static uint32_t find_mi(uint32_t complete, uint32_t chd, uint32_t src, int32_t src_offset, uint32_t target)
{
    uint32_t n = rd32(chd + 8), arr = rd32(chd + 12), i, j;
    for (i = 0; i < n; i++)
    {
        uint32_t bcd = rd32(arr + 4u * i);
        if (!typeids_equal(rd32(bcd), target)) continue;
        for (j = 0; j < rd32(bcd + 4) && i + 1u + j < n; j++)
        {
            uint32_t sub = rd32(arr + 4u * (i + 1u + j));
            if (typeids_equal(rd32(sub), src) && pmd_offset(complete, sub) == src_offset) return bcd;
        }
    }
    for (i = 0; i < n; i++)
    {
        uint32_t bcd = rd32(arr + 4u * i);
        if (typeids_equal(rd32(bcd), target) && !(rd32(bcd + 20) & (BCD_NOTVISIBLE | BCD_AMBIGUOUS))) return bcd;
    }
    return 0;
}

/* void *__RTDynamicCast(void *inptr, long VfDelta, void *SrcType,
   void *TargetType, int isReference) (cdecl) */
void rt___RTDynamicCast(_cpu *cpu)
{
    uint32_t inptr = ARG(0), src = ARG(2), target = ARG(3), is_ref = ARG(4);
    int32_t vfdelta = (int32_t)ARG(1);
    uint32_t col, chd, complete, bcd, attr;
    int32_t delta;

    if (inptr == 0)
    {
        RET(0);
        return;
    }
    col = col_of(inptr, "dynamic_cast");
    complete = inptr - rd32(col + 4);
    if (rd32(col + 8) != 0) complete += rd32(inptr - rd32(col + 8));
    inptr -= (uint32_t)vfdelta;
    delta = (int32_t)(inptr - complete);
    chd = rd32(col + 16);
    need_readable(chd, 16, "dynamic_cast");
    attr = rd32(chd + 4);
    if (!(attr & CHD_MULTINH)) bcd = find_si(chd, target);
    else bcd = find_mi(complete, chd, src, delta, target);
    if (bcd != 0)
    {
        RET(complete + (uint32_t)pmd_offset(complete, bcd));
        return;
    }
    if (is_ref != 0)
        rt_fatal("dynamic_cast to a reference failed (%s to %s); the C++ runtime would throw std::bad_cast",
                 (const char *)G2H(src + 8u), (const char *)G2H(target + 8u));
    RET(0);
}

/* void *__RTtypeid(void *inptr) (cdecl): the TypeDescriptor. */
void rt___RTtypeid(_cpu *cpu)
{
    uint32_t inptr = ARG(0), col, td;
    if (inptr == 0) rt_fatal("typeid of a NULL pointer; the C++ runtime would throw std::bad_typeid");
    col = col_of(inptr, "typeid");
    td = rd32(col + 12);
    if (td == 0) rt_fatal("typeid: no type descriptor; the C++ runtime would throw std::__non_rtti_object");
    RET(td);
}

/* bool type_info::operator==(const type_info &) const (thiscall) */
void rt_type_info_eq(_cpu *cpu)
{
    RET(strcmp(G2H(ARG(0) + 9u), G2H(ecx + 9u)) == 0 ? 1u : 0u);
}

/* int type_info::before(const type_info &) const (thiscall) */
void rt_type_info_before(_cpu *cpu)
{
    RET(strcmp(G2H(ARG(0) + 9u), G2H(ecx + 9u)) > 0 ? 1u : 0u);
}

/* The heads of the module lists (__type_info_node) seen in
   _name_internal_method, for _type_info_dtor_internal_method. */
#define MAX_HEADS 8
static uint32_t heads[MAX_HEADS];
static int nheads;

/* const char *type_info::_name_internal_method(__type_info_node *) const
   (thiscall). The undecorated name is allocated once and linked into the
   list of the module (node: +0 memory, +4 next). */
void rt_type_info_name_internal(_cpu *cpu)
{
    uint32_t self = ecx, head = ARG(0);
    if (rd32(self + 4) == 0)
    {
        char name[4096];
        const char *err = NULL;
        size_t len;
        if (rt_undname_type(G2H(self + 8u), name, sizeof(name), &err) != 0)
            rt_fatal("type_info::name: cannot undecorate \"%s\": %s", (const char *)G2H(self + 8u), err);
        len = strlen(name);
        while (len > 0 && name[len - 1] == ' ') name[--len] = 0;
        if (pthread_mutex_lock(&typeinfo_lock) != 0) rt_fatal("lock failed");
        if (rd32(self + 4) == 0)
        {
            uint32_t node = rt_heap_alloc(8);
            if (node != 0)
            {
                uint32_t mem = rt_heap_alloc((uint32_t)len + 1u);
                if (mem != 0)
                {
                    int i;
                    memcpy(G2H(mem), name, len + 1);
                    wr32(self + 4, mem);
                    wr32(node, mem);
                    wr32(node + 4, rd32(head + 4));
                    wr32(head + 4, node);
                    for (i = 0; i < nheads && heads[i] != head; i++) {}
                    if (i == nheads)
                    {
                        if (nheads == MAX_HEADS) rt_fatal("type_info: too many module name lists");
                        heads[nheads++] = head;
                    }
                }
                else
                {
                    rt_heap_free(node);
                }
            }
        }
        if (pthread_mutex_unlock(&typeinfo_lock) != 0) rt_fatal("unlock failed");
    }
    RET(rd32(self + 4));
}

/* void type_info::_type_info_dtor_internal_method(void) (thiscall). The
   Visual C++ 2005 code searches only the list of the run-time library
   itself; this implementation searches the lists of all modules so that a
   later __clean_type_info_names_internal does not free the memory twice. */
void rt_type_info_dtor_internal(_cpu *cpu)
{
    uint32_t self = ecx, mem;
    int i;
    if (pthread_mutex_lock(&typeinfo_lock) != 0) rt_fatal("lock failed");
    mem = rd32(self + 4);
    if (mem != 0)
    {
        for (i = 0; i < nheads; i++)
        {
            uint32_t prev = heads[i], node = rd32(prev + 4);
            while (node != 0 && rd32(node) != mem)
            {
                prev = node;
                node = rd32(node + 4);
            }
            if (node != 0)
            {
                wr32(prev + 4, rd32(node + 4));
                rt_heap_free(node);
                break;
            }
        }
        rt_heap_free(mem);
        wr32(self + 4, 0);
    }
    if (pthread_mutex_unlock(&typeinfo_lock) != 0) rt_fatal("unlock failed");
}

/* void __clean_type_info_names_internal(__type_info_node *) (cdecl).
   Frees the names of one module. The head is reset (the Visual C++ 2005
   code leaves it pointing to freed nodes). */
void rt___clean_type_info_names_internal(_cpu *cpu)
{
    uint32_t head = ARG(0), node;
    if (pthread_mutex_lock(&typeinfo_lock) != 0) rt_fatal("lock failed");
    node = rd32(head + 4);
    while (node != 0)
    {
        uint32_t next = rd32(node + 4);
        rt_heap_free(rd32(node));
        rt_heap_free(node);
        node = next;
    }
    wr32(head + 4, 0);
    if (pthread_mutex_unlock(&typeinfo_lock) != 0) rt_fatal("unlock failed");
}
