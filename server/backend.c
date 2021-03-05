/* nbdkit
 * Copyright (C) 2013-2021 Red Hat Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>

#include <dlfcn.h>

#include "ascii-ctype.h"
#include "minmax.h"

#include "internal.h"

/* Helpers for registering a new backend. */

/* Use:
 * -D nbdkit.backend.controlpath=0 to suppress control path debugging.
 * -D nbdkit.backend.datapath=0 to suppress data path debugging.
 */
SERVER_DEBUG_FLAG(nbdkit_debug_backend_controlpath) = 1;
SERVER_DEBUG_FLAG(nbdkit_debug_backend_datapath) = 1;

#define controlpath_debug(fs, ...)                                     \
  do {                                                                 \
    if (nbdkit_debug_backend_controlpath) debug ((fs), ##__VA_ARGS__); \
  } while (0)
#define datapath_debug(fs, ...)                                        \
  do {                                                                 \
    if (nbdkit_debug_backend_datapath) debug ((fs), ##__VA_ARGS__);    \
  } while (0)

void
backend_init (struct backend *b, struct backend *next, size_t index,
              const char *filename, void *dl, const char *type)
{
  b->next = next;
  b->i = index;
  b->type = type;
  b->filename = strdup (filename);
  if (b->filename == NULL) {
    perror ("strdup");
    exit (EXIT_FAILURE);
  }
  b->dl = dl;

  debug ("registering %s %s", type, filename);
}

void
backend_load (struct backend *b, const char *name, void (*load) (void))
{
  size_t i, len;

  /* name is required. */
  if (name == NULL) {
    fprintf (stderr, "%s: %s: %s must have a .name field\n",
             program_name, b->filename, b->type);
    exit (EXIT_FAILURE);
  }

  len = strlen (name);
  if (len == 0) {
    fprintf (stderr, "%s: %s: %s.name field must not be empty\n",
             program_name, b->filename, b->type);
    exit (EXIT_FAILURE);
  }
  if (! ascii_isalnum (*name)) {
    fprintf (stderr,
             "%s: %s: %s.name ('%s') field must begin with an "
             "ASCII alphanumeric character\n",
             program_name, b->filename, b->type, name);
    exit (EXIT_FAILURE);
  }
  for (i = 1; i < len; ++i) {
    unsigned char c = name[i];

    if (! ascii_isalnum (c) && c != '-') {
      fprintf (stderr,
               "%s: %s: %s.name ('%s') field must contain only "
               "ASCII alphanumeric or dash characters\n",
               program_name, b->filename, b->type, name);
      exit (EXIT_FAILURE);
    }
  }

  /* Copy the module's name into local storage, so that name
   * survives past unload.
   */
  b->name = strdup (name);
  if (b->name == NULL) {
    perror ("strdup");
    exit (EXIT_FAILURE);
  }

  debug ("registered %s %s (name %s)", b->type, b->filename, b->name);

  /* Apply debug flags before calling load. */
  apply_debug_flags (b->dl, name);

  /* Call the on-load callback if it exists. */
  controlpath_debug ("%s: load", name);
  if (load)
    load ();
}

void
backend_unload (struct backend *b, void (*unload) (void))
{
  /* Acquiring this lock prevents any other backend callbacks from running
   * simultaneously.
   */
  lock_unload ();

  controlpath_debug ("%s: unload %s", b->name, b->type);
  if (unload)
    unload ();

  if (DO_DLCLOSE)
    dlclose (b->dl);
  free (b->filename);

  unlock_unload ();

  free (b->name);
}

int
backend_list_exports (struct backend *b, int readonly,
                      struct nbdkit_exports *exports)
{
  GET_CONN;
  struct context *c = get_context (conn, b);
  size_t count;

  controlpath_debug ("%s: list_exports readonly=%d tls=%d",
                     b->name, readonly, conn->using_tls);

  assert (c == NULL);

  if (b->list_exports (b, readonly, conn->using_tls, exports) == -1 ||
      exports_resolve_default (exports, b, readonly) == -1) {
    controlpath_debug ("%s: list_exports failed", b->name);
    return -1;
  }

  count = nbdkit_exports_count (exports);
  controlpath_debug ("%s: list_exports returned %zu names", b->name, count);
  return 0;
}

const char *
backend_default_export (struct backend *b, int readonly)
{
  GET_CONN;
  struct context *c = get_context (conn, b);
  const char *s;

  controlpath_debug ("%s: default_export readonly=%d tls=%d",
                     b->name, readonly, conn->using_tls);

  if (conn->default_exportname[b->i] == NULL) {
    assert (c == NULL);
    s = b->default_export (b, readonly, conn->using_tls);
    /* Ignore over-length strings. XXX Also ignore non-UTF8? */
    if (s && strnlen (s, NBD_MAX_STRING + 1) > NBD_MAX_STRING) {
      controlpath_debug ("%s: default_export: ignoring invalid string",
                         b->name);
      s = NULL;
    }
    if (s) {
      /* Best effort caching */
      conn->default_exportname[b->i] = strdup (s);
      if (conn->default_exportname[b->i] == NULL)
        return s;
    }
  }
  return conn->default_exportname[b->i];
}

static struct nbdkit_next_ops next_ops = {
  .export_description = backend_export_description,
  .get_size = backend_get_size,
  .can_write = backend_can_write,
  .can_flush = backend_can_flush,
  .is_rotational = backend_is_rotational,
  .can_trim = backend_can_trim,
  .can_zero = backend_can_zero,
  .can_fast_zero = backend_can_fast_zero,
  .can_extents = backend_can_extents,
  .can_fua = backend_can_fua,
  .can_multi_conn = backend_can_multi_conn,
  .can_cache = backend_can_cache,
  .pread = backend_pread,
  .pwrite = backend_pwrite,
  .flush = backend_flush,
  .trim = backend_trim,
  .zero = backend_zero,
  .extents = backend_extents,
  .cache = backend_cache,
};

struct context *
backend_open (struct backend *b, int readonly, const char *exportname)
{
  GET_CONN;
  struct context *c = malloc (sizeof *c);

  if (c == NULL) {
    nbdkit_error ("malloc: %m");
    return NULL;
  }

  controlpath_debug ("%s: open readonly=%d exportname=\"%s\" tls=%d",
                     b->name, readonly, exportname, conn->using_tls);

  assert (conn->contexts[b->i] == NULL);
  c->next = next_ops;
  c->handle = NULL;
  c->b = b;
  c->state = 0;
  c->exportsize = -1;
  c->can_write = readonly ? 0 : -1;
  c->can_flush = -1;
  c->is_rotational = -1;
  c->can_trim = -1;
  c->can_zero = -1;
  c->can_fast_zero = -1;
  c->can_fua = -1;
  c->can_multi_conn = -1;
  c->can_extents = -1;
  c->can_cache = -1;

  /* Determine the canonical name for default export */
  if (!*exportname) {
    exportname = backend_default_export (b, readonly);
    if (exportname == NULL) {
      nbdkit_error ("default export (\"\") not permitted");
      free (c);
      return NULL;
    }
  }

  /* Most filters will call next_open first, resulting in
   * inner-to-outer ordering.
   */
  c->handle = b->open (b, readonly, exportname, conn->using_tls);
  controlpath_debug ("%s: open returned handle %p", b->name, c->handle);

  if (c->handle == NULL) {
    if (b->i) { /* Do not strand backend if this layer failed */
      struct context *c2 = get_context (conn, b->next);
      if (c2 != NULL)
        backend_close (c2);
    }
    free (c);
    return NULL;
  }

  c->state |= HANDLE_OPEN;
  return c;
}

int
backend_prepare (struct context *c)
{
  GET_CONN;
  struct backend *b = c->b;

  assert (c->handle);
  assert ((c->state & (HANDLE_OPEN | HANDLE_CONNECTED)) == HANDLE_OPEN);

  /* Call these in order starting from the filter closest to the
   * plugin, similar to typical .open order.  But remember that
   * a filter may skip opening its backend.
   */
  if (b->i) {
    struct context *c2 = get_context (conn, b->next);
    if (c2 != NULL && backend_prepare (c2) == -1)
      return -1;
  }

  controlpath_debug ("%s: prepare readonly=%d", b->name, c->can_write == 0);

  if (b->prepare (c, c->can_write == 0) == -1)
    return -1;
  c->state |= HANDLE_CONNECTED;
  return 0;
}

int
backend_finalize (struct context *c)
{
  GET_CONN;
  struct backend *b = c->b;

  /* Call these in reverse order to .prepare above, starting from the
   * filter furthest away from the plugin, and matching .close order.
   */

  /* Once finalize fails, we can do nothing further on this connection */
  if (c->state & HANDLE_FAILED)
    return -1;

  if (c->state & HANDLE_CONNECTED) {
    assert (c->state & HANDLE_OPEN && c->handle);
    controlpath_debug ("%s: finalize", b->name);
    if (b->finalize (c) == -1) {
      c->state |= HANDLE_FAILED;
      return -1;
    }
  }

  if (b->i) {
    struct context *c2 = get_context (conn, b->next);
    if (c2 != NULL)
      return backend_finalize (c2);
  }
  return 0;
}

void
backend_close (struct context *c)
{
  GET_CONN;
  struct backend *b = c->b;

  /* outer-to-inner order, opposite .open */
  assert (c->handle);
  assert (c->state & HANDLE_OPEN);
  controlpath_debug ("%s: close", b->name);
  b->close (c);
  free (c);
  set_context (conn, b, NULL);
  if (b->i) {
    struct context *c2 = get_context (conn, b->next);
    if (c2 != NULL)
      backend_close (c2);
  }
}

bool
backend_valid_range (struct context *c, uint64_t offset, uint32_t count)
{
  assert (c->exportsize <= INT64_MAX); /* Guaranteed by negotiation phase */
  return count > 0 && offset <= c->exportsize &&
    offset + count <= c->exportsize;
}

/* Core functionality of nbdkit_backend_reopen for retry filter */

int
backend_reopen (struct backend *b, int readonly, const char *exportname)
{
  GET_CONN;
  struct context *c;

  controlpath_debug ("%s: reopen readonly=%d exportname=\"%s\"",
                     b->name, readonly, exportname);

  c = get_context (conn, b);
  if (c) {
    if (backend_finalize (c) == -1)
      return -1;
    backend_close (c);
  }
  c = backend_open (b, readonly, exportname);
  if (c == NULL)
    return -1;
  set_context (conn, b, c);
  if (backend_prepare (c) == -1) {
    backend_finalize (c);
    backend_close (c);
    return -1;
  }
  return 0;
}

/* Wrappers for all callbacks in a filter's struct nbdkit_next_ops. */

const char *
backend_export_description (struct context *c)
{
  struct backend *b = c->b;
  const char *s;

  controlpath_debug ("%s: export_description", b->name);

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  /* Caching is not useful for this value. */
  s = b->export_description (c);

  /* Ignore over-length strings. XXX Also ignore non-UTF8? */
  if (s && strnlen (s, NBD_MAX_STRING + 1) > NBD_MAX_STRING) {
    controlpath_debug ("%s: export_description: ignoring invalid string",
                       b->name);
    s = NULL;
  }
  return s;
}

int64_t
backend_get_size (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->exportsize == -1) {
    controlpath_debug ("%s: get_size", b->name);
    c->exportsize = b->get_size (c);
  }
  return c->exportsize;
}

int
backend_can_write (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_write == -1) {
    controlpath_debug ("%s: can_write", b->name);
    c->can_write = b->can_write (c);
  }
  return c->can_write;
}

