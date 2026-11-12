/*
 * The converter (see rt_convert.h).
 *
 * Layout of the Recompiler directory (made by tools/package_release.sh):
 *   bin/SRW, bin/llasm, bin/opt, bin/llvm-split, bin/llvm-link, bin/clang,
 *   bin/ld64.lld
 *   lib/                    the LLVM, lld, z3 and zstd libraries,
 *                           libprototype_rt.a (the runtime objects) and
 *                           libclang_rt.osx.a
 *   include/llasm-support   the .llinc files of the SR llasm support and
 *                           sse_inline.bc (the SSE helpers as bitcode,
 *                           made by tools/build_sse_inline.sh)
 *   include/runtime         the .llinc files of runtime/llasm
 *   modules/<module>        the analysis files of each module (SR.cfg,
 *                           *.sci, relocations.csv, extern.llinc)
 *   manifest.txt            "sdk <version>" (the SDK of the runtime
 *                           objects), "content <sha256>" (a value of all
 *                           other files of the directory; the loader uses
 *                           the manifest in the cache key) and one line
 *                           "pin <module> <file> <sha256>" for each SRW
 *                           output file of the tested translation
 *
 * The tools run with a minimal environment, in the work directory, with
 * the output in one log file per step. A failed step stops the conversion
 * with the end of its log in the error message.
 */
#include "rt_convert.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH_LEN 1024
#define MAX_JOBS 16u
#define MAX_PINS 64u
#define MAX_MANIFEST (64u << 10)
#define LOG_TAIL 1500
#define TRIPLE "arm64-apple-macosx14.0.0"
#define MIN_MACOS "14.0"
#define DATALAYOUT "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
#define ENGINE_PARTS 96u
/* The engine work files need about 6 GiB (measured in the development
   build: 5.8 GiB with all intermediate files). */
#define MIN_FREE_BYTES (UINT64_C(8) << 30)
/* Memory for one compiler process (UNCONFIRMED: not measured). */
#define BYTES_PER_JOB (UINT64_C(2) << 30)

typedef struct module_def {
    const char *name; /* work directory name and output base name */
    const char *pe;   /* file name in the game directory */
    unsigned parts;   /* llvm-split parts, 0 for no split */
    double weight;    /* part of the total progress */
} module_def;

static const module_def module_defs[] = {
    { "launcher", "prototypef.exe", 0, 0.02 },
    { "engine", "prototypeenginef.dll", ENGINE_PARTS, 0.93 },
};
#define NMODULES (sizeof module_defs / sizeof module_defs[0])

typedef struct pin {
    char module[32];
    char file[128];
    char sha256[65];
} pin;

typedef struct manifest {
    pin pins[MAX_PINS];
    unsigned npins;
    char sdk[16];
    bool have_content;
} manifest;

/* The options of the clang driver for "-target arm64-apple-macosx14.0.0
   -O2 -fPIC -c -x ir" (tools/compile_engine.sh), without the options that
   depend on the build computer (SDK, host CPU tuning, directories). The
   driver is not used, because it runs xcrun to find an SDK. */
static const char *const cc1_args[] = {
    "-cc1", "-triple", TRIPLE, "-O2", "-emit-obj", "-disable-free", "-disable-llvm-verifier", "-discard-value-names",
    "-mrelocation-model", "pic", "-pic-level", "2", "-mframe-pointer=non-leaf-no-reserve", "-ffp-contract=on",
    "-fno-rounding-math", "-funwind-tables=1", "-target-cpu", "apple-m1", "-target-feature", "+v8.4a",
    "-target-feature", "+aes", "-target-feature", "+altnzcv", "-target-feature", "+ccdp", "-target-feature", "+ccpp",
    "-target-feature", "+complxnum", "-target-feature", "+crc", "-target-feature", "+dotprod", "-target-feature",
    "+flagm", "-target-feature", "+fp-armv8", "-target-feature", "+fp16fml", "-target-feature", "+fptoint",
    "-target-feature", "+fullfp16", "-target-feature", "+jsconv", "-target-feature", "+lse", "-target-feature",
    "+neon", "-target-feature", "+pauth", "-target-feature", "+perfmon", "-target-feature", "+predres",
    "-target-feature", "+ras", "-target-feature", "+rcpc", "-target-feature", "+rdm", "-target-feature", "+sb",
    "-target-feature", "+sha2", "-target-feature", "+sha3", "-target-feature", "+specrestrict", "-target-feature",
    "+ssbs", "-target-abi", "darwinpcs", "-stack-protector", "1", "-fmax-type-align=16", "-vectorize-loops",
    "-vectorize-slp",
};
#define NCC1 (sizeof cc1_args / sizeof cc1_args[0])

