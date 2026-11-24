ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir)/src \
	-include $(top_builddir)/config.h \
	$(NCURSES_CFLAGS) \
	$(PIPEWIRE_CFLAGS) \
	$(FFMPEG_CFLAGS) \
	$(SQLITE_CFLAGS) \
	$(DBUS_CFLAGS)

AM_CFLAGS  = @NUSIC_OPT_CFLAGS@
AM_LDFLAGS = @NUSIC_OPT_LDFLAGS@

# user flags may override optimisation but not warnings or hardening
COMPILE = $(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) \
	$(AM_CFLAGS) $(CFLAGS) $(NUSIC_CPPFLAGS) $(NUSIC_CFLAGS)
LINK = $(CCLD) $(AM_CFLAGS) $(CFLAGS) $(NUSIC_CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) $(NUSIC_LDFLAGS) -o $@

bin_PROGRAMS = nusic

nusic_SOURCES = \
	src/nusic.c \
	src/db.c \
	src/db.h \
	src/decode.c \
	src/decode.h \
	src/draw.c \
	src/gain.c \
	src/gain.h \
	src/library.c \
	src/library.h \
	src/mpris.c \
	src/mpris.h \
	src/output.c \
	src/output.h \
	src/player.c \
	src/player.h \
	src/report.c \
	src/report.h \
	src/ring.c \
	src/ring.h \
	src/scan.h \
	src/screen.h \
	src/text.c \
	src/text.h \
	src/ui.c \
	src/ui.h

nusic_LDADD = \
	$(NCURSES_LIBS) \
	$(PIPEWIRE_LIBS) \
	$(FFMPEG_LIBS) \
	$(SQLITE_LIBS) \
	$(DBUS_LIBS)

EXTRA_DIST = \
	README.org \
	LICENSE \
	autogen.sh
