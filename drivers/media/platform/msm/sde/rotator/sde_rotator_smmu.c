/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/iommu.h>
#include <linux/qcom_iommu.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk/msm-clk.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/of_platform.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/qcom_iommu.h>
#include <asm/dma-iommu.h>
#include <linux/delay.h>

#include "soc/qcom/secure_buffer.h"
#include "sde_rotator_base.h"
#include "sde_rotator_util.h"
#include "sde_rotator_io_util.h"
#include "sde_rotator_smmu.h"

#define SMMU_SDE_ROT_SEC	"qcom,smmu_sde_rot_sec"
#define SMMU_SDE_ROT_UNSEC	"qcom,smmu_sde_rot_unsec"

struct sde_smmu_domain {
	char *ctx_name;
	int domain;
	unsigned long start;
	unsigned long size;
};

static inline bool sde_smmu_is_valid_domain_type(
		struct sde_rot_data_type *mdata, int domain_type)
{
	return true;
}

struct sde_smmu_client *sde_smmu_get_cb(u32 domain)
{
	struct sde_rot_data_type *mdata = sde_rot_get_mdata();

	if (!sde_smmu_is_valid_domain_type(mdata, domain))
		return NULL;

	return (domain >= SDE_IOMMU_MAX_DOMAIN) ? NULL :
			&mdata->sde_smmu[domain];
}

static int sde_smmu_util_parse_dt_clock(struct platform_device *pdev,
		struct sde_module_power *mp)
{
	u32 i = 0, rc = 0;
	const char *clock_name;
	u32 clock_rate;
	int num_clk;

	num_clk = of_property_count_strings(pdev->dev.of_node,
			"clock-names");
	if (num_clk <= 0) {
		SDEROT_ERR("clocks are not defined\n");
		goto clk_err;
	}

	mp->num_clk = num_clk;
	mp->clk_config = devm_kzalloc(&pdev->dev,
			sizeof(struct sde_clk) * mp->num_clk, GFP_KERNEL);
	if (!mp->clk_config) {
		rc = -ENOMEM;
		mp->num_clk = 0;
		goto clk_err;
	}

	for (i = 0; i < mp->num_clk; i++) {
		of_property_read_string_index(pdev->dev.of_node, "clock-names",
							i, &clock_name);
		strlcpy(mp->clk_config[i].clk_name, clock_name,
				sizeof(mp->clk_config[i].clk_name));

		of_property_read_u32_index(pdev->dev.of_node, "clock-rate",
							i, &clock_rate);
		mp->clk_config[i].rate = clock_rate;

		if (!clock_rate)
			mp->clk_config[i].type = SDE_CLK_AHB;
		else
			mp->clk_config[i].type = SDE_CLK_PCLK;
	}

clk_err:
	return rc;
}

static int sde_smmu_clk_register(struct platform_device *pdev,
		struct sde_module_power *mp)
{
	int i, ret;
	struct clk *clk;

	ret = sde_smmu_util_parse_dt_clock(pdev, mp);
	if (ret) {
		SDEROT_ERR("unable to parse clocks\n");
		return -EINVAL;
	}

	for (i = 0; i < mp->num_clk; i++) {
		clk = devm_clk_get(&pdev->dev,
				mp->clk_config[i].clk_name);
		if (IS_ERR(clk)) {
			SDEROT_ERR("unable to get clk: %s\n",
					mp->clk_config[i].clk_name);
			return PTR_ERR(clk);
		}
		mp->clk_config[i].clk = clk;
	}
	return 0;
}

