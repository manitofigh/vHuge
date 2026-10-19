#!/bin/sh
set -eu

DEV=/dev/pteditor

if [ -e "$DEV" ]; then
	chmod 0600 "$DEV"
	exit 0
fi

modprobe pteditor

i=0
while [ "$i" -lt 50 ]; do
	if [ -e "$DEV" ]; then
		chmod 0600 "$DEV"
		exit 0
	fi
	i=$((i + 1))
	sleep 0.1
done

echo "pteditor loaded but $DEV did not appear" >&2
exit 1
