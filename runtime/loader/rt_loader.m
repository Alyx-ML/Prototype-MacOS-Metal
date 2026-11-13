/*
 * The loader: the executable of Prototype.app.
 *
 * 1. Find the game directory.
 *    - With "--game-dir <path>", use that directory. The launcher window
 *      does not open.
 *    - Else the launcher window opens (rt_launcher.m). The loader does not
 *      look for the game files. The window shows the folder that the
 *      player selected before, or no folder on the first start. The player
 *      selects the folder with "Choose Folder…" and can change the Mac
 *      settings. Play starts the game; a closed window quits.
 * 2. Check the SHA-256 values of prototypef.exe and prototypeenginef.dll.
 *    Only the Steam version is supported, because the converter output is
 *    tested for these files.
 * 3. Get the game bundle (the runtime and the translated game code):
 *    - development build (RT_LOADER_DEV=1): Contents/Resources/Prototype.bundle,
 *      made by tools/build_app.sh;
 *    - release build: ~/Library/Application Support/Prototype Mac Port/
 *      GameCode/<id>/Prototype.bundle. If it does not exist, the converter
 *      makes it (one time) from the game files of the player, with a
 *      progress window. <id> comes from the SHA-256 values of this
 *      executable and of Recompiler/manifest.txt, so a new release makes a
 *      new bundle. The loader removes the bundles of other releases.
 * 4. Open the bundle with dlopen and call rt_game_main on the main thread.
 *
 * The libraries and frameworks that the runtime uses are linked to this
 * executable; the bundle finds their symbols when dyld loads it
 * (-undefined dynamic_lookup).
 *
 * An error shows an alert and ends the process.
 */
#import <Cocoa/Cocoa.h>

#include "rt_convert.h"
#include "rt_game.h"
#include "rt_launcher.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dlfcn.h>
#include <errno.h>
#include <fts.h>
#include <pthread.h>
#include <removefile.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#ifndef RT_LOADER_DEV
#define RT_LOADER_DEV 0
#endif

#define PORT_NAME "Prototype Mac Port"
#define DEFAULTS_GAME_DIR @"GameDirectory"
#define PATH_LEN 1024

typedef struct pinned_file {
    const char *name;
    const char *sha256;
} pinned_file;

/* The Steam version of Prototype (the same values are in
   tools/translate_launcher.sh and tools/translate_engine.sh). */
static const pinned_file pinned[] = {
    { "prototypef.exe", "ac1542512ab5eddffcbf949919f3dcc4b26e38f07b57cb6a094def2a621b0b82" },
    { "prototypeenginef.dll", "d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071" },
};
#define NPINNED (sizeof pinned / sizeof pinned[0])

/* ---------------------------------------------------------------- messages */

static void ensure_app(void)
{
    static bool done;
    if (done) return;
    done = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}

static void show_alert(NSString *title, NSString *text, NSAlertStyle style)
{
    NSAlert *a;
    fprintf(stderr, "Prototype: %s %s\n", title.UTF8String, text.UTF8String);
    ensure_app();
    a = [NSAlert new];
    a.alertStyle = style;
    a.messageText = title;
    a.informativeText = text;
    [NSApp activate];
    [a runModal];
}

static _Noreturn void fail(NSString *title, NSString *text)
{
    show_alert(title, text, NSAlertStyleCritical);
    exit(1);
}

static NSString *str(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    return r != nil ? r : @"(a name that is not UTF-8)";
}

/* ---------------------------------------------------------------- game directory */