static pthread_mutex_t child_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t children[MAX_JOBS];
static unsigned nchildren;
static atomic_bool cancelled;

static bool fail(rt_convert_job *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(rt_convert_job *job, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(job->error, sizeof job->error, fmt, ap);
    va_end(ap);
    return false;
}

static bool join(char *out, size_t n, const char *dir, const char *name)
{
    int r = snprintf(out, n, "%s/%s", dir, name);
    return r > 0 && (size_t)r < n;
}

static void progress(rt_convert_job *job, double f, const char *text)
{
    if (job->progress != NULL) job->progress(job->ctx, f < 0 ? 0 : f > 1 ? 1 : f, text);
}

bool rt_sha256_file(const char *path, char out[65], char *err, size_t errn)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX ctx;
    unsigned char *buf;
    size_t n;
    int i;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        snprintf(err, errn, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    buf = malloc(1u << 20);
    if (buf == NULL)
    {
        fclose(f);
        snprintf(err, errn, "out of memory");
        return false;
    }
    CC_SHA256_Init(&ctx);
    while ((n = fread(buf, 1, 1u << 20, f)) > 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    i = ferror(f);
    fclose(f);
    free(buf);
    if (i != 0)
    {
        snprintf(err, errn, "read of %s failed", path);
        return false;
    }
    CC_SHA256_Final(digest, &ctx);
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
    {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
    return true;
}

/* ---------------------------------------------------------------- child processes */

static void register_child(pid_t pid)
{
    pthread_mutex_lock(&child_lock);
    if (nchildren < MAX_JOBS) children[nchildren++] = pid;
    pthread_mutex_unlock(&child_lock);
    /* A cancel between posix_spawn and the registration did not see this
       child. */
    if (atomic_load(&cancelled)) kill(pid, SIGKILL);
}

static void unregister_child(pid_t pid)
{
    unsigned i;
    pthread_mutex_lock(&child_lock);
    for (i = 0; i < nchildren; i++)
    {
        if (children[i] != pid) continue;
        children[i] = children[--nchildren];
        break;
    }
    pthread_mutex_unlock(&child_lock);
}

void rt_convert_cancel(void)
{
    unsigned i;
    atomic_store(&cancelled, true);
    pthread_mutex_lock(&child_lock);
    for (i = 0; i < nchildren; i++) kill(children[i], SIGKILL);
    pthread_mutex_unlock(&child_lock);
}

static bool child_slot_free(void)
{
    bool r;
    pthread_mutex_lock(&child_lock);
    r = nchildren < MAX_JOBS;
    pthread_mutex_unlock(&child_lock);
    return r;
}

/* Start tool with argv (argv[0] is set to the tool path) in cwd. The
   standard output and the standard error go to log. */
static bool spawn_tool(rt_convert_job *job, const char *tool, const char **argv, const char *cwd, const char *log,
                       pid_t *pid)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    char env_home[PATH_LEN + 8], env_tmp[PATH_LEN + 8];
    const char *home = getenv("HOME");
    char *envp[5];
    int n = 0, r;

    if (!child_slot_free()) return fail(job, "too many tool processes");
    envp[n++] = (char *)"PATH=/usr/bin:/bin";
    envp[n++] = (char *)"LC_ALL=C";
    if (home != NULL && (size_t)snprintf(env_home, sizeof env_home, "HOME=%s", home) < sizeof env_home)
        envp[n++] = env_home;
    if ((size_t)snprintf(env_tmp, sizeof env_tmp, "TMPDIR=%s", job->work_dir) >= sizeof env_tmp)
        return fail(job, "the work directory path is too long");
    envp[n++] = env_tmp;
    envp[n] = NULL;
    argv[0] = tool;

    if (posix_spawn_file_actions_init(&fa) != 0) return fail(job, "posix_spawn_file_actions_init failed");
    if (posix_spawnattr_init(&attr) != 0)
    {
        posix_spawn_file_actions_destroy(&fa);
        return fail(job, "posix_spawnattr_init failed");
    }
    r = posix_spawn_file_actions_addchdir_np(&fa, cwd);
    if (r == 0) r = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (r == 0) r = posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (r == 0) r = posix_spawn_file_actions_adddup2(&fa, 1, 2);
    /* Only the descriptors 0, 1 and 2 go to the tool. */
    if (r == 0) r = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (r == 0) r = posix_spawn(pid, tool, &fa, &attr, (char *const *)argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (r != 0) return fail(job, "cannot start %s: %s", tool, strerror(r));
    register_child(*pid);
    return true;
}

static pid_t wait_any(int *status)
{
    pid_t pid;
    do pid = waitpid(-1, status, 0);
    while (pid < 0 && errno == EINTR);
    if (pid > 0) unregister_child(pid);
    return pid;
}

static void log_tail(const char *log, char *out, size_t n)
{
    FILE *f = fopen(log, "rb");
    long end;
    size_t got;
    out[0] = 0;
    if (f == NULL || n < 2) goto done;
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0) goto done;
    if (end > LOG_TAIL && fseek(f, end - LOG_TAIL, SEEK_SET) != 0) goto done;
    if (end <= LOG_TAIL && fseek(f, 0, SEEK_SET) != 0) goto done;
    got = fread(out, 1, n - 1, f);
    out[got] = 0;
done:
    if (f != NULL) fclose(f);
}

static bool status_failed(rt_convert_job *job, int status, const char *what, const char *log)
{
    char tail[LOG_TAIL + 1];
    if (atomic_load(&cancelled)) return fail(job, "the conversion was cancelled");
    log_tail(log, tail, sizeof tail);
    if (WIFSIGNALED(status))
        return fail(job, "%s stopped with signal %d. The end of %s:\n%s", what, WTERMSIG(status), log, tail);
    return fail(job, "%s failed with status %d. The end of %s:\n%s", what, WEXITSTATUS(status), log, tail);
}

/* Run one tool and wait for it. */
static bool run_tool(rt_convert_job *job, const char *what, const char *tool, const char **argv, const char *cwd,
                     const char *log)
{
    pid_t pid, got;
    int status = 0;
    if (atomic_load(&cancelled)) return fail(job, "the conversion was cancelled");
    if (!spawn_tool(job, tool, argv, cwd, log, &pid)) return false;
    got = wait_any(&status);
    if (got != pid) return fail(job, "waitpid for %s failed: %s", what, got < 0 ? strerror(errno) : "other process");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return status_failed(job, status, what, log);
    return true;
}

/* ---------------------------------------------------------------- files */

static bool load_manifest(rt_convert_job *job, manifest *mf)
{
    char path[PATH_LEN], line[512];
    FILE *f;
    unsigned lineno = 0;
    long size;
    memset(mf, 0, sizeof *mf);
    if (!join(path, sizeof path, job->recompiler, "manifest.txt")) return fail(job, "the converter path is too long");
    f = fopen(path, "r");
    if (f == NULL) return fail(job, "cannot open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || size > (long)MAX_MANIFEST || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return fail(job, "%s is damaged", path);
    }
    while (fgets(line, sizeof line, f) != NULL)
    {
        char a[16], mod[32], file[128], sha[65], extra[2];
        int k;
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(f)) break;
        if (line[0] == '#' || line[0] == '\n') continue;
        k = sscanf(line, "%15s %31s %127s %64s %1s", a, mod, file, sha, extra);
        if (k == 2 && strcmp(a, "sdk") == 0 && strspn(mod, "0123456789.") == strlen(mod) && strlen(mod) < sizeof mf->sdk)
        {
            memcpy(mf->sdk, mod, strlen(mod) + 1);
            continue;
        }
        /* The content value has 64 digits: it does not fit in mod, so the
           line is read again with sha as the second field. */
        if (k >= 1 && strcmp(a, "content") == 0)
        {
            if (sscanf(line, "%15s %64s %1s", a, sha, extra) == 2 && strlen(sha) == 64 &&
                strspn(sha, "0123456789abcdef") == 64)
            {
                mf->have_content = true;
                continue;
            }
            fclose(f);
            return fail(job, "%s line %u is damaged", path, lineno);
        }
        if (k != 4 || strcmp(a, "pin") != 0 || strlen(sha) != 64 || strspn(sha, "0123456789abcdef") != 64 ||
            strchr(file, '/') != NULL || file[0] == '.' || mf->npins >= MAX_PINS)
        {
            fclose(f);
            return fail(job, "%s line %u is damaged", path, lineno);
        }
        memcpy(mf->pins[mf->npins].module, mod, strlen(mod) + 1);
        memcpy(mf->pins[mf->npins].file, file, strlen(file) + 1);
        memcpy(mf->pins[mf->npins].sha256, sha, 65);
        mf->npins++;
    }
    if (ferror(f) || !feof(f))
    {
        fclose(f);
        return fail(job, "%s is damaged (line %u)", path, lineno + 1);
    }
    fclose(f);
    if (mf->sdk[0] == 0 || !mf->have_content) return fail(job, "%s has no sdk line or no content line", path);
    return true;
}

static bool copy_file(rt_convert_job *job, const char *src, const char *dst)
{
    if (copyfile(src, dst, NULL, COPYFILE_DATA | COPYFILE_EXCL) != 0)
        return fail(job, "cannot copy %s to %s: %s", src, dst, strerror(errno));
    return true;
}

/* Copy each regular file of src_dir into dst_dir. */
static bool copy_dir_files(rt_convert_job *job, const char *src_dir, const char *dst_dir, unsigned *count)
{
    DIR *d = opendir(src_dir);
    struct dirent *e;
    *count = 0;
    if (d == NULL) return fail(job, "cannot open %s: %s", src_dir, strerror(errno));
    while ((e = readdir(d)) != NULL)
    {
        char src[PATH_LEN], dst[PATH_LEN];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        if (!join(src, sizeof src, src_dir, e->d_name) || !join(dst, sizeof dst, dst_dir, e->d_name))
        {
            closedir(d);
            return fail(job, "a path is too long");
        }
        if (lstat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!copy_file(job, src, dst))
        {
            closedir(d);
            return false;
        }
        (*count)++;
    }
    closedir(d);
    return true;
}

/* Remove each regular file of dir except keep. */
static bool clean_dir(rt_convert_job *job, const char *dir, const char *keep)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) return fail(job, "cannot open %s: %s", dir, strerror(errno));
    while ((e = readdir(d)) != NULL)
    {
        char p[PATH_LEN];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strcmp(e->d_name, keep) == 0) continue;
        if (!join(p, sizeof p, dir, e->d_name))
        {
            closedir(d);
            return fail(job, "a path is too long");
        }
        if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (unlink(p) != 0)
        {
            closedir(d);
            return fail(job, "cannot remove %s: %s", p, strerror(errno));
        }
    }
    closedir(d);
    return true;
}

