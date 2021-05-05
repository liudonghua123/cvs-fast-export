#!/bin/sh
## Test commit and blob filtering with --
out="/tmp/incremental-out-$$"
while getopts os opt
do
    case $opt in
	o) out=/dev/stdout;;
	s) opts="-t 0";;
	*) echo "$0: unknown option $opt" ;;
    esac
done
# shellcheck disable=SC2004
shift $(($OPTIND - 1))

trap '[ $out != /dev/stdout ] && rm -f $out' EXIT HUP INT QUIT TERM

# shellcheck disable=SC2006
idate=$(date -u -d"`rlog -r1.1.2.2 twobranch.repo/module/README,v | grep date | sed "s/date: \(.*\)\;  author.*/\1/"`" +%s)
# shellcheck disable=SC2003,SC2046,SC2086
find twobranch.repo/ -name "*,v" | cvs-fast-export $opts -i $(expr $idate - 1) >$out

if [ "$out" != /dev/stdout ]
then
    # :7 and :9 are the blobs attached to the selected commits
    if grep -q ":7" $out && grep -q ":9" $out
    then
	echo "ok - $0"
    else
	echo "not ok - $0"
	exit 1
    fi
fi

#end