static bool join(char *out, size_t n, const char *dir, const char *name)
{
    int r = snprintf(out, n, "%s/%s", dir, name);
    return r > 0 && (size_t)r < n;
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

#if !RT_LOADER_DEV
static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
#endif

/* Does dir hold all game files? If not, *why tells the player what is
   wrong (an installer folder, a parent folder, or no game files). */
static bool check_dir(const char *dir, NSString **why)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray<NSString *> *names;
    NSString *d = str(dir);
    size_t i;
    bool all = true;

    for (i = 0; i < NPINNED; i++)
    {
        char p[PATH_LEN];
        if (!join(p, sizeof p, dir, pinned[i].name) || !is_file(p)) all = false;
    }
    if (all) return true;
    if (why == NULL) return false;
    names = [fm contentsOfDirectoryAtPath:d error:nil];
    for (NSString *n in names)
    {
        NSString *sub = [d stringByAppendingPathComponent:n];
        BOOL isdir = NO;
        if ([fm fileExistsAtPath:sub isDirectory:&isdir] && isdir &&
            [fm fileExistsAtPath:[sub stringByAppendingPathComponent:@"prototypef.exe"]])
        {
            *why = [NSString stringWithFormat:@"The game files are in the folder \"%@\". Select that folder.", sub];
            return false;
        }
    }
    for (NSString *n in names)
    {
        NSString *l = n.lowercaseString;
        if ([l isEqualToString:@"setup.exe"] || [l hasSuffix:@".cab"] || [l hasSuffix:@".msi"])
        {
            *why = [NSString stringWithFormat:@"The folder \"%@\" holds an installer, not the installed game. Install "
                                              @"Prototype with Steam, then copy the installed game folder (the folder "
                                              @"that holds prototypef.exe).",
                                              d];
            return false;
        }
    }
    *why = [NSString stringWithFormat:@"The folder \"%@\" does not hold prototypef.exe and prototypeenginef.dll. Select "
                                      @"the installed game folder of the Steam version of Prototype.",
                                      d];
    return false;
}

static bool copy_path(char *out, size_t n, const char *in)
{
    return (size_t)snprintf(out, n, "%s", in) < n;
}

/* The problem of the SHA-256 values of the game files in dir, or nil. */
static NSString *hash_problem(const char *dir, NSString **title)
{
    size_t i;
    for (i = 0; i < NPINNED; i++)
    {
        char path[PATH_LEN], sum[65], err[512];
        if (!join(path, sizeof path, dir, pinned[i].name))
        {
            *title = @"The game folder path is too long.";
            return str(dir);
        }
        if (!rt_sha256_file(path, sum, err, sizeof err))
        {
            *title = @"A game file cannot be read.";
            return str(err);
        }
        if (strcmp(sum, pinned[i].sha256) != 0)
        {
            *title = [NSString stringWithFormat:@"%s is not the supported version.", pinned[i].name];
            return [NSString stringWithFormat:@"This port supports only the Steam version of Prototype. The file %s has "
                                              @"the SHA-256 value %s; the supported file has %s.",
                                              path, sum, pinned[i].sha256];
        }
    }
    return nil;
}

/* The check of the launcher window: the problem of dir for the player, or
   nil if the game can start from dir. */
static NSString *dir_problem(const char *dir)
{
    NSString *why = nil, *title = nil, *problem;
    if (!check_dir(dir, &why)) return why;
    problem = hash_problem(dir, &title);
    if (problem == nil) return nil;
    return [NSString stringWithFormat:@"%@ %@", title, problem];
}

static void find_game_dir(int argc, char **argv, char *out, size_t n)
{
    NSString *remembered = [NSUserDefaults.standardUserDefaults stringForKey:DEFAULTS_GAME_DIR];
    const char *home = getenv("HOME");
    char found[PATH_LEN], settings[PATH_LEN];
    const char *initial = NULL;
    int i;

    for (i = 1; i < argc; i++)
    {
        NSString *why = nil;
        if (strcmp(argv[i], "--game-dir") != 0) continue;
        if (i + 1 >= argc) fail(@"The option --game-dir needs a folder.", @"Give the game folder after --game-dir.");
        if (realpath(argv[i + 1], out) == NULL)
            fail(@"The game folder does not exist.", str(argv[i + 1]));
        if (!check_dir(out, &why)) fail(@"The game folder cannot be used.", why);
        return;
    }
    if (home == NULL || !join(settings, sizeof settings, home, "Library/Application Support/" PORT_NAME "/enhancements.txt"))
        fail(@"The home folder is not known.", @"The HOME variable is not set or is too long.");
    /* The folder that the player selected before. The window checks it and
       shows its problem if its files were moved or removed. */
    if (remembered != nil && remembered.fileSystemRepresentation != NULL &&
        copy_path(found, sizeof found, remembered.fileSystemRepresentation))
        initial = found;
    ensure_app();
    if (!rt_launcher_run(settings, initial, dir_problem, out, n)) exit(0);
    [NSUserDefaults.standardUserDefaults setObject:str(out) forKey:DEFAULTS_GAME_DIR];
}