static bool remove_file(rt_convert_job *job, const char *path)
{
    if (unlink(path) != 0) return fail(job, "cannot remove %s: %s", path, strerror(errno));
    return true;
}

static unsigned job_count(void)
{
    int ncpu = 0;
    uint64_t mem = 0;
    size_t len = sizeof ncpu;
    unsigned jobs;
    if (sysctlbyname("hw.activecpu", &ncpu, &len, NULL, 0) != 0 || ncpu < 1) ncpu = 1;
    len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) mem = 0;
    jobs = (unsigned)ncpu;
    if (mem / BYTES_PER_JOB < jobs) jobs = (unsigned)(mem / BYTES_PER_JOB);
    if (jobs > MAX_JOBS) jobs = MAX_JOBS;
    return jobs < 1 ? 1 : jobs;
}

/* ---------------------------------------------------------------- steps */

typedef struct objects {
    char (*path)[PATH_LEN];
    unsigned n, cap;
} objects;

/* Run one tool for each bitcode part (file names in dir) in parallel.

   With sse_bc == NULL, clang compiles each part. Each input is removed
   after its object is made, and the object is added to objs.

   With sse_bc != NULL, llvm-link copies the SSE helpers that a part calls
   from sse_bc into the part as internal functions (as in
   tools/compile_engine.sh), so that clang can inline them. The result
   replaces the input; objs is not changed. */
