/*
 * MACDRV menu bar
 *
 * WINESKIN: A top-level window's Win32 menu bar is shown in the Mac menu bar while the window
 * is key, the way a native application's menus are. This file takes snapshots of the window's
 * menus for the Cocoa side and turns the user's choices back into Win32 messages.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include "macdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(menu);

/* Deeper submenus are left out of a snapshot; a menu that holds itself would recurse forever. */
#define MAX_MENU_DEPTH 8

/* Unicode values of Cocoa's NS*FunctionKey characters */
#define MAC_KEY_UP          0xf700
#define MAC_KEY_DOWN        0xf701
#define MAC_KEY_LEFT        0xf702
#define MAC_KEY_RIGHT       0xf703
#define MAC_KEY_F1          0xf704
#define MAC_KEY_INSERT      0xf727
#define MAC_KEY_DELETE      0xf728
#define MAC_KEY_HOME        0xf729
#define MAC_KEY_END         0xf72b
#define MAC_KEY_PAGE_UP     0xf72c
#define MAC_KEY_PAGE_DOWN   0xf72d


static inline uint32_t hash_bytes(uint32_t hash, const void *bytes, size_t size)
{
    const unsigned char *p = bytes;
    while (size--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static inline uint32_t hash_value(uint32_t hash, uint64_t value)
{
    return hash_bytes(hash, &value, sizeof(value));
}


/***********************************************************************
 *              ascii_equal_nocase
 *
 * Compare a run of WCHARs with a lowercase ASCII string, ignoring ASCII case.
 */
static BOOL ascii_equal_nocase(const WCHAR *str, UINT len, const char *ascii)
{
    UINT i;

    for (i = 0; i < len; i++)
    {
        WCHAR c = str[i];
        if (!ascii[i]) return FALSE;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)ascii[i]) return FALSE;
    }
    return !ascii[len];
}


static BOOL name_in_list(const WCHAR *str, UINT len, const char * const *names)
{
    for (; *names; names++)
        if (ascii_equal_nocase(str, len, *names)) return TRUE;
    return FALSE;
}


/***********************************************************************
 *              key_from_name
 *
 * The Mac key equivalent character for the key part of an accelerator's text.
 */
static UniChar key_from_name(const WCHAR *name, UINT len)
{
    static const struct
    {
        const char *name;
        UniChar     key;
    } keys[] =
    {
        { "del", MAC_KEY_DELETE }, { "delete", MAC_KEY_DELETE }, { "entf", MAC_KEY_DELETE },
        { "suppr", MAC_KEY_DELETE },
        { "ins", MAC_KEY_INSERT }, { "insert", MAC_KEY_INSERT }, { "einfg", MAC_KEY_INSERT },
        { "home", MAC_KEY_HOME }, { "pos1", MAC_KEY_HOME },
        { "end", MAC_KEY_END }, { "ende", MAC_KEY_END }, { "fin", MAC_KEY_END },
        { "pgup", MAC_KEY_PAGE_UP }, { "pageup", MAC_KEY_PAGE_UP }, { "page up", MAC_KEY_PAGE_UP },
        { "pgdn", MAC_KEY_PAGE_DOWN }, { "pagedown", MAC_KEY_PAGE_DOWN },
        { "page down", MAC_KEY_PAGE_DOWN }, { "pgdown", MAC_KEY_PAGE_DOWN },
        { "up", MAC_KEY_UP }, { "down", MAC_KEY_DOWN }, { "left", MAC_KEY_LEFT },
        { "right", MAC_KEY_RIGHT },
        { "tab", '\t' },
        { "enter", '\r' }, { "return", '\r' },
        { "esc", 0x1b }, { "escape", 0x1b },
        { "space", ' ' }, { "spacebar", ' ' },
        { "backspace", 0x08 }, { "back", 0x08 }, { "bksp", 0x08 },
        { "plus", '+' }, { "minus", '-' },
    };
    UINT i;

    if (len == 1)
    {
        WCHAR c = name[0];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c < ' ' || c == 0x7f) return 0;
        return c;
    }

    if ((name[0] == 'F' || name[0] == 'f') && len <= 3)
    {
        UINT n = 0;
        for (i = 1; i < len; i++)
        {
            if (name[i] < '0' || name[i] > '9') break;
            n = n * 10 + name[i] - '0';
        }
        if (i == len && n >= 1 && n <= 24) return MAC_KEY_F1 + n - 1;
    }

    for (i = 0; i < ARRAY_SIZE(keys); i++)
        if (ascii_equal_nocase(name, len, keys[i].name)) return keys[i].key;

    return 0;
}


