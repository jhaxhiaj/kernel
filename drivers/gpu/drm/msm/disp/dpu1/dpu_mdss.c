/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2018, The Linux Foundation
 */

#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdesc.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/reset.h>
#include "dpu_kms.h"

#define to_dpu_mdss(x) container_of(x, struct dpu_mdss, base)

#define HW_REV				0x0
#define HW_INTR_STATUS			0x0010

#define UBWC_STATIC			0x144
#define UBWC_CTRL_2			0x150
#define UBWC_PREDICTION_MODE		0x154

/* Max BW defined in KBps */
#define MAX_BW				6800000

struct dpu_irq_controller {
	unsigned long enabled_mask;
	struct irq_domain *domain;
};

struct dpu_mdss {
	struct msm_mdss base;
	void __iomem *mmio;
	struct clk_bulk_data *clocks;
	size_t num_clocks;
	struct dpu_irq_controller irq_controller;
};

static void dpu_mdss_irq(struct irq_desc *desc)
{
	struct dpu_mdss *dpu_mdss = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 interrupts;

	chained_irq_enter(chip, desc);

	interrupts = readl_relaxed(dpu_mdss->mmio + HW_INTR_STATUS);

	while (interrupts) {
		irq_hw_number_t hwirq = fls(interrupts) - 1;
		int rc;

		rc = generic_handle_domain_irq(dpu_mdss->irq_controller.domain,
					       hwirq);
		if (rc < 0) {
			DRM_ERROR("handle irq fail: irq=%lu rc=%d\n",
				  hwirq, rc);
			break;
		}

		interrupts &= ~(1 << hwirq);
	}

	chained_irq_exit(chip, desc);
}

static void dpu_mdss_irq_mask(struct irq_data *irqd)
{
	struct dpu_mdss *dpu_mdss = irq_data_get_irq_chip_data(irqd);

	/* memory barrier */
	smp_mb__before_atomic();
	clear_bit(irqd->hwirq, &dpu_mdss->irq_controller.enabled_mask);
	/* memory barrier */
	smp_mb__after_atomic();
}

static void dpu_mdss_irq_unmask(struct irq_data *irqd)
{
	struct dpu_mdss *dpu_mdss = irq_data_get_irq_chip_data(irqd);

	/* memory barrier */
	smp_mb__before_atomic();
	set_bit(irqd->hwirq, &dpu_mdss->irq_controller.enabled_mask);
	/* memory barrier */
	smp_mb__after_atomic();
}

static struct irq_chip dpu_mdss_irq_chip = {
	.name = "dpu_mdss",
	.irq_mask = dpu_mdss_irq_mask,
	.irq_unmask = dpu_mdss_irq_unmask,
};

static struct lock_class_key dpu_mdss_lock_key, dpu_mdss_request_key;

static int dpu_mdss_irqdomain_map(struct irq_domain *domain,
		unsigned int irq, irq_hw_number_t hwirq)
{
	struct dpu_mdss *dpu_mdss = domain->host_data;

	irq_set_lockdep_class(irq, &dpu_mdss_lock_key, &dpu_mdss_request_key);
	irq_set_chip_and_handler(irq, &dpu_mdss_irq_chip, handle_level_irq);
	return irq_set_chip_data(irq, dpu_mdss);
}

static const struct irq_domain_ops dpu_mdss_irqdomain_ops = {
	.map = dpu_mdss_irqdomain_map,
	.xlate = irq_domain_xlate_onecell,
};

static int _dpu_mdss_irq_domain_add(struct dpu_mdss *dpu_mdss)
{
	struct device *dev;
	struct irq_domain *domain;

	dev = dpu_mdss->base.dev;

	domain = irq_domain_add_linear(dev->of_node, 32,
			&dpu_mdss_irqdomain_ops, dpu_mdss);
	if (!domain) {
		DPU_ERROR("failed to add irq_domain\n");
		return -EINVAL;
	}

	dpu_mdss->irq_controller.enabled_mask = 0;
	dpu_mdss->irq_controller.domain = domain;

	return 0;
}

