HOMEDIR=/home/beyond2002
#build kernel
cd $HOMEDIR/project/linux-stable
for i in ../bananian/kernel/4.3.3/patches/*; do patch -p1 < $i; done
cp -av ../jailhouse/ci/kernel-config-banana-pi .config
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j8 uImage modules dtbs LOADADDR=40008000
#build jailhouse
cp -av $HOMEDIR/project/freertos-cell/jailhouse-configs/bananapi.c $HOMEDIR/project/jailhouse/configs/bananapi.c
cp -av $HOMEDIR/project/freertos-cell/jailhouse-configs/bananapi-freertos-demo.c $HOMEDIR/project/jailhouse/configs/
cp -av $HOMEDIR/project/jailhouse/ci/jailhouse-config-banana-pi.h $HOMEDIR/project/jailhouse/hypervisor/include/jailhouse/config.h
cd $HOMEDIR/project/jailhouse
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR=../linux-stable
#build freertos
cd $HOMEDIR/project/freertos-cell
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR=../linux-stable

