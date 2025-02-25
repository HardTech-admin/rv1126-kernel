// SPDX-License-Identifier: GPL-2.0
/*
 * imx307 driver
 *
 * Copyright (C) 2020 Rockchip Electronics Co., Ltd.
 * v1.0x01.0x01 support lvds interface,include linear and hdr transmission via vipcap
 * support mipi linear mode
 * v1.0x01.0x02
 * 1.fixed lvds output data offset, because lvds regards ob line as valid data output
 * 2.support test pattern
 * v1.0x01.0x03 update frame rate from 25fps to 30fps
 * v1.0x01.0x04 update max exposure and formula
 *	shs1 = vts - (line + 1)
 * V0.0X01.0X05 add quick stream on/off
 * V0.0X01.0X06 support lvds 2lane
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <linux/of_graph.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <linux/pinctrl/consumer.h>
#include <linux/rk-preisp.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x06)
#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

#define IMX307_LINK_FREQ_111M		111370000
#define IMX307_LINK_FREQ_222M		222750000
#define IMX307_2LANES			2
#define IMX307_4LANES			4
#define IMX307_BITS_PER_SAMPLE		10

/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
#define IMX307_PIXEL_RATE_NORMAL (IMX307_LINK_FREQ_111M * 2 / 10 * IMX307_4LANES)
#define IMX307_PIXEL_RATE_HDR (IMX307_LINK_FREQ_222M * 2 / 10 * IMX307_4LANES)

#define IMX307_XVCLK_FREQ		37125000

#define CHIP_ID				0xb2
#define IMX307_REG_CHIP_ID		0x301e

#define IMX307_REG_CTRL_MODE		0x3000
#define IMX307_MODE_SW_STANDBY		0x1
#define IMX307_MODE_STREAMING		0x0

#define IMX307_REG_SHS1_H		0x3022
#define IMX307_REG_SHS1_M		0x3021
#define IMX307_REG_SHS1_L		0x3020

#define IMX307_REG_SHS2_H		0x3026
#define IMX307_REG_SHS2_M		0x3025
#define IMX307_REG_SHS2_L		0x3024

#define IMX307_REG_RHS1_H		0x3032
#define IMX307_REG_RHS1_M		0x3031
#define IMX307_REG_RHS1_L		0x3030

#define IMX307_FETCH_HIGH_BYTE_EXP(VAL)	(((VAL) >> 16) & 0x0F)
#define IMX307_FETCH_MID_BYTE_EXP(VAL)	(((VAL) >> 8) & 0xFF)
#define IMX307_FETCH_LOW_BYTE_EXP(VAL)	((VAL) & 0xFF)

#define	IMX307_EXPOSURE_MIN		2
#define	IMX307_EXPOSURE_STEP		1
#define IMX307_VTS_MAX			0x7fff

#define IMX307_GAIN_SWITCH_REG		0x3009
#define IMX307_REG_LF_GAIN		0x3014
#define IMX307_REG_SF_GAIN		0x30f2
#define IMX307_GAIN_MIN			0x00
#define IMX307_GAIN_MAX			0xee
#define IMX307_GAIN_STEP		1
#define IMX307_GAIN_DEFAULT		0x00

#define IMX307_GROUP_HOLD_REG		0x3001
#define IMX307_GROUP_HOLD_START		0x01
#define IMX307_GROUP_HOLD_END		0x00

#define USED_TEST_PATTERN
#ifdef USED_TEST_PATTERN
#define IMX307_REG_TEST_PATTERN		0x308c
#define	IMX307_TEST_PATTERN_ENABLE	BIT(0)
#endif

#define IMX307_REG_VTS_H		0x301a
#define IMX307_REG_VTS_M		0x3019
#define IMX307_REG_VTS_L		0x3018
#define IMX307_FETCH_HIGH_BYTE_VTS(VAL)	(((VAL) >> 16) & 0x03)
#define IMX307_FETCH_MID_BYTE_VTS(VAL)	(((VAL) >> 8) & 0xFF)
#define IMX307_FETCH_LOW_BYTE_VTS(VAL)	((VAL) & 0xFF)

#define REG_NULL			0xFFFF
#define REG_DELAY			0xFFFE

#define IMX307_REG_VALUE_08BIT		1
#define IMX307_REG_VALUE_16BIT		2
#define IMX307_REG_VALUE_24BIT		3

static bool g_isHCG;

#define IMX307_NAME			"imx307"

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

#define IMX307_FLIP_REG			0x3007
#define MIRROR_BIT_MASK			BIT(1)
#define FLIP_BIT_MASK			BIT(0)

#define RHS1 0X0B

static const char * const imx307_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define IMX307_NUM_SUPPLIES ARRAY_SIZE(imx307_supply_names)

struct regval {
	u16 addr;
	u8 val;
};

struct imx307_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
	u32 hdr_mode;
	struct rkmodule_lvds_cfg lvds_cfg;
	u32 freq_idx;
	u32 lanes;
	u32 bpp;
};

struct imx307 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[IMX307_NUM_SUPPLIES];

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct v4l2_ctrl	*h_flip;
	struct v4l2_ctrl	*v_flip;
#ifdef USED_TEST_PATTERN
	struct v4l2_ctrl	*test_pattern;
#endif
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct imx307_mode *support_modes;
	u32			support_modes_num;
	const struct imx307_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32			cur_vts;
	bool			has_init_exp;
	struct preisp_hdrae_exp_s init_hdrae_exp;
	struct v4l2_fwnode_endpoint bus_cfg;
	u8			flip;
};

#define to_imx307(sd) container_of(sd, struct imx307, subdev)

/*
 * Xclk 37.125Mhz
 */
static const struct regval imx307_global_regs[] = {
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 30fps
 * lvds_datarate per lane 111Mbps 2 lane
 */
static const struct regval imx307_linear_1920x1080_30fps_lvds_2lane_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x00},
	{0x3009, 0x02},
	{0x300a, 0x3c},
	{0x3010, 0x21},
	{0x3011, 0x0a},
	{0x3018, 0x65},
	{0x3019, 0x04},
	{0x301c, 0x30},
	{0x301d, 0x11},
	{0x3046, 0xD0},
	{0x304b, 0x0a},
	{0x305c, 0x18},
	{0x305d, 0x00},
	{0x305e, 0x20},
	{0x305f, 0x01},
	{0x309e, 0x4a},
	{0x309f, 0x4a},
	{0x311c, 0x0e},
	{0x3128, 0x04},
	{0x3129, 0x1d},
	{0x313b, 0x41},
	{0x315e, 0x1a},
	{0x3164, 0x1a},
	{0x317c, 0x12},
	{0x31ec, 0x37},
	{0x3480, 0x49},
	{0x3002, 0x00},
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 15fps
 * lvds_datarate per lane 222Mbps 2 lane
 */
