// SPDX-License-Identifier: GPL-2.0
/*
 * Quiet fan control for the CIX Sky1 (Orange Pi 6 Plus / CIX Phecda) when
 * booted with ACPI instead of devicetree.
 *
 * The ACPI firmware only exposes three fan profiles through CIXHA024
 * (mute / automatic / performance) and its automatic curve idles the fan at
 * 40%, which is far louder than the devicetree setup, where Linux drove the
 * Chrome EC PWM directly from a pwm-fan cooling device.
 *
 * This driver restores that behaviour without taking the I2C controller away
 * from AML: it installs one small ACPI method that reuses the firmware's own
 * serialized \_SB.EC0.TRAS transport, puts the EC fan into manual mode
 * (EC_CMD_THERMAL_AUTO_FAN_CTRL with profile 0) and then runs the devicetree
 * fan curve in a polling work item.
 *
 * Safety: any EC error, an unreadable thermal zone, a temperature above
 * emergency_temp, module unload, reboot or suspend all hand the fan straight
 * back to the firmware's automatic profile.
 *
 * hwmon pwm1_enable:
 *   0 - firmware performance profile (full speed)
 *   1 - manual, pwm1 is used verbatim
 *   2 - quiet curve (default)
 *   3 - firmware automatic profile
 */

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hex.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include "acpi/cix_ec_raw.hex"

#define CIX_EC_PROTO3		0xDA
#define CIX_EC_RESP_OVERHEAD	10	/* 2 i2c bytes + ec_host_response */

/* Chrome EC host commands, as used by the platform's own AML. */
#define EC_CMD_PWM_SET_DUTY		0x0025
#define EC_CMD_PWM_GET_DUTY		0x0026
#define EC_CMD_THERMAL_AUTO_FAN_CTRL	0x0052

/*
 * Probe-time proof that this EC really hands fan control over.  Move the duty
 * away from whatever the firmware picked -- downwards, so the check stays
 * quiet -- and read it back.
 */
#define CIX_PROBE_DELTA		12	/* percent to move by */
#define CIX_PROBE_MIN		10	/* never probe below this duty */
#define CIX_PROBE_MAX		90
#define CIX_PROBE_SETTLE_MS	2500	/* did the write reach the fan? */
#define CIX_PROBE_TOLERANCE	3	/* the EC rounds by a point or two */

/*
 * Runtime confirmation that we still own the fan.  Read the duty back every
 * CIX_VERIFY_EVERY polls and give up after CIX_VERIFY_STRIKES consecutive
 * disagreements, so a transient never triggers a handback.
 */
#define CIX_VERIFY_EVERY	5
#define CIX_VERIFY_STRIKES	3

/* CIX profile byte for EC_CMD_THERMAL_AUTO_FAN_CTRL. */
#define CIX_FAN_PROFILE_MANUAL	0x00
#define CIX_FAN_PROFILE_AUTO	0x01
#define CIX_FAN_PROFILE_MUTE	0x02
#define CIX_FAN_PROFILE_PERF	0x04

enum cix_mode {
	CIX_MODE_PERF = 0,
	CIX_MODE_MANUAL = 1,
	CIX_MODE_CURVE = 2,
	CIX_MODE_FW_AUTO = 3,
};

static int poll_ms = 2000;
module_param(poll_ms, int, 0644);
MODULE_PARM_DESC(poll_ms, "curve polling interval in milliseconds");

static int emergency_temp = 95000;
module_param(emergency_temp, int, 0644);
MODULE_PARM_DESC(emergency_temp,
		 "millicelsius above which the firmware profile takes over");

static bool skip_probe_check;
module_param(skip_probe_check, bool, 0444);
MODULE_PARM_DESC(skip_probe_check,
		 "skip the probe-time duty check (the runtime check still runs)");

static int hyst = 3000;
module_param(hyst, int, 0644);
MODULE_PARM_DESC(hyst, "curve downshift hysteresis in millicelsius");

/*
 * Quiet curve, in EC duty percent.  Same shape as the devicetree
 * cooling-levels/trip points, scaled into the EC's 0..100 domain.
 */
