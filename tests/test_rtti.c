/*
 * Unit tests of the C++ run-time type information functions and of the
 * type name undecorator.
 *
 * The dynamic_cast tests build the RTTI structures of small class
 * hierarchies (single, multiple and virtual inheritance) in guest memory,
 * with the layout that Visual C++ 2005 generates.
 *
 * When the environment variable RT_UNDNAME_ORACLE names a file, every line
 * "<decorated name><TAB><expected type>" is checked. The expected text can
 * come from llvm-undname, which puts a space after ',' and no space
 * between two '>'; the comparison ignores all spaces for these lines.
 */
#include "harness.h"
#include "rt_crt.h"

#include <stdio.h>
#include <stdlib.h>

void rt___RTDynamicCast(_cpu *cpu);
void rt___RTtypeid(_cpu *cpu);
void rt_type_info_eq(_cpu *cpu);
void rt_type_info_before(_cpu *cpu);
void rt_type_info_name_internal(_cpu *cpu);
void rt_type_info_dtor_internal(_cpu *cpu);
void rt___clean_type_info_names_internal(_cpu *cpu);

/* ---------------------------------------------------------------- undecorator */

static void check_undname(const char *decorated, const char *want)
{
    char out[4096];
    const char *err = "";
    checks++;
    if (rt_undname_type(decorated, out, sizeof(out), &err) != 0)
    {
        failures++;
        printf("FAIL undname %s: error %s\n", decorated, err);
    }
    else if (strcmp(out, want) != 0)
    {
        failures++;
        printf("FAIL undname %s:\n  got  %s\n  want %s\n", decorated, out, want);
    }
}

static void strip_spaces(char *s)
{
    char *d = s;
    for (; *s; s++)
        if (*s != ' ') *d++ = *s;
    *d = 0;
}

static void test_undname(void)
{
    const char *path = getenv("RT_UNDNAME_ORACLE");

    check_undname(".?AH", "int");
    check_undname(".?APAD", "char *");
    check_undname(".?APBD", "char const *");
    check_undname(".?AVString@container@@", "class container::String");
    check_undname(".?AUMessage@engine@@", "struct engine::Message");
    check_undname(".?AV?$StringC@$0EA@@container@@", "class container::StringC<64>");
    check_undname(".?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@",
                  "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >");
    check_undname(".?AV?$PrimitiveMetaType@_N@content@@", "class content::PrimitiveMetaType<bool>");
    check_undname(".?AUMemberVisitor@?1???0GASGlobalContext@@QAE@PAVGFxMovieRoot@@@Z@",
                  "struct `public: __thiscall GASGlobalContext::GASGlobalContext(class GFxMovieRoot *)'::`2'::MemberVisitor");
    check_undname(".?AV?$GRefCountBase@VGImage@@@@", "class GRefCountBase<class GImage>");

    if (path != NULL)
    {
        FILE *f = fopen(path, "r");
        char line[8192];
        int n = 0, bad = 0;
        if (f == NULL)
        {
            failures++;
            printf("FAIL cannot open %s\n", path);
            return;
        }
        while (fgets(line, sizeof(line), f) != NULL)
        {
            char *tab = strchr(line, '\t'), out[4096];
            const char *err = "";
            size_t l;
            if (tab == NULL) continue;
            *tab = 0;
            l = strlen(tab + 1);
            while (l > 0 && (tab[1 + l - 1] == '\n' || tab[1 + l - 1] == '\r')) tab[1 + --l] = 0;
            n++;
            checks++;
            if (rt_undname_type(line, out, sizeof(out), &err) != 0)
            {
                if (bad++ < 20) printf("FAIL oracle %s: error %s\n", line, err);
                failures++;
                continue;
            }
            strip_spaces(out);
            strip_spaces(tab + 1);
            if (strcmp(out, tab + 1) != 0)
            {
                if (bad++ < 20) printf("FAIL oracle %s:\n  got  %s\n  want %s\n", line, out, tab + 1);
                failures++;
            }
        }
        fclose(f);
        printf("oracle: %d names, %d failures\n", n, bad);
    }
}

/* ---------------------------------------------------------------- RTTI structures */

/* TypeDescriptor with a decorated name. */
static uint32_t td(const char *name)
{
    size_t n = strlen(name) + 1;
    uint32_t g = gbuf((uint32_t)(8 + n));
    memcpy(G2H(g + 8), name, n);
    return g;
}

static uint32_t bcd(uint32_t type, uint32_t contained, int32_t mdisp, int32_t pdisp, int32_t vdisp, uint32_t attr)
{
    uint32_t g = gbuf(24);
    wr32(g, type);
    wr32(g + 4, contained);
    wr32(g + 8, (uint32_t)mdisp);
    wr32(g + 12, (uint32_t)pdisp);
    wr32(g + 16, (uint32_t)vdisp);
    wr32(g + 20, attr);
    return g;
}

static uint32_t chd(uint32_t attr, uint32_t n, const uint32_t *bcds)
{
    uint32_t g = gbuf(16), arr = gbuf(4 * n), i;
    for (i = 0; i < n; i++) wr32(arr + 4 * i, bcds[i]);
    wr32(g + 4, attr);
    wr32(g + 8, n);
    wr32(g + 12, arr);
    return g;
}

/* A vftable with its locator; returns the vftable address. */
static uint32_t vft(uint32_t offset, uint32_t type, uint32_t hierarchy)
{
    uint32_t col = gbuf(20), v = gbuf(16);
    wr32(col + 4, offset);
    wr32(col + 12, type);
    wr32(col + 16, hierarchy);
    wr32(v, col);
    return v + 4;
}

