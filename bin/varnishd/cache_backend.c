/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2008 Linpro AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 *
 * Handle backend connections and backend request structures.
 *
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include <sys/socket.h>

#include "shmlog.h"
#include "cache.h"
#include "cache_backend.h"

/*
 * List of cached vbe_conns, used if enabled in params/heritage
 */
static VTAILQ_HEAD(,vbe_conn) vbe_conns = VTAILQ_HEAD_INITIALIZER(vbe_conns);

/*
 * List of cached bereq's
 */
static VTAILQ_HEAD(,bereq) bereq_head = VTAILQ_HEAD_INITIALIZER(bereq_head);

/*--------------------------------------------------------------------
 * Create default Host: header for backend request
 */
void
VBE_AddHostHeader(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->bereq, BEREQ_MAGIC);
	CHECK_OBJ_NOTNULL(sp->bereq->http, HTTP_MAGIC);
	CHECK_OBJ_NOTNULL(sp->backend, BACKEND_MAGIC);
	http_PrintfHeader(sp->wrk, sp->fd, sp->bereq->http,
	    "Host: %s", sp->backend->hosthdr);
}

/*--------------------------------------------------------------------
 * Attempt to connect to a given addrinfo entry.
 *
 * Must be called with locked backend, but will release the backend
 * lock during the slow/sleeping stuff, so that other worker threads
 * can have a go, while we ponder.
 *
 */

static int
VBE_TryConnect(const struct sess *sp, int pf, const struct sockaddr *sa, socklen_t salen)
{
	int s, i, tmo;
	char abuf1[TCP_ADDRBUFSIZE], abuf2[TCP_ADDRBUFSIZE];
	char pbuf1[TCP_PORTBUFSIZE], pbuf2[TCP_PORTBUFSIZE];

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->backend, BACKEND_MAGIC);

	s = socket(pf, SOCK_STREAM, 0);
	if (s < 0) {
		LOCK(&sp->backend->mtx);
		return (s);
	}

	tmo = params->connect_timeout;
	if (sp->backend->connect_timeout > 10e-3)
		tmo = (int)(sp->backend->connect_timeout * 1000);

	if (tmo > 0)
		i = TCP_connect(s, sa, salen, tmo);
	else
		i = connect(s, sa, salen);

	if (i != 0) {
		AZ(close(s));
		return (-1);
	}

	TCP_myname(s, abuf1, sizeof abuf1, pbuf1, sizeof pbuf1);
	TCP_name(sa, salen, abuf2, sizeof abuf2, pbuf2, sizeof pbuf2);
	WSL(sp->wrk, SLT_BackendOpen, s, "%s %s %s %s %s",
	    sp->backend->vcl_name, abuf1, pbuf1, abuf2, pbuf2);

	return (s);
}

/*--------------------------------------------------------------------
 * Check that there is still something at the far end of a given socket.
 * We poll the fd with instant timeout, if there are any events we can't
 * use it (backends are not allowed to pipeline).
 */

static int
VBE_CheckFd(int fd)
{
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	return(poll(&pfd, 1, 0) == 0);
}

/*--------------------------------------------------------------------
 * Get a bereq structure for talking HTTP with the backend.
 * First attempt to pick one from our stash, else make a new.
 *
 * Can fail with NULL.
 */

struct bereq *
VBE_new_bereq(void)
{
	struct bereq *bereq;
	volatile unsigned len;

	LOCK(&VBE_mtx);
	bereq = VTAILQ_FIRST(&bereq_head);
	if (bereq != NULL)
		VTAILQ_REMOVE(&bereq_head, bereq, list);
	UNLOCK(&VBE_mtx);
	if (bereq != NULL) {
		CHECK_OBJ(bereq, BEREQ_MAGIC);
	} else {
		len =  params->sess_workspace;
		bereq = calloc(sizeof *bereq + len, 1);
		if (bereq == NULL)
			return (NULL);
		bereq->magic = BEREQ_MAGIC;
		WS_Init(bereq->ws, "bereq", bereq + 1, len);
		VSL_stats->n_bereq++;
	}
	http_Setup(bereq->http, bereq->ws);
	return (bereq);
}

/*--------------------------------------------------------------------
 * Return a bereq to the stash.
 */

void
VBE_free_bereq(struct bereq *bereq)
{

	CHECK_OBJ_NOTNULL(bereq, BEREQ_MAGIC);
	WS_Reset(bereq->ws, NULL);
	LOCK(&VBE_mtx);
	VTAILQ_INSERT_HEAD(&bereq_head, bereq, list);
	UNLOCK(&VBE_mtx);
}

/*--------------------------------------------------------------------
 * Manage a pool of vbe_conn structures.
 * XXX: as an experiment, make this caching controled by a parameter
 * XXX: so we can see if it has any effect.
 */

static struct vbe_conn *
VBE_NewConn(void)
{
	struct vbe_conn *vc;

	vc = VTAILQ_FIRST(&vbe_conns);
	if (vc != NULL) {
		LOCK(&VBE_mtx);
		vc = VTAILQ_FIRST(&vbe_conns);
		if (vc != NULL) {
			VSL_stats->backend_unused--;
			VTAILQ_REMOVE(&vbe_conns, vc, list);
		}
		UNLOCK(&VBE_mtx);
	}
	if (vc != NULL)
		return (vc);
	vc = calloc(sizeof *vc, 1);
	XXXAN(vc);
	vc->magic = VBE_CONN_MAGIC;
	vc->fd = -1;
	VSL_stats->n_vbe_conn++;
	return (vc);
}

