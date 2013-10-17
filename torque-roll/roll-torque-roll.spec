Summary: torque-roll roll
Name: roll-torque-roll
Version: 6.0.0
Release: 1
License: University of California
Vendor: Rocks Clusters
Group: System Environment/Base
Source: roll-torque-roll-6.0.0.tar.gz
Buildroot: /opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.buildroot
Prefix: /export/profile
Buildarch: noarch


%description
XML files for the torque-roll roll

%package kickstart
Summary: torque-roll roll
Group: System Environment/Base
%description kickstart
XML files for the torque-roll roll
%prep
%setup
%build
printf "\n\n\n### build ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.spec.mk build
%install
printf "\n\n\n### install ###\n\n\n"
BUILDROOT=/opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.buildroot make -f /opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.spec.mk install
%files kickstart
/export/profile

