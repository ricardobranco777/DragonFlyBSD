#	$FreeBSD$
#
# Copyright (c) 2002, 2003	Sam Leffler, Errno Consulting
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#

ALL=	cryptotest cryptokeytest cryptostats
BINDIR=	/usr/local/bin
PRIVATELIB_LDFLAGS?= -rpath /lib/priv:/usr/lib/priv -L /usr/lib/priv

all:	${ALL}

# program to test asymmetric crypto functions
cryptokeytest: cryptokeytest.c
	${CC} -o cryptokeytest cryptokeytest.c \
	 -rpath /usr/lib/priv ${PRIVATELIB_LDFLAGS} \
	 -lprivate_crypto

# program to dump statistics kept by the core crypto code
cryptostats: cryptostats.c
	${CC} -o cryptostats cryptostats.c

clean:
	rm -f ${ALL} core a.out

install: ${ALL}
	for i in ${ALL}; do \
		install $$i ${DESTDIR}${BINDIR}; \
	done