static bool run_parts(rt_convert_job *job, const char *dir, char (*inputs)[PATH_LEN], unsigned n, const char *sse_bc,
                      objects *objs, double p0, double p1)
{
    char tool[PATH_LEN];
    pid_t pids[MAX_JOBS];
    unsigned slot_input[MAX_JOBS];
    unsigned jobs = job_count(), next = 0, done = 0, running = 0, i;
    bool ok = true, link = sse_bc != NULL;
    const char *what = link ? "llvm-link" : "clang";
    const char *out_ext = link ? "sse" : "o";
    const char *log_ext = link ? "link.log" : "log";

    if (!join(tool, sizeof tool, job->recompiler, link ? "bin/llvm-link" : "bin/clang"))
        return fail(job, "the converter path is too long");
    for (i = 0; i < MAX_JOBS; i++) pids[i] = 0;
    while (done < n)
    {
        while (ok && next < n && running < jobs)
        {
            const char *argv[NCC1 + 8];
            char out[PATH_LEN], log[PATH_LEN];
            unsigned k, s;
            if (atomic_load(&cancelled))
            {
                ok = fail(job, "the conversion was cancelled");
                break;
            }
            if ((size_t)snprintf(out, sizeof out, "%s/%s.%s", dir, inputs[next], out_ext) >= sizeof out ||
                (size_t)snprintf(log, sizeof log, "%s/%s.%s", dir, inputs[next], log_ext) >= sizeof log)
            {
                ok = fail(job, "a path is too long");
                break;
            }
            argv[0] = NULL;
            if (link)
            {
                argv[1] = "--only-needed";
                argv[2] = "--internalize";
                argv[3] = inputs[next];
                argv[4] = sse_bc;
                argv[5] = "-o";
                argv[6] = out;
                argv[7] = NULL;
            }
            else
            {
                for (k = 0; k < NCC1; k++) argv[1 + k] = cc1_args[k];
                argv[1 + NCC1] = "-o";
                argv[2 + NCC1] = out;
                argv[3 + NCC1] = "-x";
                argv[4 + NCC1] = "ir";
                argv[5 + NCC1] = inputs[next];
                argv[6 + NCC1] = NULL;
            }
            for (s = 0; s < MAX_JOBS && pids[s] != 0; s++) {}
            if (s == MAX_JOBS || !spawn_tool(job, tool, argv, dir, log, &pids[s]))
            {
                ok = s == MAX_JOBS ? fail(job, "no free compiler slot") : false;
                break;
            }
            slot_input[s] = next++;
            running++;
        }
        if (running == 0) break;
        {
            int status = 0;
            unsigned s;
            pid_t pid = wait_any(&status);
            char in[PATH_LEN], out[PATH_LEN], log[PATH_LEN];
            struct stat st;
            if (pid < 0)
            {
                ok = fail(job, "waitpid failed: %s", strerror(errno));
                rt_convert_cancel();
                break;
            }
            for (s = 0; s < MAX_JOBS && pids[s] != pid; s++) {}
            if (s == MAX_JOBS)
            {
                if (ok) ok = fail(job, "an unknown child process %d ended", (int)pid);
                continue;
            }
            pids[s] = 0;
            running--;
            done++;
            if ((size_t)snprintf(log, sizeof log, "%s/%s.%s", dir, inputs[slot_input[s]], log_ext) >= sizeof log ||
                (size_t)snprintf(out, sizeof out, "%s/%s.%s", dir, inputs[slot_input[s]], out_ext) >= sizeof out ||
                !join(in, sizeof in, dir, inputs[slot_input[s]]))
            {
                if (ok) ok = fail(job, "a path is too long");
                rt_convert_cancel();
                continue;
            }
            if (!ok) continue;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                ok = status_failed(job, status, what, log);
                rt_convert_cancel();
                continue;
            }
            /* The development build linked and compiled each part with no
               output. */
            if (stat(log, &st) == 0 && st.st_size > 0)
            {
                char tail[LOG_TAIL + 1];
                log_tail(log, tail, sizeof tail);
                ok = fail(job, "%s wrote messages for %s (the tested build had none). The end of %s:\n%s", what,
                          inputs[slot_input[s]], log, tail);
                rt_convert_cancel();
                continue;
            }
            if (link)
            {
                if (rename(out, in) != 0)
                {
                    ok = fail(job, "cannot replace %s with %s: %s", in, out, strerror(errno));
                    rt_convert_cancel();
                    continue;
                }
                if (!remove_file(job, log))
                {
                    ok = false;
                    rt_convert_cancel();
                    continue;
                }
                progress(job, p0 + (p1 - p0) * done / n, "Preparing the game code");
                continue;
            }
            if (!remove_file(job, in) || !remove_file(job, log))
            {
                ok = false;
                rt_convert_cancel();
                continue;
            }
            if (objs->n >= objs->cap)
            {
                ok = fail(job, "too many objects");
                rt_convert_cancel();
                continue;
            }
            if ((size_t)snprintf(objs->path[objs->n], PATH_LEN, "%s/%s.o", dir, inputs[slot_input[s]]) >= PATH_LEN)
            {
                ok = fail(job, "a path is too long");
                rt_convert_cancel();
                continue;
            }
            objs->n++;
            progress(job, p0 + (p1 - p0) * done / n, "Compiling the game code");
        }
    }
    return ok && done == n;
}

