// SPDX-License-Identifier: GPL-2.0
/*
 * Forlinx-mipi7  MIPI-DSI panel driver
 *
 * Copyright 2024 Forlinx
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

/* Panel specific color-format bits */
#define COL_FMT_16BPP 0x55
#define COL_FMT_18BPP 0x66
#define COL_FMT_24BPP 0x77

/* Write Manufacture Command Set Control */
#define WRMAUCCTR 0xFE

static const u32 forlinx_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
	MEDIA_BUS_FMT_RGB666_1X18,
	MEDIA_BUS_FMT_RGB565_1X16,
};

static const u32 forlinx_bus_flags = DRM_BUS_FLAG_DE_LOW |
				 DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE;

struct forlinx_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;

	struct gpio_desc *enable;

	struct regulator_bulk_data *supplies;
	unsigned int num_supplies;

	unsigned int width_mm;
	unsigned int height_mm;

	bool prepared;
	bool enabled;
};

static const struct drm_display_mode default_mode = {
	.clock = 74250,
	.hdisplay = 1024,
	.hsync_start = 1024 + 153,     //bp
	.hsync_end = 1024 + 153+ 153,   //bp sw
	.htotal = 1024 + 153 + 153 + 70, //bp sw fp 1400
	.vdisplay = 600,
	.vsync_start = 600 + 45,
	.vsync_end = 600 + 45+ 45,
	.vtotal = 600 + 45 + 45 + 10, // 700
	.width_mm = 68,
	.height_mm = 121,
	.flags = DRM_MODE_FLAG_NHSYNC |
		 DRM_MODE_FLAG_NVSYNC,
};

static inline struct forlinx_panel *to_forlinx_panel(struct drm_panel *panel)
{
	return container_of(panel, struct forlinx_panel, panel);
}

static int color_format_from_dsi_format(enum mipi_dsi_pixel_format format)
{
	switch (format) {
	case MIPI_DSI_FMT_RGB565:
		return COL_FMT_16BPP;
	case MIPI_DSI_FMT_RGB666:
	case MIPI_DSI_FMT_RGB666_PACKED:
		return COL_FMT_18BPP;
	case MIPI_DSI_FMT_RGB888:
		return COL_FMT_24BPP;
	default:
		return COL_FMT_24BPP; /* for backward compatibility */
	}
};

static int forlinx_panel_prepare(struct drm_panel *panel)
{
	struct forlinx_panel *forlinx = to_forlinx_panel(panel);
	int ret;

	if (forlinx->prepared)
		return 0;

	ret = regulator_bulk_enable(forlinx->num_supplies, forlinx->supplies);
	if (ret)
		return ret;

	/* At lest 10ms needed between power-on and reset-out as RM specifies */
	usleep_range(10000, 12000);

	if (forlinx->enable) {
		gpiod_set_value_cansleep(forlinx->enable, 0);
		/*
		 * 50ms delay after reset-out, as per manufacturer initalization
		 * sequence.
		 */
		msleep(50);
	}

	forlinx->prepared = true;

	return 0;
}

static int forlinx_panel_unprepare(struct drm_panel *panel)
{
	struct forlinx_panel *forlinx = to_forlinx_panel(panel);
	int ret;

	if (!forlinx->prepared)
		return 0;

	/*
	 * Right after asserting the reset, we need to release it, so that the
	 * touch driver can have an active connection with the touch controller
	 * even after the display is turned off.
	 */
	if (forlinx->enable) {
		gpiod_set_value_cansleep(forlinx->enable, 1);
		usleep_range(15000, 17000);
		gpiod_set_value_cansleep(forlinx->enable, 0);
	}

	ret = regulator_bulk_disable(forlinx->num_supplies, forlinx->supplies);
	if (ret)
		return ret;

	forlinx->prepared = false;

	return 0;
}