static int sde_smmu_enable_power(struct sde_smmu_client *sde_smmu,
	bool enable)
{
	int rc = 0;
	struct sde_module_power *mp;

	if (!sde_smmu)
		return -EINVAL;

	mp = &sde_smmu->mp;

	if (!mp->num_vreg && !mp->num_clk)
		return 0;

	if (enable) {
		rc = sde_rot_enable_vreg(mp->vreg_config, mp->num_vreg, true);
		if (rc) {
			SDEROT_ERR("vreg enable failed - rc:%d\n", rc);
			goto end;
		}
		sde_update_reg_bus_vote(sde_smmu->reg_bus_clt,
			VOTE_INDEX_19_MHZ);
		rc = sde_rot_enable_clk(mp->clk_config, mp->num_clk, true);
		if (rc) {
			SDEROT_ERR("clock enable failed - rc:%d\n", rc);
			sde_update_reg_bus_vote(sde_smmu->reg_bus_clt,
				VOTE_INDEX_DISABLE);
			sde_rot_enable_vreg(mp->vreg_config, mp->num_vreg,
				false);
			goto end;
		}
	} else {
		sde_rot_enable_clk(mp->clk_config, mp->num_clk, false);
		sde_update_reg_bus_vote(sde_smmu->reg_bus_clt,
			VOTE_INDEX_DISABLE);
		sde_rot_enable_vreg(mp->vreg_config, mp->num_vreg, false);
	}
end:
	return rc;
}

/*
 * sde_smmu_attach()
 *
 * Associates each configured VA range with the corresponding smmu context
 * bank device. Enables the clks as smmu requires voting it before the usage.
 * And iommu attach is done only once during the initial attach and it is never
 * detached as smmu v2 uses a feature called 'retention'.
 */
static int sde_smmu_attach(struct sde_rot_data_type *mdata)
{
	struct sde_smmu_client *sde_smmu;
	int i, rc = 0;

	for (i = 0; i < SDE_IOMMU_MAX_DOMAIN; i++) {
		if (!sde_smmu_is_valid_domain_type(mdata, i))
			continue;

		sde_smmu = sde_smmu_get_cb(i);
		if (sde_smmu && sde_smmu->dev) {
			rc = sde_smmu_enable_power(sde_smmu, true);
			if (rc) {
				SDEROT_ERR(
					"power enable failed - domain:[%d] rc:%d\n",
					i, rc);
				goto err;
			}

			if (!sde_smmu->domain_attached) {
				rc = arm_iommu_attach_device(sde_smmu->dev,
						sde_smmu->mmu_mapping);
				if (rc) {
					SDEROT_ERR(
						"iommu attach device failed for domain[%d] with err:%d\n",
						i, rc);
					sde_smmu_enable_power(sde_smmu,
						false);
					goto err;
				}
				sde_smmu->domain_attached = true;
				SDEROT_DBG("iommu v2 domain[%i] attached\n", i);
			}
		} else {
			SDEROT_ERR(
				"iommu device not attached for domain[%d]\n",
				i);
			return -ENODEV;
		}
	}
	return 0;

err:
	for (i--; i >= 0; i--) {
		sde_smmu = sde_smmu_get_cb(i);
		if (sde_smmu && sde_smmu->dev) {
			arm_iommu_detach_device(sde_smmu->dev);
			sde_smmu_enable_power(sde_smmu, false);
			sde_smmu->domain_attached = false;
		}
	}
	return rc;
}

/*
 * sde_smmu_detach()
 *
 * Only disables the clks as it is not required to detach the iommu mapped
 * VA range from the device in smmu as explained in the sde_smmu_attach
 */
static int sde_smmu_detach(struct sde_rot_data_type *mdata)
{
	struct sde_smmu_client *sde_smmu;
	int i;

	for (i = 0; i < SDE_IOMMU_MAX_DOMAIN; i++) {
		if (!sde_smmu_is_valid_domain_type(mdata, i))
			continue;

		sde_smmu = sde_smmu_get_cb(i);
		if (sde_smmu && sde_smmu->dev)
			sde_smmu_enable_power(sde_smmu, false);
	}
	return 0;
}

int sde_smmu_get_domain_id(u32 type)
{
	return type;
}

/*
 * sde_smmu_dma_buf_attach()
 *
 * Same as sde_smmu_dma_buf_attach except that the device is got from
 * the configured smmu v2 context banks.
 */
struct dma_buf_attachment *sde_smmu_dma_buf_attach(
		struct dma_buf *dma_buf, struct device *dev, int domain)
{
	struct sde_smmu_client *sde_smmu = sde_smmu_get_cb(domain);

	if (!sde_smmu) {
		SDEROT_ERR("not able to get smmu context\n");
		return NULL;
	}

	return dma_buf_attach(dma_buf, sde_smmu->dev);
}

