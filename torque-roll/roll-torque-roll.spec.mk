# This file is called from the generated spec file.
# It can also be used to debug rpm building.
# 	make -f roll-torque-roll.spec.mk build|install

ifndef __RULES_MK
build:
	make ROOT=/opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.buildroot build

install:
	make ROOT=/opt/rocks/share/devel/src/roll/torque-roll/roll-torque-roll.buildroot install
endif
