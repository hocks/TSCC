Summary: torque-openmpi
Name: torque-openmpi
Version: 0.0.1
Release: 0
License: University of California
Vendor: Rocks Clusters
Group: System Environment/Base
Source: torque-openmpi-0.0.1.tar.gz
Buildroot: /opt/rocks/share/devel/src/roll/torque-roll/src/torque-openmpi/torque-openmpi.buildroot




%description
torque-openmpi
%prep
%setup
%build
printf "\n\n\n### build ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/torque-openmpi/torque-openmpi.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/torque-openmpi/torque-openmpi.spec.mk build
%install
printf "\n\n\n### install ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/src/torque-openmpi/torque-openmpi.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/src/torque-openmpi/torque-openmpi.spec.mk install
%files 
/

