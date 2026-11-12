/*
 * Undecoration of Microsoft C++ type names for type_info::name.
 *
 * Input: the decorated name of a type descriptor without the leading '.',
 * for example "?AV?$StringC@$0EA@@container@@". Output: the type in the
 * form of the Microsoft undecorator (UNDNAME_TYPE_ONLY), for example
 * "class container::StringC<64>". Template argument lists use ',' without
 * a space and a space between two closing '>' characters; function
 * parameter lists use ',' without a space.
 *
 * Supported: the basic types, class, struct, union and enum types,
 * pointers and references with const and volatile, template instantiation
 * names with type and integer arguments, name and type back-references,
 * numbered and anonymous namespace scopes, and function scopes (local
 * classes) with their function signature. Other encodings (function
 * pointers, member pointers, arrays, pointer template arguments) give an
 * error; tests/test_rtti.c checks every type name of the game.
 */
#include "rt_crt.h"

#include <pthread.h>
#include <stdio.h>

#define ARENA 32768u
#define MAXREF 10

typedef struct ctx {
    const char *names[MAXREF];
    int nnames;
    const char *types[MAXREF];
    int ntypes;
} ctx;

typedef struct parser {
    const char *p;
    char arena[ARENA];
    size_t used;
    const char *error;
} parser;

static void fail(parser *ps, const char *why)
{
    if (ps->error == NULL) ps->error = why;
}

/* Copy n bytes of s into the arena as a NUL-terminated string. */
static const char *dup_n(parser *ps, const char *s, size_t n)
{
    char *d;
    if (ps->error != NULL) return "";
    if (n + 1 > ARENA - ps->used)
    {
        fail(ps, "name too long");
        return "";
    }
    d = ps->arena + ps->used;
    memcpy(d, s, n);
    d[n] = 0;
    ps->used += n + 1;
    return d;
}

/* Concatenate up to 6 strings (NULL ends the list early). */
static const char *cat(parser *ps, const char *a, const char *b, const char *c, const char *d, const char *e, const char *f)
{
    const char *parts[6] = { a, b, c, d, e, f };
    size_t n = 0, i;
    char *out;
    if (ps->error != NULL) return "";
    for (i = 0; i < 6 && parts[i] != NULL; i++) n += strlen(parts[i]);
    if (n + 1 > ARENA - ps->used)
    {
        fail(ps, "name too long");
        return "";
    }
    out = ps->arena + ps->used;
    n = 0;
    for (i = 0; i < 6 && parts[i] != NULL; i++)
    {
        size_t l = strlen(parts[i]);
        memcpy(out + n, parts[i], l);
        n += l;
    }
    out[n] = 0;
    ps->used += n + 1;
    return out;
}

static void memorize_name(ctx *c, const char *s)
{
    int i;
    for (i = 0; i < c->nnames; i++)
        if (strcmp(c->names[i], s) == 0) return;
    if (c->nnames < MAXREF) c->names[c->nnames++] = s;
}

/* Encoded number: '?' negative; '0'..'9' = 1..10; hex digits 'A'..'P'
   ended by '@'. */
static bool parse_number(parser *ps, int64_t *out)
{
    bool neg = false;
    uint64_t v = 0;
    if (*ps->p == '?')
    {
        neg = true;
        ps->p++;
    }
    if (*ps->p >= '0' && *ps->p <= '9')
    {
        v = (uint64_t)(*ps->p - '0') + 1u;
        ps->p++;
    }
    else
    {
        int digits = 0;
        while (*ps->p >= 'A' && *ps->p <= 'P')
        {
            if (++digits > 16)
            {
                fail(ps, "number too long");
                return false;
            }
            v = v * 16u + (uint64_t)(*ps->p - 'A');
            ps->p++;
        }
        if (*ps->p != '@')
        {
            fail(ps, "invalid number");
            return false;
        }
        ps->p++;
    }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return true;
}

