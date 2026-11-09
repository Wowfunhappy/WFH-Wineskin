/*
 * MACDRV Cocoa file panels
 *
 * WINESKIN: NSOpenPanel and NSSavePanel for the common file dialogs. A panel is a sheet on its
 * owner window when that window can take one, and otherwise a panel of its own. The calling
 * thread starts the panel, then polls it while it processes its messages; the main thread wakes
 * the thread's event queue when the user dismisses the panel.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#import "config.h"
#import "macdrv.h"
#import "wine/wineskin_file_panel.h"

#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_window.h"

#include <fnmatch.h>

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv);


@interface WineEventQueue (WineFilePanel)
    - (void) signalEventAvailable;
@end


@interface WineFilePanelController : NSObject <NSOpenSavePanelDelegate>
{
    NSSavePanel* panel;
    NSArray* filterSpecs;           /* per filter, its Win32 patterns, or NSNull for all files */
    NSArray* filterTypes;           /* per filter, its extensions, or NSNull for all files */
    NSArray* currentPatterns;       /* the open panel's selected filter's patterns, or nil */
    NSPopUpButton* typePopUp;
    WineEventQueue* queue;          /* of the thread that waits for the panel */
    NSWindow* sheetWindow;          /* the window the panel is a sheet on */
    NSWindow* sheetOwner;           /* the window the panel was a sheet on, once it has ended */
    BOOL hasTitle;                  /* the program gave the panel a title */
    id keyMonitor;

    /* Set on the main thread once the panel ends, read by the waiting thread. */
    BOOL done;
    NSArray* chosenURLs;
    NSUInteger chosenFilter;
}

    - (id) initWithFlags:(unsigned int)flags title:(NSString*)title prompt:(NSString*)prompt
               directory:(NSString*)directory fileName:(NSString*)fileName
             filterNames:(NSArray*)names filterSpecs:(NSArray*)specs filterIndex:(NSUInteger)index
                   queue:(WineEventQueue*)inQueue;
    - (void) beginWithOwner:(WineWindow*)owner;
    - (void) cancel;
    - (BOOL) getDone:(BOOL*)chosen URLs:(NSArray**)urls filter:(NSUInteger*)filter;

@end


/***********************************************************************
 *              patterns_from_spec
 *
 * The patterns of a Win32 filter spec, or nil if one of them matches every file.
 */
