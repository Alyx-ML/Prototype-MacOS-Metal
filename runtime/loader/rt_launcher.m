/*
 * The launcher window of the loader. See rt_launcher.h.
 *
 * The window is dark, with the title in DIN Condensed Bold (a font of
 * macOS) and the red of the game as the accent. The controls are standard
 * AppKit controls in the dark appearance:
 *   - left: the graphics settings,
 *   - right, top: the game folder and the controller settings,
 *   - right, bottom: Restore Defaults and Play (Return), level with the
 *     bottom of the graphics settings.
 *
 * The settings file has the format of rt_enh.c: one line "<key> <value>"
 * for each setting (rt_enh_defs.h). A file that does not exist gives the
 * default values. A file with an error is not changed until the player
 * selects Play; the window tells the player about the error.
 */
#import <Cocoa/Cocoa.h>

#include "rt_enh_defs.h"
#include "rt_launcher.h"
#include "rt_launcher_art.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAUNCHER_PATH_LEN 1024
#define MAX_VALUES 8

/* The text of a setting in the window. */
typedef struct setting_text {
    rt_enh_id id;
    const char *label;
    const char *hint;
    const char *values[MAX_VALUES]; /* one name for each value 0 to the maximum */
} setting_text;

static const setting_text graphics_rows[] = {
    { RT_ENH_HDR, "HDR", "Brighter highlights on an HDR display.", { "Off", "On" } },
    { RT_ENH_SHADOW_RES, "Shadow Resolution", "Sharper shadows. Uses more graphics memory.", { "Game", "2x", "4x" } },
    { RT_ENH_METALFX, "MetalFX Upscaling", "Scales a lower game resolution up to the window.", { "Off", "On" } },
    { RT_ENH_SHARPEN, "Sharpening", "Makes textures look crisper.", { "Off", "Low", "Medium", "High" } },
    { RT_ENH_SMAA, "Anti-Aliasing (SMAA)", "Smooths jagged edges. T2x also uses the last frame.", { "Off", "1x", "T2x" } },
    { RT_ENH_FOV, "Field of View", "Shows more of the city around the camera.",
      { "Game", "+5°", "+10°", "+15°", "+20°", "+25°", "+30°" } },
    { RT_ENH_AO, "Ambient Occlusion", "Darker corners and contact areas.", { "Off", "Low", "Medium", "High" } },
    { RT_ENH_PCSS, "Soft Shadows", "Shadow edges get softer with distance.", { "Off", "Low", "High" } },
    { RT_ENH_CONTACT, "Contact Shadows", "Small shadows below objects and at feet.", { "Off", "On" } },
    { RT_ENH_BLOOM, "Bloom", "Kawase gives a smoother glow around lights.", { "Game", "Kawase" } },
    { RT_ENH_TONEMAP, "Tone Mapping", "GT7 gives softer highlights and truer colors.", { "Game", "GT7" } },
};

static const setting_text controller_rows[] = {
    { RT_ENH_BUTTON_ICONS, "Button Icons", "PlayStation or Xbox buttons for your controller.", { "Game", "Controller" } },
    { RT_ENH_TRIGGERS, "Adaptive Triggers", "Light trigger resistance on a DualSense.", { "Off", "On" } },
    { RT_ENH_RUMBLE, "Enhanced Rumble", "Richer vibration on supported controllers.", { "Off", "On" } },
};

#define NGRAPHICS (sizeof graphics_rows / sizeof graphics_rows[0])
#define NCONTROLLER (sizeof controller_rows / sizeof controller_rows[0])
_Static_assert(NGRAPHICS + NCONTROLLER == RT_ENH_COUNT, "each setting needs one row in the launcher window");

/* ---------------------------------------------------------------- palette */

static NSColor *rgb(unsigned hex)
{
    return [NSColor colorWithSRGBRed:(CGFloat)((hex >> 16) & 0xFFu) / 255.0
                               green:(CGFloat)((hex >> 8) & 0xFFu) / 255.0
                                blue:(CGFloat)(hex & 0xFFu) / 255.0
                               alpha:1.0];
}

