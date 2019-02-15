###########################################################################
#
# This program is part of Zenoss Core, an open source monitoring platform.
# Copyright (C) 2008-2010, Zenoss Inc.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2, or (at your
# option) any later version, as published by the Free Software Foundation.
#
# For complete information please visit: http://www.zenoss.com/oss/
#
###########################################################################

from pysamba.library import *
from pysamba.rpc.credentials import CRED_SPECIFIED
import logging

log = logging.getLogger('p.composite_context')

( COMPOSITE_STATE_INIT, COMPOSITE_STATE_IN_PROGRESS,
  COMPOSITE_STATE_DONE, COMPOSITE_STATE_ERROR ) = range(4)

class composite_context(Structure): pass
composite_context_callback = CFUNCTYPE(None, POINTER(composite_context))
class async(Structure):
    _fields_ = [
        ('fn', composite_context_callback),
        ('private_data', c_void_p),
        ]

composite_context._fields_ = [
    ('state', enum),
    ('private_data', c_void_p),
    ('status', NTSTATUS),
    ('event_ctx', c_void_p), # struct event_context *
    ('async', async),
    ('used_wait', BOOL),
    ]

# _PUBLIC_ struct composite_context *composite_create(TALLOC_CTX *mem_ctx,
#                                                     struct event_context *ev);

library.composite_create.restype = POINTER(composite_context)
library.composite_create.argtypes = [c_void_p, c_void_p]
library.composite_create = logFuncCall(library.composite_create)

def composite_create(memctx, eventContext):
    result = library.composite_create(memctx, eventContext)
    if not result:
        raise RuntimeError("Unable to allocate a composite_context")
    return result

# _PUBLIC_ BOOL composite_nomem(const void *p, struct composite_context *ctx);
library.composite_nomem.restype = BOOL
library.composite_nomem.argtypes = [c_void_p, POINTER(composite_context)]
library.composite_nomem = logFuncCall(library.composite_nomem)

library.composite_wait.restype = NTSTATUS
library.composite_wait.argtypes = [POINTER(composite_context)]
library.composite_wait = logFuncCall(library.composite_wait)
library.composite_is_ok.restype = BOOL
library.composite_is_ok.argtypes = [POINTER(composite_context)]
library.composite_is_ok = logFuncCall(library.composite_is_ok)
library.composite_error.restype = None
library.composite_error.argtypes = [POINTER(composite_context), NTSTATUS]
library.composite_error = logFuncCall(library.composite_error)
library.composite_done.restype = None
library.composite_done.argtypes = [POINTER(composite_context)]
library.composite_done = logFuncCall(library.composite_done)
