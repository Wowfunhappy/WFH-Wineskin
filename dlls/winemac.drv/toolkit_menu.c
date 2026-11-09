/*
 * WINESKIN: Mac menu bar menus for GTK and Qt programs
 *
 * GTK (2 and 3) and Qt 4 draw their menu bars themselves; a window of theirs has no Win32 menu for the Mac
 * menu bar to show. When such a window is activated, macdrv_toolkit_menu_check (menu.c) asks
 * this code to look for the toolkit's menu bar in it. If there is one, the window is given a
 * Win32 menu bar made from it and the toolkit's own bar is hidden. The window is subclassed:
 * WM_INITMENUPOPUP, which the Mac menu bar sends as a menu opens, refills that menu from the
 * toolkit's, so it shows the items' current text and state, and WM_COMMAND activates the
 * toolkit's item.
 *
 * The toolkits are reached through the functions their DLLs export, looked up when a window is
 * first examined, so nothing is loaded into a program that does not use them.
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
#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(menu);

#define FIRST_COMMAND_ID 0x6000
#define MAX_DEPTH 6

/* A toolkit: how to read and drive its menus. */
struct toolkit
{
    const char *name;
    /* the toolkit's menu bar widget in the window, or NULL */
    void *(*find_menu_bar)(HWND hwnd);
    /* the items of a menu bar or menu, in order; free the array with free_items */
    void **(*get_items)(void *menu, UINT *count);
    void (*free_items)(void **items, UINT count);
    /* an item's text with '&' mnemonics and "\tCtrl+..." accelerator text, or NULL for a separator;
       HeapFree it */
    WCHAR *(*item_text)(void *item);
    void *(*item_submenu)(void *item);
    BOOL (*item_visible)(void *item);
    BOOL (*item_enabled)(void *item);
    /* MF_CHECKED, plus MFT_RADIOCHECK for a radio item, or 0 */
    UINT (*item_check)(void *item);
    void (*keep_item)(void *item);
    void (*activate)(void *item);
    void (*hide_menu_bar)(void *bar);
};

struct popup
{
    HMENU hmenu;
    void *menu;
};

struct bridged_window
{
    struct bridged_window *next;
    HWND hwnd;
    const struct toolkit *toolkit;
    WNDPROC old_proc;
    void **items;               /* by command ID - FIRST_COMMAND_ID */
    UINT item_count, item_capacity;
    struct popup *popups;
    UINT popup_count, popup_capacity;
};

static struct bridged_window *bridged_windows;
static CRITICAL_SECTION bridge_cs;
static CRITICAL_SECTION_DEBUG bridge_cs_debug =
{
    0, 0, &bridge_cs,
    { &bridge_cs_debug.ProcessLocksList, &bridge_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": bridge_cs") }
};
static CRITICAL_SECTION bridge_cs = { &bridge_cs_debug, -1, 0, 0, 0, 0 };

static void *grow(void *array, UINT *capacity, UINT count, SIZE_T size)
{
    UINT new_capacity;
    void *ret;

    if (count < *capacity) return array;
    new_capacity = *capacity ? *capacity * 2 : 32;
    if (array) ret = HeapReAlloc(GetProcessHeap(), 0, array, new_capacity * size);
    else ret = HeapAlloc(GetProcessHeap(), 0, new_capacity * size);
    if (ret) *capacity = new_capacity;
    return ret;
}

static WCHAR *utf8_to_wide(const char *str)
{
    int len;
    WCHAR *ret;

    if (!str) return NULL;
    len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (!(ret = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR)))) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, str, -1, ret, len);
    return ret;
}

/* "Label" + "\t" + accelerator text, with the toolkit's mnemonic character made '&' and literal
   '&'s doubled. */
static WCHAR *make_item_text(const WCHAR *label, WCHAR mnemonic, const WCHAR *accel)
{
    SIZE_T len = lstrlenW(label) * 2 + (accel ? lstrlenW(accel) + 1 : 0) + 1;
    WCHAR *ret, *p;
    const WCHAR *s;

    if (!(ret = p = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR)))) return NULL;
    for (s = label; *s; s++)
    {
        if (*s == mnemonic)
        {
            if (s[1] == mnemonic) { *p++ = mnemonic; s++; }
            else if (s[1]) *p++ = '&';
        }
        else if (*s == '&' && mnemonic != '&') { *p++ = '&'; *p++ = '&'; }
        else *p++ = *s;
    }
    if (accel && *accel)
    {
        *p++ = '\t';
        lstrcpyW(p, accel);
    }
    else *p = 0;
    return ret;
}


/***********************************************************************
 *              GTK (2 and 3)
 */

typedef ULONG_PTR GType;
typedef struct _GList { void *data; struct _GList *next, *prev; } GList;
typedef struct { unsigned int accel_key; unsigned int accel_mods; unsigned int accel_flags; } GtkAccelKey;