static const char *parse_type(parser *ps, ctx *c);
static const char *parse_qualified_name(parser *ps, ctx *c);
static const char *parse_symbol(parser *ps, ctx *c);

static const char *parse_identifier(parser *ps)
{
    const char *start = ps->p, *at = strchr(start, '@');
    if (at == NULL || at == start)
    {
        fail(ps, "invalid identifier");
        return "";
    }
    ps->p = at + 1;
    return dup_n(ps, start, (size_t)(at - start));
}

/* Template instantiation name after "?$": identifier, arguments, '@'. The
   arguments have their own back-reference tables. */
static const char *parse_template_name(parser *ps)
{
    ctx inner;
    const char *name, *args = "";
    bool first = true;
    memset(&inner, 0, sizeof(inner));
    name = parse_identifier(ps);
    memorize_name(&inner, name);
    while (ps->error == NULL && *ps->p != '@')
    {
        const char *arg;
        if (*ps->p == 0)
        {
            fail(ps, "unterminated template argument list");
            return "";
        }
        if (ps->p[0] == '$' && ps->p[1] == '0')
        {
            int64_t v;
            char num[32];
            ps->p += 2;
            if (!parse_number(ps, &v)) return "";
            snprintf(num, sizeof(num), "%lld", (long long)v);
            arg = dup_n(ps, num, strlen(num));
        }
        else if (ps->p[0] == '$')
        {
            fail(ps, "unsupported template argument kind");
            return "";
        }
        else
        {
            const char *start = ps->p;
            arg = parse_type(ps, &inner);
            if (ps->p - start > 1 && inner.ntypes < MAXREF) inner.types[inner.ntypes++] = arg;
        }
        args = first ? arg : cat(ps, args, ",", arg, NULL, NULL, NULL);
        first = false;
    }
    if (ps->error != NULL) return "";
    ps->p++; /* '@' */
    {
        size_t al = strlen(args);
        return cat(ps, name, "<", args, (al > 0 && args[al - 1] == '>') ? " >" : ">", NULL, NULL);
    }
}

/* One scope fragment of a qualified name. */
static const char *parse_fragment(parser *ps, ctx *c)
{
    if (*ps->p >= '0' && *ps->p <= '9')
    {
        int i = *ps->p - '0';
        ps->p++;
        if (i >= c->nnames)
        {
            fail(ps, "invalid name back-reference");
            return "";
        }
        return c->names[i];
    }
    if (ps->p[0] == '?' && ps->p[1] == '$')
    {
        const char *t;
        ps->p += 2;
        t = parse_template_name(ps);
        memorize_name(c, t);
        return t;
    }
    if (ps->p[0] == '?' && ps->p[1] == 'A' && ps->p[2] == '0' && ps->p[3] == 'x')
    {
        /* Anonymous namespace: ?A0x<hex>@ */
        const char *at = strchr(ps->p, '@');
        if (at == NULL)
        {
            fail(ps, "invalid anonymous namespace");
            return "";
        }
        ps->p = at + 1;
        return "`anonymous namespace'";
    }
    if (ps->p[0] == '?')
    {
        /* Numbered local scope: ?<number>[?<symbol>] */
        int64_t v;
        char num[32];
        const char *n;
        ps->p++;
        if (!parse_number(ps, &v)) return "";
        snprintf(num, sizeof(num), "`%lld'", (long long)v);
        n = dup_n(ps, num, strlen(num));
        if (*ps->p == '?')
        {
            /* "?" separator, then the symbol with its own leading '?'. */
            const char *sym;
            ps->p++;
            sym = parse_symbol(ps, c);
            return cat(ps, "`", sym, "'::", n, NULL, NULL);
        }
        return n;
    }
    {
        const char *id = parse_identifier(ps);
        memorize_name(c, id);
        return id;
    }
}

/* Fragments (innermost first) up to the terminating '@'; the result is in
   source order joined with "::". */