static void check_game_files(const char *dir)
{
    NSString *title = nil, *problem = hash_problem(dir, &title);
    if (problem != nil) fail(title, problem);
}

/* ---------------------------------------------------------------- conversion (release build) */

#if !RT_LOADER_DEV

@interface RTProgress : NSObject <NSWindowDelegate>
@property(nonatomic) NSWindow *window;
@property(nonatomic) NSProgressIndicator *bar;
@property(nonatomic) NSTextField *detail;
@property(atomic) BOOL closed;
@end

@implementation RTProgress
- (instancetype)init
{
    NSTextField *title;
    NSView *v;
    if ((self = [super init]) == nil) return nil;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 150)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Prototype";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    v = _window.contentView;
    title = [NSTextField labelWithString:@"Preparing Prototype for this Mac"];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.frame = NSMakeRect(24, 100, 432, 24);
    [v addSubview:title];
    _detail = [NSTextField labelWithString:@"This is done one time. It can take several minutes."];
    _detail.frame = NSMakeRect(24, 72, 432, 20);
    [v addSubview:_detail];
    _bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(24, 36, 432, 20)];
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = 1;
    [v addSubview:_bar];
    [_window center];
    return self;
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    self.closed = YES;
    rt_convert_cancel();
    return YES;
}

/* The quit Apple event (Quit in the Dock, log out, shut down) does what
   the close button does. AppKit installs its own handler only in
   finishLaunching, which the runtime calls when the game starts. */
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    self.closed = YES;
    rt_convert_cancel();
}
@end

typedef struct convert_state {
    rt_convert_job job;
    void *ui; /* RTProgress, retained by the caller */
    atomic_bool finished;
    bool ok;
} convert_state;

static void post_wake_event(void)
{
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:0
                                       data1:0
                                       data2:0];
    [NSApp postEvent:e atStart:NO];
}

static void on_progress(void *ctx, double fraction, const char *text)
{
    convert_state *s = ctx;
    NSString *t = str(text);
    RTProgress *ui = (__bridge RTProgress *)s->ui;
    dispatch_async(dispatch_get_main_queue(), ^{
        ui.bar.doubleValue = fraction;
        ui.detail.stringValue = t;
    });
}

static void *convert_thread(void *arg)
{
    convert_state *s = arg;
    @autoreleasepool
    {
        s->ok = rt_convert_run(&s->job);
        atomic_store(&s->finished, true);
        post_wake_event();
    }
    return NULL;
}

