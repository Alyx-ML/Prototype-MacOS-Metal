/*
 * ADVAPI32 CryptoAPI functions for the "Microsoft Base Cryptographic
 * Provider v1.0" (PROV_RSA_FULL).
 *
 * The game uses:
 *   - CRYPT_VERIFYCONTEXT contexts with CALG_MD5 hashes;
 *   - an imported RSA PUBLICKEYBLOB and CryptEncrypt with CRYPT_OAEP;
 *   - CryptGenRandom.
 * MD5 is implemented here (RFC 1321). SHA-1 uses CommonCrypto. RSA
 * encryption uses the Security framework. CryptoAPI writes RSA output in
 * little-endian byte order, so the result is reversed.
 */
#include "rt.h"
#include "rt_object.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdbool.h>
#include <stdlib.h>

#define PROV_RSA_FULL 1u
#define PROV_RSA_AES 24u
#define CRYPT_VERIFYCONTEXT 0xF0000000u
#define CRYPT_NEWKEYSET 0x8u
#define CRYPT_DELETEKEYSET 0x10u
#define CRYPT_MACHINE_KEYSET 0x20u
#define CRYPT_SILENT 0x40u
#define CRYPT_OAEP 0x40u

#define CALG_MD5 0x8003u
#define CALG_SHA1 0x8004u
#define CALG_RSA_KEYX 0xA400u
#define CALG_RSA_SIGN 0x2400u

#define HP_ALGID 1u
#define HP_HASHVAL 2u
#define HP_HASHSIZE 4u

#define NTE_BAD_LEN 0x80090004u
#define NTE_BAD_DATA 0x80090005u
#define NTE_BAD_HASH 0x80090002u
#define NTE_BAD_KEY 0x80090003u
#define NTE_BAD_ALGID 0x80090008u
#define NTE_BAD_FLAGS 0x80090009u
#define NTE_BAD_TYPE 0x8009000Au
#define NTE_BAD_HASH_STATE 0x8009000Bu
#define NTE_BAD_UID 0x80090001u
#define NTE_EXISTS 0x8009000Fu
#define NTE_BAD_KEYSET_PARAM 0x8009001Fu
#define NTE_PROV_TYPE_NOT_DEF 0x80090017u

/* ---------------------------------------------------------------- MD5 (RFC 1321) */

typedef struct md5_ctx {
    uint32_t s[4];
    uint64_t len;
    uint8_t buf[64];
} md5_ctx;

static uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }

static void md5_block(md5_ctx *c, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const unsigned r[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t m[16], a = c->s[0], b = c->s[1], cc = c->s[2], d = c->s[3];
    unsigned i;
    for (i = 0; i < 16; i++)
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    for (i = 0; i < 64; i++)
    {
        uint32_t f, t;
        unsigned g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5u * i + 1u) % 16u; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3u * i + 5u) % 16u; }
        else { f = cc ^ (b | ~d); g = (7u * i) % 16u; }
        t = d;
        d = cc;
        cc = b;
        b = b + rol(a + f + k[i] + m[g], r[i]);
        a = t;
    }
    c->s[0] += a;
    c->s[1] += b;
    c->s[2] += cc;
    c->s[3] += d;
}

static void md5_init(md5_ctx *c)
{
    c->s[0] = 0x67452301;
    c->s[1] = 0xefcdab89;
    c->s[2] = 0x98badcfe;
    c->s[3] = 0x10325476;
    c->len = 0;
}

static void md5_update(md5_ctx *c, const uint8_t *p, size_t n)
{
    size_t have = (size_t)(c->len % 64u);
    c->len += n;
    if (have)
    {
        size_t k = 64 - have < n ? 64 - have : n;
        memcpy(c->buf + have, p, k);
        p += k;
        n -= k;
        if (have + k < 64) return;
        md5_block(c, c->buf);
    }
    while (n >= 64)
    {
        md5_block(c, p);
        p += 64;
        n -= 64;
    }
    memcpy(c->buf, p, n);
}

static void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->len * 8u;
    uint8_t pad[72] = { 0x80 };
    size_t have = (size_t)(c->len % 64u), padlen = (have < 56 ? 56 : 120) - have;
    unsigned i;
    for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, pad, padlen + 8);
    for (i = 0; i < 16; i++) out[i] = (uint8_t)(c->s[i / 4] >> (8 * (i % 4)));
}

