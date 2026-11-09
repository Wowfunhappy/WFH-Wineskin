/*
 * MACDRV Cocoa menu bar
 *
 * WINESKIN: The Mac menu bar shows the Win32 menu bar of the key window, between the
 * application menu and the Window menu. Each WineWindow keeps the menu bar items built from
 * the latest snapshot of its Win32 menu bar; they go into the main menu while the window is key.
 * Before a popup opens, the window's thread initializes it (WM_INITMENUPOPUP) and sends a fresh
 * snapshot of it. Choosing an item, by mouse or key equivalent, posts an event to that thread.
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

#import <objc/runtime.h>

#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_menu.h"
#import "cocoa_window.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

WINE_DEFAULT_DEBUG_CHANNEL(menu);


/* How long the menu bar waits for a program to initialize a popup before it shows the popup
   as last seen. */
static const NSTimeInterval WineMenuPopupQueryTimeout = 0.5;

/* Associated objects of a WineWindow: the NSArray of its menu bar items, and the NSNumber hash
   of the snapshot they were built from (NSNull once a popup has been refreshed since). */
static char WineMenuItemsKey;
static char WineMenuHashKey;


/***********************************************************************
 *              macdrv_free_menu
 */
void macdrv_free_menu(struct macdrv_menu *menu)
{
    unsigned int i;

    if (!menu) return;

    for (i = 0; i < menu->count; i++)
    {
        if (menu->items[i].title)
            CFRelease(menu->items[i].title);
        macdrv_free_menu(menu->items[i].submenu);
    }
    free(menu->items);
    free(menu);
}


/* A popup of a Win32 menu bar. A NULL handle is a menu holding a single command that is
   directly in the menu bar. */
@interface WineMenu : NSMenu
{
    void* wineHandle;
    void* wineParent;
    unsigned int winePosition;
}

@property (nonatomic) void* wineHandle;
@property (nonatomic) void* wineParent;
@property (nonatomic) unsigned int winePosition;

@end

@implementation WineMenu

    @synthesize wineHandle, wineParent, winePosition;

@end


@interface WineMenuItem : NSMenuItem
{
    void* wineMenu;
    void* wineMenuParent;
    unsigned int wineMenuPosition;
    unsigned int winePosition;
    unsigned int wineCommandID;
}

@property (nonatomic) void* wineMenu;                   /* HMENU holding the item */
@property (nonatomic) void* wineMenuParent;             /* HMENU holding that menu */
@property (nonatomic) unsigned int wineMenuPosition;    /* position of that menu in its parent */
@property (nonatomic) unsigned int winePosition;        /* position of the item in its menu */
@property (nonatomic) unsigned int wineCommandID;

@end

@implementation WineMenuItem

    @synthesize wineMenu, wineMenuParent, wineMenuPosition, winePosition, wineCommandID;

@end


static NSImage* WineMenuRadioImage(void)
{
    static NSImage* image;

    if (!image)
    {
        image = [[NSImage imageWithSize:NSMakeSize(10, 10) flipped:NO drawingHandler:^BOOL(NSRect rect){
            [[NSColor blackColor] set];
            [[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 2.5, 2.5)] fill];
            return YES;
        }] retain];
        [image setTemplate:YES];
    }

    return image;
}


@interface WineMenuBarController ()

    - (void) updateForWindow:(NSWindow*)window;
    - (void) updateForKeyWindow;

@end