/*
 * sde_smmu_map_dma_buf()
 *
 * Maps existing buffer (by struct scatterlist) into SMMU context bank device.
 * From which we can take the virtual address and size allocated.
 * msm_map_dma_buf is depricated with smmu v2 and it uses dma_map_sg instead
 */
int sde_smmu_map_dma_buf(struct dma_buf *dma_buf,
		struct sg_table *table, int domain, dma_addr_t *iova,
		unsigned long *size, int dir)
{
	int rc;
	struct sde_smmu_client *sde_smmu = sde_smmu_get_cb(domain);
#if defined(CONFIG_FB_MSM_MDSS_SAMSUNG)
	int retry_cnt;
#endif

	if (!sde_smmu) {
		SDEROT_ERR("not able to get smmu context\n");
		return -EINVAL;
	}

	rc = msm_dma_map_sg_lazy(sde_smmu->dev, table->sgl, table->nents, dir,
		dma_buf);
#if defined(CONFIG_FB_MSM_MDSS_SAMSUNG)
	if (!in_interrupt()) {
		if (rc != table->nents) {
			for (retry_cnt = 0; retry_cnt < 62 ; retry_cnt++) {
				/* To wait free page by memory reclaim*/
				msleep(16);

				SDEROT_ERR("dma map sg failed : retry (%d)\n", retry_cnt);
				rc = msm_dma_map_sg_lazy(sde_smmu->dev, table->sgl, table->nents, dir,
					dma_buf);

				if (rc == table->nents)
					break;
			}
		}
	}
#endif

	if (rc != table->nents) {
		SDEROT_ERR("dma map sg failed\n");
		return -ENOMEM;
	}

	*iova = table->sgl->dma_address;
	*size = table->sgl->dma_length;
	return 0;
}

void sde_smmu_unmap_dma_buf(struct sg_table *table, int domain,
		int dir, struct dma_buf *dma_buf)
{
	struct sde_smmu_client *sde_smmu = sde_smmu_get_cb(domain);

	if (!sde_smmu) {
		SDEROT_ERR("not able to get smmu context\n");
		return;
	}

	msm_dma_unmap_sg(sde_smmu->dev, table->sgl, table->nents, dir,
		 dma_buf);
}

static DEFINE_MUTEX(sde_smmu_ref_cnt_lock);

int sde_smmu_ctrl(int enable)
{
	struct sde_rot_data_type *mdata = sde_rot_get_mdata();
	int rc = 0;

	mutex_lock(&sde_smmu_ref_cnt_lock);
	SDEROT_DBG("%pS: enable:%d ref_cnt:%d attach:%d\n",
		__builtin_return_address(0), enable, mdata->iommu_ref_cnt,
		mdata->iommu_attached);

	if (enable) {
		if (!mdata->iommu_attached) {
			rc = sde_smmu_attach(mdata);
			if (!rc)
				mdata->iommu_attached = true;
		}
		mdata->iommu_ref_cnt++;
	} else {
		if (mdata->iommu_ref_cnt) {
			mdata->iommu_ref_cnt--;
			if (mdata->iommu_ref_cnt == 0)
				if (mdata->iommu_attached) {
					rc = sde_smmu_detach(mdata);
					if (!rc)
						mdata->iommu_attached = false;
				}
		} else {
			SDEROT_ERR("unbalanced iommu ref\n");
		}
	}
	mutex_unlock(&sde_smmu_ref_cnt_lock);

	if (IS_ERR_VALUE(rc))
		return rc;
	else
		return mdata->iommu_ref_cnt;
}

/*
 * sde_smmu_device_create()
 * @dev: sde_mdp device
 *
 * For smmu, each context bank is a separate child device of sde rot.
 * Platform devices are created for those smmu related child devices of
 * sde rot here. This would facilitate probes to happen for these devices in
 * which the smmu mapping and initialization is handled.
 */
void sde_smmu_device_create(struct device *dev)
{
	struct device_node *parent, *child;

	parent = dev->of_node;
	for_each_child_of_node(parent, child) {
		if (of_device_is_compatible(child, SMMU_SDE_ROT_SEC))
			of_platform_device_create(child, NULL, dev);
		else if (of_device_is_compatible(child, SMMU_SDE_ROT_UNSEC))
			of_platform_device_create(child, NULL, dev);
	}
}

