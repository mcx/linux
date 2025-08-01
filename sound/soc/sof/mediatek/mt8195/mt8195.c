// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2021 Mediatek Inc. All rights reserved.
//
// Author: YC Hung <yc.hung@mediatek.com>
//

/*
 * Hardware interface for audio DSP on mt8195
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/module.h>

#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include "../../ops.h"
#include "../../sof-of-dev.h"
#include "../adsp_helper.h"
#include "../mtk-adsp-common.h"
#include "mt8195.h"
#include "mt8195-clk.h"

static int mt8195_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	return MBOX_OFFSET;
}

static int mt8195_get_window_offset(struct snd_sof_dev *sdev, u32 id)
{
	return MBOX_OFFSET;
}

static const struct mtk_adsp_ipc_ops dsp_ops = {
	.handle_reply		= mtk_adsp_handle_reply,
	.handle_request		= mtk_adsp_handle_request,
};

static int platform_parse_resource(struct platform_device *pdev, void *data)
{
	struct resource *mmio;
	struct resource res;
	struct device *dev = &pdev->dev;
	struct mtk_adsp_chip_info *adsp = data;
	int ret;

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "of_reserved_mem_device_init failed\n");
		return ret;
	}

	ret = of_reserved_mem_region_to_resource(dev->of_node, 1, &res);
	if (ret) {
		dev_err(dev, "of_address_to_resource sysmem failed\n");
		return ret;
	}

	adsp->pa_dram = (phys_addr_t)res.start;
	adsp->dramsize = resource_size(&res);
	if (adsp->pa_dram & DRAM_REMAP_MASK) {
		dev_err(dev, "adsp memory(%#x) is not 4K-aligned\n",
			(u32)adsp->pa_dram);
		return -EINVAL;
	}

	if (adsp->dramsize < TOTAL_SIZE_SHARED_DRAM_FROM_TAIL) {
		dev_err(dev, "adsp memory(%#x) is not enough for share\n",
			adsp->dramsize);
		return -EINVAL;
	}

	dev_dbg(dev, "dram pbase=%pa, dramsize=%#x\n",
		&adsp->pa_dram, adsp->dramsize);

	/* Parse CFG base */
	mmio = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (!mmio) {
		dev_err(dev, "no ADSP-CFG register resource\n");
		return -ENXIO;
	}
	/* remap for DSP register accessing */
	adsp->va_cfgreg = devm_ioremap_resource(dev, mmio);
	if (IS_ERR(adsp->va_cfgreg))
		return PTR_ERR(adsp->va_cfgreg);

	adsp->pa_cfgreg = (phys_addr_t)mmio->start;
	adsp->cfgregsize = resource_size(mmio);

	dev_dbg(dev, "cfgreg-vbase=%p, cfgregsize=%#x\n",
		adsp->va_cfgreg, adsp->cfgregsize);

	/* Parse SRAM */
	mmio = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (!mmio) {
		dev_err(dev, "no SRAM resource\n");
		return -ENXIO;
	}

	adsp->pa_sram = (phys_addr_t)mmio->start;
	adsp->sramsize = resource_size(mmio);

	dev_dbg(dev, "sram pbase=%pa,%#x\n", &adsp->pa_sram, adsp->sramsize);

	return ret;
}

static int adsp_sram_power_on(struct device *dev, bool on)
{
	void __iomem *va_dspsysreg;
	u32 srampool_con;

	va_dspsysreg = ioremap(ADSP_SRAM_POOL_CON, 0x4);
	if (!va_dspsysreg) {
		dev_err(dev, "failed to ioremap sram pool base %#x\n",
			ADSP_SRAM_POOL_CON);
		return -ENOMEM;
	}

	srampool_con = readl(va_dspsysreg);
	if (on)
		writel(srampool_con & ~DSP_SRAM_POOL_PD_MASK, va_dspsysreg);
	else
		writel(srampool_con | DSP_SRAM_POOL_PD_MASK, va_dspsysreg);

	iounmap(va_dspsysreg);
	return 0;
}

/*  Init the basic DSP DRAM address */
static int adsp_memory_remap_init(struct device *dev, struct mtk_adsp_chip_info *adsp)
{
	void __iomem *vaddr_emi_map;
	int offset;

	if (!adsp)
		return -ENXIO;

	vaddr_emi_map = devm_ioremap(dev, DSP_EMI_MAP_ADDR, 0x4);
	if (!vaddr_emi_map) {
		dev_err(dev, "failed to ioremap emi map base %#x\n",
			DSP_EMI_MAP_ADDR);
		return -ENOMEM;
	}

	offset = adsp->pa_dram - DRAM_PHYS_BASE_FROM_DSP_VIEW;
	adsp->dram_offset = offset;
	offset >>= DRAM_REMAP_SHIFT;
	dev_dbg(dev, "adsp->pa_dram %pa, offset %#x\n", &adsp->pa_dram, offset);
	writel(offset, vaddr_emi_map);
	if (offset != readl(vaddr_emi_map)) {
		dev_err(dev, "write emi map fail : %#x\n", readl(vaddr_emi_map));
		return -EIO;
	}

	return 0;
}

