/*
 * _ssl — native TLS module for TaterTOS64, backed by in-tree BearSSL.
 *
 * Implements the CPython `_ssl` <-> `ssl.py` contract directly over BearSSL's
 * client engine (br_ssl_client_*, br_x509_minimal) — NOT an OpenSSL-compat
 * shim. One engine-drive path serves both the blocking socket model
 * (SSLSocket) and the non-blocking memory-BIO model (SSLObject/wrap_bio, the
 * path async httpx uses). Trust anchors are decoded from the certifi PEM
 * bundle so verification is real.
 */

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "bearssl.h"
#include <sys/socket.h>
#include <errno.h>
#include <string.h>

/* ================================================================== */
/*  Constants mirrored from ssl.py's expectations                      */
/* ================================================================== */
#define TS_CERT_NONE      0
#define TS_CERT_OPTIONAL  1
#define TS_CERT_REQUIRED  2

#define TS_PROTO_TLS         2   /* PROTOCOL_TLS (auto-negotiate) */
#define TS_PROTO_TLS_CLIENT 16
#define TS_PROTO_TLS_SERVER 17

/* ================================================================== */
/*  Object state                                                       */
/* ================================================================== */

typedef struct {
    br_x509_trust_anchor ta;
    unsigned char *dn;
    unsigned char *key_a;   /* RSA n / EC q */
    unsigned char *key_b;   /* RSA e */
} ts_anchor;

typedef struct {
    PyObject_HEAD
    ts_anchor *anchors;          /* owns the DN/key buffers (for free) */
    br_x509_trust_anchor *ta_packed;  /* contiguous copies for BearSSL */
    size_t anchor_count;
    int verify_mode;
    int check_hostname;
    int protocol;
    long options;
    long verify_flags;
} PySSLContext;

typedef struct {
    PyObject_HEAD
    unsigned char *buf;     /* pending bytes */
    Py_ssize_t start;       /* read cursor */
    Py_ssize_t len;         /* write cursor */
    Py_ssize_t cap;
    int eof;
} PySSLMemoryBIO;

typedef struct {
    PyObject_HEAD
    PyObject *params;       /* opaque holder; BearSSL session params not exposed */
} PySSLSession;

typedef struct {
    PyObject_HEAD
    PySSLContext *ctx;
    PyObject *server_hostname;   /* str or NULL */
    PyObject *sock;              /* socket obj (blocking path) or NULL */
    PySSLMemoryBIO *inbio;       /* wrap_bio incoming (async) or NULL */
    PySSLMemoryBIO *outbio;      /* wrap_bio outgoing (async) or NULL */
    int sock_fd;                 /* -1 for bio path */
    int server_side;
    int handshake_done;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char *iobuf;
} PySSLSocket;

static PyTypeObject PySSLContext_Type;
static PyTypeObject PySSLMemoryBIO_Type;
static PyTypeObject PySSLSession_Type;
static PyTypeObject PySSLSocket_Type;

static PyObject *PySSLErrorObject;
static PyObject *PySSLZeroReturnErrorObject;
static PyObject *PySSLWantReadErrorObject;
static PyObject *PySSLWantWriteErrorObject;
static PyObject *PySSLSyscallErrorObject;
static PyObject *PySSLEOFErrorObject;
static PyObject *PySSLCertVerificationErrorObject;

/* ================================================================== */
/*  BearSSL error -> Python exception                                  */
/* ================================================================== */

static PyObject *raise_bearssl_error(int err) {
    /* X.509 validation failures map to SSLCertVerificationError. */
    if (err >= BR_ERR_X509_OK && err <= BR_ERR_X509_NOT_TRUSTED + 32
            && err > BR_ERR_RECV_FATAL_ALERT) {
        PyErr_Format(PySSLCertVerificationErrorObject,
                     "certificate verify failed (BearSSL x509 err %d)", err);
        return NULL;
    }
    switch (err) {
    case BR_ERR_OK:
        PyErr_SetString(PySSLErrorObject, "unexpected OK");
        break;
    case BR_ERR_IO:
        PyErr_SetString(PySSLSyscallErrorObject, "underlying I/O error");
        break;
    default:
        PyErr_Format(PySSLErrorObject, "BearSSL error %d", err);
        break;
    }
    return NULL;
}

/* ================================================================== */
/*  Trust-anchor loader (certifi PEM bundle -> br_x509_trust_anchor[]) */
/* ================================================================== */

typedef struct { unsigned char *buf; size_t len, cap; int oom; } byte_sink;

static void sink_append(void *cc, const void *data, size_t len) {
    byte_sink *d = (byte_sink *)cc;
    if (d->oom) return;
    if (d->len + len > d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 4096;
        while (ncap < d->len + len) ncap *= 2;
        unsigned char *nb = (unsigned char *)PyMem_Realloc(d->buf, ncap);
        if (!nb) { d->oom = 1; return; }
        d->buf = nb; d->cap = ncap;
    }
    memcpy(d->buf + d->len, data, len);
    d->len += len;
}

