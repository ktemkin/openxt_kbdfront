export KDIR ?= /lib/modules/`uname -r`/build

# Compiler Flags
ccflags-y := -Wall -Werror

# Build the OpenXT framebuffer module.
obj-m += openxt-kbdfront.o

all: modules
install: modules_install

modules:
	$(MAKE) -C $(KDIR) M=$(shell pwd)

modules_install: 
	$(MAKE) -C $(KDIR) M=$(shell pwd) modules_install
