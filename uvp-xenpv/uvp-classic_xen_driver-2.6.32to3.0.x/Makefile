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

obj-m += xen-platform-pci/
obj-m += xen-balloon/
obj-m += xen-vbd/
obj-m += xen-vnif/
obj-m += xen-hcall/

ifeq ("SUSE11SP3", $(OSTYPE))
OSVERSION=3.0.76
obj-m += vni_front/
endif

ifeq ("SUSE11SP4", $(OSTYPE))
OSVERSION=3.0.101
obj-m += xen-scsi/
endif

reboot=$(OSVERSION)/reboot.c

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
