/* nbdkit
 * Copyright (C) 2017-2019 Red Hat Inc.
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

#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <poll.h>

#include <libnbd.h>

#define NBDKIT_API_VERSION 2

#include <nbdkit-plugin.h>
#include "byte-swapping.h"
#include "cleanup.h"

/* The per-transaction details */
struct transaction {
  int64_t cookie;
  sem_t sem;
  uint32_t err;
  struct transaction *next;
};

/* The per-connection handle */
struct handle {
  /* These fields are read-only once initialized */
  struct nbd_handle *nbd;
  int fd; /* Cache of nbd_aio_get_fd */
  int fds[2]; /* Pipe for kicking the reader thread */
  bool readonly;
  pthread_t reader;

  pthread_mutex_t trans_lock; /* Covers access to trans list */
  struct transaction *trans; /* List of pending transactions */
};

/* Connect to server via absolute name of Unix socket */
static char *sockname;

/* Connect to server via TCP socket */
static const char *hostname;
static const char *port;

/* Connect to server via URI */
static const char *uri;

/* Name of export on remote server, default '', ignored for oldstyle */
static const char *export;

/* Number of retries */
static unsigned long retry;

/* True to share single server connection among all clients */
static bool shared;
static struct handle *shared_handle;

/* Control TLS settings */
static int tls = -1;
static char *tls_certificates;
static int tls_verify = -1;
static const char *tls_username;
static char *tls_psk;

static struct handle *nbdplug_open_handle (int readonly);
static void nbdplug_close_handle (struct handle *h);

static void
nbdplug_unload (void)
{
  if (shared)
    nbdplug_close_handle (shared_handle);
  free (sockname);
  free (tls_certificates);
  free (tls_psk);
}

/* Called for each key=value passed on the command line.  This plugin
 * accepts socket=<sockname>, hostname=<hostname>/port=<port>, or
 * [uri=]<uri> (exactly one connection required), and optional
 * parameters export=<name>, retry=<n>, shared=<bool> and various
 * tls settings.
 */
static int
nbdplug_config (const char *key, const char *value)
{
  char *end;
  int r;

  if (strcmp (key, "socket") == 0) {
    /* See FILENAMES AND PATHS in nbdkit-plugin(3) */
    free (sockname);
    sockname = nbdkit_absolute_path (value);
    if (!sockname)
      return -1;
  }
  else if (strcmp (key, "hostname") == 0)
    hostname = value;
  else if (strcmp (key, "port") == 0)
    port = value;
  else if (strcmp (key, "uri") == 0)
    uri = value;
  else if (strcmp (key, "export") == 0)
    export = value;
  else if (strcmp (key, "retry") == 0) {
    errno = 0;
    retry = strtoul (value, &end, 0);
    if (value == end || errno) {
      nbdkit_error ("could not parse retry as integer (%s)", value);
      return -1;
    }
  }
  else if (strcmp (key, "shared") == 0) {
    r = nbdkit_parse_bool (value);
    if (r == -1)
      return -1;
    shared = r;
  }
  else if (strcmp (key, "tls") == 0) {
    if (strcasecmp (value, "require") == 0 ||
        strcasecmp (value, "required") == 0 ||
        strcasecmp (value, "force") == 0)
      tls = 2;
    else {
      tls = nbdkit_parse_bool (value);
      if (tls == -1)
        exit (EXIT_FAILURE);
    }
  }
  else if (strcmp (key, "tls-certificates") == 0) {
    free (tls_certificates);
    tls_certificates = nbdkit_absolute_path (value);
    if (!tls_certificates)
      return -1;
  }
  else if (strcmp (key, "tls-verify") == 0) {
    r = nbdkit_parse_bool (value);
    if (r == -1)
      return -1;
    tls_verify = r;
  }
  else if (strcmp (key, "tls-username") == 0)
    tls_username = value;
  else if (strcmp (key, "tls-psk") == 0) {
    free (tls_psk);
    tls_psk = nbdkit_absolute_path (value);
    if (!tls_psk)
      return -1;
  }
  else {
    nbdkit_error ("unknown parameter '%s'", key);
    return -1;
  }

  return 0;
}

