/*
 * WINESKIN: the Mac's own alert icons for Windows' stock icons
 *
 * winemac.drv exports wineskin_system_icon(), which draws the OS X icon that stands for one of
 * Windows' stock icons (IDI_HAND, IDI_EXCLAMATION, IDI_ASTERISK, IDI_QUESTION) at a size in pixels,
 * from the system's own artwork at run time. user32 finds it with GetProcAddress() on the loaded
 * display driver and loads Windows' icons when it is absent or fails.
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

#ifndef __WINE_WINESKIN_SYSTEM_ICON_H
#define __WINE_WINESKIN_SYSTEM_ICON_H

/* Draws the icon for the stock icon id (an IDI_* value) into bits: width * height 32-bit pixels,
 * top-down, BGRA with straight (not premultiplied) alpha. Returns FALSE for ids it has no icon for. */
typedef BOOL (CDECL *wineskin_system_icon_func)(UINT id, UINT width, UINT height, DWORD *bits);

#endif /* __WINE_WINESKIN_SYSTEM_ICON_H */