static int der_to_anchor(const unsigned char *der, size_t der_len, ts_anchor *out) {
    br_x509_decoder_context dc;
    byte_sink dn = {0};
    memset(out, 0, sizeof(*out));
    br_x509_decoder_init(&dc, sink_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (pk == NULL || dn.oom || br_x509_decoder_last_error(&dc) != 0) {
        PyMem_Free(dn.buf);
        return -1;
    }
    out->dn = dn.buf;
    out->ta.dn.data = out->dn;
    out->ta.dn.len = dn.len;
    out->ta.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    out->ta.pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        out->key_a = (unsigned char *)PyMem_Malloc(pk->key.rsa.nlen);
        out->key_b = (unsigned char *)PyMem_Malloc(pk->key.rsa.elen);
        if (!out->key_a || !out->key_b) goto oom;
        memcpy(out->key_a, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(out->key_b, pk->key.rsa.e, pk->key.rsa.elen);
        out->ta.pkey.key.rsa.n = out->key_a;
        out->ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        out->ta.pkey.key.rsa.e = out->key_b;
        out->ta.pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        out->key_a = (unsigned char *)PyMem_Malloc(pk->key.ec.qlen);
        if (!out->key_a) goto oom;
        memcpy(out->key_a, pk->key.ec.q, pk->key.ec.qlen);
        out->ta.pkey.key.ec.curve = pk->key.ec.curve;
        out->ta.pkey.key.ec.q = out->key_a;
        out->ta.pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        goto fail;
    }
    return 0;
oom:
    PyErr_NoMemory();
fail:
    PyMem_Free(out->dn);
    PyMem_Free(out->key_a);
    PyMem_Free(out->key_b);
    memset(out, 0, sizeof(*out));
    return -1;
}

static Py_ssize_t load_ca_bundle(PySSLContext *ctx, const char *pem, size_t pem_len) {
    br_pem_decoder_context pc;
    byte_sink der = {0};
    int in_cert = 0;
    br_pem_decoder_init(&pc);
    br_pem_decoder_setdest(&pc, sink_append, &der);
    size_t cap = 64, n = 0;
    ts_anchor *arr = (ts_anchor *)PyMem_Malloc(cap * sizeof(ts_anchor));
    if (!arr) { PyErr_NoMemory(); return -1; }
    size_t off = 0;
    while (off < pem_len) {
        size_t pushed = br_pem_decoder_push(&pc, pem + off, pem_len - off);
        off += pushed;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ: {
            const char *name = br_pem_decoder_name(&pc);
            in_cert = (name && strcmp(name, "CERTIFICATE") == 0);
            der.len = 0;
            break;
        }
        case BR_PEM_END_OBJ:
            if (in_cert && der.len > 0 && !der.oom) {
                if (n == cap) {
                    cap *= 2;
                    ts_anchor *na = (ts_anchor *)PyMem_Realloc(arr, cap * sizeof(ts_anchor));
                    if (!na) { PyErr_NoMemory(); goto fail; }
                    arr = na;
                }
                if (der_to_anchor(der.buf, der.len, &arr[n]) == 0) n++;
            }
            in_cert = 0;
            break;
        case BR_PEM_ERROR:
            PyErr_SetString(PySSLErrorObject, "PEM decode error in CA bundle");
            goto fail;
        default: break;
        }
        if (der.oom) { PyErr_NoMemory(); goto fail; }
    }
    PyMem_Free(der.buf);
    if (n == 0) {
        PyMem_Free(arr);
        PyErr_SetString(PySSLErrorObject, "no usable CA certificates in bundle");
        return -1;
    }
    /* Build the contiguous br_x509_trust_anchor[] BearSSL indexes by its own
     * stride; the .dn/.pkey pointers still reference the buffers owned by arr. */
    br_x509_trust_anchor *packed =
        (br_x509_trust_anchor *)PyMem_Malloc(n * sizeof(br_x509_trust_anchor));
    if (!packed) {
        for (size_t i = 0; i < n; i++) {
            PyMem_Free(arr[i].dn); PyMem_Free(arr[i].key_a); PyMem_Free(arr[i].key_b);
        }
        PyMem_Free(arr);
        return PyErr_NoMemory() ? -1 : -1;
    }
    for (size_t i = 0; i < n; i++) packed[i] = arr[i].ta;
    ctx->anchors = arr;
    ctx->ta_packed = packed;
    ctx->anchor_count = n;
    return (Py_ssize_t)n;
fail:
    PyMem_Free(der.buf);
    for (size_t i = 0; i < n; i++) {
        PyMem_Free(arr[i].dn); PyMem_Free(arr[i].key_a); PyMem_Free(arr[i].key_b);
    }
    PyMem_Free(arr);
    return -1;
}

/* ================================================================== */
/*  Low-level transport: socket fd (blocking) or memory BIO (async)    */
/*  Returns >0 bytes, 0 = clean EOF, -1 = would-block, -2 = error.     */
/* ================================================================== */

static int ll_read(PySSLSocket *s, unsigned char *buf, size_t len) {
    if (s->sock_fd >= 0) {
        ssize_t r = recv(s->sock_fd, buf, len, 0);
        if (r > 0) return (int)r;
        if (r == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2;
    }
    /* memory BIO path */
    PySSLMemoryBIO *b = s->inbio;
    Py_ssize_t avail = b->len - b->start;
    if (avail <= 0) return b->eof ? 0 : -1;
    size_t n = (avail < (Py_ssize_t)len) ? (size_t)avail : len;
    memcpy(buf, b->buf + b->start, n);
    b->start += (Py_ssize_t)n;
    if (b->start == b->len) { b->start = b->len = 0; }
    return (int)n;
}

static int ll_write(PySSLSocket *s, const unsigned char *buf, size_t len) {
    if (s->sock_fd >= 0) {
        ssize_t w = send(s->sock_fd, buf, len, 0);
        if (w > 0) return (int)w;
        if (w == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2;
    }
    PySSLMemoryBIO *b = s->outbio;
    if (b->len + (Py_ssize_t)len > b->cap) {
        Py_ssize_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + (Py_ssize_t)len) ncap *= 2;
        unsigned char *nb = (unsigned char *)PyMem_Realloc(b->buf, ncap);
        if (!nb) return -2;
        b->buf = nb; b->cap = ncap;
    }
    memcpy(b->buf + b->len, buf, len);
    b->len += (Py_ssize_t)len;
    return (int)len;
}

/* Drive the BearSSL engine until `target_state` bits are ready (or closed).
 * want_app_send: caller wants to push app data; want_app_recv: caller wants
 * app data. Returns 0 ready, 1 clean-closed, -1 python exception set. */
static int ssl_drive(PySSLSocket *s, unsigned want) {
    br_ssl_engine_context *eng = &s->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(eng);
            if (err == BR_ERR_OK) return 1;          /* clean close */
            raise_bearssl_error(err);
            return -1;
        }
        /* flush any pending outgoing records first */
        if (st & BR_SSL_SENDREC) {
            size_t len; unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            int w = ll_write(s, buf, len);
            if (w > 0) { br_ssl_engine_sendrec_ack(eng, (size_t)w); continue; }
            if (w == -1) { PyErr_SetNone(PySSLWantWriteErrorObject); return -1; }
            PyErr_SetString(PySSLSyscallErrorObject, "write failed"); return -1;
        }
        /* satisfied? */
        if ((want & BR_SSL_SENDAPP) && (st & BR_SSL_SENDAPP)) return 0;
        if ((want & BR_SSL_RECVAPP) && (st & BR_SSL_RECVAPP)) return 0;
        if (want == 0 && (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))) return 0;
        /* feed incoming records */
        if (st & BR_SSL_RECVREC) {
            size_t len; unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
            int r = ll_read(s, buf, len);
            if (r > 0) { br_ssl_engine_recvrec_ack(eng, (size_t)r); continue; }
            if (r == 0) { PyErr_SetNone(PySSLEOFErrorObject); return -1; }
            if (r == -1) { PyErr_SetNone(PySSLWantReadErrorObject); return -1; }
            PyErr_SetString(PySSLSyscallErrorObject, "read failed"); return -1;
        }
        /* nothing actionable yet but not satisfied -> need more input */
        PyErr_SetNone(PySSLWantReadErrorObject);
        return -1;
    }
}