/* Check the user passed exactly one socket description. */
static int
nbdplug_config_complete (void)
{
  if (sockname) {
    struct sockaddr_un sock;

    if (hostname || port) {
      nbdkit_error ("cannot mix Unix socket and TCP hostname/port parameters");
      return -1;
    }
    else if (uri) {
      nbdkit_error ("cannot mix Unix socket and URI parameters");
      return -1;
    }
    if (strlen (sockname) > sizeof sock.sun_path) {
      nbdkit_error ("socket file name too large");
      return -1;
    }
  }
  else if (hostname) {
    if (uri) {
      nbdkit_error ("cannot mix TCP hostname/port and URI parameters");
      return -1;
    }
    if (!port)
      port = "10809";
  }
  else if (uri) {
    struct nbd_handle *nbd = nbd_create ();

    if (!nbd) {
      nbdkit_error ("unable to query libnbd details: %s", nbd_get_error ());
      return -1;
    }
    if (!nbd_supports_uri (nbd)) {
      nbdkit_error ("libnbd was compiled without uri support");
      nbd_close (nbd);
      return -1;
    }
    nbd_close (nbd);
  } else {
    nbdkit_error ("must supply socket=, hostname= or uri= of external NBD server");
    return -1;
  }

  if (!export)
    export = "";

  if (tls == -1)
    tls = tls_certificates || tls_verify >= 0 || tls_username || tls_psk;
  if (tls > 0) {
    struct nbd_handle *nbd = nbd_create ();

    if (!nbd) {
      nbdkit_error ("unable to query libnbd details: %s", nbd_get_error ());
      return -1;
    }
    if (!nbd_supports_tls (nbd)) {
      nbdkit_error ("libnbd was compiled without tls support");
      nbd_close (nbd);
      return -1;
    }
    nbd_close (nbd);
  }

  if (shared && (shared_handle = nbdplug_open_handle (false)) == NULL)
    return -1;
  return 0;
}

#define nbdplug_config_help \
  "[uri=]<URI>            URI of an NBD socket to connect to (if supported).\n" \
  "socket=<SOCKNAME>      The Unix socket to connect to.\n" \
  "hostname=<HOST>        The hostname for the TCP socket to connect to.\n" \
  "port=<PORT>            TCP port or service name to use (default 10809).\n" \
  "export=<NAME>          Export name to connect to (default \"\").\n" \
  "retry=<N>              Retry connection up to N seconds (default 0).\n" \
  "shared=<BOOL>          True to share one server connection among all clients,\n" \
  "                       rather than a connection per client (default false).\n" \
  "tls=<MODE>             How to use TLS; one of 'off', 'on', or 'require'.\n" \
  "tls-certificates=<DIR> Directory containing files for X.509 certificates.\n" \
  "tls-verify=<BOOL>      True (default for X.509) to validate server.\n" \
  "tls-username=<NAME>    Override username presented in X.509 TLS.\n" \
  "tls-psk=<FILE>         File containing Pre-Shared Key for TLS.\n" \

static void
nbdplug_dump_plugin (void)
{
  struct nbd_handle *nbd = nbd_create ();

  if (!nbd) {
    nbdkit_error ("unable to query libnbd details: %s", nbd_get_error ());
    exit (EXIT_FAILURE);
  }
  printf ("libnbd_version=%s\n", nbd_get_version (nbd));
  printf ("libnbd_tls=%d\n", nbd_supports_tls (nbd));
  printf ("libnbd_uri=%d\n", nbd_supports_uri (nbd));
  nbd_close (nbd);
}

#define THREAD_MODEL NBDKIT_THREAD_MODEL_PARALLEL