struct cix_step {
	int temp_mc;
	u8 percent;
};

static const struct cix_step cix_curve[] = {
	{     0,  15 },
	{ 58000,  20 },
	{ 64000,  28 },
	{ 70000,  40 },
	{ 76000,  55 },
	{ 81000,  70 },
	{ 86000, 100 },
};

static const char * const cix_zone_names[] = {
	"TZB0", "TZB1", "TZM0", "TZM1", "TZGT",
};

static acpi_handle xraw_handle;
static struct device *hwmon_dev;
static struct device *cix_dev;
static struct thermal_zone_device *cix_zones[ARRAY_SIZE(cix_zone_names)];
static struct delayed_work cix_work;
static DEFINE_MUTEX(cix_lock);
static int cix_mode = CIX_MODE_CURVE;
static u8 cix_percent;			/* last percent requested */
static unsigned int cix_step;		/* current curve step */
static int cix_last_temp;
static bool cix_manual_engaged;
static unsigned int cix_fail_count;
static unsigned int cix_verify_tick;
static unsigned int cix_verify_bad;
static int cix_prev_percent = -1;
static struct dentry *cix_dir;
static char cix_last[512];

static u8 cix_csum(const u8 *buf, size_t len, size_t skip)
{
	u8 sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		if (i != skip)
			sum += buf[i];

	return (u8)(0x100 - sum);
}

/*
 * Issue one Chrome EC host command.  The CIX EC keeps the u16 fields of
 * ec_host_request / ec_host_response big-endian, exactly as the platform AML
 * builds them.  Returns 0 on success, negative errno otherwise.
 */
