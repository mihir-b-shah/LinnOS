// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "ast_drv.h"

MODULE_FIRMWARE("ast_dp501_fw.bin");

static void ast_release_firmware(void *data)
{
	struct ast_device *ast = data;

	release_firmware(ast->dp501_fw);
	ast->dp501_fw = NULL;
}

static int ast_load_dp501_microcode(struct drm_device *dev)
{
	struct ast_device *ast = to_ast_device(dev);
	int ret;

	ret = request_firmware(&ast->dp501_fw, "ast_dp501_fw.bin", dev->dev);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev->dev, ast_release_firmware, ast);
}

static void send_ack(struct ast_device *ast)
{
	u8 sendack;
	sendack = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0x9b, 0xff);
	sendack |= 0x80;
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9b, 0x00, sendack);
}

static void send_nack(struct ast_device *ast)
{
	u8 sendack;
	sendack = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0x9b, 0xff);
	sendack &= ~0x80;
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9b, 0x00, sendack);
}

static bool wait_ack(struct ast_device *ast)
{
	u8 waitack;
	u32 retry = 0;
	do {
		waitack = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xd2, 0xff);
		waitack &= 0x80;
		udelay(100);
	} while ((!waitack) && (retry++ < 1000));

	if (retry < 1000)
		return true;
	else
		return false;
}

static bool wait_nack(struct ast_device *ast)
{
	u8 waitack;
	u32 retry = 0;
	do {
		waitack = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xd2, 0xff);
		waitack &= 0x80;
		udelay(100);
	} while ((waitack) && (retry++ < 1000));

	if (retry < 1000)
		return true;
	else
		return false;
}

static void set_cmd_trigger(struct ast_device *ast)
{
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9b, ~0x40, 0x40);
}

static void clear_cmd_trigger(struct ast_device *ast)
{
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9b, ~0x40, 0x00);
}

#if 0
static bool wait_fw_ready(struct ast_device *ast)
{
	u8 waitready;
	u32 retry = 0;
	do {
		waitready = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xd2, 0xff);
		waitready &= 0x40;
		udelay(100);
	} while ((!waitready) && (retry++ < 1000));

	if (retry < 1000)
		return true;
	else
		return false;
}
#endif

static bool ast_write_cmd(struct drm_device *dev, u8 data)
{
	struct ast_device *ast = to_ast_device(dev);
	int retry = 0;
	if (wait_nack(ast)) {
		send_nack(ast);
		ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9a, 0x00, data);
		send_ack(ast);
		set_cmd_trigger(ast);
		do {
			if (wait_ack(ast)) {
				clear_cmd_trigger(ast);
				send_nack(ast);
				return true;
			}
		} while (retry++ < 100);
	}
	clear_cmd_trigger(ast);
	send_nack(ast);
	return false;
}

static bool ast_write_data(struct drm_device *dev, u8 data)
{
	struct ast_device *ast = to_ast_device(dev);

	if (wait_nack(ast)) {
		send_nack(ast);
		ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9a, 0x00, data);
		send_ack(ast);
		if (wait_ack(ast)) {
			send_nack(ast);
			return true;
		}
	}
	send_nack(ast);
	return false;
}

#if 0
static bool ast_read_data(struct drm_device *dev, u8 *data)
{
	struct ast_device *ast = to_ast_device(dev);
	u8 tmp;

	*data = 0;

	if (wait_ack(ast) == false)
		return false;
	tmp = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xd3, 0xff);
	*data = tmp;
	if (wait_nack(ast) == false) {
		send_nack(ast);
		return false;
	}
	send_nack(ast);
	return true;
}

static void clear_cmd(struct ast_device *ast)
{
	send_nack(ast);
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0x9a, 0x00, 0x00);
}
#endif

static void ast_set_dp501_video_output(struct drm_device *dev, u8 mode)
{
	ast_write_cmd(dev, 0x40);
	ast_write_data(dev, mode);

	msleep(10);
}

static u32 get_fw_base(struct ast_device *ast)
{
	return ast_mindwm(ast, 0x1e6e2104) & 0x7fffffff;
}

