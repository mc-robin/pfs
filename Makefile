obj-m := pfs.o
pfs-objs := super.o alloc.o dir.o file.o inode.o namei.o

all: drive mkfs

drive:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
mkfs_SOURCES:
	mkfs.c pfs.h pfs_fs.h
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm mkfs
