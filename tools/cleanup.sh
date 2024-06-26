#!/usr/bin/env bash
set -x # show cmds

# $1 -> mounted volume
if [ -z "$1" ]; then
	echo "Please supply the correct arguments!"
	exit 1
fi

sudo umount "${1}"
sudo losetup -d /dev/loop101
sudo losetup -d /dev/loop102
