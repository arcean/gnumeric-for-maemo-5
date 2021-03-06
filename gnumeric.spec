# This file is basically not maintained.  Use as inspiration only.

%define  RELEASE 1
%define  rel     %{?CUSTOM_RELEASE} %{!?CUSTOM_RELEASE:%RELEASE}

Name:             gnumeric
Epoch:            %epoch
Version:          1.10.17
Release:          %rel
Summary:          Spreadsheet program for GNOME
Group:            Applications/Productivity
License:          GPLv2
URL:              http://www.gnome.org/gnumeric/
Source:           ftp://ftp.gnome.org/pub/GNOME/sources/%{name}/1.9/%{name}-%{version}.tar.bz2
BuildRoot:        %{_tmppath}/%{name}-%{PACKAGE_VERSION}-root
BuildRequires:    goffice-devel >= 0.8.0
BuildRequires:    libgsf-devel >= 1.14.15
BuildRequires:    libxml-2.0-devel >= 2.4.12
BuildRequires:    glib-2.0-devel >= 2.12.0
BuildRequires:    pango-devel >= 1.12.0
BuildRequires:    scrollkeeper
Requires:         scrollkeeper hicolor-icon-theme
Requires(pre):    GConf2
Requires(post):   /sbin/ldconfig GConf2 scrollkeeper
Requires(preun):  GConf2
Requires(postun): /sbin/ldconfig scrollkeeper

%description
This is Gnumeric, a spreadsheet for GNOME.  It aims to be a drop in
replacement for proprietary spreadsheets.  It provides import/export from
MS Excel files and many other formats (odf, csv, latex, xbase, applix, quattro pro,
planperfect).



%package devel
Summary: Files necessary to develop gnumeric-based applications
Group: Development/Libraries
Requires: %{name} = %{epoch}:%{PACKAGE_VERSION}-%{release}
Requires: pkgconfig

%description devel
Gnumeric is a spreadsheet program for the GNOME GUI desktop
environment. The gnumeric-devel package includes files necessary to
develop gnumeric-based applications.


%package plugins-extras
Summary:          Files necessary to develop gnumeric-based applications
Group:            Applications/Productivity
Requires:         %{name} = %{epoch}:%{PACKAGE_VERSION}-%{release}
Requires:         perl(:MODULE_COMPAT_%(eval "`%{__perl} -V:version`"; echo $version))

%description plugins-extras
This package contains the following additional plugins for gnumeric:
* gda and gnomedb plugins:
  Database functions for retrieval of data from a database.
* perl plugin:
  This plugin allows writing of plugins in perl


%prep
%setup -q

%build
%configure --enable-ssindex
make %{?_smp_mflags}


%install

rm -rf $RPM_BUILD_ROOT

export GCONF_DISABLE_MAKEFILE_SCHEMA_INSTALL=1
make DESTDIR=$RPM_BUILD_ROOT install
unset GCONF_DISABLE_MAKEFILE_SCHEMA_INSTALL

%find-lang %{name}

mkdir -p $RPM_BUILD_ROOT%{_datadir}/applications
desktop-file-install --vendor gnumeric --delete-original                  \
  --dir $RPM_BUILD_ROOT%{_datadir}/applications                         \
  --add-category Office                                                 \
  --add-category Spreadsheet                                            \
  $RPM_BUILD_ROOT%{_datadir}/applications/*.desktop

#put icon in the proper place
mkdir -p $RPM_BUILD_ROOT/usr/share/icons/hicolor/48x48/apps
mv $RPM_BUILD_ROOT/usr/share/pixmaps/gnome-%{name}.png \
  $RPM_BUILD_ROOT/usr/share/icons/hicolor/48x48/apps/%{name}.png

#remove unused mime type icons
rm $RPM_BUILD_ROOT/%{_datadir}/pixmaps/gnome-application-*.png
rm $RPM_BUILD_ROOT/%{_datadir}/pixmaps/%{name}/gnome-application-*.png

#remove spurious .ico thing
rm $RPM_BUILD_ROOT/usr/share/pixmaps/win32-%{name}.ico
rm $RPM_BUILD_ROOT/usr/share/pixmaps/%{name}/win32-%{name}.ico

#remove scrollkeeper stuff
rm -rf $RPM_BUILD_ROOT/var

#remove .la files
rm $RPM_BUILD_ROOT/%{_libdir}/libspreadsheet.la
rm $RPM_BUILD_ROOT/%{_libdir}/%{name}/%{version}/plugins/*/*.la


