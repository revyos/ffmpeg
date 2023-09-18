echo "Please modify the --sysroot to the absolute path of the document and remove the # sign below!"
#riscv64-linux-gcc -g -mcpu=c910 -fstack-protector-strong -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security --no-sysroot-suffix --sysroot=......./yocto/thead-build/light-fm/tmp-glibc/work/riscv64-oe-linux/ffmpeg/4.3.1-r0/recipe-sysroot api-dec-test.c -o api-dec-test \
#-I../../../image/usr/include \
#-L../../../image/usr/lib  -lavcodec -lavformat -lavutil -lswscale -lswresample -lavresample -ldl
