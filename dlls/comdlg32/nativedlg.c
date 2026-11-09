/*
 * WINESKIN: GetOpenFileName and GetSaveFileName through the Mac driver's native file panel
 *
 * When the display driver offers wineskin_file_panel() (include/wine/wineskin_file_panel.h), an
 * Explorer-style or old-style file dialog without hooks or templates shows an NSOpenPanel or
 * NSSavePanel and fills in the OPENFILENAME as the Win32 dialog would. Otherwise the Win32
 * dialog shows.
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "commdlg.h"
#include "cderr.h"
#include "shlobj.h"
#include "shlwapi.h"
#include "cdlg.h"
#include "filedlgbrowser.h"

#include "wine/debug.h"
#include "wine/wineskin_file_panel.h"

WINE_DEFAULT_DEBUG_CHANNEL(commdlg);


/***********************************************************************
 *      COMDLG32_ShowFilePanel
 *
 * Show the display driver's native file panel. Returns E_NOTIMPL when there is none.
 */
HRESULT COMDLG32_ShowFilePanel(struct wineskin_file_panel *panel)
{
    wineskin_file_panel_func func = NULL;
    HMODULE module;

    if (!(module = GetModuleHandleW(L"winemac.drv")))
    {
        /* A display DC loads the display driver. */
        ReleaseDC(0, GetDC(0));
        module = GetModuleHandleW(L"winemac.drv");
    }
    if (module) func = (wineskin_file_panel_func)GetProcAddress(module, "wineskin_file_panel");
    if (!func) return E_NOTIMPL;

    return func(panel);
}

/***********************************************************************
 *      COMDLG32_FirstExtFromSpec
 *
 * The extension, without its dot, of the first pattern of a filter spec, such as "txt" for
 * "*.txt;*.text", or NULL when that pattern names no single extension.
 */
WCHAR *COMDLG32_FirstExtFromSpec(const WCHAR *spec)
{
    const WCHAR *end, *dot;
    WCHAR *ret;

    if (!spec) return NULL;
    if (!(end = wcschr(spec, ';'))) end = spec + lstrlenW(spec);
    for (dot = end; dot > spec && dot[-1] != '.'; dot--) ;
    if (dot == spec || dot == end) return NULL;
    if (wcspbrk(dot, L"*?") && wcspbrk(dot, L"*?") < end) return NULL;

    ret = malloc((end - dot + 1) * sizeof(WCHAR));
    memcpy(ret, dot, (end - dot) * sizeof(WCHAR));
    ret[end - dot] = 0;
    return ret;
}

static WCHAR *dup_string(const void *str, BOOL unicode)
{
    WCHAR *ret;
    int len;

    if (!str) return NULL;
    if (unicode)
    {
        len = lstrlenW(str) + 1;
        ret = malloc(len * sizeof(WCHAR));
        memcpy(ret, str, len * sizeof(WCHAR));
    }
    else
    {
        len = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
        ret = malloc(len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, str, -1, ret, len);
    }
    return ret;
}

/* A list of strings, each null-terminated, ending at an empty one, such as lpstrFilter. */
static WCHAR *dup_string_list(const void *str, BOOL unicode)
{
    WCHAR *ret;
    int len;

    if (!str) return NULL;
    if (unicode)
    {
        const WCHAR *p = str;
        while (*p) p += lstrlenW(p) + 1;
        len = p - (const WCHAR *)str + 1;
        ret = malloc(len * sizeof(WCHAR));
        memcpy(ret, str, len * sizeof(WCHAR));
    }
    else
    {
        const char *p = str;
        int n;
        while (*p) p += strlen(p) + 1;
        n = p - (const char *)str + 1;
        len = MultiByteToWideChar(CP_ACP, 0, str, n, NULL, 0);
        ret = malloc(len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, str, n, ret, len);
    }
    return ret;
}

static WCHAR *full_path_dir(const WCHAR *path, WCHAR **name_ret)
{
    WCHAR buffer[MAX_PATH], *name = NULL, *ret;

    if (!GetFullPathNameW(path, MAX_PATH, buffer, &name)) return NULL;
    if (name_ret) *name_ret = name ? wcsdup(name) : NULL;
    if (name) *name = 0;
    ret = wcsdup(buffer);
    return ret;
}

/***********************************************************************
 *      initial_dir
 *
 * The directory the dialog starts in, chosen as the Win32 dialog chooses it. A path in the
 * initial file name moves to the directory, leaving the name.
 */