/* ================================================================== */
/*  _SSLSocket                                                          */
/* ================================================================== */

static int sslsock_engine_init(PySSLSocket *s) {
    s->iobuf = (unsigned char *)PyMem_Malloc(BR_SSL_BUFSIZE_BIDI);
    if (!s->iobuf) { PyErr_NoMemory(); return -1; }
    br_ssl_client_init_full(&s->sc, &s->xc, s->ctx->ta_packed, s->ctx->anchor_count);
    br_ssl_engine_set_buffer(&s->sc.eng, s->iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    const char *host = NULL;
    if (s->server_hostname && PyUnicode_Check(s->server_hostname))
        host = PyUnicode_AsUTF8(s->server_hostname);
    if (!br_ssl_client_reset(&s->sc, host, 0)) {
        PyErr_SetString(PySSLErrorObject, "br_ssl_client_reset failed");
        return -1;
    }
    return 0;
}

static PyObject *sslsock_do_handshake(PySSLSocket *s, PyObject *noargs) {
    int rc = ssl_drive(s, 0);
    if (rc < 0) return NULL;
    s->handshake_done = 1;
    Py_RETURN_NONE;
}

static PyObject *sslsock_write(PySSLSocket *s, PyObject *args) {
    Py_buffer data;
    if (!PyArg_ParseTuple(args, "y*:write", &data)) return NULL;
    int rc = ssl_drive(s, BR_SSL_SENDAPP);
    if (rc < 0) { PyBuffer_Release(&data); return NULL; }
    size_t len; unsigned char *buf = br_ssl_engine_sendapp_buf(&s->sc.eng, &len);
    size_t n = ((size_t)data.len < len) ? (size_t)data.len : len;
    memcpy(buf, data.buf, n);
    br_ssl_engine_sendapp_ack(&s->sc.eng, n);
    br_ssl_engine_flush(&s->sc.eng, 0);
    if (ssl_drive(s, BR_SSL_SENDAPP) < 0) { PyBuffer_Release(&data); return NULL; }
    PyBuffer_Release(&data);
    return PyLong_FromSize_t(n);
}

static PyObject *sslsock_read(PySSLSocket *s, PyObject *args) {
    int wanted = 0; PyObject *bufobj = NULL;
    if (!PyArg_ParseTuple(args, "i|O:read", &wanted, &bufobj)) return NULL;
    int rc = ssl_drive(s, BR_SSL_RECVAPP);
    if (rc == 1) {            /* clean close => empty */
        if (bufobj && bufobj != Py_None) return PyLong_FromLong(0);
        return PyBytes_FromStringAndSize("", 0);
    }
    if (rc < 0) return NULL;
    size_t len; unsigned char *buf = br_ssl_engine_recvapp_buf(&s->sc.eng, &len);
    size_t n = ((size_t)wanted < len) ? (size_t)wanted : len;
    PyObject *out;
    if (bufobj && bufobj != Py_None) {
        Py_buffer view;
        if (PyObject_GetBuffer(bufobj, &view, PyBUF_WRITABLE) < 0) return NULL;
        if ((size_t)view.len < n) n = (size_t)view.len;
        memcpy(view.buf, buf, n);
        PyBuffer_Release(&view);
        out = PyLong_FromSize_t(n);
    } else {
        out = PyBytes_FromStringAndSize((const char *)buf, n);
    }
    br_ssl_engine_recvapp_ack(&s->sc.eng, n);
    return out;
}

static PyObject *sslsock_shutdown(PySSLSocket *s, PyObject *noargs) {
    br_ssl_engine_close(&s->sc.eng);
    ssl_drive(s, 0);
    PyErr_Clear();
    if (s->sock) { Py_INCREF(s->sock); return s->sock; }
    Py_RETURN_NONE;
}

static PyObject *sslsock_version(PySSLSocket *s, PyObject *noargs) {
    if (!s->handshake_done) Py_RETURN_NONE;
    unsigned v = br_ssl_engine_get_version(&s->sc.eng);
    const char *name;
    switch (v) {
    case BR_TLS10: name = "TLSv1"; break;
    case BR_TLS11: name = "TLSv1.1"; break;
    case BR_TLS12: name = "TLSv1.2"; break;
    default: name = "TLSv1.3"; break;   /* BearSSL 0.6 caps at 1.2; future-proof */
    }
    return PyUnicode_FromString(name);
}

static PyObject *sslsock_cipher(PySSLSocket *s, PyObject *noargs) {
    if (!s->handshake_done) Py_RETURN_NONE;
    br_ssl_session_parameters pp;
    br_ssl_engine_get_session_parameters(&s->sc.eng, &pp);
    return Py_BuildValue("(ssi)", "BearSSL", "TLSv1.2", (int)pp.cipher_suite);
}

static PyObject *sslsock_getpeercert(PySSLSocket *s, PyObject *args) {
    int binary = 0;
    if (!PyArg_ParseTuple(args, "|i:getpeercert", &binary)) return NULL;
    /* The chain is validated by br_x509_minimal during the handshake; we do not
     * retain the EE DER. Return an empty dict (verified) / None for binary. */
    if (binary) Py_RETURN_NONE;
    return PyDict_New();
}

static PyObject *sslsock_ret_none(PySSLSocket *s, PyObject *noargs) { Py_RETURN_NONE; }
static PyObject *sslsock_pending(PySSLSocket *s, PyObject *noargs) {
    return PyLong_FromLong(0);
}

static PyObject *sslsock_get_ctx(PySSLSocket *s, void *c) {
    Py_INCREF(s->ctx); return (PyObject *)s->ctx;
}
static PyObject *sslsock_get_hostname(PySSLSocket *s, void *c) {
    if (s->server_hostname) { Py_INCREF(s->server_hostname); return s->server_hostname; }
    Py_RETURN_NONE;
}
static PyObject *sslsock_get_server_side(PySSLSocket *s, void *c) {
    return PyBool_FromLong(s->server_side);
}
static PyObject *sslsock_get_session(PySSLSocket *s, void *c) { Py_RETURN_NONE; }
static int sslsock_set_session(PySSLSocket *s, PyObject *v, void *c) { return 0; }
static PyObject *sslsock_get_session_reused(PySSLSocket *s, void *c) { Py_RETURN_FALSE; }

static void sslsock_dealloc(PySSLSocket *s) {
    Py_XDECREF(s->ctx);
    Py_XDECREF(s->server_hostname);
    Py_XDECREF(s->sock);
    Py_XDECREF(s->inbio);
    Py_XDECREF(s->outbio);
    PyMem_Free(s->iobuf);
    Py_TYPE(s)->tp_free((PyObject *)s);
}

static PyMethodDef sslsock_methods[] = {
    {"do_handshake", (PyCFunction)sslsock_do_handshake, METH_NOARGS},
    {"write", (PyCFunction)sslsock_write, METH_VARARGS},
    {"read", (PyCFunction)sslsock_read, METH_VARARGS},
    {"shutdown", (PyCFunction)sslsock_shutdown, METH_NOARGS},
    {"version", (PyCFunction)sslsock_version, METH_NOARGS},
    {"cipher", (PyCFunction)sslsock_cipher, METH_NOARGS},
    {"shared_ciphers", (PyCFunction)sslsock_ret_none, METH_NOARGS},
    {"compression", (PyCFunction)sslsock_ret_none, METH_NOARGS},
    {"getpeercert", (PyCFunction)sslsock_getpeercert, METH_VARARGS},
    {"get_verified_chain", (PyCFunction)sslsock_ret_none, METH_NOARGS},
    {"get_unverified_chain", (PyCFunction)sslsock_ret_none, METH_NOARGS},
    {"selected_alpn_protocol", (PyCFunction)sslsock_ret_none, METH_NOARGS},
    {"get_channel_binding", (PyCFunction)sslsock_ret_none, METH_VARARGS},
    {"verify_client_post_handshake", (PyCFunction)sslsock_ret_none, METH_NOARGS},
    {"pending", (PyCFunction)sslsock_pending, METH_NOARGS},
    {NULL, NULL}
};

static PyGetSetDef sslsock_getset[] = {
    {"context", (getter)sslsock_get_ctx, NULL, NULL, NULL},
    {"server_hostname", (getter)sslsock_get_hostname, NULL, NULL, NULL},
    {"server_side", (getter)sslsock_get_server_side, NULL, NULL, NULL},
    {"session", (getter)sslsock_get_session, (setter)sslsock_set_session, NULL, NULL},
    {"session_reused", (getter)sslsock_get_session_reused, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject PySSLSocket_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_ssl._SSLSocket",
    .tp_basicsize = sizeof(PySSLSocket),
    .tp_dealloc = (destructor)sslsock_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = sslsock_methods,
    .tp_getset = sslsock_getset,
};

/* ================================================================== */
/*  _SSLContext                                                         */
/* ================================================================== */

static PyObject *sslctx_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    int proto = TS_PROTO_TLS_CLIENT;
    if (!PyArg_ParseTuple(args, "|i:_SSLContext", &proto)) return NULL;
    PySSLContext *c = (PySSLContext *)type->tp_alloc(type, 0);
    if (!c) return NULL;
    c->anchors = NULL; c->anchor_count = 0;
    c->verify_mode = (proto == TS_PROTO_TLS_CLIENT) ? TS_CERT_REQUIRED : TS_CERT_NONE;
    c->check_hostname = (proto == TS_PROTO_TLS_CLIENT);
    c->protocol = proto;
    c->options = 0;
    c->verify_flags = 0;
    return (PyObject *)c;
}

static void sslctx_dealloc(PySSLContext *c) {
    for (size_t i = 0; i < c->anchor_count; i++) {
        PyMem_Free(c->anchors[i].dn);
        PyMem_Free(c->anchors[i].key_a);
        PyMem_Free(c->anchors[i].key_b);
    }
    PyMem_Free(c->anchors);
    PyMem_Free(c->ta_packed);
    Py_TYPE(c)->tp_free((PyObject *)c);
}

static PyObject *sslctx_load_verify_locations(PySSLContext *c, PyObject *args, PyObject *kw) {
    static char *kwlist[] = {"cafile", "capath", "cadata", NULL};
    PyObject *cafile = Py_None, *capath = Py_None, *cadata = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|OOO:load_verify_locations",
                                     kwlist, &cafile, &capath, &cadata))
        return NULL;
    if (c->anchors) {   /* replace any existing store */
        for (size_t i = 0; i < c->anchor_count; i++) {
            PyMem_Free(c->anchors[i].dn);
            PyMem_Free(c->anchors[i].key_a);
            PyMem_Free(c->anchors[i].key_b);
        }
        PyMem_Free(c->anchors); c->anchors = NULL;
        PyMem_Free(c->ta_packed); c->ta_packed = NULL;
        c->anchor_count = 0;
    }
    if (cafile && cafile != Py_None) {
        const char *path = PyUnicode_AsUTF8(cafile);
        if (!path) return NULL;
        FILE *f = fopen(path, "rb");
        if (!f) { PyErr_SetFromErrnoWithFilename(PyExc_OSError, path); return NULL; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); PyErr_SetString(PySSLErrorObject, "empty cafile"); return NULL; }
        char *pem = (char *)PyMem_Malloc((size_t)sz);
        if (!pem) { fclose(f); return PyErr_NoMemory(); }
        size_t rd = fread(pem, 1, (size_t)sz, f);
        fclose(f);
        Py_ssize_t rc = load_ca_bundle(c, pem, rd);
        PyMem_Free(pem);
        if (rc < 0) return NULL;
    } else if (cadata && cadata != Py_None && PyUnicode_Check(cadata)) {
        Py_ssize_t len; const char *pem = PyUnicode_AsUTF8AndSize(cadata, &len);
        if (!pem) return NULL;
        if (load_ca_bundle(c, pem, (size_t)len) < 0) return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *make_sslsock(PySSLContext *c, PyObject *sock, int server_side,
                              PyObject *hostname, PySSLMemoryBIO *inbio,
                              PySSLMemoryBIO *outbio) {
    if (c->anchor_count == 0 && c->verify_mode == TS_CERT_REQUIRED) {
        PyErr_SetString(PySSLErrorObject,
            "CERT_REQUIRED but no CA certificates loaded");
        return NULL;
    }
    PySSLSocket *s = PyObject_New(PySSLSocket, &PySSLSocket_Type);
    if (!s) return NULL;
    memset(((char *)s) + sizeof(PyObject), 0, sizeof(PySSLSocket) - sizeof(PyObject));
    Py_INCREF(c); s->ctx = c;
    s->server_side = server_side;
    s->server_hostname = NULL;
    if (hostname && hostname != Py_None) { Py_INCREF(hostname); s->server_hostname = hostname; }
    s->sock_fd = -1; s->sock = NULL; s->inbio = NULL; s->outbio = NULL;
    if (sock && sock != Py_None) {
        PyObject *fno = PyObject_CallMethod(sock, "fileno", NULL);
        if (!fno) { Py_DECREF(s); return NULL; }
        s->sock_fd = (int)PyLong_AsLong(fno);
        Py_DECREF(fno);
        Py_INCREF(sock); s->sock = sock;
    } else {
        Py_INCREF(inbio); s->inbio = inbio;
        Py_INCREF(outbio); s->outbio = outbio;
    }
    if (sslsock_engine_init(s) < 0) { Py_DECREF(s); return NULL; }
    return (PyObject *)s;
}

static PyObject *sslctx_wrap_socket(PySSLContext *c, PyObject *args, PyObject *kw) {
    static char *kwlist[] = {"sock", "server_side", "server_hostname",
                             "owner", "session", NULL};
    PyObject *sock, *hostname = Py_None, *owner = Py_None, *session = Py_None;
    int server_side = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Oi|OOO:_wrap_socket", kwlist,
            &sock, &server_side, &hostname, &owner, &session))
        return NULL;
    return make_sslsock(c, sock, server_side, hostname, NULL, NULL);
}

