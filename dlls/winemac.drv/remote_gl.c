/*
 * WINESKIN: OpenGL in a child window of another process's window
 *
 * The Unix side of cocoa_remote_gl.m: a client surface whose top-level window belongs to another
 * process gets an off-screen drawable and a layer the owner hosts at the child's place.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include "macdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv);

static void remote_gl_release(struct macdrv_client_surface *surface)
{
    if (!surface->remote) return;
    TRACE("%s toplevel %p\n", debugstr_client_surface(&surface->client), surface->remote_toplevel);
    macdrv_release_remote_layer(surface->remote_toplevel, macdrv_remote_gl_context_id(surface->remote));
    macdrv_remote_gl_destroy(surface->remote);
    surface->remote = NULL;
    surface->remote_toplevel = 0;
}

/***********************************************************************
 *              macdrv_remote_gl_update
 *
 * Called for a client surface whose top-level window has no data in this process.
 */
void macdrv_remote_gl_update(struct macdrv_client_surface *surface)
{
    struct client_surface *client = &surface->client;
    DWORD pid = 0;
    int width, height;

    if (client->toplevel == client->hwnd || !NtUserGetWindowThread(client->toplevel, &pid) ||
        pid == HandleToULong(NtCurrentTeb()->ClientId.UniqueProcess))
    {
        remote_gl_release(surface);
        return;
    }

    if (surface->remote && surface->remote_toplevel != client->toplevel) remote_gl_release(surface);
    if (!surface->remote)
    {
        if (!(surface->remote = macdrv_remote_gl_create()))
        {
            WARN("no remote drawable for %s\n", debugstr_client_surface(client));
            return;
        }
        surface->remote_toplevel = client->toplevel;
        TRACE("%s toplevel %p (pid %04x) context id %u\n", debugstr_client_surface(client), client->toplevel,
              (int)pid, macdrv_remote_gl_context_id(surface->remote));
        macdrv_create_remote_layer(client->toplevel, macdrv_remote_gl_context_id(surface->remote));
    }

    width = client->virtual_rect.right - client->virtual_rect.left;
    height = client->virtual_rect.bottom - client->virtual_rect.top;
    TRACE("%s monitor_rect %s size %dx%d\n", debugstr_client_surface(client),
          wine_dbgstr_rect(&client->monitor_rect), width, height);
    macdrv_remote_gl_set_geometry(surface->remote, cgrect_from_rect(client->monitor_rect), width, height,
                                  !NtUserIsWindowVisible(client->hwnd));
}

/***********************************************************************
 *              macdrv_remote_gl_detach
 */
void macdrv_remote_gl_detach(struct macdrv_client_surface *surface)
{
    remote_gl_release(surface);
}
