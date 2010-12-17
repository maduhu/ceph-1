#!/bin/bash -x

#
# Add some objects to the data PGs, and then test splitting those PGs
#

# Includes
source "`dirname $0`/test_common.sh"

# Constants
my_write_objects() {
        write_objects $1 $2 10 1000000 data
}

setup() {
        export CEPH_NUM_OSD=$1

        # Start ceph
        ./stop.sh

        ./vstart.sh -d -n
}

get_pgp_num() {
        ./ceph osd pool get data pgp_num > $TEMPDIR/pgp_num
        [ $? -eq 0 ] || die "failed to get pgp_num"
        PGP_NUM=`grep PGP_NUM $TEMPDIR/pgp_num | sed 's/.*PGP_NUM:\([ 0123456789]*\).*$/\1/'`
}

split1_impl() {
        # Write lots and lots of objects
        my_write_objects 1 2

        get_pgp_num
        echo "\$PGP_NUM=$PGP_NUM"

        # Double the number of PGs
        PGP_NUM=$((PGP_NUM*2))
        echo "doubling PGP_NUM to $PGP_NUM..."
        ./ceph osd pool set data pgp_num $PGP_NUM

        sleep 30

        # success
        return 0
}

split1() {
        setup 2
        split1_impl
}

run() {
        split1 || die "test failed"
}

$@