static NSArray* patterns_from_spec(NSString* spec)
{
    NSMutableArray* patterns = [NSMutableArray array];

    for (NSString* pattern in [spec componentsSeparatedByString:@";"])
    {
        pattern = [pattern stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (![pattern length]) continue;
        if ([pattern isEqualToString:@"*"] || [pattern isEqualToString:@"*.*"]) return nil;
        [patterns addObject:pattern];
    }

    return [patterns count] ? patterns : nil;
}


/***********************************************************************
 *              extensions_from_spec
 *
 * The extensions a Win32 filter spec such as "*.txt;*.text" names, or nil if it takes any
 * other pattern, such as "*.*" or "data??.bin", and so matches files the extensions can't name.
 */
static NSArray* extensions_from_spec(NSString* spec)
{
    NSMutableArray* extensions = [NSMutableArray array];
    NSCharacterSet* wildcards = [NSCharacterSet characterSetWithCharactersInString:@"*?."];

    for (NSString* pattern in [spec componentsSeparatedByString:@";"])
    {
        NSString* extension;

        pattern = [pattern stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (![pattern length]) continue;
        if (![pattern hasPrefix:@"*."]) return nil;

        extension = [pattern substringFromIndex:2];
        if (![extension length] || [extension rangeOfCharacterFromSet:wildcards].location != NSNotFound)
            return nil;
        if (![extensions containsObject:extension])
            [extensions addObject:extension];
    }

    return [extensions count] ? extensions : nil;
}


@implementation WineFilePanelController

    - (id) initWithFlags:(unsigned int)flags title:(NSString*)title prompt:(NSString*)prompt
               directory:(NSString*)directory fileName:(NSString*)fileName
             filterNames:(NSArray*)names filterSpecs:(NSArray*)specs filterIndex:(NSUInteger)index
                   queue:(WineEventQueue*)inQueue
    {
        self = [super init];
        if (self)
        {
            NSMutableArray* types = [NSMutableArray array];
            NSUInteger i;

            if (flags & WINESKIN_FILE_PANEL_SAVE)
                panel = [[NSSavePanel savePanel] retain];
            else
            {
                NSOpenPanel* openPanel = [NSOpenPanel openPanel];
                BOOL folders = (flags & WINESKIN_FILE_PANEL_FOLDERS) != 0;

                [openPanel setCanChooseFiles:!folders];
                [openPanel setCanChooseDirectories:folders];
                [openPanel setCanCreateDirectories:folders];
                [openPanel setAllowsMultipleSelection:(flags & WINESKIN_FILE_PANEL_MULTISELECT) != 0];
                panel = [openPanel retain];
            }

            if (flags & WINESKIN_FILE_PANEL_SHOW_HIDDEN)
                [panel setShowsHiddenFiles:YES];
            if ([title length])
            {
                [panel setTitle:title];
                hasTitle = YES;
            }
            if ([prompt length])
                [panel setPrompt:prompt];
            if ([directory length])
                [panel setDirectoryURL:[NSURL fileURLWithPath:directory isDirectory:YES]];
            if ([fileName length] && (flags & WINESKIN_FILE_PANEL_SAVE))
                [panel setNameFieldStringValue:fileName];

            NSMutableArray* patterns = [NSMutableArray array];

            for (i = 0; i < [specs count]; i++)
            {
                NSArray* extensions = extensions_from_spec(specs[i]);
                NSArray* specPatterns = patterns_from_spec(specs[i]);
                [types addObject:extensions ? (id)extensions : (id)[NSNull null]];
                [patterns addObject:specPatterns ? (id)specPatterns : (id)[NSNull null]];
            }
            filterTypes = [types copy];
            filterSpecs = [patterns copy];
            if (index >= [filterTypes count]) index = 0;
            chosenFilter = index;

            if ([filterTypes count] && !(flags & WINESKIN_FILE_PANEL_FOLDERS))
            {
                if ([filterTypes count] > 1)
                    [self addTypePopUpWithNames:names save:(flags & WINESKIN_FILE_PANEL_SAVE) != 0];
                [self applyFilter:index];
            }

            /* A Win32 program accepts whatever extension the user types. */
            [panel setAllowsOtherFileTypes:YES];

            queue = [inQueue retain];
        }
        return self;
    }

    - (void) dealloc
    {
        [panel setAccessoryView:nil];
        [panel setDelegate:nil];
        [panel release];
        [filterSpecs release];
        [filterTypes release];
        [currentPatterns release];
        [typePopUp release];
        [queue release];
        [sheetWindow release];
        [sheetOwner release];
        [chosenURLs release];
        [super dealloc];
    }

    - (void) addTypePopUpWithNames:(NSArray*)names save:(BOOL)save
    {
        NSView* view;
        NSTextField* label;
        NSRect frame;
        CGFloat width, height;
        NSUInteger i;

        label = [[[NSTextField alloc] initWithFrame:NSZeroRect] autorelease];
        [label setStringValue:save ? @"Format:" : @"Enable:"];
        [label setBezeled:NO];
        [label setDrawsBackground:NO];
        [label setEditable:NO];
        [label setSelectable:NO];
        [label setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
        [label sizeToFit];

        typePopUp = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        /* Items go in through the menu, which keeps items whose names repeat. */
        for (i = 0; i < [names count]; i++)
            [[typePopUp menu] addItemWithTitle:names[i] action:NULL keyEquivalent:@""];
        [typePopUp selectItemAtIndex:chosenFilter];
        [typePopUp setTarget:self];
        [typePopUp setAction:@selector(typeChanged:)];
        [typePopUp sizeToFit];

        frame = [typePopUp frame];
        if (frame.size.width > 400) frame.size.width = 400;
        width = [label frame].size.width + 8 + frame.size.width + 40;
        height = MAX([label frame].size.height, frame.size.height) + 24;

        view = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)] autorelease];
        frame.origin.x = 20 + [label frame].size.width + 8;
        frame.origin.y = floor((height - frame.size.height) / 2);
        [typePopUp setFrame:frame];
        [label setFrameOrigin:NSMakePoint(20, floor((height - [label frame].size.height) / 2) + 1)];
        [view addSubview:label];
        [view addSubview:typePopUp];
        [panel setAccessoryView:view];
    }

    /* An open panel filters through its delegate, which takes any Win32 pattern; a save panel
       takes the filter's extensions, which it gives a name that has none. */
    - (void) applyFilter:(NSUInteger)index
    {
        if ([panel isKindOfClass:[NSOpenPanel class]])
        {
            id patterns = filterSpecs[index];
            [currentPatterns release];
            currentPatterns = (patterns == [NSNull null]) ? nil : [patterns retain];
            [panel setDelegate:self];
            [panel validateVisibleColumns];
        }
        else
        {
            id types = filterTypes[index];
            [panel setAllowedFileTypes:(types == [NSNull null]) ? nil : types];
        }
    }

    - (BOOL) panel:(id)sender shouldEnableURL:(NSURL*)url
    {
        NSNumber* isDirectory = nil;
        NSNumber* isPackage = nil;

        if (!currentPatterns) return YES;

        [url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:NULL];
        [url getResourceValue:&isPackage forKey:NSURLIsPackageKey error:NULL];
        if ([isDirectory boolValue] && ![isPackage boolValue]) return YES;

        for (NSString* pattern in currentPatterns)
        {
            if (!fnmatch([pattern UTF8String], [[url lastPathComponent] UTF8String], FNM_CASEFOLD))
                return YES;
        }
        return NO;
    }

    - (void) typeChanged:(id)sender
    {
        NSInteger index = [typePopUp indexOfSelectedItem];
        if (index >= 0 && index < [filterTypes count])
            [self applyFilter:index];
    }

    /* The panel takes the standard editing key equivalents itself, whatever the main menu has. */
    - (void) addKeyMonitor
    {
        NSSavePanel* thePanel = panel;

        keyMonitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSKeyDownMask handler:^NSEvent*(NSEvent* event){
            NSUInteger modifiers = [event modifierFlags] & NSDeviceIndependentModifierFlagsMask;
            NSString* key = [[event charactersIgnoringModifiers] lowercaseString];
            SEL action = NULL;

            if ([event window] != thePanel) return event;

            modifiers &= ~(NSAlphaShiftKeyMask | NSNumericPadKeyMask | NSFunctionKeyMask);
            if (modifiers == NSCommandKeyMask)
            {
                if ([key isEqualToString:@"x"]) action = @selector(cut:);
                else if ([key isEqualToString:@"c"]) action = @selector(copy:);
                else if ([key isEqualToString:@"v"]) action = @selector(paste:);
                else if ([key isEqualToString:@"a"]) action = @selector(selectAll:);
                else if ([key isEqualToString:@"z"]) action = @selector(undo:);
            }
            else if (modifiers == (NSCommandKeyMask | NSShiftKeyMask) && [key isEqualToString:@"z"])
                action = @selector(redo:);

            if (action)
            {
                BOOL handled = [NSApp sendAction:action to:nil from:thePanel];
                TRACE("%s handled %d\n", sel_getName(action), handled);
                if (handled) return nil;
            }
            return event;
        }] retain];
    }

    - (void) panelEnded:(NSInteger)result
    {
        NSArray* urls = nil;

        /* A sheet is still attached while its completion handler runs, and a window with a sheet
           cannot become key; the owner is made key again once the program re-enables it
           (-restoreOwner). */
        if (sheetWindow) [panel orderOut:nil];

        if (keyMonitor)
        {
            [NSEvent removeMonitor:keyMonitor];
            [keyMonitor release];
            keyMonitor = nil;
        }

        if (result == NSFileHandlingPanelOKButton)
        {
            if ([panel isKindOfClass:[NSOpenPanel class]])
                urls = [(NSOpenPanel*)panel URLs];
            else if ([panel URL])
                urls = [NSArray arrayWithObject:[panel URL]];
        }

        @synchronized(self)
        {
            chosenURLs = [urls copy];
            if (typePopUp && [typePopUp indexOfSelectedItem] >= 0)
                chosenFilter = [typePopUp indexOfSelectedItem];
            done = YES;
        }

        [sheetOwner release];
        sheetOwner = sheetWindow;
        sheetWindow = nil;

        [queue signalEventAvailable];
    }

    - (void) beginWithOwner:(WineWindow*)owner
    {
        [self addKeyMonitor];

        if (owner && [owner isVisible] && ![owner isMiniaturized] && ![owner attachedSheet] &&
            ([owner styleMask] & NSTitledWindowMask))
        {
            /* A sheet shows no title; the message line carries it. */
            if (hasTitle)
                [panel setMessage:[panel title]];
            sheetWindow = [owner retain];
            [panel beginSheetModalForWindow:owner completionHandler:^(NSInteger result){
                [self panelEnded:result];
            }];
        }
        else
        {
            [[WineApplicationController sharedController] transformProcessToForeground:YES];
            if (owner && [owner isVisible] && [owner level] > [panel level])
                [panel setLevel:[owner level]];
            [panel beginWithCompletionHandler:^(NSInteger result){
                [self panelEnded:result];
            }];
            [NSApp activateIgnoringOtherApps:YES];
            [panel makeKeyAndOrderFront:nil];
        }
    }

    /* While the panel was up its owner was disabled, so Windows activation moved elsewhere, and the
       program cannot take it back itself. The owner becoming key gives it back, as a Windows dialog's
       owner is activated when the dialog closes. */
    - (void) restoreOwner
    {
        if (sheetOwner && [NSApp isActive] && [sheetOwner isVisible] && ![sheetOwner isMiniaturized] &&
            ![sheetOwner attachedSheet] && [sheetOwner canBecomeKeyWindow])
            [sheetOwner makeKeyWindow];
        [sheetOwner release];
        sheetOwner = nil;
    }

    - (void) cancel
    {
        BOOL ended;

        @synchronized(self)
        {
            ended = done;
        }
        if (!ended)
            [panel cancel:nil];
    }

    - (BOOL) getDone:(BOOL*)chosen URLs:(NSArray**)urls filter:(NSUInteger*)filter
    {
        @synchronized(self)
        {
            if (!done) return NO;
            *chosen = [chosenURLs count] != 0;
            *urls = [[chosenURLs retain] autorelease];
            *filter = chosenFilter;
        }
        return YES;
    }

