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

__doc__ = "Provide basic access to the Samba library"

import os
import sys
from ctypes import *
from ctypes.util import find_library
import logging

logging.basicConfig()
log = logging.getLogger('zen.pysamba')

def samba_find_library(library_name):
    # normal look-up
    import glob
    library_location = find_library(library_name)

    # go hunting for it
    if not library_location:
        # along the elements of the dynamic library load path
        path = os.environ.get('LD_LIBRARY_PATH', '')
        for directory in path.split(":") + sys.path:
            # with various endings, depending on platform
            for prefix in 'lib','':
                for ending in ".so", ".dylib":
                    pattern = os.path.join(directory,
                                           prefix + library_name + ending) + '*'
                    for filename in sorted(glob.glob(pattern)):
                        return filename

if "DEBUG_LIB" in os.environ:
    library = CDLL(os.environ["DEBUG_LIB"])
else:
    library = CDLL(samba_find_library('async_wmi_lib'))
if not library:
    raise ImportError("Unable to load initial pysamba library")
DEBUGLEVEL = c_int.in_dll(library, 'DEBUGLEVEL')

DEBUG_LOGGING = ("PYSAMBA_DEBUG" in os.environ)
if DEBUG_LOGGING:
    internalLog = logging.getLogger('zen.pysamba.internal')
    DEBUGLEVEL.value = 9

    def logFuncCall(f):
        def wrapper(*args, **kw):
            retstat = "FAIL"
            try:
                internalLog.debug("called %s" % f.__name__)
                res = f(*args, **kw)
                retstat = "PASS"
                return res
            finally:
                internalLog.debug("leaving %s (%s)" % (f.__name__, retstat))
        return wrapper
else:
    # define do-nothing decorator
    logFuncCall = lambda f : f
    DEBUGLEVEL.value = 0

# definitions common to most samba structures
enum = c_uint
BOOL = c_int
class NTSTATUS(Structure):
    _fields_ = [
        ('v', c_uint)
        ]
class WERROR(Structure):
    _fields_ = [
        ('v', c_uint)
        ]
if sizeof(c_uint32) == sizeof(c_void_p):
    size_t = c_uint32
else:
    size_t = c_uint64
int64_t = c_int64
uint64_t = c_uint64
uint32_t = c_uint32
int32_t = c_int32
uint8_t = c_uint8
int8_t = c_int8
uint16_t = c_uint16
int16_t = c_int16


#const char *nt_errstr(NTSTATUS nt_code);
library.nt_errstr.restype = c_char_p
library.nt_errstr.argtypes = [NTSTATUS]
library.nt_errstr = logFuncCall(library.nt_errstr)

#const char *win_errstr(WERROR werror);
library.win_errstr.restype = c_char_p
library.win_errstr.argtypes = [WERROR]
library.win_errstr = logFuncCall(library.win_errstr)

#NTSTATUS dcerpc_init(void);
library.dcerpc_init.restype = NTSTATUS
library.dcerpc_init.argtypes = []
library.dcerpc_init = logFuncCall(library.dcerpc_init)

#NTSTATUS dcerpc_table_init(void);
library.dcerpc_table_init.restype = NTSTATUS
library.dcerpc_table_init.argtypes = []
library.dcerpc_table_init = logFuncCall(library.dcerpc_table_init)

library.cli_credentials_init.restype = c_void_p
library.cli_credentials_init.argtypes = [c_void_p]
library.cli_credentials_init = logFuncCall(library.cli_credentials_init)

library.cli_credentials_set_conf.restype = None
library.cli_credentials_set_conf.argtypes = [c_void_p]
library.cli_credentials_set_conf = logFuncCall(library.cli_credentials_set_conf)

library.cli_credentials_parse_string.restype = None
library.cli_credentials_parse_string.argtypes = [c_void_p, c_void_p, enum]
library.cli_credentials_parse_string = logFuncCall(library.cli_credentials_parse_string)

