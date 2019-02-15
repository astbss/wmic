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
build:   pywmi-build 
install: pywmi-installed 
all:     build install
.PHONY:  clean debug tarball

WMI_BUILD_TARGETS = proto bin/wmic bin/winexe libraries
SAMBA_SRCDIR      = Samba/source
ZENOSS_BINDIR     = $(ZENHOME)/bin
ZENPYTHON         = $(ZENOSS_BINDIR)/python
PYTHON           ?= $(ZENPYTHON)
PYTHON_EXISTS    := $(wildcard $(PYTHON))

# Assuming python was found, tease out path to headers we should 
# compile against:  e.g., $ZENHOME/include/python#.#
# Extract WMI version for subversion tagging as desired.
#
ifeq ($(PYTHON_EXISTS),$(PYTHON))
PY_INCDIR     = $(shell $(PYTHON) pyinclude.py)
GET_VERSION   = "import version as v; print v.VERSION"
WMI_VERSION  := $(shell cd pysamba ; $(PYTHON) -c $(GET_VERSION))
WMI_CPPFLAGS := -I$(PY_INCDIR)
endif

# Install dir for libasync_wmi_lib.so.* and pysamba wrapper code.
# e.g., Samba/source/bin/shared/libasync_wmi_lib.so.0.0.1 --> $(PY_LIBDIR)
#
PY_LIBDIR     = $(ZENHOME)/lib/python

#-------------------------------------------------------------------------#
# A key deliverable of this build process is the libasync_wmi shared      #
# library.  Derive the complete filename for this target from config data #
# and the build platform so we know precisely what should get built and   #
# installed.                                                              #
#                                                                         #
# e.g., libasync_wmi_lib.dylib versus libasync_wmi_lib.so.0.0.2           #
#-------------------------------------------------------------------------#
LIBASYNC_WMI_LIB_BASENAME    = libasync_wmi_lib
WMI_CONFIG_MK                = $(SAMBA_SRCDIR)/wmi/config.mk
LIBASYNC_WMI_LIB_VERSION_nnn = $(shell fgrep -A1 "[LIBRARY::async_wmi_lib]" $(WMI_CONFIG_MK) | sed -e "s/^VERSION=\(.*\)/\1/g"    | tail -1)
LIBASYNC_WMI_LIB_VERSION_n   = $(shell fgrep -A2 "[LIBRARY::async_wmi_lib]" $(WMI_CONFIG_MK) | sed -e "s/^SO_VERSION=\(.*\)/\1/g" | tail -1)
ifeq ($(shell uname), Darwin)
LIBASYNC_WMI_LIB             = $(LIBASYNC_WMI_LIB_BASENAME).dylib.$(LIBASYNC_WMI_LIB_VERSION_nnn)
else
# e.g., libasync_wmi_lib.so.0.0.2 and libasync_wmi_lib.so.0 respectively
LIBASYNC_WMI_LIB            := $(LIBASYNC_WMI_LIB_BASENAME).so.$(LIBASYNC_WMI_LIB_VERSION_nnn)
LIBASYNC_WMI_LIB_SO_N       := $(LIBASYNC_WMI_LIB_BASENAME).so.$(LIBASYNC_WMI_LIB_VERSION_n)
endif
PATHED_LIBASYNC_WMI_LIB     := $(SAMBA_SRCDIR)/bin/shared/$(LIBASYNC_WMI_LIB)

#-------------------------------------------------------------------------#
# Google Breakpad Integration                                             #
#-------------------------------------------------------------------------#
# libasync_wmi_lib.so can be built with google-breakpad crash reporting.  #
# http://code.google.com/p/google-breakpad                                #
#                                                                         #
#    Minidumps are typically written to /tmp.                             #
#    See: Samba/source/librpc/rpc/dcerpc.c                                #
#-------------------------------------------------------------------------#
# Comment out the next line to disable google-breakpad dependency.
#ifneq ($(shell uname), Darwin)
#USE_BREAKPAD = 1
#endif