static WCHAR *initial_dir(const WCHAR *init_dir, WCHAR **name, const WCHAR *filters)
{
    WCHAR *dir = NULL, buffer[MAX_PATH];

    /* 1. A path in the file name */
    if (*name && **name && wcschr(*name, '\\'))
    {
        WCHAR *file_name = NULL;
        if ((dir = full_path_dir(*name, &file_name)))
        {
            free(*name);
            *name = file_name;
            return dir;
        }
    }

    /* 2. The initial directory, or the directory of a file it names */
    if (init_dir && *init_dir)
    {
        DWORD len = ExpandEnvironmentStringsW(init_dir, NULL, 0);
        WCHAR *expanded = malloc(len * sizeof(WCHAR));

        ExpandEnvironmentStringsW(init_dir, expanded, len);
        if (PathFileExistsW(expanded))
        {
            if (PathIsDirectoryW(expanded))
            {
                if (GetFullPathNameW(expanded, MAX_PATH, buffer, NULL)) dir = wcsdup(buffer);
            }
            else dir = full_path_dir(expanded, NULL);
        }
        free(expanded);
        if (dir) return dir;
    }

    /* 3. The directory last used by this program */
    buffer[0] = 0;
    COMDLG32_MRU_load_filename(buffer);
    if (buffer[0] && PathFileExistsW(buffer)) return wcsdup(buffer);

    /* 4. The current directory, if it holds files of the filters' types */
    if (filters)
    {
        const WCHAR *filter;

        for (filter = filters; *filter; )
        {
            const WCHAR *spec = filter + lstrlenW(filter) + 1;
            WCHAR *specs, *pattern, *next;
            BOOL found = FALSE;

            if (!*spec) break;
            specs = wcsdup(spec);
            for (pattern = specs; pattern && !found; pattern = next)
            {
                WIN32_FIND_DATAW data;
                HANDLE find;

                if ((next = wcschr(pattern, ';'))) *next++ = 0;
                while (*pattern == ' ') pattern++;
                if (!*pattern) continue;
                if ((find = FindFirstFileW(pattern, &data)) != INVALID_HANDLE_VALUE)
                {
                    FindClose(find);
                    found = TRUE;
                }
            }
            free(specs);
            if (found)
            {
                GetCurrentDirectoryW(MAX_PATH, buffer);
                return wcsdup(buffer);
            }
            filter = spec + lstrlenW(spec) + 1;
        }
    }

    /* 5. The documents folder */
    if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, 0, 0, buffer) == S_OK) return wcsdup(buffer);

    GetCurrentDirectoryW(MAX_PATH, buffer);
    return wcsdup(buffer);
}

/* For the old-style multiple selection result: a path or name with a space becomes its short form. */
static WCHAR *short_form(const WCHAR *path, BOOL name_only)
{
    WCHAR buffer[MAX_PATH];

    if (!wcschr(name_only ? PathFindFileNameW(path) : path, ' ') ||
        !GetShortPathNameW(path, buffer, MAX_PATH))
        return wcsdup(name_only ? PathFindFileNameW(path) : path);
    return wcsdup(name_only ? PathFindFileNameW(buffer) : buffer);
}

/* The directory a path is in, as the Win32 dialog gives it: with a backslash only for a root. */
static int dir_length(const WCHAR *path)
{
    const WCHAR *name = PathFindFileNameW(path);
    int len = name - path;

    if (len > 0 && path[len - 1] == '\\') len--;
    if (len == 2 && path[1] == ':') len++;
    return len;
}

/* Whether a path is in the directory the first len characters of dir name. */
static BOOL in_dir(const WCHAR *path, const WCHAR *dir, int len)
{
    if (lstrlenW(path) <= len) return FALSE;
    if (CompareStringOrdinal(path, len, dir, len, TRUE) != CSTR_EQUAL) return FALSE;
    return path[len] == '\\' || dir[len - 1] == '\\';
}

/***********************************************************************
 *      build_result
 *
 * The lpstrFile contents for the chosen paths, with its length in characters, terminators
 * included, and the length of its directory part.
 */
