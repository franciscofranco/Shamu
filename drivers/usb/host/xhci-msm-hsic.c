/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/pm_wakeup.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <linux/dma-mapping.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/clk/msm-clk.h>
#include <linux/reboot.h>

#include <mach/msm_bus.h>
#include <mach/rpm-regulator.h>
#include <mach/msm_iomap.h>
#include <linux/debugfs.h>
#include <asm/unaligned.h>
#include <linux/pinctrl/consumer.h>

#include "xhci.h"

#define MSM_HSIC_BASE			(hcd->regs)

#define MSM_HSIC_PORTSC			(MSM_HSIC_BASE + 0x0420)
#define MSM_HSIC_PORTLI			(MSM_HSIC_BASE + 0x0428)
#define MSM_HSIC_GCTL			(MSM_HSIC_BASE + 0xc110)
#define MSM_HSIC_GUSB2PHYCFG		(MSM_HSIC_BASE + 0xc200)
#define MSM_HSIC_GUSB2PHYACC		(MSM_HSIC_BASE + 0xc280)
#define MSM_HSIC_CTRL_REG		(MSM_HSIC_BASE + 0xf8800)
#define MSM_HSIC_PWR_EVENT_IRQ_STAT	(MSM_HSIC_BASE + 0xf8858)
#define MSM_HSIC_PWR_EVNT_IRQ_MASK	(MSM_HSIC_BASE + 0xf885c)

#define TLMM_GPIO_HSIC_STROBE_PAD_CTL	(MSM_TLMM_BASE + 0x2050)
#define TLMM_GPIO_HSIC_DATA_PAD_CTL	(MSM_TLMM_BASE + 0x2054)

#define GCTL_DSBLCLKGTNG	BIT(0)
#define GCTL_CORESOFTRESET	BIT(11)

/* Global USB2 PHY Configuration Register */
#define GUSB2PHYCFG_PHYSOFTRST	BIT(31)

/* Global USB2 PHY Vendor Control Register */
#define GUSB2PHYACC_NEWREGREQ	BIT(25)
#define GUSB2PHYACC_VSTSDONE	BIT(24)
#define GUSB2PHYACC_VSTSBUSY	BIT(23)
#define GUSB2PHYACC_REGWR	BIT(22)
#define GUSB2PHYACC_REGADDR(n)	(((n) & 0x3F) << 16)
#define GUSB2PHYACC_REGDATA(n)	((n) & 0xFF)

/* QSCRATCH ctrl reg */
#define CTRLREG_PLL_CTRL_SUSP	BIT(31)
#define CTRLREG_PLL_CTRL_SLEEP	BIT(30)

/* HSPHY registers*/
#define MSM_HSIC_CFG		0x30
#define MSM_HSIC_CFG_SET	0x31
#define MSM_HSIC_IO_CAL_PER	0x33

/* PWR_EVENT_IRQ_STAT reg */
#define LPM_IN_L2_IRQ_STAT	BIT(4)
#define LPM_OUT_L2_IRQ_STAT	BIT(5)

/* PWR_EVENT_IRQ_MASK reg */
#define LPM_IN_L2_IRQ_MASK	BIT(4)
#define LPM_OUT_L2_IRQ_MASK	BIT(5)

#define PHY_LPM_WAIT_TIMEOUT_MS	5000
#define ULPI_IO_TIMEOUT_USECS	(10 * 1000)

/*
 * Higher value allows xhci core to moderate interrupts resulting
 * in fewer interrupts from xhci core. This may result in better
 * overall power consumption during peak throughput. Hence set the
 * default HSIC interrupt moderation to 12000 (or 3ms interval)
 */
#define MSM_HSIC_INT_MODERATION 12000

static u64 dma_mask = DMA_BIT_MASK(64);

struct mxhci_hsic_hcd {
	struct xhci_hcd		*xhci;
	spinlock_t		wakeup_lock;
	struct device		*dev;

	struct clk		*core_clk;
	struct clk		*phy_sleep_clk;
	struct clk		*utmi_clk;
	struct clk		*hsic_clk;
	struct clk		*cal_clk;
	struct clk		*system_clk;

	struct regulator	*hsic_vddcx;
	struct regulator	*hsic_gdsc;

	u32			bus_perf_client;
	struct msm_bus_scale_pdata	*bus_scale_table;
	struct work_struct	bus_vote_w;
	bool			bus_vote;
	struct workqueue_struct	*wq;

	bool			wakeup_irq_enabled;
	bool			xhci_remove_flag;
	bool			phy_in_lpm_flag;
	bool			xhci_shutdown_flag;
	bool			port_connect;
	int			strobe;
	int			data;
	int			host_ready;
	int			resume_gpio;
	int			wakeup_irq;
	int			pwr_event_irq;
	unsigned int		vdd_no_vol_level;
	unsigned int		vdd_low_vol_level;
	unsigned int		vdd_high_vol_level;
	unsigned int		in_lpm;
	unsigned int		pm_usage_cnt;
	wait_queue_head_t	phy_in_lpm_wq;

	uint32_t		wakeup_int_cnt;
	uint32_t		pwr_evt_irq_inlpm;
	struct notifier_block   hsic_reboot;
};


#define SYNOPSIS_DWC3_VENDOR	0x5533

static struct dbg_data dbg_hsic = {
	.ctrl_idx = 0,
	.ctrl_lck = __RW_LOCK_UNLOCKED(clck),
	.data_idx = 0,
	.data_lck = __RW_LOCK_UNLOCKED(dlck),
	.log_payload = 0,
	.log_events = 0,
	.inep_log_mask = 0xffff,
	.outep_log_mask = 0xffff
};

static inline void dbg_inc(unsigned *idx)
{
	*idx = (*idx + 1) & (DBG_MAX_MSG-1);
}

/* xhci dbg logging */
module_param_named(enable_payload_log,
			dbg_hsic.log_payload, uint, S_IRUGO | S_IWUSR);
module_param_named(enable_dbg_log,
			dbg_hsic.log_events, uint, S_IRUGO | S_IWUSR);
/* select EPs to log events using this parameter; by default set to ep0 */
module_param_named(ep_addr_rxdbg_mask,
			dbg_hsic.inep_log_mask, uint, S_IRUGO | S_IWUSR);
module_param_named(ep_addr_txdbg_mask,
			dbg_hsic.outep_log_mask, uint, S_IRUGO | S_IWUSR);

static int mxhci_hsic_data_events_show(struct seq_file *s, void *unused)
{
	unsigned long	flags;
	unsigned	i;

	read_lock_irqsave(&dbg_hsic.data_lck, flags);

	i = dbg_hsic.data_idx;
	for (dbg_inc(&i); i != dbg_hsic.data_idx; dbg_inc(&i)) {
		if (!strnlen(dbg_hsic.data_buf[i], DBG_MSG_LEN))
			continue;
		seq_printf(s, "%s\n", dbg_hsic.data_buf[i]);
	}

	read_unlock_irqrestore(&dbg_hsic.data_lck, flags);

	return 0;
}

static int mxhci_hsic_data_events_open(struct inode *inode, struct file *f)
{
	return single_open(f, mxhci_hsic_data_events_show, inode->i_private);
}