/***********************************************************************
 *              is_reserved_key_equivalent
 *
 * Key equivalents the system, or the app and Window menus, already use.
 */
static BOOL is_reserved_key_equivalent(UniChar key, unsigned int mods)
{
    const unsigned int cmd = MACDRV_KEY_EQUIVALENT_COMMAND;
    const unsigned int opt = MACDRV_KEY_EQUIVALENT_OPTION;
    const unsigned int shift = MACDRV_KEY_EQUIVALENT_SHIFT;

    switch (key)
    {
    case 'q':
    case 'h':
        return mods == cmd || mods == (cmd | opt);
    case '\t':
    case ' ':
    case '`':
        return TRUE;
    case 0x1b:
        return mods == (cmd | opt);
    case '3':
    case '4':
        return mods == (cmd | shift);
    }
    return FALSE;
}


/***********************************************************************
 *              parse_accelerator
 *
 * Turn the accelerator text of a menu item ("Ctrl+S", "Ctrl+Shift+N", "F5", ...) into a Mac key
 * equivalent. Only Ctrl combinations get one, with Command in the place of Ctrl; any other key
 * goes to the program as it always does.
 */
static BOOL parse_accelerator(const WCHAR *text, UINT len, UniChar *key, unsigned int *mods)
{
    static const char * const ctrl_names[] = { "ctrl", "control", "ctl", "strg", "ctrl.", NULL };
    static const char * const shift_names[] = { "shift", "umschalt", "umsch", "maj", NULL };
    static const char * const alt_names[] = { "alt", NULL };
    BOOL ctrl = FALSE;
    unsigned int modifiers = 0;
    UniChar k = 0;
    UINT start = 0, end;

    while (start < len)
    {
        UINT tok_start, tok_len;

        /* a '+' right after a separator, or at the end, is the key itself */
        end = start;
        if (text[end] == '+') end++;
        while (end < len && text[end] != '+') end++;

        tok_start = start;
        tok_len = end - start;
        while (tok_len && text[tok_start] == ' ') { tok_start++; tok_len--; }
        while (tok_len && text[tok_start + tok_len - 1] == ' ') tok_len--;

        if (end < len)
        {
            /* not the last token: a modifier */
            if (name_in_list(text + tok_start, tok_len, ctrl_names)) ctrl = TRUE;
            else if (name_in_list(text + tok_start, tok_len, shift_names)) modifiers |= MACDRV_KEY_EQUIVALENT_SHIFT;
            else if (name_in_list(text + tok_start, tok_len, alt_names)) modifiers |= MACDRV_KEY_EQUIVALENT_OPTION;
            else return FALSE;
        }
        else if (tok_len)
            k = key_from_name(text + tok_start, tok_len);

        start = end + 1;
    }

    if (!ctrl || !k) return FALSE;

    modifiers |= MACDRV_KEY_EQUIVALENT_COMMAND;
    if (is_reserved_key_equivalent(k, modifiers)) return FALSE;

    *key = k;
    *mods = modifiers;
    return TRUE;
}


/***********************************************************************
 *              create_item_title
 *
 * The Mac title of a menu item from its Win32 text: mnemonic ampersands removed, a "(&F)"
 * mnemonic suffix dropped, a trailing "..." made an ellipsis, and the accelerator text after
 * a tab parsed into a key equivalent.
 */