static const struct regval imx307_hdr2_1920x1080_lvds_2lane_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x00},
	{0x3009, 0x02},
	{0x300a, 0x3c},
	{0x300c, 0x11},
	{0x3010, 0x21},
	{0x3011, 0x0a},
	{0x3014, 0x0f},
	{0x3018, 0x65},/* VMAX L */
	{0x3019, 0x04},/* VMAX M */
	{0x301c, 0x30},/* HMAX L */
	{0x301d, 0x11},/* HMAX H */
	{0x3020, 0x02},//hdr+ shs1 l  short
	{0x3021, 0x00},//hdr+ shs1 m
	{0x3024, 0x49},//hdr+ shs2 l
	{0x3025, 0x04},//hdr+ shs2 m
	{0x3030, RHS1},//hdr+ IMX327_RHS1
	{0x3031, 0x00},//hdr+IMX327_RHS1
	{0x3045, 0x03},//hdr+
	{0x3046, 0xd0},
	{0x305c, 0x18},
	{0x305d, 0x00},
	{0x305e, 0x20},
	{0x305f, 0x01},
	{0x309e, 0x4a},
	{0x309f, 0x4a},
	{0x30d2, 0x19},
	{0x30d7, 0x03},
	{0x3106, 0x10},
	{0x311c, 0x0e},
	{0x3128, 0x04},
	{0x3129, 0x1d},
	{0x313b, 0x41},
	{0x315e, 0x1a},
	{0x3164, 0x1a},
	{0x317c, 0x12},
	{0x31ec, 0x37},
	{0x3480, 0x49},
	{0x31a0, 0xb4},
	{0x31a1, 0x02},
	{0x303c, 0x04},//Y offset
	{0x303d, 0x00},
	{0x303e, 0x41},
	{0x303f, 0x04},//height
	{0x303A, 0x08},//hdr+
	{0x3010, 0x61},//hdr+ gain 1frame FPGC
	{0x3014, 0x00},//hdr+ gain 1frame long
	{0x30F0, 0x64},//hdr+ gain 2frame FPGC
	{0x30f2, 0x00},//hdr+ gain 2frame short
	{0x3002, 0x00},
	{0x304B, 0x0a},
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 30fps
 * lvds_datarate per lane 222.75Mbps 4 lane
 */
static const struct regval imx307_linear_1920x1080_30fps_lvds_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x00},
	{0x3009, 0x02},
	{0x300a, 0x3c},
	{0x3010, 0x21},
	{0x3011, 0x0a},
	{0x3018, 0x65},
	{0x3019, 0x04},
	{0x301c, 0x30},
	{0x301d, 0x11},
	{0x3046, 0xe0},
	{0x304b, 0x0a},
	{0x305c, 0x18},
	{0x305d, 0x00},
	{0x305e, 0x20},
	{0x305f, 0x01},
	{0x309e, 0x4a},
	{0x309f, 0x4a},
	{0x311c, 0x0e},
	{0x3128, 0x04},
	{0x3129, 0x1d},
	{0x313b, 0x41},
	{0x315e, 0x1a},
	{0x3164, 0x1a},
	{0x317c, 0x12},
	{0x31ec, 0x37},
	{0x3480, 0x49},
	{0x3002, 0x00},
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 60fps
 * lvds_datarate per lane 445.5Mbps 4 lane
 */
static const struct regval imx307_linear_1920x1080_60fps_lvds_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x00},
	{0x3009, 0x01},
	{0x300a, 0x3c},
	{0x3010, 0x21},
	{0x3011, 0x0a},
	{0x3018, 0x65},
	{0x3019, 0x04},
	{0x301c, 0x98},
	{0x301d, 0x08},
	{0x3046, 0xe0},
	{0x304b, 0x0a},
	{0x305c, 0x18},
	{0x305d, 0x00},
	{0x305e, 0x20},
	{0x305f, 0x01},
	{0x309e, 0x4a},
	{0x309f, 0x4a},
	{0x311c, 0x0e},
	{0x3128, 0x04},
	{0x3129, 0x1d},
	{0x313b, 0x41},
	{0x315e, 0x1a},
	{0x3164, 0x1a},
	{0x317c, 0x12},
	{0x31ec, 0x37},
	{0x3480, 0x49},
	{0x3002, 0x00},
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 30fps
 * lvds_datarate per lane 445.5Mbps 4 lane
 */
static const struct regval imx307_hdr2_1920x1080_lvds_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x40},
	{0x3009, 0x01},
	{0x300a, 0x3c},
	{0x300c, 0x11},
	{0x3011, 0x02},
	{0x3018, 0xc4},/* VMAX L */
	{0x3019, 0x04},/* VMAX M */
	{0x301c, 0xec},/* HMAX L */
	{0x301d, 0x07},/* HMAX H */
	{0x3020, 0x02},//hdr+ shs1 l  short
	{0x3021, 0x00},//hdr+ shs1 m
	{0x3024, 0xc9},//hdr+ shs2 l
	{0x3025, 0x07},//hdr+ shs2 m
	{0x3030, 0xe1},//hdr+ IMX327_RHS1
	{0x3031, 0x00},//hdr+IMX327_RHS1
	{0x3045, 0x03},//hdr+
	{0x3046, 0xe0},
	{0x304b, 0x0a},
	{0x305c, 0x18},
	{0x305d, 0x03},
	{0x305e, 0x20},
	{0x305f, 0x01},
	{0x309e, 0x4a},
	{0x309f, 0x4a},
	{0x30d2, 0x19},
	{0x30d7, 0x03},
	{0x3106, 0x11},
	{0x3129, 0x1d},
	{0x313b, 0x61},
	{0x315e, 0x1a},
	{0x3164, 0x1a},
	{0x317c, 0x12},
	{0x31ec, 0x37},
	{0x3414, 0x00},
	{0x3415, 0x00},
	{0x3480, 0x49},
	{0x31a0, 0xb4},
	{0x31a1, 0x02},
	{0x303c, 0x04},//Y offset
	{0x303d, 0x00},
	{0x303e, 0x41},
	{0x303f, 0x04},//height
	{0x303A, 0x08},//hdr+
	{0x3010, 0x61},//hdr+ gain 1frame FPGC
	{0x3014, 0x00},//hdr+ gain 1frame long
	{0x30F0, 0x64},//hdr+ gain 2frame FPGC
	{0x30f2, 0x00},//hdr+ gain 2frame short
	{0x3002, 0x00},
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 30fps
 * mipi_datarate per lane 222.75Mbps 4 lane
 */
static const struct regval imx307_linear_1920x1080_mipi_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x00},
	{0x3009, 0x02},
	{0x300A, 0x3c},
	{0x3010, 0x21},
	{0x3011, 0x0a},
	{0x3018, 0x65},
	{0x3019, 0x04},
	{0x301C, 0x30},
	{0x301D, 0x11},
	{0x3046, 0x00},
	{0x304B, 0x0A},
	{0x305C, 0x18},
	{0x305D, 0x03},
	{0x305E, 0x20},
	{0x305F, 0x01},
	{0x309E, 0x4A},
	{0x309F, 0x4A},
	{0x311c, 0x0e},
	{0x3128, 0x04},
	{0x3129, 0x1d},
	{0x313B, 0x41},
	{0x315E, 0x1A},
	{0x3164, 0x1A},
	{0x317C, 0x12},
	{0x31EC, 0x37},
	{0x3405, 0x20},
	{0x3407, 0x03},
	{0x3414, 0x0A},
	{0x3418, 0x49},
	{0x3419, 0x04},
	{0x3441, 0x0a},
	{0x3442, 0x0a},
	{0x3443, 0x03},
	{0x3444, 0x20},
	{0x3445, 0x25},
	{0x3446, 0x47},
	{0x3447, 0x00},
	{0x3448, 0x1f},
	{0x3449, 0x00},
	{0x344A, 0x17},
	{0x344B, 0x00},
	{0x344C, 0x0F},
	{0x344D, 0x00},
	{0x344E, 0x17},
	{0x344F, 0x00},
	{0x3450, 0x47},
	{0x3451, 0x00},
	{0x3452, 0x0F},
	{0x3453, 0x00},
	{0x3454, 0x0f},
	{0x3455, 0x00},
	{0x3472, 0x9c},
	{0x3473, 0x07},
	{0x3480, 0x49},
	{0x3002, 0x00},
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 30fps
 * mipi_datarate per lane 445.5Mbps 4 lane
 */
