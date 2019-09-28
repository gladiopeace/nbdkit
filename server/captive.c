/* nbdkit
 * Copyright (C) 2019 Red Hat Inc.
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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "utils.h"

#include "internal.h"

/* Handle the --run option.  If run is NULL, does nothing.  If run is
 * not NULL then run nbdkit as a captive subprocess of the command.
 */
void
run_command (void)
{
  FILE *fp;
  char *cmd = NULL;
  size_t len = 0;
  int r;
  pid_t pid;

  if (!run)
    return;

  fp = open_memstream (&cmd, &len);
  if (fp == NULL) {
    perror ("open_memstream");
    exit (EXIT_FAILURE);
  }

  /* Construct $uri. */
  fprintf (fp, "uri=");
  if (port) {
    fprintf (fp, "nbd://localhost:");
    shell_quote (port, fp);
    if (exportname) {
      putc ('/', fp);
      uri_quote (exportname, fp);
    }
  }
  else if (unixsocket) {
    fprintf (fp, "nbd+unix://");
    if (exportname) {
      putc ('/', fp);
      uri_quote (exportname, fp);
    }
    fprintf (fp, "\\?socket=");
    uri_quote (unixsocket, fp);
  }
  putc ('\n', fp);

  /* Expose $exportname. */
  fprintf (fp, "exportname=");
  shell_quote (exportname, fp);
  putc ('\n', fp);

  /* Construct older $nbd "URL".  Unfortunately guestfish and qemu take
   * different syntax, so try to guess which one we need.
   */
  fprintf (fp, "nbd=");
  if (strstr (run, "guestfish")) {
    if (port) {
      fprintf (fp, "nbd://localhost:");
      shell_quote (port, fp);
    }
    else if (unixsocket) {
      fprintf (fp, "nbd://\\?socket=");
      shell_quote (unixsocket, fp);
    }
    else
      abort ();
  }
  else /* qemu */ {
    if (port) {
      fprintf (fp, "nbd:localhost:");
      shell_quote (port, fp);
    }
    else if (unixsocket) {
      fprintf (fp, "nbd:unix:");
      shell_quote (unixsocket, fp);
    }
    else
      abort ();
  }
  putc ('\n', fp);

  /* Construct $port and $unixsocket. */
  fprintf (fp, "port=");
  if (port)
    shell_quote (port, fp);
  putc ('\n', fp);
  fprintf (fp, "unixsocket=");
  if (unixsocket)
    shell_quote (unixsocket, fp);
  fprintf (fp, "\n");

  /* Add the --run command.  Note we don't have to quote this. */
  fprintf (fp, "%s", run);

  if (fclose (fp) == EOF) {
    perror ("memstream failed");
    exit (EXIT_FAILURE);
  }

  /* Fork.  Captive nbdkit runs as the child process. */
  pid = fork ();
  if (pid == -1) {
    perror ("fork");
    exit (EXIT_FAILURE);
  }

  if (pid > 0) {              /* Parent process is the run command. */
    r = system (cmd);
    if (WIFEXITED (r))
      r = WEXITSTATUS (r);
    else if (WIFSIGNALED (r)) {
      fprintf (stderr, "%s: external command was killed by signal %d\n",
               program_name, WTERMSIG (r));
      r = 1;
    }
    else if (WIFSTOPPED (r)) {
      fprintf (stderr, "%s: external command was stopped by signal %d\n",
               program_name, WSTOPSIG (r));
      r = 1;
    }

    kill (pid, SIGTERM);        /* Kill captive nbdkit. */

    _exit (r);
  }

  free (cmd);

  debug ("forked into background (new pid = %d)", getpid ());
}