const struct file_operations mxhci_hsic_dbg_data_fops = {
	.open = mxhci_hsic_data_events_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mxhci_hsic_ctrl_events_show(struct seq_file *s, void *unused)
{
	unsigned long	flags;
	unsigned	i;

	read_lock_irqsave(&dbg_hsic.ctrl_lck, flags);

	i = dbg_hsic.ctrl_idx;
	for (dbg_inc(&i); i != dbg_hsic.ctrl_idx; dbg_inc(&i)) {
		if (!strnlen(dbg_hsic.ctrl_buf[i], DBG_MSG_LEN))
			continue;
		seq_printf(s, "%s\n", dbg_hsic.ctrl_buf[i]);
	}

	read_unlock_irqrestore(&dbg_hsic.ctrl_lck, flags);

	return 0;
}

static int mxhci_hsic_ctrl_events_open(struct inode *inode, struct file *f)
{
	return single_open(f, mxhci_hsic_ctrl_events_show, inode->i_private);
}

const struct file_operations mxhci_hsic_dbg_ctrl_fops = {
	.open = mxhci_hsic_ctrl_events_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct dentry *xhci_msm_hsic_dbg_dent;
static int mxhci_hsic_debugfs_init(void)
{
	struct dentry *xhci_msm_hsic_dentry;

	xhci_msm_hsic_dbg_dent = debugfs_create_dir("xhci_msm_hsic_dbg", NULL);

	if (!xhci_msm_hsic_dbg_dent || IS_ERR(xhci_msm_hsic_dbg_dent))
		return -ENODEV;

	xhci_msm_hsic_dentry = debugfs_create_file("show_ctrl_events",
						S_IRUGO,
						xhci_msm_hsic_dbg_dent, 0,
						&mxhci_hsic_dbg_ctrl_fops);

	if (!xhci_msm_hsic_dentry) {
		debugfs_remove_recursive(xhci_msm_hsic_dbg_dent);
		return -ENODEV;
	}

	xhci_msm_hsic_dentry = debugfs_create_file("show_data_events",
						S_IRUGO,
						xhci_msm_hsic_dbg_dent, 0,
						&mxhci_hsic_dbg_data_fops);

	if (!xhci_msm_hsic_dentry) {
		debugfs_remove_recursive(xhci_msm_hsic_dbg_dent);
		return -ENODEV;
	}

	return 0;
}

static void mxhci_hsic_debugfs_cleanup(void)
{
	debugfs_remove_recursive(xhci_msm_hsic_dbg_dent);
}

static void xhci_hsic_log_urb(struct urb *urb, char *event, unsigned extra)
{
	xhci_dbg_log_event(&dbg_hsic, urb, event, extra);
}

static inline struct mxhci_hsic_hcd *hcd_to_hsic(struct usb_hcd *hcd)
{
	return (struct mxhci_hsic_hcd *) (hcd->hcd_priv);
}

static inline struct usb_hcd *hsic_to_hcd(struct mxhci_hsic_hcd *mxhci)
{
	return container_of((void *) mxhci, struct usb_hcd, hcd_priv);
}

static void mxhci_hsic_bus_vote_w(struct work_struct *w)
{
	struct mxhci_hsic_hcd *mxhci =
			container_of(w, struct mxhci_hsic_hcd, bus_vote_w);
	int ret;

	ret = msm_bus_scale_client_update_request(mxhci->bus_perf_client,
			mxhci->bus_vote);
	if (ret)
		dev_err(mxhci->dev, "%s: Failed to vote for bus bandwidth %d\n",
				__func__, ret);
}

static int mxhci_hsic_reboot(struct notifier_block *nb,
			unsigned long event, void *unused)
{
	struct mxhci_hsic_hcd *mxhci =
			container_of(nb, struct mxhci_hsic_hcd, hsic_reboot);
	struct usb_hcd *hcd = hsic_to_hcd(mxhci);
	u32 reg;

	dev_dbg(mxhci->dev, "Disabling HSIC\n");
	disable_irq(hcd->irq);
	if (mxhci->wakeup_irq_enabled) {
		disable_irq_wake(mxhci->wakeup_irq);
		disable_irq_nosync(mxhci->wakeup_irq);
		mxhci->wakeup_irq_enabled = 0;
	}
	/* disable STROBE_PAD_CTL */
	reg = readl_relaxed(TLMM_GPIO_HSIC_STROBE_PAD_CTL);
	writel_relaxed(reg & 0xfdffffff, TLMM_GPIO_HSIC_STROBE_PAD_CTL);

	/* disable DATA_PAD_CTL */
	reg = readl_relaxed(TLMM_GPIO_HSIC_DATA_PAD_CTL);
	writel_relaxed(reg & 0xfdffffff, TLMM_GPIO_HSIC_DATA_PAD_CTL);

	mb();
	mxhci->xhci_shutdown_flag = true;
	wake_up(&mxhci->phy_in_lpm_wq);
	return NOTIFY_DONE;
}

static int mxhci_hsic_init_clocks(struct mxhci_hsic_hcd *mxhci, u32 init)
{
	int ret = 0;

	if (!init)
		goto disable_all_clks;

	/* 75Mhz system_clk required for normal hsic operation */
	mxhci->system_clk = devm_clk_get(mxhci->dev, "system_clk");
	if (IS_ERR(mxhci->system_clk)) {
		dev_err(mxhci->dev, "failed to get system_clk\n");
		ret = PTR_ERR(mxhci->system_clk);
		goto out;
	}
	clk_set_rate(mxhci->system_clk, 75000000);

	/* 60Mhz core_clk required for LINK protocol engine */
	mxhci->core_clk = devm_clk_get(mxhci->dev, "core_clk");
	if (IS_ERR(mxhci->core_clk)) {
		dev_err(mxhci->dev, "failed to get core_clk\n");
		ret = PTR_ERR(mxhci->core_clk);
		goto out;
	}
	clk_set_rate(mxhci->core_clk, 60000000);

	/* 480Mhz main HSIC phy clk */
	mxhci->hsic_clk = devm_clk_get(mxhci->dev, "hsic_clk");
	if (IS_ERR(mxhci->hsic_clk)) {
		dev_err(mxhci->dev, "failed to get hsic_clk\n");
		ret = PTR_ERR(mxhci->hsic_clk);
		goto out;
	}
	clk_set_rate(mxhci->hsic_clk, 480000000);

	/* 19.2Mhz utmi_clk ref_clk required to shut off HSIC PLL */
	mxhci->utmi_clk = devm_clk_get(mxhci->dev, "utmi_clk");
	if (IS_ERR(mxhci->utmi_clk)) {
		dev_err(mxhci->dev, "failed to get utmi_clk\n");
		ret = PTR_ERR(mxhci->utmi_clk);
		goto out;
	}
	clk_set_rate(mxhci->utmi_clk, 19200000);

	/* 32Khz phy sleep clk */
	mxhci->phy_sleep_clk = devm_clk_get(mxhci->dev, "phy_sleep_clk");
	if (IS_ERR(mxhci->phy_sleep_clk)) {
		dev_err(mxhci->dev, "failed to get phy_sleep_clk\n");
		ret = PTR_ERR(mxhci->phy_sleep_clk);
		goto out;
	}
	clk_set_rate(mxhci->phy_sleep_clk, 32000);

	/* 10MHz cal_clk required for calibration of I/O pads */
	mxhci->cal_clk = devm_clk_get(mxhci->dev, "cal_clk");
	if (IS_ERR(mxhci->cal_clk)) {
		dev_err(mxhci->dev, "failed to get cal_clk\n");
		ret = PTR_ERR(mxhci->cal_clk);
		goto out;
	}
	clk_set_rate(mxhci->cal_clk, 9600000);

	ret = clk_prepare_enable(mxhci->system_clk);
	if (ret) {
		dev_err(mxhci->dev, "failed to enable system_clk\n");
		goto out;
	}

	/* enable force-on mode for periph_on */
	clk_set_flags(mxhci->system_clk, CLKFLAG_RETAIN_PERIPH);

	ret = clk_prepare_enable(mxhci->core_clk);
	if (ret) {
		dev_err(mxhci->dev, "failed to enable core_clk\n");
		goto err_core_clk;
	}

	ret = clk_prepare_enable(mxhci->hsic_clk);
	if (ret) {
		dev_err(mxhci->dev, "failed to enable hsic_clk\n");
		goto err_hsic_clk;
	}

	ret = clk_prepare_enable(mxhci->utmi_clk);
	if (ret) {
		dev_err(mxhci->dev, "failed to enable utmi_clk\n");
		goto err_utmi_clk;
	}

	ret = clk_prepare_enable(mxhci->cal_clk);
	if (ret) {
		dev_err(mxhci->dev, "failed to enable cal_clk\n");
		goto err_cal_clk;
	}

	ret = clk_prepare_enable(mxhci->phy_sleep_clk);
	if (ret) {
		dev_err(mxhci->dev, "failed to enable phy_sleep_clk\n");
		goto err_phy_sleep_clk;
	}

	return 0;

disable_all_clks:
	clk_disable_unprepare(mxhci->phy_sleep_clk);
	if (mxhci->in_lpm)
		goto out;
err_phy_sleep_clk:
	clk_disable_unprepare(mxhci->cal_clk);
err_cal_clk:
	clk_disable_unprepare(mxhci->utmi_clk);
err_utmi_clk:
	clk_disable_unprepare(mxhci->hsic_clk);
err_hsic_clk:
	clk_disable_unprepare(mxhci->core_clk);
err_core_clk:
	clk_disable_unprepare(mxhci->system_clk);
out:
	return ret;
}

static int mxhci_hsic_init_vddcx(struct mxhci_hsic_hcd *mxhci, int init)
{
	int ret = 0;

	if (!init)
		goto disable_reg;

	if (!mxhci->hsic_vddcx) {
		mxhci->hsic_vddcx = devm_regulator_get(mxhci->dev,
			"hsic-vdd-dig");
		if (IS_ERR(mxhci->hsic_vddcx)) {
			dev_err(mxhci->dev, "unable to get hsic vddcx\n");
			ret = PTR_ERR(mxhci->hsic_vddcx);
			goto out;
		}
	}

	ret = regulator_set_voltage(mxhci->hsic_vddcx, mxhci->vdd_low_vol_level,
			mxhci->vdd_high_vol_level);
	if (ret) {
		dev_err(mxhci->dev,
				"unable to set the voltage for hsic vddcx\n");
		goto out;
	}

	ret = regulator_enable(mxhci->hsic_vddcx);
	if (ret) {
		dev_err(mxhci->dev, "unable to enable hsic vddcx\n");
		goto reg_enable_err;
	}

	return 0;

disable_reg:
	regulator_disable(mxhci->hsic_vddcx);
reg_enable_err:
	regulator_set_voltage(mxhci->hsic_vddcx, mxhci->vdd_no_vol_level,
			mxhci->vdd_high_vol_level);

out:
	return ret;
}

/*
 * Config Global Distributed Switch Controller (GDSC)
 * to turn on/off HSIC controller
 */
static int mxhci_msm_config_gdsc(struct mxhci_hsic_hcd *mxhci, int on)
{
	int ret = 0;

	if (!mxhci->hsic_gdsc) {
		mxhci->hsic_gdsc = devm_regulator_get(mxhci->dev, "hsic-gdsc");
			if (IS_ERR(mxhci->hsic_gdsc))
				return PTR_ERR(mxhci->hsic_gdsc);
	}

	if (on) {
		ret = regulator_enable(mxhci->hsic_gdsc);
		if (ret) {
			dev_err(mxhci->dev, "unable to enable hsic gdsc\n");
			return ret;
		}
	} else {
		regulator_disable(mxhci->hsic_gdsc);
	}

	return 0;
}

static int mxhci_hsic_config_gpios(struct mxhci_hsic_hcd *mxhci)
{
	int rc = 0;
	struct pinctrl *pinctrl;

	pinctrl = devm_pinctrl_get_select(mxhci->dev, "active");
	if (IS_ERR(pinctrl)) {
		rc = PTR_ERR(pinctrl);
		dev_err(mxhci->dev, "pinctrl failed err %d\n", rc);
		return rc;
	}

	rc = devm_gpio_request(mxhci->dev, mxhci->strobe, "HSIC_STROBE_GPIO");
	if (rc < 0) {
		dev_err(mxhci->dev, "gpio request failed for HSIC STROBE\n");
		goto out;
	}

	rc = devm_gpio_request(mxhci->dev, mxhci->data, "HSIC_DATA_GPIO");
	if (rc < 0) {
		dev_err(mxhci->dev, "gpio request failed for HSIC DATA\n");
		goto out;
	}

	if (mxhci->host_ready) {
		rc = devm_gpio_request(mxhci->dev,
				mxhci->host_ready, "host_ready");
		if (rc < 0) {
			dev_err(mxhci->dev,
				"gpio request failed host ready gpio\n");
			mxhci->host_ready = 0;
			rc = 0;
		}
	}

	if (mxhci->resume_gpio) {
		rc = devm_gpio_request(mxhci->dev,
				mxhci->resume_gpio, "HSIC_RESUME_GPIO");
		if (rc < 0) {
			dev_err(mxhci->dev,
				"gpio request failed for resume gpio\n");
			mxhci->resume_gpio = 0;
			rc = 0;
		}
	}

out:
	return rc;
}

static int mxhci_hsic_ulpi_write(struct mxhci_hsic_hcd *mxhci, u32 val,
		u32 reg)
{
	struct usb_hcd *hcd = hsic_to_hcd(mxhci);
	unsigned long timeout;

	/* set the reg write request and perfom ULPI phy reg write */
	writel_relaxed(GUSB2PHYACC_NEWREGREQ | GUSB2PHYACC_REGWR
		| GUSB2PHYACC_REGADDR(reg) | GUSB2PHYACC_REGDATA(val),
		MSM_HSIC_GUSB2PHYACC);

	/* poll for write done */
	timeout = jiffies + usecs_to_jiffies(ULPI_IO_TIMEOUT_USECS);
	while (!(readl_relaxed(MSM_HSIC_GUSB2PHYACC) & GUSB2PHYACC_VSTSDONE)) {
		if (time_after(jiffies, timeout)) {
			dev_err(mxhci->dev, "mxhci_hsic_ulpi_write: timeout\n");
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	return 0;
}

static void mxhci_hsic_reset(struct mxhci_hsic_hcd *mxhci)
{
	u32 reg;
	int ret;
	struct usb_hcd *hcd = hsic_to_hcd(mxhci);

	/* start controller reset */
	reg = readl_relaxed(MSM_HSIC_GCTL);
	reg |= GCTL_CORESOFTRESET;
	writel_relaxed(reg, MSM_HSIC_GCTL);

	usleep(1000);

	/* phy reset using asynchronous block reset */

	clk_disable_unprepare(mxhci->cal_clk);
	clk_disable_unprepare(mxhci->utmi_clk);
	clk_disable_unprepare(mxhci->hsic_clk);
	clk_disable_unprepare(mxhci->core_clk);
	clk_disable_unprepare(mxhci->system_clk);
	clk_disable_unprepare(mxhci->phy_sleep_clk);

	ret = clk_reset(mxhci->hsic_clk, CLK_RESET_ASSERT);
	if (ret) {
		dev_err(mxhci->dev, "hsic clk assert failed:%d\n", ret);
		return;
	}
	usleep_range(10000, 12000);

	ret = clk_reset(mxhci->hsic_clk, CLK_RESET_DEASSERT);
	if (ret)
		dev_err(mxhci->dev, "hsic clk deassert failed:%d\n",
				ret);
	/*
	 * Required delay between the deassertion and
	 *	clock enablement.
	*/
	ndelay(200);
	clk_prepare_enable(mxhci->phy_sleep_clk);
	clk_prepare_enable(mxhci->system_clk);
	clk_prepare_enable(mxhci->core_clk);
	clk_prepare_enable(mxhci->hsic_clk);
	clk_prepare_enable(mxhci->utmi_clk);
	clk_prepare_enable(mxhci->cal_clk);

	/* After PHY is stable we can take Core out of reset state */
	reg = readl_relaxed(MSM_HSIC_GCTL);
	reg &= ~GCTL_CORESOFTRESET;
	writel_relaxed(reg, MSM_HSIC_GCTL);

	usleep(1000);
}

static void mxhci_hsic_plat_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	struct xhci_plat_data *pdata = dev->platform_data;
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(xhci_to_hcd(xhci));

	/*
	 * As of now platform drivers don't provide MSI support so we ensure
	 * here that the generic code does not try to make a pci_dev from our
	 * dev struct in order to setup MSI
	 */
	xhci->quirks |= XHCI_PLAT;

	/* Single port controller using out of band remote wakeup */
	if (mxhci->wakeup_irq)
		xhci->quirks |= XHCI_NO_SELECTIVE_SUSPEND;

	/*
	 * Observing hw tr deq pointer getting stuck to a noop trb
	 * when aborting transfer during suspend. Reset tr deq pointer
	 * to start of the first seg of the xfer ring.
	 */
	xhci->quirks |= XHCI_TR_DEQ_RESET_QUIRK;

	if (!pdata)
		return;
	if (pdata->vendor == SYNOPSIS_DWC3_VENDOR &&
			pdata->revision < 0x230A)
		xhci->quirks |= XHCI_PORTSC_DELAY;
}

/* called during probe() after chip reset completes */
static int mxhci_hsic_plat_setup(struct usb_hcd *hcd)
{
	return xhci_gen_setup(hcd, mxhci_hsic_plat_quirks);
}

static irqreturn_t mxhci_hsic_wakeup_irq(int irq, void *data)
{
	struct mxhci_hsic_hcd *mxhci = data;
	int ret;

	mxhci->wakeup_int_cnt++;
	dev_dbg(mxhci->dev, "%s: remote wakeup interrupt cnt: %u\n",
			__func__, mxhci->wakeup_int_cnt);
	xhci_dbg_log_event(&dbg_hsic, NULL, "Remote Wakeup IRQ",
			mxhci->wakeup_int_cnt);

	pm_stay_awake(mxhci->dev);

	spin_lock(&mxhci->wakeup_lock);
	if (mxhci->wakeup_irq_enabled) {
		mxhci->wakeup_irq_enabled = 0;
		disable_irq_wake(irq);
		disable_irq_nosync(irq);
	}

	if (!mxhci->pm_usage_cnt) {
		ret = pm_runtime_get(mxhci->dev);
		/*
		 * HSIC runtime resume can race with us.
		 * if we are active (ret == 1) or resuming
		 * (ret == -EINPROGRESS), decrement the
		 * PM usage counter before returning.
		 */
		if ((ret == 1) || (ret == -EINPROGRESS))
			pm_runtime_put_noidle(mxhci->dev);
		else
			mxhci->pm_usage_cnt = 1;
	}
	spin_unlock(&mxhci->wakeup_lock);

	return IRQ_HANDLED;
}

static irqreturn_t mxhci_hsic_pwr_event_irq(int irq, void *data)
{
	struct mxhci_hsic_hcd *mxhci = data;
	struct usb_hcd *hcd = hsic_to_hcd(mxhci);
	u32 stat = 0;
	bool in_lpm = mxhci->in_lpm;

	if (in_lpm) {
		clk_prepare_enable(mxhci->core_clk);
		xhci_dbg_log_event(&dbg_hsic, NULL,
				"PWR EVT IRQ IN LPM",
				in_lpm);
		mxhci->pwr_evt_irq_inlpm++;
	}

	stat = readl_relaxed(MSM_HSIC_PWR_EVENT_IRQ_STAT);
	if (stat & LPM_IN_L2_IRQ_STAT) {
		xhci_dbg_log_event(&dbg_hsic, NULL, "LPM_IN_L2_IRQ", stat);
		writel_relaxed(stat, MSM_HSIC_PWR_EVENT_IRQ_STAT);

		/* Ensure irq is acked before turning off clks for lpm */
		mb();

		/* this can be spurious interrupt if in_lpm is true */
		if (!in_lpm) {
			mxhci->phy_in_lpm_flag = true;
			wake_up(&mxhci->phy_in_lpm_wq);
		}

	} else if (stat & LPM_OUT_L2_IRQ_STAT) {
		xhci_dbg_log_event(&dbg_hsic, NULL, "LPM_OUT_L2_IRQ", stat);
		writel_relaxed(stat, MSM_HSIC_PWR_EVENT_IRQ_STAT);

		/* ensure to ack the OUT_L2_IRQ */
		mb();
	} else {
		xhci_dbg_log_event(&dbg_hsic, NULL, "spurious pwr evt irq",
				stat);
		dev_info(mxhci->dev,
			"%s: spurious interrupt.pwr_event_irq stat = %x\n",
			__func__, stat);
	}

	if (in_lpm)
		clk_disable_unprepare(mxhci->core_clk);

	return IRQ_HANDLED;
}

static int mxhci_hsic_bus_suspend(struct usb_hcd *hcd)
{
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd->primary_hcd);
	int ret;
	u32 stat = 0;

	if (!usb_hcd_is_primary_hcd(hcd))
		return 0;

	/* don't miss connect bus state from peripheral for USB 2.0 root hub */
	if (!(readl_relaxed(MSM_HSIC_PORTSC) & PORT_PE)) {
		xhci_dbg_log_event(&dbg_hsic, NULL,
				"port is not enabled; skip suspend", 0);
		dev_dbg(mxhci->dev, "%s: port is not enabled; skip suspend\n",
				__func__);
		return -EAGAIN;
	}

	xhci_dbg_log_event(&dbg_hsic, NULL, "mxhci_hsic_bus_suspend", 0);

	mxhci->phy_in_lpm_flag = false;

	ret = xhci_bus_suspend(hcd);
	if (ret)
		return ret;

	/* make sure HSIC phy is in LPM */
	ret = wait_event_interruptible_timeout(mxhci->phy_in_lpm_wq,
			(mxhci->phy_in_lpm_flag == true) ||
			(mxhci->xhci_remove_flag == true) ||
			(mxhci->xhci_shutdown_flag == true),
			msecs_to_jiffies(PHY_LPM_WAIT_TIMEOUT_MS));

	if (!ret) {
		stat = readl_relaxed(MSM_HSIC_PWR_EVENT_IRQ_STAT);
		dev_dbg(mxhci->dev, "IN_L2_IRQ timeout\n");
		xhci_dbg_log_event(&dbg_hsic, NULL, "IN_L2_IRQ timeout",
			stat);
		xhci_dbg_log_event(&dbg_hsic, NULL, "PORTSC",
				readl_relaxed(MSM_HSIC_PORTSC));
		xhci_dbg_log_event(&dbg_hsic, NULL, "PORTLI",
				readl_relaxed(MSM_HSIC_PORTLI));
		if (stat & LPM_IN_L2_IRQ_STAT) {
			xhci_dbg_log_event(&dbg_hsic, NULL,
				"MISSING IN_L2_IRQ_EVENT", stat);
			/*clear STAT bit*/
			writel_relaxed(stat, MSM_HSIC_PWR_EVENT_IRQ_STAT);
			mb();
		} else if (!(readl_relaxed(MSM_HSIC_PORTSC) & PORT_PE)) {
			xhci_dbg_log_event(&dbg_hsic, NULL,
				"Port is not enabled", 0);
			return -EBUSY;
		} else {
			panic("IN_L2 power event irq timedout");
		}
	}

	xhci_dbg_log_event(&dbg_hsic, NULL, "Suspend RH",
			readl_relaxed(MSM_HSIC_PORTSC));
	xhci_dbg_log_event(&dbg_hsic, NULL, "IN_L2_IRQ_STAT",
			readl_relaxed(MSM_HSIC_PWR_EVENT_IRQ_STAT));
	return 0;
}

static int mxhci_hsic_bus_resume(struct usb_hcd *hcd)
{
	int ret;
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd->primary_hcd);
	struct xhci_bus_state *bus_state;

	if (!usb_hcd_is_primary_hcd(hcd))
		return 0;

	if (mxhci->resume_gpio) {
		bus_state = &mxhci->xhci->bus_state[hcd_index(hcd)];
		if (time_before_eq(jiffies, bus_state->next_statechange))
			usleep_range(10000, 11000);

		xhci_dbg_log_event(&dbg_hsic, NULL, "resume gpio high",
				readl_relaxed(MSM_HSIC_PORTSC));
		gpio_direction_output(mxhci->resume_gpio, 1);

		usleep_range(9000, 10000);
	}

	ret = xhci_bus_resume(hcd);

	xhci_dbg_log_event(&dbg_hsic, NULL, "Resume RH",
			readl_relaxed(MSM_HSIC_PORTSC));

	if (mxhci->resume_gpio) {
		xhci_dbg_log_event(&dbg_hsic, NULL, "resume gpio low",
				readl_relaxed(MSM_HSIC_PORTSC));
		gpio_direction_output(mxhci->resume_gpio, 0);
	}

	return ret;
}

static int mxhci_hsic_suspend(struct mxhci_hsic_hcd *mxhci)
{
	struct usb_hcd *hcd = hsic_to_hcd(mxhci);
	int ret;

	if (mxhci->in_lpm) {
		dev_dbg(mxhci->dev, "%s called in lpm\n", __func__);
		return 0;
	}

	disable_irq(hcd->irq);
	disable_irq(mxhci->pwr_event_irq);

	/* make sure we don't race against a remote wakeup */
	if (test_bit(HCD_FLAG_WAKEUP_PENDING, &hcd->flags) ||
	    (readl_relaxed(MSM_HSIC_PORTSC) & PORT_PLS_MASK) == XDEV_RESUME) {
		dev_dbg(mxhci->dev, "wakeup pending, aborting suspend\n");
		enable_irq(mxhci->pwr_event_irq);
		enable_irq(hcd->irq);
		return -EBUSY;
	}

	xhci_dbg_log_event(&dbg_hsic, NULL, "Read PWR_EVENT_IRQ_STAT",
			readl_relaxed(MSM_HSIC_PWR_EVENT_IRQ_STAT));

	/* Don't poll the roothubs after bus suspend. */
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	del_timer_sync(&hcd->rh_timer);

	clk_disable_unprepare(mxhci->core_clk);
	clk_disable_unprepare(mxhci->utmi_clk);
	clk_disable_unprepare(mxhci->hsic_clk);
	clk_disable_unprepare(mxhci->cal_clk);
	clk_disable_unprepare(mxhci->system_clk);

	ret = regulator_set_voltage(mxhci->hsic_vddcx, mxhci->vdd_no_vol_level,
			mxhci->vdd_high_vol_level);
	if (ret < 0)
		dev_err(mxhci->dev, "unable to set vddcx voltage for VDD MIN\n");

	if (mxhci->bus_perf_client) {
		mxhci->bus_vote = false;
		queue_work(mxhci->wq, &mxhci->bus_vote_w);
	}

	mxhci->in_lpm = 1;

	enable_irq(mxhci->pwr_event_irq);
	enable_irq(hcd->irq);

	if (mxhci->wakeup_irq) {
		mxhci->wakeup_irq_enabled = 1;
		enable_irq_wake(mxhci->wakeup_irq);
		enable_irq(mxhci->wakeup_irq);
	}

	/* disable force-on mode for periph_on */
	clk_set_flags(mxhci->system_clk, CLKFLAG_NORETAIN_PERIPH);

	pm_relax(mxhci->dev);

	dev_dbg(mxhci->dev, "HSIC-USB in low power mode\n");
	xhci_dbg_log_event(&dbg_hsic, NULL, "Controller suspended", 0);

	return 0;
}

static int mxhci_hsic_resume(struct mxhci_hsic_hcd *mxhci)
{
	struct usb_hcd *hcd = hsic_to_hcd(mxhci);
	int ret;
	unsigned long flags;

	if (!mxhci->in_lpm) {
		dev_dbg(mxhci->dev, "%s called in !in_lpm\n", __func__);
		return 0;
	}

	pm_stay_awake(mxhci->dev);

	/* enable force-on mode for periph_on */
	clk_set_flags(mxhci->system_clk, CLKFLAG_RETAIN_PERIPH);

	if (mxhci->bus_perf_client) {
		mxhci->bus_vote = true;
		queue_work(mxhci->wq, &mxhci->bus_vote_w);
	}

	spin_lock_irqsave(&mxhci->wakeup_lock, flags);
	if (mxhci->wakeup_irq_enabled) {
		disable_irq_wake(mxhci->wakeup_irq);
		disable_irq_nosync(mxhci->wakeup_irq);
		mxhci->wakeup_irq_enabled = 0;
	}

	if (mxhci->pm_usage_cnt) {
		mxhci->pm_usage_cnt = 0;
		pm_runtime_put_noidle(mxhci->dev);
	}
	spin_unlock_irqrestore(&mxhci->wakeup_lock, flags);


	ret = regulator_set_voltage(mxhci->hsic_vddcx, mxhci->vdd_low_vol_level,
			mxhci->vdd_high_vol_level);
	if (ret < 0)
		dev_err(mxhci->dev,
			"unable to set nominal vddcx voltage (no VDD MIN)\n");


	clk_prepare_enable(mxhci->system_clk);
	clk_prepare_enable(mxhci->cal_clk);
	clk_prepare_enable(mxhci->hsic_clk);
	clk_prepare_enable(mxhci->utmi_clk);
	clk_prepare_enable(mxhci->core_clk);

	/* Re-enable port polling. */
	set_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	usb_hcd_poll_rh_status(hcd);

	if (mxhci->wakeup_irq)
		usb_hcd_resume_root_hub(hcd);

	mxhci->in_lpm = 0;

	dev_dbg(mxhci->dev, "HSIC-USB exited from low power mode\n");
	xhci_dbg_log_event(&dbg_hsic, NULL, "Controller resumed", 0);

	return 0;
}

static void mxhci_hsic_set_autosuspend_delay(struct usb_device *dev)
{
	if (!dev->parent) /*for root hub no delay*/
		pm_runtime_set_autosuspend_delay(&dev->dev, 0);
	else
		pm_runtime_set_autosuspend_delay(&dev->dev, 200);
}

void mxhci_hsic_shutdown(struct usb_hcd *hcd)
{
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd->primary_hcd);

	if (!usb_hcd_is_primary_hcd(hcd))
		return;

	xhci_dbg_log_event(&dbg_hsic, NULL,  "mxhci_hsic_shutdown", 0);
	mxhci->xhci_shutdown_flag = true;
	wake_up(&mxhci->phy_in_lpm_wq);
	if (!mxhci->in_lpm)
		xhci_shutdown(hcd);

}

