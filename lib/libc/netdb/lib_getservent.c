/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/*  $OpenBSD: getservent.c,v 1.12 2007/09/02 15:19:17 deraadt Exp $ */
/*
 * Copyright (c) 1983, 1993
 *  The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BUFSIZ 1024
char *fgetln(FILE *fp, size_t *len)
{
	static char *buf = NULL;
	static size_t bufsiz = 0;
	char *ptr;

	if (buf == NULL) {
		bufsiz = BUFSIZ;
		if ((buf = malloc(bufsiz)) == NULL) {
			return NULL;
		}
	}

	if (fgets(buf, bufsiz, fp) == NULL) {
		return NULL;
	}

	*len = 0;
	while ((ptr = strchr(&buf[*len], '\n')) == NULL) {
		size_t nbufsiz = bufsiz + BUFSIZ;
		char *nbuf = realloc(buf, nbufsiz);

		if (nbuf == NULL) {
			int oerrno = errno;
			free(buf);
			errno = oerrno;
			buf = NULL;
			return NULL;
		} else {
			buf = nbuf;
		}

		*len = bufsiz;
		if (fgets(&buf[bufsiz], BUFSIZ, fp) == NULL) {
			return buf;
		}

		bufsiz = nbufsiz;
	}

	*len = (ptr - buf) + 1;
	return buf;
}

void setservent_r(int f, struct servent_data *sd)
{
	if (sd->fp == NULL) {
		sd->fp = fopen(_PATH_SERVICES, "r");
	} else {
		rewind(sd->fp);
	}
	sd->stayopen |= f;
}

void endservent_r(struct servent_data *sd)
{
	if (sd->fp) {
		fclose(sd->fp);
		sd->fp = NULL;
	}
	free(sd->aliases);
	sd->aliases = NULL;
	sd->maxaliases = 0;
	free(sd->line);
	sd->line = NULL;
	sd->stayopen = 0;
}

int getservent_r(struct servent *se, struct servent_data *sd)
{
	char *p, *cp, **q, *endp;
	size_t len;
	long l;
	int serrno;

	if (sd->fp == NULL && (sd->fp = fopen(_PATH_SERVICES, "r")) == NULL) {
		return (-1);
	}
again:
	if ((p = fgetln(sd->fp, &len)) == NULL) {
		return (-1);
	}
	if (len == 0 || *p == '#' || *p == '\n') {
		goto again;
	}
	if (p[len - 1] == '\n') {
		len--;
	}
	if ((cp = memchr(p, '#', len)) != NULL) {
		len = cp - p;
	}
	cp = realloc(sd->line, len + 1);
	if (cp == NULL) {
		return (-1);
	}
	sd->line = se->s_name = memcpy(cp, p, len);
	cp[len] = '\0';
	p = strpbrk(cp, " \t");
	if (p == NULL) {
		goto again;
	}
	*p++ = '\0';
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	cp = strpbrk(p, ",/");
	if (cp == NULL) {
		goto again;
	}
	*cp++ = '\0';
	l = strtol(p, &endp, 10);
	if (endp == p || *endp != '\0' || l < 0 || l > USHRT_MAX) {
		goto again;
	}
	se->s_port = htons((in_port_t)l);
	se->s_proto = cp;
	if (sd->aliases == NULL) {
		sd->maxaliases = 10;
		sd->aliases = calloc(sd->maxaliases, sizeof(char *));
		if (sd->aliases == NULL) {
			serrno = errno;
			endservent_r(sd);
			errno = serrno;
			return (-1);
		}
	}
	q = se->s_aliases = sd->aliases;
	cp = strpbrk(cp, " \t");
	if (cp != NULL) {
		*cp++ = '\0';
	}
	while (cp && *cp) {
		if (*cp == ' ' || *cp == '\t') {
			cp++;
			continue;
		}
		if (q == &se->s_aliases[sd->maxaliases - 1]) {
			p = realloc(se->s_aliases, 2 * sd->maxaliases * sizeof(char *));
			if (p == NULL) {
				serrno = errno;
				endservent_r(sd);
				errno = serrno;
				return (-1);
			}
			sd->maxaliases *= 2;
			q = (char **)p + (q - se->s_aliases);
			se->s_aliases = sd->aliases = (char **)p;
		}
		*q++ = cp;
		cp = strpbrk(cp, " \t");
		if (cp != NULL) {
			*cp++ = '\0';
		}
	}
	*q = NULL;
	return (0);
}

struct servent_data _servent_data;	/* shared with getservby{name,port}.c */

void setservent(int f)
{
	setservent_r(f, &_servent_data);
}

void endservent(void)
{
	endservent_r(&_servent_data);
}

struct servent *getservent(void)
{
	static struct servent serv;

	if (getservent_r(&serv, &_servent_data) != 0) {
		return (NULL);
	}
	return (&serv);
}