static WCHAR *build_result(const WCHAR *results, DWORD flags, int *len_ret, int *dir_len_ret)
{
    const WCHAR *path, *first = results;
    BOOL multi = (flags & OFN_ALLOWMULTISELECT) != 0;
    BOOL old_style = multi && !(flags & OFN_EXPLORER);
    int count = 0, dir_len, len;
    WCHAR *ret, *p;

    for (path = results; *path; path += lstrlenW(path) + 1) count++;

    if (count == 1)
    {
        WCHAR *full = old_style ? short_form(first, FALSE) : wcsdup(first);

        len = lstrlenW(full) + 1 + (multi ? 1 : 0);
        ret = calloc(len, sizeof(WCHAR));
        lstrcpyW(ret, full);
        *len_ret = len;
        *dir_len_ret = -1;
        free(full);
        return ret;
    }

    /* The directory that holds every chosen item; search results can come from several. */
    dir_len = dir_length(first);
    for (path = first; *path; path += lstrlenW(path) + 1)
    {
        while (dir_len > 0 && !in_dir(path, first, dir_len))
        {
            if (dir_len <= 3 && first[1] == ':')
            {
                dir_len = 0;
                break;
            }
            while (--dir_len > 0 && first[dir_len] != '\\') ;
            if (dir_len == 2 && first[1] == ':') dir_len = 3;
        }
    }

    {
        WCHAR *dir = malloc((dir_len + 1) * sizeof(WCHAR)), *short_dir;

        memcpy(dir, first, dir_len * sizeof(WCHAR));
        dir[dir_len] = 0;
        short_dir = old_style ? short_form(dir, FALSE) : wcsdup(dir);
        free(dir);

        len = lstrlenW(short_dir) + 2;
        for (path = first; *path; path += lstrlenW(path) + 1)
            len += lstrlenW(path) + 1;
        ret = calloc(len, sizeof(WCHAR));
        lstrcpyW(ret, short_dir);
        *dir_len_ret = lstrlenW(short_dir);
        p = ret + lstrlenW(ret);
        free(short_dir);
    }

    for (path = first; *path; path += lstrlenW(path) + 1)
    {
        const WCHAR *rel = path + dir_len;
        WCHAR *name;

        while (*rel == '\\') rel++;
        name = (old_style && !wcschr(rel, '\\')) ? short_form(path, TRUE) : wcsdup(rel);
        *p++ = old_style ? ' ' : 0;
        lstrcpyW(p, name);
        p += lstrlenW(p);
        free(name);
    }
    *p++ = 0;
    if (!old_style) *p++ = 0;
    *len_ret = p - ret;
    return ret;
}

/***********************************************************************
 *      fill_ofn
 *
 * Fill the OPENFILENAME with the chosen paths. Returns FALSE when lpstrFile is too small.
 */
static BOOL fill_ofn(OPENFILENAMEW *ofn, BOOL unicode, const WCHAR *results)
{
    OPENFILENAMEA *ofnA = (OPENFILENAMEA *)ofn;
    int len, dir_len;
    WCHAR *result = build_result(results, ofn->Flags, &len, &dir_len);
    const WCHAR *title = PathFindFileNameW(results);

    if (unicode)
    {
        if (len > ofn->nMaxFile)
        {
            if (ofn->nMaxFile >= 1) *(WORD *)ofn->lpstrFile = len;
            free(result);
            return FALSE;
        }
        memcpy(ofn->lpstrFile, result, len * sizeof(WCHAR));
        if (dir_len < 0)
        {
            WCHAR *ext = PathFindExtensionW(ofn->lpstrFile);
            ofn->nFileOffset = PathFindFileNameW(ofn->lpstrFile) - ofn->lpstrFile;
            ofn->nFileExtension = *ext ? ext - ofn->lpstrFile + 1 : 0;
        }
        else
        {
            ofn->nFileOffset = dir_len + 1;
            ofn->nFileExtension = 0;
        }
        if (ofn->lpstrFileTitle && ofn->nMaxFileTitle && dir_len < 0)
            lstrcpynW(ofn->lpstrFileTitle, title, ofn->nMaxFileTitle);
    }
    else
    {
        int alen = WideCharToMultiByte(CP_ACP, 0, result, len, NULL, 0, NULL, NULL);

        if (alen > ofnA->nMaxFile)
        {
            if (ofnA->nMaxFile >= 2) *(WORD *)ofnA->lpstrFile = alen;
            free(result);
            return FALSE;
        }
        WideCharToMultiByte(CP_ACP, 0, result, len, ofnA->lpstrFile, ofnA->nMaxFile, NULL, NULL);
        if (dir_len < 0)
        {
            char *ext = PathFindExtensionA(ofnA->lpstrFile);
            ofnA->nFileOffset = PathFindFileNameA(ofnA->lpstrFile) - ofnA->lpstrFile;
            ofnA->nFileExtension = *ext ? ext - ofnA->lpstrFile + 1 : 0;
        }
        else
        {
            ofnA->nFileOffset = WideCharToMultiByte(CP_ACP, 0, result, dir_len, NULL, 0, NULL, NULL) + 1;
            ofnA->nFileExtension = 0;
        }
        if (ofnA->lpstrFileTitle && ofnA->nMaxFileTitle && dir_len < 0)
        {
            if (!WideCharToMultiByte(CP_ACP, 0, title, -1, ofnA->lpstrFileTitle, ofnA->nMaxFileTitle, NULL, NULL))
                ofnA->lpstrFileTitle[ofnA->nMaxFileTitle - 1] = 0;
        }
    }

    free(result);
    return TRUE;
}

