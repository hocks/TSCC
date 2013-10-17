Summary: rocks-command-torque
Name: rocks-command-torque
Version: 6.1
Release: 0
License: University of California
Vendor: Rocks Clusters
Group: System Environment/Base
Source: rocks-command-torque-6.1.tar.gz
Buildroot: /opt/rocks/share/devel/src/roll/torque-roll/src/rocks-command/rocks-command-torque.buildroot




%description
rocks-command-torque
%prep
%setup
%build
printf "\n\n\n### build ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/rocks-command/rocks-command-torque.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/rocks-command/rocks-command-torque.spec.mk build
%install
printf "\n\n\n### install ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/rocks-command/rocks-command-torque.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/rocks-command/rocks-command-torque.spec.mk install
%files 
/