static int forlinx_panel_enable(struct drm_panel *panel)
{
	struct forlinx_panel *forlinx = to_forlinx_panel(panel);
	struct mipi_dsi_device *dsi = forlinx->dsi;
	struct device *dev = &dsi->dev;
	int color_format = color_format_from_dsi_format(dsi->format);
	int ret;

	if (forlinx->enabled)
		return 0;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* Software reset */
	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to do Software Reset (%d)\n", ret);
		goto fail;
	}

	usleep_range(120000, 125000);

	ret = mipi_dsi_generic_write(dsi, (u8[]){ WRMAUCCTR, 0x72 }, 2);
	if (ret < 0)
		goto fail;

	/* Use 8-bit brightness mode */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ 0x4D, 0x65 }, 2);
	if (ret < 0)
		goto fail;

	/* Select User Command Set table (CMD1) */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ WRMAUCCTR, 0x00 }, 2);
	if (ret < 0)
		goto fail;

	/* Set DSI mode */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ 0xC2, 0x0B }, 2);
	if (ret < 0) {
		dev_err(dev, "Failed to set DSI mode (%d)\n", ret);
		goto fail;
	}
	/* Set tear ON */
	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear ON (%d)\n", ret);
		goto fail;
	}
	/* Set tear scanline */
	ret = mipi_dsi_dcs_set_tear_scanline(dsi, 0x380);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear scanline (%d)\n", ret);
		goto fail;
	}
	/* Set pixel format */
	ret = mipi_dsi_dcs_set_pixel_format(dsi, color_format);
	dev_dbg(dev, "Interface color format set to 0x%x\n", color_format);
	if (ret < 0) {
		dev_err(dev, "Failed to set pixel format (%d)\n", ret);
		goto fail;
	}
	/* Exit sleep mode */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode (%d)\n", ret);
		goto fail;
	}

	usleep_range(5000, 7000);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display ON (%d)\n", ret);
		goto fail;
	}

	backlight_enable(forlinx->panel.backlight);

	forlinx->enabled = true;

	return 0;
fail:
	gpiod_set_value_cansleep(forlinx->enable, 1);

	return ret;
}

static int forlinx_panel_disable(struct drm_panel *panel)
{
	struct forlinx_panel *forlinx = to_forlinx_panel(panel);
	struct mipi_dsi_device *dsi = forlinx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (!forlinx->enabled)
		return 0;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	backlight_disable(forlinx->panel.backlight);

	usleep_range(10000, 12000);

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display OFF (%d)\n", ret);
		return ret;
	}

	usleep_range(5000, 10000);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode (%d)\n", ret);
		return ret;
	}

	forlinx->enabled = false;

	return 0;
}

static int forlinx_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct device_node *timings_np, *np = panel->dev->of_node;
	struct forlinx_panel *forlinx = to_forlinx_panel(panel);

	int ret;

	timings_np = of_get_child_by_name(np, "display-timings");
	if(timings_np){
		mode = drm_mode_create(connector->dev);
		if(!mode){
			of_node_put(timings_np);
			return 0;
		}
		ret = of_get_drm_display_mode(np, mode, NULL, 0);
		if(ret){
			dev_dbg(panel->dev, "failed to find dts display timings\n");
			drm_mode_destroy(connector->dev, mode);
			mode = NULL;
		} else {
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		}
		of_node_put(timings_np);
	}
	else{
		mode = drm_mode_duplicate(connector->dev, &default_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
				default_mode.hdisplay, default_mode.vdisplay,
				drm_mode_vrefresh(&default_mode));
			return -ENOMEM;
		}
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if(forlinx->width_mm != 0)
		mode->width_mm = forlinx->width_mm;
	if(forlinx->height_mm != 0)
		mode->height_mm = forlinx->height_mm;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bus_flags = forlinx_bus_flags;

	drm_display_info_set_bus_formats(&connector->display_info,
					 forlinx_bus_formats,
					 ARRAY_SIZE(forlinx_bus_formats));
	return 1;
}