static struct
{
    BOOL tried, ok;
    void *(CDECL *handle_table_lookup)(HWND);
    void (CDECL *window_get_user_data)(void *, void **);
    GList *(CDECL *container_get_children)(void *);
    void (CDECL *list_free)(GList *);
    int (CDECL *type_check_instance_is_a)(void *, GType);
    GType (CDECL *menu_bar_get_type)(void);
    GType (CDECL *container_get_type)(void);
    GType (CDECL *menu_item_get_type)(void);
    GType (CDECL *separator_menu_item_get_type)(void);
    GType (CDECL *check_menu_item_get_type)(void);
    GType (CDECL *radio_menu_item_get_type)(void);
    GType (CDECL *tearoff_menu_item_get_type)(void);
    GType (CDECL *label_get_type)(void);
    const char *(CDECL *menu_item_get_label)(void *);
    void *(CDECL *bin_get_child)(void *);
    const char *(CDECL *label_get_label)(void *);
    void *(CDECL *menu_item_get_submenu)(void *);
    int (CDECL *widget_is_sensitive)(void *);
    int (CDECL *widget_get_visible)(void *);
    int (CDECL *check_menu_item_get_active)(void *);
    const char *(CDECL *menu_item_get_accel_path)(void *);
    int (CDECL *accel_map_lookup_entry)(const char *, GtkAccelKey *);
    const char *(CDECL *keyval_name)(unsigned int);
    unsigned int (CDECL *keyval_to_unicode)(unsigned int);
    void (CDECL *menu_item_activate)(void *);
    void (CDECL *widget_hide)(void *);
    void (CDECL *widget_set_no_show_all)(void *, int);
    unsigned long (CDECL *signal_connect_data)(void *, const char *, void *, void *, void *, int);
    void *(CDECL *object_ref)(void *);
    unsigned int (CDECL *idle_add)(int (CDECL *)(void *), void *);
} gtk;

static BOOL load_gtk(void)
{
    static const WCHAR *gtk_names[] = { L"libgtk-win32-2.0-0.dll", L"libgtk-3-0.dll" };
    static const WCHAR *gdk_names[] = { L"libgdk-win32-2.0-0.dll", L"libgdk-3-0.dll" };
    HMODULE gtk_module = 0, gdk_module = 0, gobject, glib;
    UINT i;

    if (gtk.tried) return gtk.ok;

    for (i = 0; i < ARRAY_SIZE(gtk_names); i++)
    {
        if ((gtk_module = GetModuleHandleW(gtk_names[i])))
        {
            gdk_module = GetModuleHandleW(gdk_names[i]);
            break;
        }
    }
    if (!gtk_module) return FALSE;  /* not yet: a program may load GTK later */
    gtk.tried = TRUE;
    gobject = GetModuleHandleW(L"libgobject-2.0-0.dll");
    glib = GetModuleHandleW(L"libglib-2.0-0.dll");
    if (!gdk_module || !gobject || !glib) return FALSE;

#define GET(mod, field, name) gtk.field = (void *)GetProcAddress(mod, name)
    GET(gdk_module, handle_table_lookup, "gdk_win32_handle_table_lookup");
    GET(gdk_module, window_get_user_data, "gdk_window_get_user_data");
    GET(gdk_module, keyval_name, "gdk_keyval_name");
    GET(gdk_module, keyval_to_unicode, "gdk_keyval_to_unicode");
    GET(gtk_module, container_get_children, "gtk_container_get_children");
    GET(gtk_module, menu_bar_get_type, "gtk_menu_bar_get_type");
    GET(gtk_module, container_get_type, "gtk_container_get_type");
    GET(gtk_module, menu_item_get_type, "gtk_menu_item_get_type");
    GET(gtk_module, separator_menu_item_get_type, "gtk_separator_menu_item_get_type");
    GET(gtk_module, check_menu_item_get_type, "gtk_check_menu_item_get_type");
    GET(gtk_module, radio_menu_item_get_type, "gtk_radio_menu_item_get_type");
    GET(gtk_module, tearoff_menu_item_get_type, "gtk_tearoff_menu_item_get_type");
    GET(gtk_module, label_get_type, "gtk_label_get_type");
    GET(gtk_module, menu_item_get_label, "gtk_menu_item_get_label");
    GET(gtk_module, bin_get_child, "gtk_bin_get_child");
    GET(gtk_module, label_get_label, "gtk_label_get_label");
    GET(gtk_module, menu_item_get_submenu, "gtk_menu_item_get_submenu");
    GET(gtk_module, widget_is_sensitive, "gtk_widget_is_sensitive");
    GET(gtk_module, widget_get_visible, "gtk_widget_get_visible");
    GET(gtk_module, check_menu_item_get_active, "gtk_check_menu_item_get_active");
    GET(gtk_module, menu_item_get_accel_path, "gtk_menu_item_get_accel_path");
    GET(gtk_module, accel_map_lookup_entry, "gtk_accel_map_lookup_entry");
    GET(gtk_module, menu_item_activate, "gtk_menu_item_activate");
    GET(gtk_module, widget_hide, "gtk_widget_hide");
    GET(gtk_module, widget_set_no_show_all, "gtk_widget_set_no_show_all");
    GET(gobject, signal_connect_data, "g_signal_connect_data");
    GET(gobject, list_free, "g_list_free");
    if (!gtk.list_free) GET(glib, list_free, "g_list_free");
    GET(gobject, type_check_instance_is_a, "g_type_check_instance_is_a");
    GET(gobject, object_ref, "g_object_ref");
    GET(glib, idle_add, "g_idle_add");
#undef GET

    gtk.ok = gtk.handle_table_lookup && gtk.window_get_user_data && gtk.container_get_children &&
             gtk.list_free && gtk.type_check_instance_is_a && gtk.menu_bar_get_type &&
             gtk.container_get_type && gtk.menu_item_get_type && gtk.menu_item_get_submenu &&
             gtk.menu_item_activate && gtk.widget_hide && gtk.object_ref && gtk.idle_add &&
             (gtk.menu_item_get_label || (gtk.bin_get_child && gtk.label_get_label && gtk.label_get_type));
    TRACE("GTK %s\n", gtk.ok ? "found" : "lacks functions");
    return gtk.ok;
}