static int mt8195_run(struct snd_sof_dev *sdev)
{
	u32 adsp_bootup_addr;

	adsp_bootup_addr = SRAM_PHYS_BASE_FROM_DSP_VIEW;
	dev_dbg(sdev->dev, "HIFIxDSP boot from base : 0x%08X\n", adsp_bootup_addr);
	sof_hifixdsp_boot_sequence(sdev, adsp_bootup_addr);

	return 0;
}

static int mt8195_dsp_probe(struct snd_sof_dev *sdev)
{
	struct platform_device *pdev = to_platform_device(sdev->dev);
	struct adsp_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	sdev->pdata->hw_pdata = priv;
	priv->dev = sdev->dev;
	priv->sdev = sdev;

	priv->adsp = devm_kzalloc(&pdev->dev, sizeof(struct mtk_adsp_chip_info), GFP_KERNEL);
	if (!priv->adsp)
		return -ENOMEM;

	ret = platform_parse_resource(pdev, priv->adsp);
	if (ret)
		return ret;

	ret = mt8195_adsp_init_clock(sdev);
	if (ret) {
		dev_err(sdev->dev, "mt8195_adsp_init_clock failed\n");
		return -EINVAL;
	}

	ret = adsp_clock_on(sdev);
	if (ret) {
		dev_err(sdev->dev, "adsp_clock_on fail!\n");
		return -EINVAL;
	}

	ret = adsp_sram_power_on(sdev->dev, true);
	if (ret) {
		dev_err(sdev->dev, "adsp_sram_power_on fail!\n");
		goto exit_clk_disable;
	}

	ret = adsp_memory_remap_init(&pdev->dev, priv->adsp);
	if (ret) {
		dev_err(sdev->dev, "adsp_memory_remap_init fail!\n");
		goto err_adsp_sram_power_off;
	}

	sdev->bar[SOF_FW_BLK_TYPE_IRAM] = devm_ioremap(sdev->dev,
						       priv->adsp->pa_sram,
						       priv->adsp->sramsize);
	if (!sdev->bar[SOF_FW_BLK_TYPE_IRAM]) {
		dev_err(sdev->dev, "failed to ioremap base %pa size %#x\n",
			&priv->adsp->pa_sram, priv->adsp->sramsize);
		ret = -EINVAL;
		goto err_adsp_sram_power_off;
	}

	priv->adsp->va_sram = sdev->bar[SOF_FW_BLK_TYPE_IRAM];

	sdev->bar[SOF_FW_BLK_TYPE_SRAM] = devm_ioremap(sdev->dev,
						       priv->adsp->pa_dram,
						       priv->adsp->dramsize);
	if (!sdev->bar[SOF_FW_BLK_TYPE_SRAM]) {
		dev_err(sdev->dev, "failed to ioremap base %pa size %#x\n",
			&priv->adsp->pa_dram, priv->adsp->dramsize);
		ret = -EINVAL;
		goto err_adsp_sram_power_off;
	}
	priv->adsp->va_dram = sdev->bar[SOF_FW_BLK_TYPE_SRAM];

	sdev->bar[DSP_REG_BAR] = priv->adsp->va_cfgreg;

	sdev->mmio_bar = SOF_FW_BLK_TYPE_SRAM;
	sdev->mailbox_bar = SOF_FW_BLK_TYPE_SRAM;

	/* set default mailbox offset for FW ready message */
	sdev->dsp_box.offset = mt8195_get_mailbox_offset(sdev);

	priv->ipc_dev = platform_device_register_data(&pdev->dev, "mtk-adsp-ipc",
						      PLATFORM_DEVID_NONE,
						      pdev, sizeof(*pdev));
	if (IS_ERR(priv->ipc_dev)) {
		ret = PTR_ERR(priv->ipc_dev);
		dev_err(sdev->dev, "failed to register mtk-adsp-ipc device\n");
		goto err_adsp_sram_power_off;
	}

	priv->dsp_ipc = dev_get_drvdata(&priv->ipc_dev->dev);
	if (!priv->dsp_ipc) {
		ret = -EPROBE_DEFER;
		dev_err(sdev->dev, "failed to get drvdata\n");
		goto exit_pdev_unregister;
	}

	mtk_adsp_ipc_set_data(priv->dsp_ipc, priv);
	priv->dsp_ipc->ops = &dsp_ops;

	return 0;

exit_pdev_unregister:
	platform_device_unregister(priv->ipc_dev);
err_adsp_sram_power_off:
	adsp_sram_power_on(&pdev->dev, false);
exit_clk_disable:
	adsp_clock_off(sdev);

	return ret;
}

