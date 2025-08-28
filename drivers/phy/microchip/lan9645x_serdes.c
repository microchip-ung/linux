// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/mfd/ocelot.h>

#include <dt-bindings/phy/phy-lan9645x-serdes.h>
#include "lan9645x_serdes_regs.h"

/* Encode phy type in 4 top bits of index */
#define CUPHY_TYPE 0
#define SERDES_TYPE 1
#define RGMII_TYPE 2

#define IDX_TYPE_MASK 0xf000
#define IDX_VAL_MASK  0x0fff

#define GET_TYPE(x) FIELD_GET(IDX_TYPE_MASK, x)
#define SET_TYPE(x) FIELD_PREP(IDX_TYPE_MASK, x)
#define AS_TYPED(type, idx) (SET_TYPE(type) | ((idx) & IDX_VAL_MASK))
#define FROM_TYPED(x) FIELD_GET(IDX_VAL_MASK, x)

#define PLL_CONF_25MHZ		0
#define PLL_CONF_125MHZ		1
#define PLL_CONF_SERDES_125MHZ	2
#define PLL_CONF_BYPASS		3

/* simple address calculation for replicated registers */
#define ADDR(base, rinst) ((base) + (rinst) * 4)
/* only works if size of SD grp is 32 */
#define SD_ADDR(base, g) ((base) + (g) * 32)

#define SERDES_MUX(_idx, _port, _mode, _submode, _mask, _mux) { \
	.idx = _idx,						\
	.port = _port,						\
	.mode = _mode,						\
	.submode = _submode,					\
	.mask = _mask,						\
	.mux = _mux,						\
}

#define SERDES_MUX_GMII(i, p, m, c) \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_GMII, m, c)
#define SERDES_MUX_SGMII(i, p, m, c) \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_SGMII, m, c)
#define SERDES_MUX_QSGMII(i, p, m, c) \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_QSGMII, m, c)
#define SERDES_MUX_RGMII(i, p, m, c) \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_RGMII, m, c), \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_RGMII_TXID, m, c), \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_RGMII_RXID, m, c), \
	SERDES_MUX(i, p, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_RGMII_ID, m, c)

/* TODO:
 * Maserati has HSIO:HW_CFGSTAT:HW_CFG.RGMII_ENA for configuring GPIO0-12 for
 * RGMII0 and GPIO13-GPIO25 to RGMII1 interface.
 *
 * It seems CHIP_TOP:GPIO_CFG:GPIO_CFG[0-77].RGMII is never used? This reg
 * is otherwise used by pinctrl.
 *
 * Lan9645x does not have the HW_CFG register. It is necessary on lan9645x
 * to configure thes GPIO enable bits on the relevant pins?
 */
struct serdes_ctrl {
	struct device *dev;
	struct regmap *hsio;
	struct phy **cuphys;
	struct phy **serdes;
	struct phy **rgmiis;
	int num_phys;
	const struct serdes_match_data *mdata;
	struct serdes_inversion *inverted;
	int			ref125;
};

struct serdes_ops {
	int (*init_read_strapping)(struct platform_device *pdev,
				   struct serdes_ctrl *ctrl);
};

struct serdes_macro {
	u16 idx;
	int port;
	struct serdes_ctrl *ctrl;
	int speed;
	phy_interface_t mode;
};

struct serdes_mux {
	u32			mask;
	u32			mux;
	enum phy_mode		mode;
	int			submode;
	u16			idx;
	u16			port;
};

struct serdes_match_data {
	const struct serdes_mux *muxes;
	int num_muxes;
	int num_serdes;
	int num_rgmii;
	int num_cuphy;

	/* register addresses relative to HSIO target */
	u32 hw_cfg;
	u32 rgmii_cfg_base;
	u32 dll_cfg_base;
	/* serdes regs. replicated by group */
	u32 sd_cfg_base;
	u32 mpll_cfg_base;
	u32 sd_stat_base;
	const struct serdes_ops *ops;
};

struct serdes_inversion {
	bool tx;
	bool rx;
};