int
backend_can_flush (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_flush == -1) {
    controlpath_debug ("%s: can_flush", b->name);
    c->can_flush = b->can_flush (c);
  }
  return c->can_flush;
}

int
backend_is_rotational (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->is_rotational == -1) {
    controlpath_debug ("%s: is_rotational", b->name);
    c->is_rotational = b->is_rotational (c);
  }
  return c->is_rotational;
}

int
backend_can_trim (struct context *c)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_trim == -1) {
    controlpath_debug ("%s: can_trim", b->name);
    r = backend_can_write (c);
    if (r != 1) {
      c->can_trim = 0;
      return r;
    }
    c->can_trim = b->can_trim (c);
  }
  return c->can_trim;
}

int
backend_can_zero (struct context *c)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_zero == -1) {
    controlpath_debug ("%s: can_zero", b->name);
    r = backend_can_write (c);
    if (r != 1) {
      c->can_zero = NBDKIT_ZERO_NONE;
      return r; /* Relies on 0 == NBDKIT_ZERO_NONE */
    }
    c->can_zero = b->can_zero (c);
  }
  return c->can_zero;
}

int
backend_can_fast_zero (struct context *c)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_fast_zero == -1) {
    controlpath_debug ("%s: can_fast_zero", b->name);
    r = backend_can_zero (c);
    if (r < NBDKIT_ZERO_EMULATE) {
      c->can_fast_zero = 0;
      return r; /* Relies on 0 == NBDKIT_ZERO_NONE */
    }
    c->can_fast_zero = b->can_fast_zero (c);
  }
  return c->can_fast_zero;
}

