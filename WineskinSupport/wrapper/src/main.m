/*
 * WFH Wineskin launcher.
 *
 * One app bundle is the whole wrapper:
 *   Contents/MacOS/Wineskin                 this launcher
 *   Contents/Resources/Engine/              the Wine installation (bin/wine runs 32- and 64-bit programs)
 *   Contents/SharedSupport/prefix/          the Windows environment (WINEPREFIX), created on first use
 *   Contents/SharedSupport/Wineskin.plist   the program to run and the wrapper's options
 *   Contents/SharedSupport/Logs/            the output of the last run
 *
 * A configured wrapper runs its program in this process: before AppKit starts, the launcher execs Wine.
 * Every Wine process carries CFProcessPath, naming this launcher, from which CoreFoundation finds its main
 * bundle: the program is this app to the system -- one Dock icon, Keep in Dock and Recent Items. With no
 * program configured, or with Option held at launch, the launcher shows the configuration window instead.
 *
 * A wrapper is named by its bundle's file name: a copy renamed "Prince of Persia.app" is the application
 * "Prince of Persia", with an identifier of its own, so the system never confuses one wrapper for another.
 */

#import <Cocoa/Cocoa.h>
#include <signal.h>
#include <unistd.h>

static NSString *const kProgramKey = @"Program";
static NSString *const kArgumentsKey = @"Arguments";
static NSString *const kDebugLogKey = @"DebugLog";
static NSString *const kOptionAsAltKey = @"OptionAsAlt";
static NSString *const kRetinaModeKey = @"RetinaMode";
/* On unless set off: the Command keys act as Control, as a Mac user's shortcuts expect (Command-C copies,
   Command-S saves), and the app menu has the Mac's own Command-H and Command-Q. */
static NSString *const kCommandAsControlKey = @"CommandAsControl";

static BOOL CommandAsControl(NSDictionary *config)
{
    return config[kCommandAsControlKey] ? [config[kCommandAsControlKey] boolValue] : YES;
}
static NSString *const kWineDebugKey = @"WineDebug";

/* On unless set off: a fullscreen program's window goes full screen in a Space of its own, and its
   display mode changes are emulated, so the display keeps its own mode (winemac's FullScreenSpace and
   win32u's EmulateModeset). Off, the window covers the screen in the current Space and display mode
   changes are real. */
static NSString *const kFullScreenSpaceKey = @"FullScreenSpace";

static BOOL FullScreenSpace(NSDictionary *config)
{
    return config[kFullScreenSpaceKey] ? [config[kFullScreenSpaceKey] boolValue] : YES;
}

/* The program's AppData (on unless set off) and Documents (off unless set on) folders can live in the
   wrapper's own folder in ~/Library/Application Support, named by kDataFolderKey, where they outlast
   the wrapper. With Documents off, Documents is the Mac's, as Wine links it. */
static NSString *const kAppDataInSupportKey = @"AppDataInApplicationSupport";
static NSString *const kDocumentsInSupportKey = @"DocumentsInApplicationSupport";
static NSString *const kDataFolderKey = @"DataFolder";
/* Whether the prefix's text smoothing is set to ClearType (subpixel), as last matched to the Mac's. */
static NSString *const kLCDFontSmoothingKey = @"LCDFontSmoothing";

static BOOL AppDataInSupport(NSDictionary *config)
{
    return config[kAppDataInSupportKey] ? [config[kAppDataInSupportKey] boolValue] : YES;
}

static NSString *const kBundleIdentifierBase = @"com.wowfunhappy.wfhwineskin";
static NSString *const kConfigureArgument = @"--configure";

#pragma mark - Wrapper identity

static NSString *IdentifierForName(NSString *name)
{
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"];
    NSMutableString *component = [NSMutableString string];
    for (NSUInteger i = 0; i < name.length; i++) {
        unichar c = [name characterAtIndex:i];
        [component appendString:[allowed characterIsMember:c] ? [NSString stringWithCharacters:&c length:1] : @"-"];
    }
    return [NSString stringWithFormat:@"%@.%@", kBundleIdentifierBase, component];
}

/* Gives the bundle the name and identifier its file name calls for. Returns YES when they changed: this
   launch was registered under the old identity. */
static BOOL UpdateIdentity(void)
{
    NSURL *bundleURL = [[NSBundle mainBundle] bundleURL];
    NSString *name = [[bundleURL lastPathComponent] stringByDeletingPathExtension];
    NSString *identifier = IdentifierForName(name);
    NSString *plistPath = [[bundleURL path] stringByAppendingPathComponent:@"Contents/Info.plist"];
    NSMutableDictionary *info = [NSMutableDictionary dictionaryWithContentsOfFile:plistPath];
    if (!info || ([info[@"CFBundleName"] isEqualToString:name] && [info[@"CFBundleIdentifier"] isEqualToString:identifier]))
        return NO;
    info[@"CFBundleName"] = name;
    info[@"CFBundleIdentifier"] = identifier;
    if (![info writeToFile:plistPath atomically:YES])
        return NO;
    LSRegisterURL((__bridge CFURLRef)bundleURL, true);
    return YES;
}

/* Opens this wrapper again through Launch Services, under its current identity. */
static BOOL Relaunch(BOOL configure)
{
    NSDictionary *configuration = @{ NSWorkspaceLaunchConfigurationArguments: configure ? @[ kConfigureArgument ] : @[] };
    return [[NSWorkspace sharedWorkspace] launchApplicationAtURL:[[NSBundle mainBundle] bundleURL]
                                                         options:NSWorkspaceLaunchNewInstance
                                                   configuration:configuration error:NULL] != nil;
}

#pragma mark - Wrapper paths and configuration

