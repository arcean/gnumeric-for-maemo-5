AM_CPPFLAGS = \
    -DGNOMELOCALEDIR=\""$(datadir)/locale"\" 	\
    -I$(top_srcdir)/src	-I$(top_builddir)/src	\
    $(GNUMERIC_CFLAGS)

gnumeric_plugin_fninfodir = $(gnumeric_plugindir)/fn-info
xmldir = $(gnumeric_plugin_fninfodir)
gnumeric_plugin_fninfo_LTLIBRARIES = plugin.la
plugin_la_LDFLAGS = -module $(GNUMERIC_PLUGIN_LDFLAGS)
plugin_la_SOURCES = functions.c

xml_in_files = plugin.xml.in
xml_DATA = $(xml_in_files:.xml.in=.xml)

@INTLTOOL_XML_RULE@

EXTRA_DIST = $(xml_in_files)
DISTCLEANFILES = $(xml_DATA)