static BOOL gtk_is_a(void *instance, GType (CDECL *get_type)(void))
{
    return instance && get_type && gtk.type_check_instance_is_a(instance, get_type());
}

static void *gtk_find_menu_bar_in(void *widget, int depth)
{
    GList *children, *l;
    void *ret = NULL;

    if (depth > MAX_DEPTH || !gtk_is_a(widget, gtk.container_get_type)) return NULL;
    children = gtk.container_get_children(widget);
    for (l = children; l && !ret; l = l->next)
    {
        if (gtk_is_a(l->data, gtk.menu_bar_get_type) &&
            (!gtk.widget_get_visible || gtk.widget_get_visible(l->data)))
            ret = l->data;
        else
            ret = gtk_find_menu_bar_in(l->data, depth + 1);
    }
    gtk.list_free(children);
    return ret;
}

static void *gtk_find_menu_bar(HWND hwnd)
{
    void *window, *widget = NULL;

    if (!load_gtk()) return NULL;
    if (!(window = gtk.handle_table_lookup(hwnd))) return NULL;
    gtk.window_get_user_data(window, &widget);
    return gtk_find_menu_bar_in(widget, 0);
}

static void **gtk_get_items(void *menu, UINT *count)
{
    GList *children = gtk.container_get_children(menu), *l;
    void **ret;
    UINT n = 0;

    for (l = children; l; l = l->next) n++;
    if ((ret = HeapAlloc(GetProcessHeap(), 0, max(n, 1) * sizeof(*ret))))
        for (n = 0, l = children; l; l = l->next) ret[n++] = l->data;
    gtk.list_free(children);
    *count = ret ? n : 0;
    return ret;
}

static void gtk_free_items(void **items, UINT count)
{
    HeapFree(GetProcessHeap(), 0, items);
}

static WCHAR *gtk_accel_text(void *item)
{
    const char *path, *name;
    GtkAccelKey key = { 0 };
    WCHAR buffer[64], *p = buffer;
    unsigned int c;

    if (!gtk.menu_item_get_accel_path || !gtk.accel_map_lookup_entry) return NULL;
    if (!(path = gtk.menu_item_get_accel_path(item))) return NULL;
    if (!gtk.accel_map_lookup_entry(path, &key) || !key.accel_key) return NULL;

    if (key.accel_mods & 4) { lstrcpyW(p, L"Ctrl+"); p += 5; }     /* GDK_CONTROL_MASK */
    if (key.accel_mods & 1) { lstrcpyW(p, L"Shift+"); p += 6; }    /* GDK_SHIFT_MASK */
    if (key.accel_mods & 8) { lstrcpyW(p, L"Alt+"); p += 4; }      /* GDK_MOD1_MASK */

    c = gtk.keyval_to_unicode ? gtk.keyval_to_unicode(key.accel_key) : 0;
    if (c > ' ' && c < 0x10000)
    {
        *p++ = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
        *p = 0;
    }
    else
    {
        static const struct { const char *gdk; const WCHAR *win; } names[] =
        {
            { "Page_Up", L"PgUp" }, { "Page_Down", L"PgDn" }, { "BackSpace", L"Backspace" },
            { "Delete", L"Del" }, { "Insert", L"Ins" }, { "Return", L"Enter" }, { "KP_Enter", L"Enter" },
            { "Escape", L"Esc" }, { "space", L"Space" }, { "Home", L"Home" }, { "End", L"End" },
            { "Left", L"Left" }, { "Right", L"Right" }, { "Up", L"Up" }, { "Down", L"Down" },
            { "Tab", L"Tab" }, { "KP_Add", L"Plus" }, { "KP_Subtract", L"Minus" },
        };
        UINT i;

        if (!gtk.keyval_name || !(name = gtk.keyval_name(key.accel_key))) return NULL;
        for (i = 0; i < ARRAY_SIZE(names); i++)
            if (!lstrcmpA(name, names[i].gdk)) break;
        if (i < ARRAY_SIZE(names)) lstrcpyW(p, names[i].win);
        else if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9' && lstrlenA(name) <= 3)
            MultiByteToWideChar(CP_UTF8, 0, name, -1, p, ARRAY_SIZE(buffer) - (p - buffer));
        else return NULL;
    }
    {
        WCHAR *ret = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(buffer) + 1) * sizeof(WCHAR));
        if (ret) lstrcpyW(ret, buffer);
        return ret;
    }
}