@end


/***********************************************************************
 *              string_from_wchar
 */
static NSString* string_from_wchar(UINT64 ptr)
{
    const WCHAR *str = param_ptr(ptr);
    if (!str) return nil;
    return [NSString stringWithCharacters:(const unichar*)str length:wcslen(str)];
}


/***********************************************************************
 *              macdrv_end_file_panel_sheet
 *
 * Cancel a file panel that is a sheet on a window that is going away.
 */
void macdrv_end_file_panel_sheet(WineWindow *window)
{
    NSWindow* sheet = [window attachedSheet];
    if ([sheet isKindOfClass:[NSSavePanel class]])
        [(NSSavePanel*)sheet cancel:nil];
}


/***********************************************************************
 *              macdrv_file_panel_begin
 */
NTSTATUS macdrv_file_panel_begin(void *arg)
{
@autoreleasepool
{
    struct file_panel_begin_params *params = arg;
    struct macdrv_thread_data *thread_data;
    WineWindow *owner = NULL;
    NSMutableArray *names, *specs;
    NSString *title, *prompt, *directory = nil, *file_name;
    const WCHAR *filter;
    unsigned int flags = params->flags, filter_index = params->filter_index;
    UINT i;
    __block WineFilePanelController *controller = nil;

    if (!native_file_dialogs)
    {
        TRACE("native file dialogs are off\n");
        return STATUS_NOT_SUPPORTED;
    }

    thread_data = macdrv_init_thread_data();
    if (params->owner)
        owner = macdrv_get_cocoa_window(ULongToHandle(params->owner), TRUE);

    title = string_from_wchar(params->title);
    prompt = string_from_wchar(params->prompt);
    file_name = string_from_wchar(params->file_name);
    if (params->directory)
    {
        const char *path = param_ptr(params->directory);
        directory = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:path
                                                                                length:strlen(path)];
    }

    names = [NSMutableArray array];
    specs = [NSMutableArray array];
    filter = param_ptr(params->filters);
    for (i = 0; filter && i < params->filter_count; i++)
    {
        NSString *name = [NSString stringWithCharacters:(const unichar*)filter length:wcslen(filter)];
        filter += wcslen(filter) + 1;
        [names addObject:name];
        [specs addObject:[NSString stringWithCharacters:(const unichar*)filter length:wcslen(filter)]];
        filter += wcslen(filter) + 1;
    }

    TRACE("owner %p/%p flags 0x%x title %s directory %s name %s filters %u index %u\n",
          ULongToHandle(params->owner), owner, flags, debugstr_cf(title), debugstr_cf(directory),
          debugstr_cf(file_name), params->filter_count, filter_index);

    OnMainThread(^{
        controller = [[WineFilePanelController alloc] initWithFlags:flags title:title prompt:prompt
                                                          directory:directory fileName:file_name
                                                        filterNames:names filterSpecs:specs
                                                        filterIndex:filter_index
                                                              queue:thread_data->queue];
        [controller beginWithOwner:owner];
    });

    if (!controller) return STATUS_UNSUCCESSFUL;
    params->panel = (UINT_PTR)controller;
    return STATUS_SUCCESS;
}
}