@interface Wrapper : NSObject
- (NSString *)binPath;
@property (readonly) NSString *bundlePath;
@property (readonly) NSString *enginePath;
@property (readonly) NSString *prefixPath;
@property (readonly) NSString *configPath;
@property (readonly) NSString *logsPath;
@property (readonly) NSMutableDictionary *config;
- (NSString *)name;
- (NSString *)wineBinary;
- (NSString *)wineserverBinary;
- (NSDictionary *)environment;
- (BOOL)prefixExists;
- (void)save;
- (NSString *)programUnixPath;
- (NSString *)storedPathForUnixPath:(NSString *)path;
- (NSString *)driveCPath;
@end

@implementation Wrapper

- (instancetype)init
{
    if ((self = [super init])) {
        NSBundle *bundle = [NSBundle mainBundle];
        _bundlePath = [bundle bundlePath];
        _enginePath = [[bundle resourcePath] stringByAppendingPathComponent:@"Engine"];
        NSString *support = [bundle sharedSupportPath];
        _prefixPath = [support stringByAppendingPathComponent:@"prefix"];
        _configPath = [support stringByAppendingPathComponent:@"Wineskin.plist"];
        _logsPath = [support stringByAppendingPathComponent:@"Logs"];
        _config = [NSMutableDictionary dictionaryWithContentsOfFile:_configPath] ?: [NSMutableDictionary dictionary];
    }
    return self;
}

- (NSString *)name
{
    return [[[NSFileManager defaultManager] displayNameAtPath:self.bundlePath] stringByDeletingPathExtension];
}

- (NSString *)binPath { return [self.enginePath stringByAppendingPathComponent:@"bin"]; }
- (NSString *)wineBinary { return [[self binPath] stringByAppendingPathComponent:@"wine"]; }
- (NSString *)wineserverBinary { return [[self binPath] stringByAppendingPathComponent:@"wineserver"]; }
- (NSString *)driveCPath { return [self.prefixPath stringByAppendingPathComponent:@"drive_c"]; }

- (BOOL)prefixExists
{
    return [[NSFileManager defaultManager] fileExistsAtPath:[self.prefixPath stringByAppendingPathComponent:@"system.reg"]];
}

- (void)save
{
    [[NSFileManager defaultManager] createDirectoryAtPath:[self.configPath stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES attributes:nil error:NULL];
    [self.config writeToFile:self.configPath atomically:YES];
}

/* Programs inside the prefix are stored relative to it, so a copied or moved wrapper keeps working. */
- (NSString *)storedPathForUnixPath:(NSString *)path
{
    NSString *prefix = [[self.prefixPath stringByStandardizingPath] stringByAppendingString:@"/"];
    NSString *standard = [path stringByStandardizingPath];
    if ([standard hasPrefix:prefix]) return [standard substringFromIndex:prefix.length];
    return standard;
}

- (NSString *)programUnixPath
{
    NSString *stored = self.config[kProgramKey];
    if (!stored.length) return nil;
    if ([stored isAbsolutePath]) return stored;
    return [self.prefixPath stringByAppendingPathComponent:stored];
}

- (NSDictionary *)environment
{
    NSMutableDictionary *env = [[[NSProcessInfo processInfo] environment] mutableCopy];
    NSString *lib = [self.enginePath stringByAppendingPathComponent:@"lib"];
    env[@"WINEPREFIX"] = self.prefixPath;
    env[@"CFProcessPath"] = [[NSBundle mainBundle] executablePath];
    /* Every Wine process takes this bundle as its own. The process this one becomes (ExecProgram) is
       the bundle's application; winemac starts the others as agents, which become applications when
       they show a window. */
    env[@"WINESKIN_APP_PID"] = [NSString stringWithFormat:@"%d", getpid()];
    env[@"PATH"] = [NSString stringWithFormat:@"%@:/usr/bin:/bin:/usr/sbin:/sbin", [self binPath]];
    env[@"DYLD_FALLBACK_LIBRARY_PATH"] = [NSString stringWithFormat:@"%@:/usr/lib", lib];
    /* The wrapper is offline-first: Mono ships in the engine, and Gecko is off, so Wine never offers to
       download either. winemenubuilder is off: a wrapper exports no menu entries or file associations
       to the Mac. */
    env[@"WINEDLLOVERRIDES"] = @"mshtml=;winemenubuilder.exe=d";
    /* A WINEDEBUG already in the environment (from a command line) is kept. */
    NSString *debug = self.config[kWineDebugKey];
    if (!env[@"WINEDEBUG"])
    {
        if ([self.config[kDebugLogKey] boolValue])
            env[@"WINEDEBUG"] = debug.length ? debug : @"err+all,warn+module,fixme+all";
        else
            env[@"WINEDEBUG"] = @"-all";
    }
    [env removeObjectForKey:@"WINELOADERNOEXEC"];
    return env;
}

@end

#pragma mark - Running Wine

static NSString *LogFilePath(Wrapper *w, NSString *name)
{
    [[NSFileManager defaultManager] createDirectoryAtPath:w.logsPath withIntermediateDirectories:YES attributes:nil error:NULL];
    return [w.logsPath stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"log"]];
}

static NSTask *StartWine(Wrapper *w, NSArray *args, NSString *directory, NSString *logName)
{
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = [w wineBinary];
    task.arguments = args;
    task.environment = [w environment];
    NSString *support = [w.prefixPath stringByDeletingLastPathComponent];
    [[NSFileManager defaultManager] createDirectoryAtPath:support withIntermediateDirectories:YES attributes:nil error:NULL];
    task.currentDirectoryPath = directory ?: support;
    NSString *log = LogFilePath(w, logName);
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:log];
    task.standardOutput = fh;
    task.standardError = fh;
    [task launch];
    return task;
}

static void WaitForWineserver(Wrapper *w)
{
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = [w wineserverBinary];
    task.arguments = @[ @"-w" ];
    task.environment = [w environment];
    [task launch];
    [task waitUntilExit];
}

