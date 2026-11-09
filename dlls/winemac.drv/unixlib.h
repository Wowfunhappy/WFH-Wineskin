/*
 * Copyright 2022 Jacek Caban for CodeWeavers
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

#include "ntuser.h"
#include "wine/unixlib.h"

enum macdrv_funcs
{
    unix_init,
    unix_quit_result,
    unix_file_panel_begin,      /* WINESKIN */
    unix_file_panel_poll,       /* WINESKIN */
    unix_file_panel_release,    /* WINESKIN */
    unix_funcs_count
};

#define MACDRV_CALL(func, params) WINE_UNIX_CALL(unix_ ## func, params)

/* macdrv_init params */
struct localized_string
{
    UINT id;
    UINT len;
    UINT64 str;
};

struct init_params
{
    struct localized_string *strings;
    UINT64 app_icon_callback;
    UINT64 app_quit_request_callback;
};

/* macdrv_quit_result params */
struct quit_result_params
{
    int result;
};

/* WINESKIN: macdrv_file_panel_begin params */
struct file_panel_begin_params
{
    UINT64 title;           /* const WCHAR *, or 0 */
    UINT64 prompt;          /* const WCHAR *, or 0 */
    UINT64 directory;       /* const char *, a Unix path, or 0 */
    UINT64 file_name;       /* const WCHAR *, or 0 */
    UINT64 filters;         /* const WCHAR *: each filter's name and spec, each null-terminated */
    UINT32 owner;           /* HWND */
    UINT32 flags;           /* WINESKIN_FILE_PANEL_* */
    UINT32 filter_count;
    UINT32 filter_index;
    UINT64 panel;           /* out: the panel, for macdrv_file_panel_poll and _release */
};

/* WINESKIN: macdrv_file_panel_poll and macdrv_file_panel_release params */
struct file_panel_poll_params
{
    UINT64 panel;
    UINT64 buffer;          /* char *, or 0: receives the chosen Unix paths, each null-terminated,
                               then a null, once the panel is done */
    UINT32 size;            /* of the buffer */
    UINT32 done;            /* out: the user dismissed the panel */
    UINT32 chosen;          /* out: the user chose */
    UINT32 needed;          /* out: the size the paths need */
    UINT32 filter_index;    /* out: the filter selected at the end */
    UINT32 copied;          /* out: the paths are in the buffer */
};

/* macdrv_app_icon result */
struct app_icon_entry
{
    UINT32 width;
    UINT32 height;
    UINT32 icon;
};

/* macdrv_app_quit_request params */
struct app_quit_request_params
{
    struct dispatch_callback_params dispatch;
    UINT flags;
};

/* macdrv_dnd_query_exited params */
struct dnd_query_exited_params
{
    struct dispatch_callback_params dispatch;
    UINT32 hwnd;
};

static inline void *param_ptr(UINT64 param)
{
    return (void *)(UINT_PTR)param;
}