@implementation WineMenuBarController

    + (WineMenuBarController*) sharedController
    {
        static WineMenuBarController* sharedController;
        static dispatch_once_t once;

        dispatch_once(&once, ^{
            sharedController = [[self alloc] init];
        });

        return sharedController;
    }

    - (id) init
    {
        self = [super init];
        if (self)
        {
            NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

            [nc addObserver:self selector:@selector(windowDidBecomeKey:)
                       name:NSWindowDidBecomeKeyNotification object:nil];
            [nc addObserver:self selector:@selector(windowDidResignKey:)
                       name:NSWindowDidResignKeyNotification object:nil];
            [nc addObserver:self selector:@selector(windowWillClose:)
                       name:NSWindowWillCloseNotification object:nil];
            [nc addObserver:self selector:@selector(menuDidBeginTracking:)
                       name:NSMenuDidBeginTrackingNotification object:nil];
            [nc addObserver:self selector:@selector(menuDidEndTracking:)
                       name:NSMenuDidEndTrackingNotification object:nil];
        }
        return self;
    }


    /*
     * ---------- Building menus from snapshots ----------
     */
    - (WineMenu*) menuWithTitle:(NSString*)title snapshot:(const struct macdrv_menu*)snapshot
    {
        WineMenu* menu = [[[WineMenu alloc] initWithTitle:title] autorelease];
        [menu setAutoenablesItems:NO];
        [menu setDelegate:self];
        if (snapshot)
            [self populateMenu:menu fromSnapshot:snapshot];
        return menu;
    }

    - (WineMenuItem*) itemFromSnapshot:(const struct macdrv_menu_item*)snapshotItem
                                inMenu:(const struct macdrv_menu*)snapshot
    {
        NSString* title = (NSString*)snapshotItem->title;
        NSString* key = @"";
        WineMenuItem* item;

        if (snapshotItem->key_equivalent && !snapshotItem->submenu)
            key = [NSString stringWithCharacters:&snapshotItem->key_equivalent length:1];

        item = [[[WineMenuItem alloc] initWithTitle:title
                                             action:@selector(wineMenuItemSelected:)
                                      keyEquivalent:key] autorelease];
        [item setTarget:self];
        if ([key length])
        {
            NSUInteger mask = 0;
            if (snapshotItem->key_modifiers & MACDRV_KEY_EQUIVALENT_COMMAND)
                mask |= NSEventModifierFlagCommand;
            if (snapshotItem->key_modifiers & MACDRV_KEY_EQUIVALENT_SHIFT)
                mask |= NSEventModifierFlagShift;
            if (snapshotItem->key_modifiers & MACDRV_KEY_EQUIVALENT_OPTION)
                mask |= NSEventModifierFlagOption;
            [item setKeyEquivalentModifierMask:mask];
        }

        item.wineMenu = snapshot->handle;
        item.wineMenuParent = snapshot->parent;
        item.wineMenuPosition = snapshot->position;
        item.winePosition = snapshotItem->position;
        item.wineCommandID = snapshotItem->command_id;

        [item setEnabled:!(snapshotItem->flags & MACDRV_MENU_ITEM_DISABLED)];
        if (snapshotItem->flags & MACDRV_MENU_ITEM_CHECKED)
        {
            [item setState:NSOnState];
            if (snapshotItem->flags & MACDRV_MENU_ITEM_RADIO)
                [item setOnStateImage:WineMenuRadioImage()];
        }
        if (snapshotItem->flags & MACDRV_MENU_ITEM_DEFAULT)
        {
            NSFont* font = [[NSFontManager sharedFontManager] convertFont:[NSFont menuFontOfSize:0]
                                                              toHaveTrait:NSBoldFontMask];
            NSAttributedString* string = [[NSAttributedString alloc] initWithString:title
                                                                         attributes:@{ NSFontAttributeName: font }];
            [item setAttributedTitle:string];
            [string release];
        }

        if (snapshotItem->submenu)
            [item setSubmenu:[self menuWithTitle:title snapshot:snapshotItem->submenu]];

        return item;
    }

    - (void) populateMenu:(WineMenu*)menu fromSnapshot:(const struct macdrv_menu*)snapshot
    {
        BOOL separatorPending = NO;
        unsigned int i;

        menu.wineHandle = snapshot->handle;
        menu.wineParent = snapshot->parent;
        menu.winePosition = snapshot->position;

        [menu removeAllItems];
        for (i = 0; i < snapshot->count; i++)
        {
            const struct macdrv_menu_item* snapshotItem = &snapshot->items[i];

            if (snapshotItem->flags & MACDRV_MENU_ITEM_SEPARATOR)
            {
                separatorPending = YES;
                continue;
            }
            if (!snapshotItem->title)
                continue;

            if (separatorPending && [menu numberOfItems])
                [menu addItem:[NSMenuItem separatorItem]];
            separatorPending = NO;

            [menu addItem:[self itemFromSnapshot:snapshotItem inMenu:snapshot]];
        }
    }

    - (NSArray*) menuBarItemsFromSnapshot:(const struct macdrv_menu*)snapshot
    {
        NSMutableArray* items = [NSMutableArray array];
        unsigned int i;

        for (i = 0; i < snapshot->count; i++)
        {
            const struct macdrv_menu_item* snapshotItem = &snapshot->items[i];
            NSString* title = (NSString*)snapshotItem->title;
            NSMenuItem* item;

            if (!title || (snapshotItem->flags & MACDRV_MENU_ITEM_SEPARATOR))
                continue;

            if (snapshotItem->submenu)
                item = [self itemFromSnapshot:snapshotItem inMenu:snapshot];
            else
            {
                /* The Mac menu bar opens menus; a command directly in the Win32 menu bar
                   gets a menu of its own holding just that command. */
                WineMenu* menu = [self menuWithTitle:title snapshot:NULL];
                [menu addItem:[self itemFromSnapshot:snapshotItem inMenu:snapshot]];
                item = [[[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""] autorelease];
                [item setSubmenu:menu];
                [item setEnabled:!(snapshotItem->flags & MACDRV_MENU_ITEM_DISABLED)];
            }

            [[item submenu] setTitle:title];
            [items addObject:item];
        }

        return items;
    }


    /*
     * ---------- The main menu ----------
     */
    - (void) uninstallItems
    {
        for (NSMenuItem* item in installedItems)
        {
            if ([item menu])
                [[item menu] removeItem:item];
        }
        [installedItems release];
        installedItems = nil;
        [menuWindow release];
        menuWindow = nil;
    }

    - (void) installItemsForWindow:(WineWindow*)window
    {
        NSArray* items;
        NSMenu* mainMenu = [NSApp mainMenu];
        NSInteger index;

        [[window retain] autorelease];
        [self uninstallItems];

        items = objc_getAssociatedObject(window, &WineMenuItemsKey);
        if (![items count])
            return;

        menuWindow = [window retain];
        installedItems = [items retain];
        if (!mainMenu)
            return;

        index = [mainMenu indexOfItemWithSubmenu:[NSApp windowsMenu]];
        if (index < 0)
            index = [mainMenu numberOfItems];
        for (NSMenuItem* item in items)
        {
            if ([item menu])
                [[item menu] removeItem:item];
            [mainMenu insertItem:item atIndex:index++];
        }
    }

    - (void) mainMenuDidChange
    {
        if (menuWindow)
            [self installItemsForWindow:menuWindow];
        else
            [self updateForKeyWindow];
    }

    - (void) updateForWindow:(NSWindow*)window
    {
        if (tracking)
            return;

        if ([window isKindOfClass:[WineWindow class]] &&
            [objc_getAssociatedObject(window, &WineMenuItemsKey) count])
        {
            if (window != menuWindow)
                [self installItemsForWindow:(WineWindow*)window];
        }
        /* A Wine window without a menu bar of its own, such as a dialog, leaves the menus of a window
           still on screen in place, as a Mac app keeps its menus while one of its panels is key. */
        else if ([window isKindOfClass:[WineWindow class]] && menuWindow && [menuWindow isVisible])
            return;
        else
            [self uninstallItems];
    }

    - (void) updateForKeyWindow
    {
        NSWindow* window = [NSApp keyWindow];

        if (tracking)
            return;

        if (window)
            [self updateForWindow:window];
        else if ([NSApp isActive] && menuWindow && ![menuWindow isVisible])
            [self uninstallItems];
    }

    - (void) applyItems:(NSArray*)items hash:(NSNumber*)hash toWindow:(WineWindow*)window
    {
        BOOL installed = (window == menuWindow);

        [[window retain] autorelease];
        if (installed)
            [self uninstallItems];

        objc_setAssociatedObject(window, &WineMenuItemsKey, items, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(window, &WineMenuHashKey, hash, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        if (installed || window == [NSApp keyWindow])
            [self updateForWindow:window];
    }

    - (void) setMenu:(const struct macdrv_menu*)snapshot forWindow:(WineWindow*)window
    {
        NSNumber* hash = snapshot ? [NSNumber numberWithUnsignedInt:snapshot->hash] : nil;
        NSArray* items;
        id oldHash;

        if (window == pendingWindow)
            oldHash = pendingHash;
        else
            oldHash = objc_getAssociatedObject(window, &WineMenuHashKey);

        if (snapshot && [hash isEqual:oldHash])
            return;
        if (!snapshot && !oldHash && ![objc_getAssociatedObject(window, &WineMenuItemsKey) count])
            return;

        TRACE("window %p menu %p hash %08x\n", window, snapshot ? snapshot->handle : NULL,
              snapshot ? snapshot->hash : 0);

        items = snapshot ? [self menuBarItemsFromSnapshot:snapshot] : nil;

        /* Changing the menu bar while the user is in it would close it; wait. */
        if (tracking && window == menuWindow)
        {
            [pendingWindow release];
            pendingWindow = [window retain];
            [pendingItems release];
            pendingItems = [items retain];
            [pendingHash release];
            pendingHash = [hash retain];
            return;
        }

        [self applyItems:items hash:hash toWindow:window];
    }


    /*
     * ---------- Events to the window's thread ----------
     */
    - (void) postLoopEvent:(int)kind menu:(void*)menu window:(WineWindow*)window
    {
        macdrv_event* event = macdrv_create_event(WINDOW_MENU_LOOP, window);
        event->window_menu_loop.kind = kind;
        event->window_menu_loop.menu = menu;
        [window.queue postEvent:event];
        macdrv_release_event(event);
    }

    - (void) postCommandForItem:(WineMenuItem*)item keyEquivalent:(BOOL)keyEquivalent
    {
        macdrv_event* event;

        if (!menuWindow)
            return;

        TRACE("window %p menu %p position %u id %u key equivalent %d\n", menuWindow, item.wineMenu,
              item.winePosition, item.wineCommandID, keyEquivalent);

        event = macdrv_create_event(WINDOW_MENU_COMMAND, menuWindow);
        event->window_menu_command.menu = item.wineMenu;
        event->window_menu_command.menu_parent = item.wineMenuParent;
        event->window_menu_command.menu_position = item.wineMenuPosition;
        event->window_menu_command.position = item.winePosition;
        event->window_menu_command.command_id = item.wineCommandID;
        event->window_menu_command.key_equivalent = keyEquivalent;
        [menuWindow.queue postEvent:event];
        macdrv_release_event(event);
    }

    - (void) endMenuLoop
    {
        if (loopWindow)
        {
            [self postLoopEvent:MENU_LOOP_END menu:NULL window:loopWindow];
            [loopWindow release];
            loopWindow = nil;
        }
    }

    - (void) wineMenuItemSelected:(id)sender
    {
        if ([sender isKindOfClass:[WineMenuItem class]])
            [self postCommandForItem:sender keyEquivalent:NO];
    }


    /*
     * ---------- Key equivalents ----------
     */
    - (WineMenuItem*) itemInArray:(NSArray*)items matchingKey:(NSString*)key modifiers:(NSUInteger)modifiers
    {
        const NSUInteger relevant = NSEventModifierFlagCommand | NSEventModifierFlagShift |
                                    NSEventModifierFlagOption | NSEventModifierFlagControl;

        for (NSMenuItem* item in items)
        {
            if ([item hasSubmenu])
            {
                WineMenuItem* found = [self itemInArray:[[item submenu] itemArray]
                                            matchingKey:key
                                              modifiers:modifiers];
                if (found)
                    return found;
            }
            else if ([item isKindOfClass:[WineMenuItem class]] && [[item keyEquivalent] length] &&
                     ([item keyEquivalentModifierMask] & relevant) == modifiers &&
                     [[[item keyEquivalent] lowercaseString] isEqualToString:key])
                return (WineMenuItem*)item;
        }

        return nil;
    }

    - (BOOL) handleKeyEquivalent:(NSEvent*)event
    {
        NSUInteger modifiers = [event modifierFlags];
        NSString* key;
        WineMenuItem* item;

        if (!(modifiers & NSEventModifierFlagCommand) || !menuWindow || tracking ||
            [event window] != menuWindow)
            return NO;

        key = [[event charactersIgnoringModifiers] lowercaseString];
        if ([key length] != 1)
            return NO;

        modifiers &= NSEventModifierFlagCommand | NSEventModifierFlagShift |
                     NSEventModifierFlagOption | NSEventModifierFlagControl;
        item = [self itemInArray:installedItems matchingKey:key modifiers:modifiers];
        /* Shift may be what types a symbol such as "+" at all. */
        if (!item && (modifiers & NSEventModifierFlagShift) && [[key uppercaseString] isEqualToString:key])
        {
            item = [self itemInArray:installedItems
                         matchingKey:key
                           modifiers:modifiers & ~(NSUInteger)NSEventModifierFlagShift];
        }
        if (!item)
            return NO;

        /* The window's thread checks whether the item is enabled, after letting the program
           initialize it, as TranslateAccelerator does. */
        [self postCommandForItem:item keyEquivalent:YES];
        return YES;
    }


    /*
     * ---------- NSMenuDelegate methods ----------
     */
    - (void) menuNeedsUpdate:(NSMenu*)menu
    {
        WineMenu* wineMenu = (WineMenu*)menu;
        WineWindow* window = menuWindow;
        macdrv_query* query;
        BOOL begin;

        if (![menu isKindOfClass:[WineMenu class]] || !wineMenu.wineHandle || !window)
            return;

        /* Only a popup the user opens is initialized, never one AppKit merely looks at. */
        if (!tracking)
        {
            if (![[[NSRunLoop currentRunLoop] currentMode] isEqualToString:NSEventTrackingRunLoopMode])
                return;
            tracking = YES;
        }
        if (loopUnresponsive)
            return;

        /* A disabled window, such as the owner of a modal dialog, takes no commands: its menus show
           their items disabled until it is enabled again. */
        if (window.disabled)
        {
            for (NSMenuItem* item in [menu itemArray])
                [item setEnabled:NO];
            return;
        }

        begin = (loopWindow != window);
        if (begin)
        {
            [self endMenuLoop];
            loopWindow = [window retain];
        }

        query = macdrv_create_query();
        query->type = QUERY_MENU_POPUP;
        query->window = [window retain];
        query->menu_popup.menu = wineMenu.wineHandle;
        query->menu_popup.parent = wineMenu.wineParent;
        query->menu_popup.position = wineMenu.winePosition;
        query->menu_popup.begin = begin;

        if ([window.queue query:query timeout:WineMenuPopupQueryTimeout] && query->menu_popup.result)
        {
            [self populateMenu:wineMenu fromSnapshot:query->menu_popup.result];
            objc_setAssociatedObject(window, &WineMenuHashKey, [NSNull null], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        else if (!query->done)
        {
            WARN("window %p did not initialize menu %p in time\n", window, wineMenu.wineHandle);
            loopUnresponsive = YES;
        }

        macdrv_release_query(query);
    }

    - (void) menuDidClose:(NSMenu*)menu
    {
        if ([menu isKindOfClass:[WineMenu class]] && [(WineMenu*)menu wineHandle] && loopWindow)
            [self postLoopEvent:MENU_LOOP_POPUP_CLOSED menu:[(WineMenu*)menu wineHandle] window:loopWindow];
    }

    - (BOOL) menuHasKeyEquivalent:(NSMenu*)menu forEvent:(NSEvent*)event target:(id*)target action:(SEL*)action
    {
        /* -handleKeyEquivalent: has already looked; don't have AppKit fill the popups to look again. */
        return NO;
    }


    /*
     * ---------- Notifications ----------
     */
    - (void) windowDidBecomeKey:(NSNotification*)note
    {
        [self updateForWindow:[note object]];
    }

    - (void) windowDidResignKey:(NSNotification*)note
    {
        if ([note object] == menuWindow)
            [self performSelector:@selector(updateForKeyWindow) withObject:nil afterDelay:0];
    }

    - (void) windowWillClose:(NSNotification*)note
    {
        NSWindow* window = [note object];

        if (![window isKindOfClass:[WineWindow class]] || [(WineWindow*)window isFakingClose])
            return;

        if (window == menuWindow)
            [self uninstallItems];
        if (window == pendingWindow)
        {
            [pendingWindow release];
            pendingWindow = nil;
        }
        objc_setAssociatedObject(window, &WineMenuItemsKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(window, &WineMenuHashKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }

    - (void) menuDidBeginTracking:(NSNotification*)note
    {
        if ([note object] != [NSApp mainMenu])
            return;

        tracking = YES;
        loopUnresponsive = NO;
    }

    - (void) menuDidEndTracking:(NSNotification*)note
    {
        if ([note object] != [NSApp mainMenu])
            return;

        tracking = NO;
        loopUnresponsive = NO;
        [self endMenuLoop];

        if (pendingWindow)
        {
            WineWindow* window = [pendingWindow autorelease];
            NSArray* items = [pendingItems autorelease];
            NSNumber* hash = [pendingHash autorelease];

            pendingWindow = nil;
            pendingItems = nil;
            pendingHash = nil;
            [self applyItems:items hash:hash toWindow:window];
        }

        [self updateForKeyWindow];
    }

@end


/***********************************************************************
 *              macdrv_set_cocoa_window_menu
 *
 * Give the window the snapshot of its menu bar, which the function takes ownership of. NULL
 * takes the window's menu bar away.
 */
void macdrv_set_cocoa_window_menu(WineWindow *window, struct macdrv_menu *menu)
{
    OnMainThreadAsync(^{
        [[WineMenuBarController sharedController] setMenu:menu forWindow:window];
        macdrv_free_menu(menu);
    });
}