%clean
rm -rf $RPM_BUILD_ROOT


%pre
if [ "$1" -gt 1 ]; then
    export GCONF_CONFIG_SOURCE=`gconftool-2 --get-default-source`
    gconftool-2 --makefile-uninstall-rule \
      %{_sysconfdir}/gconf/schemas/%{name}*.schemas > /dev/null || :
fi


%post
/sbin/ldconfig
export GCONF_CONFIG_SOURCE=`gconftool-2 --get-default-source`
/usr/bin/gconftool-2 --makefile-install-rule \
  %{_sysconfdir}/gconf/schemas/%{name}*.schemas > /dev/null || :
scrollkeeper-update -q -o %{_datadir}/omf/%{name} || :
touch --no-create %{_datadir}/icons/hicolor || :
if [ -x %{_bindir}/gtk-update-icon-cache ]; then
   %{_bindir}/gtk-update-icon-cache --quiet %{_datadir}/icons/hicolor || :
fi


%preun
if [ "$1" -eq 0 ]; then
    export GCONF_CONFIG_SOURCE=`gconftool-2 --get-default-source`
    gconftool-2 --makefile-uninstall-rule \
      %{_sysconfdir}/gconf/schemas/%{name}*.schemas > /dev/null || :
fi


%postun
/sbin/ldconfig
scrollkeeper-update -q || :
touch --no-create %{_datadir}/icons/hicolor || :
if [ -x %{_bindir}/gtk-update-icon-cache ]; then
   %{_bindir}/gtk-update-icon-cache --quiet %{_datadir}/icons/hicolor || :
fi


%files -f %{name}.lang
%defattr(-,root,root,-)
%doc HACKING AUTHORS ChangeLog NEWS BUGS README COPYING
%{_sysconfdir}/gconf/schemas/*.schemas
%{_bindir}/*
%{_libdir}/libspreadsheet-%{version}.so
%dir %{_libdir}/%{name}
%{_libdir}/%{name}/%{version}
%exclude %{_libdir}/%{name}/%{version}/include
%exclude %{_libdir}/%{name}/%{version}/plugins/perl-*
#%exclude %{_libdir}/%{name}/%{version}/plugins/gdaif
#%exclude %{_libdir}/%{name}/%{version}/plugins/gnome-db
%{_datadir}/pixmaps/%{name}
%{_datadir}/icons/hicolor/48x48/apps/%{name}.png
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/%{version}
%exclude %{_datadir}/%{name}/%{version}/idl
%{_datadir}/applications/fedora-%{name}.desktop
# The actual omf file is in gnumeric.lang, but find-lang doesn't own the dir!
%dir %{_datadir}/omf/%{name}
%{_mandir}/man1/*

%files devel
%defattr(-,root,root)
%{_datadir}/%{name}/%{version}/idl
%{_libdir}/libspreadsheet.so
%{_libdir}/pkgconfig/libspreadsheet-1.8.pc
%{_includedir}/libspreadsheet-1.8
%{_libdir}/%{name}/%{version}/include

%files plugins-extras
%defattr(-,root,root,-)
%{_libdir}/%{name}/%{version}/plugins/perl-*
#%{_libdir}/%{name}/%{version}/plugins/gdaif
#%{_libdir}/%{name}/%{version}/plugins/gnome-db


%changelog