static bool convert_module(rt_convert_job *job, const module_def *m, const manifest *mf, objects *objs, double p0,
                           double p1)
{
    char dir[PATH_LEN], src[PATH_LEN], dst[PATH_LEN], tool[PATH_LEN], log[PATH_LEN], llasm_name[64], ll_name[64],
        bc_name[64], inc1[PATH_LEN], inc2[PATH_LEN], err[256];
    const char *argv[24];
    char (*inputs)[PATH_LEN];
    unsigned i, nfiles, npins = 0, ninputs;
    double span = p1 - p0;
    bool ok;

    if (!join(dir, sizeof dir, job->work_dir, m->name)) return fail(job, "the work directory path is too long");
    if (mkdir(dir, 0755) != 0) return fail(job, "cannot make %s: %s", dir, strerror(errno));
    snprintf(llasm_name, sizeof llasm_name, "%s.llasm", m->name);
    snprintf(ll_name, sizeof ll_name, "%s.ll", m->name);
    snprintf(bc_name, sizeof bc_name, "%s.bc", m->name);

    /* The PE file and the analysis files. */
    progress(job, p0, "Reading the game files");
    if (!join(src, sizeof src, job->game_dir, m->pe) || !join(dst, sizeof dst, dir, m->pe))
        return fail(job, "a path is too long");
    if (!copy_file(job, src, dst)) return false;
    if ((size_t)snprintf(src, sizeof src, "%s/modules/%s", job->recompiler, m->name) >= sizeof src)
        return fail(job, "the converter path is too long");
    if (!copy_dir_files(job, src, dir, &nfiles)) return false;
    if (nfiles == 0) return fail(job, "%s holds no analysis files", src);

    /* 1. SRW */
    progress(job, p0 + span * 0.01, "Translating the game code (SRW)");
    if (!join(tool, sizeof tool, job->recompiler, "bin/SRW") || !join(log, sizeof log, dir, "srw.log"))
        return fail(job, "a path is too long");
    argv[1] = m->pe;
    argv[2] = llasm_name;
    argv[3] = NULL;
    if (!run_tool(job, "SRW", tool, argv, dir, log)) return false;
    for (i = 0; i < mf->npins; i++)
    {
        char sum[65];
        if (strcmp(mf->pins[i].module, m->name) != 0) continue;
        npins++;
        if (!join(src, sizeof src, dir, mf->pins[i].file)) return fail(job, "a path is too long");
        if (!rt_sha256_file(src, sum, err, sizeof err)) return fail(job, "%s", err);
        if (strcmp(sum, mf->pins[i].sha256) != 0)
            return fail(job, "the translation of %s is not the tested translation: %s has SHA-256 %s, expected %s",
                        m->pe, mf->pins[i].file, sum, mf->pins[i].sha256);
    }
    if (npins == 0) return fail(job, "manifest.txt has no SRW output values for the module %s", m->name);

    /* 2. llasm */
    progress(job, p0 + span * 0.12, "Translating the game code (llasm)");
    if (!join(tool, sizeof tool, job->recompiler, "bin/llasm") || !join(log, sizeof log, dir, "llasm.log") ||
        !join(inc1, sizeof inc1, job->recompiler, "include/llasm-support") ||
        !join(inc2, sizeof inc2, job->recompiler, "include/runtime"))
        return fail(job, "a path is too long");
    i = 1;
    argv[i++] = "-m64";
    argv[i++] = "-inline-idiv";
    argv[i++] = "-inline-float";
    argv[i++] = "-pic";
    argv[i++] = "-ptrofs";
    argv[i++] = "-O";
    argv[i++] = "-I";
    argv[i++] = inc1;
    argv[i++] = "-I";
    argv[i++] = inc2;
    argv[i++] = "-I";
    argv[i++] = ".";
    argv[i++] = llasm_name;
    argv[i++] = "-o";
    argv[i++] = ll_name;
    argv[i] = NULL;
    if (!run_tool(job, "llasm", tool, argv, dir, log)) return false;
    if (!clean_dir(job, dir, ll_name)) return false;

    /* 3. opt: target, data layout, IR verifier, bitcode */
    progress(job, p0 + span * 0.25, "Checking the translated code");
    if (!join(tool, sizeof tool, job->recompiler, "bin/opt") || !join(log, sizeof log, dir, "opt.log"))
        return fail(job, "a path is too long");
    argv[1] = "--mtriple=" TRIPLE;
    argv[2] = "--data-layout=" DATALAYOUT;
    argv[3] = ll_name;
    argv[4] = "-o";
    argv[5] = bc_name;
    argv[6] = NULL;
    if (!run_tool(job, "opt", tool, argv, dir, log)) return false;
    if (!join(src, sizeof src, dir, ll_name) || !remove_file(job, src)) return false;

    ninputs = m->parts > 0 ? m->parts : 1;
    inputs = calloc(ninputs, sizeof *inputs);
    if (inputs == NULL) return fail(job, "out of memory");
    if (m->parts > 0)
    {
        /* llvm-split into parts/<module>0 ... parts/<module>N-1 */
        char parts[PATH_LEN], prefix[PATH_LEN], jobs_arg[16];
        progress(job, p0 + span * 0.30, "Dividing the translated code");
        if (!join(parts, sizeof parts, dir, "parts") || mkdir(parts, 0755) != 0)
        {
            free(inputs);
            return fail(job, "cannot make the parts directory in %s", dir);
        }
        if (!join(tool, sizeof tool, job->recompiler, "bin/llvm-split") || !join(log, sizeof log, dir, "split.log"))
        {
            free(inputs);
            return fail(job, "a path is too long");
        }
        snprintf(jobs_arg, sizeof jobs_arg, "%u", m->parts);
        snprintf(prefix, sizeof prefix, "parts/%s", m->name);
        argv[1] = "-j";
        argv[2] = jobs_arg;
        argv[3] = bc_name;
        argv[4] = "-o";
        argv[5] = prefix;
        argv[6] = NULL;
        if (!run_tool(job, "llvm-split", tool, argv, dir, log) || !join(src, sizeof src, dir, bc_name) ||
            !remove_file(job, src))
        {
            free(inputs);
            return false;
        }
        for (i = 0; i < m->parts; i++)
        {
            struct stat st;
            snprintf(inputs[i], PATH_LEN, "%s%u", m->name, i);
            if (!join(src, sizeof src, parts, inputs[i]) || stat(src, &st) != 0 || !S_ISREG(st.st_mode))
            {
                free(inputs);
                return fail(job, "llvm-split did not write %s", inputs[i]);
            }
        }
        /* The SSE helpers are linked into the engine parts only, as in
           tools/compile_engine.sh. */
        ok = join(src, sizeof src, job->recompiler, "include/llasm-support/sse_inline.bc");
        if (!ok) ok = fail(job, "the converter path is too long");
        if (ok) ok = run_parts(job, parts, inputs, m->parts, src, objs, p0 + span * 0.35, p0 + span * 0.38);
        if (ok) ok = run_parts(job, parts, inputs, m->parts, NULL, objs, p0 + span * 0.38, p1);
    }
    else
    {
        snprintf(inputs[0], PATH_LEN, "%s", bc_name);
        ok = run_parts(job, dir, inputs, 1, NULL, objs, p0 + span * 0.35, p1);
    }
    free(inputs);
    return ok;
}

