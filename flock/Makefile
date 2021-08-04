MODULE_NAME	:= flock
obj-m  += $(MODULE_NAME).o
$(MODULE_NAME)-objs := hook.o

PWD:=$(shell pwd)
KERNELPATH:=/lib/modules/$(shell uname -r)/build

all:
	@echo "|----------------------------------|"
	@echo "| building flock (kernel mode)  |"
	@echo "|----------------------------------|"
	make -C $(KERNELPATH) M=$(PWD) modules
	gcc -o ./flockc flockc.c
	@$(RM) *.o

clean:
	rm -rf ./flockc
	make -C $(KERNELPATH) M=$(PWD) clean