static PyObject *sslctx_wrap_bio(PySSLContext *c, PyObject *args, PyObject *kw) {
    static char *kwlist[] = {"incoming", "outgoing", "server_side",
                             "server_hostname", "owner", "session", NULL};
    PyObject *inbio, *outbio, *hostname = Py_None, *owner = Py_None, *session = Py_None;
    int server_side = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOi|OOO:_wrap_bio", kwlist,
            &inbio, &outbio, &server_side, &hostname, &owner, &session))
        return NULL;
    if (!PyObject_TypeCheck(inbio, &PySSLMemoryBIO_Type) ||
        !PyObject_TypeCheck(outbio, &PySSLMemoryBIO_Type)) {
        PyErr_SetString(PyExc_TypeError, "expected MemoryBIO");
        return NULL;
    }
    return make_sslsock(c, NULL, server_side, hostname,
                        (PySSLMemoryBIO *)inbio, (PySSLMemoryBIO *)outbio);
}

static PyObject *sslctx_noop(PySSLContext *c, PyObject *a) { Py_RETURN_NONE; }
static PyObject *sslctx_set_ciphers(PySSLContext *c, PyObject *a) { Py_RETURN_NONE; }
static PyObject *sslctx_get_ca_certs(PySSLContext *c, PyObject *a, PyObject *k) {
    return PyList_New(0);
}
static PyObject *sslctx_cert_store_stats(PySSLContext *c, PyObject *a) {
    return Py_BuildValue("{s:n,s:i,s:i}", "x509", (Py_ssize_t)c->anchor_count,
                         "crl", 0, "x509_ca", (int)c->anchor_count);
}

