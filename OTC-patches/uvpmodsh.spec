# uvpmodsh.spec
# Sample KMP spec file from Linux Foundation
# http://www.linuxfoundation.org/collaborate/workgroups/driver-backport/samplekmpspecfile
# Adapted by Kurt Garloff <t-systems@garloff.de>


# Following line included for SUSE "build" command; does not affect "rpmbuild"
# norootforbuild
%define buildforkernels current

Name:		uvpmodsh
BuildRequires: %kernel_module_package_buildreqs
License:	GPL-2.0
Group:		System/Kernel
Summary:	Xen kernel drivers for enhanced monitoring and performance 
Version:	2.2.0.312
Release:	18.1
Source0:	SAP-HANA-Tools-%{version}.tar.gz
Source1:	preamble
Patch0:		kverdep.diff
Patch1:		556d973f-unmodified-drivers-tolerate-IRQF_DISABLED-being-undefined.patch
Patch2:		xen-misc-device.diff
Patch3:		respect-ostype.diff
Patch4:		vni_front-fix.diff
Patch5:		module-support.diff
BuildRoot:    %{_tmppath}/%{name}-%{version}-build
# Uncomment the following line to include a required firmware package
# Requires:   samplefirmware  
#Packager:	t-systems@garloff.de
ExclusiveArch:	%{ix86} x86_64

%if 0%{?suse_version} > 0
# openSUSE42.1 could build the PV_OPS drivers for the pv kernel ... skip this for now
%kernel_module_package -n uvpmodsh -p %_sourcedir/preamble -x ec2 -x vmi -x xen -x pv
%else
%kernel_module_package -p %_sourcedir/preamble default
%endif

%description
This is the KMP/KMOD package for Huawei's Xen/UVP hypervisor and contains kernel modules:
* xen-hcall.ko allows time sync from the host
* xen-procfs.ko allows controlled access to host's xenstore for monitoring and state tracking
Both modules help to fill gaps that some versions of pvops Xen drivers leave to make the
uvp-monitor userspace daemon work.
The package also contains optimized versions of the well-known Xen PV drivers 
(xen-balloon, xen-platform-pci, xen-vnif, xen-vbd, xen-scsi) depending on the kernel version.

%prep 
%setup -n SAP-HANA-Tools-%{version}/uvp-xenpv
#%patch0 -p1
%patch1 -p1
#%patch2 -p1
%patch3 -p1
%patch4 -p1
%patch5 -p1