int
backend_can_extents (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_extents == -1) {
    controlpath_debug ("%s: can_extents", b->name);
    c->can_extents = b->can_extents (c);
  }
  return c->can_extents;
}

int
backend_can_fua (struct context *c)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_fua == -1) {
    controlpath_debug ("%s: can_fua", b->name);
    r = backend_can_write (c);
    if (r != 1) {
      c->can_fua = NBDKIT_FUA_NONE;
      return r; /* Relies on 0 == NBDKIT_FUA_NONE */
    }
    c->can_fua = b->can_fua (c);
  }
  return c->can_fua;
}

int
backend_can_multi_conn (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_multi_conn == -1) {
    controlpath_debug ("%s: can_multi_conn", b->name);
    c->can_multi_conn = b->can_multi_conn (c);
  }
  return c->can_multi_conn;
}

int
backend_can_cache (struct context *c)
{
  struct backend *b = c->b;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  if (c->can_cache == -1) {
    controlpath_debug ("%s: can_cache", b->name);
    c->can_cache = b->can_cache (c);
  }
  return c->can_cache;
}

int
backend_pread (struct context *c,
               void *buf, uint32_t count, uint64_t offset,
               uint32_t flags, int *err)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (backend_valid_range (c, offset, count));
  assert (flags == 0);
  datapath_debug ("%s: pread count=%" PRIu32 " offset=%" PRIu64,
                  b->name, count, offset);

  r = b->pread (c, buf, count, offset, flags, err);
  if (r == -1)
    assert (*err);
  return r;
}