static bool remove_tree(const char *path, char *err, size_t errn)
{
    if (!is_dir(path) && !is_file(path)) return true;
    if (removefile(path, NULL, REMOVEFILE_RECURSIVE) != 0)
    {
        snprintf(err, errn, "cannot remove %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

/* Remove the quarantine attribute from each file of the copy of the
   Recompiler directory, so that the tools can run. macOS removes an
   attribute only if the owner can write the file. Some files of the
   release are read-only (libclang_rt.osx.a has mode 0444), so a read-only
   file or folder of the copy (the copy belongs to the loader) gets the
   write permission of the owner before the removal. */
static void strip_quarantine(const char *root)
{
    static const char attr[] = "com.apple.quarantine";
    char *paths[2] = { (char *)root, NULL };
    FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    FTSENT *e;
    if (fts == NULL) fail(@"The converter cannot be prepared.", str(strerror(errno)));
    while ((e = fts_read(fts)) != NULL)
    {
        if (e->fts_info == FTS_DP) continue;
        if (e->fts_info == FTS_ERR || e->fts_info == FTS_DNR || e->fts_info == FTS_NS)
            fail(@"The converter cannot be prepared.", str(strerror(e->fts_errno)));
        if (getxattr(e->fts_path, attr, NULL, 0, 0, XATTR_NOFOLLOW) < 0)
        {
            if (errno == ENOATTR) continue;
            fail(@"The converter cannot be prepared.",
                 [NSString stringWithFormat:@"%s: %s", e->fts_path, strerror(errno)]);
        }
        if (removexattr(e->fts_path, attr, XATTR_NOFOLLOW) == 0) continue;
        if (errno == EACCES && !S_ISLNK(e->fts_statp->st_mode) && (e->fts_statp->st_mode & S_IWUSR) == 0 &&
            chmod(e->fts_path, (e->fts_statp->st_mode & 07777) | S_IWUSR) == 0 &&
            removexattr(e->fts_path, attr, XATTR_NOFOLLOW) == 0)
            continue;
        fail(@"The converter cannot be prepared.", [NSString stringWithFormat:@"%s: %s", e->fts_path, strerror(errno)]);
    }
    fts_close(fts);
}

static void sha256_text(const char *text, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    int i;
    CC_SHA256(text, (CC_LONG)strlen(text), d);
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
    {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    out[64] = 0;
}

/* Remove each entry of the GameCode directory except keep. */
static void remove_other_caches(const char *base, const char *keep)
{
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:str(base) error:nil];
    for (NSString *n in names)
    {
        char p[PATH_LEN], err[PATH_LEN + 64];
        if ([n isEqualToString:str(keep)]) continue;
        if (!join(p, sizeof p, base, n.fileSystemRepresentation)) continue;
        if (!remove_tree(p, err, sizeof err)) fail(@"An old copy of the game code cannot be removed.", str(err));
    }
}

static void mkdir_p(const char *path)
{
    NSError *e = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:str(path)
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&e])
        fail(@"A folder cannot be made.", [NSString stringWithFormat:@"%s: %@", path, e.localizedDescription]);
}

static void get_bundle(const char *game_dir, char *bundle, size_t n)
{
    NSString *res = NSBundle.mainBundle.resourcePath, *exe = NSBundle.mainBundle.executablePath;
    const char *home = getenv("HOME");
    char recompiler[PATH_LEN], manifest[PATH_LEN], base[PATH_LEN], dir[PATH_LEN], marker[PATH_LEN],
        copy[PATH_LEN], work[PATH_LEN], err[PATH_LEN + 64], exe_sum[65], man_sum[65], key_text[160], key[65], id[17];
    convert_state *s;
    RTProgress *ui;
    pthread_t th;
    pthread_attr_t attr;
    FILE *f;

    if (res == nil || exe == nil || home == NULL) fail(@"The application is damaged.", @"The resources were not found.");
    if (!join(recompiler, sizeof recompiler, res.fileSystemRepresentation, "Recompiler") ||
        !join(manifest, sizeof manifest, recompiler, "manifest.txt") || !is_file(manifest))
        fail(@"The application is damaged.", @"Contents/Resources/Recompiler/manifest.txt was not found.");
    if (!rt_sha256_file(exe.fileSystemRepresentation, exe_sum, err, sizeof err) ||
        !rt_sha256_file(manifest, man_sum, err, sizeof err))
        fail(@"The application is damaged.", str(err));
    snprintf(key_text, sizeof key_text, "%s\n%s\n", exe_sum, man_sum);
    sha256_text(key_text, key);
    memcpy(id, key, 16);
    id[16] = 0;

    if (!join(base, sizeof base, home, "Library/Application Support/" PORT_NAME "/GameCode") ||
        !join(dir, sizeof dir, base, id) || !join(marker, sizeof marker, dir, "complete") ||
        !join(copy, sizeof copy, dir, "Recompiler") || !join(work, sizeof work, dir, "work") ||
        !join(bundle, n, dir, "Prototype.bundle"))
        fail(@"The home folder path is too long.", str(home));
    mkdir_p(base);
    remove_other_caches(base, id);
    if (is_file(marker) && is_file(bundle)) return;

    /* A conversion that did not complete leaves files; start again. */
    if (!remove_tree(dir, err, sizeof err)) fail(@"An old copy of the game code cannot be removed.", str(err));
    mkdir_p(dir);
    if (copyfile(recompiler, copy, NULL, COPYFILE_CLONE | COPYFILE_RECURSIVE) != 0)
        fail(@"The converter cannot be prepared.",
             [NSString stringWithFormat:@"Copy of %s to %s: %s", recompiler, copy, strerror(errno)]);
    strip_quarantine(copy);
    mkdir_p(work);

    ensure_app();
    ui = [RTProgress new];
    [ui.window makeKeyAndOrderFront:nil];
    [NSApp activate];
    s = calloc(1, sizeof *s);
    if (s == NULL) fail(@"Out of memory.", @"The conversion cannot start.");
    s->job.recompiler = copy;
    s->job.game_dir = game_dir;
    s->job.work_dir = work;
    s->job.out_bundle = bundle;
    s->job.progress = on_progress;
    s->job.ctx = s;
    s->ui = (__bridge void *)ui;
    atomic_init(&s->finished, false);
    if (pthread_attr_init(&attr) != 0 || pthread_attr_setstacksize(&attr, 8u << 20) != 0 ||
        pthread_create(&th, &attr, convert_thread, s) != 0)
        fail(@"The conversion cannot start.", @"A thread cannot be made.");
    pthread_attr_destroy(&attr);
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:ui
                                                     andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                   forEventClass:kCoreEventClass
                                                      andEventID:kAEQuitApplication];
    while (!atomic_load(&s->finished))
    {
        @autoreleasepool
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:NSDate.distantFuture
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e != nil && e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
        }
    }
    [NSAppleEventManager.sharedAppleEventManager removeEventHandlerForEventClass:kCoreEventClass
                                                                  andEventID:kAEQuitApplication];
    if (pthread_join(th, NULL) != 0) fail(@"The conversion failed.", @"pthread_join failed.");
    [ui.window orderOut:nil];
    if (!s->ok)
    {
        char rm_err[PATH_LEN + 64];
        bool closed = ui.closed;
        NSString *msg = str(s->job.error);
        remove_tree(dir, rm_err, sizeof rm_err);
        free(s);
        if (closed) exit(0);
        fail(@"Prototype cannot be prepared for this Mac.", msg);
    }
    free(s);
    if (!remove_tree(work, err, sizeof err) || !remove_tree(copy, err, sizeof err))
        fail(@"The temporary files of the conversion cannot be removed.", str(err));
    f = fopen(marker, "w");
    if (f == NULL || fprintf(f, "%s\n", key) < 0 || fclose(f) != 0)
        fail(@"The converted game code cannot be recorded.", [NSString stringWithFormat:@"%s: %s", marker, strerror(errno)]);
    /* A quit or a close that came after the conversion completed: keep the
       result for the next start, but do not start the game. */
    if (ui.closed) exit(0);
}

