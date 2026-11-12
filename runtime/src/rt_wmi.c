/*
 * WMI (Windows Management Instrumentation): the objects that the engine
 * uses to read the video memory size.
 *
 * The engine calls:
 *   CoCreateInstance(CLSID_WbemLocator, IID_IWbemLocator)
 *   IWbemLocator::ConnectServer("ROOT\CIMV2")
 *   CoSetProxyBlanket(services)
 *   IWbemServices::GetObject("Win32_VideoController.DeviceID=\"VideoController1\"")
 *   IWbemClassObject::Get("AdapterRAM")
 * and, for each DirectInput device (the XInput check of the DirectX SDK):
 *   IWbemServices::CreateInstanceEnum("Win32_PNPEntity")
 *   IEnumWbemClassObject::Next until it gives no objects
 *
 * The runtime implements these calls for the local CIMV2 namespace and one
 * Win32_VideoController instance (the Metal device). The emulated system
 * has no Plug and Play device database (the SetupDi device sets are also
 * empty), so the Win32_PNPEntity enumeration has no instances. The instance has the
 * properties AdapterRAM, Caption, Description, DeviceID, Name and
 * VideoProcessor. Other namespaces, classes, properties and methods stop
 * the process with a message.
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"

#include <stdlib.h>
#include <strings.h>

#define WBEM_S_FALSE 0x00000001u
#define WBEM_E_NOT_FOUND 0x80041002u
#define WBEM_FLAG_RETURN_IMMEDIATELY 0x10u
#define WBEM_FLAG_FORWARD_ONLY 0x20u
#define WBEM_E_INVALID_PARAMETER 0x80041008u
#define WBEM_E_LOCAL_CREDENTIALS 0x80041064u

/* CIM types. */
#define CIM_STRING 8u
#define CIM_UINT32 19u
#define WBEM_FLAVOR_ORIGIN_PROPAGATED 0x20u