int mxhci_hsic_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
		u16 wIndex, char *buf, u16 wLength)
{
	struct mxhci_hsic_hcd *mxhci;
	int ret = 0;
	u32 status;

	ret = xhci_hub_control(hcd, typeReq, wValue, wIndex, buf, wLength);
	if (!hcd->primary_hcd)
		return ret;

	mxhci = hcd_to_hsic(hcd->primary_hcd);

	if (!hcd->primary_hcd)
		return ret;

	mxhci = hcd_to_hsic(hcd->primary_hcd);
	status = get_unaligned_le32(buf);

	if (typeReq == GetPortStatus) {
		if (mxhci->port_connect) {
			if (status & ((USB_PORT_STAT_C_CONNECTION << 16) |
					(USB_PORT_STAT_C_ENABLE << 16))) {
				xhci_dbg_log_event(&dbg_hsic, NULL,
						"spurious port change", status);
				return -ENODEV;
			}
		} else if (status & (USB_PORT_STAT_C_CONNECTION << 16)) {
			xhci_dbg_log_event(&dbg_hsic, NULL,  "port connect",
					status);
			mxhci->port_connect = true;
		}
	}
	return ret;
}

void mxhci_hsic_udev_enum_done(struct usb_hcd *hcd)
{
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd->primary_hcd);

	if (mxhci->host_ready) {
		/* after device enum lower host ready gpio */
		gpio_direction_output(mxhci->host_ready, 0);
		xhci_dbg_log_event(&dbg_hsic, NULL,  "host ready set low",
					gpio_get_value(mxhci->host_ready));
	}
}