enum lan9645x_sd6g40_mode {
	LAN9645X_SD6G40_MODE_QSGMII,
	LAN9645X_SD6G40_MODE_SGMII,
};

enum lan9645x_sd6g40_ltx2rx {
	LAN9645X_SD6G40_TX2RX_LOOP_NONE,
	LAN9645X_SD6G40_LTX2RX
};

struct lan9645x_sd6g40_setup_args {
	enum lan9645x_sd6g40_mode	mode;
	enum lan9645x_sd6g40_ltx2rx	tx2rx_loop;
	bool				txinvert;
	bool				rxinvert;
	bool				refclk125M;
	bool				mute;
};

struct lan9645x_sd6g40_mode_args {
	enum lan9645x_sd6g40_mode	mode;
	u8				 lane_10bit_sel;
	u8				 mpll_multiplier;
	u8				 ref_clkdiv2;
	u8				 tx_rate;
	u8				 rx_rate;
};

struct lan9645x_sd6g40_setup {
	u8	rx_term_en;
	u8	lane_10bit_sel;
	u8	tx_invert;
	u8	rx_invert;
	u8	mpll_multiplier;
	u8	lane_loopbk_en;
	u8	ref_clkdiv2;
	u8	tx_rate;
	u8	rx_rate;
};

static const struct serdes_mux lan9645x_serdes_mux[] = {
	/* Enable GMII on cuphys */
	SERDES_MUX_GMII(LAN9645X_CU(0), 0, BIT(0 + 1), BIT(0 + 1)),
	SERDES_MUX_GMII(LAN9645X_CU(1), 1, BIT(1 + 1), BIT(1 + 1)),
	SERDES_MUX_GMII(LAN9645X_CU(2), 2, BIT(2 + 1), BIT(2 + 1)),
	SERDES_MUX_GMII(LAN9645X_CU(3), 3, BIT(3 + 1), BIT(3 + 1)),
	SERDES_MUX_GMII(LAN9645X_CU(4), 4, BIT(4 + 1), BIT(4 + 1)),

	/* Serdes 0 with QSGMII mode, on ports 5,6,7,8 */
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 5, BIT(0), BIT(0)),
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 6, BIT(0), BIT(0)),
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 7, BIT(0), BIT(0)),
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 8, BIT(0), BIT(0)),

	/* Serdes 0 with QSGMII mode, on ports 4,5,6,8. Cuphy 4 disabled and rgmii 8 disabled */
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 4, BIT(0), BIT(0)),
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 5, BIT(0), BIT(0)),
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 6, BIT(0), BIT(0)),
	SERDES_MUX_QSGMII(LAN9645X_SERDES6G(0), 8, BIT(0), BIT(0)),

	/* Serdes 0 fixed on port 5, unless QSGMII is enabled. */
	SERDES_MUX_SGMII(LAN9645X_SERDES6G(0), 5, BIT(0), 0x0),
	/* Serdes 1 fixed on port 6. */
	SERDES_MUX_SGMII(LAN9645X_SERDES6G(1), 6, 0, 0),

	/* RGMII 0 on port 4 or 7, and GMII enabled. */
	SERDES_MUX_RGMII(LAN9645X_RGMII(0), 7, BIT(10) | BIT(7 + 1), BIT(7 + 1)),
	SERDES_MUX_RGMII(LAN9645X_RGMII(0), 4, BIT(10) | BIT(4 + 1),
			 BIT(10) | BIT(4 + 1)),

	/* RGMII 1 fixed on port 8 */
	SERDES_MUX_RGMII(LAN9645X_RGMII(1), 8, BIT(8 + 1), BIT(8 + 1)),
};

/* Register CHIP_TOP:STRAPPING:STRAPPING */
#define LAN9645X_STRAPPING_ADDR  0x118
#define LAN9645X_PLL_MASK GENMASK(1, 0)