static WCHAR *gtk_item_text(void *item)
{
    const char *label = NULL;
    WCHAR *wlabel, *accel, *ret;

    if (gtk_is_a(item, gtk.separator_menu_item_get_type) || gtk_is_a(item, gtk.tearoff_menu_item_get_type))
        return NULL;
    if (gtk.menu_item_get_label) label = gtk.menu_item_get_label(item);
    if (!label && gtk.bin_get_child)
    {
        void *child = gtk.bin_get_child(item);
        if (child && gtk_is_a(child, gtk.label_get_type)) label = gtk.label_get_label(child);
    }
    if (!label) label = "";
    if (!(wlabel = utf8_to_wide(label))) return NULL;
    accel = gtk_accel_text(item);
    ret = make_item_text(wlabel, '_', accel);
    HeapFree(GetProcessHeap(), 0, wlabel);
    HeapFree(GetProcessHeap(), 0, accel);
    return ret;
}

static void *gtk_item_submenu(void *item)
{
    if (!gtk_is_a(item, gtk.menu_item_get_type)) return NULL;
    return gtk.menu_item_get_submenu(item);
}

static BOOL gtk_item_visible(void *item)
{
    if (!gtk_is_a(item, gtk.menu_item_get_type)) return FALSE;
    if (gtk_is_a(item, gtk.tearoff_menu_item_get_type)) return FALSE;
    return !gtk.widget_get_visible || gtk.widget_get_visible(item);
}

static BOOL gtk_item_enabled(void *item)
{
    return !gtk.widget_is_sensitive || gtk.widget_is_sensitive(item);
}

static UINT gtk_item_check(void *item)
{
    UINT ret;

    if (!gtk.check_menu_item_get_active || !gtk_is_a(item, gtk.check_menu_item_get_type)) return 0;
    ret = gtk.check_menu_item_get_active(item) ? MF_CHECKED : 0;
    if (gtk_is_a(item, gtk.radio_menu_item_get_type)) ret |= MFT_RADIOCHECK;
    return ret;
}

static void gtk_keep_item(void *item)
{
    gtk.object_ref(item);
}

static int CDECL gtk_activate_idle(void *item)
{
    gtk.menu_item_activate(item);
    return 0;  /* run once */
}

/* Activated from the main loop rather than from inside the message being dispatched. */
static void gtk_activate(void *item)
{
    gtk.idle_add(gtk_activate_idle, item);
}

static void CDECL gtk_hide_again(void *widget, void *data)
{
    gtk.widget_hide(widget);
}

/* Hidden for good: gtk_widget_show_all() passes over it, and showing it hides it again. */
static void gtk_hide_menu_bar(void *bar)
{
    if (gtk.widget_set_no_show_all) gtk.widget_set_no_show_all(bar, TRUE);
    if (gtk.signal_connect_data)
        gtk.signal_connect_data(bar, "show", gtk_hide_again, NULL, NULL, 1 /* G_CONNECT_AFTER */);
    gtk.widget_hide(bar);
}

static const struct toolkit gtk_toolkit =
{
    "GTK", gtk_find_menu_bar, gtk_get_items, gtk_free_items, gtk_item_text, gtk_item_submenu,
    gtk_item_visible, gtk_item_enabled, gtk_item_check, gtk_keep_item, gtk_activate, gtk_hide_menu_bar,
};


/***********************************************************************
 *              Qt 4 (built with Visual C++)
 *
 * Member functions are called through their exported decorated names. A member function that
 * returns a class takes a hidden pointer to the result after "this".
 */

#ifdef __i386__
#define QT_THISCALL __attribute__((thiscall))
#define QT_NAME(x86, x64) x86
#else
#define QT_THISCALL
#define QT_NAME(x86, x64) x64
#endif

