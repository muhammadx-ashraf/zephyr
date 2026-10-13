/*
 * Copyright (c) 2021 IP-Logix Inc.
 * Copyright 2022 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_ksz9031

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/mii.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_microchip_ksz9031, CONFIG_PHY_LOG_LEVEL);

#include "phy_mii.h"

struct microchip_ksz9031_dev_config {
	uint8_t phy_addr;
	bool no_reset;
	enum phy_link_speed default_speeds;
	int txc_skew_ps;
	int rxc_skew_ps;
	int txen_skew_ps;
	int rxdv_skew_ps;
	int txd_skew_ps[4];
	int rxd_skew_ps[4];
	const struct device *const mdio;
};

struct microchip_ksz9031_dev_data {
	const struct device *dev;
	phy_callback_t cb;
	void *cb_data;
	struct phy_link_state state;
	struct k_sem sem;
	struct k_work_delayable monitor_work;
	bool gigabit_supported;
	bool autoneg_in_progress;
	k_timepoint_t autoneg_timeout;
};

/* Offset to align capabilities bits of 1000BASE-T Control and Status regs */
#define MICROCHIP_KSZ9031_MII_1KSTSR_OFFSET 2

#define MICROCHIP_KSZ9031_PHY_ID 0x221622

/* How often to poll auto-negotiation status while waiting for it to complete */
#define MICROCHIP_KSZ9031_MII_AUTONEG_POLL_INTERVAL_MS 100

static void microchip_ksz9031_invoke_link_cb(const struct device *dev);

static inline int microchip_ksz9031_reg_read(const struct device *dev, uint16_t reg_addr,
					     uint16_t *value)
{
	const struct microchip_ksz9031_dev_config *const cfg = dev->config;

	if (cfg->mdio == NULL) {
		return -ENODEV;
	}

	return mdio_read(cfg->mdio, cfg->phy_addr, reg_addr, value);
}

static inline int microchip_ksz9031_reg_write(const struct device *dev, uint16_t reg_addr,
					      uint16_t value)
{
	const struct microchip_ksz9031_dev_config *const cfg = dev->config;

	if (cfg->mdio == NULL) {
		return -ENODEV;
	}

	return mdio_write(cfg->mdio, cfg->phy_addr, reg_addr, value);
}

static int microchip_ksz9031_read_gigabit_supported_flag(const struct device *dev, bool *supported)
{
	uint16_t bmsr_reg;
	uint16_t estat_reg;

	if (microchip_ksz9031_reg_read(dev, MII_BMSR, &bmsr_reg) < 0) {
		return -EIO;
	}

	if ((bmsr_reg & MII_BMSR_EXTEND_STATUS) != 0U) {
		if (microchip_ksz9031_reg_read(dev, MII_ESTAT, &estat_reg) < 0) {
			return -EIO;
		}

		if ((estat_reg & (MII_ESTAT_1000BASE_T_HALF | MII_ESTAT_1000BASE_T_FULL)) != 0U) {
			*supported = true;
			return 0;
		}
	}

	*supported = false;
	return 0;
}

static void microchip_ksz9031_phy_mmd_reg_write(const struct device *dev, uint8_t mmd_addr,
						uint8_t mmd_reg, uint16_t reg_val)
{
	microchip_ksz9031_reg_write(dev, MII_MMD_ACR, mmd_addr);
	microchip_ksz9031_reg_write(dev, MII_MMD_AADR, mmd_reg);
	microchip_ksz9031_reg_write(dev, MII_MMD_ACR, 0x4000 | mmd_addr);
	microchip_ksz9031_reg_write(dev, MII_MMD_AADR, reg_val);
}

static uint16_t microchip_ksz9031_ps_to_reg_val(uint16_t ps, int offset, int width)
{
/* PHY ps to reg calculation */
#define MICROCHIP_PS_TO_REG 60

	uint16_t max = BIT_MASK(width);
	uint16_t val = (ps < max) ? ps : (ps / MICROCHIP_PS_TO_REG);

	if (val > max) {
		val = max;
	}

	return (val << offset);
}