static CFStringRef create_item_title(const WCHAR *text, UINT len, UniChar *key, unsigned int *mods)
{
    WCHAR *title;
    UINT i, out = 0;
    const WCHAR *accel = NULL;
    UINT accel_len = 0;
    CFStringRef ret = NULL;

    *key = 0;
    *mods = 0;

    if (!(title = malloc((len + 1) * sizeof(WCHAR)))) return NULL;

    for (i = 0; i < len; i++)
    {
        WCHAR c = text[i];

        if (c == '\t' || c == '\b')
        {
            accel = text + i + 1;
            accel_len = len - i - 1;
            break;
        }
        if (c == '&')
        {
            if (i + 1 < len && text[i + 1] == '&')
            {
                title[out++] = '&';
                i++;
                continue;
            }
            /* "Name(&N)" */
            if (out && title[out - 1] == '(' && i + 2 < len && text[i + 2] == ')')
            {
                out--;
                i += 2;
                while (out && title[out - 1] == ' ') out--;
            }
            continue;
        }
        title[out++] = c;
    }

    while (out && (title[out - 1] == ' ' || title[out - 1] == '\t')) out--;
    if (out >= 3 && title[out - 1] == '.' && title[out - 2] == '.' && title[out - 3] == '.')
    {
        out -= 2;
        title[out - 1] = 0x2026;
    }

    if (accel)
    {
        while (accel_len && (*accel == ' ' || *accel == '\t' || *accel == '\b')) { accel++; accel_len--; }
        while (accel_len && accel[accel_len - 1] == ' ') accel_len--;
        if (accel_len && !parse_accelerator(accel, accel_len, key, mods))
        {
            *key = 0;
            *mods = 0;
        }
    }

    if (out) ret = CFStringCreateWithCharacters(NULL, (const UniChar *)title, out);
    free(title);
    return ret;
}


/***********************************************************************
 *              get_window_menu
 *
 * The menu bar of a top-level window, or 0.
 */
static HMENU get_window_menu(HWND hwnd)
{
    HMENU menu;

    if ((NtUserGetWindowLongW(hwnd, GWL_STYLE) & (WS_CHILD | WS_POPUP)) == WS_CHILD) return 0;
    menu = (HMENU)NtUserGetWindowLongPtrW(hwnd, GWLP_ID);
    if (!menu || NtUserGetMenuItemCount(menu) == (UINT_PTR)-1) return 0;
    return menu;
}


static HMENU get_sub_menu(HMENU menu, UINT position)
{
    return UlongToHandle(NtUserThunkedMenuItemInfo(menu, position, MF_BYPOSITION, NtUserGetSubMenu,
                                                   NULL, NULL));
}


/***********************************************************************
 *              create_menu_snapshot
 */
static struct macdrv_menu *create_menu_snapshot(HMENU handle, HMENU parent, UINT position, int depth)
{
    struct macdrv_menu *menu;
    UINT_PTR count;
    UINT i;
    uint32_t hash = 2166136261u;

    count = NtUserGetMenuItemCount(handle);
    if (count == (UINT_PTR)-1) return NULL;

    if (!(menu = calloc(1, sizeof(*menu)))) return NULL;
    menu->handle = handle;
    menu->parent = parent;
    menu->position = position;
    if (count && !(menu->items = calloc(count, sizeof(*menu->items))))
    {
        free(menu);
        return NULL;
    }

    hash = hash_value(hash, (UINT_PTR)handle);
    hash = hash_value(hash, (UINT_PTR)parent);
    hash = hash_value(hash, position);

