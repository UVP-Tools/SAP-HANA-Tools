###############################################################################
###### File name   : Makefile											######
###### Author	  :													    ######
###### Modify	  : Songjiangtao 00202546							    ######
###### Description : To optimize the PV_ON_HVM drivers's Makefile		######
###### History	 :													    ######
######			   2012-06-4 : Create the file						    ######
###############################################################################

M=$(shell pwd)
-include $(M)/config.mk

COMPILEARGS = $(CROSSCOMPILE)

reboot=$(OSVERSION)/reboot.c
obj-m += xen-platform-pci/
obj-m += xen-balloon/
obj-m += xen-vbd/
obj-m += xen-vnif/
obj-m += xen-hcall/
ifneq (3.0.101, $(OSVERSION))
obj-m += xen-vmdq/
endif
ifeq (2.6.32, $(OSVERSION))
ifeq (x86_64, $(KERNELARCH))
reboot=reboot_64.c
endif
endif

ifeq (3.0.58, $(OSVERSION))
OSVERSION=3.0.13
endif

ifeq (3.0.80, $(OSVERSION))
OSVERSION=3.0.13
endif

###vni_front for VSA
ifeq ("2.6.32.59-0.7-default", "$(BUILDKERNEL)")
obj-m += vni_front/
endif

ifeq ("3.0.13-0.27-default", "$(BUILDKERNEL)")
obj-m += vni_front/
endif

ifeq ("3.0.13-0.27-pae", "$(BUILDKERNEL)")
obj-m += vni_front/
endif

ifeq (3.0.76, $(OSVERSION))
obj-m += vni_front/
endif

ifeq (3.0.101, $(OSVERSION))
obj-m += xen-scsi/
endif

all:lnfile
	make -C $(KERNDIR) M=$(M) modules $(COMPILEARGS)

modules_install:lnfile
	make -C $(KERNDIR) M=$(M) modules_install $(COMPILEARGS)

lnfile:
	@set -e; \
	for dir in $(obj-m); \
	do \
		if [ "xen-hcall/" = "$$dir" ] || [ "xen-vmdq/" = "$$dir" ]; then \
			continue; \
		fi; \
		cd $$dir; \
		for file in `ls $(OSVERSION)`; \
		do \
			ln -sbf $(OSVERSION)/$$file $$file; \
		done; \
		cd -; \
	done; \
	cd xen-platform-pci/; \
	ln -sf $(reboot) reboot.c; \
	cd ..

clean:
	make -C $(KERNDIR) M=$(M) clean $(COMPILEARGS)
	find ./ -name modules.order | xargs rm -f
	rm -rf Module.symvers
	cd $$M
	find ./ -type l | xargs rm -f
	find ./ -name '*~' | awk -F'~' '{print $$1}' | xargs -i mv {}~ {}
