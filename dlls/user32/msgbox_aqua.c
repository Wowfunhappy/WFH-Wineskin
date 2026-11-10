/*
 * WINESKIN: message boxes laid out as OS X 10.9 alerts
 *
 * MessageBox is still a Win32 dialog (programs hook message boxes, read their controls and
 * subclass them); MSGBOX_OnInit() sets it up as upstream does, and this lays it out again the
 * way NSAlert lays out an alert on 10.9:
 *
 * - a 64-pixel icon at the left: the alert type's icon, a MessageBoxIndirect custom icon, or the
 *   program's own icon when the message box has none;
 * - the text to its right, the first paragraph in bold 13-point type (NSAlert's message text) and
 *   the rest, if the text has a paragraph or line break, in regular 11-point type (its
 *   informative text). The static control keeps the whole text and draws it (SS_OWNERDRAW);
 * - the buttons at the bottom right, the default button rightmost and Cancel (or No) to its left;
 *   any others start at the left, under the text, as NSAlert places a third button;
 * - no title in the title bar (the window text stays the caption) and no close button.
 *
 * Sizes are NSAlert's on 10.9 in points, measured from native alerts, at the window's DPI.
 *
 * Copyright 2026 WFH Wineskin
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
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "dlgs.h"
#include "user_private.h"
#include "resources.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msgbox);

/* NSAlert's geometry on 10.9, in points (pixels at 96 DPI). */
#define ALERT_MIN_WIDTH     420
#define ICON_LEFT           24
#define ICON_TOP            15
#define ICON_SIZE           64
#define TEXT_LEFT           105
#define TEXT_RIGHT          20     /* from the right edge */
#define TEXT_TOP            15
#define MESSAGE_FONT        13     /* bold */
#define INFO_FONT           11
#define INFO_GAP            7      /* between the message text and the informative text */
#define NO_INFO_SPACE       15     /* NSAlert keeps this much when there is no informative text */
#define TEXT_BUTTON_GAP     17     /* from the text to the buttons */
#define ICON_BUTTON_GAP     11     /* from the icon to the buttons */
#define BUTTON_HEIGHT       23     /* Aqua's push button: a 21-point bezel and its shadow */
#define BUTTON_MIN_WIDTH    81     /* the bezel is the control less a pixel each side */
#define BUTTON_PADDING      40     /* bezel width beyond the title's */
#define BUTTON_GAP          12
#define BUTTON_RIGHT        19     /* from the right edge */
#define BUTTON_BOTTOM       18     /* from the bottom edge */
#define BUTTON_LEFT         103    /* buttons NSAlert puts at the left */
#define GROUP_GAP           24     /* at least, between the left and right buttons */

struct aqua_msgbox
{
    HFONT message_font;
    HFONT info_font;
    HICON icon;
    int split;          /* where the informative text starts, or -1 */
    int info_start;     /* after the break */
    int message_height;
    int info_gap;
};

static const WCHAR aqua_prop[] = L"WINE_MSGBOX_AQUA";

static int scale( int value, UINT dpi )
{
    return MulDiv( value, dpi, 96 );
}

/* The first paragraph is the message text; without a blank line, the first line. */
static void split_text( const WCHAR *text, int *split, int *info_start )
{
    const WCHAR *p, *nl;

    *split = *info_start = -1;
    for (p = text; *p; p++)
    {
        if (*p != '\n') continue;
        nl = p + 1;
        if (*nl == '\r') nl++;
        if (*nl == '\n')
        {
            *split = (p > text && p[-1] == '\r') ? p - 1 - text : p - text;
            for (nl++; *nl == '\r' || *nl == '\n'; nl++) ;
            *info_start = *nl ? nl - text : -1;
            if (*info_start == -1) *split = -1;
            return;
        }
    }
    if ((p = wcschr( text, '\n' )))
    {
        *split = (p > text && p[-1] == '\r') ? p - 1 - text : p - text;
        for (nl = p + 1; *nl == '\r' || *nl == '\n'; nl++) ;
        *info_start = *nl ? nl - text : -1;
        if (*info_start == -1 || !*split) *split = *info_start = -1;
    }
}