/* Reader loop. */
void *
nbdplug_reader (void *handle)
{
  struct handle *h = handle;
  int r;

  while (!nbd_aio_is_dead (h->nbd) && !nbd_aio_is_closed (h->nbd)) {
    struct pollfd fds[2] = {
      [0].fd = h->fd,
      [1].fd = h->fds[0],
      [1].events = POLLIN,
    };
    struct transaction *trans, **prev;
    int dir;
    char c;

    dir = nbd_aio_get_direction (h->nbd);
    nbdkit_debug ("polling, dir=%d", dir);
    if (dir & LIBNBD_AIO_DIRECTION_READ)
      fds[0].events |= POLLIN;
    if (dir & LIBNBD_AIO_DIRECTION_WRITE)
      fds[0].events |= POLLOUT;
    if (poll (fds, 2, -1) == -1) {
      nbdkit_error ("poll: %m");
      break;
    }

    if (dir & LIBNBD_AIO_DIRECTION_READ && fds[0].revents & POLLIN)
      nbd_aio_notify_read (h->nbd);
    else if (dir & LIBNBD_AIO_DIRECTION_WRITE && fds[0].revents & POLLOUT)
      nbd_aio_notify_write (h->nbd);

    /* Check if we were kicked because a command was started */
    if (fds[1].revents & POLLIN && read (h->fds[0], &c, 1) != 1) {
      nbdkit_error ("failed to read pipe: %m");
      break;
    }

    ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
    trans = h->trans;
    prev = &h->trans;
    while (trans) {
      r = nbd_aio_command_completed (h->nbd, trans->cookie);
      if (r == -1) {
        nbdkit_debug ("transaction %" PRId64 " failed: %s", trans->cookie,
                      nbd_get_error ());
        trans->err = nbd_get_errno ();
        if (!trans->err)
          trans->err = EIO;
      }
      if (r) {
        nbdkit_debug ("cookie %" PRId64 " completed state machine, status %d",
                      trans->cookie, trans->err);
        *prev = trans->next;
        if (sem_post (&trans->sem)) {
          nbdkit_error ("failed to post semaphore: %m");
          abort ();
        }
      }
      else
        prev = &trans->next;
      trans = *prev;
    }
  }

  /* Clean up any stranded in-flight requests */
  nbdkit_debug ("state machine changed to %s", nbd_connection_state (h->nbd));
  while (1) {
    struct transaction *trans;

    {
      ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
      trans = h->trans;
      h->trans = trans ? trans->next : NULL;
    }
    if (!trans)
      break;
    r = nbd_aio_command_completed (h->nbd, trans->cookie);
    if (r == -1) {
      nbdkit_debug ("transaction %" PRId64 " failed: %s", trans->cookie,
                    nbd_get_error ());
      trans->err = nbd_get_errno ();
      if (!trans->err)
        trans->err = ESHUTDOWN;
    }
    else if (!r)
      trans->err = ESHUTDOWN;
    if (sem_post (&trans->sem)) {
      nbdkit_error ("failed to post semaphore: %m");
      abort ();
    }
  }
  nbdkit_debug ("exiting state machine thread");
  return NULL;
}

/* Register a cookie and return a transaction. */
static struct transaction *
nbdplug_register (struct handle *h, int64_t cookie)
{
  struct transaction *trans;
  char c = 0;

  if (cookie == -1) {
    nbdkit_error ("command failed: %s", nbd_get_error ());
    errno = nbd_get_errno ();
    return NULL;
  }

  nbdkit_debug ("cookie %" PRId64 " started by state machine", cookie);
  trans = calloc (1, sizeof *trans);
  if (!trans) {
    nbdkit_error ("unable to track transaction: %m");
    return NULL;
  }

  /* While locked, kick the reader thread and add our transaction */
  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
  if (write (h->fds[1], &c, 1) != 1) {
    nbdkit_error ("write to pipe: %m");
    free (trans);
    return NULL;
  }
  if (sem_init (&trans->sem, 0, 0)) {
    nbdkit_error ("unable to create semaphore: %m");
    free (trans);
    return NULL;
  }
  trans->cookie = cookie;
  trans->next = h->trans;
  h->trans = trans;
  return trans;
}

