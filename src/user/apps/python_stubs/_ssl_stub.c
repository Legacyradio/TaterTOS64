/*
 * _ssl module stub for TaterTOS64v3
 *
 * CPython's _ssl module requires full OpenSSL API (400+ types/functions).
 * Until BearSSL wrapper or OpenSSL cross-compile is complete, this stub
 * provides a graceful ImportError: "No module named '_ssl'"
 *
 * This is referenced by Modules/config.c as PyInit__ssl.
 */
#include "Python.h"

PyMODINIT_FUNC
PyInit__ssl(void)
{
    PyErr_SetString(PyExc_ImportError,
        "No module named '_ssl' — TaterTOS64 SSL not yet implemented");
    return NULL;
}