static const rt_guid IID_IWbemLocator = { 0xDC12A687, 0x737F, 0x11CF, { 0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
static const rt_guid IID_IWbemServices = { 0x9556DC99, 0x828C, 0x11CF, { 0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x32, 0x40, 0xC7 } };
static const rt_guid IID_IWbemClassObject = { 0xDC12A681, 0x737F, 0x11CF, { 0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };

/* UNCONFIRMED: IID_IEnumWbemClassObject from memory of wbemcli.h; the
   engine does not query it. */
static const rt_guid IID_IEnumWbemClassObject = { 0x027947E1, 0xD731, 0x11CE, { 0xA3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static const rt_com_iface locator_ifaces[] = { { &IID_IWbemLocator, 0 } };
static const rt_com_iface services_ifaces[] = { { &IID_IWbemServices, 0 } };
static const rt_com_iface object_ifaces[] = { { &IID_IWbemClassObject, 0 } };

static const rt_com_class locator_class = {
    "WbemLocator", 1, { &rt_vt_WbemLocator, NULL, NULL, NULL }, locator_ifaces, 1, NULL
};
static const rt_com_class services_class = {
    "WbemServices", 1, { &rt_vt_WbemServices, NULL, NULL, NULL }, services_ifaces, 1, NULL
};
static const rt_com_iface enum_ifaces[] = { { &IID_IEnumWbemClassObject, 0 } };
static const rt_com_class enum_class = {
    "Win32_PNPEntity enumerator", 1, { &rt_vt_EnumWbemClassObject, NULL, NULL, NULL }, enum_ifaces, 1, NULL
};
static const rt_com_class object_class = {
    "Win32_VideoController", 1, { &rt_vt_WbemClassObject, NULL, NULL, NULL }, object_ifaces, 1, NULL
};

rt_com_object *rt_wmi_create_locator(void)
{
    return rt_com_create(&locator_class, NULL);
}

/* Copy a guest UTF-16 string (at most n-1 characters, ASCII only) or stop
   the process with a message naming what. */
static void ascii_arg(uint32_t ws, char *out, size_t n, const char *what)
{
    if (!rt_wstr_to_ascii(ws, out, n)) rt_fatal("WMI: %s is not ASCII text of at most %zu characters", what, n - 1);
}

/* HRESULT IWbemLocator::ConnectServer(BSTR resource, BSTR user,
   BSTR password, BSTR locale, LONG flags, BSTR authority, IWbemContext *,
   IWbemServices **) */
void rt_com_WbemLocator_ConnectServer(_cpu *cpu)
{
    uint32_t res = ARG(1), user = ARG(2), password = ARG(3), pp = ARG(8);
    char ns[128];
    rt_com_object *o;
    (void)rt_com_this(ARG(0), 0, &locator_class);
    if (pp == 0 || res == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    wr32(pp, 0);
    if (user != 0 || password != 0)
    {
        RET(WBEM_E_LOCAL_CREDENTIALS); /* credentials are not allowed for a local connection */
        return;
    }
    ascii_arg(res, ns, sizeof ns, "the namespace");
    if (!rt_wstr_eq_ascii_nocase(res, "ROOT\\CIMV2") && !rt_wstr_eq_ascii_nocase(res, "\\\\.\\ROOT\\CIMV2"))
        rt_fatal("WMI: namespace \"%s\" is not supported", ns);
    o = rt_com_create(&services_class, NULL);
    if (o == NULL)
    {
        RET(E_OUTOFMEMORY);
        return;
    }
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* HRESULT IWbemServices::GetObject(BSTR path, LONG flags, IWbemContext *,
   IWbemClassObject **, IWbemCallResult **) */
void rt_com_WbemServices_GetObject(_cpu *cpu)
{
    static const char prefix[] = "Win32_VideoController.DeviceID=\"";
    uint32_t path = ARG(1), flags = ARG(2), pp = ARG(4), result = ARG(5);
    char text[256], id[64];
    size_t np = sizeof prefix - 1, n;
    rt_com_object *o;
    (void)rt_com_this(ARG(0), 0, &services_class);
    if (result != 0) rt_fatal("IWbemServices::GetObject with a call result (semisynchronous call) is not supported");
    if (flags != 0) rt_fatal("IWbemServices::GetObject with flags 0x%x is not supported", flags);
    if (path == 0) rt_fatal("IWbemServices::GetObject without an object path (empty class) is not supported");
    if (pp == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    wr32(pp, 0);
    ascii_arg(path, text, sizeof text, "the object path");
    if (strncasecmp(text, prefix, np) != 0)
        rt_fatal("WMI: object path \"%s\" is not supported", text);
    n = strlen(text + np);
    if (n < 2 || text[np + n - 1] != '"' || n - 1 >= sizeof id)
        rt_fatal("WMI: object path \"%s\" is not supported", text);
    memcpy(id, text + np, n - 1);
    id[n - 1] = 0;
    /* The emulated system has one video controller. */
    if (strcasecmp(id, "VideoController1") != 0)
    {
        RET(WBEM_E_NOT_FOUND);
        return;
    }
    o = rt_com_create(&object_class, NULL);
    if (o == NULL)
    {
        RET(E_OUTOFMEMORY);
        return;
    }
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* HRESULT IWbemServices::CreateInstanceEnum(BSTR class, LONG flags,
   IWbemContext *, IEnumWbemClassObject **) */
void rt_com_WbemServices_CreateInstanceEnum(_cpu *cpu)
{
    uint32_t cls = ARG(1), flags = ARG(2), ctx = ARG(3), pp = ARG(4);
    char name[64];
    rt_com_object *o;
    (void)rt_com_this(ARG(0), 0, &services_class);
    if (cls == 0 || pp == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    wr32(pp, 0);
    if ((flags & ~(WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY)) != 0)
        rt_fatal("IWbemServices::CreateInstanceEnum with flags 0x%x is not supported", flags);
    if (ctx != 0) rt_fatal("IWbemServices::CreateInstanceEnum with a context object is not supported");
    ascii_arg(cls, name, sizeof name, "the class name");
    if (strcasecmp(name, "Win32_PNPEntity") != 0)
        rt_fatal("WMI: the instance enumeration of class \"%s\" is not supported", name);
    o = rt_com_create(&enum_class, NULL);
    if (o == NULL)
    {
        RET(E_OUTOFMEMORY);
        return;
    }
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* HRESULT IEnumWbemClassObject::Reset(void) */
void rt_com_EnumWbemClassObject_Reset(_cpu *cpu)
{
    (void)rt_com_this(ARG(0), 0, &enum_class);
    RET(S_OK);
}

/* HRESULT IEnumWbemClassObject::Next(LONG timeout, ULONG count,
   IWbemClassObject **objects, ULONG *returned). The enumeration has no
   instances: Next returns WBEM_S_FALSE with no objects. */
void rt_com_EnumWbemClassObject_Next(_cpu *cpu)
{
    uint32_t count = ARG(2), objs = ARG(3), pret = ARG(4);
    (void)rt_com_this(ARG(0), 0, &enum_class);
    if (pret == 0 || (count != 0 && objs == 0))
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    wr32(pret, 0);
    RET(count == 0 ? S_OK : WBEM_S_FALSE);
}

/* HRESULT IWbemClassObject::Get(LPCWSTR name, LONG flags, VARIANT *value,
   CIMTYPE *type, LONG *flavor) */
void rt_com_WbemClassObject_Get(_cpu *cpu)
{
    uint32_t name = ARG(1), flags = ARG(2), pv = ARG(3), ptype = ARG(4), pflavor = ARG(5);
    char prop[64];
    const char *text = NULL;
    uint32_t cim, b = 0;
    (void)rt_com_this(ARG(0), 0, &object_class);
    if (name == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    if (flags != 0) rt_fatal("IWbemClassObject::Get with flags 0x%x is not supported", flags);
    ascii_arg(name, prop, sizeof prop, "the property name");
    if (strcasecmp(prop, "AdapterRAM") == 0)
    {
        cim = CIM_UINT32;
    }
    else
    {
        cim = CIM_STRING;
        if (strcasecmp(prop, "DeviceID") == 0)
            text = "VideoController1";
        else if (strcasecmp(prop, "Name") == 0 || strcasecmp(prop, "Caption") == 0 ||
                 strcasecmp(prop, "Description") == 0 || strcasecmp(prop, "VideoProcessor") == 0)
            text = rt_gpu_name();
        else
            rt_fatal("WMI: property Win32_VideoController.%s is not supported", prop);
    }
    if (pv != 0)
    {
        memset(G2H(pv), 0, 16);
        if (cim == CIM_UINT32)
        {
            /* WMI returns CIM_UINT32 values as VT_I4. */
            wr16(pv, VT_I4);
            wr32(pv + 8, rt_gpu_adapter_ram());
        }
        else
        {
            b = rt_bstr_alloc_ascii(text);
            if (b == 0)
            {
                RET(E_OUTOFMEMORY);
                return;
            }
            wr16(pv, VT_BSTR);
            wr32(pv + 8, b);
        }
    }
    if (ptype != 0) wr32(ptype, cim);
    if (pflavor != 0) wr32(pflavor, WBEM_FLAVOR_ORIGIN_PROPAGATED);
    RET(S_OK);
}