static void KillWine(Wrapper *w)
{
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = [w wineserverBinary];
    task.arguments = @[ @"-k" ];
    task.environment = [w environment];
    [task launch];
    [task waitUntilExit];
}

/* Mac driver options live in the prefix's registry. In Retina mode, Windows has two pixels per point
   each way, so the system DPI (LogPixels) is 192, as on Windows scaled to 200%; it is set when Retina
   mode is turned on or off (setDPI), leaving a DPI chosen in Wine's configuration alone otherwise. */
static void ApplyRegistryOptions(Wrapper *w, BOOL setDPI)
{
    NSString *reg = [NSString stringWithFormat:
        @"REGEDIT4\n\n[HKEY_CURRENT_USER\\Software\\Wine\\Mac Driver]\n"
        @"\"LeftOptionIsAlt\"=\"%@\"\n\"RightOptionIsAlt\"=\"%@\"\n\"RetinaMode\"=\"%@\"\n"
        @"\"LeftCommandIsCtrl\"=\"%@\"\n\"RightCommandIsCtrl\"=\"%@\"\n"
        @"\"FullScreenSpace\"=\"%@\"\n\"EmulateModeset\"=\"%@\"\n",
        [w.config[kOptionAsAltKey] boolValue] ? @"y" : @"n",
        [w.config[kOptionAsAltKey] boolValue] ? @"y" : @"n",
        [w.config[kRetinaModeKey] boolValue] ? @"y" : @"n",
        CommandAsControl(w.config) ? @"y" : @"n",
        CommandAsControl(w.config) ? @"y" : @"n",
        FullScreenSpace(w.config) ? @"y" : @"n",
        FullScreenSpace(w.config) ? @"y" : @"n"];
    if (setDPI)
        reg = [reg stringByAppendingFormat:@"\n[HKEY_CURRENT_USER\\Control Panel\\Desktop]\n\"LogPixels\"=dword:%08x\n",
               [w.config[kRetinaModeKey] boolValue] ? 192 : 96];
    NSString *file = [NSTemporaryDirectory() stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"wineskin-%d.reg", getpid()]];
    [reg writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    NSTask *task = StartWine(w, @[ @"regedit", @"/S", file ], nil, @"Registry");
    [task waitUntilExit];
    [[NSFileManager defaultManager] removeItemAtPath:file error:NULL];
}

/* The command that starts Wine on a program, for the program's architecture. Started on a program
   of the other architecture, a loader runs it in a new process (through start.exe), which would leave
   this one, the wrapper's application, as an empty launcher; and the 64-bit loader on its own starts
   the 32-bit one. So a 64-bit program gets the 64-bit preloader and loader, told not to switch
   (WINELOADERNOEXEC, as Wine's loaders tell each other). */
static NSArray *LoaderCommandForProgram(Wrapper *w, NSString *program, NSMutableDictionary *env)
{
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:program];
    NSData *dos = [fh readDataOfLength:0x40];
    uint32_t pe = 0;
    uint16_t machine = 0;

    if (dos.length == 0x40)
    {
        [dos getBytes:&pe range:NSMakeRange(0x3c, 4)];
        [fh seekToFileOffset:OSSwapLittleToHostInt32(pe) + 4];
        NSData *header = [fh readDataOfLength:2];
        if (header.length == 2) [header getBytes:&machine length:2];
    }
    [fh closeFile];

    if (OSSwapLittleToHostInt16(machine) == 0x8664 /* IMAGE_FILE_MACHINE_AMD64 */)
    {
        NSString *dir = [w.enginePath stringByAppendingPathComponent:@"lib/wine/x86_64-unix"];
        NSString *preloader = [dir stringByAppendingPathComponent:@"wine-preloader"];
        NSString *loader = [dir stringByAppendingPathComponent:@"wine"];
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:preloader] &&
            [[NSFileManager defaultManager] isExecutableFileAtPath:loader])
        {
            env[@"WINELOADERNOEXEC"] = @"1";
            return @[ preloader, loader ];
        }
    }
    return @[ [w wineBinary] ];
}

/* Each user's Desktop folder is a directory in the prefix rather than Wine's link to the Mac's
   Desktop, so the shortcuts installers put there stay out of Finder. */
static void KeepDesktopInPrefix(Wrapper *w)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *users = [[w driveCPath] stringByAppendingPathComponent:@"users"];

    for (NSString *user in [fm contentsOfDirectoryAtPath:users error:NULL])
    {
        NSString *desktop = [[users stringByAppendingPathComponent:user] stringByAppendingPathComponent:@"Desktop"];
        NSDictionary *attrs = [fm attributesOfItemAtPath:desktop error:NULL];

        if (![attrs[NSFileType] isEqual:NSFileTypeSymbolicLink]) continue;
        if ([fm removeItemAtPath:desktop error:NULL])
            [fm createDirectoryAtPath:desktop withIntermediateDirectories:NO attributes:nil error:NULL];
    }
}

/* The wrapper's folder in ~/Library/Application Support. It is named after the wrapper the first time
   it is needed and keeps that name, so a renamed wrapper keeps its data. */
static NSString *DataFolderPath(Wrapper *w)
{
    NSString *name = w.config[kDataFolderKey];
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);

    if (!name.length)
    {
        name = [w name];
        w.config[kDataFolderKey] = name;
        [w save];
    }
    return [dirs.firstObject stringByAppendingPathComponent:name];
}

/* Makes a user folder in the prefix a link to another folder. Items of a folder that was a directory
   move into the other folder; one that was a link to elsewhere (Documents to the Mac's) is only
   replaced. */
