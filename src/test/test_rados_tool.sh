#!/bin/bash

die() {
    echo "$@"
    exit 1
}

usage() {
    cat <<EOF
test_rados_tool.sh: tests rados_tool
-c:        RADOS configuration file to use [optional]
-k:        keep temp files
-h:        this help message
-p:        set temporary pool to use [optional]
EOF
}

run_expect_fail() {
    echo "RUN_EXPECT_FAIL: " $@
    $@
    [ $? -eq 0 ] && die "expected failure, but got success! cmd: $@"
}

run_expect_succ() {
    echo "RUN_EXPECT_SUCC: " $@
    $@
    [ $? -ne 0 ] && die "expected success, but got failure! cmd: $@"
}

run() {
    echo "RUN: " $@
    $@
}

DNAME="`dirname $0`"
DNAME="`readlink -f $DNAME`"
RADOS_TOOL="`readlink -f \"$DNAME/../rados\"`"
KEEP_TEMP_FILES=0
POOL=trs_pool
[ -x "$RADOS_TOOL" ] || die "couldn't find $RADOS_TOOL binary to test"
[ -x "$RADOS_TOOL" ] || die "couldn't find $RADOS_TOOL binary"
which attr &>/dev/null
[ $? -eq 0 ] || die "you must install the 'attr' tool to manipulate \
extended attributes."

while getopts  "c:hkp:" flag; do
    case $flag in
        c)  RADOS_TOOL="$RADOS_TOOL -c $OPTARG";;
        k)  KEEP_TEMP_FILES=1;;
        h)  usage; exit 0;;
        p)  POOL=$OPTARG;;
        *)  echo; usage; exit 1;;
    esac
done

TDIR=`mktemp -d -t test_rados_tool.XXXXXXXXXX` || die "mktemp failed"
[ $KEEP_TEMP_FILES -eq 0 ] && trap "rm -rf ${TDIR}; exit" INT TERM EXIT

mkdir "$TDIR/dira"
attr -q -s "rados_sync_ver" -V "1" "$TDIR/dira"
touch "$TDIR/dira/foo"
attr -q -s "rados_full_name" -V "foo" "$TDIR/dira/foo"
touch "$TDIR/dira/foo2"
attr -q -s "rados_full_name" -V "foo2" "$TDIR/dira/foo2"
touch "$TDIR/dira/bar"
attr -q -s "rados_full_name" -V "bar" "$TDIR/dira/bar"
mkdir "$TDIR/dirb"
attr -q -s "rados_sync_ver" -V "1" "$TDIR/dirb"
mkdir "$TDIR/dirc"
attr -q -s "rados_sync_ver" -V "1" "$TDIR/dirc"
touch "$TDIR/dirc/foo"
attr -q -s "rados_full_name" -V "foo" "$TDIR/dirc/foo"
attr -q -s "rados.toothbrush" -V "toothbrush" "$TDIR/dirc/foo"
attr -q -s "rados.toothpaste" -V "crest" "$TDIR/dirc/foo"
attr -q -s "rados.floss" -V "myfloss" "$TDIR/dirc/foo"
touch "$TDIR/dirc/foo2"
attr -q -s "rados.toothbrush" -V "green" "$TDIR/dirc/foo2"
attr -q -s "rados_full_name" -V "foo2" "$TDIR/dirc/foo2"

# make sure that --create works
run "$RADOS_TOOL" rmpool "$POOL"
run_expect_succ "$RADOS_TOOL" --create import "$TDIR/dira" "$POOL"

# make sure that lack of --create fails
run_expect_succ "$RADOS_TOOL" rmpool "$POOL"
run_expect_fail "$RADOS_TOOL" import "$TDIR/dira" "$POOL"

run_expect_succ "$RADOS_TOOL" --create import "$TDIR/dira" "$POOL"

# inaccessible import src should fail
run_expect_fail "$RADOS_TOOL" import "$TDIR/dir_nonexistent" "$POOL"

# export some stuff
run_expect_succ "$RADOS_TOOL" --create export "$POOL" "$TDIR/dirb"
diff -q -r "$TDIR/dira" "$TDIR/dirb" \
    || die "failed to export the same stuff we imported!"

# import some stuff with extended attributes on it
run_expect_succ "$RADOS_TOOL" import "$TDIR/dirc" "$POOL" | tee "$TDIR/out"
run_expect_succ grep -q '\[xattr\]' $TDIR/out

# the second time, the xattrs should match, so there should be nothing to do.
run_expect_succ "$RADOS_TOOL" import "$TDIR/dirc" "$POOL" | tee "$TDIR/out"
run_expect_fail grep -q '\[xattr\]' "$TDIR/out"

# now force it to copy everything
run_expect_succ "$RADOS_TOOL" --force import "$TDIR/dirc" "$POOL" | tee "$TDIR/out2"
run_expect_succ grep '\[force\]' "$TDIR/out2"

# export some stuff with extended attributes on it
run_expect_succ "$RADOS_TOOL" -C export "$POOL" "$TDIR/dirc_copy"

# check to make sure extended attributes were preserved
PRE_EXPORT=`attr -qg user.rados.toothbrush "$TDIR/dirc/foo"`
POST_EXPORT=`attr -qg user.rados.toothbrush "$TDIR/dirc_copy/foo"`
if [ "$PRE_EXPORT" != "$POST_EXPORT" ]; then
    die "xattr not preserved across import/export! \
\$PRE_EXPORT = $PRE_EXPORT, \$POST_EXPORT = $POST_EXPORT"
fi

# trigger a rados delete using --delete-after
run_expect_succ "$RADOS_TOOL" --create export "$POOL" "$TDIR/dird"
rm -f "$TDIR/dird/foo"
run_expect_succ "$RADOS_TOOL" --delete-after import "$TDIR/dird" "$POOL" | tee "$TDIR/out3"
run_expect_succ grep '\[deleted\]' "$TDIR/out3"

# trigger a local delete using --delete-after
run_expect_succ "$RADOS_TOOL" --delete-after export "$POOL" "$TDIR/dirc" | tee "$TDIR/out4"
run_expect_succ grep '\[deleted\]' "$TDIR/out4"
[ -e "$TDIR/dird/foo" ] && die "--delete-after failed to delete a file!"

# test hashed pathnames
mkdir "$TDIR/dird"
attr -q -s "rados_sync_ver" -V "1" "$TDIR/dird"
touch "$TDIR/dird/bar@bar_00000000000055ca"
attr -q -s "rados_full_name" -V "bar/bar" "$TDIR/dird/bar@bar_00000000000055ca"
run_expect_succ "$RADOS_TOOL" --delete-after import "$TDIR/dird" "$POOL" | tee "$TDIR/out5"
run_expect_succ grep '\[imported\]' "$TDIR/out5"
run_expect_succ "$RADOS_TOOL" --delete-after --create export "$POOL" "$TDIR/dire" | tee "$TDIR/out6"
run_expect_succ grep '\[exported\]' "$TDIR/out6"
diff -q -r "$TDIR/dird" "$TDIR/dire" \
    || die "failed to export the same stuff we imported!"

# create a temporary file and validate that export deletes it
touch "$TDIR/dire/tmp\$tmp"
run_expect_succ "$RADOS_TOOL" --delete-after --create export "$POOL" "$TDIR/dire" | tee "$TDIR/out7"
run_expect_succ grep temporary "$TDIR/out7"

echo "SUCCESS!"
exit 0