static bool link_bundle(rt_convert_job *job, const manifest *mf, const objects *objs)
{
    char tool[PATH_LEN], log[PATH_LEN], tmp[PATH_LEN], rtlib[PATH_LEN], crt[PATH_LEN];
    const char **argv;
    unsigned i, k = 1;
    bool ok;

    if (!join(tool, sizeof tool, job->recompiler, "bin/ld64.lld") || !join(log, sizeof log, job->work_dir, "link.log") ||
        !join(rtlib, sizeof rtlib, job->recompiler, "lib/libprototype_rt.a") ||
        !join(crt, sizeof crt, job->recompiler, "lib/libclang_rt.osx.a") ||
        (size_t)snprintf(tmp, sizeof tmp, "%s.tmp", job->out_bundle) >= sizeof tmp)
        return fail(job, "a path is too long");
    argv = calloc(objs->n + 20, sizeof *argv);
    if (argv == NULL) return fail(job, "out of memory");
    argv[k++] = "-arch";
    argv[k++] = "arm64";
    argv[k++] = "-platform_version";
    argv[k++] = "macos";
    argv[k++] = MIN_MACOS;
    argv[k++] = mf->sdk;
    argv[k++] = "-bundle";
    /* The libraries and frameworks are loaded by the loader executable;
       dyld finds their symbols when it loads the bundle. */
    argv[k++] = "-undefined";
    argv[k++] = "dynamic_lookup";
    argv[k++] = "-o";
    argv[k++] = tmp;
    for (i = 0; i < objs->n; i++) argv[k++] = objs->path[i];
    argv[k++] = "-force_load";
    argv[k++] = rtlib;
    argv[k++] = crt;
    argv[k] = NULL;
    unlink(tmp);
    ok = run_tool(job, "ld64.lld", tool, argv, job->work_dir, log);
    free(argv);
    if (!ok) return false;
    if (rename(tmp, job->out_bundle) != 0) return fail(job, "cannot rename %s: %s", tmp, strerror(errno));
    return true;
}

