/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Synopsys DWC3 core host-mode programming before xHCI setup.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "xhci_dwc3_log.h"
#include <zephyr/sys/util.h>

#include "dwc3_host.h"
#include "dwc3_xilinx.h"
#include "dwc3_intel_socfpga.h"
#include "dwc3_regs.h"
#include "xhci_hw.h"

LOG_MODULE_DECLARE(uhc_dwc3);

static inline uint32_t dwc3_readl(mm_reg_t base, uint32_t offset)
{
	return sys_read32(base + offset);
}

static inline void dwc3_writel(mm_reg_t base, uint32_t offset, uint32_t val)
{
	sys_write32(val, base + offset);
}

static inline uint32_t xhci_readl(mm_reg_t base, uint32_t offset)
{
	return sys_read32(base + offset);
}

static inline void xhci_writel(mm_reg_t base, uint32_t offset, uint32_t val)
{
	sys_write32(val, base + offset);
}

void dwc3_host_susphy_post_run(const struct device *dev, bool enable)
{
	const struct uhc_dwc3_config *cfg = dev->config;
	mm_reg_t base = uhc_dwc3_core_mmio(dev);
	uint32_t reg;

	reg = dwc3_readl(base, DWC3_GUSB3PIPECTL(0));
	if (enable && !dwc3_xilinx_dwc3_quirk(&cfg->xilinx, DWC3_QUIRK_DIS_U3_SUSPHY)) {
		reg |= DWC3_GUSB3PIPECTL_SUSPHY;
	} else {
		reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
	}
	dwc3_writel(base, DWC3_GUSB3PIPECTL(0), reg);

	reg = dwc3_readl(base, DWC3_GUSB2PHYCFG(0));
	if (enable && !dwc3_xilinx_dwc3_quirk(&cfg->xilinx, DWC3_QUIRK_DIS_U2_SUSPHY)) {
		reg |= DWC3_GUSB2PHYCFG_SUSPHY;
	} else {
		reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	}
	dwc3_writel(base, DWC3_GUSB2PHYCFG(0), reg);

	UHC_DWC3_DBG("post-xHCI-RUN SUSPHY %s", enable ? "enable" : "disable");
}

/*
 * PHY tune after core/handoff.
 *
 * Agilex5 (U-Boot xhci-dwc3_probe + snps,dis_u2_susphy_quirk): clear GUSB2
 * SUSPHY only. Do not clear ENBLSLPM / USBTRDTIM or set XCVRDLY — those
 * diverge from working U-Boot and left SETUP with COMP=4 residual=8 while
 * HS chirp still succeeded.
 */
static void dwc3_host_phy_setup_linux(mm_reg_t base)
{
	uint32_t reg;
	uint32_t hsphy = DWC3_GHWPARAMS3_HSPHY_IFC(dwc3_readl(base, DWC3_GHWPARAMS3));
	uint32_t gusb2_before = dwc3_readl(base, DWC3_GUSB2PHYCFG(0));

#if defined(CONFIG_SOC_AGILEX5)
	reg = gusb2_before;
	reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	dwc3_writel(base, DWC3_GUSB2PHYCFG(0), reg);

	/* Linux dts also has snps,dis_u3_susphy_quirk. */
	reg = dwc3_readl(base, DWC3_GUSB3PIPECTL(0));
	reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
	dwc3_writel(base, DWC3_GUSB3PIPECTL(0), reg);

	LOG_INF("DWC3: Agilex5 PHY (U-Boot-like) HSPHY_IFC=%u GUSB2PHYCFG "
		"0x%08x→0x%08x GUSB3PIPECTL=0x%08x",
		hsphy, gusb2_before, dwc3_readl(base, DWC3_GUSB2PHYCFG(0)),
		dwc3_readl(base, DWC3_GUSB3PIPECTL(0)));
	return;
#endif

	reg = dwc3_readl(base, DWC3_GUSB3PIPECTL(0));
	reg &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;
	reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
	dwc3_writel(base, DWC3_GUSB3PIPECTL(0), reg);

	reg = gusb2_before;
	reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	/* Only program UTMI turnaround when the core is UTMI (not ULPI). */
	if (hsphy == DWC3_GHWPARAMS3_HSPHY_IFC_UTMI) {
		reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK |
			 DWC3_GUSB2PHYCFG_ULPI_UTMI);
		reg |= DWC3_GUSB2PHYCFG_PHYIF(UTMI_PHYIF_8_BIT) |
		       DWC3_GUSB2PHYCFG_USBTRDTIM(USBTRDTIM_UTMI_8_BIT);
	} else if ((hsphy == DWC3_GHWPARAMS3_HSPHY_IFC_ULPI) ||
		   (hsphy == DWC3_GHWPARAMS3_HSPHY_IFC_UTMI_ULPI)) {
		if (hsphy == DWC3_GHWPARAMS3_HSPHY_IFC_UTMI_ULPI) {
			reg |= DWC3_GUSB2PHYCFG_ULPI_UTMI;
		} else {
			reg &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;
		}
	}
	dwc3_writel(base, DWC3_GUSB2PHYCFG(0), reg);

	LOG_INF("DWC3: PHY setup HSPHY_IFC=%u GUSB2PHYCFG=0x%08x GUSB3PIPECTL=0x%08x",
		hsphy, dwc3_readl(base, DWC3_GUSB2PHYCFG(0)),
		dwc3_readl(base, DWC3_GUSB3PIPECTL(0)));
}