static const char *parse_qualified_name(parser *ps, ctx *c)
{
    const char *frag[64];
    int n = 0, i;
    const char *out;
    while (ps->error == NULL && *ps->p != '@')
    {
        if (*ps->p == 0 || n == 64)
        {
            fail(ps, "invalid qualified name");
            return "";
        }
        frag[n++] = parse_fragment(ps, c);
    }
    if (ps->error != NULL || n == 0)
    {
        fail(ps, "empty qualified name");
        return "";
    }
    ps->p++;
    out = frag[n - 1];
    for (i = n - 2; i >= 0; i--) out = cat(ps, out, "::", frag[i], NULL, NULL, NULL);
    return out;
}

static const char *cv_text(char cv)
{
    switch (cv)
    {
    case 'A': return "";
    case 'B': return " const";
    case 'C': return " volatile";
    case 'D': return " const volatile";
    default: return NULL;
    }
}

static const char *basic_type(parser *ps)
{
    char t = *ps->p;
    if (t == '_')
    {
        char u = ps->p[1];
        const char *s = NULL;
        switch (u)
        {
        case 'D': s = "__int8"; break;
        case 'E': s = "unsigned __int8"; break;
        case 'F': s = "__int16"; break;
        case 'G': s = "unsigned __int16"; break;
        case 'H': s = "__int32"; break;
        case 'I': s = "unsigned __int32"; break;
        case 'J': s = "__int64"; break;
        case 'K': s = "unsigned __int64"; break;
        case 'L': s = "__int128"; break;
        case 'M': s = "unsigned __int128"; break;
        case 'N': s = "bool"; break;
        case 'W': s = "wchar_t"; break;
        default: break;
        }
        if (s != NULL) ps->p += 2;
        return s;
    }
    switch (t)
    {
    case 'C': ps->p++; return "signed char";
    case 'D': ps->p++; return "char";
    case 'E': ps->p++; return "unsigned char";
    case 'F': ps->p++; return "short";
    case 'G': ps->p++; return "unsigned short";
    case 'H': ps->p++; return "int";
    case 'I': ps->p++; return "unsigned int";
    case 'J': ps->p++; return "long";
    case 'K': ps->p++; return "unsigned long";
    case 'M': ps->p++; return "float";
    case 'N': ps->p++; return "double";
    case 'O': ps->p++; return "long double";
    case 'X': ps->p++; return "void";
    default: return NULL;
    }
}

static const char *parse_type(parser *ps, ctx *c)
{
    char t = *ps->p;
    const char *b;
    if (ps->error != NULL) return "";
    if (t >= '0' && t <= '9')
    {
        int i = t - '0';
        ps->p++;
        if (i >= c->ntypes)
        {
            fail(ps, "invalid type back-reference");
            return "";
        }
        return c->types[i];
    }
    b = basic_type(ps);
    if (b != NULL) return b;
    switch (t)
    {
    case 'T':
        ps->p++;
        return cat(ps, "union ", parse_qualified_name(ps, c), NULL, NULL, NULL, NULL);
    case 'U':
        ps->p++;
        return cat(ps, "struct ", parse_qualified_name(ps, c), NULL, NULL, NULL, NULL);
    case 'V':
        ps->p++;
        return cat(ps, "class ", parse_qualified_name(ps, c), NULL, NULL, NULL, NULL);
    case 'W':
        if (ps->p[1] < '0' || ps->p[1] > '7')
        {
            fail(ps, "invalid enum type");
            return "";
        }
        ps->p += 2;
        return cat(ps, "enum ", parse_qualified_name(ps, c), NULL, NULL, NULL, NULL);
    case 'P':
    case 'Q':
    case 'R':
    case 'S':
    case 'A':
    case 'B':
    {
        const char *pointee, *cv, *decl;
        ps->p++;
        if (*ps->p == 'E' || *ps->p == 'I')
        {
            fail(ps, "unsupported pointer modifier");
            return "";
        }
        if (*ps->p == '6' || *ps->p == '8')
        {
            fail(ps, "function pointers are not supported");
            return "";
        }
        cv = cv_text(*ps->p);
        if (cv == NULL)
        {
            fail(ps, "unsupported pointee qualifier");
            return "";
        }
        ps->p++;
        pointee = parse_type(ps, c);
        switch (t)
        {
        case 'P': decl = " *"; break;
        case 'Q': decl = " * const"; break;
        case 'R': decl = " * volatile"; break;
        case 'S': decl = " * const volatile"; break;
        case 'A': decl = " &"; break;
        default: decl = " & volatile"; break;
        }
        return cat(ps, pointee, cv, decl, NULL, NULL, NULL);
    }
    case '?':
    {
        /* A qualified type (template arguments, return values). */
        const char *cv = cv_text(ps->p[1]), *inner;
        if (cv == NULL)
        {
            fail(ps, "invalid qualified type");
            return "";
        }
        ps->p += 2;
        inner = parse_type(ps, c);
        return cat(ps, inner, cv, NULL, NULL, NULL, NULL);
    }
    default:
        fail(ps, "unsupported type encoding");
        return "";
    }
}

