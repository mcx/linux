#
# gdb helper commands and functions for Linux kernel debugging
#
#  load kernel and module symbols
#
# Copyright (c) Siemens AG, 2011-2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
import os
import re
import struct

from itertools import count
from linux import modules, utils, constants


if hasattr(gdb, 'Breakpoint'):
    class LoadModuleBreakpoint(gdb.Breakpoint):
        def __init__(self, spec, gdb_command):
            super(LoadModuleBreakpoint, self).__init__(spec, internal=True)
            self.silent = True
            self.gdb_command = gdb_command

        def stop(self):
            module = gdb.parse_and_eval("mod")
            module_name = module['name'].string()
            cmd = self.gdb_command

            # enforce update if object file is not found
            cmd.module_files_updated = False

            # Disable pagination while reporting symbol (re-)loading.
            # The console input is blocked in this context so that we would
            # get stuck waiting for the user to acknowledge paged output.
            with utils.pagination_off():
                if module_name in cmd.loaded_modules:
                    gdb.write("refreshing all symbols to reload module "
                              "'{0}'\n".format(module_name))
                    cmd.load_all_symbols()
                else:
                    cmd.load_module_symbols(module)

            return False


def get_vmcore_s390():
    with utils.qemu_phy_mem_mode():
        vmcore_info = 0x0e0c
        paddr_vmcoreinfo_note = gdb.parse_and_eval("*(unsigned long long *)" +
                                                   hex(vmcore_info))
        if paddr_vmcoreinfo_note == 0 or paddr_vmcoreinfo_note & 1:
            # In the early boot case, extract vm_layout.kaslr_offset from the
            # vmlinux image in physical memory.
            if paddr_vmcoreinfo_note == 0:
                kaslr_offset_phys = 0
            else:
                kaslr_offset_phys = paddr_vmcoreinfo_note - 1
            with utils.pagination_off():
                gdb.execute("symbol-file {0} -o {1}".format(
                    utils.get_vmlinux(), hex(kaslr_offset_phys)))
            kaslr_offset = gdb.parse_and_eval("vm_layout.kaslr_offset")
            return "KERNELOFFSET=" + hex(kaslr_offset)[2:]
        inferior = gdb.selected_inferior()
        elf_note = inferior.read_memory(paddr_vmcoreinfo_note, 12)
        n_namesz, n_descsz, n_type = struct.unpack(">III", elf_note)
        desc_paddr = paddr_vmcoreinfo_note + len(elf_note) + n_namesz + 1
        return gdb.parse_and_eval("(char *)" + hex(desc_paddr)).string()


def get_kerneloffset():
    if utils.is_target_arch('s390'):
        try:
            vmcore_str = get_vmcore_s390()
        except gdb.error as e:
            gdb.write("{}\n".format(e))
            return None
        return utils.parse_vmcore(vmcore_str).kerneloffset
    return None


def is_in_s390_decompressor():
    # DAT is always off in decompressor. Use this as an indicator.
    # Note that in the kernel, DAT can be off during kexec() or restart.
    # Accept this imprecision in order to avoid complicating things.
    # It is unlikely that someone will run lx-symbols at these points.
    pswm = int(gdb.parse_and_eval("$pswm"))
    return (pswm & 0x0400000000000000) == 0


def skip_decompressor():
    if utils.is_target_arch("s390"):
        if is_in_s390_decompressor():
            # The address of the jump_to_kernel function is statically placed
            # into svc_old_psw.addr (see ipl_data.c); read it from there. DAT
            # is off, so we do not need to care about lowcore relocation.
            svc_old_pswa = 0x148
            jump_to_kernel = int(gdb.parse_and_eval("*(unsigned long long *)" +
                                                    hex(svc_old_pswa)))
            gdb.execute("tbreak *" + hex(jump_to_kernel))
            gdb.execute("continue")
            while is_in_s390_decompressor():
                gdb.execute("stepi")


