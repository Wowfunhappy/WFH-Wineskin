/*
 * MACDRV Cocoa menu bar declarations
 *
 * WINESKIN: The Mac menu bar shows the Win32 menu bar of the key window.
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

#import <AppKit/AppKit.h>

#include "macdrv_cocoa.h"


@interface WineMenuBarController : NSObject <NSMenuDelegate>
{
    WineWindow* menuWindow;     /* the window whose menus are in the menu bar */
    NSArray* installedItems;    /* those menus' items of the main menu */

    BOOL tracking;              /* the user is in the menu bar */
    WineWindow* loopWindow;     /* the window that got WM_ENTERMENULOOP in this tracking session */
    BOOL loopUnresponsive;      /* a popup query timed out in this tracking session */

    WineWindow* pendingWindow;  /* a window whose menu bar changed during tracking */
    NSArray* pendingItems;      /* its new items and their snapshot's hash */
    NSNumber* pendingHash;
}

    + (WineMenuBarController*) sharedController;

    /* The main menu was replaced; put the key window's menus into the new one. */
    - (void) mainMenuDidChange;

    /* Returns YES if the key-down event chose an item of the window's menu bar. */
    - (BOOL) handleKeyEquivalent:(NSEvent*)event;

@end