#else

static void get_bundle(const char *game_dir, char *bundle, size_t n)
{
    NSString *res = NSBundle.mainBundle.resourcePath;
    (void)game_dir;
    if (res == nil || !join(bundle, n, res.fileSystemRepresentation, "Prototype.bundle") || !is_file(bundle))
        fail(@"The development build is not complete.", @"Contents/Resources/Prototype.bundle was not found. Run "
                                                         @"tools/build_app.sh.");
}

#endif

int main(int argc, char **argv)
{
    char game[PATH_LEN], bundle[PATH_LEN];
    void *h;
    rt_game_main_fn game_main;

    @autoreleasepool
    {
        find_game_dir(argc, argv, game, sizeof game);
        check_game_files(game);
        get_bundle(game, bundle, sizeof bundle);
        h = dlopen(bundle, RTLD_NOW | RTLD_LOCAL);
        if (h == NULL)
        {
            const char *e = dlerror();
            fail(@"The game code cannot be loaded.",
                 [NSString stringWithFormat:@"%s: %s", bundle, e != NULL ? e : "unknown error"]);
        }
        game_main = (rt_game_main_fn)dlsym(h, RT_GAME_MAIN_SYMBOL);
        if (game_main == NULL)
            fail(@"The game code is damaged.", [NSString stringWithFormat:@"%s has no %s.", bundle, RT_GAME_MAIN_SYMBOL]);
    }
    game_main(game);
    fail(@"The game code returned.", @"rt_game_main must not return.");
}
