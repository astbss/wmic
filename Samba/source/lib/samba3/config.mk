################################################
# Start SUBSYSTEM LIBSAMBA3
[LIBRARY::LIBSAMBA3]
VERSION = 0.0.1
SO_VERSION = 0
DESCRIPTION = Library for reading Samba3 data files
PRIVATE_PROTO_HEADER = samba3_proto.h
PUBLIC_HEADERS = samba3.h
OBJ_FILES = smbpasswd.o tdbsam.o policy.o \
		idmap.o winsdb.o samba3.o group.o \
		registry.o secrets.o share_info.o
PRIVATE_DEPENDENCIES = LIBSAMBA-UTIL LIBTDB NDR_SECURITY \
					   CREDENTIALS
# End SUBSYSTEM LIBSAMBA3
################################################