#define C_GROUND 0x060508u  /* window */
#define C_PANEL 0x0F0E12u   /* setting groups */
#define C_TEXT 0xEFEBE5u    /* labels (16:1 on the panel) */
#define C_HINT 0x9A948Cu    /* hints (6.4:1 on the panel) */
#define C_RED 0xC4142Bu     /* the rule and the Play button (white text 6:1) */
#define C_RED_TEXT 0xE8344Au /* group titles (4.9:1 on the window) */
#define C_OK 0x6CC08Bu
#define C_WARN 0xE8A93Eu

/* ---------------------------------------------------------------- errors */

static _Noreturn void launcher_fail(NSString *title, NSString *text)
{
    NSAlert *a = [NSAlert new];
    fprintf(stderr, "Prototype: %s %s\n", title.UTF8String, text.UTF8String);
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = title;
    a.informativeText = text;
    [a runModal];
    exit(1);
}

static NSString *nsstr(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    return r != nil ? r : @"(a name that is not UTF-8)";
}

/* ---------------------------------------------------------------- settings file */

/* Read the settings file into values (the defaults first). Returns nil, or
   the problem of the file. */
static NSString *read_settings(const char *path, int values[RT_ENH_COUNT])
{
    char line[256];
    unsigned i, num = 0;
    FILE *f;
    for (i = 0; i < RT_ENH_COUNT; i++) values[i] = rt_enh_defs[i].def;
    f = fopen(path, "r");
    if (f == NULL)
    {
        if (errno == ENOENT) return nil;
        return [NSString stringWithFormat:@"The settings file cannot be read (%s).", strerror(errno)];
    }
    while (fgets(line, sizeof line, f) != NULL)
    {
        char key[128], extra[2];
        int value, n;
        bool found = false;
        num++;
        if (strchr(line, '\n') == NULL && !feof(f))
        {
            fclose(f);
            return [NSString stringWithFormat:@"Line %u of the settings file is too long.", num];
        }
        n = sscanf(line, "%127s %d %1s", key, &value, extra);
        if (n == EOF) continue;
        if (n != 2)
        {
            fclose(f);
            return [NSString stringWithFormat:@"Line %u of the settings file is not \"<setting> <value>\".", num];
        }
        if (strcmp(key, RT_ENH_REMOVED_KEY) == 0) continue;
        for (i = 0; i < RT_ENH_COUNT; i++)
        {
            if (strcmp(key, rt_enh_defs[i].key) != 0) continue;
            found = true;
            if (value < 0 || value > rt_enh_defs[i].max)
            {
                fclose(f);
                return [NSString stringWithFormat:@"Line %u of the settings file: %s %d is not in the range 0 to %d.", num,
                                                  key, value, rt_enh_defs[i].max];
            }
            values[i] = value;
        }
        if (!found)
        {
            fclose(f);
            return [NSString stringWithFormat:@"Line %u of the settings file has the unknown setting %s.", num, key];
        }
    }
    if (ferror(f))
    {
        int e = errno;
        fclose(f);
        return [NSString stringWithFormat:@"The settings file cannot be read (%s).", strerror(e)];
    }
    fclose(f);
    return nil;
}

/* Write the settings file (a temporary file, then a rename). Returns nil,
   or the problem. */
