#!/usr/bin/env bash
# nbdkit
# Copyright (C) 2018-2020 Red Hat Inc.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# * Neither the name of Red Hat nor the names of its contributors may be
# used to endorse or promote products derived from this software without
# specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
# PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
# USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
# OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.

# Test swab filter.

source ./functions.sh
set -e
set -x

requires_daemonizing
requires nbdsh --version

sock=`mktemp -u`
files="$sock swab-64r.pid"
rm -f $files
cleanup_fn rm -f $files

start_nbdkit -P swab-64r.pid -U $sock --filter=swab \
             pattern size=$((128*1024)) swab-bits=64

nbdsh --connect "nbd+unix://?socket=$sock" \
      -c '
# Read back part of the pattern and check it is byte-swapped.
# Original pattern is:
# 00 00 00 00 00 00 ff f8
# 00 00 00 00 00 01 00 00
# 00 00 00 00 00 01 00 08
# 00 00 00 00 00 01 00 10
# Expected output is:
# f8 ff 00 00 00 00 00 00
# 00 00 01 00 00 00 00 00
# 08 00 01 00 00 00 00 00
# 10 00 01 00 00 00 00 00

buf = h.pread (32, 65528)
assert buf == bytearray ([0xf8, 0xff, 0, 0, 0, 0, 0, 0,
                          0,    0,    1, 0, 0, 0, 0, 0,
                          0x08, 0,    1, 0, 0, 0, 0, 0,
                          0x10, 0,    1, 0, 0, 0, 0, 0])
'