static void microchip_ksz9031_phy_setup(const struct device *dev)
{
/* PHY MMD address */
#define DEV_ADDR     2
/* PHY MMD skew registers */
#define CTL_SKEW_REG 4
#define RXD_SKEW_REG 5
#define TXD_SKEW_REG 6
#define CLK_SKEW_REG 8

	const struct microchip_ksz9031_dev_config *const cfg = dev->config;

	uint16_t txen_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->txen_skew_ps, 0x0, 4);
	uint16_t rxdv_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->rxdv_skew_ps, 0x4, 4);
	uint16_t rxd0_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->rxd_skew_ps[0], 0x0, 4);
	uint16_t rxd1_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->rxd_skew_ps[1], 0x4, 4);
	uint16_t rxd2_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->rxd_skew_ps[2], 0x8, 4);
	uint16_t rxd3_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->rxd_skew_ps[3], 0xc, 4);
	uint16_t txd0_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->txd_skew_ps[0], 0x0, 4);
	uint16_t txd1_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->txd_skew_ps[1], 0x4, 4);
	uint16_t txd2_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->txd_skew_ps[2], 0x8, 4);
	uint16_t txd3_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->txd_skew_ps[3], 0xc, 4);
	uint16_t rxc_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->rxc_skew_ps, 0x0, 5);
	uint16_t txc_skew_val = microchip_ksz9031_ps_to_reg_val(cfg->txc_skew_ps, 0x5, 5);

	uint16_t ctl_skew_val = (rxdv_skew_val | txen_skew_val);
	uint16_t rxd_skew_val = (rxd3_skew_val | rxd2_skew_val | rxd1_skew_val | rxd0_skew_val);
	uint16_t txd_skew_val = (txd3_skew_val | txd2_skew_val | txd1_skew_val | txd0_skew_val);
	uint16_t clk_skew_val = (txc_skew_val | rxc_skew_val);

	microchip_ksz9031_phy_mmd_reg_write(dev, DEV_ADDR, CTL_SKEW_REG, ctl_skew_val);
	microchip_ksz9031_phy_mmd_reg_write(dev, DEV_ADDR, RXD_SKEW_REG, rxd_skew_val);
	microchip_ksz9031_phy_mmd_reg_write(dev, DEV_ADDR, TXD_SKEW_REG, txd_skew_val);
	microchip_ksz9031_phy_mmd_reg_write(dev, DEV_ADDR, CLK_SKEW_REG, clk_skew_val);
}

static int microchip_ksz9031_reset(const struct device *dev)
{
	uint32_t timeout = 12U;
	uint16_t value;

	microchip_ksz9031_phy_setup(dev);

	/* Issue a soft reset */
	if (microchip_ksz9031_reg_write(dev, MII_BMCR, MII_BMCR_RESET) < 0) {
		return -EIO;
	}

	/* Wait up to 0.6s for the reset sequence to finish. According to
	 * IEEE 802.3, Section 2, Subsection 22.2.4.1.1 a PHY reset may take
	 * up to 0.5 s.
	 */
	do {
		if (timeout-- == 0U) {
			return -ETIMEDOUT;
		}

		k_sleep(K_MSEC(50));

		if (microchip_ksz9031_reg_read(dev, MII_BMCR, &value) < 0) {
			return -EIO;
		}
	} while ((value & MII_BMCR_RESET) != 0U);

	return 0;
}

static int microchip_ksz9031_get_id(const struct device *dev, uint32_t *phy_id)
{
	uint16_t value;

	if (microchip_ksz9031_reg_read(dev, MII_PHYID1R, &value) < 0) {
		return -EIO;
	}

	*phy_id = value << 16;

	if (microchip_ksz9031_reg_read(dev, MII_PHYID2R, &value) < 0) {
		return -EIO;
	}

	*phy_id |= value;

	return 0;
}