int sde_smmu_init(struct device *dev)
{
	sde_smmu_device_create(dev);

	return 0;
}

static int sde_smmu_fault_handler(struct iommu_domain *domain,
		struct device *dev, unsigned long iova,
		int flags, void *token)
{
	struct sde_smmu_client *sde_smmu;
	int rc = -ENOSYS;

	if (!token) {
		SDEROT_ERR("Error: token is NULL\n");
		return -ENOSYS;
	}

	sde_smmu = (struct sde_smmu_client *)token;

	/* TODO: trigger rotator panic and dump */
	SDEROT_ERR("TODO: trigger rotator panic and dump, iova=0x%08lx\n",
			iova);

	return rc;
}

static struct sde_smmu_domain sde_rot_unsec = {
	"rot_0", SDE_IOMMU_DOMAIN_ROT_UNSECURE, SZ_128K, (SZ_1G - SZ_128K)};
static struct sde_smmu_domain sde_rot_sec = {
	"rot_1", SDE_IOMMU_DOMAIN_ROT_SECURE, SZ_1G, SZ_2G};

static const struct of_device_id sde_smmu_dt_match[] = {
	{ .compatible = SMMU_SDE_ROT_UNSEC, .data = &sde_rot_unsec},
	{ .compatible = SMMU_SDE_ROT_SEC, .data = &sde_rot_sec},
	{}
};
MODULE_DEVICE_TABLE(of, sde_smmu_dt_match);

/*
 * sde_smmu_probe()
 * @pdev: platform device
 *
 * Each smmu context acts as a separate device and the context banks are
 * configured with a VA range.
 * Registeres the clks as each context bank has its own clks, for which voting
 * has to be done everytime before using that context bank.
 */
int sde_smmu_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct sde_rot_data_type *mdata = sde_rot_get_mdata();
	struct sde_smmu_client *sde_smmu;
	int rc = 0;
	struct sde_smmu_domain smmu_domain;
	const struct of_device_id *match;
	struct sde_module_power *mp;
	int disable_htw = 1;
	char name[MAX_CLIENT_NAME_LEN];

	if (!mdata) {
		SDEROT_ERR("probe failed as mdata is not initialized\n");
		return -EPROBE_DEFER;
	}

	match = of_match_device(sde_smmu_dt_match, &pdev->dev);
	if (!match || !match->data) {
		SDEROT_ERR("probe failed as match data is invalid\n");
		return -EINVAL;
	}

	smmu_domain = *(struct sde_smmu_domain *) (match->data);
	if (smmu_domain.domain >= SDE_IOMMU_MAX_DOMAIN) {
		SDEROT_ERR("no matching device found\n");
		return -EINVAL;
	}

	if (of_find_property(pdev->dev.of_node, "iommus", NULL)) {
		dev = &pdev->dev;
	} else {
		SDEROT_ERR("Invalid SMMU ctx for domain:%d\n",
				smmu_domain.domain);
		return -EINVAL;
	}

	sde_smmu = &mdata->sde_smmu[smmu_domain.domain];
	mp = &sde_smmu->mp;
	memset(mp, 0, sizeof(struct sde_module_power));

	if (of_find_property(pdev->dev.of_node,
		"gdsc-mdss-supply", NULL)) {

		mp->vreg_config = devm_kzalloc(&pdev->dev,
			sizeof(struct sde_vreg), GFP_KERNEL);
		if (!mp->vreg_config)
			return -ENOMEM;

		strlcpy(mp->vreg_config->vreg_name, "gdsc-mdss",
				sizeof(mp->vreg_config->vreg_name));
		mp->num_vreg = 1;
	}

	rc = sde_rot_config_vreg(&pdev->dev, mp->vreg_config,
		mp->num_vreg, true);
	if (rc) {
		SDEROT_ERR("vreg config failed rc=%d\n", rc);
		return rc;
	}

	rc = sde_smmu_clk_register(pdev, mp);
	if (rc) {
		SDEROT_ERR(
			"smmu clk register failed for domain[%d] with err:%d\n",
			smmu_domain.domain, rc);
		sde_rot_config_vreg(&pdev->dev, mp->vreg_config, mp->num_vreg,
			false);
		return rc;
	}

	snprintf(name, MAX_CLIENT_NAME_LEN, "smmu:%u", smmu_domain.domain);
	sde_smmu->reg_bus_clt = sde_reg_bus_vote_client_create(name);
	if (IS_ERR_OR_NULL(sde_smmu->reg_bus_clt)) {
		SDEROT_ERR("mdss bus client register failed\n");
		sde_rot_config_vreg(&pdev->dev, mp->vreg_config, mp->num_vreg,
			false);
		return PTR_ERR(sde_smmu->reg_bus_clt);
	}

	rc = sde_smmu_enable_power(sde_smmu, true);
	if (rc) {
		SDEROT_ERR("power enable failed - domain:[%d] rc:%d\n",
			smmu_domain.domain, rc);
		goto bus_client_destroy;
	}

	sde_smmu->mmu_mapping = arm_iommu_create_mapping(
		msm_iommu_get_bus(dev), smmu_domain.start, smmu_domain.size);
	if (IS_ERR(sde_smmu->mmu_mapping)) {
		SDEROT_ERR("iommu create mapping failed for domain[%d]\n",
			smmu_domain.domain);
		rc = PTR_ERR(sde_smmu->mmu_mapping);
		goto disable_power;
	}

	rc = iommu_domain_set_attr(sde_smmu->mmu_mapping->domain,
		DOMAIN_ATTR_COHERENT_HTW_DISABLE, &disable_htw);
	if (rc) {
		SDEROT_ERR("couldn't disable coherent HTW\n");
		goto release_mapping;
	}

	if (smmu_domain.domain == SDE_IOMMU_DOMAIN_ROT_SECURE) {
		int secure_vmid = VMID_CP_PIXEL;

		rc = iommu_domain_set_attr(sde_smmu->mmu_mapping->domain,
			DOMAIN_ATTR_SECURE_VMID, &secure_vmid);
		if (rc) {
			SDEROT_ERR("couldn't set secure pixel vmid\n");
			goto release_mapping;
		}
	}

	iommu_set_fault_handler(sde_smmu->mmu_mapping->domain,
			sde_smmu_fault_handler, (void *)sde_smmu);

	sde_smmu_enable_power(sde_smmu, false);

	sde_smmu->dev = dev;
	SDEROT_INFO(
		"iommu v2 domain[%d] mapping and clk register successful!\n",
			smmu_domain.domain);
	return 0;

