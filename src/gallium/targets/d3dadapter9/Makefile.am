# Copyright © 2012 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(NINE_CFLAGS) \
	$(GALLIUM_CFLAGS) \
	$(VISIBILITY_CFLAGS) \
	-I$(top_srcdir)/src/gallium/winsys \
	-I$(top_srcdir)/src/gallium/state_trackers/nine \
	-I$(top_srcdir)/include/D3D9 \
	-I$(top_srcdir)/src/loader \
	-DPIPE_SEARCH_DIR=\"$(GALLIUM_PIPE_LOADER_DIR)\" \
	$(GALLIUM_PIPE_LOADER_DEFINES)

lib_LTLIBRARIES = libd3dadapter9.la

libd3dadapter9_la_SOURCES = \
	getproc.c \
	drm.c

libd3dadapter9_la_LDFLAGS = \
	-no-undefined \
	-shared \
	$(LD_NO_UNDEFINED)

libd3dadapter9_la_LIBADD = \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/gallium/auxiliary/pipe-loader/libpipe_loader.la \
	$(top_builddir)/src/gallium/state_trackers/nine/libninetracker.la \
	$(top_builddir)/src/gallium/winsys/sw/wrapper/libwsw.la \
	$(top_builddir)/src/gallium/winsys/sw/null/libws_null.la \
	$(top_builddir)/src/util/libmesautil.la \
	$(GALLIUM_PIPE_LOADER_LIBS) \
	$(LIBUDEV_LIBS) \
	$(GALLIUM_COMMON_LIB_DEPS)

if NEED_WINSYS_XLIB
libd3dadapter9_la_LIBADD += \
	$(top_builddir)/src/gallium/winsys/sw/xlib/libws_xlib.la \
	-lX11 -lXext -lXfixes
endif

if HAVE_MESA_LLVM
nodist_EXTRA_libd3dadapter9_la_SOURCES = dummy.cpp
libd3dadapter9_la_LDFLAGS += $(LLVM_LDFLAGS)
libd3dadapter9_la_LIBADD += $(LLVM_LIBS)
endif

if HAVE_DRI2
libd3dadapter9_la_LIBADD += \
	$(top_builddir)/src/gallium/winsys/sw/dri/libswdri.la
endif

d3dadapterdir = $(includedir)/d3dadapter
d3dadapter_HEADERS = \
	$(top_srcdir)/include/d3dadapter/d3dadapter9.h \
	$(top_srcdir)/include/d3dadapter/drm.h \
	$(top_srcdir)/include/d3dadapter/present.h