library._talloc.restype = c_void_p
library._talloc.argtypes = [c_void_p, c_size_t]
library._talloc = logFuncCall(library._talloc)
library.talloc_increase_ref_count.restype = c_int
library.talloc_increase_ref_count.argtypes = [c_void_p]
library.talloc_increase_ref_count = logFuncCall(library.talloc_increase_ref_count)
library.talloc_reference_count.restype = c_size_t
library.talloc_reference_count.argtypes = [c_void_p]
library.talloc_reference_count = logFuncCall(library.talloc_reference_count)
library._talloc_reference.restype = c_void_p
library._talloc_reference.argtypes = [c_void_p, c_void_p]
library._talloc_reference = logFuncCall(library._talloc_reference)
library.talloc_unlink.restype = c_int
library.talloc_unlink.argtypes = [c_void_p, c_void_p]
library.talloc_unlink = logFuncCall(library.talloc_unlink)
library.talloc_set_name_const.restype = None
library.talloc_set_name_const.argtypes = [c_void_p, c_char_p]
library.talloc_set_name_const = logFuncCall(library.talloc_set_name_const)
library.talloc_named_const.restype = c_void_p
library.talloc_named_const.argtypes = [c_void_p, c_size_t, c_char_p]
library.talloc_named_const = logFuncCall(library.talloc_named_const)
library.talloc_get_name.restype = c_char_p
library.talloc_get_name.argtypes = [c_void_p]
library.talloc_get_name = logFuncCall(library.talloc_get_name)
library.talloc_check_name.restype = c_void_p
library.talloc_check_name.argtypes = [c_void_p, c_char_p]
library.talloc_check_name = logFuncCall(library.talloc_check_name)
library.talloc_parent.restype = c_void_p
library.talloc_parent.argtypes = [c_void_p]
library.talloc_parent = logFuncCall(library.talloc_parent)
library.talloc_free.restype = c_int
library.talloc_free.argtypes = [c_void_p]
library.talloc_free = logFuncCall(library.talloc_free)
library.talloc_free_children.restype = None
library.talloc_free_children.argtypes = [c_void_p]
library.talloc_free_children = logFuncCall(library.talloc_free_children)
library._talloc_realloc.restype = c_void_p
library._talloc_realloc.argtypes = [c_void_p, c_void_p, c_size_t, c_char_p]
library._talloc_realloc = logFuncCall(library._talloc_realloc)
library._talloc_steal.restype = c_void_p
library._talloc_steal.argtypes = [c_void_p, c_void_p]
library._talloc_steal = logFuncCall(library._talloc_steal)
library._talloc_move.restype = c_void_p
library._talloc_move.argtypes = [c_void_p, c_void_p]
library._talloc_move = logFuncCall(library._talloc_move)
library.talloc_total_size.restype = c_size_t
library.talloc_total_size.argtypes = [c_void_p]
library.talloc_total_size = logFuncCall(library.talloc_total_size)
library.talloc_total_blocks.restype = c_size_t
library.talloc_total_blocks.argtypes = [c_void_p]
library.talloc_total_blocks = logFuncCall(library.talloc_total_blocks)
library.talloc_report_depth_file.restype = None
library.talloc_report_depth_file.argtypes = [c_void_p, c_int, c_int, c_void_p]
library.talloc_report_depth_file = logFuncCall(library.talloc_report_depth_file)
library.talloc_report_full.restype = None
library.talloc_report_full.argtypes = [c_void_p, c_void_p]
library.talloc_report_full = logFuncCall(library.talloc_report_full)
library.talloc_report.restype = None
library.talloc_report.argtypes = [c_void_p, c_void_p]
library.talloc_report = logFuncCall(library.talloc_report)
library.talloc_enable_null_tracking.restype = None
library.talloc_enable_null_tracking.argtypes = []
library.talloc_enable_null_tracking = logFuncCall(library.talloc_enable_null_tracking)
library.talloc_disable_null_tracking.restype = None
library.talloc_disable_null_tracking.argtypes = []
library.talloc_disable_null_tracking = logFuncCall(library.talloc_disable_null_tracking)
library.talloc_enable_leak_report.restype = None
library.talloc_enable_leak_report.argtypes = []
library.talloc_enable_leak_report = logFuncCall(library.talloc_enable_leak_report)
library.talloc_enable_leak_report_full.restype = None
library.talloc_enable_leak_report_full.argtypes = []
library.talloc_enable_leak_report_full = logFuncCall(library.talloc_enable_leak_report_full)
library._talloc_zero.restype = c_void_p
library._talloc_zero.argtypes = [c_void_p, c_size_t, c_char_p]
library._talloc_zero = logFuncCall(library._talloc_zero)
library._talloc_memdup.restype = c_void_p
library._talloc_memdup.argtypes = [c_void_p, c_void_p, c_size_t, c_char_p]
library._talloc_memdup = logFuncCall(library._talloc_memdup)
library.talloc_strdup.restype = c_char_p
library.talloc_strdup.argtypes = [c_void_p, c_char_p]
library.talloc_strdup = logFuncCall(library.talloc_strdup)
library.talloc_strndup.restype = c_char_p
library.talloc_strndup.argtypes = [c_void_p, c_char_p, c_size_t]
library.talloc_strndup = logFuncCall(library.talloc_strndup)
library.talloc_append_string.restype = c_char_p
library.talloc_append_string.argtypes = [c_void_p, c_char_p, c_char_p]
library.talloc_append_string = logFuncCall(library.talloc_append_string)
library._talloc_array.restype = c_void_p
library._talloc_array.argtypes = [c_void_p, c_size_t, c_uint, c_char_p]
library._talloc_array = logFuncCall(library._talloc_array)
library._talloc_zero_array.restype = c_void_p
library._talloc_zero_array.argtypes = [c_void_p, c_size_t, c_uint, c_char_p]
library._talloc_zero_array = logFuncCall(library._talloc_zero_array)
library._talloc_realloc_array.restype = c_void_p
library._talloc_realloc_array.argtypes = [c_void_p, c_void_p, c_size_t, c_uint, c_char_p]
library._talloc_realloc_array = logFuncCall(library._talloc_realloc_array)
library.talloc_realloc_fn.restype = c_void_p
library.talloc_realloc_fn.argtypes = [c_void_p, c_void_p, c_size_t]
library.talloc_realloc_fn = logFuncCall(library.talloc_realloc_fn)
library.talloc_autofree_context.restype = c_void_p
library.talloc_autofree_context.argtypes = []
library.talloc_autofree_context = logFuncCall(library.talloc_autofree_context)
library.talloc_get_size.restype = c_size_t
library.talloc_get_size.argtypes = [c_void_p]
library.talloc_get_size = logFuncCall(library.talloc_get_size)
library.strlen_m_term.restype = c_size_t
library.strlen_m_term.argtypes = [c_char_p]
library.strlen_m_term = logFuncCall(library.strlen_m_term)
library.talloc_find_parent_byname.restype = c_void_p
library.talloc_find_parent_byname.argtypes = [c_void_p, c_char_p]
library.talloc_find_parent_byname = logFuncCall(library.talloc_find_parent_byname)
library.talloc_show_parents.restype = None
library.talloc_show_parents.argtypes = [c_void_p, c_void_p]
library.talloc_show_parents = logFuncCall(library.talloc_show_parents)
library.talloc_is_parent.restype = c_int
library.talloc_is_parent.argtypes = [c_void_p, c_void_p]
library.talloc_is_parent = logFuncCall(library.talloc_is_parent)