/***********************************************************************
 *      COMDLG32_GetNativeFileName
 *
 * Show the native file panel for GetOpenFileName or GetSaveFileName. Returns FALSE when the
 * Win32 dialog has to show instead; otherwise *ret is the function's result.
 */
BOOL COMDLG32_GetNativeFileName(OPENFILENAMEW *ofn, BOOL unicode, BOOL save, BOOL *ret)
{
    OPENFILENAMEA *ofnA = (OPENFILENAMEA *)ofn;
    struct wineskin_file_panel panel = { 0 };
    struct wineskin_file_panel_filter *filters = NULL;
    WCHAR *filter_str, *custom_str, *title, *defext, *init_dir, *name = NULL, *dir;
    WCHAR *chosen = NULL, *wildcard = NULL;
    const WCHAR *p;
    UINT count = 0, custom = 0, index;
    BOOL handled = FALSE;
    HRESULT hr;

    if (ofn->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) return FALSE;
    if (!ofn->lpstrFile) return FALSE;

    filter_str = dup_string_list(ofn->lpstrFilter, unicode);
    custom_str = dup_string_list(ofn->lpstrCustomFilter, unicode);
    title = dup_string(ofn->lpstrTitle, unicode);
    defext = dup_string(ofn->lpstrDefExt, unicode);
    init_dir = dup_string(ofn->lpstrInitialDir, unicode);

    if (unicode)
    {
        name = calloc(ofn->nMaxFile + 1, sizeof(WCHAR));
        if (ofn->nMaxFile) lstrcpynW(name, ofn->lpstrFile, ofn->nMaxFile);
    }
    else
    {
        int n = 0, len;
        while (n < ofnA->nMaxFile && ofnA->lpstrFile[n]) n++;
        len = MultiByteToWideChar(CP_ACP, 0, ofnA->lpstrFile, n, NULL, 0);
        name = calloc(len + 1, sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, ofnA->lpstrFile, n, name, len);
    }

    /* The custom filter comes first, as filter 0. */
    for (p = filter_str; p && *p; p += lstrlenW(p) + 1) count++;
    count /= 2;
    if (custom_str && *custom_str && custom_str[lstrlenW(custom_str) + 1]) custom = 1;
    filters = calloc(count + custom + 1, sizeof(*filters));
    if (custom)
    {
        filters[0].name = custom_str;
        filters[0].spec = custom_str + lstrlenW(custom_str) + 1;
    }
    for (index = 0, p = filter_str; index < count; index++)
    {
        filters[custom + index].name = p;
        p += lstrlenW(p) + 1;
        filters[custom + index].spec = p;
        p += lstrlenW(p) + 1;
    }
    count += custom;

    dir = initial_dir(init_dir, &name, filter_str);

    /* A file name with wildcards is a filter, as the Win32 dialog takes it. */
    if (name && wcspbrk(name, L"*?"))
    {
        if (!count)
        {
            wildcard = wcsdup(name);
            filters[0].name = filters[0].spec = wildcard;
            count = 1;
        }
        name[0] = 0;
    }

    if (custom) index = ofn->nFilterIndex;
    else index = ofn->nFilterIndex ? ofn->nFilterIndex - 1 : 0;
    if (index >= count) index = 0;

    COMDLG32_SetCommDlgExtendedError(0);

    panel.owner = ofn->hwndOwner;
    panel.flags = save ? WINESKIN_FILE_PANEL_SAVE : 0;
    if (!save && (ofn->Flags & OFN_ALLOWMULTISELECT)) panel.flags |= WINESKIN_FILE_PANEL_MULTISELECT;
    panel.title = title;
    panel.directory = dir;
    panel.file_name = name;
    panel.filters = filters;
    panel.filter_count = count;
    panel.filter_index = index;

    for (;;)
    {
        WCHAR *ext;

        hr = COMDLG32_ShowFilePanel(&panel);
        TRACE("panel returned 0x%08lx\n", hr);
        if (hr != S_OK) break;

        chosen = malloc((lstrlenW(panel.results) + MAX_PATH + 2) * sizeof(WCHAR));
        lstrcpyW(chosen, panel.results);
        chosen[lstrlenW(chosen) + 1] = 0;

        /* A save without an extension takes the filter's, or else the default, as the Win32
           dialog does; the panel adds the filter's extension itself when it knows it. */
        ext = PathFindExtensionW(chosen);
        if (save && !*ext && defext)
        {
            WCHAR *filter_ext = count ? COMDLG32_FirstExtFromSpec(filters[panel.filter_index].spec) : NULL;
            const WCHAR *new_ext = filter_ext ? filter_ext : defext;

            if (*new_ext && lstrlenW(chosen) + lstrlenW(new_ext) + 1 < lstrlenW(panel.results) + MAX_PATH)
            {
                lstrcatW(chosen, L".");
                lstrcatW(chosen, new_ext);
                chosen[lstrlenW(chosen) + 1] = 0;

                if ((ofn->Flags & OFN_OVERWRITEPROMPT) && PathFileExistsW(chosen))
                {
                    WCHAR text[100];
                    int answer;

                    LoadStringW(COMDLG32_hInstance, IDS_OVERWRITEFILE, text, ARRAY_SIZE(text));
                    answer = MessageBoxW(ofn->hwndOwner, text, title, MB_YESNO | MB_ICONEXCLAMATION);
                    if (answer != IDYES)
                    {
                        /* Show the panel again where the user was. */
                        WCHAR *name_part = PathFindFileNameW(panel.results);

                        free(dir);
                        dir = malloc((name_part - panel.results + 1) * sizeof(WCHAR));
                        memcpy(dir, panel.results, (name_part - panel.results) * sizeof(WCHAR));
                        dir[name_part - panel.results] = 0;
                        free(name);
                        name = wcsdup(name_part);
                        panel.directory = dir;
                        panel.file_name = name;
                        HeapFree(GetProcessHeap(), 0, panel.results);
                        panel.results = NULL;
                        free(chosen);
                        chosen = NULL;
                        free(filter_ext);
                        continue;
                    }
                }
            }
            free(filter_ext);
        }

        /* An open panel's paths stay as they are. */
        if (!save)
        {
            free(chosen);
            p = panel.results;
            while (*p) p += lstrlenW(p) + 1;
            chosen = malloc((p - panel.results + 1) * sizeof(WCHAR));
            memcpy(chosen, panel.results, (p - panel.results + 1) * sizeof(WCHAR));
        }
        break;
    }

    if (hr == S_FALSE)
    {
        handled = TRUE;
        *ret = FALSE;
    }
    else if (hr == S_OK)
    {
        handled = TRUE;
        if (!fill_ofn(ofn, unicode, chosen))
        {
            COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
            *ret = FALSE;
        }
        else
        {
            BOOL single = !chosen[lstrlenW(chosen) + 1];

            ofn->nFilterIndex = custom ? panel.filter_index : panel.filter_index + 1;

            if (defext && single)
            {
                const WCHAR *ext = PathFindExtensionW(chosen);
                if (*ext) ext++;
                if (!lstrcmpiW(defext, ext)) ofn->Flags &= ~OFN_EXTENSIONDIFFERENT;
                else ofn->Flags |= OFN_EXTENSIONDIFFERENT;
            }

            if (single) COMDLG32_MRU_save_filename(chosen);

            if (!(ofn->Flags & OFN_NOCHANGEDIR))
            {
                WCHAR *cur = wcsdup(chosen);
                PathRemoveFileSpecW(cur);
                SetCurrentDirectoryW(cur);
                free(cur);
            }
            *ret = TRUE;
        }
    }

    HeapFree(GetProcessHeap(), 0, panel.results);
    free(chosen);
    free(filters);
    free(filter_str);
    free(custom_str);
    free(title);
    free(defext);
    free(init_dir);
    free(name);
    free(dir);
    free(wildcard);
    return handled;
}