static PyObject *ctx_get_verify(PySSLContext *c, void *x) { return PyLong_FromLong(c->verify_mode); }
static int ctx_set_verify(PySSLContext *c, PyObject *v, void *x) {
    long m = PyLong_AsLong(v); if (m == -1 && PyErr_Occurred()) return -1;
    c->verify_mode = (int)m; return 0;
}
static PyObject *ctx_get_check(PySSLContext *c, void *x) { return PyBool_FromLong(c->check_hostname); }
static int ctx_set_check(PySSLContext *c, PyObject *v, void *x) {
    int t = PyObject_IsTrue(v); if (t < 0) return -1; c->check_hostname = t; return 0;
}
static PyObject *ctx_get_options(PySSLContext *c, void *x) { return PyLong_FromLong(c->options); }
static int ctx_set_options(PySSLContext *c, PyObject *v, void *x) {
    long o = PyLong_AsLong(v); if (o == -1 && PyErr_Occurred()) return -1;
    c->options = o; return 0;
}
static PyObject *ctx_get_vflags(PySSLContext *c, void *x) { return PyLong_FromLong(c->verify_flags); }
static int ctx_set_vflags(PySSLContext *c, PyObject *v, void *x) {
    long o = PyLong_AsLong(v); if (o == -1 && PyErr_Occurred()) return -1;
    c->verify_flags = o; return 0;
}
static PyObject *ctx_get_proto(PySSLContext *c, void *x) { return PyLong_FromLong(c->protocol); }
static PyObject *ctx_get_minmax(PySSLContext *c, void *x) { return PyLong_FromLong(0); }
static int ctx_set_minmax(PySSLContext *c, PyObject *v, void *x) { return 0; }

