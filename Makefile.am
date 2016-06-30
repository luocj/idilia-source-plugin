ACLOCAL_AMFLAGS = -I m4

# FIXME: These flags should be produced in configure.ac using AX_APPEND_COMPILE_FLAGS
AM_CFLAGS = \
	-Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wunused -fstrict-aliasing -Wextra -Wformat=2 -Winit-self -Winline \
	-Wpacked -Wpointer-arith -Wmissing-format-attribute -Wmissing-noreturn \
	-Wnested-externs -Wstrict-prototypes -Wunsafe-loop-optimizations \
	-Wwrite-strings -Wno-missing-field-initializers -Wno-unused-parameter \
	-Wcast-align -Wformat-nonliteral -Wformat-security -Wswitch-default \
	-Wmissing-include-dirs -Wno-aggregate-return -Wunused-but-set-variable \
	-Warray-bounds -Wold-style-definition -Wsign-compare -Wlarger-than=65537
AM_CFLAGS += -Wno-undef  # sophiasip is not -Wundef--safe
AM_CFLAGS += -Wno-redundant-decls  # sophiasip also contains redundant declarations
AM_CFLAGS += -Wno-override-init  # we need this for JANUS_PLUGIN_INIT and JANUS_TRANSPORT_INIT
# FIXME: These should be enabled once the code is safe for them. That requires
# some fairly big refactoring though, which can wait.
# AM_CFLAGS += -Wshadow -Wstrict-aliasing=2

AM_CFLAGS += -fstack-protector-all -g -ggdb -fPIC -rdynamic -pthread

# FIXME: make docs work with distcheck
DISTCHECK_CONFIGURE_FLAGS = --disable-docs --enable-post-processing

%.sample: %.sample.in
	$(MKDIR_P) $(@D)
	$(AM_V_GEN) sed -e "\
	    s|[@]confdir[@]|$(confdir)|;\
	    s|[@]plugindir[@]|$(plugindir)|;" \
	$< > $@ || rm $@

EXTRA_DIST = $(NULL)
CLEANFILES = $(NULL)

confdir = $(sysconfdir)/janus
conf_DATA = $(NULL)

plugindir = $(libdir)/janus/plugins
plugin_LTLIBRARIES = $(NULL)

##
# Plugins
##

plugins_cflags = \
	$(AM_CFLAGS) \
	$(PLUGINS_CFLAGS) \
	$(NULL)

plugins_libadd = \
	$(PLUGINS_LIBS) \
	$(NULL)

if ENABLE_PLUGIN_SOURCE
plugin_LTLIBRARIES += plugins/libidilia_source.la
plugins_libidilia_source_la_SOURCES = plugins/idilia_source.c plugins/ports_pool.c plugins/node_service_access.c
plugins_libidilia_source_la_CFLAGS = $(plugins_cflags)
plugins_libidilia_source_la_LDFLAGS = $(plugins_ldflags)
plugins_libidilia_source_la_LIBADD = $(plugins_libadd)
conf_DATA += conf/idilia.plugin.source.cfg.sample
EXTRA_DIST += \
	conf/idilia.plugin.source.cfg.sample.in \
	$(stream_DATA)
CLEANFILES += conf/idilia.plugin.source.cfg.sample
endif

##
# Configuration
##

configs:
	$(MKDIR_P) $(DESTDIR)$(confdir)
	$(foreach config,$(conf_DATA),cp "$(CURDIR)/$(config)" "$(DESTDIR)$(confdir)/$(notdir $(basename $(config) .sample))";)