bool ast_backup_fw(struct drm_device *dev, u8 *addr, u32 size)
{
	struct ast_device *ast = to_ast_device(dev);
	u32 i, data;
	u32 boot_address;

	if (ast->config_mode != ast_use_p2a)
		return false;

	data = ast_mindwm(ast, 0x1e6e2100) & 0x01;
	if (data) {
		boot_address = get_fw_base(ast);
		for (i = 0; i < size; i += 4)
			*(u32 *)(addr + i) = ast_mindwm(ast, boot_address + i);
		return true;
	}
	return false;
}

static bool ast_launch_m68k(struct drm_device *dev)
{
	struct ast_device *ast = to_ast_device(dev);
	u32 i, data, len = 0;
	u32 boot_address;
	u8 *fw_addr = NULL;
	u8 jreg;

	if (ast->config_mode != ast_use_p2a)
		return false;

	data = ast_mindwm(ast, 0x1e6e2100) & 0x01;
	if (!data) {

		if (ast->dp501_fw_addr) {
			fw_addr = ast->dp501_fw_addr;
			len = 32*1024;
		} else {
			if (!ast->dp501_fw &&
			    ast_load_dp501_microcode(dev) < 0)
				return false;

			fw_addr = (u8 *)ast->dp501_fw->data;
			len = ast->dp501_fw->size;
		}
		/* Get BootAddress */
		ast_moutdwm(ast, 0x1e6e2000, 0x1688a8a8);
		data = ast_mindwm(ast, 0x1e6e0004);
		switch (data & 0x03) {
		case 0:
			boot_address = 0x44000000;
			break;
		default:
		case 1:
			boot_address = 0x48000000;
			break;
		case 2:
			boot_address = 0x50000000;
			break;
		case 3:
			boot_address = 0x60000000;
			break;
		}
		boot_address -= 0x200000; /* -2MB */

		/* copy image to buffer */
		for (i = 0; i < len; i += 4) {
			data = *(u32 *)(fw_addr + i);
			ast_moutdwm(ast, boot_address + i, data);
		}

		/* Init SCU */
		ast_moutdwm(ast, 0x1e6e2000, 0x1688a8a8);

		/* Launch FW */
		ast_moutdwm(ast, 0x1e6e2104, 0x80000000 + boot_address);
		ast_moutdwm(ast, 0x1e6e2100, 1);

		/* Update Scratch */
		data = ast_mindwm(ast, 0x1e6e2040) & 0xfffff1ff;		/* D[11:9] = 100b: UEFI handling */
		data |= 0x800;
		ast_moutdwm(ast, 0x1e6e2040, data);

		jreg = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0x99, 0xfc); /* D[1:0]: Reserved Video Buffer */
		jreg |= 0x02;
		ast_set_index_reg(ast, AST_IO_VGACRI, 0x99, jreg);
	}
	return true;
}

static bool ast_dp501_is_connected(struct ast_device *ast)
{
	u32 boot_address, offset, data;

	if (ast->config_mode == ast_use_p2a) {
		boot_address = get_fw_base(ast);

		/* validate FW version */
		offset = AST_DP501_GBL_VERSION;
		data = ast_mindwm(ast, boot_address + offset);
		if ((data & AST_DP501_FW_VERSION_MASK) != AST_DP501_FW_VERSION_1)
			return false;

		/* validate PnP Monitor */
		offset = AST_DP501_PNPMONITOR;
		data = ast_mindwm(ast, boot_address + offset);
		if (!(data & AST_DP501_PNP_CONNECTED))
			return false;
	} else {
		if (!ast->dp501_fw_buf)
			return false;

		/* dummy read */
		offset = 0x0000;
		data = readl(ast->dp501_fw_buf + offset);

		/* validate FW version */
		offset = AST_DP501_GBL_VERSION;
		data = readl(ast->dp501_fw_buf + offset);
		if ((data & AST_DP501_FW_VERSION_MASK) != AST_DP501_FW_VERSION_1)
			return false;

		/* validate PnP Monitor */
		offset = AST_DP501_PNPMONITOR;
		data = readl(ast->dp501_fw_buf + offset);
		if (!(data & AST_DP501_PNP_CONNECTED))
			return false;
	}
	return true;
}