# openSUSE > 42.1: PV_OPS
%if 0%{?suse_version} > 1320
set -- uvp-classic_xen_driver-3.12.xto3.16.x/*
# openSUSE13.x, SLES12
%else 
# SLES12SP2
  %if 0%{?sle_version} == 120200
  set -- uvp-classic_xen_driver-3.12.xto3.16.x/*
  echo -e "export DRDIR=uvp-classic_xen_driver-3.12.xto3.16.x\nexport OSVERSION=4.4.21\nexport OSTYPE=SUSE12SP2" > setos.sh
  %else
  # SLES12SP1
    %if 0%{?sle_version} == 120100
    set -- uvp-classic_xen_driver-3.12.xto3.16.x/*
    echo -e "export DRDIR=uvp-classic_xen_driver-3.12.xto3.16.x\nexport OSVERSION=3.12.49\nexport OSTYPE=SUSE12SP1" > setos.sh
    # SLES11
    %else
      %if 0%{?suse_version} >= 1100
      set -- uvp-classic_xen_driver-2.6.32to3.0.x/*
      # SLES11-SP3,4
      case $(uname -r) in
      	3.0.76*)
      		echo -e "export DRDIR=uvp-classic_xen_driver-2.6.32to3.0.x\nexport OSVERSION=3.0.76\nexport OSTYPE=SUSE11SP3" > setos.sh
      		sed -i 's@#obj-m += vni_front@obj-m += vni_front@' uvp-classic_xen_driver-2.6.32to3.0.x/Makefile
      		;;
      	*)
      		echo -e "export DRDIR=uvp-classic_xen_driver-2.6.32to3.0.x\nexport OSVERSION=3.0.101\nexport OSTYPE=SUSE11SP4" > setos.sh
      		sed -i 's@#obj-m += xen-scsi@obj-m += xen-scsi@' uvp-classic_xen_driver-2.6.32to3.0.x/Makefile
      		;;
      esac
      %else
        %if 0%{?suse_version} >= 900
        set -- others/SLES10/*
        %else
          %if 0%{?centos_version} < 600 && 0%{?centos_version} > 0
          set -- others/RHEL5/*
          %else
            # Non-SUSE: PV_OPS
            #set -- uvp-pvops_xen_driver-2.6.32to4.0.x/*
            %if 0%{?centos_version} >= 700 || 0%{?rhel_version} >= 700
            set -- uvp-classic_xen_driver-3.12.xto3.16.x/*
            echo -e "export DRDIR=uvp-classic_xen_driver-3.12.xto3.16.x\nexport OSVERSION=3.16.6\nexport OSTYPE=CENTOS7" > setos.sh
            %else
            set -- uvp-classic_xen_driver-2.6.32to3.0.x/*
            %endif
          %endif
        %endif
      %endif
    %endif
  %endif
%endif

ls -l
if test -e setos.sh; then 
  source ./setos.sh
  cd $DRDIR
  make lnfile OSVERSION=$OSVERSION OSTYPE=$OSTYPE
  cd ..
fi
mkdir source
mv "$@" source/
mkdir obj

%build
echo Build kernel modules for kernel flavors %flavors_to_build ...
if test -e setos.sh; then 
  source ./setos.sh
  echo "OSTYPE=$OSTYPE; OSVERSION=$OSVERSION"
  MAKEOSFLAGS="OSTYPE=$OSTYPE OSVERSION=$OSVERSION"
fi
export XEN=/usr/src/linux/include/xen
export XL=/usr/src/linux
CFLAGS_MODULE="-DMODULE "
%if 0%{?suse_version} >= 1315 && 0%{?is_opensuse} > 0
  CFLAGS_MODULE+="-DOPENSUSE_1302 "
%else
%if 0%{?suse_version} >= 1103
  CFLAGS_MODULE+="-DSUSE_1103 "
%endif
%endif
for flavor in %flavors_to_build; do
    CFLAGS_MOD="$CFLAGS_MODULE"
    rm -rf obj/$flavor
    if test "$flavor" = "pv"; then
	echo "Can not build xen-classic for PVOPS kernel"
    else
	cp -r source obj/$flavor
    fi
    MOD=$PWD/obj/$flavor
    #cd obj/$flavor && ./mkbuildtree
    SRC=$(grep 'MAKEARGS := -C' %{kernel_source $flavor}/Makefile | sed 's@^.*-C \([^ ]*\).*$@\1@')
    if test "${SRC:0:1}" == "/"; then SRCP=$SRC; else SRCP=%{kernel_source $flavor}/$SRC; fi
    if test -e $SRCP/include/xen/interface/arch-x86/xen-mca.h; then
	CFLAGS_MOD+="-DARCH_X86_XEN_MCA_H=1 "
    fi
    #export CFLAGS_MODULE
    make -C %{kernel_source $flavor} modules %{?_smp_mflags} M=$MOD CFLAGS_MODULE="$CFLAGS_MOD" $MAKEOSFLAGS
    #modinfo `find $MOD -name *.ko`
done

%install
export INSTALL_MOD_PATH=$RPM_BUILD_ROOT
# Following line works for SUSE 11+ and RHEL 6.1+ only, must set INSTALL_MOD_DIR manually for other targets
%if 0%{?suse_version} > 0
export INSTALL_MOD_DIR=%{kernel_module_package_moddir %{name}}
%else
export INSTALL_MOD_DIR=%{kernel_module_package_moddir %{name}}/uvpmodsh
%endif
for flavor in %flavors_to_build; do
    make -C %{kernel_source $flavor} modules_install M=$PWD/obj/$flavor
done
# CentOS-7 needs this ...
%if 0%{?suse_version} == 0
rm -f $RPM_BUILD_ROOT/lib/modules/*/modules.*
%endif



%clean
rm -rf %{buildroot}

%changelog
* Fri Sep  9 2016 kurt@garloff.de
- Mark modules as externally supported.
* Tue Sep  6 2016 kurt@garloff.de
- Update to SAP-HANA-Tools-2.2.0.308:
  * Xen-Classic drivers for SLES11SP3/4 including pvscsi.
* Mon Sep  5 2016 kurt@garloff.de
- Update to 2.2.0.302.
  * Restructuring directory layout
  * Bugfixes ...
* Fri Sep  2 2016 kurt@garloff.de
- Remove modules.* for CentOS7 kmod.
* Sun Mar  6 2016 kurt@garloff.de
- Register misc device /dev/xen/xenbus to use in preference for
  PV_OPS kernels.
* Sat Feb 20 2016 kurt@garloff.de
- Various fixes for RedHat builds, most importantly always pass
  - DMODULE.
- kernel-pv module build.
* Sun Feb 14 2016 kurt@garloff.de
- Build PV_OPS for kernels past openSUSE 42.1/13.2 and exclude
  kernel-pv for now on 42.1.
* Sun Feb 14 2016 kurt@garloff.de
- Leverage patch from JanB and fix build on openSUSE 13.2, 42.1.
* Sun Feb 14 2016 kurt@garloff.de
- Use full source tree from Huawei and use SLES12, SLES11 or
  PV_OPS kernel module sources depending on distro.
* Mon Feb  1 2016 kurt@garloff.de
- Add headers and full set of PV drivers for pre-3.0 kernels.
* Fri Jan 29 2016 kurt@garloff.de
- Support compilation on newer kernels by abstracting
  proc_create() function.
* Fri Jan 29 2016 kurt@garloff.de
- Initial package creation for uvpmod-kmp.
- Works for SLES11SP3/4 currently, more to come.