/* Perform the reply half of a transaction. */
static int
nbdplug_reply (struct handle *h, struct transaction *trans)
{
  int err;

  if (!trans) {
    assert (errno);
    return -1;
  }

  while ((err = sem_wait (&trans->sem)) == -1 && errno == EINTR)
    /* try again */;
  if (err) {
    nbdkit_debug ("failed to wait on semaphore: %m");
    err = EIO;
  }
  else
    err = trans->err;
  if (sem_destroy (&trans->sem))
    abort ();
  free (trans);
  errno = err;
  return err ? -1 : 0;
}

/* Create the shared or per-connection handle. */
static struct handle *
nbdplug_open_handle (int readonly)
{
  struct handle *h;
  int r;

  h = calloc (1, sizeof *h);
  if (h == NULL) {
    nbdkit_error ("malloc: %m");
    return NULL;
  }
  if (pipe (h->fds)) {
    nbdkit_error ("pipe: %m");
    free (h);
    return NULL;
  }

 retry:
  h->fd = -1;
  h->nbd = nbd_create ();
  if (!h->nbd)
    goto err;
  if (nbd_set_export_name (h->nbd, export) == -1)
    goto err;
  if (nbd_add_meta_context (h->nbd, "base:allocation") == -1)
    goto err;
  if (nbd_set_tls (h->nbd, tls) == -1)
    goto err;
  if (tls_certificates &&
      nbd_set_tls_certificates (h->nbd, tls_certificates) == -1)
    goto err;
  if (tls_verify >= 0 && nbd_set_tls_verify_peer (h->nbd, tls_verify) == -1)
    goto err;
  if (tls_username && nbd_set_tls_username (h->nbd, tls_username) == -1)
    goto err;
  if (tls_psk && nbd_set_tls_psk_file (h->nbd, tls_psk) == -1)
    goto err;
  if (uri)
    r = nbd_connect_uri (h->nbd, uri);
  else if (sockname)
    r = nbd_connect_unix (h->nbd, sockname);
  else
    r = nbd_connect_tcp (h->nbd, hostname, port);
  if (r == -1) {
    if (retry--) {
      nbdkit_debug ("connect failed; will try again: %s", nbd_get_error ());
      nbd_close (h->nbd);
      sleep (1);
      goto retry;
    }
    goto err;
  }
  h->fd = nbd_aio_get_fd (h->nbd);
  if (h->fd == -1)
    goto err;

  if (readonly)
    h->readonly = true;

  /* Spawn a dedicated reader thread */
  if ((errno = pthread_mutex_init (&h->trans_lock, NULL))) {
    nbdkit_error ("failed to initialize transaction mutex: %m");
    goto err;
  }
  if ((errno = pthread_create (&h->reader, NULL, nbdplug_reader, h))) {
    nbdkit_error ("failed to initialize reader thread: %m");
    pthread_mutex_destroy (&h->trans_lock);
    goto err;
  }

  return h;

 err:
  close (h->fds[0]);
  close (h->fds[1]);
  nbdkit_error ("failure while creating nbd handle: %s", nbd_get_error ());
  if (h->nbd)
    nbd_close (h->nbd);
  free (h);
  return NULL;
}

/* Create the per-connection handle. */
static void *
nbdplug_open (int readonly)
{
  if (shared)
    return shared_handle;
  return nbdplug_open_handle (readonly);
}

