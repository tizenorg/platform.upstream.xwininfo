Summary: window information utility for X
Name: xwininfo
Version: 1.1.3
Release: 1
License: MIT
Group: User Interface/X
URL: http://www.x.org
Source: %{name}-%{version}.tar.gz

BuildRequires: pkgconfig(dmx) pkgconfig(xext) pkgconfig(xft) pkgconfig(xrandr)
BuildRequires: pkgconfig(xi) pkgconfig(xinerama) pkgconfig(xmu)
BuildRequires: pkgconfig(xpm) pkgconfig(xt) pkgconfig(xtst) pkgconfig(xv)
BuildRequires: pkgconfig(xxf86dga) pkgconfig(xxf86vm)
BuildRequires: pkgconfig(xcb) pkgconfig(xcb-atom)
BuildRequires: gettext

%description
A collectty  for  displaying  information about windows.  Various
information is displayed depending on  which  options  are  selected.
If  no options are chosen, -stats is assumed.ion of client utilities
which can be used to query the X server for various information.

%prep
%setup -q

%build
%autogen
make

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p %{buildroot}/usr/share/license
make install DESTDIR=$RPM_BUILD_ROOT

%remove_docs

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%license COPYING
#%doc
%{_bindir}/*
#%{_bindir}/xwininfo
#%{_mandir}/man1/xwininfo.1*