ifneq ($(USE_BREAKPAD),)
breakpad_CPPFLAGS    = -DBREAKPAD
WMI_CPPFLAGS        += $(breakpad_CPPFLAGS)
breakpad_LIB         = libbreakpad_client.a
breakpad_LIBDIR     ?= $(ZENHOME)/lib
_fqp_breakpad_LIB   := $(DESTDIR)$(breakpad_LIBDIR)/$(breakpad_LIB)
fqp_breakpad_LIB     = $(patsubst //%,/%,$(_fqp_breakpad_LIB))
breakpad_LIB_SYMLINK = $(SAMBA_SRCDIR)/bin/static/$(breakpad_LIB)
endif
#-------------------------------------------------------------------------#

# Check existence of a directory or file.  Bail out of the build if it is missing.
#
define check
	@if [ "$1" = "directory" ]; then \
		if [ ! -d "$2" ];then \
			echo $3 | awk '{printf("Missing: %-20s\n",$$1)}';\
			exit 1 ;\
		else \
			echo "$3 $2" | awk '{printf("Found:   %-20s %20s\n",$$1,$$2)}' 1>/dev/null;\
		fi ;\
	fi
	@if [ "$1" = "file" ]; then \
		if [ ! -f "$2" ];then \
			echo "$3 $2" | awk '{printf("Missing: %-20s %s\n",$$1,$$2)}';\
			exit 1 ;\
		else \
			echo "$3 $2" | awk '{printf("Found:   %-20s %20s\n",$$1,$$2)}' 1>/dev/null;\
		fi ;\
	fi
endef

build-prereqs:
	@echo
	@echo "Checking prequisites for building WMI"
	$(call check,directory,$(ZENHOME),"ZENHOME")
	$(call check,file,$(PYTHON),"PYTHON")
	$(call check,directory,$(PY_INCDIR),"PY_INCDIR")
ifneq ($(USE_BREAKPAD),)
	@if [ ! -f "$(fqp_breakpad_LIB)" ];then \
		echo "Unable to find the google breakpad client library we require at:" ;\
		echo "   $(fqp_breakpad_LIB)" ;\
		echo ;\
		echo "Either comment out USE_BREAKPAD in this makefile or build the" ;\
		echo "breakpad library." ;\
		echo ;\
		exit 1 ;\
	fi
endif
	@touch $@

install-prereqs:
	@echo
	@echo "Checking prequisites for installing WMI"
	$(call check,directory,$(ZENHOME),"ZENHOME")
	$(call check,directory,$(DESTDIR)$(ZENOSS_BINDIR),"ZENOSS_BINDIR")
	$(call check,directory,$(DESTDIR)$(PY_LIBDIR),"PY_LIBDIR")
	@touch $@

LIBRPC_CONFIG_MK = $(SAMBA_SRCDIR)/librpc/config.mk
ifeq ($(USE_BREAKPAD),)
LIBRPC_CONFIG_MK_NOBP = $(SAMBA_SRCDIR)/librpc/config.mk.nobreakpad
$(LIBRPC_CONFIG_MK): $(LIBRPC_CONFIG_MK_NOBP)
	cp $< $@
else
# Tell the build how to link against the breakpad library.
# e.g., Muck with Samba/source/librpc/config.mk to provide that visibility.
#
LIBRPC_CONFIG_MK_BP = $(SAMBA_SRCDIR)/librpc/config.mk.breakpad
LIBRPC_CONFIG_TAG  := $(fqp_breakpad_LIB)
$(LIBRPC_CONFIG_MK): $(LIBRPC_CONFIG_MK_BP)
	sed -e "s|_sed_tag_libbreakpad_client_path_|$(LIBRPC_CONFIG_TAG)|" $< >$@ || rm $@

# Create symlink to actual google breakpad library.
# e.g., Samba/source/bin/static/libbreakpad_client.a -> /actual/path/to/libbreakpad_client.a
$(breakpad_LIB_SYMLINK): $(fqp_breakpad_LIB)
	@if [ ! -d "$(@D)" ];then \
		 mkdir -p $(@D) ;\
	fi
	ln -sf $(fqp_breakpad_LIB) $@
endif

$(SAMBA_SRCDIR)/Makefile: $(SAMBA_SRCDIR)/autogen.sh
	cd $(SAMBA_SRCDIR) ;\
	./autogen.sh ;\
	CPPFLAGS="$(WMI_CPPFLAGS)" ./configure --without-readline --enable-debug

ifeq ($(USE_BREAKPAD),)
pywmi-build: build-prereqs $(LIBRPC_CONFIG_MK) $(SAMBA_SRCDIR)/Makefile
else
pywmi-build: build-prereqs $(LIBRPC_CONFIG_MK) $(SAMBA_SRCDIR)/Makefile $(breakpad_LIB_SYMLINK)
endif
	cd $(SAMBA_SRCDIR);\
	$(MAKE) $(WMI_BUILD_TARGETS) ;\
	touch $@

pywmi-installed: install-prereqs $(DESTDIR)$(PY_LIBDIR) $(DESTDIR)$(ZENOSS_BINDIR) $(SAMBA_SRCDIR)/bin/wmic $(SAMBA_SRCDIR)/bin/winexe $(PATHED_LIBASYNC_WMI_LIB)
	cp $(SAMBA_SRCDIR)/bin/wmic   $(DESTDIR)$(ZENOSS_BINDIR)
	cp $(SAMBA_SRCDIR)/bin/winexe $(DESTDIR)$(ZENOSS_BINDIR)
ifeq ($(shell uname), Darwin)
	-(cd $(DESTDIR)$(PY_LIBDIR) && rm -f $(LIBASYNC_WMI_LIB_BASENAME)*)
	cp $(PATHED_LIBASYNC_WMI_LIB) $(DESTDIR)$(PY_LIBDIR)/$(LIBASYNC_WMI_LIB_BASENAME).$(LIBASYNC_WMI_LIB_VERSION_nnn).dylib
	(cd $(DESTDIR)$(PY_LIBDIR) && ln -sf $(LIBASYNC_WMI_LIB_BASENAME).$(LIBASYNC_WMI_LIB_VERSION_nnn).dylib $(LIBASYNC_WMI_LIB_BASENAME).dylib)
else
	-(cd $(DESTDIR)$(PY_LIBDIR) && rm -f $(LIBASYNC_WMI_LIB_BASENAME)*)
	cp $(PATHED_LIBASYNC_WMI_LIB) $(DESTDIR)$(PY_LIBDIR)
	(cd $(DESTDIR)$(PY_LIBDIR) && ln -sf $(LIBASYNC_WMI_LIB) $(LIBASYNC_WMI_LIB_SO_N))
endif
	rm -rf $(DESTDIR)$(PY_LIBDIR)/pysamba
	cp -r pysamba $(DESTDIR)$(PY_LIBDIR) 

$(DESTDIR)$(ZENOSS_BINDIR) $(DESTDIR)$(PY_LIBDIR):
	mkdir -p $@

clean: $(LIBRPC_CONFIG_MK)
	-if [ -f "$(SAMBA_SRCDIR)/Makefile" ] ; then\
		cd $(SAMBA_SRCDIR) ;\
		make distclean ;\
	fi
	rm -f $(SAMBA_SRCDIR)/bin/shared/* 
	rm -f $(SAMBA_SRCDIR)/bin/static/* 
	rm -f $(SAMBA_SRCDIR)/heimdal/lib/des/hcrypto
	rm -f build-prereqs
	rm -f install-prereqs
	rm -f $(LIBRPC_CONFIG_MK)
	@-[ -L $(breakpad_LIB_SYMLINK) ] && rm -f $(breakpad_LIB_SYMLINK)

tarball:
	-svn rm -m 'cleanup' http://dev.zenoss.org/svn/tags/wmi-$(WMI_VERSION)
	svn cp -m "tagging wmi-$(WMI_VERSION)" http://dev.zenoss.org/svn/trunk/wmi http://dev.zenoss.org/svn/tags/wmi-$(WMI_VERSION)
	svn export http://dev.zenoss.org/svn/tags/wmi-$(WMI_VERSION)
	tar -cjf ../wmi-$(WMI_VERSION).tar.bz2 wmi-$(WMI_VERSION)
	rm -rf wmi-$(WMI_VERSION)

debug: 
	@echo "WMI_VERSION       = $(WMI_VERSION)"
	@echo "SAMBA_SRCDIR      = $(SAMBA_SRCDIR)"
	@echo "PY_INCDIR         = $(PY_INCDIR)"
	@echo "PY_LIBDIR         = $(PY_LIBDIR)"
	@echo "ZENOSS_BINDIR     = $(ZENOSS_BINDIR)"
	@echo "PYTHON            = $(PYTHON_EXISTS)"
	@echo "WMI_CONFIGURE       CPPFLAGS="$(WMI_CPPFLAGS)" ./configure --without-readline --enable-debug"
	@echo "WMI_MAKE            $(MAKE) $(WMI_BUILD_TARGETS)"
ifeq ($(USE_BREAKPAD),)
	@echo "USE_BREAKPAD        [ disabled ]"
else
	@echo "LIBRPC_CONFIG_TAG = $(LIBRPC_CONFIG_TAG)"
	@echo "USE_BREAKPAD        [ enabled ]"
	@echo "breakpad_CPPFLAGS = $(breakpad_CPPFLAGS)"
	@echo "breakpad_LIB      = $(breakpad_LIB)"
	@echo "breakpad_LIBDIR   = $(breakpad_LIBDIR)"
	@echo "fqp_breakpad_LIB  = $(fqp_breakpad_LIB)"
endif