static void LinkFolder(NSString *folder, NSString *target)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSDictionary *attrs = [fm attributesOfItemAtPath:folder error:NULL];

    [fm createDirectoryAtPath:target withIntermediateDirectories:YES attributes:nil error:NULL];
    if ([attrs[NSFileType] isEqual:NSFileTypeSymbolicLink])
    {
        if ([[fm destinationOfSymbolicLinkAtPath:folder error:NULL] isEqual:target]) return;
        [fm removeItemAtPath:folder error:NULL];
    }
    else if (attrs)
    {
        for (NSString *item in [fm contentsOfDirectoryAtPath:folder error:NULL])
        {
            NSString *to = [target stringByAppendingPathComponent:item];
            if (![fm fileExistsAtPath:to])
                [fm moveItemAtPath:[folder stringByAppendingPathComponent:item] toPath:to error:NULL];
        }
        if (![fm removeItemAtPath:folder error:NULL]) return;
    }
    [fm createSymbolicLinkAtPath:folder withDestinationPath:target error:NULL];
}

/* Undoes LinkFolder for a folder that belongs in the prefix: it is a directory again, with the items
   moved back. */
static void UnlinkFolder(NSString *folder, NSString *target)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSDictionary *attrs = [fm attributesOfItemAtPath:folder error:NULL];

    if (![attrs[NSFileType] isEqual:NSFileTypeSymbolicLink] ||
        ![[fm destinationOfSymbolicLinkAtPath:folder error:NULL] isEqual:target])
        return;
    [fm removeItemAtPath:folder error:NULL];
    [fm createDirectoryAtPath:folder withIntermediateDirectories:NO attributes:nil error:NULL];
    for (NSString *item in [fm contentsOfDirectoryAtPath:target error:NULL])
        [fm moveItemAtPath:[target stringByAppendingPathComponent:item]
                    toPath:[folder stringByAppendingPathComponent:item] error:NULL];
}

/* Puts each user's AppData and Documents folders where the options say. */
static void ApplyDataFolders(Wrapper *w)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *users = [[w driveCPath] stringByAppendingPathComponent:@"users"];
    BOOL appData = AppDataInSupport(w.config), documents = [w.config[kDocumentsInSupportKey] boolValue];
    NSString *data = (appData || documents) ? DataFolderPath(w) : nil;

    for (NSString *user in [fm contentsOfDirectoryAtPath:users error:NULL])
    {
        NSString *home = [users stringByAppendingPathComponent:user];
        NSString *docs = [home stringByAppendingPathComponent:@"Documents"];
        NSDictionary *attrs;

        if ([user isEqual:@"Public"] || [user hasPrefix:@"."]) continue;

        if (appData)
            LinkFolder([home stringByAppendingPathComponent:@"AppData"], [data stringByAppendingPathComponent:@"AppData"]);
        else if (w.config[kDataFolderKey])
            UnlinkFolder([home stringByAppendingPathComponent:@"AppData"],
                         [DataFolderPath(w) stringByAppendingPathComponent:@"AppData"]);

        if (documents)
            LinkFolder(docs, [data stringByAppendingPathComponent:@"Documents"]);
        else if ((attrs = [fm attributesOfItemAtPath:docs error:NULL]) &&
                 [attrs[NSFileType] isEqual:NSFileTypeSymbolicLink] && w.config[kDataFolderKey] &&
                 [[fm destinationOfSymbolicLinkAtPath:docs error:NULL]
                     isEqual:[DataFolderPath(w) stringByAppendingPathComponent:@"Documents"]])
        {
            /* back to the Mac's Documents; what the program saved stays in Application Support */
            NSString *mac = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
            [fm removeItemAtPath:docs error:NULL];
            [fm createSymbolicLinkAtPath:docs withDestinationPath:mac error:NULL];
        }
    }
}

/* Whether the Mac draws text with LCD (subpixel) smoothing: the default, and any AppleFontSmoothing
   level but 0, which "Use LCD font smoothing when available" off sets. */
static BOOL MacUsesLCDFontSmoothing(void)
{
    CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("AppleFontSmoothing"), kCFPreferencesAnyApplication);
    int level = 2;

    if (value)
    {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue(value, kCFNumberIntType, &level);
        CFRelease(value);
    }
    return level != 0;
}

/* Windows text smoothing follows the Mac's: ClearType when the Mac uses LCD smoothing, standard
   (grayscale) otherwise. The registry is written only when the Mac's setting differs from the one
   last matched, so a launch normally costs nothing. */
static void MatchFontSmoothing(Wrapper *w)
{
    BOOL lcd = MacUsesLCDFontSmoothing();

    if (w.config[kLCDFontSmoothingKey] && [w.config[kLCDFontSmoothingKey] boolValue] == lcd) return;

    NSString *reg = [NSString stringWithFormat:
        @"REGEDIT4\n\n[HKEY_CURRENT_USER\\Control Panel\\Desktop]\n"
        @"\"FontSmoothing\"=\"2\"\n\"FontSmoothingType\"=dword:%08x\n\"FontSmoothingOrientation\"=dword:00000001\n",
        lcd ? 2 /* FE_FONTSMOOTHINGCLEARTYPE */ : 1 /* FE_FONTSMOOTHINGSTANDARD */];
    NSString *file = [NSTemporaryDirectory() stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"wineskin-%d.reg", getpid()]];
    [reg writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    NSTask *task = StartWine(w, @[ @"regedit", @"/S", file ], nil, @"Registry");
    [task waitUntilExit];
    [[NSFileManager defaultManager] removeItemAtPath:file error:NULL];
    if (task.terminationStatus == 0)
    {
        w.config[kLCDFontSmoothingKey] = @(lcd);
        [w save];
    }
}