static int mt8195_dsp_shutdown(struct snd_sof_dev *sdev)
{
	return snd_sof_suspend(sdev->dev);
}

static void mt8195_dsp_remove(struct snd_sof_dev *sdev)
{
	struct platform_device *pdev = to_platform_device(sdev->dev);
	struct adsp_priv *priv = sdev->pdata->hw_pdata;

	platform_device_unregister(priv->ipc_dev);
	adsp_sram_power_on(&pdev->dev, false);
	adsp_clock_off(sdev);
}

static int mt8195_dsp_suspend(struct snd_sof_dev *sdev, u32 target_state)
{
	struct platform_device *pdev = to_platform_device(sdev->dev);
	int ret;
	u32 reset_sw, dbg_pc;

	/* wait dsp enter idle, timeout is 1 second */
	ret = snd_sof_dsp_read_poll_timeout(sdev, DSP_REG_BAR,
					    DSP_RESET_SW, reset_sw,
					    ((reset_sw & ADSP_PWAIT) == ADSP_PWAIT),
					    SUSPEND_DSP_IDLE_POLL_INTERVAL_US,
					    SUSPEND_DSP_IDLE_TIMEOUT_US);
	if (ret < 0) {
		dbg_pc = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGPC);
		dev_warn(sdev->dev, "dsp not idle, powering off anyway : swrest %#x, pc %#x, ret %d\n",
			 reset_sw, dbg_pc, ret);
	}

	/* stall and reset dsp */
	sof_hifixdsp_shutdown(sdev);

	/* power down adsp sram */
	ret = adsp_sram_power_on(&pdev->dev, false);
	if (ret) {
		dev_err(sdev->dev, "adsp_sram_power_off fail!\n");
		return ret;
	}

	/* turn off adsp clock */
	return adsp_clock_off(sdev);
}

static int mt8195_dsp_resume(struct snd_sof_dev *sdev)
{
	int ret;

	/* turn on adsp clock */
	ret = adsp_clock_on(sdev);
	if (ret) {
		dev_err(sdev->dev, "adsp_clock_on fail!\n");
		return ret;
	}

	/* power on adsp sram */
	ret = adsp_sram_power_on(sdev->dev, true);
	if (ret)
		dev_err(sdev->dev, "adsp_sram_power_on fail!\n");

	return ret;
}

static void mt8195_adsp_dump(struct snd_sof_dev *sdev, u32 flags)
{
	u32 dbg_pc, dbg_data, dbg_bus0, dbg_bus1, dbg_inst;
	u32 dbg_ls0stat, dbg_ls1stat, faultbus, faultinfo, swrest;

	/* dump debug registers */
	dbg_pc = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGPC);
	dbg_data = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGDATA);
	dbg_bus0 = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGBUS0);
	dbg_bus1 = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGBUS1);
	dbg_inst = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGINST);
	dbg_ls0stat = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGLS0STAT);
	dbg_ls1stat = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PDEBUGLS1STAT);
	faultbus = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PFAULTBUS);
	faultinfo = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_PFAULTINFO);
	swrest = snd_sof_dsp_read(sdev, DSP_REG_BAR, DSP_RESET_SW);

	dev_info(sdev->dev, "adsp dump : pc %#x, data %#x, bus0 %#x, bus1 %#x, swrest %#x",
		 dbg_pc, dbg_data, dbg_bus0, dbg_bus1, swrest);
	dev_info(sdev->dev, "dbg_inst %#x, ls0stat %#x, ls1stat %#x, faultbus %#x, faultinfo %#x",
		 dbg_inst, dbg_ls0stat, dbg_ls1stat, faultbus, faultinfo);

	mtk_adsp_dump(sdev, flags);
}

static struct snd_soc_dai_driver mt8195_dai[] = {
{
	.name = "SOF_DL2",
	.playback = {
		.channels_min = 1,
		.channels_max = 2,
	},
},
{
	.name = "SOF_DL3",
	.playback = {
		.channels_min = 1,
		.channels_max = 2,
	},
},
{
	.name = "SOF_UL4",
	.capture = {
		.channels_min = 1,
		.channels_max = 2,
	},
},
{
	.name = "SOF_UL5",
	.capture = {
		.channels_min = 1,
		.channels_max = 2,
	},
},
};