static int lan9645x_init_read_strapping(struct platform_device *pdev,
					struct serdes_ctrl *ctrl)
{
	const struct regmap_config rmap_cfg = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};
	struct regmap *chip_top;
	u32 strap;
	int err;

	/* Fetch regmap for chip_top target */
	chip_top = ocelot_regmap_from_resource(pdev, 1, &rmap_cfg);
	if (IS_ERR_OR_NULL(chip_top))
		return dev_err_probe(&pdev->dev, PTR_ERR_OR_ZERO(chip_top),
				     "Failed to create regmap\n");

	err = regmap_read(chip_top, LAN9645X_STRAPPING_ADDR, &strap);
	if (err)
		return dev_err_probe(&pdev->dev, err, "Failed to read strapping\n");

	dev_dbg(&pdev->dev, "strapping value 0x%x\n", strap);

	strap = FIELD_GET(LAN9645X_PLL_MASK, strap);
	ctrl->ref125 = (strap == PLL_CONF_125MHZ ||
			strap == PLL_CONF_SERDES_125MHZ);

	return 0;
}

static const struct serdes_ops lan9645x_serdes_ops = {
	.init_read_strapping = lan9645x_init_read_strapping,
};

static int lan9645x_sd6g40_reg_cfg(struct serdes_macro *macro,
				   struct lan9645x_sd6g40_setup *res_struct,
				   u32 idx)
{
	struct serdes_ctrl *ctrl = macro->ctrl;
	const struct serdes_match_data *d = ctrl->mdata;
	struct regmap *map = ctrl->hsio;
	u32 value;

	regmap_update_bits(map, SD_ADDR(d->sd_cfg_base, idx),
			   HSIO_SD_CFG_LANE_10BIT_SEL |
			   HSIO_SD_CFG_RX_RATE |
			   HSIO_SD_CFG_TX_RATE |
			   HSIO_SD_CFG_TX_INVERT |
			   HSIO_SD_CFG_RX_INVERT |
			   HSIO_SD_CFG_LANE_LOOPBK_EN |
			   HSIO_SD_CFG_RX_RESET |
			   HSIO_SD_CFG_TX_RESET,
			   HSIO_SD_CFG_LANE_10BIT_SEL_SET(res_struct->lane_10bit_sel) |
			   HSIO_SD_CFG_RX_RATE_SET(res_struct->rx_rate) |
			   HSIO_SD_CFG_TX_RATE_SET(res_struct->tx_rate) |
			   HSIO_SD_CFG_TX_INVERT_SET(res_struct->tx_invert) |
			   HSIO_SD_CFG_RX_INVERT_SET(res_struct->rx_invert) |
			   HSIO_SD_CFG_LANE_LOOPBK_EN_SET(res_struct->lane_loopbk_en) |
			   HSIO_SD_CFG_RX_RESET_SET(0) |
			   HSIO_SD_CFG_TX_RESET_SET(0));

	regmap_update_bits(map, SD_ADDR(d->mpll_cfg_base, idx),
			   HSIO_MPLL_CFG_MPLL_MULTIPLIER |
			   HSIO_MPLL_CFG_REF_CLKDIV2,
			   HSIO_MPLL_CFG_MPLL_MULTIPLIER_SET(res_struct->mpll_multiplier) |
			   HSIO_MPLL_CFG_REF_CLKDIV2_SET(res_struct->ref_clkdiv2));

	regmap_update_bits(map, SD_ADDR(d->sd_cfg_base, idx),
			   HSIO_SD_CFG_RX_TERM_EN,
			   HSIO_SD_CFG_RX_TERM_EN_SET(res_struct->rx_term_en));

	regmap_update_bits(map, SD_ADDR(d->mpll_cfg_base, idx),
			   HSIO_MPLL_CFG_REF_SSP_EN,
			   HSIO_MPLL_CFG_REF_SSP_EN_SET(1));

	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	regmap_update_bits(map, SD_ADDR(d->sd_cfg_base, idx),
			   HSIO_SD_CFG_PHY_RESET,
			   HSIO_SD_CFG_PHY_RESET_SET(0));

	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	regmap_update_bits(map, SD_ADDR(d->mpll_cfg_base, idx),
			   HSIO_MPLL_CFG_MPLL_EN,
			   HSIO_MPLL_CFG_MPLL_EN_SET(1));

	usleep_range(7 * USEC_PER_MSEC, 8 * USEC_PER_MSEC);

	regmap_read(map, SD_ADDR(d->sd_stat_base, idx), &value);
	value = HSIO_SD_STAT_MPLL_STATE_GET(value);
	if (value != 0x1) {
		dev_err(macro->ctrl->dev,
			"Unexpected sd_sd_stat[%u] mpll_state was 0x1 but is 0x%x\n",
			idx, value);
		return -EIO;
	}

	regmap_update_bits(map, SD_ADDR(d->sd_cfg_base, idx),
			   HSIO_SD_CFG_TX_CM_EN,
			   HSIO_SD_CFG_TX_CM_EN_SET(1));

	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	regmap_read(map, SD_ADDR(d->sd_stat_base, idx), &value);
	value = HSIO_SD_STAT_TX_CM_STATE_GET(value);
	if (value != 0x1) {
		dev_err(macro->ctrl->dev,
			"Unexpected sd_sd_stat[%u] tx_cm_state was 0x1 but is 0x%x\n",
			idx, value);
		return -EIO;
	}

	regmap_update_bits(map, SD_ADDR(d->sd_cfg_base, idx),
			   HSIO_SD_CFG_RX_PLL_EN |
			   HSIO_SD_CFG_TX_EN,
			   HSIO_SD_CFG_RX_PLL_EN_SET(1) |
			   HSIO_SD_CFG_TX_EN_SET(1));

	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	/* Waiting for serdes 0 rx DPLL to lock...  */
	regmap_read(map, SD_ADDR(d->sd_stat_base, idx), &value);
	value = HSIO_SD_STAT_RX_PLL_STATE_GET(value);
	if (value != 0x1) {
		dev_err(macro->ctrl->dev,
			"Unexpected sd_sd_stat[%u] rx_pll_state was 0x1 but is 0x%x\n",
			idx, value);
		return -EIO;
	}

	/* Waiting for serdes 0 tx operational...  */
	regmap_read(map, SD_ADDR(d->sd_stat_base, idx), &value);
	value = HSIO_SD_STAT_TX_STATE_GET(value);
	if (value != 0x1) {
		dev_err(macro->ctrl->dev,
			"Unexpected sd_sd_stat[%u] tx_state was 0x1 but is 0x%x\n",
			idx, value);
		return -EIO;
	}

	regmap_update_bits(map, SD_ADDR(d->sd_cfg_base, idx),
			   HSIO_SD_CFG_TX_DATA_EN |
			   HSIO_SD_CFG_RX_DATA_EN,
			   HSIO_SD_CFG_TX_DATA_EN_SET(1) |
			   HSIO_SD_CFG_RX_DATA_EN_SET(1));

	return 0;
}