/* Special member and operator names after '?' as the first fragment of a
   function symbol. Returns NULL for an unsupported code. */
static const char *special_name(parser *ps, bool *ctor, bool *dtor)
{
    char k = *ps->p;
    *ctor = *dtor = false;
    ps->p++;
    switch (k)
    {
    case '0': *ctor = true; return "";
    case '1': *dtor = true; return "";
    case '2': return "operator new";
    case '3': return "operator delete";
    case '4': return "operator=";
    case '5': return "operator>>";
    case '6': return "operator<<";
    case '7': return "operator!";
    case '8': return "operator==";
    case '9': return "operator!=";
    case 'A': return "operator[]";
    case 'C': return "operator->";
    case 'D': return "operator*";
    case 'E': return "operator++";
    case 'F': return "operator--";
    case 'G': return "operator-";
    case 'H': return "operator+";
    case 'I': return "operator&";
    case 'K': return "operator/";
    case 'L': return "operator%";
    case 'M': return "operator<";
    case 'N': return "operator<=";
    case 'O': return "operator>";
    case 'P': return "operator>=";
    case 'R': return "operator()";
    case 'S': return "operator~";
    case 'T': return "operator^";
    case 'U': return "operator|";
    case 'V': return "operator&&";
    case 'W': return "operator||";
    case 'X': return "operator*=";
    case 'Y': return "operator+=";
    case 'Z': return "operator-=";
    default: return NULL;
    }
}

/* Function parameter list up to '@' or 'Z' (variable arguments); "X"
   alone is "void". */
static const char *parse_params(parser *ps, ctx *c)
{
    const char *out = "";
    bool first = true;
    if (*ps->p == 'X')
    {
        ps->p++;
        return "void";
    }
    while (ps->error == NULL)
    {
        if (*ps->p == '@')
        {
            ps->p++;
            break;
        }
        if (*ps->p == 'Z')
        {
            ps->p++;
            out = first ? "..." : cat(ps, out, ",...", NULL, NULL, NULL, NULL);
            break;
        }
        if (*ps->p == 0)
        {
            fail(ps, "unterminated parameter list");
            return "";
        }
        {
            const char *start = ps->p, *ty = parse_type(ps, c);
            if (ps->p - start > 1 && c->ntypes < MAXREF) c->types[c->ntypes++] = ty;
            out = first ? ty : cat(ps, out, ",", ty, NULL, NULL, NULL);
            first = false;
        }
    }
    return out;
}

/* A decorated function symbol ("?name@scope@@<signature>"), the scope of a
   local class. */