/* ---------------------------------------------------------------- objects */

typedef enum { C_PROV = 1, C_HASH, C_KEY } crypt_kind;

typedef struct crypt_obj {
    crypt_kind kind;
    uint32_t algid;
    /* Hash */
    bool finished;
    md5_ctx md5;
    CC_SHA1_CTX sha1;
    uint8_t value[20];
    /* Key */
    SecKeyRef key;
    uint32_t modulus_bytes;
} crypt_obj;

static void crypt_destroy(rt_object *o)
{
    crypt_obj *c = o->data;
    if (c->key != NULL) CFRelease(c->key);
    free(c);
}

static uint32_t new_crypt(crypt_obj *c)
{
    rt_object *o = rt_object_new(OBJ_CRYPT);
    uint32_t h;
    o->data = c;
    o->destroy = crypt_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    return h;
}

/* The object of a handle with the kind, or NULL. The caller unrefs *o. */
static crypt_obj *get_crypt(uint32_t h, crypt_kind kind, rt_object **o)
{
    *o = rt_handle_get(h, OBJ_CRYPT);
    if (*o == NULL) return NULL;
    if (((crypt_obj *)(*o)->data)->kind != kind)
    {
        rt_object_unref(*o);
        *o = NULL;
        return NULL;
    }
    return (*o)->data;
}

static void fail(_cpu *cpu, uint32_t err)
{
    rt_set_last_error(err);
    RET(0);
}

static crypt_obj *alloc_obj(crypt_kind kind)
{
    crypt_obj *c = calloc(1, sizeof(*c));
    if (c == NULL) rt_fatal("out of host memory");
    c->kind = kind;
    return c;
}

/* ---------------------------------------------------------------- contexts */

/* BOOL CryptAcquireContextA(HCRYPTPROV *, LPCSTR container, LPCSTR provider,
   DWORD type, DWORD flags). The default key container of the user exists
   (no key pairs are used). */