static NSString *write_settings(const char *path, const int values[RT_ENH_COUNT])
{
    char tmp[LAUNCHER_PATH_LEN + 8];
    NSString *dir = nsstr(path).stringByDeletingLastPathComponent;
    NSError *err = nil;
    FILE *f;
    unsigned i;
    int r = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (r <= 0 || (size_t)r >= sizeof tmp) return @"The path of the settings file is too long.";
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&err])
        return [NSString stringWithFormat:@"The folder %@ cannot be made: %@", dir, err.localizedDescription];
    f = fopen(tmp, "w");
    if (f == NULL) return [NSString stringWithFormat:@"The settings file cannot be written (%s).", strerror(errno)];
    for (i = 0; i < RT_ENH_COUNT; i++)
        if (fprintf(f, "%s %d\n", rt_enh_defs[i].key, values[i]) < 0)
        {
            int e = errno;
            fclose(f);
            remove(tmp);
            return [NSString stringWithFormat:@"The settings file cannot be written (%s).", strerror(e)];
        }
    if (fclose(f) != 0)
    {
        int e = errno;
        remove(tmp);
        return [NSString stringWithFormat:@"The settings file cannot be written (%s).", strerror(e)];
    }
    if (rename(tmp, path) != 0)
    {
        int e = errno;
        remove(tmp);
        return [NSString stringWithFormat:@"The settings file cannot be replaced (%s).", strerror(e)];
    }
    return nil;
}

/* ---------------------------------------------------------------- window */

@interface RTLauncher : NSObject <NSWindowDelegate>
@property(nonatomic) NSWindow *window;
@property(nonatomic) NSTextField *path;
@property(nonatomic) NSImageView *statusIcon;
@property(nonatomic) NSTextField *status;
@property(nonatomic) NSTextField *banner;
@property(nonatomic) NSButton *play;
@property(nonatomic) NSMutableDictionary<NSNumber *, NSPopUpButton *> *popups;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL playChosen;
@property(nonatomic) NSTextField *titleText; /* shown until the art of a game folder is loaded */
@property(nonatomic) NSImageView *logo;
@property(nonatomic) NSImageView *backdrop;
@end

@implementation RTLauncher
{
@public
    char settingsPath[LAUNCHER_PATH_LEN];
    char dir[LAUNCHER_PATH_LEN];
    bool dirOK;
    rt_launcher_check_fn check;
    char artDir[LAUNCHER_PATH_LEN]; /* the folder of the shown art, or "" */
}

/* An NSImage that owns the pixels of img (img becomes empty). */
static void free_pixels(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)(uintptr_t)data);
}

static NSImage *image_from(rt_art_image *img)
{
    size_t bytes = (size_t)img->width * img->height * 4;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp;
    CGImageRef cg;
    NSImage *ns;
    if (cs == NULL) launcher_fail(@"The title art cannot be shown.", @"The sRGB color space cannot be made.");
    dp = CGDataProviderCreateWithData(NULL, img->rgba, bytes, free_pixels);
    if (dp == NULL) launcher_fail(@"The title art cannot be shown.", @"The image data cannot be made.");
    img->rgba = NULL; /* the provider frees the pixels */
    cg = CGImageCreate(img->width, img->height, 8, 32, (size_t)img->width * 4, cs,
                       (CGBitmapInfo)kCGImageAlphaLast | kCGBitmapByteOrderDefault, dp, NULL, false,
                       kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (cg == NULL) launcher_fail(@"The title art cannot be shown.", @"The image cannot be made.");
    ns = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(img->width, img->height)];
    CGImageRelease(cg);
    img->width = img->height = 0;
    return ns;
}

static NSTextField *text_label(NSString *s, NSFont *font, unsigned color)
{
    NSTextField *t = [NSTextField labelWithString:s];
    t.font = font;
    t.textColor = rgb(color);
    return t;
}

static NSTextField *group_title(NSString *s)
{
    NSFont *f = [NSFont fontWithName:@"DINCondensed-Bold" size:17];
    NSTextField *t;
    if (f == nil) launcher_fail(@"A font of macOS is missing.", @"The font DIN Condensed Bold is not installed.");
    t = [NSTextField labelWithAttributedString:[[NSAttributedString alloc]
                                                   initWithString:s.uppercaseString
                                                       attributes:@{
                                                           NSFontAttributeName : f,
                                                           NSForegroundColorAttributeName : rgb(C_RED_TEXT),
                                                           NSKernAttributeName : @1.6,
                                                       }]];
    t.accessibilityRole = NSAccessibilityStaticTextRole;
    return t;
}