bool rt_convert_run(rt_convert_job *job)
{
    manifest *mf;
    objects objs;
    struct statfs fs;
    double p = 0.0;
    size_t i;
    bool ok = true;

    job->error[0] = 0;
    if (statfs(job->work_dir, &fs) != 0) return fail(job, "cannot read the free space of %s", job->work_dir);
    if ((uint64_t)fs.f_bavail * fs.f_bsize < MIN_FREE_BYTES)
        return fail(job, "The conversion needs 8 GB of free disk space for temporary files. Free space: %.1f GB.",
                    (double)((uint64_t)fs.f_bavail * fs.f_bsize) / 1e9);
    mf = malloc(sizeof *mf);
    if (mf == NULL) return fail(job, "out of memory");
    if (!load_manifest(job, mf))
    {
        free(mf);
        return false;
    }
    objs.cap = ENGINE_PARTS + NMODULES;
    objs.n = 0;
    objs.path = calloc(objs.cap, sizeof *objs.path);
    if (objs.path == NULL)
    {
        free(mf);
        return fail(job, "out of memory");
    }
    for (i = 0; i < NMODULES && ok; i++)
    {
        ok = convert_module(job, &module_defs[i], mf, &objs, p, p + module_defs[i].weight);
        p += module_defs[i].weight;
    }
    if (ok)
    {
        progress(job, p, "Linking the game");
        ok = link_bundle(job, mf, &objs);
    }
    if (ok) progress(job, 1.0, "Done");
    free(objs.path);
    free(mf);
    return ok;
}
