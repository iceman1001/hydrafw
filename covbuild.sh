#!/bin/bash

## 2016-09-23,  Iceman
## build script for Coverity Scan of the Hydrabus source code

## clean up pre-compiled objects.
make clean

## coverity build
cov-build --dir cov-int make all

## delete all previous tarballs
rm hydrabus.all.*.tgz

##
VERSION="0.1.`date --date now +%H%M`"
TODAY="`date --date now +%Y%m%d.%H%M`"
DESCNAME="autoHydra.$TODAY"
FILENAME=hydrabus.all.$TODAY.tgz

## create tarball
tar cfz $FILENAME cov-int
echo "Coverity build file is ready"


## clean up build folders
rm -rf cov-int
echo "Coverity build cleaned"

## upload tarball to Coverity.com
curl --form token=TjsqhldtBi6uEqoqDnICAw \
 --form email=herrmann1001@gmail.com \
  --form file=@$FILENAME \
  --form version="$VERSION" \
  --form description="$DESCNAME" \
  https://scan.coverity.com/builds?project=hydrabusfw
echo "tarball uploaded to Coverity for analyse"