static int microchip_ksz9031_update_link_state(const struct device *dev)
{
	const struct microchip_ksz9031_dev_config *const cfg = dev->config;
	struct microchip_ksz9031_dev_data *const data = dev->data;
	bool link_up;

	uint16_t bmcr_reg = 0;
	uint16_t bmsr_reg = 0;

	if (microchip_ksz9031_reg_read(dev, MII_BMSR, &bmsr_reg) < 0) {
		return -EIO;
	}

	link_up = (bmsr_reg & MII_BMSR_LINK_STATUS) != 0U;

	/* If link is down, we can stop here. */
	if (!link_up) {
		data->state.speed = 0;
		if (link_up != data->state.is_up) {
			data->state.is_up = false;
			LOG_INF("PHY (%d) is down", cfg->phy_addr);
			return 0;
		}
		return -EAGAIN;
	}

	if (microchip_ksz9031_reg_read(dev, MII_BMCR, &bmcr_reg) < 0) {
		return -EIO;
	}

	/* If auto-negotiation is not enabled, we only need to check the link speed */
	if ((bmcr_reg & MII_BMCR_AUTONEG_ENABLE) == 0U) {
		enum phy_link_speed new_speed = phy_mii_get_link_speed_bmcr_reg(dev, bmcr_reg);

		if ((data->state.speed != new_speed) || !data->state.is_up) {
			data->state.is_up = true;
			data->state.speed = new_speed;

			LOG_INF("PHY (%d) Link speed %s Mb, %s duplex", cfg->phy_addr,
				PHY_LINK_IS_SPEED_1000M(data->state.speed)
					? "1000"
					: (PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100"
										     : "10"),
				PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");

			return 0;
		}
		return -EAGAIN;
	}

	/* If auto-negotiation is enabled and the link was already up last time we checked,
	 * we can return immediately, as the link state has not changed.
	 * If the link was down, we will start the auto-negotiation sequence.
	 */
	if (data->state.is_up) {
		return -EAGAIN;
	}

	data->state.is_up = true;

	LOG_DBG("PHY (%d) Starting MII PHY auto-negotiate sequence", cfg->phy_addr);

	data->autoneg_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	return -EINPROGRESS;
}

static int microchip_ksz9031_check_autonegotiation_completion(const struct device *dev)
{
	const struct microchip_ksz9031_dev_config *const cfg = dev->config;
	struct microchip_ksz9031_dev_data *const data = dev->data;

	uint16_t anar_reg = 0;
	uint16_t bmsr_reg = 0;
	uint16_t anlpar_reg = 0;
	uint16_t c1kt_reg = 0;
	uint16_t s1kt_reg = 0;

	/* On some PHY chips, the BMSR bits are latched, so the first read may
	 * show incorrect status. A second read ensures correct values.
	 */
	if (microchip_ksz9031_reg_read(dev, MII_BMSR, &bmsr_reg) < 0) {
		return -EIO;
	}

	/* Second read, clears the latched bits and gives the correct status */
	if (microchip_ksz9031_reg_read(dev, MII_BMSR, &bmsr_reg) < 0) {
		return -EIO;
	}

	if ((bmsr_reg & MII_BMSR_AUTONEG_COMPLETE) == 0U) {
		if (sys_timepoint_expired(data->autoneg_timeout)) {
			LOG_DBG("PHY (%d) auto-negotiate timeout", cfg->phy_addr);
			return -ETIMEDOUT;
		}
		return -EINPROGRESS;
	}

	LOG_DBG("PHY (%d) auto-negotiate sequence completed", cfg->phy_addr);

	/* Read PHY default advertising parameters */
	if (microchip_ksz9031_reg_read(dev, MII_ANAR, &anar_reg) < 0) {
		return -EIO;
	}

	/** Read peer device capability */
	if (microchip_ksz9031_reg_read(dev, MII_ANLPAR, &anlpar_reg) < 0) {
		return -EIO;
	}

	if (data->gigabit_supported) {
		if (microchip_ksz9031_reg_read(dev, MII_1KTCR, &c1kt_reg) < 0) {
			return -EIO;
		}
		if (microchip_ksz9031_reg_read(dev, MII_1KSTSR, &s1kt_reg) < 0) {
			return -EIO;
		}
		s1kt_reg = (uint16_t)(s1kt_reg >> MICROCHIP_KSZ9031_MII_1KSTSR_OFFSET);
	}

	if (data->gigabit_supported && ((c1kt_reg & s1kt_reg & MII_ADVERTISE_1000_FULL) != 0U)) {
		data->state.speed = LINK_FULL_1000BASE;
	} else if (data->gigabit_supported &&
		   ((c1kt_reg & s1kt_reg & MII_ADVERTISE_1000_HALF) != 0U)) {
		data->state.speed = LINK_HALF_1000BASE;
	} else if ((anar_reg & anlpar_reg & MII_ADVERTISE_100_FULL) != 0U) {
		data->state.speed = LINK_FULL_100BASE;
	} else if ((anar_reg & anlpar_reg & MII_ADVERTISE_100_HALF) != 0U) {
		data->state.speed = LINK_HALF_100BASE;
	} else if ((anar_reg & anlpar_reg & MII_ADVERTISE_10_FULL) != 0U) {
		data->state.speed = LINK_FULL_10BASE;
	} else {
		data->state.speed = LINK_HALF_10BASE;
	}

	data->state.is_up = (bmsr_reg & MII_BMSR_LINK_STATUS) != 0U;

	LOG_INF("PHY (%d) Link speed %s Mb, %s duplex", cfg->phy_addr,
		PHY_LINK_IS_SPEED_1000M(data->state.speed)
			? "1000"
			: (PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100" : "10"),
		PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");

	return 0;
}

static void microchip_ksz9031_monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct microchip_ksz9031_dev_data *const data =
		CONTAINER_OF(dwork, struct microchip_ksz9031_dev_data, monitor_work);
	const struct device *dev = data->dev;
	int rc;

	if (k_sem_take(&data->sem, K_NO_WAIT) == 0) {
		if (data->autoneg_in_progress) {
			rc = microchip_ksz9031_check_autonegotiation_completion(dev);
		} else {
			/* If autonegotiation is not in progress, just update the link state */
			rc = microchip_ksz9031_update_link_state(dev);
		}

		data->autoneg_in_progress = (rc == -EINPROGRESS);

		k_sem_give(&data->sem);

		/* If link state has changed and a callback is set, invoke callback */
		if (rc == 0) {
			microchip_ksz9031_invoke_link_cb(dev);
		}
	}

	k_work_reschedule(&data->monitor_work,
			  data->autoneg_in_progress
				  ? K_MSEC(MICROCHIP_KSZ9031_MII_AUTONEG_POLL_INTERVAL_MS)
				  : K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

static int microchip_ksz9031_read(const struct device *dev, uint16_t reg_addr, uint32_t *data)
{
	return microchip_ksz9031_reg_read(dev, reg_addr, (uint16_t *)data);
}

static int microchip_ksz9031_write(const struct device *dev, uint16_t reg_addr, uint32_t data)
{
	return microchip_ksz9031_reg_write(dev, reg_addr, (uint16_t)data);
}

static int microchip_ksz9031_cfg_link(const struct device *dev, enum phy_link_speed adv_speeds,
				      enum phy_cfg_link_flag flags)
{
	struct microchip_ksz9031_dev_data *const data = dev->data;
	const struct microchip_ksz9031_dev_config *const cfg = dev->config;
	int ret = 0;

	if (cfg->mdio == NULL) {
		return -ENODEV;
	}

	k_sem_take(&data->sem, K_FOREVER);

	if ((flags & PHY_FLAG_AUTO_NEGOTIATION_DISABLED) != 0U) {
		/* If auto-negotiation is disabled, only one speed can be selected.
		 * If gigabit is not supported, this speed must not be 1000M.
		 */
		if (!data->gigabit_supported && PHY_LINK_IS_SPEED_1000M(adv_speeds)) {
			LOG_ERR("PHY (%d) Gigabit not supported, can't configure link",
				cfg->phy_addr);
			ret = -ENOTSUP;
			goto cfg_link_end;
		}

		ret = phy_mii_set_bmcr_reg_autoneg_disabled(dev, adv_speeds);
		if (ret >= 0) {
			data->autoneg_in_progress = false;
			k_work_reschedule(&data->monitor_work, K_NO_WAIT);
		}
	} else {
		ret = phy_mii_cfg_link_autoneg(dev, adv_speeds, data->gigabit_supported);
		if (ret >= 0) {
			LOG_DBG("PHY (%d) Starting MII PHY auto-negotiate sequence", cfg->phy_addr);
			data->autoneg_in_progress = true;
			data->autoneg_timeout =
				sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
			k_work_reschedule(&data->monitor_work,
					  K_MSEC(MICROCHIP_KSZ9031_MII_AUTONEG_POLL_INTERVAL_MS));
		}
	}

	if (ret == -EALREADY) {
		LOG_DBG("PHY (%d) Link already configured", cfg->phy_addr);
	}

cfg_link_end:
	k_sem_give(&data->sem);

	return ret;
}

static int microchip_ksz9031_get_link_state(const struct device *dev, struct phy_link_state *state)
{
	struct microchip_ksz9031_dev_data *const data = dev->data;

	k_sem_take(&data->sem, K_FOREVER);

	memcpy(state, &data->state, sizeof(struct phy_link_state));

	if (state->speed == 0) {
		/* If speed is 0, then link is also down, happens when autonegotiation is in
		 * progress
		 */
		state->is_up = false;
	}

	k_sem_give(&data->sem);

	return 0;
}

static void microchip_ksz9031_invoke_link_cb(const struct device *dev)
{
	struct microchip_ksz9031_dev_data *const data = dev->data;
	struct phy_link_state state;

	if (data->cb == NULL) {
		return;
	}

	microchip_ksz9031_get_link_state(dev, &state);

	data->cb(dev, &state, data->cb_data);
}

static int microchip_ksz9031_link_cb_set(const struct device *dev, phy_callback_t cb,
					 void *user_data)
{
	struct microchip_ksz9031_dev_data *const data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	/**
	 * Immediately invoke the callback to notify the caller of the
	 * current link status.
	 */
	microchip_ksz9031_invoke_link_cb(dev);

	return 0;
}

static int microchip_ksz9031_initialize_dynamic_link(const struct device *dev)
{
	const struct microchip_ksz9031_dev_config *const cfg = dev->config;
	struct microchip_ksz9031_dev_data *const data = dev->data;
	uint32_t phy_id;
	int ret = 0;

	data->state.is_up = false;

	mdio_bus_enable(cfg->mdio);

	if (cfg->no_reset == false) {
		ret = microchip_ksz9031_reset(dev);
		if (ret < 0) {
			LOG_ERR("Failed to reset PHY (%d): %d", cfg->phy_addr, ret);
			return ret;
		}
	}

	if (microchip_ksz9031_get_id(dev, &phy_id) == 0) {
		if (phy_id != MICROCHIP_KSZ9031_PHY_ID) {
			LOG_ERR("PHY found at address %d is %x while %x is expected", cfg->phy_addr,
				phy_id, MICROCHIP_KSZ9031_PHY_ID);

			return -EINVAL;
		}

		LOG_INF("PHY (%d) ID %X", cfg->phy_addr, phy_id);
	}

	ret = microchip_ksz9031_read_gigabit_supported_flag(dev, &data->gigabit_supported);
	if (ret < 0) {
		LOG_ERR("Failed to read PHY capabilities: %d", ret);
		return ret;
	}

	k_work_init_delayable(&data->monitor_work, microchip_ksz9031_monitor_work_handler);

	/* Advertise default speeds */
	microchip_ksz9031_cfg_link(dev, cfg->default_speeds, 0);

	/* This will schedule the monitor work, if not already scheduled by
	 * microchip_ksz9031_cfg_link().
	 */
	k_work_schedule(&data->monitor_work, K_NO_WAIT);

	return 0;
}

static DEVICE_API(ethphy, microchip_ksz9031_driver_api) = {
	.get_link = microchip_ksz9031_get_link_state,
	.link_cb_set = microchip_ksz9031_link_cb_set,
	.cfg_link = microchip_ksz9031_cfg_link,
	.read = microchip_ksz9031_read,
	.write = microchip_ksz9031_write,
};

#define MICROCHIP_KSZ9031_CONFIG(n)                                                                \
	BUILD_ASSERT(PHY_INST_GENERATE_DEFAULT_SPEEDS(n) != 0,                                     \
		     "At least one valid speed must be configured for this driver");               \
                                                                                                   \
	static const struct microchip_ksz9031_dev_config microchip_ksz9031_dev_config_##n = {      \
		.phy_addr = DT_INST_REG_ADDR(n),                                                   \
		.no_reset = DT_INST_PROP(n, no_reset),                                             \
		.default_speeds = PHY_INST_GENERATE_DEFAULT_SPEEDS(n),                             \
		.txc_skew_ps = DT_INST_PROP(n, txc_skew_ps),                                       \
		.rxc_skew_ps = DT_INST_PROP(n, rxc_skew_ps),                                       \
		.txen_skew_ps = DT_INST_PROP(n, txen_skew_ps),                                     \
		.rxdv_skew_ps = DT_INST_PROP(n, rxdv_skew_ps),                                     \
		.txd_skew_ps = DT_INST_PROP(n, txd_skew_ps),                                       \
		.rxd_skew_ps = DT_INST_PROP(n, rxd_skew_ps),                                       \
		.mdio = DEVICE_DT_GET(DT_INST_BUS(n))};

#define MICROCHIP_KSZ9031_DATA(n)                                                                  \
	static struct microchip_ksz9031_dev_data microchip_ksz9031_dev_data_##n = {                \
		.dev = DEVICE_DT_INST_GET(n),                                                      \
		.cb = NULL,                                                                        \
		.sem = Z_SEM_INITIALIZER(microchip_ksz9031_dev_data_##n.sem, 1, 1),                \
	};

#define MICROCHIP_KSZ9031_INIT(n) &microchip_ksz9031_initialize_dynamic_link

#define MICROCHIP_KSZ9031_DEVICE(n)                                                                \
	MICROCHIP_KSZ9031_CONFIG(n);                                                               \
	MICROCHIP_KSZ9031_DATA(n);                                                                 \
	DEVICE_DT_INST_DEFINE(n, MICROCHIP_KSZ9031_INIT(n), NULL, &microchip_ksz9031_dev_data_##n, \
			      &microchip_ksz9031_dev_config_##n, POST_KERNEL,                      \
			      CONFIG_PHY_INIT_PRIORITY, &microchip_ksz9031_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MICROCHIP_KSZ9031_DEVICE)
