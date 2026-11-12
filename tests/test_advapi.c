/*
 * Unit tests of the ADVAPI32 registry and CryptoAPI functions
 * (runtime/src/rt_registry.c, runtime/src/rt_crypt.c).
 *
 * MD5 and SHA-1 use the test vectors of RFC 1321 and FIPS 180. The RSA
 * test makes a key pair with the Security framework, imports the public
 * key as a PUBLICKEYBLOB, encrypts with CryptEncrypt and decrypts the
 * result with the private key.
 */
#include "harness.h"
#include "rt_registry.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void rt_RegOpenKeyA(_cpu *cpu);
void rt_RegOpenKeyExA(_cpu *cpu);
void rt_RegCreateKeyExA(_cpu *cpu);
void rt_RegCloseKey(_cpu *cpu);
void rt_RegEnumKeyA(_cpu *cpu);
void rt_RegQueryValueExA(_cpu *cpu);
void rt_RegSetValueExA(_cpu *cpu);
void rt_CryptAcquireContextA(_cpu *cpu);
void rt_CryptReleaseContext(_cpu *cpu);
void rt_CryptGenRandom(_cpu *cpu);
void rt_CryptCreateHash(_cpu *cpu);
void rt_CryptHashData(_cpu *cpu);
void rt_CryptGetHashParam(_cpu *cpu);
void rt_CryptDestroyHash(_cpu *cpu);
void rt_CryptImportKey(_cpu *cpu);
void rt_CryptEncrypt(_cpu *cpu);
void rt_CryptDestroyKey(_cpu *cpu);

#define HKLM 0x80000002u
#define HKCU 0x80000001u

static uint32_t query(uint32_t key, const char *name, uint32_t ptype, uint32_t data, uint32_t pcb)
{
    argv_t a = { .n = 0 };
    a32(&a, key);
    a32(&a, gs(name));
    a32(&a, 0);
    a32(&a, ptype);
    a32(&a, data);
    a32(&a, pcb);
    return callv(rt_RegQueryValueExA, &a);
}

static void test_registry(const char *file)
{
    uint32_t phk = gbuf(4), ptype = gbuf(4), data = gbuf(256), pcb = gbuf(4), disp = gbuf(4), name = gbuf(64), key;
    argv_t a;

    rt_registry_init(file);

    check_u32("open Steam key", call3(rt_RegOpenKeyA, HKLM, gs("software\\activision\\PROTOTYPE"), phk), 0);
    key = rd32(phk);
    wr32(pcb, 256);
    check_u32("query FE_Language", query(key, "FE_Language", ptype, data, pcb), 0);
    check_u32("FE_Language type", rd32(ptype), 1);
    check_str("FE_Language value", data, "69");
    check_u32("FE_Language size", rd32(pcb), 3);
    wr32(pcb, 256);
    query(key, "Path", ptype, data, pcb);
    check_str("Path value", data, "C:\\Prototype");
    wr32(pcb, 2);
    check_u32("small buffer", query(key, "Path", ptype, data, pcb), 234);
    check_u32("small buffer size", rd32(pcb), 13);
    check_u32("size query", query(key, "Path", 0, 0, pcb), 0);
    check_u32("missing value", query(key, "NoSuchValue", 0, 0, pcb), 2);
    check_u32("close", call1(rt_RegCloseKey, key), 0);
    check_u32("closed handle", query(key, "Path", 0, 0, pcb), 6);
    check_u32("missing key", call3(rt_RegOpenKeyA, HKLM, gs("software\\asio"), phk), 2);

    /* Create, set and enumerate. */
    a = (argv_t){ .n = 0 };
    a32(&a, HKCU);
    a32(&a, gs("Software\\Test\\Sub"));
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 0xF003F);
    a32(&a, 0);
    a32(&a, phk);
    a32(&a, disp);
    check_u32("create", callv(rt_RegCreateKeyExA, &a), 0);
    check_u32("create disposition", rd32(disp), 1);
    key = rd32(phk);
    wr32(data, 0x12345678u);
    a = (argv_t){ .n = 0 };
    a32(&a, key);
    a32(&a, gs("Number"));
    a32(&a, 0);
    a32(&a, 4);
    a32(&a, data);
    a32(&a, 4);
    check_u32("set DWORD", callv(rt_RegSetValueExA, &a), 0);
    wr32(data, 0);
    wr32(pcb, 4);
    check_u32("query DWORD", query(key, "number", ptype, data, pcb), 0);
    check_u32("DWORD value", rd32(data), 0x12345678u);
    check_u32("DWORD type", rd32(ptype), 4);
    call1(rt_RegCloseKey, key);
    a = (argv_t){ .n = 0 };
    a32(&a, HKCU);
    a32(&a, gs("Software\\Test\\Sub"));
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 0xF003F);
    a32(&a, 0);
    a32(&a, phk);
    a32(&a, disp);
    callv(rt_RegCreateKeyExA, &a);
    check_u32("open disposition", rd32(disp), 2);
    call1(rt_RegCloseKey, rd32(phk));

    check_u32("open Software", call3(rt_RegOpenKeyA, HKCU, gs("Software"), phk), 0);
    key = rd32(phk);
    check_u32("open with an empty name", call3(rt_RegOpenKeyA, key, gs(""), phk), 0);
    check_u32("empty name gives the same handle", rd32(phk), key);
    check_u32("enum 0", call4(rt_RegEnumKeyA, key, 0, name, 64), 0);
    check_str("enum 0 name", name, "Test");
    check_u32("enum 1", call4(rt_RegEnumKeyA, key, 1, name, 64), 259);
    call1(rt_RegCloseKey, key);

    /* The file has the new value. */
    {
        FILE *f = fopen(file, "r");
        char line[512];
        bool found = false;
        if (f != NULL)
        {
            while (fgets(line, sizeof(line), f) != NULL)
                if (strcmp(line, "V\tHKEY_CURRENT_USER\\Software\\Test\\Sub\tNumber\t4\t78563412\n") == 0) found = true;
            fclose(f);
        }
        check_u32("registry file has the value", found, 1);
    }
}

