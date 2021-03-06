/*
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/clk.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/device_cooling.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/types.h>

#define REG_SET		0x4
#define REG_CLR		0x8
#define REG_TOG		0xc

#define MISC0				0x0150
#define MISC0_REFTOP_SELBIASOFF		(1 << 3)

#define TEMPSENSE0			0x0180
#define TEMPSENSE0_TEMP_CNT_SHIFT	8
#define TEMPSENSE0_TEMP_CNT_MASK	(0xfff << TEMPSENSE0_TEMP_CNT_SHIFT)
#define TEMPSENSE0_FINISHED		(1 << 2)
#define TEMPSENSE0_MEASURE_TEMP		(1 << 1)
#define TEMPSENSE0_POWER_DOWN		(1 << 0)

#define TEMPSENSE1			0x0190
#define TEMPSENSE1_MEASURE_FREQ		0xffff

#define OCOTP_ANA1			0x04e0

/* The driver supports 1 passive trip point and 1 critical trip point */
enum imx_thermal_trip {
	IMX_TRIP_PASSIVE,
	IMX_TRIP_CRITICAL,
	IMX_TRIP_NUM,
};

/*
 * It defines the temperature in millicelsius for passive trip point
 * that will trigger cooling action when crossed.
 */
#define IMX_TEMP_PASSIVE		85000
#define IMX_TEMP_PASSIVE_COOL_DELTA	10000

/*
 * The maximum die temperature on imx parts is 105C, let's give some cushion
 * for noise and possible temperature rise between measurements.
 */
#define IMX_TEMP_CRITICAL		100000

#define IMX_POLLING_DELAY		2000 /* millisecond */
#define IMX_PASSIVE_DELAY		1000

#define FACTOR0				10000000
#define FACTOR1				15976
#define FACTOR2				4297157

struct imx_thermal_data {
	struct thermal_zone_device *tz;
	struct thermal_cooling_device *cdev[2];
	enum thermal_device_mode mode;
	struct regmap *tempmon;
	unsigned long trip_temp[IMX_TRIP_NUM];
	u32 c1, c2; /* See formula in imx_get_sensor_data() */
	struct clk *thermal_clk;
};

static int imx_get_temp(struct thermal_zone_device *tz, unsigned long *temp)
{
	struct imx_thermal_data *data = tz->devdata;
	struct regmap *map = data->tempmon;
	static unsigned long last_temp;
	unsigned int n_meas;
	u32 val;

	clk_prepare_enable(data->thermal_clk);
	/*
	 * Every time we measure the temperature, we will power on the
	 * temperature sensor, enable measurements, take a reading,
	 * disable measurements, power off the temperature sensor.
	 */
	regmap_write(map, TEMPSENSE0 + REG_CLR, TEMPSENSE0_POWER_DOWN);
	regmap_write(map, TEMPSENSE0 + REG_SET, TEMPSENSE0_MEASURE_TEMP);

	/*
	 * According to the temp sensor designers, it may require up to ~17us
	 * to complete a measurement.
	 */
	usleep_range(20, 50);

	regmap_read(map, TEMPSENSE0, &val);
	regmap_write(map, TEMPSENSE0 + REG_CLR, TEMPSENSE0_MEASURE_TEMP);
	regmap_write(map, TEMPSENSE0 + REG_SET, TEMPSENSE0_POWER_DOWN);

	if ((val & TEMPSENSE0_FINISHED) == 0) {
		dev_dbg(&tz->device, "temp measurement never finished\n");
		return -EAGAIN;
	}

	n_meas = (val & TEMPSENSE0_TEMP_CNT_MASK) >> TEMPSENSE0_TEMP_CNT_SHIFT;

	/* See imx_get_sensor_data() for formula derivation */
	*temp = data->c2 - n_meas * data->c1;

	if (*temp != last_temp) {
		dev_dbg(&tz->device, "millicelsius: %ld\n", *temp);
		last_temp = *temp;
	}

	clk_disable_unprepare(data->thermal_clk);

	return 0;
}

static int imx_get_mode(struct thermal_zone_device *tz,
			enum thermal_device_mode *mode)
{
	struct imx_thermal_data *data = tz->devdata;

	*mode = data->mode;

	return 0;
}

static int imx_set_mode(struct thermal_zone_device *tz,
			enum thermal_device_mode mode)
{
	struct imx_thermal_data *data = tz->devdata;

	if (mode == THERMAL_DEVICE_ENABLED) {
		tz->polling_delay = IMX_POLLING_DELAY;
		tz->passive_delay = IMX_PASSIVE_DELAY;
	} else {
		tz->polling_delay = 0;
		tz->passive_delay = 0;
	}

	data->mode = mode;
	thermal_zone_device_update(tz);

	return 0;
}

static int imx_get_trip_type(struct thermal_zone_device *tz, int trip,
			     enum thermal_trip_type *type)
{
	*type = (trip == IMX_TRIP_PASSIVE) ? THERMAL_TRIP_PASSIVE :
					     THERMAL_TRIP_CRITICAL;
	return 0;
}