    for (i = 0; i < count; i++)
    {
        struct macdrv_menu_item *item = &menu->items[menu->count];
        WCHAR text[256];
        MENUITEMINFOW info;

        memset(item, 0, sizeof(*item));
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = text;
        info.cch = ARRAY_SIZE(text);
        if (!NtUserThunkedMenuItemInfo(handle, i, MF_BYPOSITION, NtUserGetMenuItemInfoW, &info, NULL))
            continue;

        item->position = i;
        item->command_id = info.wID;

        if (info.fType & MFT_SEPARATOR)
            item->flags = MACDRV_MENU_ITEM_SEPARATOR;
        else
        {
            if (info.cch && !(info.fType & MFT_BITMAP))
                item->title = create_item_title(text, min(info.cch, ARRAY_SIZE(text) - 1),
                                                &item->key_equivalent, &item->key_modifiers);
            if (!item->title)
            {
                TRACE("skipping item %u of %p without a title, type 0x%x\n", i, handle, info.fType);
                continue;
            }

            if (info.fState & (MFS_GRAYED | MFS_DISABLED)) item->flags |= MACDRV_MENU_ITEM_DISABLED;
            if (info.fState & MFS_CHECKED) item->flags |= MACDRV_MENU_ITEM_CHECKED;
            if (info.fType & MFT_RADIOCHECK) item->flags |= MACDRV_MENU_ITEM_RADIO;
            if (info.fState & MFS_DEFAULT) item->flags |= MACDRV_MENU_ITEM_DEFAULT;

            if (info.hSubMenu && depth < MAX_MENU_DEPTH)
                item->submenu = create_menu_snapshot(info.hSubMenu, handle, i, depth + 1);
        }

        hash = hash_value(hash, item->flags);
        hash = hash_value(hash, item->position);
        hash = hash_value(hash, item->command_id);
        hash = hash_value(hash, item->key_equivalent);
        hash = hash_value(hash, item->key_modifiers);
        if (item->title)
        {
            CFIndex len = CFStringGetLength(item->title);
            UniChar buffer[256];
            if (len > ARRAY_SIZE(buffer)) len = ARRAY_SIZE(buffer);
            CFStringGetCharacters(item->title, CFRangeMake(0, len), buffer);
            hash = hash_bytes(hash, buffer, len * sizeof(UniChar));
        }
        hash = hash_value(hash, item->submenu ? item->submenu->hash : 0);

        menu->count++;
    }

    menu->hash = hash;
    return menu;
}


/***********************************************************************
 *              macdrv_sync_window_menu
 *
 * Give a window's new Cocoa window the window's menu bar. The caller holds the win data.
 */
void macdrv_sync_window_menu(struct macdrv_win_data *data)
{
    struct macdrv_menu *snapshot;
    HMENU menu;

    if (!native_menu_bar || !data->cocoa_window) return;
    if (!(menu = get_window_menu(data->hwnd))) return;

    TRACE("win %p menu %p\n", data->hwnd, menu);
    if ((snapshot = create_menu_snapshot(menu, NULL, 0, 0)))
        macdrv_set_cocoa_window_menu(data->cocoa_window, snapshot);
}


/***********************************************************************
 *              SetWindowMenu   (MACDRV.@)
 *
 * Returns whether the Mac menu bar shows the window's menu bar.
 */
BOOL macdrv_SetWindowMenu(HWND hwnd, HMENU menu)
{
    struct macdrv_win_data *data;
    struct macdrv_menu *snapshot = NULL;
    DWORD pid;

    if (!native_menu_bar) return FALSE;
    if (NtUserGetAncestor(hwnd, GA_PARENT) != NtUserGetDesktopWindow()) return FALSE;
    if (!NtUserGetWindowThread(hwnd, &pid) || pid != GetCurrentProcessId()) return FALSE;

    TRACE("win %p menu %p\n", hwnd, menu);

    if (menu && macdrv_get_cocoa_window(hwnd, FALSE))
        snapshot = create_menu_snapshot(menu, NULL, 0, 0);

    if ((data = get_win_data(hwnd)))
    {
        if (data->cocoa_window && (snapshot || !menu))
        {
            macdrv_set_cocoa_window_menu(data->cocoa_window, snapshot);
            snapshot = NULL;
        }
        release_win_data(data);
    }

    if (snapshot) macdrv_free_menu(snapshot);
    return TRUE;
}


/***********************************************************************
 *              query_menu_popup
 *
 * Handler for QUERY_MENU_POPUP: a popup of the window's menu bar is opening in the Mac menu
 * bar. Let the program initialize it, as Windows does, and reply with its snapshot.
 */
