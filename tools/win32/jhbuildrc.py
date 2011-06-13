# -*- mode: python -*-
# This code is licensed under the GPLv2 License
# Derived work from the original freedesktop.org example.jhbuildrc
# Derived work from Alberto Ruiz <aruiz@gnome.org>
#
# This jhbuildrc file is created for the purpose of cross compile Gnumeric
# with Mingw32 under Linux.
#
# Author: Jody Goldberg <jody@gnome.org>

moduleset = os.getenv('JH_MODULE_SET')
modules = ['gnumeric', 'evince']

# checkoutroot: path to download packages elsewhere
# prefix:       target path to install the compiled binaries
checkoutroot = os.path.join(os.sep, os.getenv('JH_PREFIX'), "src")
prefix	     = os.path.join(os.sep, os.getenv('JH_PREFIX'), "deploy")
os.environ['prefix']	= prefix	# for use in zlib kludge

#The host value is obtained with the result of executing
#the config.guess script on any of the packages.
#This value must be valid for most linux/x86 out there
os.environ['HOST']	= 'i686-pc-linux-gnuaout'
os.environ['TARGET']	= 'i586-pc-mingw32msvc'
os.environ['PKG_CONFIG']= '/usr/bin/pkg-config'

addpath('PKG_CONFIG_PATH', os.path.join(os.sep, prefix, 'lib', 'pkgconfig'))
addpath('PKG_CONFIG_PATH', os.path.join(os.sep, prefix, 'lib64', 'pkgconfig'))
addpath('PKG_CONFIG_PATH', os.path.join(os.sep, prefix, 'share', 'pkgconfig'))

#Prefix for all the tools
mingw_tool_prefix1 = '/usr/bin/i586-mingw32msvc-'
mingw_tool_prefix2 = '/usr/bin/i686-pc-mingw32-'
mingw_tool_prefix3 = '/opt/cross/bin/i386-mingw32msvc-'

if os.path.exists(mingw_tool_prefix1 + 'gcc'):
    mingw_tool_prefix = mingw_tool_prefix1
elif os.path.exists(mingw_tool_prefix2 + 'gcc'):
    mingw_tool_prefix = mingw_tool_prefix2
elif os.path.exists(mingw_tool_prefix3 + 'gcc'):
    mingw_tool_prefix = mingw_tool_prefix3
else:
    print "Unable to find mingw"
    sys.exit (1)

mingw_tools = {'ADDR2LINE': 'addr2line',
	'AS': 'as', 'CC': 'gcc', 'CPP': 'cpp',
	'CPPFILT': 'c++filt', 'CXX': 'g++',
	'DLLTOOL': 'dlltool', 'DLLWRAP': 'dllwrap',
	'GCOV': 'gcov', 'LD': 'ld', 'NM': 'nm',
	'OBJCOPY': 'objcopy', 'OBJDUMP': 'objdump',
	'READELF': 'readelf', 'SIZE': 'size',
	'STRINGS': 'strings', 'WINDRES': 'windres',
	'AR': 'ar', 'RANLIB': 'ranlib', 'STRIP': 'strip'}

#Exporting all as enviroment variables with its prefix
mingw_tools_args = str()
for tool in mingw_tools.keys():
	fullpath_tool = mingw_tool_prefix + mingw_tools[tool]
	os.environ[tool] = fullpath_tool

if os.getenv('JH_BUILD') == "debug":
    optim = ' -O0 -gstabs'
elif os.getenv('JH_BUILD') == "release":
    optim = ' -O2'
else:
    print "Best to invoke this via build script from make"
    sys.exit (0)

#Exporting tool flags enviroment variables
# -no-undefined' /
use_lib64 = False
os.environ['LDFLAGS']	 \
	= ' -mno-cygwin' \
	+ ' -L' + os.path.join(os.sep, prefix, 'lib') \
	+ ' -L' + os.path.join(os.sep, prefix, 'lib64')
os.environ['CFLAGS']	 = optim + ' -mno-cygwin -mms-bitfields -march=i686 ' + ' -I' + os.path.join(os.sep, prefix, 'include')
os.environ['CPPLAGS']	 = optim + ' -mno-cygwin -mms-bitfields -march=i686 ' + ' -I' + os.path.join(os.sep, prefix, 'include')
os.environ['CXXLAGS']	 = optim + ' -mno-cygwin -mms-bitfields -march=i686 ' + ' -I' + os.path.join(os.sep, prefix, 'include')
os.environ['ARFLAGS']	 = 'rcs'
os.environ['INSTALL']	 = os.path.expanduser('~/bin/install-check')
os.environ['ACLOCAL_AMFLAGS'] = ' -I m4 -I '+prefix+'/share/aclocal'	# for libgnomedb