/* Free up the shared or per-connection handle. */
static void
nbdplug_close_handle (struct handle *h)
{
  if (nbd_shutdown (h->nbd) == -1)
    nbdkit_debug ("failed to clean up handle: %s", nbd_get_error ());
  if ((errno = pthread_join (h->reader, NULL)))
    nbdkit_debug ("failed to join reader thread: %m");
  close (h->fds[0]);
  close (h->fds[1]);
  nbd_close (h->nbd);
  pthread_mutex_destroy (&h->trans_lock);
  free (h);
}

/* Free up the per-connection handle. */
static void
nbdplug_close (void *handle)
{
  struct handle *h = handle;

  if (!shared)
    nbdplug_close_handle (h);
}



/* Get the file size. */
static int64_t
nbdplug_get_size (void *handle)
{
  struct handle *h = handle;
  int64_t size = nbd_get_size (h->nbd);

  if (size == -1) {
    nbdkit_error ("failure to get size: %s", nbd_get_error ());
    return -1;
  }
  return size;
}

static int
nbdplug_can_write (void *handle)
{
  struct handle *h = handle;
  int i = nbd_read_only (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check readonly flag: %s", nbd_get_error ());
    return -1;
  }
  return !(i || h->readonly);
}

static int
nbdplug_can_flush (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_flush (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check flush flag: %s", nbd_get_error ());
    return -1;
  }
  return i;
}

static int
nbdplug_is_rotational (void *handle)
{
  struct handle *h = handle;
  int i = nbd_is_rotational (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check rotational flag: %s", nbd_get_error ());
    return -1;
  }
  return i;
}

static int
nbdplug_can_trim (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_trim (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check trim flag: %s", nbd_get_error ());
    return -1;
  }
  return i;
}

static int
nbdplug_can_zero (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_zero (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check zero flag: %s", nbd_get_error ());
    return -1;
  }
  return i;
}

static int
nbdplug_can_fua (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_fua (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check fua flag: %s", nbd_get_error ());
    return -1;
  }
  return i ? NBDKIT_FUA_NATIVE : NBDKIT_FUA_NONE;
}

static int
nbdplug_can_multi_conn (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_multi_conn (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check multi-conn flag: %s", nbd_get_error ());
    return -1;
  }
  return i;
}

static int
nbdplug_can_cache (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_cache (h->nbd);

  if (i == -1) {
    nbdkit_error ("failure to check cache flag: %s", nbd_get_error ());
    return -1;
  }
  return i ? NBDKIT_CACHE_NATIVE : NBDKIT_CACHE_NONE;
}

static int
nbdplug_can_extents (void *handle)
{
  struct handle *h = handle;
  int i = nbd_can_meta_context (h->nbd, "base:allocation");

  if (i == -1) {
    nbdkit_error ("failure to check extents ability: %s", nbd_get_error ());
    return -1;
  }
  return i;
}

/* Read data from the file. */
static int
nbdplug_pread (void *handle, void *buf, uint32_t count, uint64_t offset,
               uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!flags);
  s = nbdplug_register (h, nbd_aio_pread (h->nbd, buf, count, offset, 0));
  return nbdplug_reply (h, s);
}

/* Write data to the file. */
static int
nbdplug_pwrite (void *handle, const void *buf, uint32_t count, uint64_t offset,
                uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;
  uint32_t f = flags & NBDKIT_FLAG_FUA ? LIBNBD_CMD_FLAG_FUA : 0;

  assert (!(flags & ~NBDKIT_FLAG_FUA));
  s = nbdplug_register (h, nbd_aio_pwrite (h->nbd, buf, count, offset, f));
  return nbdplug_reply (h, s);
}

/* Write zeroes to the file. */
static int
nbdplug_zero (void *handle, uint32_t count, uint64_t offset, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;
  uint32_t f = 0;

  assert (!(flags & ~(NBDKIT_FLAG_FUA | NBDKIT_FLAG_MAY_TRIM)));

  if (!(flags & NBDKIT_FLAG_MAY_TRIM))
    f |= LIBNBD_CMD_FLAG_NO_HOLE;
  if (flags & NBDKIT_FLAG_FUA)
    f |= LIBNBD_CMD_FLAG_FUA;
  s = nbdplug_register (h, nbd_aio_zero (h->nbd, count, offset, f));
  return nbdplug_reply (h, s);
}

