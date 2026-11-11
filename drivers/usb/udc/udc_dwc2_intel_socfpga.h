/*
 * Copyright (c) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_UDC_DWC2_INTEL_SOCFPGA_H
#define ZEPHYR_DRIVERS_USB_UDC_DWC2_INTEL_SOCFPGA_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/reset.h>

/*
 * intel,socfpga-dwc2 is a vendor-prefixed snps,dwc2 compatible. The common
 * UDC DWC2 define macro expects dwc2_vendor_quirks_<n> when a second vendor
 * string is present. Buffer DMA is disabled for this SoC family (see
 * Kconfig.dwc2) because KERNEL_VM means DMA descriptors need phys/bus
 * addresses and UDC DWC2 does not yet have a dma_addr_xlate quirk hook.
 *
 * Agilex5 holds USB OTG in reset until software clears the reset-manager
 * bits (TRM); without deassert, CSR reads return 0 (GRSTCTL AHB-idle fail).
 */
#define QUIRK_INTEL_SOCFPGA_DWC2_DEFINE(n)					\
										\
	static int intel_socfpga_udc_pre_enable_##n(const struct device *dev)	\
	{									\
		const struct reset_dt_spec reset_dwc2 =				\
			RESET_DT_SPEC_INST_GET_BY_IDX(n, 0);			\
		const struct reset_dt_spec reset_ecc =				\
			RESET_DT_SPEC_INST_GET_BY_IDX(n, 1);			\
		int ret;							\
										\
		ARG_UNUSED(dev);						\
										\
		ret = reset_line_deassert_dt(&reset_dwc2);			\
		if (ret != 0) {							\
			return ret;						\
		}								\
										\
		ret = reset_line_deassert_dt(&reset_ecc);			\
		if (ret != 0) {							\
			return ret;						\
		}								\
										\
		/* Allow AHB/PHY domain logic to leave reset. */		\
		k_busy_wait(1000);						\
										\
		return 0;							\
	}									\
										\
	static const struct dwc2_vendor_quirks dwc2_vendor_quirks_##n = {	\
		.pre_enable = intel_socfpga_udc_pre_enable_##n,			\
	};

DT_INST_FOREACH_STATUS_OKAY(QUIRK_INTEL_SOCFPGA_DWC2_DEFINE)

#endif /* ZEPHYR_DRIVERS_USB_UDC_DWC2_INTEL_SOCFPGA_H */