static HFONT create_font( HWND hwnd, int points, LONG weight, UINT dpi )
{
    LOGFONTW lf;
    HFONT font = (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 );

    if (!font || !GetObjectW( font, sizeof(lf), &lf ))
    {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, 0, &ncm, 0 );
        lf = ncm.lfMessageFont;
    }
    lf.lfHeight = -scale( points, dpi );
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    return CreateFontIndirectW( &lf );
}

/* The program's own icon: the first icon group of its executable. */
static BOOL CALLBACK first_icon_proc( HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param )
{
    LPWSTR *out = (LPWSTR *)param;

    *out = IS_INTRESOURCE(name) ? name : wcsdup( name );
    return FALSE;
}

#include "pshpack2.h"
struct group_icon_entry
{
    BYTE  width, height, colors, reserved;
    WORD  planes, bit_count;
    DWORD bytes;
    WORD  id;
};
struct group_icon_dir
{
    WORD reserved, type, count;
    struct group_icon_entry entries[1];
};
#include "poppack.h"

/* A program's icon at its own size, the one closest to size but never scaled up beyond what the
 * program has: *actual is the size loaded. */
static HICON load_own_icon( HINSTANCE inst, LPCWSTR name, int size, int *actual )
{
    const struct group_icon_dir *dir;
    HRSRC rsrc;
    int id, i, best = size;

    if ((rsrc = FindResourceW( inst, name, (LPCWSTR)RT_GROUP_ICON )) &&
        (dir = LockResource( LoadResource( inst, rsrc ) )) &&
        (id = LookupIconIdFromDirectoryEx( (BYTE *)dir, TRUE, size, size, LR_DEFAULTCOLOR )))
    {
        for (i = 0; i < dir->count; i++)
            if (dir->entries[i].id == id)
                best = dir->entries[i].width ? dir->entries[i].width : 256;
    }
    *actual = min( best, size );
    return LoadImageW( inst, name, IMAGE_ICON, *actual, *actual, 0 );
}

static HICON load_icon( const MSGBOXPARAMSW *mb, UINT dpi, int *actual )
{
    int size = scale( ICON_SIZE, dpi );
    LPCWSTR name = NULL;
    HINSTANCE inst;
    LPWSTR first = NULL;
    HICON icon;

    /* The alert types' icons are the Mac's own at any size (user32's create_mac_system_icon). */
    *actual = size;
    switch (mb->dwStyle & MB_ICONMASK)
    {
    case MB_ICONEXCLAMATION: name = (LPCWSTR)IDI_EXCLAMATION; break;
    case MB_ICONQUESTION:    name = (LPCWSTR)IDI_QUESTION; break;
    case MB_ICONASTERISK:    name = (LPCWSTR)IDI_ASTERISK; break;
    case MB_ICONHAND:        name = (LPCWSTR)IDI_HAND; break;
    case MB_USERICON:
        if ((icon = load_own_icon( mb->hInstance, mb->lpszIcon, size, actual ))) return icon;
        break;
    }
    if (name && (icon = LoadImageW( 0, name, IMAGE_ICON, size, size, 0 ))) return icon;

    inst = GetModuleHandleW( NULL );
    EnumResourceNamesW( inst, (LPCWSTR)RT_GROUP_ICON, first_icon_proc, (LONG_PTR)&first );
    icon = first ? load_own_icon( inst, first, size, actual ) : 0;
    if (first && !IS_INTRESOURCE(first)) free( first );
    if (!icon) icon = load_own_icon( user32_module, (LPCWSTR)IDI_APPLICATION, size, actual );
    return icon;
}

static int button_width( HWND button, HDC hdc, UINT dpi )
{
    WCHAR text[256];
    RECT rect = { 0 };

    GetWindowTextW( button, text, ARRAY_SIZE(text) );
    DrawTextW( hdc, text, -1, &rect, DT_SINGLELINE | DT_CALCRECT );
    return max( scale( BUTTON_MIN_WIDTH, dpi ), rect.right + scale( BUTTON_PADDING, dpi ) );
}