static int cix_ec_cmd(u16 cmd, u8 ver, const u8 *data, u16 dlen,
		      u8 *out, u16 outlen)
{
	union acpi_object args[2];
	struct acpi_object_list arg_list = { .count = 2, .pointer = args };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *pkg, *rsp;
	u16 rtotal = CIX_EC_RESP_OVERHEAD + outlen;
	acpi_status status;
	u16 ec_result;
	u8 *req;
	int ret;

	if (!xraw_handle)
		return -ENODEV;
	if (rtotal > 0x40)
		return -EINVAL;

	req = kzalloc(9 + dlen, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req[0] = CIX_EC_PROTO3;
	req[1] = 3;
	req[3] = cmd >> 8;
	req[4] = cmd & 0xFF;
	req[5] = ver;
	req[7] = dlen >> 8;
	req[8] = dlen & 0xFF;
	if (dlen)
		memcpy(req + 9, data, dlen);
	/* checksum covers the request struct and data, minus itself */
	req[2] = cix_csum(req + 1, 8 + dlen, 1);

	args[0].type = ACPI_TYPE_BUFFER;
	args[0].buffer.length = 9 + dlen;
	args[0].buffer.pointer = req;
	args[1].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = rtotal;

	status = acpi_evaluate_object(xraw_handle, NULL, &arg_list, &result);
	kfree(req);
	if (ACPI_FAILURE(status))
		return -EIO;

	pkg = result.pointer;
	if (!pkg || pkg->type != ACPI_TYPE_PACKAGE || pkg->package.count != 2) {
		ret = -EPROTO;
		goto out;
	}
	if (pkg->package.elements[0].type != ACPI_TYPE_INTEGER ||
	    pkg->package.elements[0].integer.value != 0) {
		ret = -EREMOTEIO;	/* EC0.TRAS transport failure */
		goto out;
	}
	rsp = &pkg->package.elements[1];
	if (rsp->type != ACPI_TYPE_BUFFER || rsp->buffer.length < rtotal) {
		ret = -EPROTO;
		goto out;
	}
	ec_result = (rsp->buffer.pointer[4] << 8) | rsp->buffer.pointer[5];
	if (rsp->buffer.pointer[0] || ec_result) {
		ret = -EIO;
		goto out;
	}
	if (outlen && out)
		memcpy(out, rsp->buffer.pointer + CIX_EC_RESP_OVERHEAD, outlen);
	ret = 0;
out:
	kfree(result.pointer);
	return ret;
}

static int cix_set_profile(u8 profile)
{
	return cix_ec_cmd(EC_CMD_THERMAL_AUTO_FAN_CTRL, 0, &profile, 1,
			  NULL, 0);
}

static int cix_set_duty(u8 percent)
{
	u8 data[4] = { 0x00, percent, 0x00, 0x00 };	/* duty BE, generic, ch0 */

	return cix_ec_cmd(EC_CMD_PWM_SET_DUTY, 0, data, sizeof(data), NULL, 0);
}

static int cix_get_duty(u8 *percent)
{
	u8 data[2] = { 0x00, 0x00 };
	u8 out[2];
	int ret;

	ret = cix_ec_cmd(EC_CMD_PWM_GET_DUTY, 0, data, sizeof(data), out,
			 sizeof(out));
	if (ret)
		return ret;
	*percent = out[1];
	return 0;
}

/* Hand the fan back to the firmware and stop touching it. */
static void cix_handoff_locked(const char *why)
{
	int ret = cix_set_profile(CIX_FAN_PROFILE_AUTO);

	cix_manual_engaged = false;
	if (ret)
		pr_crit("cixfan: %s, but restoring firmware auto failed (%d)\n",
			why, ret);
	else
		pr_warn("cixfan: %s, firmware automatic profile restored\n",
			why);
}

static int cix_max_temp(int *temp_mc)
{
	int max = INT_MIN;
	unsigned int i;
	int t, ret;

	for (i = 0; i < ARRAY_SIZE(cix_zones); i++) {
		if (!cix_zones[i])
			continue;
		ret = thermal_zone_get_temp(cix_zones[i], &t);
		if (ret)
			continue;
		if (t > max)
			max = t;
	}
	if (max == INT_MIN)
		return -ENODATA;

	*temp_mc = max;
	return 0;
}

/* Pick a curve step with hysteresis so the fan does not hunt. */
static unsigned int cix_pick_step(unsigned int cur, int temp_mc)
{
	unsigned int i, want = 0;

	for (i = 0; i < ARRAY_SIZE(cix_curve); i++)
		if (temp_mc >= cix_curve[i].temp_mc)
			want = i;

	if (want > cur)
		return want;
	if (want < cur && temp_mc < cix_curve[cur].temp_mc - hyst)
		return cur - 1;
	return cur;
}

static int cix_apply_locked(u8 percent)
{
	int ret;

	if (!cix_manual_engaged) {
		ret = cix_set_profile(CIX_FAN_PROFILE_MANUAL);
		if (ret)
			return ret;
		cix_manual_engaged = true;
	}

	ret = cix_set_duty(percent);
	if (ret) {
		ret = cix_set_duty(percent);	/* one retry on a busy EC */
		if (ret)
			return ret;
	}

	cix_percent = percent;
	return 0;
}

/*
 * Profile 0 is undocumented -- it was found by probing this board's EC, not
 * from any CIX documentation.  An EC firmware that numbers its fan profiles
 * differently could accept the command and then quietly ignore the duty
 * writes, leaving us running a curve we are not actually driving.  So prove
 * control before taking it: step the duty to a value the EC's own loop would
 * not choose, and read it back.  Caller holds cix_lock.
 */
static int cix_verify_manual_control(struct device *dev)
{
	u8 baseline, target, seen;
	int ret;

	ret = cix_get_duty(&baseline);
	if (ret) {
		dev_err(dev, "EC did not report the current fan duty (%d)\n",
			ret);
		return ret;
	}

	if (baseline > CIX_PROBE_MIN + CIX_PROBE_DELTA)
		target = baseline - CIX_PROBE_DELTA;
	else
		target = baseline + CIX_PROBE_DELTA;
	target = clamp_val(target, CIX_PROBE_MIN, CIX_PROBE_MAX);

	ret = cix_set_profile(CIX_FAN_PROFILE_MANUAL);
	if (ret) {
		dev_err(dev, "EC rejected the manual fan profile (%d)\n", ret);
		goto restore;
	}

	ret = cix_set_duty(target);
	if (ret) {
		dev_err(dev, "EC rejected a fan duty write (%d)\n", ret);
		goto restore;
	}

	msleep(CIX_PROBE_SETTLE_MS);

	ret = cix_get_duty(&seen);
	if (ret) {
		dev_err(dev, "EC did not report the fan duty back (%d)\n", ret);
		goto restore;
	}

	if (abs((int)seen - (int)target) > CIX_PROBE_TOLERANCE) {
		dev_err(dev,
			"EC ignored the fan duty write: asked for %u%%, it reports %u%% (was %u%%)\n",
			target, seen, baseline);
		dev_err(dev, "not taking the fan over\n");
		ret = -ENODEV;
		goto restore;
	}

	dev_info(dev, "fan duty writes verified (%u%% -> %u%%, EC reports %u%%)\n",
		 baseline, target, seen);
	cix_manual_engaged = true;
	return 0;

restore:
	cix_set_profile(CIX_FAN_PROFILE_AUTO);
	cix_manual_engaged = false;
	return ret ? ret : -ENODEV;
}

/*
 * Keep confirming that the duty we ask for is the duty the EC has.
 *
 * The probe check can only prove that a duty write takes effect.  It cannot
 * prove that fan profile 0 really means manual on this firmware, because this
 * EC does not touch the fan while its own computed target is unchanged -- at
 * a stable temperature its automatic mode is indistinguishable from manual,
 * and re-sending a profile does not force a re-evaluation either.  The
 * difference only becomes visible when the EC re-evaluates, which is why this
 * belongs in the poll loop rather than in probe.  Caller holds cix_lock.
 */
static void cix_check_duty_locked(u8 asked)
{
	u8 seen;

	/* A freshly changed target needs a poll to settle before comparing. */
	if ((int)asked != cix_prev_percent) {
		cix_prev_percent = asked;
		cix_verify_tick = 0;
		cix_verify_bad = 0;
		return;
	}

	if (++cix_verify_tick < CIX_VERIFY_EVERY)
		return;
	cix_verify_tick = 0;

	if (cix_get_duty(&seen))
		return;		/* a busy transport is not a verdict */

	if (abs((int)seen - (int)asked) <= CIX_PROBE_TOLERANCE) {
		cix_verify_bad = 0;
		return;
	}

	if (++cix_verify_bad < CIX_VERIFY_STRIKES) {
		dev_dbg(cix_dev, "fan duty disagrees: asked %u%%, EC has %u%%\n",
			asked, seen);
		return;
	}

	dev_warn(cix_dev,
		 "EC keeps overriding the fan: asked %u%%, it has %u%%\n",
		 asked, seen);
	cix_handoff_locked("something else is driving the fan");
	cix_mode = CIX_MODE_FW_AUTO;
}

static void cix_work_fn(struct work_struct *w)
{
	int temp, ret;
	u8 percent;

	mutex_lock(&cix_lock);

	if (cix_mode != CIX_MODE_CURVE && cix_mode != CIX_MODE_MANUAL)
		goto out;

	ret = cix_max_temp(&temp);
	if (ret) {
		cix_handoff_locked("no thermal zone readable");
		cix_mode = CIX_MODE_FW_AUTO;
		goto out;
	}
	cix_last_temp = temp;

	if (temp >= emergency_temp) {
		cix_handoff_locked("emergency temperature reached");
		cix_mode = CIX_MODE_FW_AUTO;
		goto out;
	}

	if (cix_mode == CIX_MODE_CURVE) {
		cix_step = cix_pick_step(cix_step, temp);
		percent = cix_curve[cix_step].percent;
	} else {
		percent = cix_percent;
	}

	/*
	 * Look at what the EC has *before* re-asserting: reading straight
	 * after a write would just hand back the value we had put there.
	 */
	cix_check_duty_locked(percent);
	if (cix_mode != CIX_MODE_CURVE && cix_mode != CIX_MODE_MANUAL)
		goto out;	/* the check gave the fan back */

	/* Re-assert every poll: the EC drops back to auto after a reset. */
	ret = cix_apply_locked(percent);
	if (ret) {
		/*
		 * The EC transport is shared with AML, so an occasional busy
		 * transfer is normal.  Only give up after repeated failures.
		 */
		cix_manual_engaged = false;
		if (++cix_fail_count >= 5) {
			cix_handoff_locked("EC kept rejecting fan commands");
			cix_mode = CIX_MODE_FW_AUTO;
		}
	} else {
		cix_fail_count = 0;
	}
out:
	mutex_unlock(&cix_lock);
	schedule_delayed_work(&cix_work, msecs_to_jiffies(poll_ms));
}

/* Apply a mode change.  Caller holds cix_lock. */
static int cix_set_mode_locked(int mode)
{
	int ret = 0;

	switch (mode) {
	case CIX_MODE_PERF:
		ret = cix_set_profile(CIX_FAN_PROFILE_PERF);
		cix_manual_engaged = false;
		break;
	case CIX_MODE_FW_AUTO:
		ret = cix_set_profile(CIX_FAN_PROFILE_AUTO);
		cix_manual_engaged = false;
		break;
	case CIX_MODE_MANUAL:
		ret = cix_apply_locked(cix_percent ?: 15);
		break;
	case CIX_MODE_CURVE: {
		int temp;
		unsigned int i;

		cix_step = 0;
		if (cix_max_temp(&temp) == 0) {
			for (i = 0; i < ARRAY_SIZE(cix_curve); i++)
				if (temp >= cix_curve[i].temp_mc)
					cix_step = i;
			cix_last_temp = temp;
		}
		ret = cix_apply_locked(cix_curve[cix_step].percent);
		break;
	}
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;

	cix_prev_percent = -1;
	cix_verify_tick = 0;
	cix_verify_bad = 0;
	cix_mode = mode;
	return 0;
}

/* ---- hwmon ---- */

static umode_t cix_hwmon_is_visible(const void *drv, enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	if (type == hwmon_pwm)
		return 0644;
	return 0444;
}

static int cix_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	u8 percent;
	int ret = 0;

	mutex_lock(&cix_lock);
	switch (type) {
	case hwmon_pwm:
		if (attr == hwmon_pwm_enable) {
			*val = cix_mode;
		} else if (attr == hwmon_pwm_input) {
			ret = cix_get_duty(&percent);
			if (ret)
				percent = cix_percent;
			*val = DIV_ROUND_CLOSEST((int)percent * 255, 100);
			ret = 0;
		} else {
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_temp:
		if (cix_max_temp(&cix_last_temp) == 0)
			*val = cix_last_temp;
		else
			ret = -ENODATA;
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	mutex_unlock(&cix_lock);
	return ret;
}

static int cix_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long val)
{
	int ret = -EOPNOTSUPP;

	if (type != hwmon_pwm)
		return ret;

	mutex_lock(&cix_lock);
	if (attr == hwmon_pwm_enable) {
		if (val < 0 || val > 3)
			ret = -EINVAL;
		else
			ret = cix_set_mode_locked(val);
	} else if (attr == hwmon_pwm_input) {
		if (val < 0 || val > 255) {
			ret = -EINVAL;
		} else {
			u8 percent = DIV_ROUND_CLOSEST((int)val * 100, 255);

			cix_percent = percent;
			if (cix_mode == CIX_MODE_MANUAL)
				ret = cix_apply_locked(percent);
			else
				ret = 0;
		}
	}
	mutex_unlock(&cix_lock);
	return ret;
}

static int cix_hwmon_read_string(struct device *dev, enum hwmon_sensor_types t,
				 u32 attr, int channel, const char **str)
{
	*str = "maximum SoC temperature";
	return 0;
}

static const struct hwmon_ops cix_hwmon_ops = {
	.is_visible = cix_hwmon_is_visible,
	.read = cix_hwmon_read,
	.write = cix_hwmon_write,
	.read_string = cix_hwmon_read_string,
};

static const struct hwmon_channel_info * const cix_hwmon_info[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info cix_hwmon_chip = {
	.ops = &cix_hwmon_ops,
	.info = cix_hwmon_info,
};

/* ---- debugfs raw EC passthrough (diagnostics) ---- */

static ssize_t cix_cmd_write(struct file *f, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	char kbuf[256], dhex[128] = "";
	u8 data[64], out[64];
	unsigned int cmd, ver, rlen;
	int dlen = 0, ret, i, n;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	n = sscanf(kbuf, "%x %x %x %127s", &cmd, &ver, &rlen, dhex);
	if (n < 3)
		return -EINVAL;
	if (n == 4) {
		int hl = strlen(dhex);

		if (hl % 2 || hl / 2 > (int)sizeof(data))
			return -EINVAL;
		for (i = 0; i < hl / 2; i++)
			if (hex2bin(&data[i], dhex + i * 2, 1))
				return -EINVAL;
		dlen = hl / 2;
	}
	if (rlen > sizeof(out))
		return -EINVAL;

	mutex_lock(&cix_lock);
	ret = cix_ec_cmd(cmd, ver, data, dlen, out, rlen);
	n = scnprintf(cix_last, sizeof(cix_last), "cmd=0x%04x ver=%u ret=%d data=",
		      cmd, ver, ret);
	for (i = 0; i < (int)rlen; i++)
		n += scnprintf(cix_last + n, sizeof(cix_last) - n, "%02x", out[i]);
	scnprintf(cix_last + n, sizeof(cix_last) - n, "\n");
	mutex_unlock(&cix_lock);

	return len;
}

static ssize_t cix_cmd_read(struct file *f, char __user *ubuf, size_t len,
			    loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, len, ppos, cix_last,
				       strlen(cix_last));
}

static const struct file_operations cix_cmd_fops = {
	.owner = THIS_MODULE,
	.write = cix_cmd_write,
	.read = cix_cmd_read,
	.llseek = default_llseek,
};

/* ---- ACPI helper installation ---- */

static u8 cix_table_csum(const u8 *buf, size_t len)
{
	u8 sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		sum += buf[i];

	return -sum;
}

/*
 * acpi_install_method() wants a table header immediately followed by a single
 * Method opcode, so drop the External() declarations iasl needs at build time.
 */
static int cix_install_xraw(void)
{
	const size_t hdr = sizeof(struct acpi_table_header);
	const size_t src = sizeof(cix_ec_raw_aml_code);
	struct acpi_table_header *tbl;
	acpi_handle ec;
	acpi_status status;
	size_t off, tlen;

	status = acpi_get_handle(NULL, "\\_SB.EC0", &ec);
	if (ACPI_FAILURE(status)) {
		pr_err("cixfan: no \\_SB.EC0: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}

	for (off = hdr; off + 16 < src; off++) {
		if (cix_ec_raw_aml_code[off] == 0x14 &&
		    !memcmp(cix_ec_raw_aml_code + off + 1, "\x45\x05\x5c\x2f", 4))
			break;
	}
	if (off + 16 >= src) {
		pr_err("cixfan: cannot locate MethodOp in helper AML\n");
		return -EINVAL;
	}

	tlen = hdr + src - off;
	tbl = kmalloc(tlen, GFP_KERNEL);
	if (!tbl)
		return -ENOMEM;
	memcpy(tbl, cix_ec_raw_aml_code, hdr);
	memcpy((u8 *)tbl + hdr, cix_ec_raw_aml_code + off, src - off);
	tbl->length = tlen;
	tbl->checksum = 0;
	tbl->checksum = cix_table_csum((u8 *)tbl, tlen);

	status = acpi_install_method((u8 *)tbl);
	kfree(tbl);
	if (ACPI_FAILURE(status)) {
		pr_err("cixfan: acpi_install_method failed: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}

	status = acpi_get_handle(ec, "XRAW", &xraw_handle);
	if (ACPI_FAILURE(status)) {
		pr_err("cixfan: XRAW missing after install\n");
		return -EIO;
	}

	return 0;
}

/* ---- lifecycle ---- */

static int cixfan_probe(struct platform_device *pdev)
{
	unsigned int i, found = 0;
	int ret;

	ret = cix_install_xraw();
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(cix_zone_names); i++) {
		cix_zones[i] = thermal_zone_get_zone_by_name(cix_zone_names[i]);
		if (IS_ERR(cix_zones[i]))
			cix_zones[i] = NULL;
		else
			found++;
	}
	if (!found) {
		dev_err(&pdev->dev, "no SoC thermal zone found, not taking over\n");
		return -ENODEV;
	}

	cix_dev = &pdev->dev;
	INIT_DELAYED_WORK(&cix_work, cix_work_fn);

	mutex_lock(&cix_lock);
	cix_percent = cix_curve[0].percent;
	if (skip_probe_check) {
		dev_warn(&pdev->dev,
			 "skipping the manual-control check on request\n");
		ret = 0;
	} else {
		ret = cix_verify_manual_control(&pdev->dev);
	}
	if (!ret)
		ret = cix_set_mode_locked(CIX_MODE_CURVE);
	mutex_unlock(&cix_lock);
	if (ret) {
		dev_err(&pdev->dev, "EC did not accept manual fan control (%d)\n",
			ret);
		cix_set_profile(CIX_FAN_PROFILE_AUTO);
		return ret;
	}

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
							"cix_fan_quiet", NULL,
							&cix_hwmon_chip, NULL);
	if (IS_ERR(hwmon_dev)) {
		cix_set_profile(CIX_FAN_PROFILE_AUTO);
		return PTR_ERR(hwmon_dev);
	}

	cix_dir = debugfs_create_dir("cixfan", NULL);
	debugfs_create_file("cmd", 0600, cix_dir, NULL, &cix_cmd_fops);

	schedule_delayed_work(&cix_work, msecs_to_jiffies(poll_ms));
	dev_info(&pdev->dev,
		 "quiet fan curve active (%u thermal zones, start %u%%)\n",
		 found, cix_curve[cix_step].percent);
	return 0;
}

static void cixfan_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&cix_work);
	debugfs_remove_recursive(cix_dir);

	mutex_lock(&cix_lock);
	cix_set_profile(CIX_FAN_PROFILE_AUTO);
	cix_manual_engaged = false;
	mutex_unlock(&cix_lock);

	dev_info(&pdev->dev, "firmware automatic fan profile restored\n");
}

static void cixfan_shutdown(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&cix_work);
	mutex_lock(&cix_lock);
	cix_set_profile(CIX_FAN_PROFILE_AUTO);
	cix_manual_engaged = false;
	mutex_unlock(&cix_lock);
}

static int __maybe_unused cixfan_suspend(struct device *dev)
{
	cancel_delayed_work_sync(&cix_work);
	mutex_lock(&cix_lock);
	cix_set_profile(CIX_FAN_PROFILE_AUTO);
	cix_manual_engaged = false;
	mutex_unlock(&cix_lock);
	return 0;
}

static int __maybe_unused cixfan_resume(struct device *dev)
{
	mutex_lock(&cix_lock);
	/* The EC came back in its own profile; re-take it on the next poll. */
	if (cix_mode == CIX_MODE_CURVE || cix_mode == CIX_MODE_MANUAL)
		cix_manual_engaged = false;
	mutex_unlock(&cix_lock);
	schedule_delayed_work(&cix_work, msecs_to_jiffies(500));
	return 0;
}

static SIMPLE_DEV_PM_OPS(cixfan_pm_ops, cixfan_suspend, cixfan_resume);

static const struct acpi_device_id cixfan_acpi_match[] = {
	{ "CIXHA024", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cixfan_acpi_match);

static struct platform_driver cixfan_driver = {
	.driver = {
		.name = "cix_fan_quiet",
		.acpi_match_table = cixfan_acpi_match,
		.pm = &cixfan_pm_ops,
	},
	.probe = cixfan_probe,
	.remove = cixfan_remove,
	.shutdown = cixfan_shutdown,
};
module_platform_driver(cixfan_driver);

MODULE_DESCRIPTION("Quiet CIX Sky1 EC fan control for ACPI boot");
MODULE_AUTHOR("Roy Kaandorp");
MODULE_LICENSE("GPL");
