# KERNELDIR points to the pre-built kernel tree for module compilation
KERNELDIR ?= /root/clion/kernel/linux-6.16.2/kgdb_build

obj-m += ptemap.o
ptemap-objs := ptemap_main.o ptemap_core.o ptemap_cdev.o ptemap_debugfs.o

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
