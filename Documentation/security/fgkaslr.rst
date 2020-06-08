.. SPDX-License-Identifier: GPL-2.0

=====================================================================
Function Granular Kernel Address Space Layout Randomization (fgkaslr)
=====================================================================

:Date: 6 April 2020
:Author: Kristen Accardi

Kernel Address Space Layout Randomization (KASLR) was merged into the kernel
with the objective of increasing the difficulty of code reuse attacks. Code
reuse attacks reused existing code snippets to get around existing memory
protections. They exploit software bugs which expose addresses of useful code
snippets to control the flow of execution for their own nefarious purposes.
KASLR as it was originally implemented moves the entire kernel code text as a
unit at boot time in order to make addresses less predictable. The order of the
code within the segment is unchanged - only the base address is shifted. There
are a few shortcomings to this algorithm.

1. Low Entropy - there are only so many locations the kernel can fit in. This
   means an attacker could guess without too much trouble.
2. Knowledge of a single address can reveal the offset of the base address,
   exposing all other locations for a published/known kernel image.
3. Info leaks abound.

Finer grained ASLR has been proposed as a way to make ASLR more resistant
to info leaks. It is not a new concept at all, and there are many variations
possible. Function reordering is an implementation of finer grained ASLR
which randomizes the layout of an address space on a function level
granularity. The term "fgkaslr" is used in this document to refer to the
technique of function reordering when used with KASLR, as well as finer grained
KASLR in general.

The objective of this patch set is to improve a technology that is already
merged into the kernel (KASLR). This code will not prevent all code reuse
attacks, and should be considered as one of several tools that can be used.

Implementation Details
======================

The over-arching objective of the fgkaslr implementation is incremental
improvement over the existing KASLR algorithm. It is designed to work with
the existing solution, and there are two main area where code changes occur:
Build time, and Load time.

Build time
----------

GCC has had an option to place functions into individual .text sections
for many years now (-ffunction-sections). This option is used to implement
function reordering at load time. The final compiled vmlinux retains all the
section headers, which can be used to help find the address ranges of each
function. Using this information and an expanded table of relocation addresses,
individual text sections can be shuffled immediately after decompression.
Some data tables inside the kernel that have assumptions about order
require sorting after the update. In order to modify these tables,
a few key symbols from the objcopy symbol stripping process are preserved
for use after shuffling the text segments. Any special input sections which are
defined by the kernel build process and collected into the .text output
segment are left unmodified and will still be present inside the .text segment,
unrandomized other than normal base address randomization.

Load time
---------

The boot kernel was modified to parse the vmlinux elf file after
decompression to check for symbols for modifying data tables, and to
look for any .text.* sections to randomize. The sections are then shuffled,
and tables are updated or resorted. The existing code which updated relocation
addresses was modified to account for not just a fixed delta from the load
address, but the offset that the function section was moved to. This requires
inspection of each address to see if it was impacted by a randomization.

In order to hide the new layout, symbols reported through /proc/kallsyms will
be displayed in a random order.

Performance Impact
==================

There are two areas where function reordering can impact performance: boot
time latency, and run time performance.

Boot time latency
-----------------

This implementation of finer grained KASLR impacts the boot time of the kernel
in several places. It requires additional parsing of the kernel ELF file to
obtain the section headers of the sections to be randomized. It calls the
random number generator for each section to be randomized to determine that
section's new memory location. It copies the decompressed kernel into a new
area of memory to avoid corruption when laying out the newly randomized
sections. It increases the number of relocations the kernel has to perform at
boot time vs. standard KASLR, and it also requires a lookup on each address
that needs to be relocated to see if it was in a randomized section and needs
to be adjusted by a new offset. Finally, it re-sorts a few data tables that
are required to be sorted by address.

Booting a test VM on a modern, well appointed system showed an increase in
latency of approximately 1 second.

Run time
--------

The performance impact at run-time of function reordering varies by workload.
Randomly reordering the functions will cause an increase in cache misses
for some workloads. Some workloads perform significantly worse under FGKASLR,
while others stay the same or even improve. In general, it will depend on the
code flow whether or not finer grained KASLR will impact a workload, and how
the underlying code was designed. Because the layout changes per boot, each
time a system is rebooted the performance of a workload may change.

Image Size
==========

fgkaslr increases the size of the kernel binary due to the extra section
headers that are included, as well as the extra relocations that need to
be added. You can expect fgkaslr to increase the size of the resulting
vmlinux by about 3%, and the compressed image (bzImage) by 15%.

Memory Usage
============

fgkaslr increases the amount of heap that is required at boot time,
although this extra memory is released when the kernel has finished
decompression. As a result, it may not be appropriate to use this feature
on systems without much memory.

Building
========

To enable fine grained KASLR, you need to have the following config options
set (including all the ones you would use to build normal KASLR)

``CONFIG_FG_KASLR=y``

fgkaslr for the kernel is only supported for the X86_64 architecture.

Modules
=======

Modules are randomized similarly to the rest of the kernel by shuffling
the sections at load time prior to moving them into memory. The module must
also have been build with the -ffunction-sections compiler option.

Although fgkaslr for the kernel is only supported for the X86_64 architecture,
it is possible to use fgkaslr with modules on other architectures. To enable
this feature, select the following config option:

``CONFIG_MODULE_FG_KASLR``

This option is selected automatically for X86_64 when CONFIG_FG_KASLR is set.

Disabling
=========

Disabling normal kaslr using the nokaslr command line option also disables
fgkaslr. In addition, it is possible to disable fgkaslr separately by booting
with "nofgkaslr" on the commandline.

Further Information
===================

There are a lot of academic papers which explore finer grained ASLR.
This paper in particular contributed significantly to the implementation design.

Selfrando: Securing the Tor Browser against De-anonymization Exploits,
M. Conti, S. Crane, T. Frassetto, et al.

For more information on how function layout impacts performance, see:

Optimizing Function Placement for Large-Scale Data-Center Applications,
G. Ottoni, B. Maher