/* Replace this process with Wine running the configured program. */
static void ExecProgram(Wrapper *w, NSArray *extraArgs)
{
    NSString *program = [w programUnixPath];
    NSMutableDictionary *env = [[w environment] mutableCopy];
    NSString *log = LogFilePath(w, @"Last Run");

    KeepDesktopInPrefix(w);
    ApplyDataFolders(w);
    MatchFontSmoothing(w);
    NSMutableArray *args = [LoaderCommandForProgram(w, program, env) mutableCopy];
    [args addObject:program];
    NSString *argString = w.config[kArgumentsKey];
    if (argString.length)
        [args addObjectsFromArray:[argString componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]];
    [args addObjectsFromArray:extraArgs];
    [args removeObject:@""];

    chdir([[program stringByDeletingLastPathComponent] fileSystemRepresentation]);
    int fd = open([log fileSystemRepresentation], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }

    for (NSString *key in [[[NSProcessInfo processInfo] environment] allKeys])
        if (!env[key]) unsetenv([key UTF8String]);
    for (NSString *key in env) setenv([key UTF8String], [env[key] UTF8String], 1);

    char **argv = calloc(args.count + 1, sizeof(char *));
    for (NSUInteger i = 0; i < args.count; i++) argv[i] = strdup([args[i] fileSystemRepresentation]);
    execv(argv[0], argv);
    perror("execv");
    _exit(1);
}

#pragma mark - Configuration window

@interface ConfigController : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property (strong) Wrapper *wrapper;
@property (strong) NSWindow *window;
@property (strong) NSTextField *programField;
@property (strong) NSTextField *argumentsField;
@property (strong) NSButton *debugLogBox;
@property (strong) NSButton *optionAltBox;
@property (strong) NSButton *retinaBox;
@property (strong) NSButton *commandCtrlBox;
@property (strong) NSButton *appDataBox;
@property (strong) NSButton *documentsBox;
@property (strong) NSButton *fullScreenSpaceBox;
@property (strong) NSTextField *status;
@property (strong) NSProgressIndicator *spinner;
@property (strong) NSMutableArray *busyControls;
@property (strong) NSPanel *chooserPanel;
@property (strong) NSTableView *chooserTable;
@property (strong) NSArray *chooserItems;
@property (assign) NSInteger busyCount;
@end

@implementation ConfigController

- (NSButton *)button:(NSString *)title action:(SEL)action frame:(NSRect)frame
{
    NSButton *b = [[NSButton alloc] initWithFrame:frame];
    b.title = title;
    b.bezelStyle = NSRoundedBezelStyle;
    b.target = self;
    b.action = action;
    [self.window.contentView addSubview:b];
    [self.busyControls addObject:b];
    return b;
}

- (NSButton *)checkbox:(NSString *)title key:(NSString *)key frame:(NSRect)frame
{
    NSButton *b = [[NSButton alloc] initWithFrame:frame];
    [b setButtonType:NSSwitchButton];
    b.title = title;
    b.state = [self.wrapper.config[key] boolValue] ? NSOnState : NSOffState;
    b.target = self;
    b.action = @selector(optionChanged:);
    [self.window.contentView addSubview:b];
    return b;
}

- (NSTextField *)label:(NSString *)text frame:(NSRect)frame bold:(BOOL)bold
{
    NSTextField *l = [[NSTextField alloc] initWithFrame:frame];
    l.stringValue = text;
    l.bezeled = NO; l.drawsBackground = NO; l.editable = NO; l.selectable = NO;
    l.font = bold ? [NSFont boldSystemFontOfSize:13] : [NSFont systemFontOfSize:12];
    [self.window.contentView addSubview:l];
    return l;
}