struct qt_list_data { LONG ref; int alloc, begin, end; unsigned int sharable; void *array[1]; };
struct qt_string_data { LONG ref; int alloc, size; WCHAR *data; };

static struct
{
    BOOL tried, ok;
    void *(CDECL *widget_find)(HWND);
    struct qt_list_data **(QT_THISCALL *object_children)(void *);
    unsigned char (QT_THISCALL *object_inherits)(void *, const char *);
    struct qt_list_data **(QT_THISCALL *widget_actions)(void *, struct qt_list_data **);
    struct qt_string_data **(QT_THISCALL *action_text)(void *, struct qt_string_data **);
    void *(QT_THISCALL *action_menu)(void *);
    unsigned char (QT_THISCALL *action_is_separator)(void *);
    unsigned char (QT_THISCALL *action_is_enabled)(void *);
    unsigned char (QT_THISCALL *action_is_visible)(void *);
    unsigned char (QT_THISCALL *action_is_checkable)(void *);
    unsigned char (QT_THISCALL *action_is_checked)(void *);
    void *(QT_THISCALL *action_group)(void *);
    unsigned char (QT_THISCALL *group_is_exclusive)(void *);
    void **(QT_THISCALL *action_shortcut)(void *, void **);
    struct qt_string_data **(QT_THISCALL *key_sequence_to_string)(void **, struct qt_string_data **, int);
    void (QT_THISCALL *key_sequence_destroy)(void **);
    void (QT_THISCALL *action_activate)(void *, int);
    void (QT_THISCALL *menu_about_to_show)(void *);
    void (QT_THISCALL *menu_bar_set_visible)(void *, int);
    void (QT_THISCALL *widget_set_fixed_height)(void *, int);
    void (CDECL *string_free)(struct qt_string_data *);
    void (CDECL *mem_free)(void *);
} qt;