os.environ['WINEDEBUG']	 = '-all'
#os.environ['MAKE']	 = 'colormake'

py_prefix = prefix+'/Python26'
os.environ['PYTHON']	 = py_prefix+'/python.exe'
os.environ['PY_PREFIX']  = py_prefix
os.environ['PY_INCLUDE_DIR'] = py_prefix+'/include'
os.environ['PY_LIB_DIR']     = py_prefix+'/libs'

#Populating autogenargs
autogenargs =  ' --build='+os.environ['HOST']
autogenargs += ' --host='+os.environ['TARGET']
autogenargs += ' --target='+os.environ['TARGET']
autogenargs += ' --disable-docs'
autogenargs += ' --disable-static'
autogenargs += ' --enable-all-warnings'
autogenargs += ' --enable-maintainer-mode'
autogenargs += ' --enable-explicit-deps=no'
autogenargs += ' --prefix='+prefix 

for tool in ('AR', 'RANLIB', 'STRIP', 'AS',
	     'DLLTOOL', 'OBJDUMP', 'NM', 'WINDRES'):
	autogenargs += ' '+tool+'="'+os.environ[tool]+'" '

#Module specific configure arguments
module_autogenargs['zlib']    = autogenargs
module_autogenargs['libbz2'] = ' --prefix='+prefix + ' --shared'
module_autogenargs['pcre']    = autogenargs + ' --enable-utf8' + ' --enable-shared'
module_autogenargs['gettext'] = autogenargs + """ --without-emacs \
						  --disable-libasprintf \
                                                  --disable-java \
                                                  --disable-native-java \
                                                  --enable-relocatable"""

#module_autogenargs['jpeg']   = ' --enable-shared' + ' --disable-static' + ' --prefix='+prefix 

module_autogenargs['glib']	=    autogenargs + """ --enable-explicit-deps=no \
                                                  --cache-file=win32.cache \
                                                  --disable-gtk-doc"""
module_autogenargs['freetype']  = autogenargs
module_autogenargs['png']  = autogenargs + """ --without-libpng-compat"""
module_autogenargs['fontconfig']= autogenargs + """ --with-arch=x86"""
module_autogenargs['pango']	= autogenargs + """ --disable-gtk-doc \
                                                  --enable-explicit-deps=no \
                                                  --with-included-modules"""
#						  --disable-ft
module_autogenargs['pixman']	= autogenargs + """ --enable-explicit-deps=no \
                                                  --enable-gtk=no \
                                                  --enable-xlib=no \
                                                  --enable-xlib-xrender=no \
                                                  --enable-win32-font=yes"""
module_autogenargs['cairo']	= autogenargs + """ --enable-explicit-deps=no \
                                                  --enable-xlib=no \
                                                  --enable-xlib-xrender=no \
                                                  --enable-win32-font=yes \
						  --enable-ft=yes \
						  --disable-static \
						  --enable-shared"""
#						  --enable-ft=no
module_autogenargs['psiconv']	= autogenargs + """ --disable-xhtml-docs \
						    --disable-html4-docs \
						    --disable-ascii-docs \
						    --without-imagemagick"""

module_autogenargs['libxml2']	= autogenargs + """ --disable-scrollkeeper --without-iconv"""
module_autogenargs['libxslt']	= autogenargs + """ --without-crypto"""

autogenargs += """ --disable-scrollkeeper --disable-gtk-doc"""

module_autogenargs['atk']	= autogenargs + """ --disable-glibtest --enable-introspection=no"""
module_autogenargs['gtk+']	= autogenargs + """ --disable-glibtest --enable-gdiplus --without-libjasper  --without-libtiff --without-libjpeg --enable-cups=no --enable-introspection=no"""

module_autogenargs['libgda']	= autogenargs + """ --without-odbc --without-java --without-libsoup"""
module_autogenargs['pxlib']	= autogenargs + """ --with-gsf=""" + prefix
module_autogenargs['libglade']	= autogenargs
module_autogenargs['pygobject']	= autogenargs + """ --without-ffi --without-gio-unix"""
module_autogenargs['libgsf']	= autogenargs + """ --without-gnome-vfs --without-bonobo"""
module_autogenargs['goffice']	= autogenargs + """ --without-gconf --with-gmathml --without-long-double"""
module_autogenargs['gnumeric']	= autogenargs + """ --disable-component --without-perl"""
module_autogenargs['poppler']	= autogenargs + """ --disable-cms"""
module_autogenargs['evince']	= autogenargs + """ --without-gconf --without-keyring"""