static int lan9645x_sd6g40_get_conf_from_mode(struct serdes_macro *macro,
					      enum lan9645x_sd6g40_mode f_mode,
					      bool ref125M,
					      struct lan9645x_sd6g40_mode_args *ret_val)
{
	switch (f_mode) {
	case LAN9645X_SD6G40_MODE_QSGMII:
		ret_val->lane_10bit_sel = 0;
		if (ref125M) {
			ret_val->mpll_multiplier = 40;
			ret_val->ref_clkdiv2 = 0x1;
			ret_val->tx_rate = 0x0;
			ret_val->rx_rate = 0x0;
		} else {
			ret_val->mpll_multiplier = 100;
			ret_val->ref_clkdiv2 = 0x0;
			ret_val->tx_rate = 0x0;
			ret_val->rx_rate = 0x0;
		}
		break;

	case LAN9645X_SD6G40_MODE_SGMII:
		ret_val->lane_10bit_sel = 1;
		if (ref125M) {
			ret_val->mpll_multiplier =
				macro->speed == SPEED_2500 ? 50 : 40;
			ret_val->ref_clkdiv2 = 0x1;
			ret_val->tx_rate = macro->speed == SPEED_2500 ? 0x1 :
									0x2;
			ret_val->rx_rate = macro->speed == SPEED_2500 ? 0x1 :
									0x2;
		} else {
			ret_val->mpll_multiplier =
				macro->speed == SPEED_2500 ? 125 : 100;
			ret_val->ref_clkdiv2 = 0x0;
			ret_val->tx_rate = macro->speed == SPEED_2500 ? 0x1 :
									0x2;
			ret_val->rx_rate = macro->speed == SPEED_2500 ? 0x1 :
									0x2;
		}
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int lan9645x_calc_sd6g40_setup_lane(struct serdes_macro *macro,
					   struct lan9645x_sd6g40_setup_args config,
					   struct lan9645x_sd6g40_setup *ret_val)
{
	struct lan9645x_sd6g40_mode_args sd6g40_mode;
	struct lan9645x_sd6g40_mode_args *mode_args = &sd6g40_mode;
	int ret;

	ret = lan9645x_sd6g40_get_conf_from_mode(macro, config.mode,
						 config.refclk125M, mode_args);
	if (ret)
		return ret;

	ret_val->lane_10bit_sel = mode_args->lane_10bit_sel;
	ret_val->rx_rate = mode_args->rx_rate;
	ret_val->tx_rate = mode_args->tx_rate;
	ret_val->mpll_multiplier = mode_args->mpll_multiplier;
	ret_val->ref_clkdiv2 = mode_args->ref_clkdiv2;
	ret_val->rx_term_en = 0;

	if (config.tx2rx_loop == LAN9645X_SD6G40_LTX2RX)
		ret_val->lane_loopbk_en = 1;
	else
		ret_val->lane_loopbk_en = 0;

	ret_val->tx_invert = !!config.txinvert;
	ret_val->rx_invert = !!config.rxinvert;

	return 0;
}

static int lan9645x_sd6g40_setup_lane(struct serdes_macro *macro,
				      struct lan9645x_sd6g40_setup_args config,
				      u32 idx)
{
	struct lan9645x_sd6g40_setup calc_results = {};
	int ret;

	ret = lan9645x_calc_sd6g40_setup_lane(macro, config, &calc_results);
	if (ret)
		return ret;

	return lan9645x_sd6g40_reg_cfg(macro, &calc_results, idx);
}

static int lan9645x_sd6g40_setup(struct serdes_macro *macro, u32 idx, int mode)
{
	struct lan9645x_sd6g40_setup_args conf = {};

	if (GET_TYPE(idx) != SERDES_TYPE)
		return -EINVAL;

	idx = FROM_TYPED(idx);

	conf.refclk125M = macro->ctrl->ref125;
	conf.txinvert = !!macro->ctrl->inverted[idx].tx;
	conf.rxinvert = !!macro->ctrl->inverted[idx].rx;

	if (mode == PHY_INTERFACE_MODE_QSGMII)
		conf.mode = LAN9645X_SD6G40_MODE_QSGMII;
	else
		conf.mode = LAN9645X_SD6G40_MODE_SGMII;

	return lan9645x_sd6g40_setup_lane(macro, conf, idx);
}

static int lan9645x_rgmii_setup(struct serdes_macro *macro, u32 idx, int mode)
{
	bool tx_delay = false, rx_delay = false;
	struct serdes_ctrl *ctrl = macro->ctrl;
	const struct serdes_match_data *d;
	u32 rx_idx, tx_idx;
	u8 tx_clk;

	if (GET_TYPE(idx) != RGMII_TYPE)
		return -EINVAL;

	d = ctrl->mdata;
	idx = FROM_TYPED(idx);

	tx_clk = macro->speed == SPEED_1000 ? 1 :
		 macro->speed == SPEED_100  ? 2 :
		 macro->speed == SPEED_10   ? 3 :
					      0;

	/* Configure RGMII */
	regmap_update_bits(ctrl->hsio, ADDR(d->rgmii_cfg_base, idx),
			   HSIO_RGMII_CFG_RGMII_RX_RST |
			   HSIO_RGMII_CFG_RGMII_TX_RST |
			   HSIO_RGMII_CFG_TX_CLK_CFG,
			   HSIO_RGMII_CFG_RGMII_RX_RST_SET(0) |
			   HSIO_RGMII_CFG_RGMII_TX_RST_SET(0) |
			   HSIO_RGMII_CFG_TX_CLK_CFG_SET(tx_clk));

	/* We configure delays on the MAC side. When the PHY is not responsible
	 * for delays, the MAC is, which is why RGMII_TXID results in
	 * rx_delay=true
	 *
	 * See: Documentation/networking/phy.rst
	 *
	 * TODO: https://www.kernel.org/doc/Documentation/devicetree/bindings/net/ethernet-controller.yaml
	 *
	 * rx-internal-delay-ps
	 * tx-internal-delay-ps
	 *
	 * See laguna (upstream).
	 */
	if (mode == PHY_INTERFACE_MODE_RGMII ||
	    mode == PHY_INTERFACE_MODE_RGMII_TXID)
		rx_delay = true;

	if (mode == PHY_INTERFACE_MODE_RGMII ||
	    mode == PHY_INTERFACE_MODE_RGMII_RXID)
		tx_delay = true;

	/* Setup DLL configuration. Register layout:
	 * 0:        RGMII_0_RX
	 * 1:        RGMII_0_TX
	 * 2:        RGMII_1_RX
	 * 3:        RGMII_1_TX
	 * ...
	 * (N<<1)    RGMII_N_RX,
	 * (N<<1)+1: RGMII_N_TX,
	 */

	rx_idx = idx << 1;
	tx_idx = rx_idx + 1;

	/* Enable DLL in RGMII clock paths, deassert DLL reset, and start the delay tune FSM. */
	regmap_update_bits(ctrl->hsio,
			   ADDR(d->dll_cfg_base, rx_idx),
			   HSIO_DLL_CFG_DLL_CLK_ENA |
			   HSIO_DLL_CFG_DLL_RST |
			   HSIO_DLL_CFG_DLL_ENA |
			   HSIO_DLL_CFG_DELAY_ENA,
			   HSIO_DLL_CFG_DLL_CLK_ENA_SET(1) |
			   HSIO_DLL_CFG_DLL_RST_SET(0) |
			   HSIO_DLL_CFG_DLL_ENA_SET(rx_delay) |
			   HSIO_DLL_CFG_DELAY_ENA_SET(rx_delay));

	regmap_update_bits(ctrl->hsio,
			   ADDR(d->dll_cfg_base, tx_idx),
			   HSIO_DLL_CFG_DLL_CLK_ENA |
			   HSIO_DLL_CFG_DLL_RST |
			   HSIO_DLL_CFG_DLL_ENA |
			   HSIO_DLL_CFG_DELAY_ENA,
			   HSIO_DLL_CFG_DLL_CLK_ENA_SET(1) |
			   HSIO_DLL_CFG_DLL_RST_SET(0) |
			   HSIO_DLL_CFG_DLL_ENA_SET(tx_delay) |
			   HSIO_DLL_CFG_DELAY_ENA_SET(tx_delay));

	return 0;
}

static bool serdes_mux_equal(const struct serdes_mux *mux,
			     const struct serdes_mux *other)
{
	return mux->idx == other->idx && mux->mode == other->mode &&
	       mux->submode == other->submode && mux->port == other->port;
}

static int serdes_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct serdes_macro *macro = phy_get_drvdata(phy);
	struct serdes_ctrl *ctrl = macro->ctrl;
	const struct serdes_match_data *d = ctrl->mdata;
	struct serdes_mux needle = {
		.idx = macro->idx,
		.port = macro->port,
		.mode = mode,
		.submode = submode,
	};
	const struct serdes_mux *mux;
	unsigned int i;

	if (submode == PHY_INTERFACE_MODE_2500BASEX)
		macro->speed = SPEED_2500;
	else
		macro->speed = SPEED_1000;

	if (submode == PHY_INTERFACE_MODE_1000BASEX ||
	    submode == PHY_INTERFACE_MODE_2500BASEX)
		needle.submode = PHY_INTERFACE_MODE_SGMII;

	if (submode == PHY_INTERFACE_MODE_QUSGMII)
		needle.submode = PHY_INTERFACE_MODE_QSGMII;

	for (i = 0; i < d->num_muxes; i++) {
		mux = &d->muxes[i];

		if (!serdes_mux_equal(mux, &needle))
			continue;

		regmap_update_bits(ctrl->hsio, d->hw_cfg, mux->mask, mux->mux);

		macro->mode = mux->submode;

		switch (GET_TYPE(macro->idx)) {
		case CUPHY_TYPE:
			return 0;
		case RGMII_TYPE:
			return lan9645x_rgmii_setup(macro, macro->idx,
						    macro->mode);
		case SERDES_TYPE:
			return lan9645x_sd6g40_setup(macro, macro->idx,
						     macro->mode);
		default:
			return -EOPNOTSUPP;
		}

		return -EOPNOTSUPP;
	}

	return -EINVAL;
}

static int serdes_set_speed(struct phy *phy, int speed)
{
	struct serdes_macro *macro = phy_get_drvdata(phy);

	if (!phy_interface_mode_is_rgmii(macro->mode))
		return 0;

	macro->speed = speed;
	lan9645x_rgmii_setup(macro, macro->idx, macro->mode);

	return 0;
}

static struct phy *serdes_get_phy(struct serdes_ctrl *ctrl, u16 typed_idx)
{
	u16 idx = FROM_TYPED(typed_idx);