static const struct drm_panel_funcs forlinx_panel_funcs = {
	.prepare = forlinx_panel_prepare,
	.unprepare = forlinx_panel_unprepare,
	.enable = forlinx_panel_enable,
	.disable = forlinx_panel_disable,
	.get_modes = forlinx_panel_get_modes,
};

static const char * const forlinx_supply_names[] = {
	"v3p3",
	"v1p8",
};

static int forlinx_init_regulators(struct forlinx_panel *forlinx)
{
	struct device *dev = &forlinx->dsi->dev;
	int i;

	forlinx->num_supplies = ARRAY_SIZE(forlinx_supply_names);
	forlinx->supplies = devm_kcalloc(dev, forlinx->num_supplies,
				     sizeof(*forlinx->supplies), GFP_KERNEL);
	if (!forlinx->supplies)
		return -ENOMEM;

	for (i = 0; i < forlinx->num_supplies; i++)
		forlinx->supplies[i].supply = forlinx_supply_names[i];

	return devm_regulator_bulk_get(dev, forlinx->num_supplies, forlinx->supplies);
};

static const struct of_device_id forlinx_of_match[] = {
	{ .compatible = "forlinx,panel-mipi7", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, forlinx_of_match);

static int forlinx_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct forlinx_panel *forlinx;
	struct device_node *np = dev->of_node;
	int ret;

	forlinx = devm_kzalloc(&dsi->dev, sizeof(*forlinx), GFP_KERNEL);
	if (!forlinx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, forlinx);

	forlinx->dsi = dsi;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_HSE |
			  MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	forlinx->enable = devm_gpiod_get_optional(dev, "enable",
					       GPIOD_OUT_LOW |
					       GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(forlinx->enable)) {
		ret = PTR_ERR(forlinx->enable);
		dev_err(dev, "Failed to get enable gpio (%d)\n", ret);
		return ret;
	}
	gpiod_set_value_cansleep(forlinx->enable, 1);


	ret = of_property_read_u32(np, "width-mm", &forlinx->width_mm);
	if(ret){
		dev_warn(dev, "failed to get width-mm from dtb\n");
	}

	ret = of_property_read_u32(np, "height-mm",&forlinx->height_mm);
	if(ret){
		dev_warn(dev, "failed to get height-mm from dtb\n");
	}

	ret = forlinx_init_regulators(forlinx);
	if (ret)
		return ret;

	drm_panel_init(&forlinx->panel, dev, &forlinx_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	dev_set_drvdata(dev, forlinx);

	drm_panel_add(&forlinx->panel);

	ret = drm_panel_of_backlight(&forlinx->panel);
	if (ret) {
		dev_err_probe(dev, ret, "Could not find backlight\n");
	}

	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&forlinx->panel);

	return ret;
}

static void forlinx_panel_remove(struct mipi_dsi_device *dsi)
{
	struct forlinx_panel *forlinx = mipi_dsi_get_drvdata(dsi);
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret)
		dev_err(dev, "Failed to detach from host (%d)\n", ret);

	drm_panel_remove(&forlinx->panel);
}

static void forlinx_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct forlinx_panel *forlinx = mipi_dsi_get_drvdata(dsi);

	forlinx_panel_disable(&forlinx->panel);
	forlinx_panel_unprepare(&forlinx->panel);
}

static struct mipi_dsi_driver forlinx_panel_driver = {
	.driver = {
		.name = "forlinx-panel-mipi7",
		.of_match_table = forlinx_of_match,
	},
	.probe = forlinx_panel_probe,
	.remove = forlinx_panel_remove,
	.shutdown = forlinx_panel_shutdown,
};
module_mipi_dsi_driver(forlinx_panel_driver);

MODULE_AUTHOR("Forlinx");
MODULE_DESCRIPTION("DRM Driver for Forlinx-mipi7 DSI panel");
MODULE_LICENSE("GPL v2");