static void hash_hex(uint32_t prov, uint32_t alg, const char *msg, char *out)
{
    uint32_t ph = gbuf(4), val = gbuf(32), plen = gbuf(4), hh, n, i;
    argv_t a = { .n = 0 };
    a32(&a, prov);
    a32(&a, alg);
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, ph);
    check_u32("CryptCreateHash", callv(rt_CryptCreateHash, &a), 1);
    hh = rd32(ph);
    check_u32("CryptHashData", call4(rt_CryptHashData, hh, gs(msg), (uint32_t)strlen(msg), 0), 1);
    wr32(plen, 32);
    a = (argv_t){ .n = 0 };
    a32(&a, hh);
    a32(&a, 2);
    a32(&a, val);
    a32(&a, plen);
    a32(&a, 0);
    check_u32("CryptGetHashParam", callv(rt_CryptGetHashParam, &a), 1);
    n = rd32(plen);
    for (i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", rd8(val + i));
    check_u32("hash data after the value fails", call4(rt_CryptHashData, hh, gs("x"), 1, 0), 0);
    check_u32("CryptDestroyHash", call1(rt_CryptDestroyHash, hh), 1);
}

static void check_hash(uint32_t prov, uint32_t alg, const char *msg, const char *want)
{
    char out[64];
    hash_hex(prov, alg, msg, out);
    checks++;
    if (strcmp(out, want) != 0)
    {
        failures++;
        printf("FAIL hash of \"%s\": got %s, want %s\n", msg, out, want);
    }
}

/* Read a DER INTEGER at p; returns its length and sets *v. */
static size_t der_read_int(const uint8_t *p, const uint8_t **v, size_t *vn)
{
    size_t pos = 1, n;
    if (p[0] != 0x02) return 0;
    if (p[1] < 0x80) { n = p[1]; pos = 2; }
    else if (p[1] == 0x81) { n = p[2]; pos = 3; }
    else { n = ((size_t)p[2] << 8) | p[3]; pos = 4; }
    *v = p + pos;
    *vn = n;
    return pos + n;
}