#define PANEL_PAD 20 /* the left and right inset of a panel (panel()) */
#define MENU_WIDTH 118
#define GRID_GAP 16

/* A setting group for a panel of the given width: the title, then one row
   for each setting (the label and its hint at the left, the menu at the
   right). */
- (NSStackView *)groupWithTitle:(NSString *)title
                           rows:(const setting_text *)rows
                          count:(size_t)count
                     panelWidth:(CGFloat)width
{
    NSMutableArray<NSArray<NSView *> *> *cells = [NSMutableArray new];
    NSGridView *grid;
    NSStackView *stack;
    size_t i;
    for (i = 0; i < count; i++)
    {
        const setting_text *st = &rows[i];
        NSTextField *label = text_label(nsstr(st->label), [NSFont systemFontOfSize:13 weight:NSFontWeightMedium], C_TEXT);
        NSTextField *hint = text_label(nsstr(st->hint), [NSFont systemFontOfSize:11], C_HINT);
        NSStackView *texts = [NSStackView stackViewWithViews:@[ label, hint ]];
        NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        int v;
        texts.orientation = NSUserInterfaceLayoutOrientationVertical;
        texts.alignment = NSLayoutAttributeLeading;
        texts.spacing = 1;
        hint.lineBreakMode = NSLineBreakByWordWrapping;
        hint.preferredMaxLayoutWidth = width - 2 * PANEL_PAD - MENU_WIDTH - GRID_GAP;
        for (v = 0; v <= rt_enh_defs[st->id].max; v++)
        {
            if (st->values[v] == NULL) launcher_fail(@"The launcher is damaged.", @"A setting value has no name.");
            [pop addItemWithTitle:nsstr(st->values[v])];
        }
        pop.accessibilityLabel = nsstr(st->label);
        pop.toolTip = nsstr(st->hint);
        [pop.widthAnchor constraintEqualToConstant:MENU_WIDTH].active = YES;
        self.popups[@(st->id)] = pop;
        [cells addObject:@[ texts, pop ]];
    }
    grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing = 12;
    grid.columnSpacing = GRID_GAP;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementTrailing;
    grid.rowAlignment = NSGridRowAlignmentNone;
    for (i = 0; i < count; i++) [grid rowAtIndex:(NSInteger)i].yPlacement = NSGridCellPlacementCenter;
    stack = [NSStackView stackViewWithViews:@[ group_title(title), grid ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    [grid.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}

/* A panel hugs its content, and the content wants the smallest height
   that its views allow (a weak height-0 constraint: weaker than the
   compression resistance of the views, 750, stronger than the hugging of
   the right column, 250). So the free space of the right column goes
   between the panels and the buttons, not into the rows of a panel. A
   panel grows only when a required constraint makes it taller (the
   Graphics panel next to a taller right column). */
static NSView *panel(NSStackView *content, CGFloat width)
{
    NSView *p = [NSView new];
    NSLayoutConstraint *hug, *shortest;
    p.wantsLayer = YES;
    /* A little of the background glow shows through the panels. */
    p.layer.backgroundColor = [rgb(C_PANEL) colorWithAlphaComponent:0.84].CGColor;
    p.layer.cornerRadius = 12;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [p addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:p.leadingAnchor constant:PANEL_PAD],
        [content.trailingAnchor constraintEqualToAnchor:p.trailingAnchor constant:-PANEL_PAD],
        [content.topAnchor constraintEqualToAnchor:p.topAnchor constant:18],
        [content.bottomAnchor constraintLessThanOrEqualToAnchor:p.bottomAnchor constant:-20],
        [p.widthAnchor constraintEqualToConstant:width],
    ]];
    hug = [content.bottomAnchor constraintEqualToAnchor:p.bottomAnchor constant:-20];
    hug.priority = NSLayoutPriorityDefaultHigh;
    hug.active = YES;
    shortest = [content.heightAnchor constraintEqualToConstant:0];
    shortest.priority = 500;
    shortest.active = YES;
    return p;
}

- (NSStackView *)folderGroup
{
    NSButton *choose = [NSButton buttonWithTitle:@"Choose Folder…" target:self action:@selector(choose:)];
    NSStackView *statusRow, *stack;
    self.path = text_label(@"", [NSFont systemFontOfSize:12], C_TEXT);
    self.path.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.path.selectable = YES;
    self.path.accessibilityLabel = @"Game folder";
    [self.path setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    self.statusIcon = [NSImageView new];
    [self.statusIcon.widthAnchor constraintEqualToConstant:16].active = YES;
    [self.statusIcon.heightAnchor constraintEqualToConstant:16].active = YES;
    self.status = text_label(@"", [NSFont systemFontOfSize:12], C_HINT);
    self.status.lineBreakMode = NSLineBreakByWordWrapping;
    self.status.preferredMaxLayoutWidth = 250;
    statusRow = [NSStackView stackViewWithViews:@[ self.statusIcon, self.status ]];
    statusRow.alignment = NSLayoutAttributeTop;
    statusRow.spacing = 8;
    stack = [NSStackView stackViewWithViews:@[ group_title(@"Game folder"), self.path, statusRow, choose ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10;
    [stack setCustomSpacing:14 afterView:statusRow];
    [self.path.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    [statusRow.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}

- (void)showFolder
{
    NSString *problem;
    NSImage *img;
    unsigned color;
    if (dir[0] == 0)
    {
        self.path.stringValue = @"No folder selected";
        self.path.textColor = rgb(C_HINT);
        problem = @"Choose the installed Steam game folder (the folder that holds prototypef.exe).";
        dirOK = false;
    }
    else
    {
        self.path.stringValue = [nsstr(dir) stringByAbbreviatingWithTildeInPath];
        self.path.textColor = rgb(C_TEXT);
        self.path.toolTip = nsstr(dir);
        problem = check(dir);
        dirOK = problem == nil;
    }
    color = dirOK ? C_OK : C_WARN;
    img = [NSImage imageWithSystemSymbolName:dirOK ? @"checkmark.circle.fill" : @"exclamationmark.triangle.fill"
                    accessibilityDescription:dirOK ? @"Ready" : @"Problem"];
    if (img == nil) launcher_fail(@"A symbol of macOS is missing.", @"The status symbol cannot be loaded.");
    self.statusIcon.image = img;
    self.statusIcon.contentTintColor = rgb(color);
    self.status.stringValue = dirOK ? @"The Steam version of Prototype. Ready to play." : problem;
    self.status.textColor = dirOK ? rgb(C_TEXT) : rgb(C_WARN);
    self.play.enabled = dirOK;
    [self updateArt];
}

- (void)clearArt
{
    self.logo.image = nil;
    self.backdrop.image = nil;
    self.logo.hidden = YES;
    self.backdrop.hidden = YES;
    self.titleText.hidden = NO;
    artDir[0] = 0;
}

/* Show the logo and the background glow from the game files of the
   selected folder (rt_launcher_art.c). The repository and the release hold
   no game art, so the window shows the text title until a usable folder is
   selected. */
- (void)updateArt
{
    rt_art_image logoImg, glowImg;
    char err[512];
    if (!dirOK)
    {
        if (artDir[0] != 0) [self clearArt];
        return;
    }
    if (strcmp(artDir, dir) == 0) return;
    if (!rt_launcher_load_art(dir, &logoImg, &glowImg, err, sizeof err))
    {
        [self clearArt];
        [self showProblem:[@"The title art cannot be shown: " stringByAppendingString:nsstr(err)]];
        return;
    }
    self.logo.image = image_from(&logoImg);
    self.backdrop.image = image_from(&glowImg);
    self.titleText.hidden = YES;
    self.logo.hidden = NO;
    self.backdrop.hidden = NO;
    memcpy(artDir, dir, sizeof artDir);
}

- (void)showValues:(const int *)values
{
    unsigned i;
    for (i = 0; i < RT_ENH_COUNT; i++) [self.popups[@(i)] selectItemAtIndex:values[i]];
}

- (void)build
{
    NSFont *titleFont = [NSFont fontWithName:@"DINCondensed-Bold" size:58];
    NSTextField *title, *versionNote;
    NSView *rule, *left, *right, *content;
    NSStackView *header, *columns, *rightStack, *buttons, *notes, *actions, *root;
    NSButton *restore;
    if (titleFont == nil) launcher_fail(@"A font of macOS is missing.", @"The font DIN Condensed Bold is not installed.");
    self.popups = [NSMutableDictionary new];

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 640)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable |
                                                        NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Prototype";
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.backgroundColor = rgb(C_GROUND);
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;

    title = [NSTextField labelWithAttributedString:[[NSAttributedString alloc]
                                                       initWithString:@"PROTOTYPE"
                                                           attributes:@{
                                                               NSFontAttributeName : titleFont,
                                                               NSForegroundColorAttributeName : rgb(C_TEXT),
                                                               NSKernAttributeName : @4.0,
                                                           }]];
    self.titleText = title;
    self.logo = [NSImageView new];
    self.logo.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.logo.accessibilityLabel = @"Prototype";
    self.logo.hidden = YES;
    /* The text title and the logo have the same height, so the window does
       not change size when the art is loaded. */
    [title.heightAnchor constraintEqualToConstant:64].active = YES;
    [self.logo.widthAnchor constraintEqualToConstant:256].active = YES;
    [self.logo.heightAnchor constraintEqualToConstant:64].active = YES;
    header = [NSStackView stackViewWithViews:@[ title, self.logo ]];
    header.orientation = NSUserInterfaceLayoutOrientationVertical;
    header.alignment = NSLayoutAttributeLeading;
    header.spacing = 0;

    rule = [NSView new];
    rule.wantsLayer = YES;
    rule.layer.backgroundColor = rgb(C_RED).CGColor;
    [rule.heightAnchor constraintEqualToConstant:2].active = YES;

    self.play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
    self.play.keyEquivalent = @"\r";
    self.play.controlSize = NSControlSizeLarge;
    self.play.bezelColor = rgb(C_RED);
    self.play.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    [self.play.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;

    restore = [NSButton buttonWithTitle:@"Restore Defaults" target:self action:@selector(restore:)];
    restore.controlSize = NSControlSizeLarge;
    versionNote = text_label(@"The game uses these settings when it starts.",
                             [NSFont systemFontOfSize:11], C_HINT);
    self.banner = text_label(@"", [NSFont systemFontOfSize:12 weight:NSFontWeightMedium], C_WARN);
    self.banner.lineBreakMode = NSLineBreakByWordWrapping;
    self.banner.preferredMaxLayoutWidth = 330;
    self.banner.hidden = YES;
    versionNote.lineBreakMode = NSLineBreakByWordWrapping;
    versionNote.preferredMaxLayoutWidth = 330;
    notes = [NSStackView stackViewWithViews:@[ self.banner, versionNote ]];
    notes.orientation = NSUserInterfaceLayoutOrientationVertical;
    notes.alignment = NSLayoutAttributeLeading;
    notes.spacing = 4;
    buttons = [NSStackView stackViewWithViews:@[ restore, self.play ]];
    buttons.distribution = NSStackViewDistributionEqualSpacing;
    actions = [NSStackView stackViewWithViews:@[ notes, buttons ]];
    actions.orientation = NSUserInterfaceLayoutOrientationVertical;
    actions.alignment = NSLayoutAttributeLeading;
    actions.spacing = 12;
    [buttons.widthAnchor constraintEqualToAnchor:actions.widthAnchor].active = YES;
    [notes.widthAnchor constraintEqualToAnchor:actions.widthAnchor].active = YES;
    [actions.widthAnchor constraintEqualToConstant:330].active = YES;
    {
        NSLayoutConstraint *shortest = [actions.heightAnchor constraintEqualToConstant:0];
        shortest.priority = 500; /* as in panel() */
        shortest.active = YES;
    }

    /* Left: Graphics. Right: Game folder and Controller at the top, the
       notes and the buttons at the bottom. The two columns have the same
       height, so the free space of the right column is between the panels
       and the buttons, and Play is level with the bottom of Graphics. */
    left = panel([self groupWithTitle:@"Graphics" rows:graphics_rows count:NGRAPHICS panelWidth:480], 480);
    rightStack = [NSStackView new];
    rightStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    rightStack.alignment = NSLayoutAttributeLeading;
    rightStack.spacing = 16;
    [rightStack addView:panel([self folderGroup], 330) inGravity:NSStackViewGravityTop];
    [rightStack addView:panel([self groupWithTitle:@"Controller" rows:controller_rows count:NCONTROLLER panelWidth:330], 330)
              inGravity:NSStackViewGravityTop];
    [rightStack addView:actions inGravity:NSStackViewGravityBottom];
    [rightStack setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    right = rightStack;
    columns = [NSStackView stackViewWithViews:@[ left, right ]];
    columns.alignment = NSLayoutAttributeTop;
    columns.spacing = 16;
    [left.heightAnchor constraintEqualToAnchor:right.heightAnchor].active = YES;

    root = [NSStackView stackViewWithViews:@[ header, rule, columns ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 16;
    [root setCustomSpacing:6 afterView:header];
    [root setCustomSpacing:22 afterView:rule];
    root.translatesAutoresizingMaskIntoConstraints = NO;
    content = self.window.contentView;
    self.backdrop = [NSImageView new];
    self.backdrop.imageScaling = NSImageScaleAxesIndependently;
    self.backdrop.hidden = YES;
    self.backdrop.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:self.backdrop];
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:28],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-28],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:36],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-24],
        [rule.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [header.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [self.backdrop.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [self.backdrop.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [self.backdrop.topAnchor constraintEqualToAnchor:content.topAnchor],
        [self.backdrop.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    [self.window setContentSize:content.fittingSize];
    self.window.initialFirstResponder = self.play;
}

- (void)showProblem:(NSString *)text
{
    self.banner.stringValue = text != nil ? text : @"";
    self.banner.hidden = text == nil;
}

- (IBAction)choose:(id)sender
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    (void)sender;
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.allowsMultipleSelection = NO;
    p.prompt = @"Choose";
    p.message = @"Choose the installed Steam game folder of Prototype (the folder that holds prototypef.exe).";
    if (dir[0] != 0) p.directoryURL = [NSURL fileURLWithPath:nsstr(dir) isDirectory:YES];
    [p beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse r) {
                const char *fs;
                if (r != NSModalResponseOK || p.URL == nil) return;
                fs = p.URL.fileSystemRepresentation;
                if (fs == NULL || strlen(fs) >= sizeof self->dir)
                {
                    [self showProblem:@"The folder path is too long. Choose a folder with a shorter path."];
                    return;
                }
                memcpy(self->dir, fs, strlen(fs) + 1);
                [self showProblem:nil];
                [self showFolder];
              }];
}

- (IBAction)restore:(id)sender
{
    int values[RT_ENH_COUNT];
    unsigned i;
    (void)sender;
    for (i = 0; i < RT_ENH_COUNT; i++) values[i] = rt_enh_defs[i].def;
    [self showValues:values];
    [self showProblem:nil];
}

/* End the event loop of rt_launcher_run. The loop waits for the next
   event, so a posted event wakes it when the action came from outside the
   handling of an event (a menu action that AppKit sends later from the run
   loop). */
- (void)finishWithPlay:(BOOL)play
{
    NSEvent *wake;
    if (self.finished) return;
    self.finished = YES;
    self.playChosen = play;
    wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                              location:NSZeroPoint
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                               subtype:0
                                 data1:0
                                 data2:0];
    if (wake == nil) launcher_fail(@"The window cannot be closed.", @"The wake event cannot be made.");
    [NSApp postEvent:wake atStart:NO];
}

- (IBAction)play:(id)sender
{
    int values[RT_ENH_COUNT];
    unsigned i;
    NSString *problem;
    (void)sender;
    [self showFolder];
    if (!dirOK) return;
    for (i = 0; i < RT_ENH_COUNT; i++) values[i] = (int)self.popups[@(i)].indexOfSelectedItem;
    problem = write_settings(settingsPath, values);
    if (problem != nil)
    {
        [self showProblem:problem];
        return;
    }
    [self finishWithPlay:YES];
}

- (IBAction)quit:(id)sender
{
    (void)sender;
    [self finishWithPlay:NO];
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    [self finishWithPlay:NO];
    return YES;
}

/* The quit Apple event (Quit in the Dock, log out, shut down). AppKit
   installs its own handler in finishLaunching, which the runtime calls
   only when the game starts; without this handler the event got the
   error -1708 (not handled) and the window stayed open. */
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    [self finishWithPlay:NO];
}
@end

/* The application menu while the window is open: About is not needed;
   Quit closes the window. The runtime makes its own menu later
   (rt_cocoa_init). */
static void launcher_menu(RTLauncher *l)
{
    NSMenu *bar = [NSMenu new], *app = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new], *quit;
    quit = [[NSMenuItem alloc] initWithTitle:@"Quit Prototype" action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = l;
    [app addItem:quit];
    appItem.submenu = app;
    [bar addItem:appItem];
    NSApp.mainMenu = bar;
}

bool rt_launcher_run(const char *settings, const char *initial, rt_launcher_check_fn check, char *out, size_t n)
{
    RTLauncher *l = [RTLauncher new];
    int values[RT_ENH_COUNT];
    NSString *problem;
    if (strlen(settings) >= sizeof l->settingsPath) launcher_fail(@"The settings path is too long.", nsstr(settings));
    memcpy(l->settingsPath, settings, strlen(settings) + 1);
    l->check = check;
    l->dir[0] = 0;
    if (initial != NULL)
    {
        if (strlen(initial) >= sizeof l->dir) launcher_fail(@"The game folder path is too long.", nsstr(initial));
        memcpy(l->dir, initial, strlen(initial) + 1);
    }
    [l build];
    launcher_menu(l);
    problem = read_settings(settings, values);
    [l showValues:values];
    if (problem != nil)
        [l showProblem:[problem stringByAppendingString:@" The window shows the default values. Play writes a new file."]];
    [l showFolder];
    [l.window center];
    [NSApp activate];
    [l.window makeKeyAndOrderFront:nil];
    /* An event loop in the default run loop mode, not runModalForWindow:.
       AppKit sends the action of a main-menu item (Quit, Command-Q) later
       from the run loop, and a modal loop (NSModalPanelRunLoopMode) did not
       run it: Quit did nothing. The loader uses the same loop for its
       progress window. */
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:l
                                                     andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                   forEventClass:kCoreEventClass
                                                      andEventID:kAEQuitApplication];
    while (!l.finished)
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
    /* The game installs the handler of AppKit (rt_cocoa_init). */
    [NSAppleEventManager.sharedAppleEventManager removeEventHandlerForEventClass:kCoreEventClass
                                                                  andEventID:kAEQuitApplication];
    [l.window orderOut:nil];
    if (!l.playChosen) return false;
    if (strlen(l->dir) >= n) launcher_fail(@"The game folder path is too long.", nsstr(l->dir));
    memcpy(out, l->dir, strlen(l->dir) + 1);
    return true;
}
