    SAP-HANA-Tools 2.5.0.xxx
The following is the readme file of SAP-HANA-Tools 2.5.0. From the readme file, you will learn what a SAP-HANA-Tools project is, how source code of SAP-HANA-Tools is structured, and how SAP-HANA-Tools is installed.

What Is a SAP-HANA-Tools Project?

SAP-HANA-Tools is a tool that integrates the Xen front-end driver. It is designed for use on virtual machines (VMs) equipped with a 32-bit x86-based CPU (386 or higher) or x86_64 CPU.

The Xen front-end driver improves I/O processing efficiency of VMs. 

Components of the Xen front-end driver:
  - xen-platform-pci driver: Xen front-end pci driver, which provides xenbus access, establishes event channels, and grants references.
  - xen-balloon driver: Xen front-end balloon driver, which adjusts VM memory through the host.
  - xen-vbd/xen-blkfront driver: Xen front-end disk driver.
  - xen-vnif/xen-netfront driver: Xen front-end NIC driver.
  - xen-scsi/xen-scsifront driver: Xen front-end PV SCSI driver.
  - xen-hcall driver: synchronizes VM time with host clock through Xen hypercall.

Structure of SAP-HANA-Tools source code:

SAP-HANA-Tools-2.5.0.xxx

bin/             # Directory that stores tools required for installing, such as the tool used for acquiring Linux distribution information.

build_tools      # Scripts used for building the SAP-HANA-Tools package.

install          # Scripts for installing, uninstalling, and upgrading the SAP-HANA-Tools package.

Makefile         # File that defines rules for building the SAP-HANA-Tools package.

README.md        # Readme file of SAP-HANA-Tools.

uvp-xenpv/       # Source code of the Xen front-end driver.

  uvp-classic_xen_driver-2.6.32to3.0.x/   # Source code of the classic Xen front-end driver, which works well with SLES 11 SP.

  uvp-classic_xen_driver-3.12.xto3.16.x/  # Source code of the classic Xen front-end driver, which works well with SLES 12 SP.

version.ini      # Version information about SAP-HANA-Tools source code.


Installing SAP-HANA-Tools
  - Obtain the SAP-HANA-Tools source code package. Save the SAP-HANA-Tools source code package to a directory on the Linux VM where SAP-HANA-Tools will be installed, and unpack the SAP-HANA-Tools source code package. Be sure that you have the permission to access this directory.
    - If the downloaded SAP-HANA-Tools source code package is an official release, run the following command: 

        tar -xzf SAP-HANA-Tools-2.5.0.xxx.tar.gz

        Or

        unzip SAP-HANA-Tools-2.5.0.xxx.zip

    - If the downloaded SAP-HANA-Tools source code package is a source code package of the master branch, run the following command: 

        unzip SAP-HANA-Tools-master.zip

The Linux VM where SAP-HANA-Tools will be installed must come with gcc, make, libc, and kernel-devel. For simplicity purposes, the Linux VM where SAP-HANA-Tools will be installed is referred to as the Linux VM.

  - Build the Xen front-end driver. Take SLES 11 SP3 x86_64 as an example.
    - Go to the uvp-xenpv/uvp-classic_xen_driver-2.6.32to3.0.x directory.
    - Run the following command to build the Xen front-end driver:

        make KERNDIR="/lib/modules/$(uname -r)/build" BUILDKERNEL=$(uname -r)
  
    After the Xen front-end driver is successfully built, run the following command to install the Xen front-end driver:

        make KERNDIR="/lib/modules/$(uname -r)/build" BUILDKERNEL=$(uname -r) modules_install

        Note: This operation may replace the Xen front-end driver provided by the OS vendor with the one provided by SAP-HANA-Tools. 

    If the Xen front-end driver used by the Linux VM is not provided by SAP-HANA-Tools, uninstall it. Take the SLES 11 SP3 x86_64 as an example. 
    - Run the following command to check whether the Linux VM is armed with the Xen front-end driver provided by the OS vendor. 

        rpm -qa | grep xen-kmp
    - If a command output is returned, the Linux VM is armed with the Xen front-end driver provided by the OS vendor. Then, run the following command to uninstall the Xen front-end driver: 

        rpm -e xen-kmp-default-4.2.2_04_3.0.76_0.11-0.7.5

        Note: "xen-kmp-default-4.2.2_04_3.0.76_0.11-0.7.5" is merely an example. Replace it with the Xen front-end driver installed on the Linux VM. 

  Alternatively, you can use the build_tools script to build a SAP-HANA-Tools installation package for the Linux VM. Take SLES 11 SP3 x86_64 as an example. 
  - After the SAP-HANA-Tools source code package is unpacked, run the following command in the SAP-HANA-Tools-2.5.0.xxx directory to build the SAP-HANA-Tools installation package: 

        make all

  A directory named SAP-HANA-tools-linux-2.5.0.xxx is generated in the current directory after the make all command is run. Go to the SAP-HANA-tools-linux-2.5.0.xxx directory and run the following command to execute the SAP-HANA-Tools installation script install. 
  
        ./install

    Note: If you want to know what the install script has done, run the sh -x install command to print script execution information, or read through the script. 


What can you do if a problem is encountered?
  1. Briefly describe your problem. A one-line summary is recommended.
  2. Describe your problem in detail, including environments, log information, and automatic handling of the problem.
  3. Send the mail to uvptools@huawei.com for further assistance.