static int imx_get_crit_temp(struct thermal_zone_device *tz,
			     unsigned long *temp)
{
	struct imx_thermal_data *data = tz->devdata;

	*temp = data->trip_temp[IMX_TRIP_CRITICAL];

	return 0;
}

static int imx_get_trip_temp(struct thermal_zone_device *tz, int trip,
			     unsigned long *temp)
{
	struct imx_thermal_data *data = tz->devdata;

	*temp = data->trip_temp[trip];

	return 0;
}

static int imx_set_trip_temp(struct thermal_zone_device *tz, int trip,
			     unsigned long temp)
{
	struct imx_thermal_data *data = tz->devdata;

	data->trip_temp[trip] = temp;

	return 0;
}

static int imx_bind(struct thermal_zone_device *tz,
		    struct thermal_cooling_device *cdev)
{
	int ret;

	ret = thermal_zone_bind_cooling_device(tz, IMX_TRIP_PASSIVE, cdev,
					       THERMAL_NO_LIMIT,
					       THERMAL_NO_LIMIT);
	if (ret) {
		dev_err(&tz->device,
			"binding zone %s with cdev %s failed:%d\n",
			tz->type, cdev->type, ret);
		return ret;
	}

	return 0;
}

static int imx_unbind(struct thermal_zone_device *tz,
		      struct thermal_cooling_device *cdev)
{
	int ret;

	ret = thermal_zone_unbind_cooling_device(tz, IMX_TRIP_PASSIVE, cdev);
	if (ret) {
		dev_err(&tz->device,
			"unbinding zone %s with cdev %s failed:%d\n",
			tz->type, cdev->type, ret);
		return ret;
	}

	return 0;
}

static int imx_get_trend(struct thermal_zone_device *tz,
		int trip, enum thermal_trend *trend)
{
	int ret;
	unsigned long trip_temp;

	ret = imx_get_trip_temp(tz, trip, &trip_temp);
	if (ret < 0)
		return ret;

	if (tz->temperature >= (trip_temp - IMX_TEMP_PASSIVE_COOL_DELTA))
		*trend = THERMAL_TREND_RAISE_FULL;
	else
		*trend = THERMAL_TREND_DROP_FULL;

	return 0;
}

static const struct thermal_zone_device_ops imx_tz_ops = {
	.bind = imx_bind,
	.unbind = imx_unbind,
	.get_temp = imx_get_temp,
	.get_mode = imx_get_mode,
	.set_mode = imx_set_mode,
	.get_trip_type = imx_get_trip_type,
	.get_trip_temp = imx_get_trip_temp,
	.get_crit_temp = imx_get_crit_temp,
	.get_trend = imx_get_trend,
	.set_trip_temp = imx_set_trip_temp,
};

static int imx_get_sensor_data(struct platform_device *pdev)
{
	struct imx_thermal_data *data = platform_get_drvdata(pdev);
	struct regmap *map;
	int t1, n1;
	int ret;
	u32 val;
	u64 temp64;

	map = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
					      "fsl,tempmon-data");
	if (IS_ERR(map)) {
		ret = PTR_ERR(map);
		dev_err(&pdev->dev, "failed to get sensor regmap: %d\n", ret);
		return ret;
	}

	ret = regmap_read(map, OCOTP_ANA1, &val);
	if (ret) {
		dev_err(&pdev->dev, "failed to read sensor data: %d\n", ret);
		return ret;
	}

	if (val == 0 || val == ~0) {
		dev_err(&pdev->dev, "invalid sensor calibration data\n");
		return -EINVAL;
	}

	/*
	 * Sensor data layout:
	 *   [31:20] - sensor value @ 25C
	 * We use universal formula now and only need sensor value @ 25C
	 * slope = 0.4297157 - (0.0015976 * 25C fuse)
	 */
	n1 = val >> 20;
	t1 = 25; /* t1 always 25C */

	/*
	 * Derived from linear interpolation:
	 * slope = 0.4297157 - (0.0015976 * 25C fuse)
	 * slope = (FACTOR2 - FACTOR1 * n1) / FACTOR0
	 * (Nmeas - n1) / (Tmeas - t1) = slope
	 * We want to reduce this down to the minimum computation necessary
	 * for each temperature read.  Also, we want Tmeas in millicelsius
	 * and we don't want to lose precision from integer division. So...
	 * Tmeas = (Nmeas - n1) / slope + t1
	 * milli_Tmeas = 1000 * (Nmeas - n1) / slope + 1000 * t1
	 * milli_Tmeas = -1000 * (n1 - Nmeas) / slope + 1000 * t1
	 * Let constant c1 = (-1000 / slope)
	 * milli_Tmeas = (n1 - Nmeas) * c1 + 1000 * t1
	 * Let constant c2 = n1 *c1 + 1000 * t1
	 * milli_Tmeas = c2 - Nmeas * c1
	 */
	temp64 = FACTOR0;
	temp64 *= 1000;
	do_div(temp64, FACTOR1 * n1 - FACTOR2);
	data->c1 = temp64;
	data->c2 = n1 * data->c1 + 1000 * t1;

	return 0;
}

