Summary: mauiweb
Name: mauiweb
Version: 0.0.1
Release: 0
License: University of California
Vendor: Rocks Clusters
Group: System Environment/Base
Source: mauiweb-0.0.1.tar.gz
Buildroot: /opt/rocks/share/devel/src/roll/torque-roll/src/mauiweb/mauiweb.buildroot




%description
mauiweb
%prep
%setup
%build
printf "\n\n\n### build ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/mauiweb/mauiweb.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/mauiweb/mauiweb.spec.mk build
%install
printf "\n\n\n### install ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/mauiweb/mauiweb.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/mauiweb/mauiweb.spec.mk install
%files 
/