- (void)show
{
    self.busyControls = [NSMutableArray array];
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 478)
                                              styleMask:NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = [NSString stringWithFormat:@"%@ — WFH Wineskin", [self.wrapper name]];
    self.window.delegate = self;
    [self.window center];

    CGFloat y = 438;
    [self label:@"Program" frame:NSMakeRect(20, y, 520, 20) bold:YES];
    y -= 28;
    self.programField = [[NSTextField alloc] initWithFrame:NSMakeRect(20, y, 410, 22)];
    self.programField.editable = NO;
    [self.programField.cell setPlaceholderString:@"No program chosen"];
    [self.window.contentView addSubview:self.programField];
    [self button:@"Choose…" action:@selector(chooseProgram:) frame:NSMakeRect(434, y - 6, 110, 32)];
    y -= 30;
    [self label:@"Arguments:" frame:NSMakeRect(20, y - 2, 80, 20) bold:NO];
    self.argumentsField = [[NSTextField alloc] initWithFrame:NSMakeRect(100, y, 330, 22)];
    self.argumentsField.target = self;
    self.argumentsField.action = @selector(optionChanged:);
    [self.window.contentView addSubview:self.argumentsField];
    [self button:@"Run" action:@selector(runProgram:) frame:NSMakeRect(434, y - 6, 110, 32)];

    y -= 48;
    [self label:@"Install" frame:NSMakeRect(20, y, 520, 20) bold:YES];
    y -= 34;
    [self button:@"Install Software…" action:@selector(installSoftware:) frame:NSMakeRect(14, y, 170, 32)];
    [self button:@"Open C: Drive" action:@selector(openDriveC:) frame:NSMakeRect(186, y, 170, 32)];
    [self button:@"Run Other Program…" action:@selector(runOther:) frame:NSMakeRect(358, y, 186, 32)];

    y -= 40;
    [self label:@"Tools" frame:NSMakeRect(20, y, 520, 20) bold:YES];
    y -= 34;
    [self button:@"Wine Configuration" action:@selector(winecfg:) frame:NSMakeRect(14, y, 170, 32)];
    [self button:@"Registry Editor" action:@selector(regedit:) frame:NSMakeRect(186, y, 170, 32)];
    [self button:@"Command Prompt" action:@selector(cmd:) frame:NSMakeRect(358, y, 186, 32)];
    y -= 32;
    [self button:@"Task Manager" action:@selector(taskmgr:) frame:NSMakeRect(14, y, 170, 32)];
    [self button:@"Show Logs" action:@selector(showLogs:) frame:NSMakeRect(186, y, 170, 32)];
    NSButton *kill = [self button:@"Kill Wine Processes" action:@selector(killWine:) frame:NSMakeRect(358, y, 186, 32)];
    [self.busyControls removeObject:kill];

    y -= 40;
    [self label:@"Options" frame:NSMakeRect(20, y, 520, 20) bold:YES];
    y -= 24;
    self.optionAltBox = [self checkbox:@"Option keys act as Alt" key:kOptionAsAltKey frame:NSMakeRect(20, y, 250, 20)];
    self.retinaBox = [self checkbox:@"Retina mode" key:kRetinaModeKey frame:NSMakeRect(290, y, 250, 20)];
    y -= 24;
    self.commandCtrlBox = [self checkbox:@"Command keys act as Control" key:kCommandAsControlKey frame:NSMakeRect(20, y, 250, 20)];
    self.commandCtrlBox.state = CommandAsControl(self.wrapper.config) ? NSOnState : NSOffState;
    self.debugLogBox = [self checkbox:@"Write a debug log of each run" key:kDebugLogKey frame:NSMakeRect(290, y, 250, 20)];
    y -= 24;
    self.appDataBox = [self checkbox:@"AppData in Application Support" key:kAppDataInSupportKey frame:NSMakeRect(20, y, 250, 20)];
    self.appDataBox.state = AppDataInSupport(self.wrapper.config) ? NSOnState : NSOffState;
    self.appDataBox.toolTip = @"Keep the program's settings and saved games in ~/Library/Application Support, where they outlast this app.";
    self.documentsBox = [self checkbox:@"Documents in Application Support" key:kDocumentsInSupportKey frame:NSMakeRect(290, y, 250, 20)];
    self.documentsBox.toolTip = @"Keep what the program saves to Documents in ~/Library/Application Support instead of your Documents folder.";
    y -= 24;
    self.fullScreenSpaceBox = [self checkbox:@"Full screen in its own Space" key:kFullScreenSpaceKey frame:NSMakeRect(20, y, 250, 20)];
    self.fullScreenSpaceBox.state = FullScreenSpace(self.wrapper.config) ? NSOnState : NSOffState;
    self.fullScreenSpaceBox.toolTip = @"A fullscreen program goes full screen in a Space of its own, as with the green button, "
        @"and its resolution changes are scaled to the screen instead of changing your display's resolution.";

    self.spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 18, 16, 16)];
    self.spinner.style = NSProgressIndicatorSpinningStyle;
    self.spinner.controlSize = NSSmallControlSize;
    self.spinner.displayedWhenStopped = NO;
    [self.window.contentView addSubview:self.spinner];
    self.status = [self label:@"Hold Option while opening this app to return to this window."
                        frame:NSMakeRect(42, 16, 380, 20) bold:NO];
    self.status.textColor = [NSColor disabledControlTextColor];
    NSButton *done = [self button:@"Done" action:@selector(done:) frame:NSMakeRect(434, 10, 110, 32)];
    done.keyEquivalent = @"\r";
    [self.busyControls removeObject:done];

    [self refresh];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];

    if (![self.wrapper prefixExists]) [self createPrefix];
}

- (void)refresh
{
    NSString *program = self.wrapper.config[kProgramKey];
    self.programField.stringValue = program ?: @"";
    self.argumentsField.stringValue = self.wrapper.config[kArgumentsKey] ?: @"";
}

- (void)setBusy:(BOOL)busy message:(NSString *)message
{
    self.busyCount += busy ? 1 : -1;
    BOOL anyBusy = self.busyCount > 0;
    for (NSControl *c in self.busyControls) c.enabled = !anyBusy;
    if (anyBusy) [self.spinner startAnimation:nil]; else [self.spinner stopAnimation:nil];
    self.status.stringValue = message ?: @"";
}

/* Run Wine in the background; the completion runs on the main thread once the task and, when asked,
   every process of the prefix have exited. */
- (void)runWine:(NSArray *)args directory:(NSString *)directory log:(NSString *)log
        message:(NSString *)message waitServer:(BOOL)waitServer completion:(void (^)(int status))completion
{
    [self setBusy:YES message:message];
    Wrapper *w = self.wrapper;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        NSTask *task = StartWine(w, args, directory, log);
        [task waitUntilExit];
        if (waitServer) WaitForWineserver(w);
        int status = task.terminationStatus;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setBusy:NO message:@""];
            if (completion) completion(status);
        });
    });
}

- (void)createPrefix
{
    [self runWine:@[ @"wineboot", @"--init" ] directory:nil log:@"Prefix Creation"
          message:@"Creating the Windows environment…" waitServer:YES completion:^(int status) {
        if (![self.wrapper prefixExists]) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"The Windows environment could not be created.";
            alert.informativeText = @"See “Prefix Creation.log” in the wrapper's logs.";
            [alert runModal];
            return;
        }
        KeepDesktopInPrefix(self.wrapper);
        ApplyDataFolders(self.wrapper);
        ApplyRegistryOptions(self.wrapper, [self.wrapper.config[kRetinaModeKey] boolValue]);
        self.status.stringValue = @"Ready.";
    }];
}

- (NSOpenPanel *)exePanel
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[ @"exe", @"EXE", @"msi", @"MSI", @"bat", @"BAT" ];
    panel.allowsOtherFileTypes = NO;
    panel.canChooseDirectories = NO;
    return panel;
}

- (NSArray *)argsForFile:(NSString *)path
{
    NSString *ext = path.pathExtension.lowercaseString;
    if ([ext isEqualToString:@"msi"]) return @[ @"msiexec", @"/i", path ];
    if ([ext isEqualToString:@"bat"]) return @[ @"cmd", @"/c", path ];
    return @[ path ];
}

- (void)installSoftware:(id)sender
{
    NSOpenPanel *panel = [self exePanel];
    panel.title = @"Choose the installer to run";
    if ([panel runModal] != NSFileHandlingPanelOKButton) return;
    NSString *installer = panel.URL.path;
    NSSet *before = [NSSet setWithArray:[self programs]];
    [self runWine:[self argsForFile:installer] directory:[installer stringByDeletingLastPathComponent]
              log:@"Install" message:@"Installing… (the installer runs in its own windows)" waitServer:YES
       completion:^(int status) {
        [self offerProgramsNotIn:before];
    }];
}