/*
 * When stop ep command times out due to controller halt failure
 * no point waiting till XHCI_STOP_EP_CMD_TIMEOUT to giveback urbs.
 * Kick stop ep command watchdog to finish endpoint related cleanup
 * as early as possible.
 */
static void mxhci_hsic_ep_cleanup(struct usb_hcd *hcd)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	struct xhci_virt_ep *temp_ep;
	int i, j;
	unsigned long flags;
	int locked;
	bool kick_wdog = false;

	locked = spin_trylock_irqsave(&xhci->lock, flags);
	if (xhci->xhc_state & XHCI_STATE_DYING)
		goto unlock;

	for (i = 0; i < MAX_HC_SLOTS; i++) {
		if (!xhci->devs[i])
			continue;
		for (j = 0; j < 31; j++) {
			temp_ep = &xhci->devs[i]->eps[j];
			/* find first ep with pending stop ep cmd */
			if (temp_ep->stop_cmds_pending) {
				kick_wdog = true;
				/* kick stop ep cmd watchdog asap */
				mod_timer(&temp_ep->stop_cmd_timer, jiffies);
				goto unlock;
			}
		}
	}
unlock:
	/*
	 * if no stop ep cmd pending set xhci state to halted so that
	 * xhci_urb_dequeue() gives back urb right away.
	 */
	if (!kick_wdog)
		xhci->xhc_state |= XHCI_STATE_HALTED;
	if (locked)
		spin_unlock_irqrestore(&xhci->lock, flags);
}

