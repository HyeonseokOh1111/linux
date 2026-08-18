// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>

#include <asm/setup.h>
#include <asm/bios_ebda.h>
#include <asm/x86_init.h>
#include <asm/cmdline.h>
#include <asm/sev.h>

static bool __init faasev_cmdline_requests_vmpl(const char *cmdline)
{
	char value[2] = {};
	int len;

	if (!cmdline)
		return false;

	len = cmdline_find_option(cmdline, "faasev_vmpl", value, sizeof(value));
	if (len >= 0)
		return len == 0 || value[0] == '1';

	return cmdline_find_option_bool(cmdline, "faasev_vmpl") > 0;
}

static const char *__init faasev_boot_params_cmdline(void)
{
	unsigned long cmdline_paddr = boot_params.hdr.cmd_line_ptr;

	cmdline_paddr |= (u64)boot_params.ext_cmd_line_ptr << 32;
	if (!cmdline_paddr)
		return NULL;

	return __va(cmdline_paddr);
}

static bool __init faasev_vmpl_boot(void)
{
	if (faasev_vmpl_boot_early)
		return true;

	if (faasev_cmdline_requests_vmpl(boot_command_line))
		return true;

	return faasev_cmdline_requests_vmpl(faasev_boot_params_cmdline());
}

void __init x86_early_init_platform_quirks(void)
{
	bool vmpl_boot = faasev_vmpl_boot();

	x86_platform.legacy.i8042 = X86_LEGACY_I8042_EXPECTED_PRESENT;
	x86_platform.legacy.rtc = 1;
	x86_platform.legacy.warm_reset = 1;
	x86_platform.legacy.reserve_bios_regions = 0;
	x86_platform.legacy.devices.pnpbios = 1;

	switch (boot_params.hdr.hardware_subarch) {
	case X86_SUBARCH_PC:
		x86_platform.legacy.reserve_bios_regions = 1;
		break;
	case X86_SUBARCH_XEN:
		x86_platform.legacy.devices.pnpbios = 0;
		x86_platform.legacy.rtc = 0;
		break;
	case X86_SUBARCH_INTEL_MID:
	case X86_SUBARCH_CE4100:
		x86_platform.legacy.devices.pnpbios = 0;
		x86_platform.legacy.rtc = 0;
		x86_platform.legacy.i8042 = X86_LEGACY_I8042_PLATFORM_ABSENT;
		break;
	}

	if (x86_platform.set_legacy_features)
		x86_platform.set_legacy_features();

	/*
	 * A VMPL1 Linux entry does not run with conventional PC firmware state.
	 * The template recorder has already prepared the low-memory boot data,
	 * and probing BIOS/EBDA words through direct-map low-memory aliases can
	 * fault before normal exception handling is available.
	 */
	if (vmpl_boot) {
		x86_init.resources.probe_roms = x86_init_noop;
		x86_init.mpparse.find_smp_config = x86_init_noop;
		x86_init.mpparse.get_smp_config = x86_init_uint_noop;
		x86_platform.legacy.reserve_bios_regions = 0;
	}
}

bool __init x86_pnpbios_disabled(void)
{
	return x86_platform.legacy.devices.pnpbios == 0;
}

#if defined(CONFIG_PNPBIOS)
bool __init arch_pnpbios_disabled(void)
{
	return x86_pnpbios_disabled();
}
#endif