static void test_crypt(void)
{
    uint32_t ph = gbuf(4), prov, rnd = gbuf(32);
    argv_t a = { .n = 0 };

    a32(&a, ph);
    a32(&a, 0);
    a32(&a, 0);
    a32(&a, 1);
    a32(&a, 0xF0000020u);
    check_u32("CryptAcquireContextA", callv(rt_CryptAcquireContextA, &a), 1);
    prov = rd32(ph);

    check_hash(prov, 0x8003, "", "d41d8cd98f00b204e9800998ecf8427e");
    check_hash(prov, 0x8003, "abc", "900150983cd24fb0d6963f7d28e17f72");
    check_hash(prov, 0x8003, "message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    check_hash(prov, 0x8003, "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
               "57edf4a22be3c955ac49da2e2107b67a");
    check_hash(prov, 0x8004, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d");

    check_u32("CryptGenRandom", call3(rt_CryptGenRandom, prov, 32, rnd), 1);

    /* RSA OAEP round trip. */
    {
        int bits = 1024;
        CFNumberRef nbits = CFNumberCreate(NULL, kCFNumberIntType, &bits);
        const void *keys[2] = { kSecAttrKeyType, kSecAttrKeySizeInBits };
        const void *vals[2] = { kSecAttrKeyTypeRSA, nbits };
        CFDictionaryRef params = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFErrorRef err = NULL;
        SecKeyRef priv = SecKeyCreateRandomKey(params, &err), pubk;
        CFDataRef pubder;
        const uint8_t *d, *mod, *ex;
        size_t mn, en, pos;
        uint32_t blob = gbuf(256), pkey = gbuf(4), buf = gbuf(256), plen = gbuf(4), key, i;
        uint8_t cipher_be[128];
        CFDataRef cd, plain;

        CFRelease(params);
        CFRelease(nbits);
        if (priv == NULL)
        {
            failures++;
            printf("FAIL RSA key generation\n");
            return;
        }
        pubk = SecKeyCopyPublicKey(priv);
        pubder = SecKeyCopyExternalRepresentation(pubk, &err);
        d = CFDataGetBytePtr(pubder);
        pos = (d[1] < 0x80) ? 2 : (d[1] == 0x81 ? 3 : 4);
        pos += der_read_int(d + pos, &mod, &mn);
        der_read_int(d + pos, &ex, &en);
        while (mn > 128 && mod[0] == 0) { mod++; mn--; }

        /* PUBLICKEYBLOB: BLOBHEADER, RSAPUBKEY, little-endian modulus. */
        wr8(blob, 6);
        wr8(blob + 1, 2);
        wr32(blob + 4, 0xA400);
        wr32(blob + 8, 0x31415352);
        wr32(blob + 12, 1024);
        {
            uint32_t e = 0;
            for (i = 0; i < en; i++) e = (e << 8) | ex[i];
            wr32(blob + 16, e);
        }
        for (i = 0; i < 128; i++) wr8(blob + 20 + i, mod[127 - i]);

        a = (argv_t){ .n = 0 };
        a32(&a, prov);
        a32(&a, blob);
        a32(&a, 148);
        a32(&a, 0);
        a32(&a, 0);
        a32(&a, pkey);
        check_u32("CryptImportKey", callv(rt_CryptImportKey, &a), 1);
        key = rd32(pkey);

        memcpy(G2H(buf), "secret message", 14);
        wr32(plen, 14);
        a = (argv_t){ .n = 0 };
        a32(&a, key);
        a32(&a, 0);
        a32(&a, 1);
        a32(&a, 0x40);
        a32(&a, buf);
        a32(&a, plen);
        a32(&a, 256);
        check_u32("CryptEncrypt OAEP", callv(rt_CryptEncrypt, &a), 1);
        check_u32("CryptEncrypt length", rd32(plen), 128);
        for (i = 0; i < 128; i++) cipher_be[i] = rd8(buf + 127 - i);
        cd = CFDataCreate(NULL, cipher_be, 128);
        plain = SecKeyCreateDecryptedData(priv, kSecKeyAlgorithmRSAEncryptionOAEPSHA1, cd, &err);
        checks++;
        if (plain == NULL || CFDataGetLength(plain) != 14 || memcmp(CFDataGetBytePtr(plain), "secret message", 14) != 0)
        {
            failures++;
            printf("FAIL RSA OAEP round trip\n");
        }
        if (plain != NULL) CFRelease(plain);
        CFRelease(cd);

        wr32(plen, 100);
        check_u32("CryptEncrypt too long", callv(rt_CryptEncrypt, &a), 0);
        check_u32("CryptDestroyKey", call1(rt_CryptDestroyKey, key), 1);
        CFRelease(pubder);
        CFRelease(pubk);
        CFRelease(priv);
    }
    check_u32("CryptReleaseContext", call2(rt_CryptReleaseContext, prov, 0), 1);
    check_u32("released context", call3(rt_CryptGenRandom, prov, 4, rnd), 0);
}

int main(void)
{
    char dir[1024], file[1100];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/rt_test_advapi.XXXXXX", tmp != NULL ? tmp : "/tmp");
    if (mkdtemp(dir) == NULL)
    {
        perror("mkdtemp");
        return 2;
    }
    snprintf(file, sizeof(file), "%s/registry.txt", dir);
    harness_init();
    test_registry(file);
    test_crypt();
    return harness_report();
}