int dwc3_host_burst_init(const struct device *dev)
{
	const struct uhc_dwc3_config *cfg = dev->config;
	mm_reg_t base = uhc_dwc3_core_mmio(dev);
	mm_reg_t usb2_wrapper = uhc_dwc3_usb2_wrapper_mmio(dev);
	uint32_t reg;
	uint8_t caplength;
	mm_reg_t op_temp;
	int timeout;
	int pret;

	UHC_DWC3_DBG("DWC3: BURST host init, base 0x%lx", (unsigned long)base);

	pret = dwc3_intel_socfpga_pre_host(dev);
	if (pret != 0) {
		return pret;
	}

	dwc3_xilinx_pre_host_burst(usb2_wrapper);

	caplength = XHCI_CAPLENGTH(xhci_readl(base, XHCI_CAP_CAPBASE));
	op_temp = base + caplength;

	if (cfg->usb2_wrapper_present) {
		/*
		 * Versal/ZynqMP wrapper path: legacy CORESOFTRESET + PHYSOFTRST
		 * + HCRST (kept for platforms that already rely on it).
		 */
		reg = xhci_readl(op_temp, XHCI_OP_USBCMD);
		if ((reg & XHCI_USBCMD_RUN) != 0U) {
			reg &= ~XHCI_USBCMD_RUN;
			xhci_writel(op_temp, XHCI_OP_USBCMD, reg);
			k_busy_wait(1000);
		}

		reg = dwc3_readl(base, DWC3_GCTL);
		reg |= DWC3_GCTL_CORESOFTRESET;
		reg &= ~DWC3_GCTL_PRTCAP_MASK;
		reg |= DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_HOST);
		reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
		reg |= DWC3_GCTL_DSBLCLKGTNG;
		dwc3_writel(base, DWC3_GCTL, reg);

		dwc3_writel(base, DWC3_GUSB3PIPECTL(0),
			    dwc3_readl(base, DWC3_GUSB3PIPECTL(0)) | DWC3_GUSB3PIPECTL_PHYSOFTRST);
		dwc3_writel(base, DWC3_GUSB2PHYCFG(0),
			    dwc3_readl(base, DWC3_GUSB2PHYCFG(0)) | DWC3_GUSB2PHYCFG_PHYSOFTRST);

		k_busy_wait(5000);

		dwc3_writel(base, DWC3_GUSB3PIPECTL(0),
			    dwc3_readl(base, DWC3_GUSB3PIPECTL(0)) & ~DWC3_GUSB3PIPECTL_PHYSOFTRST);
		dwc3_writel(base, DWC3_GUSB2PHYCFG(0),
			    dwc3_readl(base, DWC3_GUSB2PHYCFG(0)) & ~DWC3_GUSB2PHYCFG_PHYSOFTRST);

		k_busy_wait(5000);

		reg = dwc3_readl(base, DWC3_GCTL);
		reg &= ~DWC3_GCTL_CORESOFTRESET;
		dwc3_writel(base, DWC3_GCTL, reg);

		k_busy_wait(5000);

		xhci_writel(op_temp, XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);

		timeout = 1000;
		while ((xhci_readl(op_temp, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) != 0U &&
		       timeout > 0) {
			k_busy_wait(100);
			timeout--;
		}
		if (timeout == 0) {
			LOG_ERR("DWC3: HCRST timed out");
			return -ETIMEDOUT;
		}

		timeout = 5000;
		while ((xhci_readl(op_temp, XHCI_OP_USBSTS) & XHCI_USBSTS_CNR) != 0U &&
		       timeout > 0) {
			k_busy_wait(1000);
			timeout--;
		}
		if (timeout == 0) {
			LOG_ERR("DWC3: CNR stuck after wrapper HCRST (USBSTS=0x%08x)",
				xhci_readl(op_temp, XHCI_OP_USBSTS));
			return -ETIMEDOUT;
		}
	} else {
		/*
		 * Agilex5 U-Boot handoff: keep GUCTL/GFLADJ/PWRDNSCALE that U-Boot
		 * programmed for its refclk (e.g. REFCLKPER=50 → 20 MHz, scale=1250).
		 * Rewriting PWRDNSCALE for OSC1/25 MHz while GUCTL stayed at 20 MHz
		 * produced Setup COMP=4 with otherwise-perfect TRBs and contexts.
		 */
		uint32_t hwparams1 = dwc3_readl(base, DWC3_GHWPARAMS1);
		uint32_t gctl_before = dwc3_readl(base, DWC3_GCTL);
		uint32_t gfladj = dwc3_readl(base, DWC3_GFLADJ);
		uint32_t guctl = dwc3_readl(base, DWC3_GUCTL);
		uint32_t scale_uboot = (gctl_before & DWC3_GCTL_PWRDNSCALE_MASK) >> 19;
		uint32_t refclkper = (guctl & 0xffc00000U) >> 22;
		uint32_t scratch_hz = dwc3_intel_socfpga_suspend_clk_hz();

		LOG_INF("DWC3: Agilex5 pre-softreset GCTL=0x%08x scale=%u GFLADJ=0x%08x "
			"GUCTL=0x%08x REFCLKPER=%u scratch_hz=%u USBSTS=0x%08x",
			gctl_before, scale_uboot, gfladj, guctl, refclkper, scratch_hz,
			xhci_readl(op_temp, XHCI_OP_USBSTS));

		/* U-Boot dwc3_core_soft_reset: 100 ms assert, 100 ms settle. */
		reg = dwc3_readl(base, DWC3_GCTL);
		reg |= DWC3_GCTL_CORESOFTRESET;
		dwc3_writel(base, DWC3_GCTL, reg);

		dwc3_writel(base, DWC3_GUSB3PIPECTL(0),
			    dwc3_readl(base, DWC3_GUSB3PIPECTL(0)) | DWC3_GUSB3PIPECTL_PHYSOFTRST);
		dwc3_writel(base, DWC3_GUSB2PHYCFG(0),
			    dwc3_readl(base, DWC3_GUSB2PHYCFG(0)) | DWC3_GUSB2PHYCFG_PHYSOFTRST);
		k_busy_wait(100000);

		dwc3_writel(base, DWC3_GUSB3PIPECTL(0),
			    dwc3_readl(base, DWC3_GUSB3PIPECTL(0)) & ~DWC3_GUSB3PIPECTL_PHYSOFTRST);
		dwc3_writel(base, DWC3_GUSB2PHYCFG(0),
			    dwc3_readl(base, DWC3_GUSB2PHYCFG(0)) & ~DWC3_GUSB2PHYCFG_PHYSOFTRST);
		k_busy_wait(100000);

		reg = dwc3_readl(base, DWC3_GCTL);
		reg &= ~DWC3_GCTL_CORESOFTRESET;
		dwc3_writel(base, DWC3_GCTL, reg);
		k_busy_wait(100000);

		/* Match U-Boot dwc3_core_init — do not replace PWRDNSCALE. */
		reg = dwc3_readl(base, DWC3_GCTL);
		reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
		reg &= ~DWC3_GCTL_DISSCRAMBLE;
		reg &= ~DWC3_GCTL_PRTCAP_MASK;
		reg |= DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_HOST);
		if (DWC3_GHWPARAMS1_EN_PWROPT(hwparams1) == DWC3_GHWPARAMS1_EN_PWROPT_CLK) {
			reg &= ~DWC3_GCTL_DSBLCLKGTNG;
		}
		dwc3_writel(base, DWC3_GCTL, reg);

		dwc3_host_phy_setup_linux(base);

		LOG_INF("DWC3: Agilex5 host GCTL=0x%08x (scale preserved=%u) GSNPSID=0x%08x "
			"GFLADJ=0x%08x GUCTL=0x%08x USBSTS=0x%08x",
			dwc3_readl(base, DWC3_GCTL),
			(dwc3_readl(base, DWC3_GCTL) & DWC3_GCTL_PWRDNSCALE_MASK) >> 19,
			dwc3_readl(base, DWC3_GSNPSID), dwc3_readl(base, DWC3_GFLADJ),
			dwc3_readl(base, DWC3_GUCTL), xhci_readl(op_temp, XHCI_OP_USBSTS));
		k_busy_wait(10000);
	}

	if (cfg->usb2_wrapper_present) {
		reg = dwc3_readl(base, DWC3_OCTL);
		reg &= ~DWC3_OCTL_PERIMODE;
		dwc3_writel(base, DWC3_OCTL, reg);
	}

	UHC_DWC3_DBG("DWC3: host mode init done (GCTL=0x%08x)", dwc3_readl(base, DWC3_GCTL));

	dwc3_xilinx_host_tune_post(&cfg->xilinx, base, usb2_wrapper);

	{
		struct uhc_dwc3_data *priv = DEV_DATA(dev);

		priv->max_link_speed =
			dwc3_xilinx_resolve_max_speed(base, cfg->xilinx.maximum_speed);
		LOG_INF("DWC3: max link speed %u (GHWPARAMS3=0x%08x)",
			(unsigned int)priv->max_link_speed,
			(unsigned int)dwc3_readl(base, DWC3_GHWPARAMS3));
	}

	return 0;
}