static void _dpu_mdss_irq_domain_fini(struct dpu_mdss *dpu_mdss)
{
	if (dpu_mdss->irq_controller.domain) {
		irq_domain_remove(dpu_mdss->irq_controller.domain);
		dpu_mdss->irq_controller.domain = NULL;
	}
}
static int dpu_mdss_enable(struct msm_mdss *mdss)
{
	struct dpu_mdss *dpu_mdss = to_dpu_mdss(mdss);
	int ret;

	ret = clk_bulk_prepare_enable(dpu_mdss->num_clocks, dpu_mdss->clocks);
	if (ret) {
		DPU_ERROR("clock enable failed, ret:%d\n", ret);
		return ret;
	}

	/*
	 * ubwc config is part of the "mdss" region which is not accessible
	 * from the rest of the driver. hardcode known configurations here
	 */
	switch (readl_relaxed(dpu_mdss->mmio + HW_REV)) {
	case DPU_HW_VER_500:
	case DPU_HW_VER_501:
		writel_relaxed(0x420, dpu_mdss->mmio + UBWC_STATIC);
		break;
	case DPU_HW_VER_600:
		/* TODO: 0x102e for LP_DDR4 */
		writel_relaxed(0x103e, dpu_mdss->mmio + UBWC_STATIC);
		writel_relaxed(2, dpu_mdss->mmio + UBWC_CTRL_2);
		writel_relaxed(1, dpu_mdss->mmio + UBWC_PREDICTION_MODE);
		break;
	case DPU_HW_VER_620:
		writel_relaxed(0x1e, dpu_mdss->mmio + UBWC_STATIC);
		break;
	case DPU_HW_VER_720:
		writel_relaxed(0x101e, dpu_mdss->mmio + UBWC_STATIC);
		break;
	}

	return ret;
}

static int dpu_mdss_disable(struct msm_mdss *mdss)
{
	struct dpu_mdss *dpu_mdss = to_dpu_mdss(mdss);

	clk_bulk_disable_unprepare(dpu_mdss->num_clocks, dpu_mdss->clocks);

	return 0;
}

static void dpu_mdss_destroy(struct msm_mdss *mdss)
{
	struct platform_device *pdev = to_platform_device(mdss->dev);
	struct dpu_mdss *dpu_mdss = to_dpu_mdss(mdss);
	int irq;

	pm_runtime_suspend(mdss->dev);
	pm_runtime_disable(mdss->dev);
	_dpu_mdss_irq_domain_fini(dpu_mdss);
	irq = platform_get_irq(pdev, 0);
	irq_set_chained_handler_and_data(irq, NULL, NULL);

	if (dpu_mdss->mmio)
		devm_iounmap(&pdev->dev, dpu_mdss->mmio);
	dpu_mdss->mmio = NULL;
}

static int dpu_mdss_reset(struct device *dev)
{
	struct reset_control *reset;

	reset = reset_control_get_optional_exclusive(dev, NULL);
	if (!reset) {
		/* Optional reset not specified */
		return 0;
	} else if (IS_ERR(reset)) {
		DPU_ERROR("failed to acquire mdss reset, ret=%ld", PTR_ERR(reset));
		return PTR_ERR(reset);
	}

	reset_control_assert(reset);
	/*
	 * Tests indicate that reset has to be held for some period of time,
	 * make it one frame in a typical system
	 */
	msleep(20);
	reset_control_deassert(reset);

	reset_control_put(reset);

	return 0;
}

static const struct msm_mdss_funcs mdss_funcs = {
	.enable	= dpu_mdss_enable,
	.disable = dpu_mdss_disable,
	.destroy = dpu_mdss_destroy,
};

int dpu_mdss_init(struct platform_device *pdev)
{
	struct msm_drm_private *priv = platform_get_drvdata(pdev);
	struct dpu_mdss *dpu_mdss;
	int ret;
	int irq;

	ret = dpu_mdss_reset(&pdev->dev);
	if (ret)
		return ret;

	dpu_mdss = devm_kzalloc(&pdev->dev, sizeof(*dpu_mdss), GFP_KERNEL);
	if (!dpu_mdss)
		return -ENOMEM;

	dpu_mdss->mmio = msm_ioremap(pdev, "mdss");
	if (IS_ERR(dpu_mdss->mmio))
		return PTR_ERR(dpu_mdss->mmio);

	DRM_DEBUG("mapped mdss address space @%pK\n", dpu_mdss->mmio);

	ret = devm_clk_bulk_get_all(&pdev->dev, &dpu_mdss->clocks);
	if (ret < 0) {
		DPU_ERROR("failed to parse clocks, ret=%d\n", ret);
		goto clk_parse_err;
	}
	dpu_mdss->num_clocks = ret;

	dpu_mdss->base.dev = &pdev->dev;
	dpu_mdss->base.funcs = &mdss_funcs;

	ret = _dpu_mdss_irq_domain_add(dpu_mdss);
	if (ret)
		goto irq_domain_error;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto irq_error;
	}

	irq_set_chained_handler_and_data(irq, dpu_mdss_irq,
					 dpu_mdss);

	priv->mdss = &dpu_mdss->base;

	pm_runtime_enable(&pdev->dev);

	return 0;

irq_error:
	_dpu_mdss_irq_domain_fini(dpu_mdss);
irq_domain_error:
clk_parse_err:
	if (dpu_mdss->mmio)
		devm_iounmap(&pdev->dev, dpu_mdss->mmio);
	dpu_mdss->mmio = NULL;
	return ret;
}
