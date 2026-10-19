KERNEL ?= $(shell uname -r)
KDIR := /lib/modules/$(KERNEL)/build

obj-m += pteditor.o

ccflags-y += -Wno-unused-result
ccflags-y += -DCC_USING_FENTRY

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