static struct hc_driver mxhci_hsic_hc_driver = {
	.description =		"xhci-hcd",
	.product_desc =		"Qualcomm xHCI Host Controller using HSIC",

	/*
	 * generic hardware linkage
	 */
	.irq =			xhci_irq,
	.flags =		HCD_MEMORY | HCD_USB3,

	/*
	 * basic lifecycle operations
	 */
	.reset =		mxhci_hsic_plat_setup,
	.start =		xhci_run,
	.stop =			xhci_stop,
	.shutdown =		mxhci_hsic_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue =		xhci_urb_enqueue,
	.urb_dequeue =		xhci_urb_dequeue,
	.alloc_dev =		xhci_alloc_dev,
	.free_dev =		xhci_free_dev,
	.alloc_streams =	xhci_alloc_streams,
	.free_streams =		xhci_free_streams,
	.add_endpoint =		xhci_add_endpoint,
	.drop_endpoint =	xhci_drop_endpoint,
	.endpoint_reset =	xhci_endpoint_reset,
	.check_bandwidth =	xhci_check_bandwidth,
	.reset_bandwidth =	xhci_reset_bandwidth,
	.address_device =	xhci_address_device,
	.update_hub_device =	xhci_update_hub_device,
	.reset_device =		xhci_discover_or_reset_device,
	.halt_failed_cleanup =	mxhci_hsic_ep_cleanup,

