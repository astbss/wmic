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

__doc__= "Re-implement the talloc macros in a python-compatible way"

from pysamba.library import library, logFuncCall
from ctypes import *
class TallocError(Exception): pass

# function wrapper to check for out-of-memory and turn it into an exception
def check(f):
    def inner(*args, **kw):
        res = f(*args, **kw)
        if not res:
            raise TallocError("Out of memory - %08x" % res)
        return res
    inner.__name__ = f.__name__
    return inner

@logFuncCall
@check
def talloc_zero(ctx, type):
    typename = 'struct ' + type.__name__
    return cast(library._talloc_zero(ctx,
                                     sizeof(type),
                                     typename),
                POINTER(type))

#char *talloc_asprintf(const void *t, const char *fmt, ...) PRINTF_ATTRIBUTE(2,3);
library.talloc_asprintf.restype = c_char_p
library.talloc_asprintf = logFuncCall(library.talloc_asprintf)

@logFuncCall
@check
def talloc_asprintf(*args):
    ctx  = args[0]
    fmt = args[1]
    s = fmt % args[2:]
    ret = library.talloc_strdup(ctx, s)
    return ret

def talloc_get_type(obj, type):
    result = library.talloc_check_name(obj, 'struct ' + type.__name__)
    if not result:
        raise TallocError("Probable mis-interpretation of memory block: "
                          "Have %s, wanted %s" % (talloc_get_name(obj),
                                                  'struct ' + type.__name__))
    return cast(result, POINTER(type))

@logFuncCall
@check
def talloc_array(ctx, type, count):
    obj = library._talloc_array(ctx,
                                sizeof(type),
                                count,
                                'struct ' + type.__name__)
    return cast(obj, POINTER(type))

talloc_free = library.talloc_free
talloc_increase_ref_count = library.talloc_increase_ref_count
talloc_get_name = library.talloc_get_name