	switch (GET_TYPE(typed_idx)) {
	case SERDES_TYPE:
		return ctrl->serdes[idx];
	case RGMII_TYPE:
		return ctrl->rgmiis[idx];
	case CUPHY_TYPE:
		return ctrl->cuphys[idx];
	default:
		return NULL;
	}
}

static struct phy *serdes_simple_xlate(struct device *dev,
				       const struct of_phandle_args *args)
{
	struct serdes_ctrl *ctrl = dev_get_drvdata(dev);
	struct serdes_macro *macro;
	unsigned int port, idx;
	struct phy *phy;

	if (args->args_count != 2)
		return ERR_PTR(-EINVAL);

	port = args->args[0];
	idx = args->args[1];

	phy = serdes_get_phy(ctrl, idx);
	if (!phy)
		return ERR_PTR(-ENODEV);

	macro = phy_get_drvdata(phy);
	WARN_ON(macro->idx != idx);
	macro->port = port;
	return phy;
}

static const struct phy_ops serdes_ops = {
	.set_mode	= serdes_set_mode,
	.set_speed	= serdes_set_speed,
	.owner		= THIS_MODULE,
};

static int serdes_phy_create(struct serdes_ctrl *ctrl, u16 idx,
			     struct phy **phy)
{
	struct serdes_macro *macro;