static PyMethodDef sslctx_methods[] = {
    {"load_verify_locations", (PyCFunction)sslctx_load_verify_locations,
        METH_VARARGS | METH_KEYWORDS},
    {"_wrap_socket", (PyCFunction)sslctx_wrap_socket, METH_VARARGS | METH_KEYWORDS},
    {"_wrap_bio", (PyCFunction)sslctx_wrap_bio, METH_VARARGS | METH_KEYWORDS},
    {"load_cert_chain", (PyCFunction)sslctx_noop, METH_VARARGS | METH_KEYWORDS},
    {"load_dh_params", (PyCFunction)sslctx_noop, METH_VARARGS},
    {"set_ciphers", (PyCFunction)sslctx_set_ciphers, METH_VARARGS},
    {"set_alpn_protocols", (PyCFunction)sslctx_noop, METH_VARARGS},
    {"_set_alpn_protocols", (PyCFunction)sslctx_noop, METH_VARARGS},
    {"get_ca_certs", (PyCFunction)sslctx_get_ca_certs, METH_VARARGS | METH_KEYWORDS},
    {"cert_store_stats", (PyCFunction)sslctx_cert_store_stats, METH_NOARGS},
    {"set_default_verify_paths", (PyCFunction)sslctx_noop, METH_NOARGS},
    {NULL, NULL}
};

static PyGetSetDef sslctx_getset[] = {
    {"verify_mode", (getter)ctx_get_verify, (setter)ctx_set_verify, NULL, NULL},
    {"check_hostname", (getter)ctx_get_check, (setter)ctx_set_check, NULL, NULL},
    {"options", (getter)ctx_get_options, (setter)ctx_set_options, NULL, NULL},
    {"verify_flags", (getter)ctx_get_vflags, (setter)ctx_set_vflags, NULL, NULL},
    {"protocol", (getter)ctx_get_proto, NULL, NULL, NULL},
    {"minimum_version", (getter)ctx_get_minmax, (setter)ctx_set_minmax, NULL, NULL},
    {"maximum_version", (getter)ctx_get_minmax, (setter)ctx_set_minmax, NULL, NULL},
    {NULL}
};

static PyTypeObject PySSLContext_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_ssl._SSLContext",
    .tp_basicsize = sizeof(PySSLContext),
    .tp_dealloc = (destructor)sslctx_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_methods = sslctx_methods,
    .tp_getset = sslctx_getset,
    .tp_new = sslctx_new,
};

/* ================================================================== */
/*  MemoryBIO                                                           */
/* ================================================================== */

static PyObject *membio_new(PyTypeObject *t, PyObject *a, PyObject *k) {
    PySSLMemoryBIO *b = (PySSLMemoryBIO *)t->tp_alloc(t, 0);
    if (!b) return NULL;
    b->buf = NULL; b->start = b->len = b->cap = 0; b->eof = 0;
    return (PyObject *)b;
}
static void membio_dealloc(PySSLMemoryBIO *b) {
    PyMem_Free(b->buf); Py_TYPE(b)->tp_free((PyObject *)b);
}
static PyObject *membio_write(PySSLMemoryBIO *b, PyObject *args) {
    Py_buffer d;
    if (!PyArg_ParseTuple(args, "y*:write", &d)) return NULL;
    if (b->len + d.len > b->cap) {
        Py_ssize_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + d.len) ncap *= 2;
        unsigned char *nb = (unsigned char *)PyMem_Realloc(b->buf, ncap);
        if (!nb) { PyBuffer_Release(&d); return PyErr_NoMemory(); }
        b->buf = nb; b->cap = ncap;
    }
    memcpy(b->buf + b->len, d.buf, d.len);
    b->len += d.len;
    Py_ssize_t w = d.len;
    PyBuffer_Release(&d);
    return PyLong_FromSsize_t(w);
}
static PyObject *membio_read(PySSLMemoryBIO *b, PyObject *args) {
    Py_ssize_t want = -1;
    if (!PyArg_ParseTuple(args, "|n:read", &want)) return NULL;
    Py_ssize_t avail = b->len - b->start;
    if (want < 0 || want > avail) want = avail;
    PyObject *out = PyBytes_FromStringAndSize((const char *)(b->buf + b->start), want);
    if (!out) return NULL;
    b->start += want;
    if (b->start == b->len) b->start = b->len = 0;
    return out;
}
static PyObject *membio_get_pending(PySSLMemoryBIO *b, void *x) {
    return PyLong_FromSsize_t(b->len - b->start);
}
static PyObject *membio_get_eof(PySSLMemoryBIO *b, void *x) {
    return PyBool_FromLong(b->eof && (b->len - b->start) == 0);
}
static PyObject *membio_write_eof(PySSLMemoryBIO *b, PyObject *no) {
    b->eof = 1; Py_RETURN_NONE;
}
static PyMethodDef membio_methods[] = {
    {"read", (PyCFunction)membio_read, METH_VARARGS},
    {"write", (PyCFunction)membio_write, METH_VARARGS},
    {"write_eof", (PyCFunction)membio_write_eof, METH_NOARGS},
    {NULL, NULL}
};
static PyGetSetDef membio_getset[] = {
    {"pending", (getter)membio_get_pending, NULL, NULL, NULL},
    {"eof", (getter)membio_get_eof, NULL, NULL, NULL},
    {NULL}
};
static PyTypeObject PySSLMemoryBIO_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_ssl.MemoryBIO",
    .tp_basicsize = sizeof(PySSLMemoryBIO),
    .tp_dealloc = (destructor)membio_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = membio_methods,
    .tp_getset = membio_getset,
    .tp_new = membio_new,
};

/* ================================================================== */
/*  SSLSession (opaque, minimal)                                       */
/* ================================================================== */
static void sslsession_dealloc(PySSLSession *s) {
    Py_XDECREF(s->params); Py_TYPE(s)->tp_free((PyObject *)s);
}
static PyTypeObject PySSLSession_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_ssl.SSLSession",
    .tp_basicsize = sizeof(PySSLSession),
    .tp_dealloc = (destructor)sslsession_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

