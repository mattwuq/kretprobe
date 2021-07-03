MODULE_NAME	:= krp
obj-m  += $(MODULE_NAME).o
$(MODULE_NAME)-objs := kmod.o

PWD:=$(shell pwd)
KERNELPATH:=/lib/modules/$(shell uname -r)/build

all:
	@echo "|-----------------------------------|"
	@echo "| building krp array (kernel mode)  |"
	@echo "|-----------------------------------|"
	make -C $(KERNELPATH) M=$(PWD) modules
	@$(RM) *.o .*.cmd

clean:
	make -C $(KERNELPATH) M=$(PWD) clean