- (void)runOther:(id)sender
{
    NSOpenPanel *panel = [self exePanel];
    panel.directoryURL = [NSURL fileURLWithPath:[self.wrapper driveCPath]];
    if ([panel runModal] != NSFileHandlingPanelOKButton) return;
    NSString *path = panel.URL.path;
    [self runWine:[self argsForFile:path] directory:[path stringByDeletingLastPathComponent]
              log:@"Other Program" message:[NSString stringWithFormat:@"Running %@…", path.lastPathComponent]
       waitServer:NO completion:nil];
}

/* The programs in the C: drive. Uninstallers and setup programs are left out, as are Windows' own folders
   and the programs Wine installs in every prefix. */
- (NSArray *)programs
{
    NSMutableArray *found = [NSMutableArray array];
    NSString *root = [self.wrapper driveCPath];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSDirectoryEnumerator *e = [fm enumeratorAtURL:[NSURL fileURLWithPath:root]
                        includingPropertiesForKeys:@[ NSURLContentModificationDateKey, NSURLIsDirectoryKey ]
                                           options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:nil];
    for (NSURL *url in e) {
        NSString *rel = [url.path substringFromIndex:root.length];
        NSString *lower = rel.lowercaseString;
        if ([lower isEqualToString:@"/windows"] || [lower isEqualToString:@"/users"] ||
            [lower isEqualToString:@"/programdata"] || [lower hasSuffix:@"/common files"] ||
            [lower hasSuffix:@"/internet explorer"] || [lower hasSuffix:@"/windows media player"] ||
            [lower hasSuffix:@"/windows nt"]) {
            [e skipDescendants];
            continue;
        }
        if (![lower hasSuffix:@".exe"]) continue;
        NSString *name = lower.lastPathComponent;
        BOOL skip = [name hasPrefix:@"unins"];
        for (NSString *word in @[ @"setup", @"install", @"crash", @"redist", @"vcredist" ])
            if ([name rangeOfString:word].location != NSNotFound) skip = YES;
        if (skip) continue;
        [found addObject:url.path];
    }
    return found;
}

/* The programs an installer added, largest first: a game's own executable is usually its largest. */
- (void)offerProgramsNotIn:(NSSet *)before
{
    NSMutableArray *programs = [NSMutableArray array];
    for (NSString *path in [self programs])
        if (![before containsObject:path]) [programs addObject:path];
    [programs sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        unsigned long long sa = [[[NSFileManager defaultManager] attributesOfItemAtPath:a error:NULL] fileSize];
        unsigned long long sb = [[[NSFileManager defaultManager] attributesOfItemAtPath:b error:NULL] fileSize];
        return sa > sb ? NSOrderedAscending : sa < sb ? NSOrderedDescending : NSOrderedSame;
    }];
    if (!programs.count) {
        self.status.stringValue = @"Installed. Choose the program to run.";
        return;
    }
    self.chooserItems = programs;
    self.chooserPanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 300)
                                                   styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
    NSView *content = self.chooserPanel.contentView;
    NSTextField *title = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 262, 480, 20)];
    title.stringValue = @"Which program should this wrapper run?";
    title.bezeled = NO; title.drawsBackground = NO; title.editable = NO;
    title.font = [NSFont boldSystemFontOfSize:13];
    [content addSubview:title];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 56, 480, 198)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    self.chooserTable = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"path"];
    col.width = 460;
    [self.chooserTable addTableColumn:col];
    self.chooserTable.headerView = nil;
    self.chooserTable.dataSource = self;
    self.chooserTable.delegate = self;
    self.chooserTable.target = self;
    self.chooserTable.doubleAction = @selector(chooserOK:);
    scroll.documentView = self.chooserTable;
    [content addSubview:scroll];
    [self.chooserTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    NSButton *ok = [[NSButton alloc] initWithFrame:NSMakeRect(394, 12, 112, 32)];
    ok.title = @"Use Program"; ok.bezelStyle = NSRoundedBezelStyle; ok.keyEquivalent = @"\r";
    ok.target = self; ok.action = @selector(chooserOK:);
    [content addSubview:ok];
    NSButton *cancel = [[NSButton alloc] initWithFrame:NSMakeRect(282, 12, 112, 32)];
    cancel.title = @"Not Now"; cancel.bezelStyle = NSRoundedBezelStyle; cancel.keyEquivalent = @"\033";
    cancel.target = self; cancel.action = @selector(chooserCancel:);
    [content addSubview:cancel];
    [NSApp beginSheet:self.chooserPanel modalForWindow:self.window modalDelegate:nil didEndSelector:NULL contextInfo:NULL];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView { return self.chooserItems.count; }

- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    return [self.chooserItems[row] substringFromIndex:[self.wrapper driveCPath].length - @"drive_c".length];
}

- (void)endChooser
{
    [NSApp endSheet:self.chooserPanel];
    [self.chooserPanel orderOut:nil];
    self.chooserPanel = nil;
}

- (void)chooserOK:(id)sender
{
    NSInteger row = self.chooserTable.selectedRow;
    if (row >= 0) [self setProgram:self.chooserItems[row]];
    [self endChooser];
}

- (void)chooserCancel:(id)sender { [self endChooser]; }

- (void)setProgram:(NSString *)path
{
    self.wrapper.config[kProgramKey] = [self.wrapper storedPathForUnixPath:path];
    [self.wrapper save];
    [self refresh];
    self.status.stringValue = [NSString stringWithFormat:@"%@ will run when this app opens.", path.lastPathComponent];
}