static const char *parse_symbol(parser *ps, ctx *c)
{
    const char *uname = NULL, *qual, *access = "", *storage = "", *callconv, *ret = "", *params, *thiscv = "";
    bool ctor = false, dtor = false, member = true;
    char fc;

    if (*ps->p != '?')
    {
        fail(ps, "invalid function symbol");
        return "";
    }
    ps->p++;
    if (*ps->p == '?')
    {
        ps->p++;
        uname = special_name(ps, &ctor, &dtor);
        if (uname == NULL)
        {
            fail(ps, "unsupported special function name");
            return "";
        }
    }
    else if (*ps->p == '$')
    {
        fail(ps, "template functions as scopes are not supported");
        return "";
    }
    else
    {
        uname = parse_identifier(ps);
        memorize_name(c, uname);
    }
    qual = (*ps->p == '@') ? (ps->p++, "") : parse_qualified_name(ps, c);
    if (ps->error != NULL) return "";
    if (ctor || dtor)
    {
        const char *cls = strrchr(qual, ':');
        cls = cls != NULL ? cls + 1 : qual;
        uname = ctor ? cls : cat(ps, "~", cls, NULL, NULL, NULL, NULL);
    }
    {
        const char *full = (*qual != 0) ? cat(ps, qual, "::", uname, NULL, NULL, NULL) : uname;
        fc = *ps->p++;
        if (fc >= 'A' && fc <= 'X')
        {
            int k = (fc - 'A') / 8, sub = ((fc - 'A') % 8) / 2;
            access = k == 0 ? "private: " : k == 1 ? "protected: " : "public: ";
            if (sub == 1)
            {
                storage = "static ";
                member = false;
            }
            else if (sub == 2)
            {
                storage = "virtual ";
            }
            else if (sub == 3)
            {
                fail(ps, "thunks are not supported");
                return "";
            }
        }
        else if (fc == 'Y' || fc == 'Z')
        {
            member = false;
        }
        else
        {
            fail(ps, "invalid function class");
            return "";
        }
        if (member)
        {
            const char *cv = cv_text(*ps->p);
            if (cv == NULL)
            {
                fail(ps, "invalid this qualifier");
                return "";
            }
            thiscv = cv;
            ps->p++;
        }
        switch (*ps->p++)
        {
        case 'A': case 'B': callconv = "__cdecl"; break;
        case 'C': case 'D': callconv = "__pascal"; break;
        case 'E': case 'F': callconv = "__thiscall"; break;
        case 'G': case 'H': callconv = "__stdcall"; break;
        case 'I': case 'J': callconv = "__fastcall"; break;
        default: fail(ps, "invalid calling convention"); return "";
        }
        if (*ps->p == '@')
        {
            ps->p++; /* no return type (constructor, destructor) */
        }
        else
        {
            ret = cat(ps, parse_type(ps, c), " ", NULL, NULL, NULL, NULL);
        }
        params = parse_params(ps, c);
        if (*ps->p != 'Z')
        {
            fail(ps, "unsupported exception specification");
            return "";
        }
        ps->p++;
        {
            const char *head = cat(ps, access, storage, ret, callconv, " ", full);
            return cat(ps, head, "(", params, ")", thiscv, NULL);
        }
    }
}

int rt_undname_type(const char *decorated, char *out, size_t n, const char **error)
{
    static parser ps_storage;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    parser *ps = &ps_storage;
    ctx c;
    const char *r;
    int status = 0;

    if (pthread_mutex_lock(&lock) != 0) rt_fatal("lock failed");
    memset(&c, 0, sizeof(c));
    ps->used = 0;
    ps->error = NULL;
    ps->p = decorated;
    if (ps->p[0] == '.') ps->p++;
    if (ps->p[0] != '?' || ps->p[1] != 'A')
    {
        fail(ps, "not a type descriptor name");
        r = "";
    }
    else
    {
        ps->p += 2;
        r = parse_type(ps, &c);
        if (ps->error == NULL && *ps->p != 0) fail(ps, "characters after the type");
    }
    if (ps->error != NULL)
    {
        if (error != NULL) *error = ps->error;
        status = -1;
    }
    else if ((size_t)snprintf(out, n, "%s", r) >= n)
    {
        if (error != NULL) *error = "output buffer too small";
        status = -1;
    }
    if (pthread_mutex_unlock(&lock) != 0) rt_fatal("unlock failed");
    return status;
}