/* mt8195 ops */
static const struct snd_sof_dsp_ops sof_mt8195_ops = {
	/* probe and remove */
	.probe		= mt8195_dsp_probe,
	.remove		= mt8195_dsp_remove,
	.shutdown	= mt8195_dsp_shutdown,

	/* DSP core boot */
	.run		= mt8195_run,

	/* Block IO */
	.block_read	= sof_block_read,
	.block_write	= sof_block_write,

	/* Mailbox IO */
	.mailbox_read	= sof_mailbox_read,
	.mailbox_write	= sof_mailbox_write,

	/* Register IO */
	.write		= sof_io_write,
	.read		= sof_io_read,
	.write64	= sof_io_write64,
	.read64		= sof_io_read64,

	/* ipc */
	.send_msg		= mtk_adsp_send_msg,
	.get_mailbox_offset	= mt8195_get_mailbox_offset,
	.get_window_offset	= mt8195_get_window_offset,
	.ipc_msg_data		= sof_ipc_msg_data,
	.set_stream_data_offset = sof_set_stream_data_offset,

	/* misc */
	.get_bar_index	= mtk_adsp_get_bar_index,

	/* stream callbacks */
	.pcm_open	= sof_stream_pcm_open,
	.pcm_hw_params	= mtk_adsp_stream_pcm_hw_params,
	.pcm_pointer	= mtk_adsp_stream_pcm_pointer,
	.pcm_close	= sof_stream_pcm_close,

	/* firmware loading */
	.load_firmware	= snd_sof_load_firmware_memcpy,

	/* Firmware ops */
	.dsp_arch_ops = &sof_xtensa_arch_ops,

	/* Debug information */
	.dbg_dump = mt8195_adsp_dump,
	.debugfs_add_region_item = snd_sof_debugfs_add_region_item_iomem,

	/* DAI drivers */
	.drv = mt8195_dai,
	.num_drv = ARRAY_SIZE(mt8195_dai),

	/* PM */
	.suspend	= mt8195_dsp_suspend,
	.resume		= mt8195_dsp_resume,

	/* ALSA HW info flags */
	.hw_info =	SNDRV_PCM_INFO_MMAP |
			SNDRV_PCM_INFO_MMAP_VALID |
			SNDRV_PCM_INFO_INTERLEAVED |
			SNDRV_PCM_INFO_PAUSE |
			SNDRV_PCM_INFO_NO_PERIOD_WAKEUP,
};

static struct snd_sof_of_mach sof_mt8195_machs[] = {
	{
		.compatible = "google,tomato",
		.sof_tplg_filename = "sof-mt8195-mt6359-rt1019-rt5682.tplg"
	}, {
		.compatible = "google,dojo",
		.sof_tplg_filename = "sof-mt8195-mt6359-max98390-rt5682.tplg"
	}, {
		.compatible = "mediatek,mt8195",
		.sof_tplg_filename = "sof-mt8195.tplg"
	}, {
		/* sentinel */
	}
};

static const struct sof_dev_desc sof_of_mt8195_desc = {
	.of_machines = sof_mt8195_machs,
	.ipc_supported_mask	= BIT(SOF_IPC_TYPE_3),
	.ipc_default		= SOF_IPC_TYPE_3,
	.default_fw_path = {
		[SOF_IPC_TYPE_3] = "mediatek/sof",
	},
	.default_tplg_path = {
		[SOF_IPC_TYPE_3] = "mediatek/sof-tplg",
	},
	.default_fw_filename = {
		[SOF_IPC_TYPE_3] = "sof-mt8195.ri",
	},
	.nocodec_tplg_filename = "sof-mt8195-nocodec.tplg",
	.ops = &sof_mt8195_ops,
	.ipc_timeout = 1000,
};

static const struct of_device_id sof_of_mt8195_ids[] = {
	{ .compatible = "mediatek,mt8195-dsp", .data = &sof_of_mt8195_desc},
	{ }
};
MODULE_DEVICE_TABLE(of, sof_of_mt8195_ids);

/* DT driver definition */
static struct platform_driver snd_sof_of_mt8195_driver = {
	.probe = sof_of_probe,
	.remove = sof_of_remove,
	.shutdown = sof_of_shutdown,
	.driver = {
	.name = "sof-audio-of-mt8195",
		.pm = pm_ptr(&sof_of_pm),
		.of_match_table = sof_of_mt8195_ids,
	},
};
module_platform_driver(snd_sof_of_mt8195_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SOF support for MTL 8195 platforms");
MODULE_IMPORT_NS("SND_SOC_SOF_XTENSA");
MODULE_IMPORT_NS("SND_SOC_SOF_MTK_COMMON");
