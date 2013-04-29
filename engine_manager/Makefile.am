ACLOCAL_AMFLAGS = -I ../../acinclude

include $(top_srcdir)/build/common.mk

lib_LTLIBRARIES = libibengmgr.la
libibengmgr_la_CPPFLAGS = $(AM_CPPFLAGS)
if HAVE_NO_UNUSED_BUT_SET_VARIABLE
libibengmgr_la_CPPFLAGS += -Wno-unused-but-set-variable
endif

libibengmgr_la_SOURCES = \
	engine_manager.c

EXTRA_DIST = \
        engine_manager_private.h