static uint32_t dyncast(uint32_t p, uint32_t src, uint32_t target)
{
    argv_t a = { .n = 0 };
    a32(&a, p);
    a32(&a, 0);
    a32(&a, src);
    a32(&a, target);
    a32(&a, 0);
    return callv(rt___RTDynamicCast, &a);
}

static void test_dynamic_cast(void)
{
    uint32_t tB = td(".?AVBase@@"), tD = td(".?AVDerived@@"), tX = td(".?AVOther@@");
    uint32_t tA = td(".?AVA@@"), tB2 = td(".?AVB@@"), tC = td(".?AVC@@");
    uint32_t tV = td(".?AVVirt@@"), tW = td(".?AVWithVirt@@");

    /* Single inheritance: Derived : Base. */
    {
        uint32_t b[2] = { bcd(tD, 1, 0, -1, 0, 0x40), bcd(tB, 0, 0, -1, 0, 0x40) };
        uint32_t h = chd(0, 2, b), obj = gbuf(16);
        wr32(obj, vft(0, tD, h));
        check_u32("SI down-cast", dyncast(obj, tB, tD), obj);
        check_u32("SI up-cast", dyncast(obj, tD, tB), obj);
        check_u32("SI unrelated", dyncast(obj, tB, tX), 0);
        check_u32("SI NULL", dyncast(0, tB, tD), 0);
        /* A copy of the target type descriptor (other module) is equal by
           name. */
        check_u32("SI equal by name", dyncast(obj, tB, td(".?AVDerived@@")), obj);
        check_u32("typeid", call1(rt___RTtypeid, obj), tD);
    }

    /* Multiple inheritance: C : A, B with B at offset 8. */
    {
        uint32_t b[3] = { bcd(tC, 2, 0, -1, 0, 0x40), bcd(tA, 0, 0, -1, 0, 0x40), bcd(tB2, 0, 8, -1, 0, 0x40) };
        uint32_t h = chd(1, 3, b), obj = gbuf(32);
        wr32(obj, vft(0, tC, h));
        wr32(obj + 8, vft(8, tC, h));
        check_u32("MI down-cast from second base", dyncast(obj + 8, tB2, tC), obj);
        check_u32("MI cross-cast", dyncast(obj + 8, tB2, tA), obj);
        check_u32("MI up-cast to second base", dyncast(obj, tA, tB2), obj + 8);
        check_u32("MI unrelated", dyncast(obj, tA, tX), 0);
        check_u32("typeid through second base", call1(rt___RTtypeid, obj + 8), tC);
    }

    /* Virtual inheritance: WithVirt : virtual Virt. The vbptr is at offset 4,
       the vbtable entry 1 is 8, so Virt is at offset 12. */
    {
        uint32_t b[2] = { bcd(tW, 1, 0, -1, 0, 0x40), bcd(tV, 0, 0, 4, 4, 0x50) };
        uint32_t h = chd(3, 2, b), obj = gbuf(32), vbt = gbuf(8);
        wr32(vbt + 4, 8);
        wr32(obj, vft(0, tW, h));
        wr32(obj + 4, vbt);
        wr32(obj + 12, vft(12, tW, h));
        check_u32("VI down-cast", dyncast(obj + 12, tV, tW), obj);
        check_u32("VI up-cast", dyncast(obj, tW, tV), obj + 12);
    }

    /* Ambiguous cross-cast target. */
    {
        uint32_t b[3] = { bcd(tC, 2, 0, -1, 0, 0x40), bcd(tA, 0, 0, -1, 0, 0x42), bcd(tA, 0, 8, -1, 0, 0x42) };
        uint32_t h = chd(1, 3, b), obj = gbuf(32);
        wr32(obj, vft(0, tC, h));
        check_u32("MI ambiguous", dyncast(obj, tC, tA), 0);
    }
}

static void test_type_info(void)
{
    uint32_t t1 = td(".?AV?$StringC@$0EA@@container@@"), t2 = td(".?AV?$StringC@$0EA@@container@@");
    uint32_t t3 = td(".?AVZeta@@"), head = gbuf(8), name, saved = cpu->_ecx;

    cpu->_ecx = t1;
    check_u32("type_info ==", call1(rt_type_info_eq, t2), 1);
    check_u32("type_info !=", call1(rt_type_info_eq, t3), 0);
    check_u32("type_info before", call1(rt_type_info_before, t3), 1);
    cpu->_ecx = t3;
    check_u32("type_info not before", call1(rt_type_info_before, t1), 0);

    cpu->_ecx = t1;
    name = call1(rt_type_info_name_internal, head);
    check_str("type_info name", name, "class container::StringC<64>");
    check_u32("type_info name cached", call1(rt_type_info_name_internal, head), name);
    check_u32("type_info node memory", rd32(rd32(head + 4)), name);
    cpu->_ecx = t2;
    call1(rt_type_info_name_internal, head);
    check_u32("type_info second node", rd32(rd32(head + 4)), rd32(t2 + 4));
    /* The destructor of t2 removes its node; the clean-up frees t1. */
    {
        argv_t a = { .n = 0 };
        callv(rt_type_info_dtor_internal, &a);
    }
    check_u32("type_info dtor clears cache", rd32(t2 + 4), 0);
    check_u32("type_info dtor unlinks node", rd32(rd32(head + 4)), name);
    call1(rt___clean_type_info_names_internal, head);
    check_u32("clean resets the list", rd32(head + 4), 0);
    cpu->_ecx = saved;
}

int main(void)
{
    harness_init();
    test_undname();
    test_dynamic_cast();
    test_type_info();
    return harness_report();
}
