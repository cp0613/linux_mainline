

- kernel
CONFIG_NONPORTABLE=y
CONFIG_HVC_RISCV_SBI=y
CONFIG_KALLSYMS_ALL=y
CONFIG_LIVEPATCH=y
CONFIG_SAMPLE_LIVEPATCH=m

- selftest
make ARCH=riscv \
  CROSS_COMPILE=/rvhome/chenp/toolchain/gcc/Xuantie-900-gcc-linux-6.6.36-glibc-x86_64-V3.4.0/bin/riscv64-unknown-linux-gnu- \
  KDIR=/mnt/ssd/workarea/chenp/riscv/linux_mainline \
  -C tools/testing/selftests/livepatch

# insmod /mnt/tools/testing/selftests/livepatch/test_modules/test_klp_livepatch.ko
[ 1669.409556] livepatch: enabling patch 'test_klp_livepatch'
[ 1669.442344] livepatch: 'test_klp_livepatch': starting patching transition
# rmmod /mnt/tools/testing/selftests/livepatch/test_modules/test_klp_livepatch.ko