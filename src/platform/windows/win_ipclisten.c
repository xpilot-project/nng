//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
// Copyright 2018 Devolutions <info@devolutions.net>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include "win_ipc.h"

#include <stdio.h>

static void
ipc_accept_done(nni_ipc_listener *l, int rv)
{
	nni_aio *     aio;
	HANDLE        f;
	nni_ipc_conn *c;

	aio = nni_list_first(&l->aios);
	nni_list_remove(&l->aios, aio);
	nni_cv_wake(&l->cv);

	if (l->closed) {
		// Closed, so bail.
		DisconnectNamedPipe(l->f);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	// Create a replacement pipe.
	f = CreateNamedPipeA(l->path,
	    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
	        PIPE_REJECT_REMOTE_CLIENTS,
	    PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &l->sec_attr);
	if (f == INVALID_HANDLE_VALUE) {
		// We couldn't create a replacement pipe, so we have to
		// abort the client from our side, so that we can keep
		// our server pipe available.
		rv = nni_win_error(GetLastError());
		DisconnectNamedPipe(l->f);
		nni_aio_finish_error(aio, rv);
		return;
	}

	if (((rv = nni_win_io_register(f)) != 0) ||
	    ((rv = nni_win_ipc_conn_init(&c, l->f)) != 0)) {
		DisconnectNamedPipe(l->f);
		DisconnectNamedPipe(f);
		CloseHandle(f);
		nni_aio_finish_error(aio, rv);
		return;
	}
	l->f                  = f;
	c->sa.s_ipc.sa_family = NNG_AF_IPC;
	snprintf(c->sa.s_ipc.sa_path, sizeof(c->sa.s_ipc.sa_path), "%s",
	    l->path + strlen(IPC_PIPE_PREFIX));
	c->dialer = false;
	nni_aio_set_output(aio, 0, c);
	nni_aio_finish(aio, 0, 0);
}

static void
ipc_accept_start(nni_ipc_listener *l)
{
	nni_aio *aio;

	if (l->closed) {
		while ((aio = nni_list_first(&l->aios)) != NULL) {
			nni_list_remove(&l->aios, aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		nni_cv_wake(&l->cv);
	}

	while ((aio = nni_list_first(&l->aios)) != NULL) {
		int rv;

		if ((ConnectNamedPipe(l->f, &l->io.olpd)) ||
		    ((rv = GetLastError()) == ERROR_IO_PENDING)) {
			// Success, or pending, handled via completion pkt.
			return;
		}
		if (rv == ERROR_PIPE_CONNECTED) {
			// Kind of like success, but as this is technically
			// an "error", we have to complete it ourself.
			// Fake a completion.
			ipc_accept_done(l, 0);
		} else {
			// Fast-fail (synchronous).
			nni_aio_finish_error(aio, nni_win_error(rv));
		}
	}
}

static void
ipc_accept_cb(nni_win_io *io, int rv, size_t cnt)
{
	nni_ipc_listener *l = io->ptr;

	NNI_ARG_UNUSED(cnt);

	nni_mtx_lock(&l->mtx);
	if (nni_list_empty(&l->aios)) {
		// We canceled this somehow.  We no longer care.
		DisconnectNamedPipe(l->f);
		nni_mtx_unlock(&l->mtx);
		return;
	}
	if (l->rv != 0) {
		rv    = l->rv;
		l->rv = 0;
	}
	ipc_accept_done(l, rv);
	ipc_accept_start(l);
	nni_mtx_unlock(&l->mtx);
}

int
nni_ipc_listener_init(nni_ipc_listener **lp)
{
	nni_ipc_listener *l;
	int               rv;

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((rv = nni_win_io_init(&l->io, ipc_accept_cb, l)) != 0) {
		NNI_FREE_STRUCT(l);
		return (rv);
	}
	l->started                       = false;
	l->closed                        = false;
	l->sec_attr.nLength              = sizeof(l->sec_attr);
	l->sec_attr.lpSecurityDescriptor = NULL;
	l->sec_attr.bInheritHandle       = FALSE;
	nni_aio_list_init(&l->aios);
	nni_mtx_init(&l->mtx);
	nni_cv_init(&l->cv, &l->mtx);
	*lp = l;
	return (0);
}

static int
ipc_listener_set_sec_desc(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ipc_listener *l = arg;
	void *            desc;
	int               rv;

	if ((rv = nni_copyin_ptr(&desc, buf, sz, t)) != 0) {
		return (rv);
	}
	if (!IsValidSecurityDescriptor((SECURITY_DESCRIPTOR *) desc)) {
		return (NNG_EINVAL);
	}
	if (l != NULL) {
		nni_mtx_lock(&l->mtx);
		if (l->started) {
			nni_mtx_unlock(&l->mtx);
			return (NNG_EBUSY);
		}
		l->sec_attr.lpSecurityDescriptor = desc;
		nni_mtx_unlock(&l->mtx);
	}
	return (0);
}

static int
ipc_listener_get_addr(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ipc_listener *l = arg;
	return ((nni_copyout_sockaddr(&l->sa, buf, szp, t)));
}

static const nni_option ipc_listener_options[] = {
	{
	    .o_name = NNG_OPT_IPC_SECURITY_DESCRIPTOR,
	    .o_set  = ipc_listener_set_sec_desc,
	},
	{
	    .o_name = NNG_OPT_LOCADDR,
	    .o_get  = ipc_listener_get_addr,
	},
	{
	    .o_name = NULL,
	},
};

int
nni_ipc_listener_setopt(nni_ipc_listener *l, const char *name, const void *buf,
    size_t sz, nni_type t)
{
	return (nni_setopt(ipc_listener_options, name, l, buf, sz, t));
}

int
nni_ipc_listener_getopt(
    nni_ipc_listener *l, const char *name, void *buf, size_t *szp, nni_type t)
{
	return (nni_getopt(ipc_listener_options, name, l, buf, szp, t));
}

int
nni_ipc_listener_listen(nni_ipc_listener *l, const nni_sockaddr *sa)
{
	int    rv;
	HANDLE f;
	char * path;

	nni_mtx_lock(&l->mtx);
	if (l->started) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_EBUSY);
	}
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ECLOSED);
	}
	rv = nni_asprintf(&path, IPC_PIPE_PREFIX "%s", sa->s_ipc.sa_path);
	if (rv != 0) {
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}

	f = CreateNamedPipeA(path,
	    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
	        FILE_FLAG_FIRST_PIPE_INSTANCE,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
	        PIPE_REJECT_REMOTE_CLIENTS,
	    PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &l->sec_attr);
	if (f == INVALID_HANDLE_VALUE) {
		if ((rv = GetLastError()) == ERROR_ACCESS_DENIED) {
			rv = NNG_EADDRINUSE;
		} else {
			rv = nni_win_error(rv);
		}
		nni_mtx_unlock(&l->mtx);
		nni_strfree(path);
		return (rv);
	}
	if ((rv = nni_win_io_register(f)) != 0) {
		CloseHandle(f);
		nni_mtx_unlock(&l->mtx);
		nni_strfree(path);
		return (rv);
	}

	l->f       = f;
	l->path    = path;
	l->started = true;
	l->sa      = *sa;
	nni_mtx_unlock(&l->mtx);
	return (0);
}