int
backend_pwrite (struct context *c,
                const void *buf, uint32_t count, uint64_t offset,
                uint32_t flags, int *err)
{
  struct backend *b = c->b;
  bool fua = !!(flags & NBDKIT_FLAG_FUA);
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (c->can_write == 1);
  assert (backend_valid_range (c, offset, count));
  assert (!(flags & ~NBDKIT_FLAG_FUA));
  if (fua)
    assert (c->can_fua > NBDKIT_FUA_NONE);
  datapath_debug ("%s: pwrite count=%" PRIu32 " offset=%" PRIu64 " fua=%d",
                  b->name, count, offset, fua);

  r = b->pwrite (c, buf, count, offset, flags, err);
  if (r == -1)
    assert (*err);
  return r;
}

int
backend_flush (struct context *c,
               uint32_t flags, int *err)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (c->can_flush == 1);
  assert (flags == 0);
  datapath_debug ("%s: flush", b->name);

  r = b->flush (c, flags, err);
  if (r == -1)
    assert (*err);
  return r;
}

int
backend_trim (struct context *c,
              uint32_t count, uint64_t offset, uint32_t flags,
              int *err)
{
  struct backend *b = c->b;
  bool fua = !!(flags & NBDKIT_FLAG_FUA);
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (c->can_write == 1);
  assert (c->can_trim == 1);
  assert (backend_valid_range (c, offset, count));
  assert (!(flags & ~NBDKIT_FLAG_FUA));
  if (fua)
    assert (c->can_fua > NBDKIT_FUA_NONE);
  datapath_debug ("%s: trim count=%" PRIu32 " offset=%" PRIu64 " fua=%d",
                  b->name, count, offset, fua);

  r = b->trim (c, count, offset, flags, err);
  if (r == -1)
    assert (*err);
  return r;
}