static const struct regval imx307_hdr2_1920x1080_mipi_regs[] = {
	{0x3003, 0x01},
	{REG_DELAY, 0x10},
	{0x3000, 0x01},
	{0x3001, 0x00},
	{0x3002, 0x01},
	{0x3005, 0x00},
	{0x3007, 0x40},
	{0x3009, 0x01},
	{0x300a, 0x3c},
	{0x300c, 0x11}, //hdr+
	{0x3011, 0x02},
	{0x3018, 0xc4},/* VMAX L */
	{0x3019, 0x04},/* VMAX M */
	{0x301a, 0x00},
	{0x301c, 0xEc},/* HMAX L */
	{0x301d, 0x07},/* HMAX H */
	{0x3045, 0x05},//hdr+
	{0x3046, 0x00},
	{0x304b, 0x0a},
	{0x305c, 0x18},
	{0x305d, 0x03},
	{0x305e, 0x20},
	{0x305f, 0x01},
	{0x309e, 0x4a},
	{0x309f, 0x4a},
	{0x30d2, 0x19},
	{0x30d7, 0x03},
	{0x3106, 0x11},//hdr+
	{0x3129, 0x1d},
	{0x313b, 0x61},
	{0x315e, 0x1a},
	{0x3164, 0x1a},
	{0x317c, 0x12},
	{0x31ec, 0x37},
	{0x3405, 0x10},
	{0x3407, 0x03},
	{0x3414, 0x00},
	{0x3415, 0x00},//hdr+
	{0x3418, 0x72},
	{0x3419, 0x09},
	{0x3441, 0x0a},
	{0x3442, 0x0a},
	{0x3443, 0x03},
	{0x3444, 0x20},
	{0x3445, 0x25},
	{0x3446, 0x57},
	{0x3447, 0x00},
	{0x3448, 0x37},//37?
	{0x3449, 0x00},
	{0x344a, 0x1f},
	{0x344b, 0x00},
	{0x344c, 0x1f},
	{0x344d, 0x00},
	{0x344e, 0x1f},
	{0x344f, 0x00},
	{0x3450, 0x77},
	{0x3451, 0x00},
	{0x3452, 0x1f},
	{0x3453, 0x00},
	{0x3454, 0x17},
	{0x3455, 0x00},
	{0x3472, 0xa0},
	{0x3473, 0x07},
	{0x347b, 0x23},
	{0x3480, 0x49},
	{0x31a0, 0xb4},//hdr+
	{0x31a1, 0x02},//hdr+
	{0x3020, 0x02},//hdr+ shs1 l  short
	{0x3021, 0x00},//hdr+ shs1 m
	{0x3022, 0x00},//hdr+ shs1 h
	{0x3030, 0xe1},//hdr+ IMX307_RHS1
	{0x3031, 0x00},//hdr+IMX307_RHS1
	{0x3032, 0x00},//hdr+
	{0x31A0, 0xe8},//hdr+ HBLANK1
	{0x31A1, 0x01},//hdr+
	{0x303c, 0x04},
	{0x303d, 0x00},
	{0x303e, 0x41},
	{0x303f, 0x04},
	{0x303A, 0x08},//hdr+
	{0x3024, 0xc9},//hdr+ shs2 l
	{0x3025, 0x06},//hdr+ shs2 m
	{0x3026, 0x00},//hdr+ shs2 h
	{0x3010, 0x61},//hdr+ gain 1frame FPGC
	{0x3014, 0x00},//hdr+ gain 1frame long
	{0x30F0, 0x64},//hdr+ gain 2frame FPGC
	{0x30f2, 0x00},//hdr+ gain 2frame short
	{0x3002, 0x00},
	{REG_NULL, 0x00},
};

/*
 * The width and height must be configured to be
 * the same as the current output resolution of the sensor.
 * The input width of the isp needs to be 16 aligned.
 * The input height of the isp needs to be 8 aligned.
 * If the width or height does not meet the alignment rules,
 * you can configure the cropping parameters with the following function to
 * crop out the appropriate resolution.
 * struct v4l2_subdev_pad_ops {
 *	.get_selection
 * }
 */

static const struct imx307_mode lvds_2lane_supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1948,
		.height = 1110,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x03fe,
		.hts_def = 0x1130,
		.vts_def = 0x0465,
		.reg_list = imx307_linear_1920x1080_30fps_lvds_2lane_regs,
		.hdr_mode = NO_HDR,
		.lanes = 2,
		.freq_idx = 0,
		.bpp = 10,
		.lvds_cfg = {
			.mode = LS_FIRST,
			.frm_sync_code[LVDS_CODE_GRP_LINEAR] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x200,
						.eav = 0x274,
					},
					.blk = {
						.sav = 0x2ac,
						.eav = 0x2d8,
					},
				},
			},
		},
	},
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1948,
		.height = 1098,
		.max_fps = {
			.numerator = 10000,
			.denominator = 150000,
		},
		.exp_def = 0x0473,
		.hts_def = 0x07ec,
		.vts_def = 0x04c4 * 2,
		.reg_list = imx307_hdr2_1920x1080_lvds_2lane_regs,
		.hdr_mode = HDR_X2,
		.lanes = 2,
		.freq_idx = 1,
		.bpp = 10,
		.lvds_cfg = {
			.mode  = SONY_DOL_HDR_1,
			.frm_sync_code[LVDS_CODE_GRP_LONG] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x001,
						.eav = 0x075,
					},
					.blk = {
						.sav = 0x0ac,
						.eav = 0x0d8,
					},
				},
				.even_sync_code = {
					.act = {
						.sav = 0x101,
						.eav = 0x175,
					},
					.blk = {
						.sav = 0x1ac,
						.eav = 0x1d8,
					},
				},
			},
			.frm_sync_code[LVDS_CODE_GRP_SHORT] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x002,
						.eav = 0x076,
					},
					.blk = {
						.sav = 0x0ac,
						.eav = 0x0d8,
					},
				},
				.even_sync_code = {
					.act = {
						.sav = 0x102,
						.eav = 0x176,
					},
					.blk = {
						.sav = 0x1ac,
						.eav = 0x1d8,
					},
				},
			},
		},
	},
};

static const struct imx307_mode lvds_supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1948,
		.height = 1110,
		.max_fps = {
			.numerator = 10000,
			.denominator = 600000,
		},
		.exp_def = 0x03fe,
		.hts_def = 0x0889,
		.vts_def = 0x0465,
		.reg_list = imx307_linear_1920x1080_60fps_lvds_regs,
		.hdr_mode = NO_HDR,
		.lanes = 4,
		.freq_idx = 0,
		.bpp = 10,
		.lvds_cfg = {
			.mode = LS_FIRST,
			.frm_sync_code[LVDS_CODE_GRP_LINEAR] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x200,
						.eav = 0x274,
					},
					.blk = {
						.sav = 0x2ac,
						.eav = 0x2d8,
					},
				},
			},
		},
	}, {
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1948,
		.height = 1110,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x03fe,
		.hts_def = 0x1130,
		.vts_def = 0x0465,
		.reg_list = imx307_linear_1920x1080_30fps_lvds_regs,
		.hdr_mode = NO_HDR,
		.lanes = 4,
		.freq_idx = 0,
		.bpp = 10,
		.lvds_cfg = {
			.mode = LS_FIRST,
			.frm_sync_code[LVDS_CODE_GRP_LINEAR] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x200,
						.eav = 0x274,
					},
					.blk = {
						.sav = 0x2ac,
						.eav = 0x2d8,
					},
				},
			},
		},
	},
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1948,
		.height = 1098,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0473,
		.hts_def = 0x07ec,
		.vts_def = 0x04c4 * 2,
		.reg_list = imx307_hdr2_1920x1080_lvds_regs,
		.hdr_mode = HDR_X2,
		.lanes = 4,
		.freq_idx = 1,
		.bpp = 10,
		.lvds_cfg = {
			.mode  = SONY_DOL_HDR_1,
			.frm_sync_code[LVDS_CODE_GRP_LONG] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x001,
						.eav = 0x075,
					},
					.blk = {
						.sav = 0x0ac,
						.eav = 0x0d8,
					},
				},
				.even_sync_code = {
					.act = {
						.sav = 0x101,
						.eav = 0x175,
					},
					.blk = {
						.sav = 0x1ac,
						.eav = 0x1d8,
					},
				},
			},
			.frm_sync_code[LVDS_CODE_GRP_SHORT] = {
				.odd_sync_code = {
					.act = {
						.sav = 0x002,
						.eav = 0x076,
					},
					.blk = {
						.sav = 0x0ac,
						.eav = 0x0d8,
					},
				},
				.even_sync_code = {
					.act = {
						.sav = 0x102,
						.eav = 0x176,
					},
					.blk = {
						.sav = 0x1ac,
						.eav = 0x1d8,
					},
				},
			},
		},
	},
};

