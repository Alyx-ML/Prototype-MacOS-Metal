/* The converter: makes the game bundle from the game files of the player.
 *
 * The converter runs the same translation as the development build
 * (tools/translate_module.sh, tools/compile_engine.sh) with the tools in
 * Contents/Resources/Recompiler:
 *   1. SRW translates each PE module to llasm with the analysis files of
 *      the module (modules/<module>). The SHA-256 values of the SRW output
 *      must agree with the values in manifest.txt (the tested translation).
 *   2. llasm translates the llasm files to LLVM IR.
 *   3. opt checks the IR and writes bitcode; llvm-split divides the engine.
 *   4. clang -cc1 compiles each part to an ARM64 object.
 *   5. ld64.lld links the objects and the runtime archive into the bundle.
 * The converter runs on a thread that is not the main thread.
 */
#ifndef RT_CONVERT_H
#define RT_CONVERT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/* fraction: 0 to 1. text: a short description of the current step. */
typedef void (*rt_convert_progress_fn)(void *ctx, double fraction, const char *text);

typedef struct rt_convert_job {
    const char *recompiler;  /* the Recompiler directory (a copy without quarantine) */
    const char *game_dir;    /* the checked game directory */
    const char *work_dir;    /* an empty directory for intermediate files */
    const char *out_bundle;  /* the bundle to write */
    rt_convert_progress_fn progress;
    void *ctx;
    char error[4096];        /* the reason when rt_convert_run returns false */
} rt_convert_job;

/* Run all steps. Returns false with job->error set if a step fails or the
   conversion was cancelled. */
bool rt_convert_run(rt_convert_job *job);

/* Stop the conversion: stops the running tools. rt_convert_run then
   returns false. Can be called from any thread. */
void rt_convert_cancel(void);

/* The SHA-256 of a file as 64 lower-case hex digits. Returns false with a
   message in err if the file cannot be read. */
bool rt_sha256_file(const char *path, char out[65], char *err, size_t errn);

#endif
