/*
 * In-process synchronization objects on msync
 *
 * WINESKIN: the macOS backend of inproc_sync.c, which includes this file where the Linux build has
 * its ntsync backend. It follows the "#elif defined(__APPLE__)" block of CrossOver 26.3.0's
 * server/inproc_sync.c (CodeWeavers' wine sources, LGPL 2.1+): an in-process sync object is an
 * msync object (msync.c), and what a client is given for it is the object's index in msync's shared
 * memory instead of a file descriptor.
 *
 * The index travels in fields the protocol already has, so that the protocol is upstream's: in the
 * type of a get_inproc_sync_fd reply, above its MSYNC_INPROC_TYPE_BITS low bits, and as the handle of
 * a get_inproc_alert_fd reply. A process learns that the server runs msync from being sent an
 * in-process device at all (a descriptor for /dev/null).
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

#include <fcntl.h>

#include "msync.h"

#define MSYNC_INPROC_TYPE_BITS 4

int get_inproc_device_fd(void)
{
    static int fd = -2;

    if (!do_msync()) return -1;
    if (fd == -2)
    {
        /* the first thread of the first process is being created: the server is in its final process */
        msync_init_shm();
        msync_init();
        fd = open( "/dev/null", O_CLOEXEC | O_RDONLY );
    }
    return fd;
}

struct inproc_sync
{
    struct object          obj;
    enum inproc_sync_type  type;
    struct msync          *msync;
};

static void inproc_sync_dump( struct object *obj, int verbose );
static int inproc_sync_signal( struct object *obj, unsigned int access, int signal );
static void inproc_sync_destroy( struct object *obj );

static const struct object_ops inproc_sync_ops =
{
    .size    = sizeof(struct inproc_sync),
    .type    = &no_type,
    .dump    = inproc_sync_dump,
    .signal  = inproc_sync_signal,
    .destroy = inproc_sync_destroy,
};

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    if (!sync) return -1;
    if (do_msync()) return (int)sync->msync->shm_idx;
    return -1;
}

static struct inproc_sync *create_inproc_msync( enum inproc_sync_type type, int low, int high,
                                                enum msync_type msync_type )
{
    struct inproc_sync *sync;

    if (!(sync = alloc_object( &inproc_sync_ops ))) return NULL;
    sync->type = type;
    if (!(sync->msync = create_msync( low, high, msync_type )))
    {
        set_error( STATUS_NO_MEMORY );
        release_object( sync );
        return NULL;
    }
    return sync;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    return create_inproc_msync( INPROC_SYNC_INTERNAL, signaled, 0xdeadbeef,
                                manual ? MSYNC_MANUAL_SERVER : MSYNC_AUTO_SERVER );
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    return create_inproc_msync( INPROC_SYNC_EVENT, signaled, 0xdeadbeef,
                                manual ? MSYNC_MANUAL_EVENT : MSYNC_AUTO_EVENT );
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    return create_inproc_msync( INPROC_SYNC_MUTEX, owner, count, MSYNC_MUTEX );
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    return create_inproc_msync( INPROC_SYNC_SEMAPHORE, initial, max, MSYNC_SEMAPHORE );
}

static void inproc_sync_dump( struct object *obj, int verbose )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    fprintf( stderr, "Inproc sync type=%d, msync idx=%d\n", sync->type, sync->msync ? sync->msync->shm_idx : 0 );
}

void signal_inproc_sync( struct inproc_sync *sync )
{
    if (debug_level) fprintf( stderr, "set_inproc_event %d\n", sync->msync->shm_idx );
    msync_set_event( sync->msync );
}

void reset_inproc_sync( struct inproc_sync *sync )
{
    if (debug_level) fprintf( stderr, "reset_inproc_event %d\n", sync->msync->shm_idx );
    msync_reset_event( sync->msync );
}

static int inproc_sync_signal( struct object *obj, unsigned int access, int signal )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );

    assert( sync->type == INPROC_SYNC_INTERNAL || sync->type == INPROC_SYNC_EVENT ); /* never called for mutex / semaphore */
    assert( signal == 0 || signal == 1 ); /* never called from signal_object */

    if (signal) signal_inproc_sync( sync );
    else reset_inproc_sync( sync );
    return 1;
}

static void inproc_sync_destroy( struct object *obj )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    if (sync->msync) msync_destroy( sync->msync );
}

void abandon_inproc_mutexes( thread_id_t tid )
{
    if (do_msync()) msync_abandon_mutexes( tid );
}

/* returns the object's shared memory index, and sets the type the reply carries: the sync type with
 * the index above it */
static int get_obj_inproc_sync( struct object *obj, int *type )
{
    struct object *sync;
    int shm_idx = -1;

    if (!do_msync()) return -1;
    if (!(sync = get_obj_sync( obj ))) return -1;
    if (sync->ops == &inproc_sync_ops)
    {
        struct inproc_sync *inproc = (struct inproc_sync *)sync;

        /* the client's reference, which its msync_close() gives back */
        msync_grab_object( inproc->msync );
        shm_idx = (int)inproc->msync->shm_idx;
        *type = inproc->type | (shm_idx << MSYNC_INPROC_TYPE_BITS);
    }

    release_object( sync );
    return shm_idx;
}