/* ================================================================== */
/*  Module-level functions                                             */
/* ================================================================== */
/* Return the _ASN1Object 4-tuple (nid, shortname, longname, oid) ssl.py's
 * _ASN1Object.__new__ unpacks. BearSSL has no OID registry; echo the text as
 * the OID — Purpose/_ASN1Object only need a stable, comparable oid string. */
static PyObject *m_txt2obj(PyObject *self, PyObject *args, PyObject *kw) {
    static char *kwlist[] = {"txt", "name", NULL};
    const char *txt; int name = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|p:txt2obj", kwlist, &txt, &name))
        return NULL;
    return Py_BuildValue("(isss)", 0, txt, txt, txt);
}
static PyObject *m_nid2obj(PyObject *self, PyObject *args) {
    int nid;
    if (!PyArg_ParseTuple(args, "i:nid2obj", &nid)) return NULL;
    return Py_BuildValue("(isss)", nid, "", "", "");
}
static PyObject *m_rand_status(PyObject *self, PyObject *a) { return PyLong_FromLong(1); }
static PyObject *m_rand_add(PyObject *self, PyObject *a) { Py_RETURN_NONE; }
static PyObject *m_rand_bytes(PyObject *self, PyObject *args) {
    int n;
    if (!PyArg_ParseTuple(args, "i:RAND_bytes", &n)) return NULL;
    if (n < 0) { PyErr_SetString(PyExc_ValueError, "negative"); return NULL; }
    PyObject *o = PyBytes_FromStringAndSize(NULL, n);
    if (!o) return NULL;
    br_hmac_drbg_context rng;
    br_hmac_drbg_init(&rng, &br_sha256_vtable, "tatertos-rng", 12);
    br_hmac_drbg_generate(&rng, PyBytes_AS_STRING(o), (size_t)n);
    return o;
}
static PyObject *m_get_default_verify_paths(PyObject *self, PyObject *a) {
    return Py_BuildValue("(ssss)", "SSL_CERT_FILE",
        "/etc/ssl/certs/ca-certificates.crt", "SSL_CERT_DIR", "/etc/ssl/certs");
}
static PyMethodDef module_methods[] = {
    {"txt2obj", (PyCFunction)m_txt2obj, METH_VARARGS | METH_KEYWORDS},
    {"nid2obj", (PyCFunction)m_nid2obj, METH_VARARGS},
    {"RAND_status", m_rand_status, METH_NOARGS},
    {"RAND_add", m_rand_add, METH_VARARGS},
    {"RAND_bytes", m_rand_bytes, METH_VARARGS},
    {"get_default_verify_paths", m_get_default_verify_paths, METH_NOARGS},
    {NULL, NULL}
};

