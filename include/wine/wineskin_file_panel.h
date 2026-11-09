/*
 * WINESKIN: the native file panel winemac.drv offers the common dialogs
 *
 * winemac.drv exports wineskin_file_panel(), which shows an NSOpenPanel or NSSavePanel for the
 * calling thread and returns once the user has dismissed it. comdlg32 and shell32 find it with
 * GetProcAddress() on the loaded display driver, and show their own dialogs when it is absent or
 * fails.
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

#ifndef __WINE_WINESKIN_FILE_PANEL_H
#define __WINE_WINESKIN_FILE_PANEL_H

#define WINESKIN_FILE_PANEL_SAVE            0x0001  /* NSSavePanel; otherwise NSOpenPanel */
#define WINESKIN_FILE_PANEL_MULTISELECT     0x0002  /* the user may choose several items */
#define WINESKIN_FILE_PANEL_FOLDERS         0x0004  /* the user chooses folders, not files */
#define WINESKIN_FILE_PANEL_SHOW_HIDDEN     0x0008  /* show hidden files */

struct wineskin_file_panel_filter
{
    const WCHAR *name;          /* shown in the panel's file type popup */
    const WCHAR *spec;          /* patterns separated by semicolons, such as "*.txt;*.text" */
};

struct wineskin_file_panel
{
    HWND owner;                 /* the panel is a sheet on this window's top-level window when it can be */
    UINT flags;                 /* WINESKIN_FILE_PANEL_* */
    const WCHAR *title;         /* or NULL for the Mac default */
    const WCHAR *prompt;        /* the default button's label, or NULL for the Mac default */
    const WCHAR *directory;     /* the initial directory as a Windows path, or NULL */
    const WCHAR *file_name;     /* the save panel's initial name, or NULL */
    const struct wineskin_file_panel_filter *filters;
    UINT filter_count;
    UINT filter_index;          /* in: the initially selected filter; out: the filter selected at the end */
    WCHAR *results;             /* out: the chosen items' Windows paths, each null-terminated, then a
                                   null; free with HeapFree(GetProcessHeap(), ...) */
};

/* Returns S_OK when the user chose, S_FALSE when the user canceled, and a failure when no native
   panel can be shown. */
typedef HRESULT (CDECL *wineskin_file_panel_func)(struct wineskin_file_panel *panel);

#endif /* __WINE_WINESKIN_FILE_PANEL_H */
