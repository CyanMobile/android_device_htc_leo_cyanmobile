#!/bin/sh
# This script is included in squisher
# It is the final build step (after OTA package)

echo "updater-script: Making Compatible Update script"
cd $REPACK/ota/META-INF/com/google/android
echo 'format("MTD", "boot");' >> temp
echo 'mount("MTD", "boot", "/boot");' >> temp
echo 'package_extract_dir("boot", "/boot");' >> temp
echo 'unmount("/boot");' >> temp
echo 'package_extract_file("check_data_app", "/tmp/check_data_app");' >> temp
echo 'set_perm(0, 0, 0777, "/tmp/check_data_app");' >> temp
echo 'run_program("/tmp/check_data_app");' >> temp
echo 'package_extract_dir("data", "/data");' >> temp
echo 'set_perm_recursive(1000, 1000, 0771, 0644, "/data/app");' >> temp
grep -vw assert  updater-script >> temp
rm -rf updater-script
grep -vw boot.img  temp > updater-script
rm -rf temp

cd $REPACK/ota
echo "Removing: $REPACK/ota/boot.img"
rm -rf $REPACK/ota/boot.img
echo "Removing: $REPACK/ota/boot"
rm -rf $REPACK/ota/boot

echo "Copying: $OUT/boot ($REPACK/ota/boot)"
cp -a $OUT/boot $REPACK/ota/boot
echo "Copying: check_data_app ($REPACK/ota)"
cp -a $OUT/scripts/check_data_app $REPACK/ota/check_data_app
echo "Copying: updater-script ($REPACK/ota/META-INF)"
cp -a $OUT/scripts/updater-script $REPACK/ota/META-INF/com/google/android/updater-script
echo "Copying: update-binary ($REPACK/ota/META-INF)"
cp -a $OUT/scripts/update-binary $REPACK/ota/META-INF/com/google/android/update-binary

if [ ! -e $REPACK/ota/boot/initrd.gz ] ; then
  echo "Copying: $OUT/ramdisk.img ($REPACK/ota/boot/initrd.gz)"
  cp -a $OUT/ramdisk.img $REPACK/ota/boot/initrd.gz
fi