	*phy = devm_phy_create(ctrl->dev, NULL, &serdes_ops);
	if (IS_ERR(*phy))
		return PTR_ERR(*phy);

	macro = devm_kzalloc(ctrl->dev, sizeof(*macro), GFP_KERNEL);
	if (!macro)
		return -ENOMEM;

	macro->idx = idx;
	macro->ctrl = ctrl;
	macro->port = -1;

	phy_set_drvdata(*phy, macro);

	return 0;
}

static int serdes_probe(struct platform_device *pdev)
{
	const struct serdes_match_data *d =
		of_device_get_match_data(&pdev->dev);
	const struct regmap_config rmap_cfg = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};
	struct device *dev = &pdev->dev;
	struct serdes_ctrl *ctrl;
	struct regmap *map;
	int err, i;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	map = ocelot_regmap_from_resource(pdev, 0, &rmap_cfg);
	if (IS_ERR_OR_NULL(map))
		return dev_err_probe(dev, PTR_ERR_OR_ZERO(map),
				     "Failed to create regmap\n");

	ctrl->dev = &pdev->dev;
	ctrl->mdata = d;
	ctrl->num_phys = d->num_serdes + d->num_rgmii + d->num_cuphy;
	ctrl->hsio = map;

	ctrl->inverted = devm_kcalloc(ctrl->dev, ctrl->mdata->num_serdes,
				      sizeof(*ctrl->inverted), GFP_KERNEL);
	if (!ctrl->inverted)
		return -ENOMEM;