/***********************************************************************
 *              macdrv_file_panel_poll
 */
NTSTATUS macdrv_file_panel_poll(void *arg)
{
@autoreleasepool
{
    struct file_panel_poll_params *params = arg;
    WineFilePanelController *controller = (WineFilePanelController*)param_ptr(params->panel);
    NSArray *urls;
    NSUInteger filter;
    BOOL chosen;
    UINT needed = 1;

    params->done = params->chosen = params->needed = params->copied = 0;
    if (![controller getDone:&chosen URLs:&urls filter:&filter]) return STATUS_SUCCESS;

    params->done = TRUE;
    params->chosen = chosen;
    params->filter_index = filter;

    for (NSURL *url in urls)
        needed += strlen([url fileSystemRepresentation]) + 1;
    params->needed = needed;

    if (params->buffer && params->size >= needed)
    {
        char *p = param_ptr(params->buffer);
        for (NSURL *url in urls)
        {
            const char *path = [url fileSystemRepresentation];
            strcpy(p, path);
            p += strlen(path) + 1;
        }
        *p = 0;
        params->copied = TRUE;
    }

    return STATUS_SUCCESS;
}
}


/***********************************************************************
 *              macdrv_file_panel_release
 *
 * Cancel the panel if it is still up, and release it.
 */
NTSTATUS macdrv_file_panel_release(void *arg)
{
    struct file_panel_poll_params *params = arg;
    WineFilePanelController *controller = (WineFilePanelController*)param_ptr(params->panel);

    if (!controller) return STATUS_SUCCESS;

    /* The panel's last release happens on the main thread. */
    OnMainThreadAsync(^{
        [controller cancel];
        [controller restoreOwner];
        [controller release];
    });
    return STATUS_SUCCESS;
}