int
backend_zero (struct context *c,
              uint32_t count, uint64_t offset, uint32_t flags,
              int *err)
{
  struct backend *b = c->b;
  bool fua = !!(flags & NBDKIT_FLAG_FUA);
  bool fast = !!(flags & NBDKIT_FLAG_FAST_ZERO);
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (c->can_write == 1);
  assert (c->can_zero > NBDKIT_ZERO_NONE);
  assert (backend_valid_range (c, offset, count));
  assert (!(flags & ~(NBDKIT_FLAG_MAY_TRIM | NBDKIT_FLAG_FUA |
                      NBDKIT_FLAG_FAST_ZERO)));
  if (fua)
    assert (c->can_fua > NBDKIT_FUA_NONE);
  if (fast)
    assert (c->can_fast_zero == 1);
  datapath_debug ("%s: zero count=%" PRIu32 " offset=%" PRIu64
                  " may_trim=%d fua=%d fast=%d",
                  b->name, count, offset,
                  !!(flags & NBDKIT_FLAG_MAY_TRIM), fua, fast);

  r = b->zero (c, count, offset, flags, err);
  if (r == -1) {
    assert (*err);
    if (!fast)
      assert (*err != ENOTSUP && *err != EOPNOTSUPP);
  }
  return r;
}

int
backend_extents (struct context *c,
                 uint32_t count, uint64_t offset, uint32_t flags,
                 struct nbdkit_extents *extents, int *err)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (c->can_extents >= 0);
  assert (backend_valid_range (c, offset, count));
  assert (!(flags & ~NBDKIT_FLAG_REQ_ONE));
  datapath_debug ("%s: extents count=%" PRIu32 " offset=%" PRIu64 " req_one=%d",
                  b->name, count, offset, !!(flags & NBDKIT_FLAG_REQ_ONE));

  if (c->can_extents == 0) {
    /* By default it is safe assume that everything in the range is
     * allocated.
     */
    r = nbdkit_add_extent (extents, offset, count, 0 /* allocated data */);
    if (r == -1)
      *err = errno;
    return r;
  }
  r = b->extents (c, count, offset, flags, extents, err);
  if (r == -1)
    assert (*err);
  return r;
}

int
backend_cache (struct context *c,
               uint32_t count, uint64_t offset,
               uint32_t flags, int *err)
{
  struct backend *b = c->b;
  int r;

  assert (c->handle && (c->state & HANDLE_CONNECTED));
  assert (c->can_cache > NBDKIT_CACHE_NONE);
  assert (backend_valid_range (c, offset, count));
  assert (flags == 0);
  datapath_debug ("%s: cache count=%" PRIu32 " offset=%" PRIu64,
                  b->name, count, offset);

  if (c->can_cache == NBDKIT_CACHE_EMULATE) {
    static char buf[MAX_REQUEST_SIZE]; /* data sink, never read */
    uint32_t limit;

    while (count) {
      limit = MIN (count, sizeof buf);
      if (backend_pread (c, buf, limit, offset, flags, err) == -1)
        return -1;
      count -= limit;
    }
    return 0;
  }
  r = b->cache (c, count, offset, flags, err);
  if (r == -1)
    assert (*err);
  return r;
}
