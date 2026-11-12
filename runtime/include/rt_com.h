/*
 * COM support: GUIDs, guest-callable vtables and the runtime COM objects.
 *
 * A runtime COM object has a guest part and a host part.
 *
 * The guest part is one guest heap block:
 *   [block + 0]           object id (index into the host object table)
 *   [block + 4 + 4 * k]   guest address of the vtable of slot k
 * The interface pointer of slot k is block + 4 + 4 * k. A method of slot k
 * finds the object id at this - 4 * k - 4.
 *
 * The host part (rt_com_object) holds the class, the reference count and
 * the class data. Slot 0 is the IUnknown identity of the object.
 *
 * A vtable is an array of guest addresses of thunks (runtime/com.spec,
 * tools/gen_com_thunks.py). The runtime copies the thunk addresses into a
 * guest heap block the first time that an object uses the vtable.
 */
#ifndef RT_COM_H
#define RT_COM_H

#include "rt.h"

#include <stdbool.h>

/* HRESULT values. */
#define S_OK 0x00000000u
#define S_FALSE 0x00000001u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define E_OUTOFMEMORY 0x8007000Eu
#define E_INVALIDARG 0x80070057u
#define CLASS_E_NOAGGREGATION 0x80040110u
#define REGDB_E_CLASSNOTREG 0x80040154u
#define CO_E_NOTINITIALIZED 0x800401F0u
#define CO_E_CLASSSTRING 0x800401F3u
#define RPC_E_CHANGED_MODE 0x80010106u

typedef struct rt_guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} rt_guid;

/* Read and write a guest GUID (16 bytes). */
rt_guid rt_guid_read(uint32_t g);
void rt_guid_write(uint32_t g, const rt_guid *v);
bool rt_guid_eq(const rt_guid *a, const rt_guid *b);
/* Format as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (39 bytes with NUL). */
void rt_guid_format(const rt_guid *v, char out[39], bool braces, bool upper);
/* Parse "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" with optional braces (the
   braces must both be present or both be absent). Returns false if the
   text is not in this form. */
bool rt_guid_parse(const char *s, rt_guid *out, bool braces);

/* A vtable (generated, see runtime/com.spec). */
typedef struct rt_com_vtable {
    const char *interface;
    uint32_t count;
    char *const *thunks;  /* host addresses of the thunks */
    uint32_t guest;       /* guest copy; 0 until the first use */
} rt_com_vtable;

/* One interface of a class: the IID and the vtable slot that answers it. */
typedef struct rt_com_iface {
    const rt_guid *iid;
    int slot;
} rt_com_iface;

typedef struct rt_com_object rt_com_object;

typedef struct rt_com_class {
    const char *name;
    int nslots;
    rt_com_vtable *vtables[4];
    const rt_com_iface *ifaces; /* IUnknown is always slot 0 */
    int nifaces;
    /* Release the class data. Called when the last reference goes. */
    void (*destroy)(rt_com_object *o);
} rt_com_class;

struct rt_com_object {
    const rt_com_class *cls;
    uint32_t guest;   /* guest block (the object id word) */
    uint32_t id;
    uint32_t refs;    /* atomic; 0 when no object uses the entry (rt_ole32.c) */
    void *data;       /* class data (host memory owned by the class) */
};

extern const rt_guid rt_IID_IUnknown;

/* Create an object with one reference. data becomes owned by the object
   (the class destroy function releases it). Returns NULL when no guest
   memory is left; the caller then still owns data. */
rt_com_object *rt_com_create(const rt_com_class *cls, void *data);
/* Interface pointer of slot k. */
uint32_t rt_com_ptr(const rt_com_object *o, int slot);
/* The object of interface pointer this_ptr of vtable slot k of class cls.
   Stops the process if this_ptr is not such an interface pointer. */
rt_com_object *rt_com_this(uint32_t this_ptr, int slot, const rt_com_class *cls);
/* The object of any runtime interface pointer, or NULL if this_ptr is not
   the interface pointer of a runtime object. */
rt_com_object *rt_com_lookup(uint32_t this_ptr);
void rt_com_addref(rt_com_object *o);
/* Returns the new reference count. The object is destroyed at 0. */
uint32_t rt_com_release(rt_com_object *o);
/* QueryInterface semantics: writes the interface pointer (with a new
   reference) to the guest address ppv. */
uint32_t rt_com_query(rt_com_object *o, const rt_guid *iid, uint32_t ppv);

/* Release a guest interface pointer with a guest call of IUnknown::Release
   (vtable entry 2). */
uint32_t rt_com_guest_release(_cpu *cpu, uint32_t unk);

/* COM initialization state of the calling thread (CoInitialize). */
bool rt_com_thread_initialized(void);

/* Guest BSTR helpers (rt_oleaut32.c). A BSTR is a guest pointer to UTF-16
   characters; the byte length is in the dword before it. */
uint32_t rt_bstr_alloc_utf16(const uint16_t *s, uint32_t nchars); /* 0 when no memory */
uint32_t rt_bstr_alloc_ascii(const char *s);
void rt_bstr_free(uint32_t b);
uint32_t rt_bstr_len(uint32_t b); /* characters */
/* Release the error information object of the calling thread (called by
   CoUninitialize when the thread leaves COM). */
void rt_oleaut_thread_uninit(_cpu *cpu);

/* Compare a guest UTF-16 string with an ASCII string without regard to the
   case of ASCII letters. */
bool rt_wstr_eq_ascii_nocase(uint32_t ws, const char *s);
/* Copy a guest UTF-16 string into a host buffer as ASCII. Returns false if
   the string is longer than n-1 characters or has a character above 0x7F. */
bool rt_wstr_to_ascii(uint32_t ws, char *out, size_t n);

/* VARIANT (16 bytes): vt at +0, value at +8. */
#define VT_EMPTY 0u
#define VT_NULL 1u
#define VT_I2 2u
#define VT_I4 3u
#define VT_R4 4u
#define VT_R8 5u
#define VT_CY 6u
#define VT_DATE 7u
#define VT_BSTR 8u
#define VT_DISPATCH 9u
#define VT_ERROR 10u
#define VT_BOOL 11u
#define VT_VARIANT 12u
#define VT_UNKNOWN 13u
#define VT_DECIMAL 14u
#define VT_I1 16u
#define VT_UI1 17u
#define VT_UI2 18u
#define VT_UI4 19u
#define VT_I8 20u
#define VT_UI8 21u
#define VT_INT 22u
#define VT_UINT 23u
#define VT_ARRAY 0x2000u
#define VT_BYREF 0x4000u

#define DISP_E_TYPEMISMATCH 0x80020005u
#define DISP_E_BADVARTYPE 0x80020008u
#define DISP_E_OVERFLOW 0x8002000Au

/* GPU description for the guest (rt_gpu_info.m). */
/* Video memory that the emulated system reports, in bytes. */
uint32_t rt_gpu_adapter_ram(void);
/* Name of the Metal device ("Apple M2 Pro"). */
const char *rt_gpu_name(void);

#endif