static int ast_dp512_read_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct ast_device *ast = data;
	size_t rdlen = round_up(len, 4);
	u32 i, boot_address, offset, ediddata;

	if (block > (512 / EDID_LENGTH))
		return -EIO;

	offset = AST_DP501_EDID_DATA + block * EDID_LENGTH;

	if (ast->config_mode == ast_use_p2a) {
		boot_address = get_fw_base(ast);

		for (i = 0; i < rdlen; i += 4) {
			ediddata = ast_mindwm(ast, boot_address + offset + i);
			memcpy(buf, &ediddata, min((len - i), 4));
			buf += 4;
		}
	} else {
		for (i = 0; i < rdlen; i += 4) {
			ediddata = readl(ast->dp501_fw_buf + offset + i);
			memcpy(buf, &ediddata, min((len - i), 4));
			buf += 4;
		}
	}

	return true;
}

static bool ast_init_dvo(struct drm_device *dev)
{
	struct ast_device *ast = to_ast_device(dev);
	u8 jreg;
	u32 data;
	ast_write32(ast, 0xf004, 0x1e6e0000);
	ast_write32(ast, 0xf000, 0x1);
	ast_write32(ast, 0x12000, 0x1688a8a8);

	jreg = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xd0, 0xff);
	if (!(jreg & 0x80)) {
		/* Init SCU DVO Settings */
		data = ast_read32(ast, 0x12008);
		/* delay phase */
		data &= 0xfffff8ff;
		data |= 0x00000500;
		ast_write32(ast, 0x12008, data);

		if (IS_AST_GEN4(ast)) {
			data = ast_read32(ast, 0x12084);
			/* multi-pins for DVO single-edge */
			data |= 0xfffe0000;
			ast_write32(ast, 0x12084, data);

			data = ast_read32(ast, 0x12088);
			/* multi-pins for DVO single-edge */
			data |= 0x000fffff;
			ast_write32(ast, 0x12088, data);

			data = ast_read32(ast, 0x12090);
			/* multi-pins for DVO single-edge */
			data &= 0xffffffcf;
			data |= 0x00000020;
			ast_write32(ast, 0x12090, data);
		} else { /* AST GEN5+ */
			data = ast_read32(ast, 0x12088);
			/* multi-pins for DVO single-edge */
			data |= 0x30000000;
			ast_write32(ast, 0x12088, data);

			data = ast_read32(ast, 0x1208c);
			/* multi-pins for DVO single-edge */
			data |= 0x000000cf;
			ast_write32(ast, 0x1208c, data);

			data = ast_read32(ast, 0x120a4);
			/* multi-pins for DVO single-edge */
			data |= 0xffff0000;
			ast_write32(ast, 0x120a4, data);

			data = ast_read32(ast, 0x120a8);
			/* multi-pins for DVO single-edge */
			data |= 0x0000000f;
			ast_write32(ast, 0x120a8, data);

			data = ast_read32(ast, 0x12094);
			/* multi-pins for DVO single-edge */
			data |= 0x00000002;
			ast_write32(ast, 0x12094, data);
		}
	}

	/* Force to DVO */
	data = ast_read32(ast, 0x1202c);
	data &= 0xfffbffff;
	ast_write32(ast, 0x1202c, data);

	/* Init VGA DVO Settings */
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xa3, 0xcf, 0x80);
	return true;
}


static void ast_init_analog(struct drm_device *dev)
{
	struct ast_device *ast = to_ast_device(dev);
	u32 data;

	/*
	 * Set DAC source to VGA mode in SCU2C via the P2A
	 * bridge. First configure the P2U to target the SCU
	 * in case it isn't at this stage.
	 */
	ast_write32(ast, 0xf004, 0x1e6e0000);
	ast_write32(ast, 0xf000, 0x1);

	/* Then unlock the SCU with the magic password */
	ast_write32(ast, 0x12000, 0x1688a8a8);
	ast_write32(ast, 0x12000, 0x1688a8a8);
	ast_write32(ast, 0x12000, 0x1688a8a8);

	/* Finally, clear bits [17:16] of SCU2c */
	data = ast_read32(ast, 0x1202c);
	data &= 0xfffcffff;
	ast_write32(ast, 0, data);

	/* Disable DVO */
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xa3, 0xcf, 0x00);
}

