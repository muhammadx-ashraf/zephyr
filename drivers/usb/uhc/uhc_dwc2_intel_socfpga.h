/*
 * Copyright (c) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_UHC_DWC2_INTEL_SOCFPGA_H
#define ZEPHYR_DRIVERS_USB_UHC_DWC2_INTEL_SOCFPGA_H

#include <zephyr/kernel.h>

/* Core passes a CPU pointer; HCDMA needs the physical / bus address. */
static inline void intel_socfpga_dma_addr_xlate(const struct device *dev,
					       mem_addr_t *const dma_addr)
{
	ARG_UNUSED(dev);

	*dma_addr = (mem_addr_t)k_mem_phys_addr((void *)(uintptr_t)*dma_addr);
}

#define QUIRK_INTEL_SOCFPGA_DWC2_DEFINE(n)					\
										\
	static const struct uhc_dwc2_vendor_quirks uhc_dwc2_vendor_quirks_##n = {\
		.dma_addr_xlate = intel_socfpga_dma_addr_xlate,			\
	};									\
										\
	static uint8_t uhc_dwc2_quirk_data_##n;					\
	static const uint8_t uhc_dwc2_quirk_config_##n;

DT_INST_FOREACH_STATUS_OKAY(QUIRK_INTEL_SOCFPGA_DWC2_DEFINE)

#endif /* ZEPHYR_DRIVERS_USB_UHC_DWC2_INTEL_SOCFPGA_H */