- (void)chooseProgram:(id)sender
{
    NSOpenPanel *panel = [self exePanel];
    panel.allowedFileTypes = @[ @"exe", @"EXE", @"bat", @"BAT" ];
    NSString *current = [self.wrapper programUnixPath];
    panel.directoryURL = [NSURL fileURLWithPath:current ? [current stringByDeletingLastPathComponent] : [self.wrapper driveCPath]];
    if ([panel runModal] == NSFileHandlingPanelOKButton) [self setProgram:panel.URL.path];
}

/* Saves the window's options; YES when the Mac driver options in the registry need applying, and
   *retinaChanged YES when Retina mode was turned on or off. */
- (BOOL)saveOptions:(BOOL *)retinaChanged
{
    NSMutableDictionary *c = self.wrapper.config;
    c[kArgumentsKey] = self.argumentsField.stringValue ?: @"";
    c[kDebugLogKey] = @(self.debugLogBox.state == NSOnState);
    *retinaChanged = [c[kRetinaModeKey] boolValue] != (self.retinaBox.state == NSOnState);
    BOOL registryChanged = [c[kOptionAsAltKey] boolValue] != (self.optionAltBox.state == NSOnState) ||
                           *retinaChanged ||
                           CommandAsControl(c) != (self.commandCtrlBox.state == NSOnState) ||
                           FullScreenSpace(c) != (self.fullScreenSpaceBox.state == NSOnState);
    c[kOptionAsAltKey] = @(self.optionAltBox.state == NSOnState);
    c[kRetinaModeKey] = @(self.retinaBox.state == NSOnState);
    c[kCommandAsControlKey] = @(self.commandCtrlBox.state == NSOnState);
    c[kFullScreenSpaceKey] = @(self.fullScreenSpaceBox.state == NSOnState);
    BOOL foldersChanged = AppDataInSupport(c) != (self.appDataBox.state == NSOnState) ||
                          [c[kDocumentsInSupportKey] boolValue] != (self.documentsBox.state == NSOnState);
    c[kAppDataInSupportKey] = @(self.appDataBox.state == NSOnState);
    c[kDocumentsInSupportKey] = @(self.documentsBox.state == NSOnState);
    [self.wrapper save];
    if (foldersChanged && [self.wrapper prefixExists]) ApplyDataFolders(self.wrapper);
    return registryChanged && [self.wrapper prefixExists];
}

- (void)optionChanged:(id)sender
{
    BOOL retinaChanged;
    if ([self saveOptions:&retinaChanged]) {
        Wrapper *w = self.wrapper;
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{ ApplyRegistryOptions(w, retinaChanged); });
    }
}

/* The program runs as this process, as when the wrapper is opened, so it keeps the wrapper's Dock icon. */
- (void)runProgram:(id)sender
{
    BOOL retinaChanged;
    if ([self saveOptions:&retinaChanged]) ApplyRegistryOptions(self.wrapper, retinaChanged);
    if (![self.wrapper programUnixPath]) { [self chooseProgram:nil]; return; }
    ExecProgram(self.wrapper, @[]);
}

- (void)tool:(NSArray *)args name:(NSString *)name
{
    [self runWine:args directory:nil log:name message:[NSString stringWithFormat:@"Running %@…", name]
       waitServer:NO completion:nil];
}

- (void)winecfg:(id)sender { [self tool:@[ @"winecfg" ] name:@"Wine Configuration"]; }
- (void)regedit:(id)sender { [self tool:@[ @"regedit" ] name:@"Registry Editor"]; }
- (void)taskmgr:(id)sender { [self tool:@[ @"taskmgr" ] name:@"Task Manager"]; }
- (void)cmd:(id)sender { [self tool:@[ @"wineconsole", @"cmd" ] name:@"Command Prompt"]; }

- (void)openDriveC:(id)sender
{
    [[NSWorkspace sharedWorkspace] openFile:[self.wrapper driveCPath]];
}

- (void)showLogs:(id)sender
{
    [[NSFileManager defaultManager] createDirectoryAtPath:self.wrapper.logsPath withIntermediateDirectories:YES attributes:nil error:NULL];
    [[NSWorkspace sharedWorkspace] openFile:self.wrapper.logsPath];
}

- (void)killWine:(id)sender
{
    Wrapper *w = self.wrapper;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{ KillWine(w); });
}

- (void)done:(id)sender
{
    [self optionChanged:nil];
    [self.window close];
}

- (void)windowWillClose:(NSNotification *)notification
{
    if (notification.object == self.window) [NSApp terminate:nil];
}

@end

#pragma mark - Startup

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) Wrapper *wrapper;
@property (strong) ConfigController *config;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    self.config = [[ConfigController alloc] init];
    self.config.wrapper = self.wrapper;
    [self.config show];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }

@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        /* Command-line use: Wineskin --run <program args...> runs a program in this wrapper's prefix. */
        if (argc > 2 && !strcmp(argv[1], "--run")) {
            Wrapper *w = [[Wrapper alloc] init];
            NSMutableArray *args = [NSMutableArray array];
            for (int i = 2; i < argc; i++) [args addObject:@(argv[i])];
            NSTask *task = [[NSTask alloc] init];
            task.launchPath = [w wineBinary];
            task.arguments = args;
            task.environment = [w environment];
            [task launch];
            [task waitUntilExit];
            return task.terminationStatus;
        }
        BOOL optionHeld = (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskAlternate) != 0;
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], kConfigureArgument.UTF8String)) optionHeld = YES;
        if (UpdateIdentity() && Relaunch(optionHeld))
            return 0;

        Wrapper *wrapper = [[Wrapper alloc] init];
        NSString *program = [wrapper programUnixPath];
        if (!optionHeld && program && [[NSFileManager defaultManager] fileExistsAtPath:program] && [wrapper prefixExists])
            ExecProgram(wrapper, @[]);

        signal(SIGPIPE, SIG_IGN);
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        delegate.wrapper = wrapper;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