static const struct imx307_mode mipi_supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1948,
		.height = 1097,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x03fe,
		.hts_def = 0x1130,
		.vts_def = 0x0465,
		.reg_list = imx307_linear_1920x1080_mipi_regs,
		.hdr_mode = NO_HDR,
		.lanes = 4,
		.freq_idx = 0,
		.bpp = 10,
	}, {
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.width = 1952,
		.height = 1089,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0473,
		.hts_def = 0x07ec,
		.vts_def = 0x04c4 * 2,
		.reg_list = imx307_hdr2_1920x1080_mipi_regs,
		.hdr_mode = HDR_X2,
		.lanes = 4,
		.freq_idx = 1,
		.bpp = 10,
	},
};

static const s64 link_freq_menu_items[] = {
	IMX307_LINK_FREQ_111M,
	IMX307_LINK_FREQ_222M
};

#ifdef USED_TEST_PATTERN
static const char * const imx307_test_pattern_menu[] = {
	"Disabled",
	"Bar Type 1",
	"Bar Type 2",
	"Bar Type 3",
	"Bar Type 4",
	"Bar Type 5",
	"Bar Type 6",
	"Bar Type 7",
	"Bar Type 8",
	"Bar Type 9",
	"Bar Type 10",
	"Bar Type 11",
	"Bar Type 12",
	"Bar Type 13",
	"Bar Type 14",
	"Bar Type 15"
};
#endif

/* Write registers up to 4 at a time */
static int imx307_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

static int imx307_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		if (unlikely(regs[i].addr == REG_DELAY))
			usleep_range(regs[i].val * 1000, regs[i].val * 2000);
		else
			ret = imx307_write_reg(client, regs[i].addr,
				IMX307_REG_VALUE_08BIT,
				regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int imx307_read_reg(struct i2c_client *client, u16 reg,
			   unsigned int len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);
	return 0;
}

static int imx307_get_reso_dist(const struct imx307_mode *mode,
				 struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct imx307_mode *
imx307_find_best_fit(struct imx307 *imx307, struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < imx307->support_modes_num; i++) {
		dist = imx307_get_reso_dist(&imx307->support_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}
	return &imx307->support_modes[cur_best_fit];
}

static int imx307_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx307 *imx307 = to_imx307(sd);
	const struct imx307_mode *mode;
	s64 h_blank, vblank_def;
	s32 dst_link_freq = 0;
	s64 dst_pixel_rate = 0;

	mutex_lock(&imx307->mutex);

	mode = imx307_find_best_fit(imx307, fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx307->mutex);
		return -ENOTTY;
#endif
	} else {
		imx307->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(imx307->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(imx307->vblank, vblank_def,
					 IMX307_VTS_MAX - mode->height,
					 1, vblank_def);
		dst_link_freq = mode->freq_idx;
		dst_pixel_rate = (u32)link_freq_menu_items[mode->freq_idx] / mode->bpp * 2 * mode->lanes;
		__v4l2_ctrl_s_ctrl_int64(imx307->pixel_rate,
					 dst_pixel_rate);
		__v4l2_ctrl_s_ctrl(imx307->link_freq,
				   dst_link_freq);
		imx307->cur_vts = mode->vts_def;
	}

	mutex_unlock(&imx307->mutex);

	return 0;
}

static int imx307_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx307 *imx307 = to_imx307(sd);
	const struct imx307_mode *mode = imx307->cur_mode;

	mutex_lock(&imx307->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&imx307->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&imx307->mutex);
	return 0;
}

static int imx307_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx307 *imx307 = to_imx307(sd);
	const struct imx307_mode *mode = imx307->cur_mode;

	if (code->index != 0)
		return -EINVAL;
	code->code = mode->bus_fmt;

	return 0;
}

static int imx307_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx307 *imx307 = to_imx307(sd);

	if (fse->index >= imx307->support_modes_num)
		return -EINVAL;

	if (fse->code != imx307->support_modes[fse->index].bus_fmt)
		return -EINVAL;

	fse->min_width  = imx307->support_modes[fse->index].width;
	fse->max_width  = imx307->support_modes[fse->index].width;
	fse->max_height = imx307->support_modes[fse->index].height;
	fse->min_height = imx307->support_modes[fse->index].height;

	return 0;
}

#ifdef USED_TEST_PATTERN
static int imx307_enable_test_pattern(struct imx307 *imx307, u32 pattern)
{
	u32 val = 0;

	imx307_read_reg(imx307->client,
			IMX307_REG_TEST_PATTERN,
			IMX307_REG_VALUE_08BIT,
			&val);
	if (pattern) {
		val = ((pattern - 1) << 4) | IMX307_TEST_PATTERN_ENABLE;
		imx307_write_reg(imx307->client,
				 0x300a,
				 IMX307_REG_VALUE_08BIT,
				 0x00);
		imx307_write_reg(imx307->client,
				 0x300e,
				 IMX307_REG_VALUE_08BIT,
				 0x00);
	} else {
		val &= ~IMX307_TEST_PATTERN_ENABLE;
		imx307_write_reg(imx307->client,
				 0x300a,
				 IMX307_REG_VALUE_08BIT,
				 0x3c);
		imx307_write_reg(imx307->client,
				 0x300e,
				 IMX307_REG_VALUE_08BIT,
				 0x01);
	}
	return imx307_write_reg(imx307->client,
				IMX307_REG_TEST_PATTERN,
				IMX307_REG_VALUE_08BIT,
				val);
}
#endif

static int imx307_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx307 *imx307 = to_imx307(sd);
	const struct imx307_mode *mode = imx307->cur_mode;

	mutex_lock(&imx307->mutex);
	fi->interval = mode->max_fps;
	mutex_unlock(&imx307->mutex);

	return 0;
}

static int imx307_set_hdrae(struct imx307 *imx307,
			    struct preisp_hdrae_exp_s *ae)
{
	u32 l_exp_time, m_exp_time, s_exp_time;
	u32 l_gain, m_gain, s_gain;
	u32 shs1, shs2, rhs1;
	u32 gain_switch = 0;
	int ret = 0;
	u8 cg_mode = 0;
	u32 fsc = imx307->cur_vts;//The HDR mode vts is double by default to workaround T-line