	ctrl->cuphys = devm_kcalloc(ctrl->dev, d->num_cuphy,
				    sizeof(*ctrl->cuphys), GFP_KERNEL);
	if (!ctrl->cuphys)
		return -ENOMEM;

	ctrl->serdes = devm_kcalloc(ctrl->dev, d->num_serdes,
				    sizeof(*ctrl->serdes), GFP_KERNEL);
	if (!ctrl->serdes)
		return -ENOMEM;

	ctrl->rgmiis = devm_kcalloc(ctrl->dev, d->num_rgmii,
				    sizeof(*ctrl->rgmiis), GFP_KERNEL);
	if (!ctrl->rgmiis)
		return -ENOMEM;

	/* Read strapping to set configured reference clock */
	err = d->ops->init_read_strapping(pdev, ctrl);
	if (err)
		return err;

	for (i = 0; i < d->num_cuphy; i++) {
		err = serdes_phy_create(ctrl, AS_TYPED(CUPHY_TYPE, i),
					&ctrl->cuphys[i]);
		if (err)
			return err;
	}

	for (i = 0; i < d->num_serdes; i++) {
		err = serdes_phy_create(ctrl, AS_TYPED(SERDES_TYPE, i),
					&ctrl->serdes[i]);
		if (err)
			return err;
	}