static BOOL load_qt(void)
{
    HMODULE core, gui;

    if (qt.tried) return qt.ok;
    if (!(gui = GetModuleHandleW(L"QtGui4.dll")) && !(gui = GetModuleHandleW(L"QtGuid4.dll"))) return FALSE;
    qt.tried = TRUE;
    if (!(core = GetModuleHandleW(L"QtCore4.dll")) && !(core = GetModuleHandleW(L"QtCored4.dll"))) return FALSE;

#define GET(mod, field, x86, x64) qt.field = (void *)GetProcAddress(mod, QT_NAME(x86, x64))
    GET(gui, widget_find, "?find@QWidget@@SAPAV1@PAUHWND__@@@Z", "?find@QWidget@@SAPEAV1@PEAUHWND__@@@Z");
    GET(core, object_children, "?children@QObject@@QBEABV?$QList@PAVQObject@@@@XZ",
        "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
    GET(core, object_inherits, "?inherits@QObject@@QBE_NPBD@Z", "?inherits@QObject@@QEBA_NPEBD@Z");
    GET(gui, widget_actions, "?actions@QWidget@@QBE?AV?$QList@PAVQAction@@@@XZ",
        "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ");
    GET(gui, action_text, "?text@QAction@@QBE?AVQString@@XZ", "?text@QAction@@QEBA?AVQString@@XZ");
    GET(gui, action_menu, "?menu@QAction@@QBEPAVQMenu@@XZ", "?menu@QAction@@QEBAPEAVQMenu@@XZ");
    GET(gui, action_is_separator, "?isSeparator@QAction@@QBE_NXZ", "?isSeparator@QAction@@QEBA_NXZ");
    GET(gui, action_is_enabled, "?isEnabled@QAction@@QBE_NXZ", "?isEnabled@QAction@@QEBA_NXZ");
    GET(gui, action_is_visible, "?isVisible@QAction@@QBE_NXZ", "?isVisible@QAction@@QEBA_NXZ");
    GET(gui, action_is_checkable, "?isCheckable@QAction@@QBE_NXZ", "?isCheckable@QAction@@QEBA_NXZ");
    GET(gui, action_is_checked, "?isChecked@QAction@@QBE_NXZ", "?isChecked@QAction@@QEBA_NXZ");
    GET(gui, action_group, "?actionGroup@QAction@@QBEPAVQActionGroup@@XZ",
        "?actionGroup@QAction@@QEBAPEAVQActionGroup@@XZ");
    GET(gui, group_is_exclusive, "?isExclusive@QActionGroup@@QBE_NXZ", "?isExclusive@QActionGroup@@QEBA_NXZ");
    GET(gui, action_shortcut, "?shortcut@QAction@@QBE?AVQKeySequence@@XZ",
        "?shortcut@QAction@@QEBA?AVQKeySequence@@XZ");
    GET(gui, key_sequence_to_string, "?toString@QKeySequence@@QBE?AVQString@@W4SequenceFormat@1@@Z",
        "?toString@QKeySequence@@QEBA?AVQString@@W4SequenceFormat@1@@Z");
    GET(gui, key_sequence_destroy, "??1QKeySequence@@QAE@XZ", "??1QKeySequence@@QEAA@XZ");
    GET(gui, action_activate, "?activate@QAction@@QAEXW4ActionEvent@1@@Z", "?activate@QAction@@QEAAXW4ActionEvent@1@@Z");
    GET(gui, menu_about_to_show, "?aboutToShow@QMenu@@IAEXXZ", "?aboutToShow@QMenu@@IEAAXXZ");
    GET(gui, menu_bar_set_visible, "?setVisible@QMenuBar@@UAEX_N@Z", "?setVisible@QMenuBar@@UEAAX_N@Z");
    GET(gui, widget_set_fixed_height, "?setFixedHeight@QWidget@@QAEXH@Z", "?setFixedHeight@QWidget@@QEAAXH@Z");
    GET(core, string_free, "?free@QString@@CAXPAUData@1@@Z", "?free@QString@@CAXPEAUData@1@@Z");
    GET(core, mem_free, "?qFree@@YAXPAX@Z", "?qFree@@YAXPEAX@Z");
#undef GET

    qt.ok = qt.widget_find && qt.object_children && qt.object_inherits && qt.widget_actions &&
            qt.action_text && qt.action_menu && qt.action_is_separator && qt.action_is_enabled &&
            qt.action_is_visible && qt.action_activate && qt.menu_bar_set_visible &&
            qt.string_free && qt.mem_free;
    TRACE("Qt 4 %s\n", qt.ok ? "found" : "lacks functions");
    return qt.ok;
}

static void qt_release_list(struct qt_list_data *d)
{
    if (d && !InterlockedDecrement(&d->ref)) qt.mem_free(d);
}

static WCHAR *qt_take_string(struct qt_string_data *d)
{
    WCHAR *ret;

    if (!d) return NULL;
    if ((ret = HeapAlloc(GetProcessHeap(), 0, (d->size + 1) * sizeof(WCHAR))))
    {
        memcpy(ret, d->data, d->size * sizeof(WCHAR));
        ret[d->size] = 0;
    }
    if (!InterlockedDecrement(&d->ref)) qt.string_free(d);
    return ret;
}

static void *qt_find_menu_bar_in(void *object, int depth)
{
    struct qt_list_data *children;
    void *ret = NULL;
    int i;

    if (depth > MAX_DEPTH || !object) return NULL;
    children = *qt.object_children(object);
    for (i = children->begin; i < children->end && !ret; i++)
    {
        void *child = children->array[i];
        if (qt.object_inherits(child, "QMenuBar")) ret = child;
        else if (qt.object_inherits(child, "QWidget")) ret = qt_find_menu_bar_in(child, depth + 1);
    }
    return ret;
}

static void *qt_find_menu_bar(HWND hwnd)
{
    if (!load_qt()) return NULL;
    return qt_find_menu_bar_in(qt.widget_find(hwnd), 0);
}

static void **qt_get_items(void *menu, UINT *count)
{
    struct qt_list_data *list = NULL;
    void **ret;
    UINT n;
    int i;

    /* a program may fill a menu as it is about to show */
    if (qt.menu_about_to_show && !qt.object_inherits(menu, "QMenuBar")) qt.menu_about_to_show(menu);

    qt.widget_actions(menu, &list);
    n = list ? list->end - list->begin : 0;
    if ((ret = HeapAlloc(GetProcessHeap(), 0, max(n, 1) * sizeof(*ret))))
        for (i = 0; i < (int)n; i++) ret[i] = list->array[list->begin + i];
    qt_release_list(list);
    *count = ret ? n : 0;
    return ret;
}

static WCHAR *qt_item_text(void *item)
{
    struct qt_string_data *text = NULL, *accel_text = NULL;
    WCHAR *label, *accel = NULL, *ret;
    void *shortcut = NULL;

    if (qt.action_is_separator(item)) return NULL;
    qt.action_text(item, &text);
    if (!(label = qt_take_string(text))) return NULL;

    if (qt.action_shortcut && qt.key_sequence_to_string && qt.key_sequence_destroy)
    {
        qt.action_shortcut(item, &shortcut);
        qt.key_sequence_to_string(&shortcut, &accel_text, 1 /* QKeySequence::PortableText */);
        qt.key_sequence_destroy(&shortcut);
        accel = qt_take_string(accel_text);
    }

    /* Qt marks mnemonics with '&', as Win32 does */
    if ((ret = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(label) + (accel ? lstrlenW(accel) + 1 : 0) + 1) * sizeof(WCHAR))))
    {
        lstrcpyW(ret, label);
        if (accel && *accel)
        {
            lstrcatW(ret, L"\t");
            lstrcatW(ret, accel);
        }
    }
    HeapFree(GetProcessHeap(), 0, label);
    HeapFree(GetProcessHeap(), 0, accel);
    return ret;
}