void ast_init_3rdtx(struct drm_device *dev)
{
	struct ast_device *ast = to_ast_device(dev);
	u8 jreg;

	if (IS_AST_GEN4(ast) || IS_AST_GEN5(ast)) {
		jreg = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xd1, 0xff);
		switch (jreg & 0x0e) {
		case 0x04:
			ast_init_dvo(dev);
			break;
		case 0x08:
			ast_launch_m68k(dev);
			break;
		case 0x0c:
			ast_init_dvo(dev);
			break;
		default:
			if (ast->tx_chip_types & BIT(AST_TX_SIL164))
				ast_init_dvo(dev);
			else
				ast_init_analog(dev);
		}
	}
}

/*
 * Encoder
 */

static const struct drm_encoder_funcs ast_dp501_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void ast_dp501_encoder_helper_atomic_enable(struct drm_encoder *encoder,
						   struct drm_atomic_state *state)
{
	struct drm_device *dev = encoder->dev;

	ast_set_dp501_video_output(dev, 1);
}

static void ast_dp501_encoder_helper_atomic_disable(struct drm_encoder *encoder,
						    struct drm_atomic_state *state)
{
	struct drm_device *dev = encoder->dev;

	ast_set_dp501_video_output(dev, 0);
}

static const struct drm_encoder_helper_funcs ast_dp501_encoder_helper_funcs = {
	.atomic_enable = ast_dp501_encoder_helper_atomic_enable,
	.atomic_disable = ast_dp501_encoder_helper_atomic_disable,
};

/*
 * Connector
 */

static int ast_dp501_connector_helper_get_modes(struct drm_connector *connector)
{
	struct ast_connector *ast_connector = to_ast_connector(connector);
	int count;

	if (ast_connector->physical_status == connector_status_connected) {
		struct ast_device *ast = to_ast_device(connector->dev);
		const struct drm_edid *drm_edid;

		drm_edid = drm_edid_read_custom(connector, ast_dp512_read_edid_block, ast);
		drm_edid_connector_update(connector, drm_edid);
		count = drm_edid_connector_add_modes(connector);
		drm_edid_free(drm_edid);
	} else {
		drm_edid_connector_update(connector, NULL);

		/*
		 * There's no EDID data without a connected monitor. Set BMC-
		 * compatible modes in this case. The XGA default resolution
		 * should work well for all BMCs.
		 */
		count = drm_add_modes_noedid(connector, 4096, 4096);
		if (count)
			drm_set_preferred_mode(connector, 1024, 768);
	}

	return count;
}

static int ast_dp501_connector_helper_detect_ctx(struct drm_connector *connector,
						 struct drm_modeset_acquire_ctx *ctx,
						 bool force)
{
	struct ast_connector *ast_connector = to_ast_connector(connector);
	struct ast_device *ast = to_ast_device(connector->dev);
	enum drm_connector_status status = connector_status_disconnected;

	if (ast_dp501_is_connected(ast))
		status = connector_status_connected;

	if (status != ast_connector->physical_status)
		++connector->epoch_counter;
	ast_connector->physical_status = status;

	return connector_status_connected;
}

static const struct drm_connector_helper_funcs ast_dp501_connector_helper_funcs = {
	.get_modes = ast_dp501_connector_helper_get_modes,
	.detect_ctx = ast_dp501_connector_helper_detect_ctx,
};

static const struct drm_connector_funcs ast_dp501_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int ast_dp501_connector_init(struct drm_device *dev, struct drm_connector *connector)
{
	int ret;

	ret = drm_connector_init(dev, connector, &ast_dp501_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;

	drm_connector_helper_add(connector, &ast_dp501_connector_helper_funcs);

	connector->interlace_allowed = 0;
	connector->doublescan_allowed = 0;

	connector->polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	return 0;
}

int ast_dp501_output_init(struct ast_device *ast)
{
	struct drm_device *dev = &ast->base;
	struct drm_crtc *crtc = &ast->crtc;
	struct drm_encoder *encoder = &ast->output.dp501.encoder;
	struct ast_connector *ast_connector = &ast->output.dp501.connector;
	struct drm_connector *connector = &ast_connector->base;
	int ret;

	ret = drm_encoder_init(dev, encoder, &ast_dp501_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	drm_encoder_helper_add(encoder, &ast_dp501_encoder_helper_funcs);

	encoder->possible_crtcs = drm_crtc_mask(crtc);

	ret = ast_dp501_connector_init(dev, connector);
	if (ret)
		return ret;
	ast_connector->physical_status = connector->status;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ret;

	return 0;
}