void MSGBOX_AquaLayout( HWND hwnd, const MSGBOXPARAMSW *mb )
{
    static const int ids[] = { IDOK, IDCANCEL, IDABORT, IDRETRY, IDIGNORE, IDYES, IDNO,
                               IDTRYAGAIN, IDCONTINUE, IDHELP };
    /* Windows' left-to-right order */
    static const int order[] = { IDYES, IDNO, IDOK, IDABORT, IDRETRY, IDCANCEL, IDIGNORE,
                                 IDTRYAGAIN, IDCONTINUE, IDHELP };
    UINT dpi = GetDpiForWindow( hwnd );
    HWND text_ctrl = GetDlgItem( hwnd, MSGBOX_IDTEXT ), icon_ctrl = GetDlgItem( hwnd, MSGBOX_IDICON );
    HWND right[2] = { 0 }, left[ARRAY_SIZE(ids)], def = 0, button;
    int widths[ARRAY_SIZE(ids)], right_w[2] = { 0 }, nleft = 0, nright = 0;
    int i, len, text_w, text_h, info_h = 0, client_w, client_h, buttons_top, x, need, icon_size;
    struct aqua_msgbox *box;
    RECT rect, wrect, crect;
    WCHAR *text;
    HFONT old;
    HDC hdc;
    HMONITOR monitor;
    MONITORINFO mon_info = { sizeof(mon_info) };

    if (!text_ctrl || !(box = calloc( 1, sizeof(*box) ))) return;

    len = GetWindowTextLengthW( text_ctrl );
    if (!(text = malloc( (len + 1) * sizeof(WCHAR) ))) { free( box ); return; }
    GetWindowTextW( text_ctrl, text, len + 1 );
    split_text( text, &box->split, &box->info_start );

    box->message_font = create_font( hwnd, MESSAGE_FONT, FW_BOLD, dpi );
    box->info_font = create_font( hwnd, INFO_FONT, FW_NORMAL, dpi );
    box->icon = load_icon( mb, dpi, &icon_size );
    SetPropW( hwnd, aqua_prop, box );

    /* The buttons: the default rightmost, Cancel or No to its left, the others at the left. */
    for (i = 0; i < ARRAY_SIZE(order); i++)
    {
        if (!(button = GetDlgItem( hwnd, order[i] )) || !(GetWindowLongW( button, GWL_STYLE ) & WS_VISIBLE)) continue;
        if ((GetWindowLongW( button, GWL_STYLE ) & BS_TYPEMASK) == BS_DEFPUSHBUTTON) def = button;
    }
    hdc = NtUserGetDC( hwnd );
    old = SelectObject( hdc, (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 ) );
    if (def) right[nright++] = def;
    for (i = 0; i < 2 && nright < 2; i++)
    {
        static const int cancel_ids[] = { IDCANCEL, IDNO };
        if ((button = GetDlgItem( hwnd, cancel_ids[i] )) && button != def &&
            (GetWindowLongW( button, GWL_STYLE ) & WS_VISIBLE))
            right[nright++] = button;
    }
    for (i = 0; i < ARRAY_SIZE(order); i++)
    {
        if (!(button = GetDlgItem( hwnd, order[i] )) || !(GetWindowLongW( button, GWL_STYLE ) & WS_VISIBLE)) continue;
        if (button == right[0] || button == right[1]) continue;
        if (nright < 2 && order[i] != IDHELP) right[nright++] = button;
        else left[nleft++] = button;
    }
    /* Help goes leftmost, as the Mac's help button does. */
    for (i = 1; i < nleft; i++)
        if (GetDlgCtrlID( left[i] ) == IDHELP)
        {
            HWND help = left[i];
            memmove( left + 1, left, i * sizeof(*left) );
            left[0] = help;
        }
    for (i = 0; i < nright; i++) right_w[i] = button_width( right[i], hdc, dpi );
    for (i = 0; i < nleft; i++) widths[i] = button_width( left[i], hdc, dpi );

    /* The width the buttons need, then the text's height at the text width that leaves. */
    need = scale( BUTTON_RIGHT, dpi );
    for (i = 0; i < nright; i++) need += right_w[i] + (i ? scale( BUTTON_GAP, dpi ) : 0);
    if (nleft)
    {
        need += scale( BUTTON_LEFT, dpi ) + scale( GROUP_GAP, dpi );
        for (i = 0; i < nleft; i++) need += widths[i] + (i ? scale( BUTTON_GAP, dpi ) : 0);
    }
    else need += scale( TEXT_LEFT, dpi );
    client_w = max( scale( ALERT_MIN_WIDTH, dpi ), need );
    text_w = client_w - scale( TEXT_LEFT, dpi ) - scale( TEXT_RIGHT, dpi );

    SelectObject( hdc, box->message_font );
    SetRect( &rect, 0, 0, text_w, 0 );
    DrawTextW( hdc, text, box->split >= 0 ? box->split : -1, &rect,
               DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX | DT_CALCRECT );
    box->message_height = rect.bottom;
    text_h = rect.bottom;
    if (box->split >= 0)
    {
        SelectObject( hdc, box->info_font );
        SetRect( &rect, 0, 0, text_w, 0 );
        DrawTextW( hdc, text + box->info_start, -1, &rect, DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX | DT_CALCRECT );
        info_h = rect.bottom;
        box->info_gap = scale( INFO_GAP, dpi );
        text_h += box->info_gap + info_h;
    }
    else text_h += scale( NO_INFO_SPACE, dpi );
    SelectObject( hdc, old );
    NtUserReleaseDC( hwnd, hdc );
    free( text );

    buttons_top = max( scale( TEXT_TOP, dpi ) + text_h + scale( TEXT_BUTTON_GAP, dpi ),
                       scale( ICON_TOP + ICON_SIZE + ICON_BUTTON_GAP, dpi ) );
    client_h = buttons_top + scale( BUTTON_HEIGHT + BUTTON_BOTTOM, dpi );

    /* No title in the title bar and no close button, as an alert's; the caption stays the window text. */
    SetPropW( hwnd, L"__wine_mac_untitled", (HANDLE)1 );
    SetWindowLongW( hwnd, GWL_STYLE, GetWindowLongW( hwnd, GWL_STYLE ) & ~WS_SYSMENU );
    len = GetWindowTextLengthW( hwnd );
    if ((text = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        GetWindowTextW( hwnd, text, len + 1 );
        SetWindowTextW( hwnd, text );
        free( text );
    }

    GetWindowRect( hwnd, &wrect );
    GetClientRect( hwnd, &crect );
    client_w += (wrect.right - wrect.left) - crect.right;
    client_h += (wrect.bottom - wrect.top) - crect.bottom;
    monitor = MonitorFromWindow( mb->hwndOwner ? mb->hwndOwner : GetActiveWindow(), MONITOR_DEFAULTTOPRIMARY );
    GetMonitorInfoW( monitor, &mon_info );
    /* NSAlert sits a little above the middle. */
    NtUserSetWindowPos( hwnd, 0, (mon_info.rcWork.left + mon_info.rcWork.right - client_w) / 2,
                        mon_info.rcWork.top + (mon_info.rcWork.bottom - mon_info.rcWork.top - client_h) / 3,
                        client_w, client_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_FRAMECHANGED );
    GetClientRect( hwnd, &crect );

    if (icon_ctrl)
    {
        if (box->icon) SendMessageW( icon_ctrl, STM_SETICON, (WPARAM)box->icon, 0 );
        /* an icon smaller than the box sits in its middle */
        NtUserSetWindowPos( icon_ctrl, 0, scale( ICON_LEFT, dpi ) + (scale( ICON_SIZE, dpi ) - icon_size) / 2,
                            scale( ICON_TOP, dpi ) + (scale( ICON_SIZE, dpi ) - icon_size) / 2,
                            icon_size, icon_size, SWP_NOZORDER | SWP_NOACTIVATE );
        NtUserShowWindow( icon_ctrl, box->icon ? SW_SHOWNA : SW_HIDE );
    }

    SetWindowLongW( text_ctrl, GWL_STYLE, (GetWindowLongW( text_ctrl, GWL_STYLE ) & ~SS_TYPEMASK) | SS_OWNERDRAW );
    NtUserSetWindowPos( text_ctrl, 0, scale( TEXT_LEFT, dpi ), scale( TEXT_TOP, dpi ), text_w, text_h,
                        SWP_NOZORDER | SWP_NOACTIVATE );

    x = crect.right - scale( BUTTON_RIGHT, dpi );
    for (i = 0; i < nright; i++)
    {
        x -= right_w[i];
        NtUserSetWindowPos( right[i], 0, x, buttons_top, right_w[i], scale( BUTTON_HEIGHT, dpi ),
                            SWP_NOZORDER | SWP_NOACTIVATE );
        x -= scale( BUTTON_GAP, dpi );
    }
    x = scale( BUTTON_LEFT, dpi );
    for (i = 0; i < nleft; i++)
    {
        NtUserSetWindowPos( left[i], 0, x, buttons_top, widths[i], scale( BUTTON_HEIGHT, dpi ),
                            SWP_NOZORDER | SWP_NOACTIVATE );
        x += widths[i] + scale( BUTTON_GAP, dpi );
    }
}

/* WM_DRAWITEM for the text: the message text in bold, the informative text below it. */
BOOL MSGBOX_AquaDrawItem( HWND hwnd, const DRAWITEMSTRUCT *dis )
{
    struct aqua_msgbox *box = GetPropW( hwnd, aqua_prop );
    UINT flags = DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX;
    RECT rect = dis->rcItem;
    WCHAR *text;
    HFONT old;
    int len;

    if (!box || dis->CtlID != MSGBOX_IDTEXT) return FALSE;
    len = GetWindowTextLengthW( dis->hwndItem );
    if (!(text = malloc( (len + 1) * sizeof(WCHAR) ))) return FALSE;
    GetWindowTextW( dis->hwndItem, text, len + 1 );

    SetBkMode( dis->hDC, TRANSPARENT );
    SetTextColor( dis->hDC, GetSysColor( COLOR_WINDOWTEXT ) );
    old = SelectObject( dis->hDC, box->message_font );
    DrawTextW( dis->hDC, text, box->split >= 0 ? box->split : -1, &rect, flags );
    if (box->split >= 0)
    {
        rect.top += box->message_height + box->info_gap;
        SelectObject( dis->hDC, box->info_font );
        DrawTextW( dis->hDC, text + box->info_start, -1, &rect, flags );
    }
    SelectObject( dis->hDC, old );
    free( text );
    return TRUE;
}

void MSGBOX_AquaDestroy( HWND hwnd )
{
    struct aqua_msgbox *box = RemovePropW( hwnd, aqua_prop );

    if (!box) return;
    DeleteObject( box->message_font );
    DeleteObject( box->info_font );
    if (box->icon) DestroyIcon( box->icon );
    free( box );
}

/* Message boxes run in an activation context that asks for comctl32 6, so their buttons are the
 * themed common control in any program, as Aqua draws them: the default button blue. */
ULONG_PTR MSGBOX_AquaActivate( void )
{
    static HANDLE actctx = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie = 0;

    if (actctx == INVALID_HANDLE_VALUE)
    {
        ACTCTXW ctx = { sizeof(ctx) };
        ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        ctx.hModule = user32_module;
        ctx.lpResourceName = MAKEINTRESOURCEW( MSGBOX_AQUA_MANIFEST );
        actctx = CreateActCtxW( &ctx );
        if (actctx == INVALID_HANDLE_VALUE) actctx = 0;
    }
    if (actctx && !ActivateActCtx( actctx, &cookie )) cookie = 0;
    return cookie;
}

void MSGBOX_AquaDeactivate( ULONG_PTR cookie )
{
    if (cookie) DeactivateActCtx( 0, cookie );
}
