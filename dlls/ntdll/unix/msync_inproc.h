/*
 * In-process synchronization objects on msync
 *
 * WINESKIN: the macOS backend of sync.c's in-process synchronization, which includes this file where
 * the Linux build has its ntsync backend; it follows the "#elif defined(__APPLE__)" block of
 * CrossOver 26.3.0's dlls/ntdll/unix/sync.c (CodeWeavers' wine sources, LGPL 2.1+). The "fd" of an
 * in-process sync object is its index in msync's shared memory (msync.c). The server sends it in
 * fields the protocol already has: in the type of a get_inproc_sync_fd reply, above its
 * MSYNC_INPROC_TYPE_BITS low bits, and as the handle of a get_inproc_alert_fd reply. The alert
 * object's index is kept here, not in the thread data's alert_fd, which thread exit closes as a file
 * descriptor.
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

#include "msync.h"

#define MSYNC_INPROC_TYPE_BITS 4

static inline int msync_inproc_reply_obj( int reply_type )
{
    return reply_type >> MSYNC_INPROC_TYPE_BITS;
}

static inline enum inproc_sync_type msync_inproc_reply_type( int reply_type )
{
    return reply_type & ((1 << MSYNC_INPROC_TYPE_BITS) - 1);
}

/* the msync object the server signals when the current thread has a user APC */
static int msync_inproc_alert_obj(void)
{
    static __thread int alert_obj;

    if (!alert_obj)
    {
        SERVER_START_REQ( get_inproc_alert_fd )
        {
            if (!wine_server_call( req )) alert_obj = reply->handle;
        }
        SERVER_END_REQ;
    }
    return alert_obj;
}

static NTSTATUS linux_release_semaphore_obj( int obj, ULONG count, ULONG *prev_count )
{
    return msync_release_semaphore_obj( obj, count, prev_count );
}

static NTSTATUS linux_query_semaphore_obj( int obj, SEMAPHORE_BASIC_INFORMATION *info )
{
    return msync_query_semaphore_obj( obj, info );
}

static NTSTATUS linux_set_event_obj( int obj, LONG *prev_state )
{
    return msync_set_event_obj( obj, prev_state );
}

static NTSTATUS linux_reset_event_obj( int obj, LONG *prev_state )
{
    return msync_reset_event_obj( obj, prev_state );
}

static NTSTATUS linux_pulse_event_obj( int obj, LONG *prev_state )
{
    return msync_pulse_event_obj( obj, prev_state );
}

static NTSTATUS linux_query_event_obj( int obj, EVENT_BASIC_INFORMATION *info )
{
    return msync_query_event_obj( obj, info );
}

static NTSTATUS linux_release_mutex_obj( int obj, LONG *prev_count )
{
    return msync_release_mutex_obj( obj, prev_count );
}

static NTSTATUS linux_query_mutex_obj( int obj, MUTANT_BASIC_INFORMATION *info )
{
    return msync_query_mutex_obj( obj, info );
}

static NTSTATUS linux_wait_objs( int device, DWORD count, const int *objs, WAIT_TYPE type,
                                 int alert_fd, const LARGE_INTEGER *timeout )
{
    return msync_wait_objs( count, objs, type != WaitAll, alert_fd, timeout );
}
