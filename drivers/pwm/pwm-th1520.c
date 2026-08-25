// SPDX-License-Identifier: GPL-2.0
/*
 * T-HEAD TH1520 PWM driver (C implementation)
 *
 * Copyright (c) 2026 Chen Pei <cp0613@linux.alibaba.com>
 *
 * Based on the Rust driver (pwm_th1520.rs) by
 * Michal Wilczynski <m.wilczynski@samsung.com>.
 *
 * Limitations:
 * - The period and duty cycle are controlled by 32-bit hardware
 *   registers, limiting the maximum resolution.
 * - Only continuous output mode is supported; one-shot mode is not
 *   implemented.
 * - The controller hardware provides up to 6 PWM channels.
 * - Reconfiguration is glitch free: new period and duty cycle values
 *   are latched and take effect at the start of the next period.
 * - Polarity is handled via a simple hardware inversion bit; arbitrary
 *   duty cycle offsets are not supported.
 * - Disabling a channel is achieved by configuring its duty cycle to
 *   zero to produce a static low output. Clearing the start bit does
 *   not reliably force the static inactive level, hence it is not used.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define TH1520_MAX_PWM_NUM		6

#define TH1520_PWM_CHN_BASE(n)		((n) * 0x20)
#define TH1520_PWM_CTRL(n)		(TH1520_PWM_CHN_BASE(n) + 0x00)
#define TH1520_PWM_PER(n)		(TH1520_PWM_CHN_BASE(n) + 0x08)
#define TH1520_PWM_FP(n)		(TH1520_PWM_CHN_BASE(n) + 0x0c)

#define TH1520_PWM_START		BIT(0)
#define TH1520_PWM_CFG_UPDATE		BIT(2)
#define TH1520_PWM_CONTINUOUS_MODE	BIT(5)
#define TH1520_PWM_FPOUT		BIT(8)

struct th1520_pwm {
	void __iomem *base;
	struct clk *clk;
};

static u32 th1520_ns_to_cycles(u64 ns, unsigned long rate)
{
	u64 cycles = mul_u64_u32_div(ns, rate, NSEC_PER_SEC);

	return min_t(u64, cycles, U32_MAX);
}

static u64 th1520_cycles_to_ns(u32 cycles, unsigned long rate)
{
	return DIV_ROUND_UP_ULL((u64)cycles * NSEC_PER_SEC, rate);
}

static int th1520_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct th1520_pwm *priv = pwmchip_get_drvdata(chip);
	unsigned long rate = clk_get_rate(priv->clk);
	unsigned int hwpwm = pwm->hwpwm;
	void __iomem *base = priv->base;
	bool inverted, was_enabled;
	u32 period_cycles, duty_cycles, ctrl;

	was_enabled = readl_relaxed(base + TH1520_PWM_FP(hwpwm)) != 0;

	if (!state->enabled) {
		if (was_enabled) {
			ctrl = readl_relaxed(base + TH1520_PWM_CTRL(hwpwm));
			writel_relaxed(ctrl, base + TH1520_PWM_CTRL(hwpwm));
			writel_relaxed(0, base + TH1520_PWM_FP(hwpwm));
			writel_relaxed(ctrl | TH1520_PWM_CFG_UPDATE,
				       base + TH1520_PWM_CTRL(hwpwm));
		}
		return 0;
	}

	period_cycles = th1520_ns_to_cycles(state->period, rate);
	if (!period_cycles)
		period_cycles = 1;

	duty_cycles = th1520_ns_to_cycles(state->duty_cycle, rate);

	ctrl = TH1520_PWM_CONTINUOUS_MODE;

	/*
	 * Inversion is implemented by programming the low time into the
	 * FP register and leaving the FPOUT bit cleared. With zero duty
	 * cycle the output is statically low regardless of polarity.
	 */
	inverted = state->polarity == PWM_POLARITY_INVERSED &&
		   state->duty_cycle > 0;
	if (inverted)
		duty_cycles = period_cycles - duty_cycles;
	else
		ctrl |= TH1520_PWM_FPOUT;

	writel_relaxed(ctrl, base + TH1520_PWM_CTRL(hwpwm));
	writel_relaxed(period_cycles, base + TH1520_PWM_PER(hwpwm));
	writel_relaxed(duty_cycles, base + TH1520_PWM_FP(hwpwm));
	writel_relaxed(ctrl | TH1520_PWM_CFG_UPDATE,
		       base + TH1520_PWM_CTRL(hwpwm));

	/*
	 * The START bit must be written in a separate, final transaction,
	 * and only when enabling the channel from a disabled state.
	 */
	if (!was_enabled)
		writel_relaxed(ctrl | TH1520_PWM_START,
			       base + TH1520_PWM_CTRL(hwpwm));

	return 0;
}