static void *qt_item_submenu(void *item)
{
    return qt.action_menu(item);
}

static BOOL qt_item_visible(void *item)
{
    return qt.action_is_visible(item);
}

static BOOL qt_item_enabled(void *item)
{
    return qt.action_is_enabled(item);
}

static UINT qt_item_check(void *item)
{
    void *group;
    UINT ret;

    if (!qt.action_is_checkable || !qt.action_is_checked || !qt.action_is_checkable(item)) return 0;
    ret = qt.action_is_checked(item) ? MF_CHECKED : 0;
    if (qt.action_group && (group = qt.action_group(item)) &&
        (!qt.group_is_exclusive || qt.group_is_exclusive(group)))
        ret |= MFT_RADIOCHECK;
    return ret;
}

static void qt_keep_item(void *item)
{
}

static void qt_activate(void *item)
{
    qt.action_activate(item, 0 /* QAction::Trigger */);
}

/* Hidden for good: with no height, the window's layout gives it no room even if it is shown again. */
static void qt_hide_menu_bar(void *bar)
{
    if (qt.widget_set_fixed_height) qt.widget_set_fixed_height(bar, 0);
    qt.menu_bar_set_visible(bar, FALSE);
}

static const struct toolkit qt4_toolkit =
{
    "Qt 4", qt_find_menu_bar, qt_get_items, gtk_free_items, qt_item_text, qt_item_submenu,
    qt_item_visible, qt_item_enabled, qt_item_check, qt_keep_item, qt_activate, qt_hide_menu_bar,
};


/***********************************************************************
 *              building the Win32 menus
 */

static const struct toolkit *toolkits[] = { &gtk_toolkit, &qt4_toolkit };

static struct bridged_window *find_bridged_window(HWND hwnd)
{
    struct bridged_window *win;
    for (win = bridged_windows; win; win = win->next) if (win->hwnd == hwnd) return win;
    return NULL;
}

static UINT command_id_for_item(struct bridged_window *win, void *item)
{
    UINT i;
    void **items;

    for (i = 0; i < win->item_count; i++) if (win->items[i] == item) return FIRST_COMMAND_ID + i;
    if (!(items = grow(win->items, &win->item_capacity, win->item_count, sizeof(*items)))) return 0;
    win->items = items;
    win->toolkit->keep_item(item);
    win->items[win->item_count] = item;
    return FIRST_COMMAND_ID + win->item_count++;
}

static void remember_popup(struct bridged_window *win, HMENU hmenu, void *menu)
{
    struct popup *popups;
    UINT i;

    for (i = 0; i < win->popup_count; i++)
    {
        if (win->popups[i].hmenu == hmenu)
        {
            win->popups[i].menu = menu;
            return;
        }
    }
    if (!(popups = grow(win->popups, &win->popup_capacity, win->popup_count, sizeof(*popups)))) return;
    win->popups = popups;
    win->popups[win->popup_count].hmenu = hmenu;
    win->popups[win->popup_count++].menu = menu;
}

static void *menu_for_popup(struct bridged_window *win, HMENU hmenu)
{
    UINT i;
    for (i = 0; i < win->popup_count; i++) if (win->popups[i].hmenu == hmenu) return win->popups[i].menu;
    return NULL;
}

static void fill_menu(struct bridged_window *win, HMENU hmenu, void *menu, int depth)
{
    const struct toolkit *tk = win->toolkit;
    void **items;
    UINT count, i, n;

    while ((n = GetMenuItemCount(hmenu)) && n != (UINT)-1) DeleteMenu(hmenu, 0, MF_BYPOSITION);
    if (!(items = tk->get_items(menu, &count))) return;

    for (i = 0; i < count; i++)
    {
        MENUITEMINFOW info = { sizeof(info) };
        void *item = items[i], *submenu;
        WCHAR *text;
        UINT check;

        if (!tk->item_visible(item)) continue;
        if (!(text = tk->item_text(item)))
        {
            /* no separator first, last or doubled */
            n = GetMenuItemCount(hmenu);
            if (n && n != (UINT)-1)
            {
                info.fMask = MIIM_FTYPE;
                if (GetMenuItemInfoW(hmenu, n - 1, TRUE, &info) && !(info.fType & MFT_SEPARATOR))
                    AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL);
            }
            continue;
        }

        info.fMask = MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
        info.fType = MFT_STRING;
        info.dwTypeData = text;
        info.fState = tk->item_enabled(item) ? MFS_ENABLED : MFS_GRAYED;
        check = tk->item_check(item);
        if (check & MF_CHECKED) info.fState |= MFS_CHECKED;
        if (check & MFT_RADIOCHECK) info.fType |= MFT_RADIOCHECK;

        if ((submenu = tk->item_submenu(item)) && depth < MAX_DEPTH)
        {
            HMENU popup = CreatePopupMenu();
            fill_menu(win, popup, submenu, depth + 1);
            remember_popup(win, popup, submenu);
            info.fMask |= MIIM_SUBMENU;
            info.hSubMenu = popup;
        }
        else
        {
            info.fMask |= MIIM_ID;
            info.wID = command_id_for_item(win, item);
        }
        InsertMenuItemW(hmenu, GetMenuItemCount(hmenu), TRUE, &info);
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* no trailing separator */
    n = GetMenuItemCount(hmenu);
    if (n && n != (UINT)-1)
    {
        MENUITEMINFOW info = { sizeof(info), MIIM_FTYPE };
        if (GetMenuItemInfoW(hmenu, n - 1, TRUE, &info) && (info.fType & MFT_SEPARATOR))
            DeleteMenu(hmenu, n - 1, MF_BYPOSITION);
    }
    tk->free_items(items, count);
}