	for (i = 0; i < d->num_rgmii; i++) {
		err = serdes_phy_create(ctrl, AS_TYPED(RGMII_TYPE, i),
					&ctrl->rgmiis[i]);
		if (err)
			return err;
	}

	for (i = 0; i < d->num_serdes; i++) {
		u8 prop[25];

		sprintf(prop, "microchip,s%d-tx-inverted", i);
		if (device_property_read_bool(ctrl->dev, prop))
			ctrl->inverted[i].tx = true;

		sprintf(prop, "microchip,s%d-rx-inverted", i);
		if (device_property_read_bool(ctrl->dev, prop))
			ctrl->inverted[i].rx = true;
	}

	dev_set_drvdata(&pdev->dev, ctrl);

	dev_info(dev, "Driver registered.\n");

	return PTR_ERR_OR_ZERO(devm_of_phy_provider_register(ctrl->dev,
							     serdes_simple_xlate));
}

static const struct serdes_match_data lan9645x_match_data = {
	.num_serdes = 2,
	.num_cuphy = 5,
	.num_rgmii = 2,
	.muxes = lan9645x_serdes_mux,
	.num_muxes = ARRAY_SIZE(lan9645x_serdes_mux),
	.hw_cfg = 0x48,
	.rgmii_cfg_base = 0x54,
	.dll_cfg_base = 0x64,
	.sd_cfg_base = 0x8,
	.mpll_cfg_base = 0x10,
	.sd_stat_base = 0x14,
	.ops = &lan9645x_serdes_ops,
};

static const struct of_device_id serdes_ids[] = {
	{ .compatible = "microchip,lan9645x-serdes", .data = &lan9645x_match_data },
	{},
};

static struct platform_driver mscc_lan9645x_serdes = {
	.probe = serdes_probe,
	.driver = {
		.name = "microchip,lan9645x-serdes",
		.of_match_table = of_match_ptr(serdes_ids),
	},
};
module_platform_driver(mscc_lan9645x_serdes);

MODULE_DESCRIPTION("Microchip lan9645x switch serdes driver");
MODULE_AUTHOR("Jens Emil Schulz Østergaard <jensemil.schulzostergaard@microchip.com>");
MODULE_LICENSE("GPL v2");
