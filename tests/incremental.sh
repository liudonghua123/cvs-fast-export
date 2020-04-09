#!/bin/sh
## Test commit and blovb filtering with --
out="/tmp/incremental-out-$$"
mode=test
while getopts os opt
do
    case $opt in
	o) out=/dev/stdout;;
	s) opts="-t 0";;
    esac
done
# shellcheck disable=SC2004
shift $(($OPTIND - 1))

trap '[ $out != /dev/stdout ] && rm -f $out' EXIT HUP INT QUIT TERM

idate=$(date -u -d"`rlog -r1.1.2.2 twobranch.repo/module/README,v | grep date | sed "s/date: \(.*\)\;  author.*/\1/"`" +%s)
find twobranch.repo/ -name "*,v" | cvs-fast-export $opts -i `expr $idate - 1` >$out

if [ "$out" != /dev/stdout ]
then
    # :7 and :9 are the blobs attached to the selected commits
    if grep -q ":7" $out && grep -q ":9" $out
    then
	echo "$0: PASSED"
    else
	echo "$0: FAILED"
	exit 1
    fi
fi

#end