	if (!imx307->has_init_exp && !imx307->streaming) {
		imx307->init_hdrae_exp = *ae;
		imx307->has_init_exp = true;
		dev_dbg(&imx307->client->dev, "imx307 don't stream, record exp for hdr!\n");
		return ret;
	}

	l_exp_time = ae->long_exp_reg;
	m_exp_time = ae->middle_exp_reg;
	s_exp_time = ae->short_exp_reg;
	l_gain = ae->long_gain_reg;
	m_gain = ae->middle_gain_reg;
	s_gain = ae->short_gain_reg;

	if (imx307->cur_mode->hdr_mode == HDR_X2) {
		//2 stagger
		l_gain = m_gain;
		l_exp_time = m_exp_time;
		cg_mode = ae->middle_cg_mode;
	}
	dev_dbg(&imx307->client->dev,
		"rev exp req: L_time=%d, gain=%d, S_time=%d, gain=%d\n",
		l_exp_time, l_gain,
		s_exp_time, s_gain);
	ret = imx307_read_reg(imx307->client, IMX307_GAIN_SWITCH_REG,
				IMX307_REG_VALUE_08BIT, &gain_switch);
	if (!g_isHCG && cg_mode == GAIN_MODE_HCG) {
		gain_switch |= 0x0110;
		g_isHCG = true;
	} else if (g_isHCG && cg_mode == GAIN_MODE_LCG) {
		gain_switch &= 0xef;
		gain_switch |= 0x100;
		g_isHCG = false;
	}

	//long exposure and short exposure
	if (imx307->cur_mode->lanes == 2 && imx307->bus_cfg.bus_type == V4L2_MBUS_CCP2)
		rhs1 = RHS1;
	else
		rhs1 = 0xe1;
	shs1 = rhs1 - s_exp_time - 1;
	shs2 = fsc - l_exp_time - 1;
	if (shs1 < 2)
		shs1 = 2;
	if (shs2 < (rhs1 + 2))
		shs2 = rhs1 + 2;
	else if (shs2 > (fsc - 2))
		shs2 = fsc - 2;

	ret |= imx307_write_reg(imx307->client, IMX307_REG_SHS1_L,
		IMX307_REG_VALUE_08BIT,
		IMX307_FETCH_LOW_BYTE_EXP(shs1));
	ret |= imx307_write_reg(imx307->client, IMX307_REG_SHS1_M,
		IMX307_REG_VALUE_08BIT,
		IMX307_FETCH_MID_BYTE_EXP(shs1));
	ret |= imx307_write_reg(imx307->client, IMX307_REG_SHS1_H,
		IMX307_REG_VALUE_08BIT,
		IMX307_FETCH_HIGH_BYTE_EXP(shs1));
	ret |= imx307_write_reg(imx307->client, IMX307_REG_SHS2_L,
		IMX307_REG_VALUE_08BIT,
		IMX307_FETCH_LOW_BYTE_EXP(shs2));
	ret |= imx307_write_reg(imx307->client, IMX307_REG_SHS2_M,
		IMX307_REG_VALUE_08BIT,
		IMX307_FETCH_MID_BYTE_EXP(shs2));
	ret |= imx307_write_reg(imx307->client, IMX307_REG_SHS2_H,
		IMX307_REG_VALUE_08BIT,
		IMX307_FETCH_HIGH_BYTE_EXP(shs2));

	ret |= imx307_write_reg(imx307->client, IMX307_REG_LF_GAIN,
		IMX307_REG_VALUE_08BIT,
		l_gain);
	ret |= imx307_write_reg(imx307->client, IMX307_REG_SF_GAIN,
		IMX307_REG_VALUE_08BIT,
		s_gain);
	if (gain_switch & 0x100) {
		ret |= imx307_write_reg(imx307->client,
				IMX307_GROUP_HOLD_REG,
				IMX307_REG_VALUE_08BIT,
				IMX307_GROUP_HOLD_START);
		ret |= imx307_write_reg(imx307->client, IMX307_GAIN_SWITCH_REG,
				IMX307_REG_VALUE_08BIT, gain_switch);
		ret |= imx307_write_reg(imx307->client,
				IMX307_GROUP_HOLD_REG,
				IMX307_REG_VALUE_08BIT,
				IMX307_GROUP_HOLD_END);
	}
	dev_dbg(&imx307->client->dev,
		"set l_gain:0x%x s_gain:0x%x shs2:0x%x shs1:0x%x\n",
		l_gain, s_gain, shs2, shs1);
	return ret;
}

static void imx307_get_module_inf(struct imx307 *imx307,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, IMX307_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, imx307->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, imx307->len_name, sizeof(inf->base.lens));
}

static int imx307_set_conversion_gain(struct imx307 *imx307, u32 *cg)
{
	int ret = 0;
	struct i2c_client *client = imx307->client;
	int cur_cg = *cg;
	u32 gain_switch = 0;

	ret = imx307_read_reg(client,
		IMX307_GAIN_SWITCH_REG,
		IMX307_REG_VALUE_08BIT,
		&gain_switch);
	if (g_isHCG && cur_cg == GAIN_MODE_LCG) {
		gain_switch &= 0xef;
		gain_switch |= 0x0100;
		g_isHCG = false;
	} else if (!g_isHCG && cur_cg == GAIN_MODE_HCG) {
		gain_switch |= 0x0110;
		g_isHCG = true;
	}

	if (gain_switch & 0x100) {
		ret |= imx307_write_reg(client,
			IMX307_GROUP_HOLD_REG,
			IMX307_REG_VALUE_08BIT,
			IMX307_GROUP_HOLD_START);
		ret |= imx307_write_reg(client,
			IMX307_GAIN_SWITCH_REG,
			IMX307_REG_VALUE_08BIT,
			gain_switch & 0xff);
		ret |= imx307_write_reg(client,
			IMX307_GROUP_HOLD_REG,
			IMX307_REG_VALUE_08BIT,
			IMX307_GROUP_HOLD_END);
	}

	return ret;
}

#define USED_SYS_DEBUG
#ifdef USED_SYS_DEBUG
//ag: echo 0 >  /sys/devices/platform/ff510000.i2c/i2c-1/1-0037/cam_s_cg
static ssize_t set_conversion_gain_status(struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx307 *imx307 = to_imx307(sd);
	int status = 0;
	int ret = 0;

	ret = kstrtoint(buf, 0, &status);
	if (!ret && status >= 0 && status < 2)
		imx307_set_conversion_gain(imx307, &status);
	else
		dev_err(dev, "input 0 for LCG, 1 for HCG, cur %d\n", status);
	return count;
}

static struct device_attribute attributes[] = {
	__ATTR(cam_s_cg, S_IWUSR, NULL, set_conversion_gain_status),
};

static int add_sysfs_interfaces(struct device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(attributes); i++)
		if (device_create_file(dev, attributes + i))
			goto undo;
	return 0;
undo:
	for (i--; i >= 0 ; i--)
		device_remove_file(dev, attributes + i);
	dev_err(dev, "%s: failed to create sysfs interface\n", __func__);
	return -ENODEV;
}
#endif

