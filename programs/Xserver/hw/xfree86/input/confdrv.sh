#!/bin/sh

# $XFree86: xc/programs/Xserver/hw/xfree86/input/confdrv.sh,v 1.4 1999/05/17 13:17:16 dawes Exp $
#
# This script generates drvConf.c
#
# usage: confdrv.sh driver1 driver2 ...
#

DRVCONF=./drvConf.c

cat > $DRVCONF <<EOF
/*
 * This file is generated automatically -- DO NOT EDIT
 */

#include "xf86.h"
#include "xf86Xinput.h"

extern InputDriverRec
EOF
Args=`echo $* | tr '[a-z]' '[A-Z]'`
set - $Args
while [ $# -gt 1 ]; do
  echo "#undef $1" >> $DRVCONF
  echo "	$1," >> $DRVCONF
  shift
done
echo "#undef $1" >> $DRVCONF
echo "	$1;" >> $DRVCONF
cat >> $DRVCONF <<EOF

InputDriverPtr xf86InputDriverList[] =
{
EOF
for i in $Args; do
  echo "	&$i," >> $DRVCONF
done
cat >> $DRVCONF <<EOF
};

int xf86NumInputDrivers = sizeof(xf86InputDriverList) / sizeof(xf86InputDriverList[0]);

EOF
