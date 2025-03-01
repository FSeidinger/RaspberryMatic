#!/bin/sh

# Stop on error
set -e

#MKIMAGE=${HOST_DIR}/usr/bin/mkimage
BOARD_DIR="$(dirname "$0")"
BOARD_NAME="$(basename "${BOARD_DIR}")"

#
# Create user filesystem
#
echo "Create user filesystem"
mkdir -p "${BUILD_DIR}/userfs"
touch "${BUILD_DIR}/userfs/.doFactoryReset"
rm -f "${BINARIES_DIR}/userfs.ext4"
"${HOST_DIR}/sbin/mkfs.ext4" -d "${BUILD_DIR}/userfs" -F -L userfs -I 256 -E lazy_itable_init=0,lazy_journal_init=0 "${BINARIES_DIR}/userfs.ext4" 3000

#
# VERSION File
#
cp "${TARGET_DIR}/boot/VERSION" "${BINARIES_DIR}"

# prepare grub config and bootloader
mkdir -p "${BINARIES_DIR}/boot/grub"
cp -a "${BOARD_DIR}/grub.cfg" "${BINARIES_DIR}/boot/grub/"

# create *.img file using genimage
support/scripts/genimage.sh -c "${BR2_EXTERNAL_EQ3_PATH}/board/${BOARD_NAME}/genimage.cfg"

exit $?
