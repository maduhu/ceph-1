#!/bin/bash -x

mkdir -p testspace
cfuse testspace -m $1

./runallonce.sh testspace
killall cfuse