/* ================================================================== */
/*  Module init                                                        */
/* ================================================================== */
static struct PyModuleDef ssl_module = {
    PyModuleDef_HEAD_INIT, "_ssl", "Native TLS (BearSSL) for TaterTOS64",
    -1, module_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__ssl(void) {
    if (PyType_Ready(&PySSLContext_Type) < 0) return NULL;
    if (PyType_Ready(&PySSLSocket_Type) < 0) return NULL;
    if (PyType_Ready(&PySSLMemoryBIO_Type) < 0) return NULL;
    if (PyType_Ready(&PySSLSession_Type) < 0) return NULL;

    PyObject *m = PyModule_Create(&ssl_module);
    if (!m) return NULL;

    PySSLErrorObject = PyErr_NewException("ssl.SSLError", PyExc_OSError, NULL);
    PySSLZeroReturnErrorObject = PyErr_NewException("ssl.SSLZeroReturnError", PySSLErrorObject, NULL);
    PySSLWantReadErrorObject = PyErr_NewException("ssl.SSLWantReadError", PySSLErrorObject, NULL);
    PySSLWantWriteErrorObject = PyErr_NewException("ssl.SSLWantWriteError", PySSLErrorObject, NULL);
    PySSLSyscallErrorObject = PyErr_NewException("ssl.SSLSyscallError", PySSLErrorObject, NULL);
    PySSLEOFErrorObject = PyErr_NewException("ssl.SSLEOFError", PySSLErrorObject, NULL);
    PySSLCertVerificationErrorObject = PyErr_NewException("ssl.SSLCertVerificationError",
        PySSLErrorObject, NULL);

#define ADD(name, obj) do { Py_INCREF(obj); PyModule_AddObject(m, name, obj); } while (0)
    ADD("SSLError", PySSLErrorObject);
    ADD("SSLZeroReturnError", PySSLZeroReturnErrorObject);
    ADD("SSLWantReadError", PySSLWantReadErrorObject);
    ADD("SSLWantWriteError", PySSLWantWriteErrorObject);
    ADD("SSLSyscallError", PySSLSyscallErrorObject);
    ADD("SSLEOFError", PySSLEOFErrorObject);
    ADD("SSLCertVerificationError", PySSLCertVerificationErrorObject);
    ADD("_SSLContext", (PyObject *)&PySSLContext_Type);
    ADD("MemoryBIO", (PyObject *)&PySSLMemoryBIO_Type);
    ADD("SSLSession", (PyObject *)&PySSLSession_Type);
#undef ADD

    PyModule_AddIntConstant(m, "CERT_NONE", TS_CERT_NONE);
    PyModule_AddIntConstant(m, "CERT_OPTIONAL", TS_CERT_OPTIONAL);
    PyModule_AddIntConstant(m, "CERT_REQUIRED", TS_CERT_REQUIRED);
    PyModule_AddIntConstant(m, "PROTOCOL_TLS", TS_PROTO_TLS);
    PyModule_AddIntConstant(m, "PROTOCOL_TLS_CLIENT", TS_PROTO_TLS_CLIENT);
    PyModule_AddIntConstant(m, "PROTOCOL_TLS_SERVER", TS_PROTO_TLS_SERVER);
    PyModule_AddIntConstant(m, "PROTO_MINIMUM_SUPPORTED", -2);
    PyModule_AddIntConstant(m, "PROTO_MAXIMUM_SUPPORTED", -1);
    PyModule_AddIntConstant(m, "PROTO_SSLv3", 0x0300);
    PyModule_AddIntConstant(m, "PROTO_TLSv1", 0x0301);
    PyModule_AddIntConstant(m, "PROTO_TLSv1_1", 0x0302);
    PyModule_AddIntConstant(m, "PROTO_TLSv1_2", 0x0303);
    PyModule_AddIntConstant(m, "PROTO_TLSv1_3", 0x0304);
    PyModule_AddIntConstant(m, "VERIFY_DEFAULT", 0);
    PyModule_AddIntConstant(m, "VERIFY_CRL_CHECK_LEAF", 0x4);
    PyModule_AddIntConstant(m, "VERIFY_CRL_CHECK_CHAIN", 0xC);
    PyModule_AddIntConstant(m, "VERIFY_X509_STRICT", 0x20);
    PyModule_AddIntConstant(m, "VERIFY_X509_PARTIAL_CHAIN", 0x80000);
    PyModule_AddIntConstant(m, "VERIFY_X509_TRUSTED_FIRST", 0x8000);
    PyModule_AddIntConstant(m, "HOSTFLAG_NEVER_CHECK_SUBJECT", 0x20);
    /* OP_* -> ssl.Options (IntFlag). OpenSSL values; honored where meaningful. */
    PyModule_AddIntConstant(m, "OP_ALL", 0x80000054L);
    PyModule_AddIntConstant(m, "OP_NO_SSLv2", 0x0);
    PyModule_AddIntConstant(m, "OP_NO_SSLv3", 0x02000000L);
    PyModule_AddIntConstant(m, "OP_NO_TLSv1", 0x04000000L);
    PyModule_AddIntConstant(m, "OP_NO_TLSv1_1", 0x10000000L);
    PyModule_AddIntConstant(m, "OP_NO_TLSv1_2", 0x08000000L);
    PyModule_AddIntConstant(m, "OP_NO_TLSv1_3", 0x20000000L);
    PyModule_AddIntConstant(m, "OP_CIPHER_SERVER_PREFERENCE", 0x00400000L);
    PyModule_AddIntConstant(m, "OP_SINGLE_DH_USE", 0x00100000L);
    PyModule_AddIntConstant(m, "OP_SINGLE_ECDH_USE", 0x00080000L);
    PyModule_AddIntConstant(m, "OP_NO_COMPRESSION", 0x00020000L);
    PyModule_AddIntConstant(m, "OP_NO_TICKET", 0x00004000L);
    PyModule_AddIntConstant(m, "OP_NO_RENEGOTIATION", 0x40000000L);
    PyModule_AddIntConstant(m, "OP_ENABLE_MIDDLEBOX_COMPAT", 0x00100000L);
    PyModule_AddIntConstant(m, "OP_IGNORE_UNEXPECTED_EOF", 0x00000080L);
    PyModule_AddIntConstant(m, "OP_LEGACY_SERVER_CONNECT", 0x00000004L);
    /* SSL_ERROR_* -> ssl.SSLErrorNumber */
    PyModule_AddIntConstant(m, "SSL_ERROR_NONE", 0);
    PyModule_AddIntConstant(m, "SSL_ERROR_SSL", 1);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_READ", 2);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_WRITE", 3);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_X509_LOOKUP", 4);
    PyModule_AddIntConstant(m, "SSL_ERROR_SYSCALL", 5);
    PyModule_AddIntConstant(m, "SSL_ERROR_ZERO_RETURN", 6);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_CONNECT", 7);
    PyModule_AddIntConstant(m, "SSL_ERROR_EOF", 8);
    PyModule_AddIntConstant(m, "SSL_ERROR_INVALID_ERROR_CODE", 10);
    /* ALERT_DESCRIPTION_* -> ssl.AlertDescription */
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_CLOSE_NOTIFY", 0);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_UNEXPECTED_MESSAGE", 10);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_BAD_RECORD_MAC", 20);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_HANDSHAKE_FAILURE", 40);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_BAD_CERTIFICATE", 42);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_UNSUPPORTED_CERTIFICATE", 43);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_CERTIFICATE_EXPIRED", 45);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_CERTIFICATE_UNKNOWN", 46);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_ILLEGAL_PARAMETER", 47);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_UNKNOWN_CA", 48);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_ACCESS_DENIED", 49);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_DECODE_ERROR", 50);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_DECRYPT_ERROR", 51);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_PROTOCOL_VERSION", 70);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_INTERNAL_ERROR", 80);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_USER_CANCELLED", 90);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_NO_RENEGOTIATION", 100);
    PyModule_AddIntConstant(m, "ALERT_DESCRIPTION_UNSUPPORTED_EXTENSION", 110);
    PyModule_AddIntConstant(m, "ENCODING_DER", 0);
    PyModule_AddIntConstant(m, "ENCODING_PEM", 1);
    PyModule_AddIntConstant(m, "OPENSSL_VERSION_NUMBER", 0);
    PyModule_AddIntConstant(m, "_OPENSSL_API_VERSION", 0);
    PyModule_AddIntConstant(m, "HAS_SNI", 1);
    PyModule_AddIntConstant(m, "HAS_ECDH", 1);
    PyModule_AddIntConstant(m, "HAS_NPN", 0);
    PyModule_AddIntConstant(m, "HAS_ALPN", 0);
    PyModule_AddIntConstant(m, "HAS_SSLv2", 0);
    PyModule_AddIntConstant(m, "HAS_SSLv3", 0);
    PyModule_AddIntConstant(m, "HAS_TLSv1", 1);
    PyModule_AddIntConstant(m, "HAS_TLSv1_1", 1);
    PyModule_AddIntConstant(m, "HAS_TLSv1_2", 1);
    PyModule_AddIntConstant(m, "HAS_TLSv1_3", 0);
    PyModule_AddIntConstant(m, "HAS_PSK", 0);
    PyModule_AddIntConstant(m, "HAS_PHA", 0);
    PyModule_AddStringConstant(m, "OPENSSL_VERSION", "BearSSL 0.6 (TaterTOS native _ssl)");
    PyModule_AddStringConstant(m, "_DEFAULT_CIPHERS", "DEFAULT");
    PyObject *vinfo = Py_BuildValue("(iiiii)", 0, 6, 0, 0, 0);
    if (vinfo) PyModule_AddObject(m, "OPENSSL_VERSION_INFO", vinfo);
    return m;
}