class event_context(Structure): pass
library.watch_fd_callback = CFUNCTYPE(c_int, c_int, c_uint16)
library.loop_callback = CFUNCTYPE(c_int)
class reactor_functions(Structure):
    _fields_ = [
        ('fd_callback', library.watch_fd_callback),
        ('loop_callback', library.loop_callback)
        ]
#struct event_context* async_create_context(struct reactor_functions *funcs)
library.async_create_context.restype = POINTER(event_context)
library.async_create_context.argtypes = [POINTER(reactor_functions)]
library.async_create_context = logFuncCall(library.async_create_context)

def W_ERROR_IS_OK(err):
    return err.v == 0

class WError(Exception):
    def __init__(self, werror, deviceId, action):
        # error code from samba/windows
        self.werror = werror
        # what device was this for?
        self.deviceId = deviceId
        # what were we doing at the time?
        self._action = action

    def __str__(self):
        return '%s on %s (%s)' % (self._action, self.deviceId, self.why())

    def action(self):
        return self._action

    def why(self):
        return DCOM_ERROR_CONSTANTS.get(self.werror.v,
                                        library.win_errstr(self.werror))

def WERR_CHECK(result, deviceId, action):
    if not W_ERROR_IS_OK(result):
        log.debug("ERROR: %s - %s", deviceId, action)
        raise WError(result, deviceId, action)
    log.debug("OK: %s - %s", deviceId, action)