/* Trim a portion of the file. */
static int
nbdplug_trim (void *handle, uint32_t count, uint64_t offset, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;
  uint32_t f = flags & NBDKIT_FLAG_FUA ? LIBNBD_CMD_FLAG_FUA : 0;

  assert (!(flags & ~NBDKIT_FLAG_FUA));
  s = nbdplug_register (h, nbd_aio_trim (h->nbd, count, offset, f));
  return nbdplug_reply (h, s);
}

/* Flush the file to disk. */
static int
nbdplug_flush (void *handle, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!flags);
  s = nbdplug_register (h, nbd_aio_flush (h->nbd, 0));
  return nbdplug_reply (h, s);
}

static int
nbdplug_extent (void *opaque, const char *metacontext, uint64_t offset,
                uint32_t *entries, size_t nr_entries)
{
  struct nbdkit_extents *extents = opaque;

  assert (strcmp (metacontext, "base:allocation") == 0);
  assert (nr_entries % 2 == 0);
  while (nr_entries) {
    /* We rely on the fact that NBDKIT_EXTENT_* match NBD_STATE_* */
    if (nbdkit_add_extent (extents, offset, entries[0], entries[1]) == -1)
      return -1;
    offset += entries[0];
    entries += 2;
    nr_entries -= 2;
  }
  return 0;
}

/* Read extents of the file. */
static int
nbdplug_extents (void *handle, uint32_t count, uint64_t offset,
                 uint32_t flags, struct nbdkit_extents *extents)
{
  struct handle *h = handle;
  struct transaction *s;
  uint32_t f = flags & NBDKIT_FLAG_REQ_ONE ? LIBNBD_CMD_FLAG_REQ_ONE : 0;

  assert (!(flags & ~NBDKIT_FLAG_REQ_ONE));
  s = nbdplug_register (h, nbd_aio_block_status (h->nbd, count, offset,
                                                 extents, nbdplug_extent, f));
  return nbdplug_reply (h, s);
}

/* Cache a portion of the file. */
static int
nbdplug_cache (void *handle, uint32_t count, uint64_t offset, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!flags);
  s = nbdplug_register (h, nbd_aio_cache (h->nbd, count, offset, 0));
  return nbdplug_reply (h, s);
}

static struct nbdkit_plugin plugin = {
  .name               = "nbd",
  .longname           = "nbdkit nbd plugin",
  .version            = PACKAGE_VERSION,
  .unload             = nbdplug_unload,
  .config             = nbdplug_config,
  .config_complete    = nbdplug_config_complete,
  .config_help        = nbdplug_config_help,
  .magic_config_key   = "uri",
  .dump_plugin        = nbdplug_dump_plugin,
  .open               = nbdplug_open,
  .close              = nbdplug_close,
  .get_size           = nbdplug_get_size,
  .can_write          = nbdplug_can_write,
  .can_flush          = nbdplug_can_flush,
  .is_rotational      = nbdplug_is_rotational,
  .can_trim           = nbdplug_can_trim,
  .can_zero           = nbdplug_can_zero,
  .can_fua            = nbdplug_can_fua,
  .can_multi_conn     = nbdplug_can_multi_conn,
  .can_extents        = nbdplug_can_extents,
  .can_cache          = nbdplug_can_cache,
  .pread              = nbdplug_pread,
  .pwrite             = nbdplug_pwrite,
  .zero               = nbdplug_zero,
  .flush              = nbdplug_flush,
  .trim               = nbdplug_trim,
  .extents            = nbdplug_extents,
  .cache              = nbdplug_cache,
  .errno_is_preserved = 1,
};

NBDKIT_REGISTER_PLUGIN (plugin)