class LxSymbols(gdb.Command):
    """(Re-)load symbols of Linux kernel and currently loaded modules.

The kernel (vmlinux) is taken from the current working directly. Modules (.ko)
are scanned recursively, starting in the same directory. Optionally, the module
search path can be extended by a space separated list of paths passed to the
lx-symbols command."""

    module_paths = []
    module_files = []
    module_files_updated = False
    loaded_modules = []
    breakpoint = None

    def __init__(self):
        super(LxSymbols, self).__init__("lx-symbols", gdb.COMMAND_FILES,
                                        gdb.COMPLETE_FILENAME)

    def _update_module_files(self):
        self.module_files = []
        for path in self.module_paths:
            gdb.write("scanning for modules in {0}\n".format(path))
            for root, dirs, files in os.walk(path):
                for name in files:
                    if name.endswith(".ko") or name.endswith(".ko.debug"):
                        self.module_files.append(root + "/" + name)
        self.module_files_updated = True

    def _get_module_file(self, module_name):
        module_pattern = r".*/{0}\.ko(?:.debug)?$".format(
            module_name.replace("_", r"[_\-]"))
        for name in self.module_files:
            if re.match(module_pattern, name) and os.path.exists(name):
                return name
        return None

    def _section_arguments(self, module, module_addr):
        try:
            sect_attrs = module['sect_attrs'].dereference()
        except gdb.error:
            return str(module_addr)

        section_name_to_address = {}
        for i in count():
            # this is a NULL terminated array
            if sect_attrs['grp']['bin_attrs'][i] == 0x0:
                break

            attr = sect_attrs['grp']['bin_attrs'][i].dereference()
            section_name_to_address[attr['attr']['name'].string()] = attr['private']

        textaddr = section_name_to_address.get(".text", module_addr)
        args = []
        for section_name in [".data", ".data..read_mostly", ".rodata", ".bss",
                             ".text.hot", ".text.unlikely"]:
            address = section_name_to_address.get(section_name)
            if address:
                args.append(" -s {name} {addr}".format(
                    name=section_name, addr=str(address)))
        return "{textaddr} {sections}".format(
            textaddr=textaddr, sections="".join(args))

    def load_module_symbols(self, module):
        module_name = module['name'].string()
        module_addr = str(module['mem'][constants.LX_MOD_TEXT]['base']).split()[0]

        module_file = self._get_module_file(module_name)
        if not module_file and not self.module_files_updated:
            self._update_module_files()
            module_file = self._get_module_file(module_name)

        if module_file:
            if utils.is_target_arch('s390'):
                # Module text is preceded by PLT stubs on s390.
                module_arch = module['arch']
                plt_offset = int(module_arch['plt_offset'])
                plt_size = int(module_arch['plt_size'])
                module_addr = hex(int(module_addr, 0) + plt_offset + plt_size)
            gdb.write("loading @{addr}: {filename}\n".format(
                addr=module_addr, filename=module_file))
            cmdline = "add-symbol-file {filename} {sections}".format(
                filename=module_file,
                sections=self._section_arguments(module, module_addr))
            gdb.execute(cmdline, to_string=True)
            if module_name not in self.loaded_modules:
                self.loaded_modules.append(module_name)
        else:
            gdb.write("no module object found for '{0}'\n".format(module_name))

    def load_all_symbols(self):
        gdb.write("loading vmlinux\n")

        # Dropping symbols will disable all breakpoints. So save their states
        # and restore them afterward.
        saved_states = []
        if hasattr(gdb, 'breakpoints') and not gdb.breakpoints() is None:
            for bp in gdb.breakpoints():
                saved_states.append({'breakpoint': bp, 'enabled': bp.enabled})

        # drop all current symbols and reload vmlinux
        orig_vmlinux = utils.get_vmlinux()
        gdb.execute("symbol-file", to_string=True)
        kerneloffset = get_kerneloffset()
        if kerneloffset is None:
            offset_arg = ""
        else:
            offset_arg = " -o " + hex(kerneloffset)
        gdb.execute("symbol-file {0}{1}".format(orig_vmlinux, offset_arg))

        self.loaded_modules = []
        module_list = modules.module_list()
        if not module_list:
            gdb.write("no modules found\n")
        else:
            [self.load_module_symbols(module) for module in module_list]

        for saved_state in saved_states:
            saved_state['breakpoint'].enabled = saved_state['enabled']

    def invoke(self, arg, from_tty):
        skip_decompressor()

        self.module_paths = [os.path.abspath(os.path.expanduser(p))
                             for p in arg.split()]
        self.module_paths.append(os.getcwd())

        # enforce update
        self.module_files = []
        self.module_files_updated = False

        self.load_all_symbols()

        if not modules.has_modules():
            return

        if hasattr(gdb, 'Breakpoint'):
            if self.breakpoint is not None:
                self.breakpoint.delete()
                self.breakpoint = None
            self.breakpoint = LoadModuleBreakpoint(
                "kernel/module/main.c:do_init_module", self)
        else:
            gdb.write("Note: symbol update on module loading not supported "
                      "with this gdb version\n")


LxSymbols()