static long imx307_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx307 *imx307 = to_imx307(sd);
	struct rkmodule_hdr_cfg *hdr;
	struct rkmodule_lvds_cfg *lvds_cfg;
	const struct imx307_mode *mode;
	u32 i, h, w;
	long ret = 0;
	s64 dst_pixel_rate = 0;
	s32 dst_link_freq = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		imx307_get_module_inf(imx307, (struct rkmodule_inf *)arg);
		break;
	case PREISP_CMD_SET_HDRAE_EXP:
		ret = imx307_set_hdrae(imx307, arg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		if (imx307->cur_mode->hdr_mode == NO_HDR)
			hdr->esp.mode = HDR_NORMAL_VC;
		else
			hdr->esp.mode = HDR_ID_CODE;
		hdr->hdr_mode = imx307->cur_mode->hdr_mode;
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		for (i = 0; i < imx307->support_modes_num; i++) {
			if (imx307->support_modes[i].hdr_mode == hdr->hdr_mode) {
				imx307->cur_mode = &imx307->support_modes[i];
				break;
			}
		}
		if (i == imx307->support_modes_num) {
			dev_err(&imx307->client->dev,
				"not find hdr mode:%d config\n",
				hdr->hdr_mode);
			ret = -EINVAL;
		} else {
			mode = imx307->cur_mode;
			w = mode->hts_def - mode->width;
			h = mode->vts_def - mode->height;
			__v4l2_ctrl_modify_range(imx307->hblank, w, w, 1, w);
			__v4l2_ctrl_modify_range(imx307->vblank, h,
				IMX307_VTS_MAX - mode->height,
				1, h);
			dst_link_freq = mode->freq_idx;
			dst_pixel_rate = (u32)link_freq_menu_items[mode->freq_idx] / mode->bpp * 2 * mode->lanes;
			__v4l2_ctrl_s_ctrl_int64(imx307->pixel_rate,
						 dst_pixel_rate);
			__v4l2_ctrl_s_ctrl(imx307->link_freq,
					   dst_link_freq);
			imx307->cur_vts = mode->vts_def;
		}
		break;
	case RKMODULE_SET_CONVERSION_GAIN:
		ret = imx307_set_conversion_gain(imx307, (u32 *)arg);
		break;
	case RKMODULE_GET_LVDS_CFG:
		lvds_cfg = (struct rkmodule_lvds_cfg *)arg;
		if (imx307->bus_cfg.bus_type == V4L2_MBUS_CCP2)
			memcpy(lvds_cfg, &imx307->cur_mode->lvds_cfg,
				sizeof(struct rkmodule_lvds_cfg));
		else
			ret = -ENOIOCTLCMD;
		break;
	case RKMODULE_SET_QUICK_STREAM:

		stream = *((u32 *)arg);

		if (stream)
			ret = imx307_write_reg(imx307->client,
					       IMX307_REG_CTRL_MODE,
					       IMX307_REG_VALUE_08BIT,
					       0);
		else
			ret = imx307_write_reg(imx307->client,
					       IMX307_REG_CTRL_MODE,
					       IMX307_REG_VALUE_08BIT,
					       1);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long imx307_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	struct rkmodule_hdr_cfg *hdr;
	struct preisp_hdrae_exp_s *hdrae;
	long ret;
	u32 cg = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx307_ioctl(sd, cmd, inf);
		if (!ret)
			ret = copy_to_user(up, inf, sizeof(*inf));
		kfree(inf);
		break;
	case RKMODULE_AWB_CFG:
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}
		ret = copy_from_user(cfg, up, sizeof(*cfg));
		if (!ret)
			ret = imx307_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx307_ioctl(sd, cmd, hdr);
		if (!ret)
			ret = copy_to_user(up, hdr, sizeof(*hdr));
		kfree(hdr);
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr) {
			ret = -ENOMEM;
			return ret;
		}

		ret = copy_from_user(hdr, up, sizeof(*hdr));
		if (!ret)
			ret = imx307_ioctl(sd, cmd, hdr);
		kfree(hdr);
		break;
	case PREISP_CMD_SET_HDRAE_EXP:
		hdrae = kzalloc(sizeof(*hdrae), GFP_KERNEL);
		if (!hdrae) {
			ret = -ENOMEM;
			return ret;
		}

		ret = copy_from_user(hdrae, up, sizeof(*hdrae));
		if (!ret)
			ret = imx307_ioctl(sd, cmd, hdrae);
		kfree(hdrae);
		break;
	case RKMODULE_SET_CONVERSION_GAIN:
		ret = copy_from_user(&cg, up, sizeof(cg));
		if (!ret)
			ret = imx307_ioctl(sd, cmd, &cg);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		ret = copy_from_user(&stream, up, sizeof(u32));
		if (!ret)
			ret = imx307_ioctl(sd, cmd, &stream);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int imx307_init_conversion_gain(struct imx307 *imx307)
{
	int ret = 0;
	struct i2c_client *client = imx307->client;
	u32 val = 0;

	ret = imx307_read_reg(client,
		IMX307_GAIN_SWITCH_REG,
		IMX307_REG_VALUE_08BIT,
		&val);
	val &= 0xef;
	ret |= imx307_write_reg(client,
		IMX307_GAIN_SWITCH_REG,
		IMX307_REG_VALUE_08BIT,
		val);
	if (!ret)
		g_isHCG = false;
	return ret;
}

static int __imx307_start_stream(struct imx307 *imx307)
{
	int ret;

	ret = imx307_write_array(imx307->client, imx307->cur_mode->reg_list);
	if (ret)
		return ret;
	ret = imx307_init_conversion_gain(imx307);
	if (ret)
		return ret;
	/* In case these controls are set before streaming */
	ret = __v4l2_ctrl_handler_setup(&imx307->ctrl_handler);
	if (ret)
		return ret;
	if (imx307->has_init_exp && imx307->cur_mode->hdr_mode != NO_HDR) {
		ret = imx307_ioctl(&imx307->subdev, PREISP_CMD_SET_HDRAE_EXP,
			&imx307->init_hdrae_exp);
		if (ret) {
			dev_err(&imx307->client->dev,
				"init exp fail in hdr mode\n");
			return ret;
		}
	}

	ret = imx307_write_reg(imx307->client,
		IMX307_REG_CTRL_MODE,
		IMX307_REG_VALUE_08BIT,
		0);
	return ret;
}

static int __imx307_stop_stream(struct imx307 *imx307)
{
	return imx307_write_reg(imx307->client,
		IMX307_REG_CTRL_MODE,
		IMX307_REG_VALUE_08BIT,
		1);
}

static int imx307_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx307 *imx307 = to_imx307(sd);
	struct i2c_client *client = imx307->client;
	int ret = 0;

	mutex_lock(&imx307->mutex);
	on = !!on;
	if (on == imx307->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __imx307_start_stream(imx307);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__imx307_stop_stream(imx307);
		pm_runtime_put(&client->dev);
	}

	imx307->streaming = on;

unlock_and_return:
	mutex_unlock(&imx307->mutex);

	return ret;
}

static int imx307_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx307 *imx307 = to_imx307(sd);
	struct i2c_client *client = imx307->client;
	int ret = 0;

	mutex_lock(&imx307->mutex);

	/* If the power state is not modified - no work to do. */
	if (imx307->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = imx307_write_array(imx307->client, imx307_global_regs);
		if (ret) {
			v4l2_err(sd, "could not set init registers\n");
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		imx307->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx307->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&imx307->mutex);

	return ret;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 imx307_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, IMX307_XVCLK_FREQ / 1000 / 1000);
}

