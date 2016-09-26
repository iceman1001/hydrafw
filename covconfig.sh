#!/bin/bash

## 20160923, iceman

## remove old
rm /home/user/cov-analysis-linux-7.7.0.4/config/coverity_config.xml
rm -rf /home/user/cov-analysis-linux-7.7.0.4/config/gcc-config-?
rm -rf /home/user/cov-analysis-linux-7.7.0.4/config/g++-config-?

## Configure ARM ,  make sure you have the arm gcc in your $PATH variable.
##cov-configure --comptype gcc --compiler /opt/launchpad.net/gcc-arm-none-eabi-4_7-2013q3/bin/arm-none-eabi-gcc
##cov-configure -co arm-none-eabi-gcc -- -mthumb-interwork
cov-configure --comptype gcc --compiler /opt/launchpad.net/gcc-arm-none-eabi-4_7-2013q3/bin/arm-none-eabi-gcc -- -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16 -mno-thumb-interwork -mthumb