release_mapping:
	arm_iommu_release_mapping(sde_smmu->mmu_mapping);
disable_power:
	sde_smmu_enable_power(sde_smmu, false);
bus_client_destroy:
	sde_reg_bus_vote_client_destroy(sde_smmu->reg_bus_clt);
	sde_smmu->reg_bus_clt = NULL;
	sde_rot_config_vreg(&pdev->dev, mp->vreg_config, mp->num_vreg,
			false);
	return rc;
}

int sde_smmu_remove(struct platform_device *pdev)
{
	int i;
	struct sde_smmu_client *sde_smmu;

	for (i = 0; i < SDE_IOMMU_MAX_DOMAIN; i++) {
		sde_smmu = sde_smmu_get_cb(i);
		if (sde_smmu && sde_smmu->dev &&
			(sde_smmu->dev == &pdev->dev))
			arm_iommu_release_mapping(sde_smmu->mmu_mapping);
	}
	return 0;
}

static struct platform_driver sde_smmu_driver = {
	.probe = sde_smmu_probe,
	.remove = sde_smmu_remove,
	.shutdown = NULL,
	.driver = {
		.name = "sde_smmu",
		.of_match_table = sde_smmu_dt_match,
	},
};

static int sde_smmu_register_driver(void)
{
	return platform_driver_register(&sde_smmu_driver);
}

static int __init sde_smmu_driver_init(void)
{
	int ret;

	ret = sde_smmu_register_driver();
	if (ret)
		SDEROT_ERR("sde_smmu_register_driver() failed!\n");

	return ret;
}
module_init(sde_smmu_driver_init);

static void __exit sde_smmu_driver_cleanup(void)
{
	platform_driver_unregister(&sde_smmu_driver);
}
module_exit(sde_smmu_driver_cleanup);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("SDE SMMU driver");