void rt_CryptAcquireContextA(_cpu *cpu)
{
    uint32_t ph = ARG(0), container = ARG(1), provider = ARG(2), type = ARG(3), flags = ARG(4), h;
    if (ph == 0)
    {
        fail(cpu, ERROR_INVALID_PARAMETER);
        return;
    }
    if (type != PROV_RSA_FULL && type != PROV_RSA_AES)
        rt_fatal("CryptAcquireContextA: provider type %u is not supported", type);
    if (provider != 0)
    {
        const char *p = G2H(provider);
        if (strcmp(p, "Microsoft Base Cryptographic Provider v1.0") != 0 &&
            strcmp(p, "Microsoft Enhanced Cryptographic Provider v1.0") != 0 &&
            strcmp(p, "Microsoft Strong Cryptographic Provider") != 0)
            rt_fatal("CryptAcquireContextA: provider \"%s\" is not supported", p);
    }
    if (container != 0 && !(flags & CRYPT_VERIFYCONTEXT))
        rt_fatal("CryptAcquireContextA: named key containers are not supported");
    if (flags & ~(CRYPT_VERIFYCONTEXT | CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET | CRYPT_SILENT))
    {
        fail(cpu, NTE_BAD_FLAGS);
        return;
    }
    if ((flags & CRYPT_NEWKEYSET) && !(flags & CRYPT_VERIFYCONTEXT))
    {
        fail(cpu, NTE_EXISTS);
        return;
    }
    h = new_crypt(alloc_obj(C_PROV));
    if (h == 0)
    {
        fail(cpu, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    wr32(ph, h);
    RET(1);
}

/* BOOL CryptReleaseContext(HCRYPTPROV, DWORD flags) */
void rt_CryptReleaseContext(_cpu *cpu)
{
    rt_object *o;
    if (get_crypt(ARG(0), C_PROV, &o) == NULL)
    {
        fail(cpu, NTE_BAD_UID);
        return;
    }
    rt_object_unref(o);
    if (ARG(1) != 0)
    {
        fail(cpu, NTE_BAD_FLAGS);
        return;
    }
    rt_handle_close(ARG(0));
    RET(1);
}

/* BOOL CryptGenRandom(HCRYPTPROV, DWORD len, BYTE *buf) */
void rt_CryptGenRandom(_cpu *cpu)
{
    rt_object *o;
    uint32_t len = ARG(1), buf = ARG(2);
    if (get_crypt(ARG(0), C_PROV, &o) == NULL)
    {
        fail(cpu, NTE_BAD_UID);
        return;
    }
    rt_object_unref(o);
    if (len != 0 && buf == 0)
    {
        fail(cpu, ERROR_INVALID_PARAMETER);
        return;
    }
    if (len != 0) arc4random_buf(G2H(buf), len);
    RET(1);
}

/* ---------------------------------------------------------------- hashes */

/* BOOL CryptCreateHash(HCRYPTPROV, ALG_ID, HCRYPTKEY, DWORD flags,
   HCRYPTHASH *) */
void rt_CryptCreateHash(_cpu *cpu)
{
    uint32_t algid = ARG(1), key = ARG(2), flags = ARG(3), ph = ARG(4), h;
    rt_object *o;
    crypt_obj *c;
    if (get_crypt(ARG(0), C_PROV, &o) == NULL)
    {
        fail(cpu, NTE_BAD_UID);
        return;
    }
    rt_object_unref(o);
    if (algid != CALG_MD5 && algid != CALG_SHA1) rt_fatal("CryptCreateHash: algorithm 0x%x is not supported", algid);
    if (key != 0 || flags != 0 || ph == 0)
    {
        fail(cpu, key != 0 ? NTE_BAD_KEY : flags != 0 ? NTE_BAD_FLAGS : ERROR_INVALID_PARAMETER);
        return;
    }
    c = alloc_obj(C_HASH);
    c->algid = algid;
    if (algid == CALG_MD5) md5_init(&c->md5);
    else CC_SHA1_Init(&c->sha1);
    h = new_crypt(c);
    if (h == 0)
    {
        fail(cpu, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    wr32(ph, h);
    RET(1);
}

/* BOOL CryptHashData(HCRYPTHASH, const BYTE *, DWORD len, DWORD flags) */
void rt_CryptHashData(_cpu *cpu)
{
    uint32_t data = ARG(1), len = ARG(2), flags = ARG(3);
    rt_object *o;
    crypt_obj *c = get_crypt(ARG(0), C_HASH, &o);
    if (c == NULL)
    {
        fail(cpu, NTE_BAD_HASH);
        return;
    }
    if (flags != 0 || (len != 0 && data == 0) || c->finished)
    {
        rt_object_unref(o);
        fail(cpu, c->finished ? NTE_BAD_HASH_STATE : flags != 0 ? NTE_BAD_FLAGS : ERROR_INVALID_PARAMETER);
        return;
    }
    if (len != 0)
    {
        if (c->algid == CALG_MD5) md5_update(&c->md5, G2H(data), len);
        else CC_SHA1_Update(&c->sha1, G2H(data), len);
    }
    rt_object_unref(o);
    RET(1);
}

/* BOOL CryptGetHashParam(HCRYPTHASH, DWORD param, BYTE *data, DWORD *len,
   DWORD flags) */
void rt_CryptGetHashParam(_cpu *cpu)
{
    uint32_t param = ARG(1), data = ARG(2), plen = ARG(3), flags = ARG(4), size, need;
    uint8_t tmp[20];
    const uint8_t *src;
    rt_object *o;
    crypt_obj *c = get_crypt(ARG(0), C_HASH, &o);
    if (c == NULL)
    {
        fail(cpu, NTE_BAD_HASH);
        return;
    }
    if (flags != 0 || plen == 0)
    {
        rt_object_unref(o);
        fail(cpu, flags != 0 ? NTE_BAD_FLAGS : ERROR_INVALID_PARAMETER);
        return;
    }
    size = c->algid == CALG_MD5 ? 16u : 20u;
    switch (param)
    {
    case HP_ALGID:
        memcpy(tmp, &c->algid, 4);
        src = tmp;
        need = 4;
        break;
    case HP_HASHSIZE:
        memcpy(tmp, &size, 4);
        src = tmp;
        need = 4;
        break;
    case HP_HASHVAL:
        if (!c->finished)
        {
            if (c->algid == CALG_MD5) md5_final(&c->md5, c->value);
            else CC_SHA1_Final(c->value, &c->sha1);
            c->finished = true;
        }
        src = c->value;
        need = size;
        break;
    default:
        rt_object_unref(o);
        rt_fatal("CryptGetHashParam: parameter %u is not supported", param);
    }
    if (data == 0)
    {
        wr32(plen, need);
        rt_object_unref(o);
        RET(1);
        return;
    }
    if (rd32(plen) < need)
    {
        wr32(plen, need);
        rt_object_unref(o);
        fail(cpu, ERROR_MORE_DATA);
        return;
    }
    memcpy(G2H(data), src, need);
    wr32(plen, need);
    rt_object_unref(o);
    RET(1);
}

/* BOOL CryptDestroyHash(HCRYPTHASH) */
void rt_CryptDestroyHash(_cpu *cpu)
{
    rt_object *o;
    if (get_crypt(ARG(0), C_HASH, &o) == NULL)
    {
        fail(cpu, NTE_BAD_HASH);
        return;
    }
    rt_object_unref(o);
    rt_handle_close(ARG(0));
    RET(1);
}

/* ---------------------------------------------------------------- keys */

/* DER length bytes. Returns the number of bytes written to out. */
static size_t der_len(uint8_t *out, size_t n)
{
    if (n < 0x80)
    {
        out[0] = (uint8_t)n;
        return 1;
    }
    if (n < 0x100)
    {
        out[0] = 0x81;
        out[1] = (uint8_t)n;
        return 2;
    }
    out[0] = 0x82;
    out[1] = (uint8_t)(n >> 8);
    out[2] = (uint8_t)n;
    return 3;
}

/* DER INTEGER from a big-endian unsigned number. */
static size_t der_int(uint8_t *out, const uint8_t *be, size_t n)
{
    size_t pos = 0, lead;
    while (n > 1 && be[0] == 0)
    {
        be++;
        n--;
    }
    lead = (be[0] & 0x80) ? 1 : 0;
    out[pos++] = 0x02;
    pos += der_len(out + pos, n + lead);
    if (lead) out[pos++] = 0;
    memcpy(out + pos, be, n);
    return pos + n;
}

/* BOOL CryptImportKey(HCRYPTPROV, const BYTE *blob, DWORD len,
   HCRYPTKEY pubkey, DWORD flags, HCRYPTKEY *). PUBLICKEYBLOB only. */
void rt_CryptImportKey(_cpu *cpu)
{
    uint32_t blob = ARG(1), len = ARG(2), pub = ARG(3), flags = ARG(4), ph = ARG(5), h;
    uint32_t alg, magic, bits, pubexp, mb, i;
    const uint8_t *b;
    uint8_t *modulus_be, exp_be[4], *der, *body;
    size_t pos, blen;
    rt_object *o;
    CFDataRef cfd;
    CFMutableDictionaryRef attrs;
    CFErrorRef err = NULL;
    SecKeyRef key;
    crypt_obj *c;

    if (get_crypt(ARG(0), C_PROV, &o) == NULL)
    {
        fail(cpu, NTE_BAD_UID);
        return;
    }
    rt_object_unref(o);
    if (blob == 0 || ph == 0 || len < 20)
    {
        fail(cpu, ERROR_INVALID_PARAMETER);
        return;
    }
    b = G2H(blob);
    if (b[0] != 0x06) rt_fatal("CryptImportKey: key blob type %u is not supported (PUBLICKEYBLOB only)", b[0]);
    if (pub != 0 || flags != 0) rt_fatal("CryptImportKey: import with a key or flags is not supported");
    memcpy(&alg, b + 4, 4);
    memcpy(&magic, b + 8, 4);
    memcpy(&bits, b + 12, 4);
    memcpy(&pubexp, b + 16, 4);
    if (b[1] != 2 || (alg != CALG_RSA_KEYX && alg != CALG_RSA_SIGN) || magic != 0x31415352u || bits == 0 || bits % 8 != 0 ||
        bits > 16384 || len < 20u + bits / 8u)
    {
        fail(cpu, NTE_BAD_DATA);
        return;
    }
    mb = bits / 8u;
    modulus_be = malloc(mb);
    der = malloc((size_t)mb + 64);
    body = malloc((size_t)mb + 64);
    if (modulus_be == NULL || der == NULL || body == NULL) rt_fatal("out of host memory");
    for (i = 0; i < mb; i++) modulus_be[i] = b[20 + mb - 1 - i];
    for (i = 0; i < 4; i++) exp_be[i] = (uint8_t)(pubexp >> (24 - 8 * i));
    /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
    blen = der_int(body, modulus_be, mb);
    blen += der_int(body + blen, exp_be, 4);
    pos = 0;
    der[pos++] = 0x30;
    pos += der_len(der + pos, blen);
    memcpy(der + pos, body, blen);
    pos += blen;

    cfd = CFDataCreate(kCFAllocatorDefault, der, (CFIndex)pos);
    attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (cfd == NULL || attrs == NULL) rt_fatal("out of host memory");
    CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeRSA);
    CFDictionarySetValue(attrs, kSecAttrKeyClass, kSecAttrKeyClassPublic);
    key = SecKeyCreateWithData(cfd, attrs, &err);
    CFRelease(cfd);
    CFRelease(attrs);
    free(modulus_be);
    free(der);
    free(body);
    if (key == NULL)
    {
        if (err != NULL) CFRelease(err);
        fail(cpu, NTE_BAD_DATA);
        return;
    }
    c = alloc_obj(C_KEY);
    c->algid = alg;
    c->key = key;
    c->modulus_bytes = mb;
    h = new_crypt(c);
    if (h == 0)
    {
        fail(cpu, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    wr32(ph, h);
    RET(1);
}

/* BOOL CryptEncrypt(HCRYPTKEY, HCRYPTHASH, BOOL final, DWORD flags,
   BYTE *data, DWORD *len, DWORD buflen): RSA PKCS #1 v1.5 or OAEP
   (SHA-1), output in little-endian byte order. */
void rt_CryptEncrypt(_cpu *cpu)
{
    uint32_t hash = ARG(1), final = ARG(2), flags = ARG(3), data = ARG(4), plen = ARG(5), buflen = ARG(6);
    uint32_t in_len, k, i;
    rt_object *o;
    crypt_obj *c = get_crypt(ARG(0), C_KEY, &o);
    CFDataRef plain, cipher;
    CFErrorRef err = NULL;
    const uint8_t *cb;

    if (c == NULL)
    {
        fail(cpu, NTE_BAD_KEY);
        return;
    }
    k = c->modulus_bytes;
    if (hash != 0) rt_fatal("CryptEncrypt: hashing during encryption is not supported");
    if (final == 0 || (flags & ~CRYPT_OAEP) != 0 || plen == 0)
    {
        rt_object_unref(o);
        fail(cpu, final == 0 ? NTE_BAD_DATA : plen == 0 ? ERROR_INVALID_PARAMETER : NTE_BAD_FLAGS);
        return;
    }
    in_len = rd32(plen);
    if (in_len > k - ((flags & CRYPT_OAEP) ? 42u : 11u))
    {
        rt_object_unref(o);
        fail(cpu, NTE_BAD_LEN);
        return;
    }
    if (data == 0)
    {
        wr32(plen, k);
        rt_object_unref(o);
        RET(1);
        return;
    }
    if (buflen < k)
    {
        wr32(plen, k);
        rt_object_unref(o);
        fail(cpu, ERROR_MORE_DATA);
        return;
    }
    /* CryptoAPI input is in the byte order of the message; the output is
       reversed. */
    plain = CFDataCreate(kCFAllocatorDefault, G2H(data), (CFIndex)in_len);
    if (plain == NULL) rt_fatal("out of host memory");
    cipher = SecKeyCreateEncryptedData(c->key,
                                       (flags & CRYPT_OAEP) ? kSecKeyAlgorithmRSAEncryptionOAEPSHA1 : kSecKeyAlgorithmRSAEncryptionPKCS1,
                                       plain, &err);
    CFRelease(plain);
    if (cipher == NULL || (uint32_t)CFDataGetLength(cipher) != k)
    {
        if (cipher != NULL) CFRelease(cipher);
        if (err != NULL) CFRelease(err);
        rt_object_unref(o);
        rt_fatal("CryptEncrypt: RSA encryption failed in the Security framework");
    }
    cb = CFDataGetBytePtr(cipher);
    for (i = 0; i < k; i++) wr8(data + i, cb[k - 1 - i]);
    CFRelease(cipher);
    wr32(plen, k);
    rt_object_unref(o);
    RET(1);
}

/* BOOL CryptDestroyKey(HCRYPTKEY) */
void rt_CryptDestroyKey(_cpu *cpu)
{
    rt_object *o;
    if (get_crypt(ARG(0), C_KEY, &o) == NULL)
    {
        fail(cpu, NTE_BAD_KEY);
        return;
    }
    rt_object_unref(o);
    rt_handle_close(ARG(0));
    RET(1);
}
