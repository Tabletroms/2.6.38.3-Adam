/* linux/arch/arm/mach-s5pv210/include/mach/regs-gpio.h
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * S5PV210 - GPIO register definitions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#ifndef __ASM_ARCH_REGS_GPIO_H
#define __ASM_ARCH_REGS_GPIO_H __FILE__

#include <mach/map.h>

/* Base addresses for each of the banks */

#define S5PV210_GPA0_BASE	(S5P_VA_GPIO + 0x000)
#define S5PV210_GPA1_BASE	(S5P_VA_GPIO + 0x020)
#define S5PV210_GPB_BASE	(S5P_VA_GPIO + 0x040)
#define S5PV210_GPC0_BASE	(S5P_VA_GPIO + 0x060)
#define S5PV210_GPC1_BASE	(S5P_VA_GPIO + 0x080)
#define S5PV210_GPD0_BASE	(S5P_VA_GPIO + 0x0A0)
#define S5PV210_GPD1_BASE	(S5P_VA_GPIO + 0x0C0)
#define S5PV210_GPE0_BASE	(S5P_VA_GPIO + 0x0E0)
#define S5PV210_GPE1_BASE	(S5P_VA_GPIO + 0x100)
#define S5PV210_GPF0_BASE	(S5P_VA_GPIO + 0x120)
#define S5PV210_GPF1_BASE	(S5P_VA_GPIO + 0x140)
#define S5PV210_GPF2_BASE	(S5P_VA_GPIO + 0x160)
#define S5PV210_GPF3_BASE	(S5P_VA_GPIO + 0x180)
#define S5PV210_GPG0_BASE	(S5P_VA_GPIO + 0x1A0)
#define S5PV210_GPG1_BASE	(S5P_VA_GPIO + 0x1C0)
#define S5PV210_GPG2_BASE	(S5P_VA_GPIO + 0x1E0)
#define S5PV210_GPG3_BASE	(S5P_VA_GPIO + 0x200)
#define S5PV210_GPH0_BASE	(S5P_VA_GPIO + 0xC00)
#define S5PV210_GPH1_BASE	(S5P_VA_GPIO + 0xC20)
#define S5PV210_GPH2_BASE	(S5P_VA_GPIO + 0xC40)
#define S5PV210_GPH3_BASE	(S5P_VA_GPIO + 0xC60)
#define S5PV210_GPI_BASE	(S5P_VA_GPIO + 0x220)
#define S5PV210_GPJ0_BASE	(S5P_VA_GPIO + 0x240)
#define S5PV210_GPJ1_BASE	(S5P_VA_GPIO + 0x260)
#define S5PV210_GPJ2_BASE	(S5P_VA_GPIO + 0x280)
#define S5PV210_GPJ3_BASE	(S5P_VA_GPIO + 0x2A0)
#define S5PV210_GPJ4_BASE	(S5P_VA_GPIO + 0x2C0)
#define S5PV210_MP01_BASE	(S5P_VA_GPIO + 0x2E0)
#define S5PV210_MP02_BASE	(S5P_VA_GPIO + 0x300)
#define S5PV210_MP03_BASE	(S5P_VA_GPIO + 0x320)
#define S5PV210_MP04_BASE	(S5P_VA_GPIO + 0x340)
#define S5PV210_MP05_BASE	(S5P_VA_GPIO + 0x360)
#define S5PV210_MP06_BASE	(S5P_VA_GPIO + 0x380)
#define S5PV210_MP07_BASE	(S5P_VA_GPIO + 0x3A0)
#define S5PV210_MP10_BASE	(S5P_VA_GPIO + 0x3C0)
#define S5PV210_MP11_BASE	(S5P_VA_GPIO + 0x3E0)
#define S5PV210_MP12_BASE	(S5P_VA_GPIO + 0x400)
#define S5PV210_MP13_BASE	(S5P_VA_GPIO + 0x420)
#define S5PV210_MP14_BASE	(S5P_VA_GPIO + 0x440)
#define S5PV210_MP15_BASE	(S5P_VA_GPIO + 0x460)
#define S5PV210_MP16_BASE	(S5P_VA_GPIO + 0x480)
#define S5PV210_MP17_BASE	(S5P_VA_GPIO + 0x4A0)
#define S5PV210_MP18_BASE	(S5P_VA_GPIO + 0x4C0)
#define S5PV210_MP20_BASE	(S5P_VA_GPIO + 0x4E0)
#define S5PV210_MP21_BASE	(S5P_VA_GPIO + 0x500)
#define S5PV210_MP22_BASE	(S5P_VA_GPIO + 0x520)
#define S5PV210_MP23_BASE	(S5P_VA_GPIO + 0x540)
#define S5PV210_MP24_BASE	(S5P_VA_GPIO + 0x560)
#define S5PV210_MP25_BASE	(S5P_VA_GPIO + 0x580)
#define S5PV210_MP26_BASE	(S5P_VA_GPIO + 0x5A0)
#define S5PV210_MP27_BASE	(S5P_VA_GPIO + 0x5C0)
#define S5PV210_MP28_BASE	(S5P_VA_GPIO + 0x5E0)
#define S5PV210_ETC0_BASE	(S5P_VA_GPIO + 0x600)
#define S5PV210_ETC1_BASE	(S5P_VA_GPIO + 0x620)
#define S5PV210_ETC2_BASE	(S5P_VA_GPIO + 0x640)
#define S5PV210_ETC4_BASE	(S5P_VA_GPIO + 0x660)

#endif /* __ASM_ARCH_REGS_GPIO_H */