bool query_menu_popup(HWND hwnd, macdrv_query *query)
{
    HMENU bar, menu = query->menu_popup.menu, parent = query->menu_popup.parent, sub;
    UINT position = query->menu_popup.position;

    if (!hwnd || !(bar = get_window_menu(hwnd))) return false;

    TRACE("win %p menu %p parent %p position %u begin %d\n", hwnd, menu, parent, position,
          query->menu_popup.begin);

    if (query->menu_popup.begin)
    {
        send_message(hwnd, WM_ENTERMENULOOP, FALSE, 0);
        send_message(hwnd, WM_INITMENU, (WPARAM)bar, 0);
    }

    /* the program may have replaced the popup */
    if (parent && (sub = get_sub_menu(parent, position))) menu = sub;
    if (NtUserGetMenuItemCount(menu) == (UINT_PTR)-1) return false;

    send_message(hwnd, WM_INITMENUPOPUP, (WPARAM)menu, MAKELPARAM(position, FALSE));

    query->menu_popup.result = create_menu_snapshot(menu, parent, position, 0);
    return query->menu_popup.result != NULL;
}


static BOOL menu_notifies_by_position(HMENU menu, HMENU bar)
{
    MENUINFO info;
    DWORD style = 0;

    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = MIM_STYLE;
    if (NtUserGetMenuInfo(menu, &info)) style |= info.dwStyle;
    info.dwStyle = 0;
    if (menu != bar && NtUserGetMenuInfo(bar, &info)) style |= info.dwStyle;
    return (style & MNS_NOTIFYBYPOS) != 0;
}


/***********************************************************************
 *              macdrv_window_menu_command
 *
 * Handler for WINDOW_MENU_COMMAND events.
 */
void macdrv_window_menu_command(HWND hwnd, const macdrv_event *event)
{
    HMENU menu = event->window_menu_command.menu;
    UINT id = event->window_menu_command.command_id;
    HMENU bar;
    DWORD style;

    TRACE("win %p menu %p position %u id %u key equivalent %d\n", hwnd, menu,
          event->window_menu_command.position, id, event->window_menu_command.key_equivalent);

    if (!hwnd || !(bar = get_window_menu(hwnd))) return;
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if (style & WS_DISABLED)
    {
        TRACE("window is disabled\n");
        return;
    }

    if (event->window_menu_command.key_equivalent)
    {
        UINT state;

        /* as TranslateAccelerator does */
        if (get_capture() || (style & WS_MINIMIZE)) return;

        send_message(hwnd, WM_INITMENU, (WPARAM)bar, 0);
        if (menu != bar)
            send_message(hwnd, WM_INITMENUPOPUP, (WPARAM)menu,
                         MAKELPARAM(event->window_menu_command.menu_position, FALSE));

        state = NtUserThunkedMenuItemInfo(bar, id, MF_BYCOMMAND, NtUserGetMenuState, NULL, NULL);
        if (state != ~0u && (state & (MF_DISABLED | MF_GRAYED)))
        {
            TRACE("item %u is disabled\n", id);
            return;
        }

        /* Posted, not sent: the command may run a modal loop, and this thread processes no
           nested Mac events while handling this one. */
        NtUserPostMessage(hwnd, WM_COMMAND, MAKEWPARAM(id, 1), 0);
        return;
    }

    if (menu_notifies_by_position(menu, bar))
        NtUserPostMessage(hwnd, WM_MENUCOMMAND, event->window_menu_command.position, (LPARAM)menu);
    else
        NtUserPostMessage(hwnd, WM_COMMAND, id, 0);
}


/***********************************************************************
 *              macdrv_window_menu_loop
 *
 * Handler for WINDOW_MENU_LOOP events.
 */
void macdrv_window_menu_loop(HWND hwnd, const macdrv_event *event)
{
    TRACE("win %p kind %d menu %p\n", hwnd, event->window_menu_loop.kind, event->window_menu_loop.menu);

    if (!hwnd) return;

    switch (event->window_menu_loop.kind)
    {
    case MENU_LOOP_POPUP_CLOSED:
        send_message(hwnd, WM_UNINITMENUPOPUP, (WPARAM)event->window_menu_loop.menu, 0);
        break;
    case MENU_LOOP_END:
        send_message(hwnd, WM_EXITMENULOOP, FALSE, 0);
        break;
    }
}
