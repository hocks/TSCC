Summary: maui
Name: maui
Version: 3.3.1
Release: 2
License: University of California
Vendor: Rocks Clusters
Group: System Environment/Base
Source: maui-3.3.1.tar.gz
Buildroot: /opt/rocks/share/devel/src/roll/torque-roll/src/maui/maui.buildroot




%description
maui
%prep
%setup
%build
printf "\n\n\n### build ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/maui/maui.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/maui/maui.spec.mk build
%install
printf "\n\n\n### install ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/maui/maui.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/maui/maui.spec.mk install
%files 
/

