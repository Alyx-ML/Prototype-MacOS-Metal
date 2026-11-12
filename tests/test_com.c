/*
 * Unit tests of the COM support: OLE32 (rt_ole32.c), OLEAUT32
 * (rt_oleaut32.c), the WMI objects (rt_wmi.c), RPCRT4 (rt_rpcrt4.c) and
 * SETUPAPI (rt_setupapi.c).
 *
 * The COM methods are called the way the game calls them: through the
 * guest vtable, with rt_call and the generated llasm thunks
 * (build/rt/com_thunks.o). The expected values are the documented Windows
 * results (HRESULT codes, VariantChangeType rounding and text rules).
 */
#include "harness.h"
#include "rt_com.h"
#include "rt_registry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void rt_CoInitialize(_cpu *cpu);
void rt_CoUninitialize(_cpu *cpu);
void rt_CoCreateInstance(_cpu *cpu);
void rt_CoCreateGuid(_cpu *cpu);
void rt_CLSIDFromString(_cpu *cpu);
void rt_CoSetProxyBlanket(_cpu *cpu);
void rt_SysAllocString(_cpu *cpu);
void rt_SysAllocStringLen(_cpu *cpu);
void rt_SysFreeString(_cpu *cpu);
void rt_VariantInit(_cpu *cpu);
void rt_VariantClear(_cpu *cpu);
void rt_VariantChangeType(_cpu *cpu);
void rt_CreateErrorInfo(_cpu *cpu);
void rt_SetErrorInfo(_cpu *cpu);
void rt_GetErrorInfo(_cpu *cpu);
void rt_UuidToStringA(_cpu *cpu);
void rt_RpcStringFreeA(_cpu *cpu);
void rt_SetupDiGetClassDevsA(_cpu *cpu);
void rt_SetupDiEnumDeviceInterfaces(_cpu *cpu);
void rt_SetupDiDestroyDeviceInfoList(_cpu *cpu);
void rt_RegCreateKeyExA(_cpu *cpu);
void rt_RegSetValueExA(_cpu *cpu);
void rt_GetLastError(_cpu *cpu);