static int __imx307_power_on(struct imx307 *imx307)
{
	int ret;
	u32 delay_us;
	struct device *dev = &imx307->client->dev;

	if (!IS_ERR_OR_NULL(imx307->pins_default)) {
		ret = pinctrl_select_state(imx307->pinctrl,
					   imx307->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	ret = clk_set_rate(imx307->xvclk, IMX307_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (37.125M Hz)\n");

	if (clk_get_rate(imx307->xvclk) != IMX307_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched,based on 24M Hz\n");

	ret = clk_prepare_enable(imx307->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}


	ret = regulator_bulk_enable(IMX307_NUM_SUPPLIES, imx307->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(imx307->reset_gpio))
		gpiod_set_value_cansleep(imx307->reset_gpio, 0);
	usleep_range(500, 1000);
	if (!IS_ERR(imx307->reset_gpio))
		gpiod_set_value_cansleep(imx307->reset_gpio, 1);

	if (!IS_ERR(imx307->pwdn_gpio))
		gpiod_set_value_cansleep(imx307->pwdn_gpio, 1);

	/* 8192 cycles prior to first SCCB transaction */
	delay_us = imx307_cal_delay(8192);
	usleep_range(delay_us, delay_us * 2);
	usleep_range(5000, 10000);
	return 0;

disable_clk:
	clk_disable_unprepare(imx307->xvclk);

	return ret;
}

static void __imx307_power_off(struct imx307 *imx307)
{
	int ret;
	struct device *dev = &imx307->client->dev;

	if (!IS_ERR(imx307->pwdn_gpio))
		gpiod_set_value_cansleep(imx307->pwdn_gpio, 0);
	clk_disable_unprepare(imx307->xvclk);
	if (!IS_ERR(imx307->reset_gpio))
		gpiod_set_value_cansleep(imx307->reset_gpio, 0);
	if (!IS_ERR_OR_NULL(imx307->pins_sleep)) {
		ret = pinctrl_select_state(imx307->pinctrl,
					   imx307->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	regulator_bulk_disable(IMX307_NUM_SUPPLIES, imx307->supplies);
}

static int imx307_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx307 *imx307 = to_imx307(sd);

	return __imx307_power_on(imx307);
}

static int imx307_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx307 *imx307 = to_imx307(sd);

	__imx307_power_off(imx307);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx307_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx307 *imx307 = to_imx307(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct imx307_mode *def_mode = &imx307->support_modes[0];

	mutex_lock(&imx307->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx307->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int imx307_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_pad_config *cfg,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct imx307 *imx307 = to_imx307(sd);

	if (fie->index >= imx307->support_modes_num)
		return -EINVAL;

	fie->code = imx307->support_modes[fie->index].bus_fmt;
	fie->width = imx307->support_modes[fie->index].width;
	fie->height = imx307->support_modes[fie->index].height;
	fie->interval = imx307->support_modes[fie->index].max_fps;
	fie->reserved[0] = imx307->support_modes[fie->index].hdr_mode;
	return 0;
}

#define CROP_START(SRC, DST) (((SRC) - (DST)) / 2 / 4 * 4)
#define DST_WIDTH 1920
#define DST_HEIGHT 1080

/*
 * The resolution of the driver configuration needs to be exactly
 * the same as the current output resolution of the sensor,
 * the input width of the isp needs to be 16 aligned,
 * the input height of the isp needs to be 8 aligned.
 * Can be cropped to standard resolution by this function,
 * otherwise it will crop out strange resolution according
 * to the alignment rules.
 */

static int imx307_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	struct imx307 *imx307 = to_imx307(sd);

	if (sel->target == V4L2_SEL_TGT_CROP_BOUNDS) {
		sel->r.left = CROP_START(imx307->cur_mode->width, DST_WIDTH);
		sel->r.width = DST_WIDTH;
		if (imx307->bus_cfg.bus_type == V4L2_MBUS_CCP2) {
			if (imx307->cur_mode->hdr_mode == NO_HDR)
				sel->r.top = 21;
			else
				sel->r.top = 13;
		} else {
			sel->r.top = CROP_START(imx307->cur_mode->height, DST_HEIGHT);
		}
		sel->r.height = DST_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static const struct dev_pm_ops imx307_pm_ops = {
	SET_RUNTIME_PM_OPS(imx307_runtime_suspend,
			   imx307_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx307_internal_ops = {
	.open = imx307_open,
};
#endif

static const struct v4l2_subdev_core_ops imx307_core_ops = {
	.s_power = imx307_s_power,
	.ioctl = imx307_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx307_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx307_video_ops = {
	.s_stream = imx307_s_stream,
	.g_frame_interval = imx307_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx307_pad_ops = {
	.enum_mbus_code = imx307_enum_mbus_code,
	.enum_frame_size = imx307_enum_frame_sizes,
	.enum_frame_interval = imx307_enum_frame_interval,
	.get_fmt = imx307_get_fmt,
	.set_fmt = imx307_set_fmt,
	.get_selection = imx307_get_selection,
};

static const struct v4l2_subdev_ops imx307_subdev_ops = {
	.core	= &imx307_core_ops,
	.video	= &imx307_video_ops,
	.pad	= &imx307_pad_ops,
};

static int imx307_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx307 *imx307 = container_of(ctrl->handler,
					     struct imx307, ctrl_handler);
	struct i2c_client *client = imx307->client;
	s64 max;
	int ret = 0;
	u32 shs1 = 0;
	u32 vts = 0;
	u32 val = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = imx307->cur_mode->height + ctrl->val - 2;
		__v4l2_ctrl_modify_range(imx307->exposure,
					 imx307->exposure->minimum, max,
					 imx307->exposure->step,
					 imx307->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		if (imx307->cur_mode->hdr_mode == NO_HDR) {
			shs1 = imx307->cur_vts - (ctrl->val + 1);
			ret = imx307_write_reg(imx307->client,
				IMX307_REG_SHS1_H,
				IMX307_REG_VALUE_08BIT,
				IMX307_FETCH_HIGH_BYTE_EXP(shs1));
			ret |= imx307_write_reg(imx307->client,
				IMX307_REG_SHS1_M,
				IMX307_REG_VALUE_08BIT,
				IMX307_FETCH_MID_BYTE_EXP(shs1));
			ret |= imx307_write_reg(imx307->client,
				IMX307_REG_SHS1_L,
				IMX307_REG_VALUE_08BIT,
				IMX307_FETCH_LOW_BYTE_EXP(shs1));
			dev_dbg(&client->dev, "set exposure 0x%x, cur_vts 0x%x,shs1 0x%x\n",
				ctrl->val, imx307->cur_vts, shs1);
		}
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		if (imx307->cur_mode->hdr_mode == NO_HDR) {
			ret = imx307_write_reg(imx307->client,
				IMX307_REG_LF_GAIN,
				IMX307_REG_VALUE_08BIT,
				ctrl->val);
			dev_dbg(&client->dev, "set analog gain 0x%x\n",
				ctrl->val);
		}
		break;
	case V4L2_CID_VBLANK:
		vts = ctrl->val + imx307->cur_mode->height;
		imx307->cur_vts = vts;
		if (imx307->cur_mode->hdr_mode == HDR_X2)
			vts /= 2;
		ret = imx307_write_reg(imx307->client,
			IMX307_REG_VTS_H,
			IMX307_REG_VALUE_08BIT,
			IMX307_FETCH_HIGH_BYTE_VTS(vts));
		ret |= imx307_write_reg(imx307->client,
			IMX307_REG_VTS_M,
			IMX307_REG_VALUE_08BIT,
			IMX307_FETCH_MID_BYTE_VTS(vts));
		ret |= imx307_write_reg(imx307->client,
			IMX307_REG_VTS_L,
			IMX307_REG_VALUE_08BIT,
			IMX307_FETCH_LOW_BYTE_VTS(vts));
		dev_dbg(&client->dev, "set vts 0x%x\n",
			vts);
		break;
	case V4L2_CID_TEST_PATTERN:
#ifdef USED_TEST_PATTERN
		ret = imx307_enable_test_pattern(imx307, ctrl->val);
#endif
		break;
	case V4L2_CID_HFLIP:
		ret = imx307_read_reg(client,
				      IMX307_FLIP_REG,
				      IMX307_REG_VALUE_08BIT,
				      &val);
		if (ctrl->val)
			val |= MIRROR_BIT_MASK;
		else
			val &= ~MIRROR_BIT_MASK;
		ret |= imx307_write_reg(client,
					IMX307_FLIP_REG,
					IMX307_REG_VALUE_08BIT,
					val);
		if (ret == 0)
			imx307->flip = val;
		break;
	case V4L2_CID_VFLIP:
		ret = imx307_read_reg(client,
				      IMX307_FLIP_REG,
				      IMX307_REG_VALUE_08BIT,
				      &val);
		if (ctrl->val)
			val |= FLIP_BIT_MASK;
		else
			val &= ~FLIP_BIT_MASK;
		ret |= imx307_write_reg(client,
					IMX307_FLIP_REG,
					IMX307_REG_VALUE_08BIT,
					val);
		if (ret == 0)
			imx307->flip = val;
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx307_ctrl_ops = {
	.s_ctrl = imx307_set_ctrl,
};

static int imx307_initialize_controls(struct imx307 *imx307)
{
	const struct imx307_mode *mode;
	struct v4l2_ctrl_handler *handler;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;
	s32 dst_link_freq = 0;
	s64 dst_pixel_rate = 0;

	handler = &imx307->ctrl_handler;
	mode = imx307->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 9);
	if (ret)
		return ret;
	handler->lock = &imx307->mutex;

	imx307->link_freq = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      1, 0, link_freq_menu_items);

	dst_link_freq = mode->freq_idx;
	dst_pixel_rate = (u32)link_freq_menu_items[mode->freq_idx] / mode->bpp * 2 * mode->lanes;
	__v4l2_ctrl_s_ctrl(imx307->link_freq,
			   dst_link_freq);
	imx307->pixel_rate = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, IMX307_PIXEL_RATE_HDR, 1, dst_pixel_rate);

	h_blank = mode->hts_def - mode->width;

	imx307->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (imx307->hblank)
		imx307->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	imx307->cur_vts = mode->vts_def;
	imx307->vblank = v4l2_ctrl_new_std(handler, &imx307_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				IMX307_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 4;

	imx307->exposure = v4l2_ctrl_new_std(handler, &imx307_ctrl_ops,
				V4L2_CID_EXPOSURE, IMX307_EXPOSURE_MIN,
				exposure_max, IMX307_EXPOSURE_STEP,
				mode->exp_def);

	imx307->anal_gain = v4l2_ctrl_new_std(handler, &imx307_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, IMX307_GAIN_MIN,
				IMX307_GAIN_MAX, IMX307_GAIN_STEP,
				IMX307_GAIN_DEFAULT);

#ifdef USED_TEST_PATTERN
	imx307->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&imx307_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(imx307_test_pattern_menu) - 1,
				0, 0, imx307_test_pattern_menu);
#endif
	imx307->h_flip = v4l2_ctrl_new_std(handler, &imx307_ctrl_ops,
				V4L2_CID_HFLIP, 0, 1, 1, 0);

	imx307->v_flip = v4l2_ctrl_new_std(handler, &imx307_ctrl_ops,
				V4L2_CID_VFLIP, 0, 1, 1, 0);
	imx307->flip = 0;
	if (handler->error) {
		ret = handler->error;
		dev_err(&imx307->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	imx307->subdev.ctrl_handler = handler;
	imx307->has_init_exp = false;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int imx307_check_sensor_id(struct imx307 *imx307,
				  struct i2c_client *client)
{
	struct device *dev = &imx307->client->dev;
	u32 id = 0;
	int ret;

	ret = imx307_read_reg(client, IMX307_REG_CHIP_ID,
			      IMX307_REG_VALUE_08BIT, &id);

	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		return -EINVAL;
	}
	return ret;
}

static int imx307_configure_regulators(struct imx307 *imx307)
{
	unsigned int i;

	for (i = 0; i < IMX307_NUM_SUPPLIES; i++)
		imx307->supplies[i].supply = imx307_supply_names[i];

	return devm_regulator_bulk_get(&imx307->client->dev,
				       IMX307_NUM_SUPPLIES,
				       imx307->supplies);
}

static int imx307_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx307 *imx307;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;
	struct device_node *endpoint;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	imx307 = devm_kzalloc(dev, sizeof(*imx307), GFP_KERNEL);
	if (!imx307)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx307->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx307->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx307->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx307->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}
	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!endpoint) {
		dev_err(dev, "Failed to get endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint),
		&imx307->bus_cfg);
	if (ret)
		dev_warn(dev, "could not get bus config!\n");
	if (imx307->bus_cfg.bus_type == V4L2_MBUS_CCP2) {
		if (imx307->bus_cfg.bus.mipi_csi1.data_lane == 2) {
			imx307->support_modes = lvds_2lane_supported_modes;
			imx307->support_modes_num = ARRAY_SIZE(lvds_2lane_supported_modes);
		} else if (imx307->bus_cfg.bus.mipi_csi1.data_lane  == 4) {
			imx307->support_modes = lvds_supported_modes;
			imx307->support_modes_num = ARRAY_SIZE(lvds_supported_modes);
		} else {
			dev_err(dev, "lvds lanes err!\n");
		}
	} else {
		imx307->support_modes = mipi_supported_modes;
		imx307->support_modes_num = ARRAY_SIZE(mipi_supported_modes);
	}
	imx307->client = client;
	imx307->cur_mode = &imx307->support_modes[0];

	imx307->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx307->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	imx307->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx307->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	imx307->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(imx307->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	ret = imx307_configure_regulators(imx307);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	imx307->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx307->pinctrl)) {
		imx307->pins_default =
			pinctrl_lookup_state(imx307->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(imx307->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		imx307->pins_sleep =
			pinctrl_lookup_state(imx307->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(imx307->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}

	mutex_init(&imx307->mutex);

	sd = &imx307->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx307_subdev_ops);
	ret = imx307_initialize_controls(imx307);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx307_power_on(imx307);
	if (ret)
		goto err_free_handler;

	ret = imx307_check_sensor_id(imx307, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	dev_err(dev, "set the video v4l2 subdev api\n");
	sd->internal_ops = &imx307_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	dev_err(dev, "set the media controller\n");
	imx307->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &imx307->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx307->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx307->module_index, facing,
		 IMX307_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);
	g_isHCG = false;
#ifdef USED_SYS_DEBUG
	add_sysfs_interfaces(dev);
#endif
	dev_err(dev, "v4l2 async register subdev success\n");
	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__imx307_power_off(imx307);
err_free_handler:
	v4l2_ctrl_handler_free(&imx307->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx307->mutex);

	return ret;
}

static int imx307_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx307 *imx307 = to_imx307(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx307->ctrl_handler);
	mutex_destroy(&imx307->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx307_power_off(imx307);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx307_of_match[] = {
	{ .compatible = "sony,imx307" },
	{},
};
MODULE_DEVICE_TABLE(of, imx307_of_match);
#endif

static const struct i2c_device_id imx307_match_id[] = {
	{ "sony,imx307", 0 },
	{ },
};

static struct i2c_driver imx307_i2c_driver = {
	.driver = {
		.name = IMX307_NAME,
		.pm = &imx307_pm_ops,
		.of_match_table = of_match_ptr(imx307_of_match),
	},
	.probe		= &imx307_probe,
	.remove		= &imx307_remove,
	.id_table	= imx307_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx307_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx307_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony imx307 sensor driver");
MODULE_LICENSE("GPL v2");