static int imx_thermal_probe(struct platform_device *pdev)
{
	struct imx_thermal_data *data;
	struct cpumask clip_cpus;
	struct regmap *map;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	map = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "fsl,tempmon");
	if (IS_ERR(map)) {
		ret = PTR_ERR(map);
		dev_err(&pdev->dev, "failed to get tempmon regmap: %d\n", ret);
		return ret;
	}
	data->tempmon = map;

	platform_set_drvdata(pdev, data);

	ret = imx_get_sensor_data(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to get sensor data\n");
		return ret;
	}

	data->thermal_clk = devm_clk_get(&pdev->dev, NULL);
	ret = IS_ERR(data->thermal_clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to get thermal clk!\n");
		return ret;
	}

	/* Make sure sensor is in known good state for measurements */
	regmap_write(map, TEMPSENSE0 + REG_CLR, TEMPSENSE0_POWER_DOWN);
	regmap_write(map, TEMPSENSE0 + REG_CLR, TEMPSENSE0_MEASURE_TEMP);
	regmap_write(map, TEMPSENSE1 + REG_CLR, TEMPSENSE1_MEASURE_FREQ);
	regmap_write(map, MISC0 + REG_SET, MISC0_REFTOP_SELBIASOFF);
	regmap_write(map, TEMPSENSE0 + REG_SET, TEMPSENSE0_POWER_DOWN);

	cpumask_set_cpu(0, &clip_cpus);
	data->cdev[0] = cpufreq_cooling_register(&clip_cpus);
	if (IS_ERR(data->cdev[0])) {
		ret = PTR_ERR(data->cdev[0]);
		dev_err(&pdev->dev,
			"failed to register cpufreq cooling device: %d\n", ret);
		return ret;
	}

	data->cdev[1] = devfreq_cooling_register();
	if (IS_ERR(data->cdev[1])) {
		ret = PTR_ERR(data->cdev[1]);
		dev_err(&pdev->dev,
			"failed to register devfreq cooling device: %d\n", ret);
		return ret;
	}

	data->trip_temp[IMX_TRIP_PASSIVE] = IMX_TEMP_PASSIVE;
	data->trip_temp[IMX_TRIP_CRITICAL] = IMX_TEMP_CRITICAL;
	data->tz = thermal_zone_device_register("imx_thermal_zone",
						IMX_TRIP_NUM,
						(1 << IMX_TRIP_NUM) - 1,
						data,
						&imx_tz_ops, NULL,
						IMX_PASSIVE_DELAY,
						IMX_POLLING_DELAY);
	if (IS_ERR(data->tz)) {
		ret = PTR_ERR(data->tz);
		dev_err(&pdev->dev,
			"failed to register thermal zone device %d\n", ret);
		cpufreq_cooling_unregister(data->cdev[0]);
		devfreq_cooling_unregister(data->cdev[1]);
		return ret;
	}

	data->mode = THERMAL_DEVICE_ENABLED;

	return 0;
}

static int imx_thermal_remove(struct platform_device *pdev)
{
	struct imx_thermal_data *data = platform_get_drvdata(pdev);

	thermal_zone_device_unregister(data->tz);
	cpufreq_cooling_unregister(data->cdev[0]);
	devfreq_cooling_unregister(data->cdev[1]);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int imx_thermal_suspend(struct device *dev)
{
	struct imx_thermal_data *data = dev_get_drvdata(dev);
	struct regmap *map = data->tempmon;
	u32 val;

	regmap_read(map, TEMPSENSE0, &val);
	if ((val & TEMPSENSE0_POWER_DOWN) == 0) {
		/*
		 * If a measurement is taking place, wait for a long enough
		 * time for it to finish, and then check again.  If it still
		 * does not finish, something must go wrong.
		 */
		udelay(50);
		regmap_read(map, TEMPSENSE0, &val);
		if ((val & TEMPSENSE0_POWER_DOWN) == 0)
			return -ETIMEDOUT;
	}

	return 0;
}

static int imx_thermal_resume(struct device *dev)
{
	/* Nothing to do for now */
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(imx_thermal_pm_ops,
			 imx_thermal_suspend, imx_thermal_resume);

static const struct of_device_id of_imx_thermal_match[] = {
	{ .compatible = "fsl,imx6q-tempmon", },
	{ /* end */ }
};

static struct platform_driver imx_thermal = {
	.driver = {
		.name	= "imx_thermal",
		.owner  = THIS_MODULE,
		.pm	= &imx_thermal_pm_ops,
		.of_match_table = of_imx_thermal_match,
	},
	.probe		= imx_thermal_probe,
	.remove		= imx_thermal_remove,
};

static int __init imx_thermal_init(void)
{
	return platform_driver_register(&imx_thermal);
}

late_initcall(imx_thermal_init);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Thermal driver for Freescale i.MX SoCs");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:imx-thermal");