static const rt_guid CLSID_WbemLocator = { 0x4590F811, 0x1D3A, 0x11D0, { 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
static const rt_guid IID_IWbemLocator = { 0xDC12A687, 0x737F, 0x11CF, { 0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
static const rt_guid CLSID_GameExplorer = { 0x9A5EA990, 0x3034, 0x4D6F, { 0x91, 0x28, 0x01, 0xF3, 0xC6, 0x10, 0x22, 0xBC } };
static const rt_guid IID_IGameExplorer = { 0xE7B2FB72, 0xD728, 0x49B3, { 0xA5, 0xF2, 0x18, 0xEB, 0xF5, 0xF1, 0x34, 0x9E } };
static const rt_guid IID_IErrorInfo = { 0x1CF2B120, 0x547D, 0x101B, { 0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19 } };
static const rt_guid KSCATEGORY_AUDIO = { 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

static uint32_t gguid(const rt_guid *g)
{
    uint32_t p = gbuf(16);
    rt_guid_write(p, g);
    return p;
}

/* Call method index of interface pointer p through its guest vtable. */
static uint32_t com_call(uint32_t p, int index, int n, const uint32_t *args)
{
    uint32_t w[16];
    int i;
    w[0] = p;
    for (i = 0; i < n; i++) w[i + 1] = args[i];
    return rt_call(cpu, rd32(rd32(p) + 4u * (uint32_t)index), true, 0, n + 1, w);
}
#define COM(p, index, ...) com_call((p), (index), (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), (const uint32_t[]){ __VA_ARGS__ })
#define COM0(p, index) com_call((p), (index), 0, NULL)

static uint32_t last_error(void)
{
    argv_t a = { .n = 0 };
    return callv(rt_GetLastError, &a);
}

static uint32_t create(const rt_guid *clsid, const rt_guid *iid, uint32_t ppv)
{
    argv_t a = { .n = 0 };
    a32(&a, gguid(clsid));
    a32(&a, 0);
    a32(&a, 1); /* CLSCTX_INPROC_SERVER */
    a32(&a, gguid(iid));
    a32(&a, ppv);
    return callv(rt_CoCreateInstance, &a);
}

static uint32_t bstr(const char *s)
{
    return call1(rt_SysAllocString, gws(s));
}

static void check_wstr(const char *what, uint32_t ws, const char *want)
{
    char buf[256];
    bool ok = ws != 0 && rt_wstr_to_ascii(ws, buf, sizeof buf);
    if (!ok) snprintf(buf, sizeof buf, "<not ASCII or NULL>");
    check_bytes(what, buf, strlen(buf), want, strlen(want));
}

static void test_com_init(void)
{
    uint32_t pp = gbuf(4);
    argv_t none = { .n = 0 };
    check_u32("CoCreateInstance before CoInitialize", create(&CLSID_WbemLocator, &IID_IWbemLocator, pp), CO_E_NOTINITIALIZED);
    check_u32("CoCreateInstance clears ppv", rd32(pp), 0);
    check_u32("CoInitialize reserved", call1(rt_CoInitialize, 4), E_INVALIDARG);
    check_u32("CoInitialize", call1(rt_CoInitialize, 0), S_OK);
    check_u32("CoInitialize again", call1(rt_CoInitialize, 0), S_FALSE);
    callv(rt_CoUninitialize, &none);
    check_u32("still initialized", rt_com_thread_initialized(), 1);
    check_u32("GameExplorer is not on Windows XP", create(&CLSID_GameExplorer, &IID_IGameExplorer, pp), REGDB_E_CLASSNOTREG);
}

static void test_wmi(void)
{
    uint32_t pp = gbuf(4), loc, svc, obj, var = gbuf(16), type = gbuf(4), flavor = gbuf(4), unk;
    uint32_t ns = bstr("root\\cimv2"), path;

    check_u32("create WbemLocator", create(&CLSID_WbemLocator, &IID_IWbemLocator, pp), S_OK);
    loc = rd32(pp);
    check_u32("locator pointer", loc != 0 && rt_com_lookup(loc) != NULL, 1);
    check_u32("QueryInterface IUnknown", COM(loc, 0, gguid(&rt_IID_IUnknown), pp), S_OK);
    unk = rd32(pp);
    check_u32("IUnknown identity", unk, loc);
    check_u32("AddRef count", COM0(loc, 1), 3);
    check_u32("Release count", COM0(loc, 2), 2);
    check_u32("Release of the QueryInterface reference", COM0(unk, 2), 1);

    check_u32("ConnectServer with user", COM(loc, 3, ns, bstr("user"), 0, 0, 0, 0, 0, pp), 0x80041064u);
    check_u32("ConnectServer", COM(loc, 3, ns, 0, 0, 0, 0, 0, 0, pp), S_OK);
    svc = rd32(pp);
    check_u32("CoSetProxyBlanket services", call4(rt_CoSetProxyBlanket, svc, 10, 0, 0), S_OK);
    check_u32("CoSetProxyBlanket locator", call4(rt_CoSetProxyBlanket, loc, 10, 0, 0), E_NOINTERFACE);

    path = bstr("Win32_VideoController.DeviceID=\"VideoController2\"");
    check_u32("GetObject of a second controller", COM(svc, 6, path, 0, 0, pp, 0), 0x80041002u);
    check_u32("GetObject not found clears ppv", rd32(pp), 0);
    path = bstr("Win32_VideoController.DeviceID=\"VideoController1\"");
    check_u32("GetObject", COM(svc, 6, path, 0, 0, pp, 0), S_OK);
    obj = rd32(pp);

    call1(rt_VariantInit, var);
    check_u32("Get AdapterRAM", COM(obj, 4, gws("AdapterRAM"), 0, var, type, flavor), S_OK);
    check_u32("AdapterRAM vt", rd16(var), VT_I4);
    check_u32("AdapterRAM value", rd32(var + 8), rt_gpu_adapter_ram());
    check_u32("AdapterRAM is positive", rd32(var + 8) > 0 && rd32(var + 8) <= 0x7FF00000u, 1);
    check_u32("AdapterRAM CIM type", rd32(type), 19);
    check_u32("AdapterRAM flavor", rd32(flavor), 0x20);
    check_u32("Get Name", COM(obj, 4, gws("name"), 0, var, 0, 0), S_OK);
    check_u32("Name vt", rd16(var), VT_BSTR);
    check_wstr("Name text", rd32(var + 8), rt_gpu_name());
    check_u32("VariantClear BSTR", call1(rt_VariantClear, var), S_OK);
    check_u32("VariantClear sets VT_EMPTY", rd16(var), VT_EMPTY);
    check_u32("Get DeviceID", COM(obj, 4, gws("DeviceID"), 0, var, 0, 0), S_OK);
    check_wstr("DeviceID text", rd32(var + 8), "VideoController1");
    call1(rt_VariantClear, var);

    check_u32("Release object", COM0(obj, 2), 0);
    check_u32("Release services", COM0(svc, 2), 0);
    check_u32("Release locator", COM0(loc, 2), 0);
    check_u32("released object is gone", rt_com_lookup(loc) == NULL, 1);
}

static void test_guids(void)
{
    uint32_t g = gbuf(16), g2 = gbuf(16), pp = gbuf(4), phk = gbuf(4), disp = gbuf(4);
    const char *text = "{4590F811-1D3A-11D0-891F-00AA004B2E24}";
    rt_guid v, v2;
    argv_t a;

    check_u32("CLSIDFromString", call2(rt_CLSIDFromString, gws(text), g), S_OK);
    v = rt_guid_read(g);
    check_u32("CLSIDFromString value", rt_guid_eq(&v, &CLSID_WbemLocator), 1);
    check_u32("CLSIDFromString lower case", call2(rt_CLSIDFromString, gws("{4590f811-1d3a-11d0-891f-00aa004b2e24}"), g), S_OK);
    check_u32("CLSIDFromString without braces", call2(rt_CLSIDFromString, gws("4590F811-1D3A-11D0-891F-00AA004B2E24"), g), CO_E_CLASSSTRING);
    check_u32("CLSIDFromString bad digit", call2(rt_CLSIDFromString, gws("{4590F811-1D3A-11D0-891F-00AA004B2E2X}"), g), CO_E_CLASSSTRING);
    check_u32("CLSIDFromString unknown ProgID", call2(rt_CLSIDFromString, gws("No.Such.Class"), g), CO_E_CLASSSTRING);

    /* A ProgID in HKEY_CLASSES_ROOT. */
    a = (argv_t){ .n = 0 };
    a32(&a, 0x80000000u);
    a32(&a, gs("Test.ProgID\\CLSID"));
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 1); /* REG_OPTION_VOLATILE: not saved */
    a32(&a, 0xF003F);
    a32(&a, 0);
    a32(&a, phk);
    a32(&a, disp);
    check_u32("create ProgID key", callv(rt_RegCreateKeyExA, &a), 0);
    a = (argv_t){ .n = 0 };
    a32(&a, rd32(phk));
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 1); /* REG_SZ */
    a32(&a, gs(text));
    a32(&a, (uint32_t)strlen(text) + 1);
    check_u32("set ProgID CLSID", callv(rt_RegSetValueExA, &a), 0);
    memset(G2H(g), 0, 16);
    check_u32("CLSIDFromString ProgID", call2(rt_CLSIDFromString, gws("Test.ProgID"), g), S_OK);
    v = rt_guid_read(g);
    check_u32("CLSIDFromString ProgID value", rt_guid_eq(&v, &CLSID_WbemLocator), 1);

    check_u32("CoCreateGuid", call1(rt_CoCreateGuid, g), S_OK);
    check_u32("CoCreateGuid 2", call1(rt_CoCreateGuid, g2), S_OK);
    v = rt_guid_read(g);
    v2 = rt_guid_read(g2);
    check_u32("GUID version 4", v.d3 >> 12, 4);
    check_u32("GUID variant", v.d4[0] >> 6, 2);
    check_u32("GUIDs differ", rt_guid_eq(&v, &v2), 0);

    rt_guid_write(g, &CLSID_WbemLocator);
    check_u32("UuidToStringA", call2(rt_UuidToStringA, g, pp), 0);
    check_str("UuidToStringA text", rd32(pp), "4590f811-1d3a-11d0-891f-00aa004b2e24");
    check_u32("RpcStringFreeA", call1(rt_RpcStringFreeA, pp), 0);
    check_u32("RpcStringFreeA clears", rd32(pp), 0);
    check_u32("UuidToStringA NULL", call2(rt_UuidToStringA, 0, pp), 0);
    check_str("nil UUID", rd32(pp), "00000000-0000-0000-0000-000000000000");
    call1(rt_RpcStringFreeA, pp);
}

static void test_bstr(void)
{
    uint32_t b = bstr("abc"), z;
    check_u32("BSTR byte length", rd32(b - 4), 6);
    check_wstr("BSTR text", b, "abc");
    check_u32("BSTR terminator", rd16(b + 6), 0);
    z = call2(rt_SysAllocStringLen, 0, 3);
    check_u32("SysAllocStringLen NULL length", rd32(z - 4), 6);
    check_u32("SysAllocStringLen NULL zero", rd16(z) | rd16(z + 2) | rd16(z + 4) | rd16(z + 6), 0);
    check_u32("SysAllocString NULL", call1(rt_SysAllocString, 0), 0);
    call1(rt_SysFreeString, b);
    call1(rt_SysFreeString, z);
    call1(rt_SysFreeString, 0);
}

static uint32_t var_i4(uint32_t v, int32_t x)
{
    memset(G2H(v), 0, 16);
    wr16(v, VT_I4);
    wr32(v + 8, (uint32_t)x);
    return v;
}

static uint32_t var_r8(uint32_t v, double d)
{
    memset(G2H(v), 0, 16);
    wr16(v, VT_R8);
    memcpy(G2H(v + 8), &d, 8);
    return v;
}

static uint32_t var_bstr(uint32_t v, const char *s)
{
    memset(G2H(v), 0, 16);
    wr16(v, VT_BSTR);
    wr32(v + 8, bstr(s));
    return v;
}

static uint32_t var_bool(uint32_t v, bool b)
{
    memset(G2H(v), 0, 16);
    wr16(v, VT_BOOL);
    wr16(v + 8, b ? 0xFFFF : 0);
    return v;
}

static uint32_t change(uint32_t dst, uint32_t src, uint32_t flags, uint32_t vt)
{
    return call4(rt_VariantChangeType, dst, src, flags, vt);
}

static void test_variant(void)
{
    uint32_t s = gbuf(16), d = gbuf(16), ref = gbuf(16), cell = gbuf(4);
    double r;

    check_u32("I4 to BSTR", change(d, var_i4(s, -7), 0, VT_BSTR), S_OK);
    check_wstr("I4 to BSTR text", rd32(d + 8), "-7");
    call1(rt_VariantClear, d);
    check_u32("R8 2.5 to I4", change(d, var_r8(s, 2.5), 0, VT_I4), S_OK);
    check_u32("R8 2.5 rounds to even", rd32(d + 8), 2);
    change(d, var_r8(s, 3.5), 0, VT_I4);
    check_u32("R8 3.5 rounds to even", rd32(d + 8), 4);
    change(d, var_r8(s, -2.5), 0, VT_I2);
    check_u32("R8 -2.5 to I2", rd16(d + 8), 0xFFFE);
    check_u32("R8 overflow", change(d, var_r8(s, 1e10), 0, VT_I4), DISP_E_OVERFLOW);
    check_u32("failed change leaves VT_EMPTY", rd16(d), VT_EMPTY);
    check_u32("I4 -1 to UI4", change(d, var_i4(s, -1), 0, VT_UI4), DISP_E_OVERFLOW);
    check_u32("I4 300 to UI1", change(d, var_i4(s, 300), 0, VT_UI1), DISP_E_OVERFLOW);
    check_u32("I4 255 to UI1", change(d, var_i4(s, 255), 0, VT_UI1), S_OK);
    check_u32("I4 255 to UI1 value", rd8(d + 8), 255);
    check_u32("BSTR to I2", change(d, var_bstr(s, " 12 "), 0, VT_I2), S_OK);
    check_u32("BSTR to I2 value", rd16(d + 8), 12);
    call1(rt_VariantClear, s);
    check_u32("BSTR to R8", change(d, var_bstr(s, "1,234.5"), 0, VT_R8), S_OK);
    r = rdf64(d + 8);
    check_u32("BSTR to R8 value", r == 1234.5, 1);
    call1(rt_VariantClear, s);
    check_u32("BSTR exponent to I4", change(d, var_bstr(s, "1.5e2"), 0, VT_I4), S_OK);
    check_u32("BSTR exponent value", rd32(d + 8), 150);
    call1(rt_VariantClear, s);
    check_u32("BSTR not a number", change(d, var_bstr(s, "abc"), 0, VT_I4), DISP_E_TYPEMISMATCH);
    call1(rt_VariantClear, s);
    check_u32("BOOL to BSTR", change(d, var_bool(s, true), 0, VT_BSTR), S_OK);
    check_wstr("BOOL to BSTR text", rd32(d + 8), "-1");
    call1(rt_VariantClear, d);
    check_u32("BOOL to BSTR alpha", change(d, var_bool(s, false), 2, VT_BSTR), S_OK);
    check_wstr("BOOL to BSTR alpha text", rd32(d + 8), "False");
    call1(rt_VariantClear, d);
    check_u32("BSTR true to BOOL", change(d, var_bstr(s, "TRUE"), 0, VT_BOOL), S_OK);
    check_u32("BSTR true to BOOL value", rd16(d + 8), 0xFFFF);
    call1(rt_VariantClear, s);
    check_u32("BOOL to UI1", change(d, var_bool(s, true), 0, VT_UI1), S_OK);
    check_u32("BOOL to UI1 value", rd8(d + 8), 255);
    check_u32("R8 to BSTR", change(d, var_r8(s, 0.1), 0, VT_BSTR), S_OK);
    check_wstr("R8 to BSTR text", rd32(d + 8), "0.1");
    call1(rt_VariantClear, d);
    check_u32("R8 large to BSTR", change(d, var_r8(s, 1e20), 0, VT_BSTR), S_OK);
    check_wstr("R8 large to BSTR text", rd32(d + 8), "1E+20");
    call1(rt_VariantClear, d);
    call1(rt_VariantInit, s);
    check_u32("EMPTY to I4", change(d, s, 0, VT_I4), S_OK);
    check_u32("EMPTY to I4 value", rd32(d + 8), 0);
    check_u32("EMPTY to BSTR", change(d, s, 0, VT_BSTR), S_OK);
    check_u32("EMPTY to BSTR is empty", rd32(d + 8) != 0 && rt_bstr_len(rd32(d + 8)) == 0, 1);
    call1(rt_VariantClear, d);
    wr16(s, VT_NULL);
    check_u32("NULL to I4", change(d, s, 0, VT_I4), DISP_E_TYPEMISMATCH);
    check_u32("in place BSTR to I4", change(s, var_bstr(s, "5"), 0, VT_I4), S_OK);
    check_u32("in place value", rd16(s) == VT_I4 && rd32(s + 8) == 5, 1);
    wr32(cell, 42);
    memset(G2H(ref), 0, 16);
    wr16(ref, VT_I4 | VT_BYREF);
    wr32(ref + 8, cell);
    check_u32("BYREF I4 to R8", change(d, ref, 0, VT_R8), S_OK);
    check_u32("BYREF value", rdf64(d + 8) == 42.0, 1);
    check_u32("VariantClear BYREF", call1(rt_VariantClear, ref), S_OK);
    check_u32("BYREF target kept", rd32(cell), 42);
    wr16(s, 0x7F);
    check_u32("VariantClear bad type", call1(rt_VariantClear, s), DISP_E_BADVARTYPE);
}

static void test_error_info(void)
{
    uint32_t pp = gbuf(4), cei, err, got, b, var = gbuf(16), g = gbuf(16);
    rt_guid v;
    check_u32("CreateErrorInfo", call1(rt_CreateErrorInfo, pp), S_OK);
    cei = rd32(pp);
    check_u32("SetGUID", COM(cei, 3, gguid(&CLSID_WbemLocator)), S_OK);
    check_u32("SetSource", COM(cei, 4, gws("source")), S_OK);
    check_u32("SetDescription", COM(cei, 5, gws("desc")), S_OK);
    check_u32("SetHelpContext", COM(cei, 7, 77), S_OK);
    check_u32("QueryInterface IErrorInfo", COM(cei, 0, gguid(&IID_IErrorInfo), pp), S_OK);
    err = rd32(pp);
    check_u32("IErrorInfo is slot 1", err, cei + 4);
    check_u32("QueryInterface IUnknown from slot 1", COM(err, 0, gguid(&rt_IID_IUnknown), pp), S_OK);
    check_u32("identity from slot 1", rd32(pp), cei);
    COM0(rd32(pp), 2);

    check_u32("SetErrorInfo", call2(rt_SetErrorInfo, 0, err), S_OK);
    check_u32("references after SetErrorInfo", COM0(err, 1), 4); /* create, QI, SetErrorInfo, this AddRef */
    COM0(err, 2);
    check_u32("GetErrorInfo", call2(rt_GetErrorInfo, 0, pp), S_OK);
    got = rd32(pp);
    check_u32("GetErrorInfo pointer", got, err);
    check_u32("GetErrorInfo again", call2(rt_GetErrorInfo, 0, pp), S_FALSE);
    check_u32("GetErrorInfo again NULL", rd32(pp), 0);
    check_u32("GetDescription", COM(err, 5, pp), S_OK);
    b = rd32(pp);
    check_wstr("GetDescription text", b, "desc");
    call1(rt_SysFreeString, b);
    check_u32("GetHelpFile unset", COM(err, 6, pp), S_OK);
    check_u32("GetHelpFile unset NULL", rd32(pp), 0);
    check_u32("GetHelpContext", COM(err, 7, pp), S_OK);
    check_u32("GetHelpContext value", rd32(pp), 77);
    check_u32("GetGUID", COM(err, 3, g), S_OK);
    v = rt_guid_read(g);
    check_u32("GetGUID value", rt_guid_eq(&v, &CLSID_WbemLocator), 1);

    /* VariantClear releases a VT_UNKNOWN through its vtable. */
    memset(G2H(var), 0, 16);
    wr16(var, VT_UNKNOWN);
    wr32(var + 8, got); /* the GetErrorInfo reference */
    check_u32("VariantClear UNKNOWN", call1(rt_VariantClear, var), S_OK);
    check_u32("release QI reference", COM0(err, 2), 1);
    check_u32("release last reference", COM0(cei, 2), 0);
    check_u32("error object is gone", rt_com_lookup(cei) == NULL, 1);
}

static void test_setupapi(void)
{
    uint32_t data = gbuf(28), h;
    argv_t a;
    h = call4(rt_SetupDiGetClassDevsA, gguid(&KSCATEGORY_AUDIO), 0, 0, 0x12);
    check_u32("SetupDiGetClassDevsA", h != 0 && h != 0xFFFFFFFFu, 1);
    wr32(data, 28);
    a = (argv_t){ .n = 0 };
    a32(&a, h);
    a32(&a, 0);
    a32(&a, gguid(&KSCATEGORY_AUDIO));
    a32(&a, 0);
    a32(&a, data);
    check_u32("SetupDiEnumDeviceInterfaces", callv(rt_SetupDiEnumDeviceInterfaces, &a), 0);
    check_u32("no interfaces", last_error(), ERROR_NO_MORE_ITEMS);
    wr32(data, 30);
    check_u32("bad cbSize", callv(rt_SetupDiEnumDeviceInterfaces, &a), 0);
    check_u32("bad cbSize error", last_error(), 1784);
    check_u32("SetupDiDestroyDeviceInfoList", call1(rt_SetupDiDestroyDeviceInfoList, h), 1);
    check_u32("destroy again", call1(rt_SetupDiDestroyDeviceInfoList, h), 0);
    check_u32("destroy again error", last_error(), ERROR_INVALID_HANDLE);
    check_u32("bad flags", call4(rt_SetupDiGetClassDevsA, gguid(&KSCATEGORY_AUDIO), 0, 0, 0x100), 0xFFFFFFFFu);
    check_u32("bad flags error", last_error(), 1004);
}

int main(void)
{
    char file[1024];
    const char *tmp = getenv("TMPDIR");
    argv_t none = { .n = 0 };
    harness_init();
    snprintf(file, sizeof file, "%s/test_com_registry_%d.txt", tmp != NULL ? tmp : "/tmp", (int)getpid());
    rt_registry_init(file);
    test_com_init();
    test_wmi();
    test_guids();
    test_bstr();
    test_variant();
    test_error_info();
    test_setupapi();
    callv(rt_CoUninitialize, &none);
    check_u32("uninitialized", rt_com_thread_initialized(), 0);
    unlink(file);
    return harness_report();
}