static int th1520_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct th1520_pwm *priv = pwmchip_get_drvdata(chip);
	unsigned long rate = clk_get_rate(priv->clk);
	unsigned int hwpwm = pwm->hwpwm;
	void __iomem *base = priv->base;
	u32 ctrl, period_cycles, duty_cycles;

	ctrl = readl_relaxed(base + TH1520_PWM_CTRL(hwpwm));
	period_cycles = readl_relaxed(base + TH1520_PWM_PER(hwpwm));
	duty_cycles = readl_relaxed(base + TH1520_PWM_FP(hwpwm));

	if (!period_cycles) {
		state->enabled = false;
		return 0;
	}

	state->period = th1520_cycles_to_ns(period_cycles, rate);

	if (ctrl & TH1520_PWM_FPOUT) {
		state->duty_cycle = th1520_cycles_to_ns(duty_cycles, rate);
		state->polarity = PWM_POLARITY_NORMAL;
	} else {
		state->duty_cycle = th1520_cycles_to_ns(period_cycles - duty_cycles, rate);
		state->polarity = PWM_POLARITY_INVERSED;
	}

	state->enabled = duty_cycles != 0;

	return 0;
}

static const struct pwm_ops th1520_pwm_ops = {
	.apply = th1520_pwm_apply,
	.get_state = th1520_pwm_get_state,
};

static void th1520_pwm_clk_exclusive_put(void *data)
{
	clk_rate_exclusive_put(data);
}

static int th1520_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct th1520_pwm *priv;
	unsigned long rate;
	int ret;

	chip = devm_pwmchip_alloc(dev, TH1520_MAX_PWM_NUM, sizeof(*priv));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	priv = pwmchip_get_drvdata(chip);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);

	rate = clk_get_rate(priv->clk);
	if (!rate)
		return dev_err_probe(dev, -EINVAL, "clock rate is zero\n");

	/* 32-bit period register limits the supported clock rate */
	if (rate > NSEC_PER_SEC)
		return dev_err_probe(dev, -EINVAL,
				     "clock rate %lu Hz too high\n", rate);

	ret = clk_rate_exclusive_get(priv->clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get exclusive clock rate\n");

	ret = devm_add_action_or_reset(dev, th1520_pwm_clk_exclusive_put,
				       priv->clk);
	if (ret)
		return ret;

	chip->ops = &th1520_pwm_ops;

	return devm_pwmchip_add(dev, chip);
}

static const struct of_device_id th1520_pwm_of_match[] = {
	{ .compatible = "thead,th1520-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, th1520_pwm_of_match);

static struct platform_driver th1520_pwm_driver = {
	.probe = th1520_pwm_probe,
	.driver = {
		.name = "pwm-th1520",
		.of_match_table = th1520_pwm_of_match,
	},
};
module_platform_driver(th1520_pwm_driver);

MODULE_AUTHOR("Chen Pei <cp0613@linux.alibaba.com>");
MODULE_DESCRIPTION("T-HEAD TH1520 PWM driver (C implementation)");
MODULE_LICENSE("GPL");
