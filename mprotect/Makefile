MODULE_NAME	:= mp
obj-m  += $(MODULE_NAME).o
$(MODULE_NAME)-objs := hook.o

PWD:=$(shell pwd)
KERNELPATH:=/lib/modules/$(shell uname -r)/build

all:
	@echo "|----------------------------------|"
	@echo "| building mprotect (kernel mode)  |"
	@echo "|----------------------------------|"
	make -C $(KERNELPATH) M=$(PWD) modules
	gcc -o ./mprotect mprotect.c
	@$(RM) *.o

clean:
	rm -rf ./mprotect
	make -C $(KERNELPATH) M=$(PWD) clean