	/*
	 * scheduling support
	 */
	.get_frame_number =	xhci_get_frame,

	/* Root hub support */
	.hub_control =		mxhci_hsic_hub_control,
	.hub_status_data =	xhci_hub_status_data,
	.bus_suspend =		mxhci_hsic_bus_suspend,
	.bus_resume =		mxhci_hsic_bus_resume,

	/* dbg log support */
	.log_urb =		xhci_hsic_log_urb,

	.set_autosuspend_delay = mxhci_hsic_set_autosuspend_delay,
	.udev_enum_done =	mxhci_hsic_udev_enum_done,
};

static ssize_t config_imod_store(struct device *pdev,
		struct device_attribute *attr, const char *buff, size_t size)
{
	struct usb_hcd *hcd = dev_get_drvdata(pdev);
	struct xhci_hcd *xhci;
	struct mxhci_hsic_hcd *mxhci;
	u32 temp;
	u32 imod;
	unsigned long flags;

	sscanf(buff, "%u", &imod);
	imod &= ER_IRQ_INTERVAL_MASK;

	mxhci = hcd_to_hsic(hcd);
	xhci = hcd_to_xhci(hcd);

	if (mxhci->in_lpm)
		return -EACCES;

	spin_lock_irqsave(&xhci->lock, flags);
	temp = xhci_readl(xhci, &xhci->ir_set->irq_control);
	temp &= ~ER_IRQ_INTERVAL_MASK;
	temp |= imod;
	xhci_writel(xhci, temp, &xhci->ir_set->irq_control);
	spin_unlock_irqrestore(&xhci->lock, flags);

	return size;
}

static ssize_t config_imod_show(struct device *pdev,
		struct device_attribute *attr, char *buff)
{
	struct usb_hcd *hcd = dev_get_drvdata(pdev);
	struct xhci_hcd *xhci;
	struct mxhci_hsic_hcd *mxhci;
	u32 temp;
	unsigned long flags;

	mxhci = hcd_to_hsic(hcd);
	xhci = hcd_to_xhci(hcd);

	if (mxhci->in_lpm)
		return -EACCES;

	spin_lock_irqsave(&xhci->lock, flags);
	temp = xhci_readl(xhci, &xhci->ir_set->irq_control) &
			ER_IRQ_INTERVAL_MASK;
	spin_unlock_irqrestore(&xhci->lock, flags);

	return snprintf(buff, PAGE_SIZE, "%08x\n", temp);
}

static DEVICE_ATTR(config_imod, S_IRUGO | S_IWUSR,
		config_imod_show, config_imod_store);

static ssize_t host_ready_store(struct device *pdev,
			struct device_attribute *attr,
			const char *buff, size_t size)
{
	int assert;
	struct usb_hcd *hcd = dev_get_drvdata(pdev);
	struct mxhci_hsic_hcd *mxhci;

	sscanf(buff, "%d", &assert);
	assert = !!assert;

	if (!hcd) {
		pr_err("%s: hsic: null hcd\n", __func__);
		return -ENODEV;
	}

	dev_dbg(pdev, "assert: %d\n", assert);

	mxhci = hcd_to_hsic(hcd);
	if (mxhci->host_ready)
		gpio_direction_output(mxhci->host_ready, assert);
	else
		return -ENODEV;

	return size;
}

static ssize_t host_ready_show(struct device *pdev,
			struct device_attribute *attr, char *buff)
{
	struct usb_hcd *hcd = dev_get_drvdata(pdev);
	struct mxhci_hsic_hcd *mxhci;
	int val = -ENODEV;

	if (!hcd) {
		pr_err("%s: hsic: null hcd\n", __func__);
		return -ENODEV;
	}

	mxhci = hcd_to_hsic(hcd);

	if (mxhci->host_ready)
		val = gpio_get_value(mxhci->host_ready);

	return snprintf(buff, PAGE_SIZE, "%d\n", val);

}

static DEVICE_ATTR(host_ready, S_IRUGO | S_IWUSR,
		host_ready_show, host_ready_store);

