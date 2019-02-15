#!/bin/sh
if [ ! -n "$PERL" ]
then
	PERL=perl
fi

incdir=`dirname $0`
. $incdir/test_functions.sh

failed=0

if $PERL -e 'eval require Test::More;' > /dev/null 2>&1; then
  for f in pidl/tests/*.pl; do
     testit "$f" $PERL $f || failed=`expr $failed + 1`
  done
else 
   echo "Skipping pidl tests - Test::More not installed"
fi

testok $0 $failed