void
VBE_ReleaseConn(struct vbe_conn *vc)
{

	CHECK_OBJ_NOTNULL(vc, VBE_CONN_MAGIC);
	assert(vc->backend == NULL);
	assert(vc->fd < 0);

	if (params->cache_vbe_conns) {
		LOCK(&VBE_mtx);
		VTAILQ_INSERT_HEAD(&vbe_conns, vc, list);
		VSL_stats->backend_unused++;
		UNLOCK(&VBE_mtx);
	} else {
		VSL_stats->n_vbe_conn--;
		free(vc);
	}
}

/*--------------------------------------------------------------------*/

static int
bes_conn_try(const struct sess *sp, struct backend *bp)
{
	int s;

	LOCK(&bp->mtx);
	bp->refcount++;
	UNLOCK(&sp->backend->mtx);

	s = -1;
	assert(bp->ipv6 != NULL || bp->ipv4 != NULL);

	/* release lock during stuff that can take a long time */

	if (params->prefer_ipv6 && bp->ipv6 != NULL)
		s = VBE_TryConnect(sp, PF_INET6, bp->ipv6, bp->ipv6len);
	if (s == -1 && bp->ipv4 != NULL)
		s = VBE_TryConnect(sp, PF_INET, bp->ipv4, bp->ipv4len);
	if (s == -1 && !params->prefer_ipv6 && bp->ipv6 != NULL)
		s = VBE_TryConnect(sp, PF_INET6, bp->ipv6, bp->ipv6len);

	if (s < 0) {
		LOCK(&sp->backend->mtx);
		bp->refcount--;		/* Only keep ref on success */
		UNLOCK(&bp->mtx);
	}
	return (s);
}

/*--------------------------------------------------------------------*/

struct vbe_conn *
VBE_GetFd(const struct sess *sp)
{
	struct backend *bp;
	struct vbe_conn *vc;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	bp = sp->backend;

	/* first look for vbe_conn's we can recycle */
	while (1) {
		LOCK(&bp->mtx);
		vc = VTAILQ_FIRST(&bp->connlist);
		if (vc != NULL) {
			bp->refcount++;
			assert(vc->backend == bp);
			assert(vc->fd >= 0);
			VTAILQ_REMOVE(&bp->connlist, vc, list);
		}
		UNLOCK(&bp->mtx);
		if (vc == NULL)
			break;
		if (VBE_CheckFd(vc->fd)) {
			/* XXX locking of stats */
			VSL_stats->backend_reuse += 1;
			VSL_stats->backend_conn++;
			return (vc);
		}
		VBE_ClosedFd(sp->wrk, vc);
	}

	vc = VBE_NewConn();
	assert(vc->fd == -1);
	AZ(vc->backend);
	vc->fd = bes_conn_try(sp, bp);
	if (vc->fd < 0) {
		VBE_ReleaseConn(vc);
		VSL_stats->backend_fail++;
		return (NULL);
	}
	vc->backend = bp;
	VSL_stats->backend_conn++;
	return (vc);
}

/* Close a connection ------------------------------------------------*/

void
VBE_ClosedFd(struct worker *w, struct vbe_conn *vc)
{
	struct backend *b;
	int i;

	CHECK_OBJ_NOTNULL(vc, VBE_CONN_MAGIC);
	CHECK_OBJ_NOTNULL(vc->backend, BACKEND_MAGIC);
	b = vc->backend;
	assert(vc->fd >= 0);
	WSL(w, SLT_BackendClose, vc->fd, "%s", vc->backend->vcl_name);
	i = close(vc->fd);
	assert(i == 0 || errno == ECONNRESET || errno == ENOTCONN);
	vc->fd = -1;
	VBE_DropRef(vc->backend);
	vc->backend = NULL;
	VBE_ReleaseConn(vc);
	CHECK_OBJ_NOTNULL(b, BACKEND_MAGIC);
}

/* Recycle a connection ----------------------------------------------*/

void
VBE_RecycleFd(struct worker *w, struct vbe_conn *vc)
{
	struct backend *bp;

	CHECK_OBJ_NOTNULL(vc, VBE_CONN_MAGIC);
	CHECK_OBJ_NOTNULL(vc->backend, BACKEND_MAGIC);
	assert(vc->fd >= 0);
	bp = vc->backend;
	WSL(w, SLT_BackendReuse, vc->fd, "%s", vc->backend->vcl_name);
	LOCK(&vc->backend->mtx);
	VSL_stats->backend_recycle++;
	VTAILQ_INSERT_HEAD(&bp->connlist, vc, list);
	VBE_DropRefLocked(vc->backend);
	CHECK_OBJ_NOTNULL(bp, BACKEND_MAGIC);
}

/* Update health ----------------------------------------------------*/
/* See cache_backend_random.c and/or cache_backend_round_robin.c for
 * details and comments about this function. 
 */
void
VBE_UpdateHealth(const struct sess *sp, const struct vbe_conn *vc, int a)
{
	(void)sp;
	(void)vc;
	(void)a;
#if 0
	INCOMPL();
	struct backend *b;

	if (vc != NULL) {
		CHECK_OBJ_NOTNULL(vc, VBE_CONN_MAGIC);
		CHECK_OBJ_NOTNULL(vc->backend, BACKEND_MAGIC);
		b = vc->backend;
	}
	else {
		CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
		CHECK_OBJ_NOTNULL(sp->backend, BACKEND_MAGIC);
		b = sp->backend;
	}
	AN(b->method);
	if (b->method->updatehealth != NULL)
		b->method->updatehealth(sp, vc, a);
	CHECK_OBJ_NOTNULL(b, BACKEND_MAGIC);
#endif
}