static int mxhci_hsic_probe(struct platform_device *pdev)
{
	struct hc_driver *driver;
	struct device_node *node = pdev->dev.of_node;
	struct mxhci_hsic_hcd *mxhci;
	struct xhci_hcd		*xhci;
	struct resource *res;
	struct usb_hcd *hcd;
	unsigned int reg;
	int ret;
	int irq;
	u32 tmp[3];
	u32 temp;

	if (usb_disabled())
		return -ENODEV;

	driver = &mxhci_hsic_hc_driver;

	pdev->dev.dma_mask = &dma_mask;

	/* usb2.0 root hub */
	driver->hcd_priv_size =	sizeof(struct mxhci_hsic_hcd);
	hcd = usb_create_hcd(driver, &pdev->dev, dev_name(&pdev->dev));
	if (!hcd)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = -ENODEV;
		goto put_hcd;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto put_hcd;
	}

	hcd_to_bus(hcd)->skip_resume = true;
	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	hcd->regs = devm_request_and_ioremap(&pdev->dev, res);
	if (!hcd->regs) {
		dev_err(&pdev->dev, "error mapping memory\n");
		ret = -EFAULT;
		goto put_hcd;
	}

	mxhci = hcd_to_hsic(hcd);
	mxhci->dev = &pdev->dev;
	mxhci->xhci_remove_flag = false;
	mxhci->xhci_shutdown_flag = false;

	mxhci->strobe = of_get_named_gpio(node, "hsic,strobe-gpio", 0);
	if (mxhci->strobe < 0) {
		ret = -EINVAL;
		goto put_hcd;
	}

	mxhci->data  = of_get_named_gpio(node, "hsic,data-gpio", 0);
	if (mxhci->data < 0) {
		ret = -EINVAL;
		goto put_hcd;
	}

	mxhci->host_ready = of_get_named_gpio(node,
					"qcom,host-ready-gpio", 0);
	if (mxhci->host_ready < 0)
		mxhci->host_ready = 0;

	mxhci->resume_gpio = of_get_named_gpio(node, "hsic,resume-gpio", 0);
	if (mxhci->resume_gpio < 0)
		mxhci->resume_gpio = 0;

	ret = of_property_read_u32_array(node, "qcom,vdd-voltage-level",
							tmp, ARRAY_SIZE(tmp));
	if (!ret) {
		mxhci->vdd_no_vol_level = tmp[0];
		mxhci->vdd_low_vol_level = tmp[1];
		mxhci->vdd_high_vol_level = tmp[2];
	} else {
		dev_err(&pdev->dev,
			"failed to read qcom,vdd-voltage-level property\n");
		ret = -EINVAL;
		goto put_hcd;
	}

	ret = mxhci_msm_config_gdsc(mxhci, 1);
	if (ret) {
		dev_err(&pdev->dev, "unable to configure hsic gdsc\n");
		goto put_hcd;
	}

	ret = mxhci_hsic_init_clocks(mxhci, 1);
	if (ret) {
		dev_err(&pdev->dev, "unable to initialize clocks\n");
		goto put_hcd;
	}

	ret = mxhci_hsic_init_vddcx(mxhci, 1);
	if (ret) {
		dev_err(&pdev->dev, "unable to initialize vddcx\n");
		goto deinit_clocks;
	}

	mxhci_hsic_reset(mxhci);

	/* HSIC phy caliberation:set periodic caliberation interval ~2.048sec */
	mxhci_hsic_ulpi_write(mxhci, 0xFF, MSM_HSIC_IO_CAL_PER);

	/* Enable periodic IO calibration in HSIC_CFG register */
	mxhci_hsic_ulpi_write(mxhci, 0xA8, MSM_HSIC_CFG);

	/* Configure Strobe and Data GPIOs to enable HSIC */
	ret = mxhci_hsic_config_gpios(mxhci);
	if (ret) {
		dev_err(mxhci->dev, " gpio configuarion failed\n");
		goto deinit_vddcx;
	}

	/* enable STROBE_PAD_CTL */
	reg = readl_relaxed(TLMM_GPIO_HSIC_STROBE_PAD_CTL);
	writel_relaxed(reg | 0x2000000, TLMM_GPIO_HSIC_STROBE_PAD_CTL);

	/* enable DATA_PAD_CTL */
	reg = readl_relaxed(TLMM_GPIO_HSIC_DATA_PAD_CTL);
	writel_relaxed(reg | 0x2000000, TLMM_GPIO_HSIC_DATA_PAD_CTL);

	mb();

	/* Enable LPM in Sleep mode and suspend mode */
	reg = readl_relaxed(MSM_HSIC_CTRL_REG);
	reg |= CTRLREG_PLL_CTRL_SLEEP | CTRLREG_PLL_CTRL_SUSP;
	writel_relaxed(reg, MSM_HSIC_CTRL_REG);

	if (of_property_read_bool(node, "qcom,disable-hw-clk-gating")) {
		reg = readl_relaxed(MSM_HSIC_GCTL);
		writel_relaxed((reg | GCTL_DSBLCLKGTNG), MSM_HSIC_GCTL);
	}

	mxhci->wakeup_irq = platform_get_irq_byname(pdev, "wakeup_irq");
	if (mxhci->wakeup_irq < 0) {
		mxhci->wakeup_irq = 0;
		dev_err(&pdev->dev, "failed to init wakeup_irq\n");
	} else {
		/* enable wakeup irq only when entering lpm */
		irq_set_status_flags(mxhci->wakeup_irq, IRQ_NOAUTOEN);
		ret = devm_request_irq(&pdev->dev, mxhci->wakeup_irq,
			mxhci_hsic_wakeup_irq, 0, "mxhci_hsic_wakeup", mxhci);
		if (ret) {
			dev_err(&pdev->dev,
					"request irq failed (wakeup irq)\n");
			goto deinit_vddcx;
		}
	}

	/* enable pwr event irq for LPM_IN_L2_IRQ */
	if (mxhci->wakeup_irq)
		reg = LPM_IN_L2_IRQ_MASK;
	else
		reg = LPM_IN_L2_IRQ_MASK | LPM_OUT_L2_IRQ_MASK;

	writel_relaxed(reg, MSM_HSIC_PWR_EVNT_IRQ_MASK);

	irq_set_status_flags(irq, IRQ_NOAUTOEN);
	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto deinit_vddcx;

	hcd = dev_get_drvdata(&pdev->dev);
	xhci = hcd_to_xhci(hcd);

	/* USB 3.0 roothub */

	/* no need for another instance of mxhci */
	driver->hcd_priv_size = sizeof(struct xhci_hcd *);

	xhci->shared_hcd = usb_create_shared_hcd(driver, &pdev->dev,
			dev_name(&pdev->dev), hcd);
	if (!xhci->shared_hcd) {
		ret = -ENOMEM;
		goto remove_usb2_hcd;
	}

	hcd_to_bus(xhci->shared_hcd)->skip_resume = true;
	/*
	 * Set the xHCI pointer before xhci_plat_setup() (aka hcd_driver.reset)
	 * is called by usb_add_hcd().
	 */
	*((struct xhci_hcd **) xhci->shared_hcd->hcd_priv) = xhci;

	ret = usb_add_hcd(xhci->shared_hcd, irq, IRQF_SHARED);
	if (ret)
		goto put_usb3_hcd;

	spin_lock_init(&mxhci->wakeup_lock);

	mxhci->pwr_event_irq = platform_get_irq_byname(pdev, "pwr_event_irq");
	if (mxhci->pwr_event_irq < 0) {
		dev_err(&pdev->dev,
				"platform_get_irq for pwr_event_irq failed\n");
		goto remove_usb3_hcd;
	}

	ret = devm_request_threaded_irq(&pdev->dev, mxhci->pwr_event_irq,
				NULL, mxhci_hsic_pwr_event_irq,
				IRQF_ONESHOT, "mxhci_hsic_pwr_evt", mxhci);
	if (ret) {
		dev_err(&pdev->dev, "request irq failed (pwr event irq)\n");
		goto remove_usb3_hcd;
	}

	mxhci->wq = create_singlethread_workqueue("mxhci_wq");
	if (!mxhci->wq) {
		dev_err(&pdev->dev, "unable to create workqueue\n");
		ret = -ENOMEM;
		goto remove_usb3_hcd;
	}

	INIT_WORK(&mxhci->bus_vote_w, mxhci_hsic_bus_vote_w);

	mxhci->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (!mxhci->bus_scale_table) {
		dev_dbg(&pdev->dev, "bus scaling is disabled\n");
	} else {
		mxhci->bus_perf_client =
			msm_bus_scale_register_client(mxhci->bus_scale_table);
		/* Configure BUS performance parameters for MAX bandwidth */
		if (mxhci->bus_perf_client) {
			mxhci->bus_vote = true;
			queue_work(mxhci->wq, &mxhci->bus_vote_w);
		} else {
			dev_err(&pdev->dev, "%s: bus scaling client reg err\n",
					__func__);
			ret = -ENODEV;
			goto delete_wq;
		}
	}

	temp = xhci_readl(xhci, &xhci->ir_set->irq_control);
	temp &= ~ER_IRQ_INTERVAL_MASK;
	temp |= (u32) MSM_HSIC_INT_MODERATION;
	xhci_writel(xhci, temp, &xhci->ir_set->irq_control);

	ret = device_create_file(&pdev->dev, &dev_attr_config_imod);
	if (ret)
		dev_dbg(&pdev->dev, "%s: unable to create imod sysfs entry\n",
					__func__);

	enable_irq(irq);
	/* Enable HSIC PHY */
	mxhci_hsic_ulpi_write(mxhci, 0x01, MSM_HSIC_CFG_SET);

	init_waitqueue_head(&mxhci->phy_in_lpm_wq);

	device_init_wakeup(&pdev->dev, 1);
	pm_stay_awake(mxhci->dev);

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = device_create_file(&pdev->dev, &dev_attr_host_ready);
	if (ret)
		pr_err("err creating sysfs node\n");

	mxhci->hsic_reboot.notifier_call = mxhci_hsic_reboot;
	mxhci->hsic_reboot.next = NULL;
	mxhci->hsic_reboot.priority = 1;
	ret = register_reboot_notifier(&mxhci->hsic_reboot);
	if (ret)
		dev_err(&pdev->dev, "%s: register for reboot failed\n",
					__func__);

	dev_dbg(&pdev->dev, "%s: Probe complete\n", __func__);

	ret = mxhci_hsic_debugfs_init();
	if (ret)
		dev_dbg(&pdev->dev, "debugfs is not availabile\n");
	return 0;