def W_ERROR_EQUAL(a, b):
    return a.v == b



DCOM_ERROR_CONSTANTS = {
    1726:'RPC_S_CALL_FAILED',
    0:'WBEM_NO_ERROR',
    0x40001:'WBEM_S_ALREADY_EXISTS',
    0x40002:'WBEM_S_RESET_TO_DEFAULT',
    0x40003:'WBEM_S_DIFFERENT',
    0x40004:'WBEM_S_TIMEDOUT',
    0x40005:'WBEM_S_NO_MORE_DATA',
    0x40006:'WBEM_S_OPERATION_CANCELLED',
    0x40007:'WBEM_S_PENDING',
    0x40008:'WBEM_S_DUPLICATE_OBJECTS',
    0x40009:'WBEM_S_ACCESS_DENIED',
    0x40010:'WBEM_S_PARTIAL_RESULTS',
    0x40011:'WBEM_S_NO_POSTHOOK',
    0x40012:'WBEM_S_POSTHOOK_WITH_BOTH',
    0x40013:'WBEM_S_POSTHOOK_WITH_NEW',
    0x40014:'WBEM_S_POSTHOOK_WITH_STATUS',
    0x40015:'WBEM_S_POSTHOOK_WITH_OLD',
    0x40016:'WBEM_S_REDO_PREHOOK_WITH_ORIGINAL_OBJECT',
    0x40017:'WBEM_S_SOURCE_NOT_AVAILABLE',
    0x80041001:'WBEM_E_FAILED',
    0x80041002:'WBEM_E_NOT_FOUND',
    0x80041003:'WBEM_E_ACCESS_DENIED',
    0x80041004:'WBEM_E_PROVIDER_FAILURE',
    0x80041005:'WBEM_E_TYPE_MISMATCH',
    0x80041006:'WBEM_E_OUT_OF_MEMORY',
    0x80041007:'WBEM_E_INVALID_CONTEXT',
    0x80041008:'WBEM_E_INVALID_PARAMETER',
    0x80041009:'WBEM_E_NOT_AVAILABLE',
    0x8004100A:'WBEM_E_CRITICAL_ERROR',
    0x8004100B:'WBEM_E_INVALID_STREAM',
    0x8004100C:'WBEM_E_NOT_SUPPORTED',
    0x8004100D:'WBEM_E_INVALID_SUPERCLASS',
    0x8004100E:'WBEM_E_INVALID_NAMESPACE',
    0x8004100F:'WBEM_E_INVALID_OBJECT',
    0x80041010:'WBEM_E_INVALID_CLASS',
    0x80041011:'WBEM_E_PROVIDER_NOT_FOUND',
    0x80041012:'WBEM_E_INVALID_PROVIDER_REGISTRATION',
    0x80041013:'WBEM_E_PROVIDER_LOAD_FAILURE',
    0x80041014:'WBEM_E_INITIALIZATION_FAILURE',
    0x80041015:'WBEM_E_TRANSPORT_FAILURE',
    0x80041016:'WBEM_E_INVALID_OPERATION',
    0x80041017:'WBEM_E_INVALID_QUERY',
    0x80041018:'WBEM_E_INVALID_QUERY_TYPE',
    0x80041019:'WBEM_E_ALREADY_EXISTS',
    0x8004101A:'WBEM_E_OVERRIDE_NOT_ALLOWED',
    0x8004101B:'WBEM_E_PROPAGATED_QUALIFIER',
    0x8004101C:'WBEM_E_PROPAGATED_PROPERTY',
    0x8004101D:'WBEM_E_UNEXPECTED',
    0x8004101E:'WBEM_E_ILLEGAL_OPERATION',
    0x8004101F:'WBEM_E_CANNOT_BE_KEY',
    0x80041020:'WBEM_E_INCOMPLETE_CLASS',
    0x80041021:'WBEM_E_INVALID_SYNTAX',
    0x80041022:'WBEM_E_NONDECORATED_OBJECT',
    0x80041023:'WBEM_E_READ_ONLY',
    0x80041024:'WBEM_E_PROVIDER_NOT_CAPABLE',
    0x80041025:'WBEM_E_CLASS_HAS_CHILDREN',
    0x80041026:'WBEM_E_CLASS_HAS_INSTANCES',
    0x80041027:'WBEM_E_QUERY_NOT_IMPLEMENTED',
    0x80041028:'WBEM_E_ILLEGAL_NULL',
    0x80041029:'WBEM_E_INVALID_QUALIFIER_TYPE',
    0x8004102A:'WBEM_E_INVALID_PROPERTY_TYPE',
    0x8004102B:'WBEM_E_VALUE_OUT_OF_RANGE',
    0x8004102C:'WBEM_E_CANNOT_BE_SINGLETON',
    0x8004102D:'WBEM_E_INVALID_CIM_TYPE',
    0x8004102E:'WBEM_E_INVALID_METHOD',
    0x8004102F:'WBEM_E_INVALID_METHOD_PARAMETERS',
    0x80041030:'WBEM_E_SYSTEM_PROPERTY',
    0x80041031:'WBEM_E_INVALID_PROPERTY',
    0x80041032:'WBEM_E_CALL_CANCELLED',
    0x80041033:'WBEM_E_SHUTTING_DOWN',
    0x80041034:'WBEM_E_PROPAGATED_METHOD',
    0x80041035:'WBEM_E_UNSUPPORTED_PARAMETER',
    0x80041036:'WBEM_E_MISSING_PARAMETER_ID',
    0x80041037:'WBEM_E_INVALID_PARAMETER_ID',
    0x80041038:'WBEM_E_NONCONSECUTIVE_PARAMETER_IDS',
    0x80041039:'WBEM_E_PARAMETER_ID_ON_RETVAL',
    0x8004103A:'WBEM_E_INVALID_OBJECT_PATH',
    0x8004103B:'WBEM_E_OUT_OF_DISK_SPACE',
    0x8004103C:'WBEM_E_BUFFER_TOO_SMALL',
    0x8004103D:'WBEM_E_UNSUPPORTED_PUT_EXTENSION',
    0x8004103E:'WBEM_E_UNKNOWN_OBJECT_TYPE',
    0x8004103F:'WBEM_E_UNKNOWN_PACKET_TYPE',
    0x80041040:'WBEM_E_MARSHAL_VERSION_MISMATCH',
    0x80041041:'WBEM_E_MARSHAL_INVALID_SIGNATURE',
    0x80041042:'WBEM_E_INVALID_QUALIFIER',
    0x80041043:'WBEM_E_INVALID_DUPLICATE_PARAMETER',
    0x80041044:'WBEM_E_TOO_MUCH_DATA',
    0x80041045:'WBEM_E_SERVER_TOO_BUSY',
    0x80041046:'WBEM_E_INVALID_FLAVOR',
    0x80041047:'WBEM_E_CIRCULAR_REFERENCE',
    0x80041048:'WBEM_E_UNSUPPORTED_CLASS_UPDATE',
    0x80041049:'WBEM_E_CANNOT_CHANGE_KEY_INHERITANCE',
    0x80041050:'WBEM_E_CANNOT_CHANGE_INDEX_INHERITANCE',
    0x80041051:'WBEM_E_TOO_MANY_PROPERTIES',
    0x80041052:'WBEM_E_UPDATE_TYPE_MISMATCH',
    0x80041053:'WBEM_E_UPDATE_OVERRIDE_NOT_ALLOWED',
    0x80041054:'WBEM_E_UPDATE_PROPAGATED_METHOD',
    0x80041055:'WBEM_E_METHOD_NOT_IMPLEMENTED',
    0x80041056:'WBEM_E_METHOD_DISABLED',
    0x80041057:'WBEM_E_REFRESHER_BUSY',
    0x80041058:'WBEM_E_UNPARSABLE_QUERY',
    0x80041059:'WBEM_E_NOT_EVENT_CLASS',
    0x8004105A:'WBEM_E_MISSING_GROUP_WITHIN',
    0x8004105B:'WBEM_E_MISSING_AGGREGATION_LIST',
    0x8004105C:'WBEM_E_PROPERTY_NOT_AN_OBJECT',
    0x8004105D:'WBEM_E_AGGREGATING_BY_OBJECT',
    0x8004105F:'WBEM_E_UNINTERPRETABLE_PROVIDER_QUERY',
    0x80041060:'WBEM_E_BACKUP_RESTORE_WINMGMT_RUNNING',
    0x80041061:'WBEM_E_QUEUE_OVERFLOW',
    0x80041062:'WBEM_E_PRIVILEGE_NOT_HELD',
    0x80041063:'WBEM_E_INVALID_OPERATOR',
    0x80041064:'WBEM_E_LOCAL_CREDENTIALS',
    0x80041065:'WBEM_E_CANNOT_BE_ABSTRACT',
    0x80041066:'WBEM_E_AMENDED_OBJECT',
    0x80041067:'WBEM_E_CLIENT_TOO_SLOW',
    0x80041068:'WBEM_E_NULL_SECURITY_DESCRIPTOR',
    0x80041069:'WBEM_E_TIMED_OUT',
    0x8004106A:'WBEM_E_INVALID_ASSOCIATION',
    0x8004106B:'WBEM_E_AMBIGUOUS_OPERATION',
    0x8004106C:'WBEM_E_QUOTA_VIOLATION',
    0x8004106D:'WBEM_E_RESERVED_001',
    0x8004106E:'WBEM_E_RESERVED_002',
    0x8004106F:'WBEM_E_UNSUPPORTED_LOCALE',
    0x80041070:'WBEM_E_HANDLE_OUT_OF_DATE',
    0x80041071:'WBEM_E_CONNECTION_FAILED',
    0x80041072:'WBEM_E_INVALID_HANDLE_REQUEST',
    0x80041073:'WBEM_E_PROPERTY_NAME_TOO_WIDE',
    0x80041074:'WBEM_E_CLASS_NAME_TOO_WIDE',
    0x80041075:'WBEM_E_METHOD_NAME_TOO_WIDE',
    0x80041076:'WBEM_E_QUALIFIER_NAME_TOO_WIDE',
    0x80041077:'WBEM_E_RERUN_COMMAND',
    0x80041078:'WBEM_E_DATABASE_VER_MISMATCH',
    0x80041079:'WBEM_E_VETO_DELETE',
    0x8004107A:'WBEM_E_VETO_PUT',
    0x80041080:'WBEM_E_INVALID_LOCALE',
    0x80041081:'WBEM_E_PROVIDER_SUSPENDED',
    0x80041082:'WBEM_E_SYNCHRONIZATION_REQUIRED',
    0x80041083:'WBEM_E_NO_SCHEMA',
    0x80041084:'WBEM_E_PROVIDER_ALREADY_REGISTERED',
    0x80041085:'WBEM_E_PROVIDER_NOT_REGISTERED',
    0x80041086:'WBEM_E_FATAL_TRANSPORT_ERROR',
    0x80041087:'WBEM_E_ENCRYPTED_CONNECTION_REQUIRED',
    0x80041088:'WBEM_E_PROVIDER_TIMED_OUT',
    0x80041089:'WBEM_E_NO_KEY',
    0x8004108a:'WBEM_E_PROVIDER_DISABLED',
    # not dcom, but frequently seen
    0x000006be: 'OPERATION_COULD_NOT_BE_COMPLETED',
}