static void
ipc_accept_cancel(nni_aio *aio, void *arg, int rv)
{
	nni_ipc_listener *l = arg;

	nni_mtx_unlock(&l->mtx);
	if (aio == nni_list_first(&l->aios)) {
		l->rv = rv;
		CancelIoEx(l->f, &l->io.olpd);
	} else if (nni_aio_list_active(aio)) {
		nni_list_remove(&l->aios, aio);
		nni_cv_wake(&l->cv);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&l->mtx);
}

void
nni_ipc_listener_accept(nni_ipc_listener *l, nni_aio *aio)
{
	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&l->mtx);
	if (!l->started) {
		nni_mtx_unlock(&l->mtx);
		nni_aio_finish_error(aio, NNG_ESTATE);
		return;
	}
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_list_append(&l->aios, aio);
	if (nni_list_first(&l->aios) == aio) {
		ipc_accept_start(l);
	}
	nni_mtx_unlock(&l->mtx);
}

void
nni_ipc_listener_close(nni_ipc_listener *l)
{
	nni_mtx_lock(&l->mtx);
	if (!l->closed) {
		l->closed = true;
		if (!nni_list_empty(&l->aios)) {
			CancelIoEx(l->f, &l->io.olpd);
		}
		DisconnectNamedPipe(l->f);
		CloseHandle(l->f);
	}
	nni_mtx_unlock(&l->mtx);
}

void
nni_ipc_listener_fini(nni_ipc_listener *l)
{
	nni_mtx_lock(&l->mtx);
	while (!nni_list_empty(&l->aios)) {
		nni_cv_wait(&l->cv);
	}
	nni_mtx_unlock(&l->mtx);
	nni_win_io_fini(&l->io);
	nni_strfree(l->path);
	nni_cv_fini(&l->cv);
	nni_mtx_fini(&l->mtx);
	NNI_FREE_STRUCT(l);
}