delete_wq:
	destroy_workqueue(mxhci->wq);
remove_usb3_hcd:
	usb_remove_hcd(xhci->shared_hcd);
put_usb3_hcd:
	usb_put_hcd(xhci->shared_hcd);
remove_usb2_hcd:
	usb_remove_hcd(hcd);
deinit_vddcx:
	mxhci_hsic_init_vddcx(mxhci, 0);
deinit_clocks:
	mxhci_hsic_init_clocks(mxhci, 0);
put_hcd:
	usb_put_hcd(hcd);

	return ret;
}

static int mxhci_hsic_remove(struct platform_device *pdev)
{
	struct usb_hcd	*hcd = platform_get_drvdata(pdev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd);
	u32 reg;

	xhci_dbg_log_event(&dbg_hsic, NULL,  "mxhci_hsic_remove", 0);

	/* disable STROBE_PAD_CTL */
	reg = readl_relaxed(TLMM_GPIO_HSIC_STROBE_PAD_CTL);
	writel_relaxed(reg & 0xfdffffff, TLMM_GPIO_HSIC_STROBE_PAD_CTL);

	/* disable DATA_PAD_CTL */
	reg = readl_relaxed(TLMM_GPIO_HSIC_DATA_PAD_CTL);
	writel_relaxed(reg & 0xfdffffff, TLMM_GPIO_HSIC_DATA_PAD_CTL);

	mb();

	device_remove_file(&pdev->dev, &dev_attr_config_imod);
	device_remove_file(&pdev->dev, &dev_attr_host_ready);
	mxhci_hsic_debugfs_cleanup();

	mxhci->xhci_remove_flag = true;
	wake_up(&mxhci->phy_in_lpm_wq);

	pm_runtime_get_sync(mxhci->dev);
	/* If the device was removed no need to call pm_runtime_disable */
	if (pdev->dev.power.power_state.event != PM_EVENT_INVALID)
		pm_runtime_disable(&pdev->dev);

	pm_runtime_set_suspended(&pdev->dev);

	usb_remove_hcd(xhci->shared_hcd);
	usb_put_hcd(xhci->shared_hcd);

	usb_remove_hcd(hcd);

	pm_runtime_put_noidle(mxhci->dev);

	if (mxhci->wakeup_irq_enabled)
		disable_irq_wake(mxhci->wakeup_irq);

	mxhci->bus_vote = false;
	cancel_work_sync(&mxhci->bus_vote_w);

	if (mxhci->bus_perf_client)
		msm_bus_scale_unregister_client(mxhci->bus_perf_client);

	destroy_workqueue(mxhci->wq);

	device_wakeup_disable(&pdev->dev);
	mxhci_hsic_init_vddcx(mxhci, 0);
	mxhci_hsic_init_clocks(mxhci, 0);
	mxhci_msm_config_gdsc(mxhci, 0);
	kfree(xhci);
	unregister_reboot_notifier(&mxhci->hsic_reboot);
	usb_put_hcd(hcd);


	/* only need this if we want to set default state on exit */
	if (IS_ERR(devm_pinctrl_get_select_default(&pdev->dev)))
		dev_err(&pdev->dev, "pinctrl set default failed\n");

	return 0;
}

#ifdef CONFIG_PM_RUNTIME
static int mxhci_hsic_runtime_idle(struct device *dev)
{
	dev_dbg(dev, "xhci msm runtime idle\n");
	return 0;
}

static int mxhci_hsic_runtime_suspend(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd);

	dev_dbg(dev, "xhci msm runtime suspend\n");
	xhci_dbg_log_event(&dbg_hsic, NULL,  "Run Time PM Suspend", 0);

	return mxhci_hsic_suspend(mxhci);
}

static int mxhci_hsic_runtime_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd);

	dev_dbg(dev, "xhci msm runtime resume\n");
	xhci_dbg_log_event(&dbg_hsic, NULL, "Run Time PM Resume", 0);

	return mxhci_hsic_resume(mxhci);
}
#endif

#ifdef CONFIG_PM_SLEEP
static int mxhci_hsic_pm_suspend(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd);

	dev_dbg(dev, "xhci-msm PM suspend\n");
	xhci_dbg_log_event(&dbg_hsic, NULL, "PM Suspend", 0);

	if (!mxhci->in_lpm) {
		dev_dbg(dev, "abort suspend\n");
		return -EBUSY;
	}

	if (device_may_wakeup(dev))
		enable_irq_wake(hcd->irq);

	return 0;
}

static int mxhci_hsic_pm_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct mxhci_hsic_hcd *mxhci = hcd_to_hsic(hcd);
	unsigned long flags;
	int ret;

	dev_dbg(dev, "xhci-msm PM resume\n");
	xhci_dbg_log_event(&dbg_hsic, NULL, "PM Resume", 0);

	if (device_may_wakeup(dev))
		disable_irq_wake(hcd->irq);

	/*
	 * Keep HSIC in Low Power Mode if system is resumed
	 * by any other wakeup source.  HSIC is resumed later
	 * when remote wakeup is received or interface driver
	 * start I/O.
	 */
	spin_lock_irqsave(&mxhci->wakeup_lock, flags);
	if (!mxhci->pm_usage_cnt &&
			pm_runtime_suspended(dev)) {
		spin_unlock_irqrestore(&mxhci->wakeup_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&mxhci->wakeup_lock, flags);

	ret = mxhci_hsic_resume(mxhci);
	if (ret)
		return ret;

	/* Bring the device to full powered state upon system resume */
	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;
}
#endif

static const struct dev_pm_ops xhci_msm_hsic_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mxhci_hsic_pm_suspend, mxhci_hsic_pm_resume)
	SET_RUNTIME_PM_OPS(mxhci_hsic_runtime_suspend,
			mxhci_hsic_runtime_resume, mxhci_hsic_runtime_idle)
};

static const struct of_device_id of_mxhci_hsic_matach[] = {
	{ .compatible = "qcom,xhci-msm-hsic",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_mxhci_hsic_matach);

static struct platform_driver mxhci_hsic_driver = {
	.probe	= mxhci_hsic_probe,
	.remove	= mxhci_hsic_remove,
	.shutdown = usb_hcd_platform_shutdown,
	.driver	= {
		.owner  = THIS_MODULE,
		.name = "xhci_msm_hsic",
		.pm = &xhci_msm_hsic_dev_pm_ops,
		.of_match_table	= of_mxhci_hsic_matach,
	},
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("XHCI MSM HSIC Glue Layer");

module_platform_driver(mxhci_hsic_driver);