static LRESULT WINAPI bridged_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct bridged_window *win;
    WNDPROC old_proc;
    void *menu, *item;

    EnterCriticalSection(&bridge_cs);
    if (!(win = find_bridged_window(hwnd)))
    {
        LeaveCriticalSection(&bridge_cs);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    old_proc = win->old_proc;

    switch (msg)
    {
    case WM_INITMENUPOPUP:
        if ((menu = menu_for_popup(win, (HMENU)wparam)))
        {
            fill_menu(win, (HMENU)wparam, menu, 0);
            LeaveCriticalSection(&bridge_cs);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!lparam && LOWORD(wparam) >= FIRST_COMMAND_ID &&
            LOWORD(wparam) - FIRST_COMMAND_ID < win->item_count)
        {
            item = win->items[LOWORD(wparam) - FIRST_COMMAND_ID];
            TRACE("win %p command %u item %p\n", hwnd, LOWORD(wparam), item);
            win->toolkit->activate(item);
            LeaveCriticalSection(&bridge_cs);
            return 0;
        }
        break;
    case WM_NCDESTROY:
    {
        struct bridged_window **p;
        for (p = &bridged_windows; *p; p = &(*p)->next)
        {
            if (*p == win)
            {
                *p = win->next;
                break;
            }
        }
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)old_proc);
        HeapFree(GetProcessHeap(), 0, win->items);
        HeapFree(GetProcessHeap(), 0, win->popups);
        HeapFree(GetProcessHeap(), 0, win);
        break;
    }
    }

    LeaveCriticalSection(&bridge_cs);
    return CallWindowProcW(old_proc, hwnd, msg, wparam, lparam);
}

static void bridge_window(HWND hwnd)
{
    const struct toolkit *tk = NULL;
    struct bridged_window *win;
    void *bar = NULL, **items;
    HMENU hbar;
    UINT i, count;

    if (GetMenu(hwnd) || find_bridged_window(hwnd)) return;

    for (i = 0; i < ARRAY_SIZE(toolkits) && !bar; i++)
        if ((bar = toolkits[i]->find_menu_bar(hwnd))) tk = toolkits[i];
    if (!bar) return;

    if (!(items = tk->get_items(bar, &count))) return;
    tk->free_items(items, count);
    if (!count) return;

    if (!(win = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*win)))) return;
    win->hwnd = hwnd;
    win->toolkit = tk;

    hbar = CreateMenu();
    fill_menu(win, hbar, bar, 0);
    if (!GetMenuItemCount(hbar))
    {
        DestroyMenu(hbar);
        HeapFree(GetProcessHeap(), 0, win->items);
        HeapFree(GetProcessHeap(), 0, win->popups);
        HeapFree(GetProcessHeap(), 0, win);
        return;
    }

    TRACE("win %p: %s menu bar %p, %u items\n", hwnd, tk->name, bar, GetMenuItemCount(hbar));
    win->old_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)bridged_window_proc);
    win->next = bridged_windows;
    bridged_windows = win;

    tk->hide_menu_bar(bar);
    SetMenu(hwnd, hbar);
}


/***********************************************************************
 *              macdrv_toolkit_menu
 *
 * Callback for macdrv_toolkit_menu_check (menu.c).
 */
NTSTATUS WINAPI macdrv_toolkit_menu(void *arg, ULONG size)
{
    struct toolkit_menu_params *params = arg;
    HWND hwnd = UlongToHandle(params->hwnd);

    EnterCriticalSection(&bridge_cs);
    bridge_window(hwnd);
    LeaveCriticalSection(&bridge_cs);
    return STATUS_SUCCESS;
}
