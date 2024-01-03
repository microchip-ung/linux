// SPDX-License-Identifier: (GPL-2.0 OR MIT)

/* Copyright (C) 2022 Microchip Technology Inc. */

#include <crypto/sha2.h>
#include <linux/arm-smccc.h>
#include <linux/dma-map-ops.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>

#define	TFA_CTL_VERSION "0.0.0"

static DEFINE_MUTEX(mchp_tfa_mutex);

#define MCHP_SIP_SMC(func)	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, ARM_SMCCC_OWNER_SIP, func)
#define MCHP_SIP_SMC_UUID		MCHP_SIP_SMC(0xff01)
#define MCHP_SIP_SMC_VERSION		MCHP_SIP_SMC(0xff02)
#define MCHP_SIP_SMC_SJTAG_STATUS	MCHP_SIP_SMC(0xff03)
#define MCHP_SIP_SMC_SJTAG_CHALLENGE	MCHP_SIP_SMC(0xff04)
#define MCHP_SIP_SMC_SJTAG_UNLOCK	MCHP_SIP_SMC(0xff05)
#define MCHP_SIP_SMC_FW_BIND		MCHP_SIP_SMC(0xff06)

#define SHA256_DIGEST_SIZE	32
typedef struct {
        union {
		u8  b[SHA256_DIGEST_SIZE];
                u32 w[SHA256_DIGEST_SIZE / 4];
        };
} lan966x_key32_t;

struct tfa_ctl_data {
	/* SJTAG */
	lan966x_key32_t sjtag_key;
	lan966x_key32_t sjtag_challenge;
	lan966x_key32_t sjtag_response;
	/* FW_BIND */
	void *fw_bind_buf;
	size_t fw_bind_buf_len;
	size_t fw_bind_data_len;
};

struct tfa_ctl_dev {
	size_t	data_len;
	void	*data_ptr;
};

static void sjtag_derive_key(const void *in, const void *salt, u8 *out)
{
	u8 buf[SHA256_DIGEST_SIZE * 2];

	/* Use one contiguous buffer */
	memcpy(buf, in, SHA256_DIGEST_SIZE);
	memcpy(buf + SHA256_DIGEST_SIZE, salt, SHA256_DIGEST_SIZE);

	/* Derived key is an in+salt SHA */
	sha256(buf, sizeof(buf), out);

	/* Don't leak */
	memset(buf, 0, sizeof(buf));
}

static ssize_t sjtag_status_read(struct file *filp, struct kobject *kobj,
				 struct bin_attribute *bin_attr,
				 char *buffer, loff_t offset, size_t count)
{
	struct device *pdev = kobj_to_dev(kobj);
	struct arm_smccc_res res;
	u32 status_data[2];
	int ret;

	if (mutex_lock_interruptible(&mchp_tfa_mutex))
		return -ERESTARTSYS;

	/* Invoke SMC */
	arm_smccc_smc(MCHP_SIP_SMC_SJTAG_STATUS, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 != SMCCC_RET_SUCCESS) {
		ret = -EIO;
	} else {
		dev_info(pdev, "sjtag status = %08lx %08lx\n", res.a1, res.a2);
		status_data[0] = res.a1; /* CTL */
		status_data[1] = res.a2; /* INT_STATUS */
		ret = memory_read_from_buffer(buffer, count, &offset,
					      status_data, sizeof(status_data));
	}

	mutex_unlock(&mchp_tfa_mutex);

	return ret;
}

static int sjtag_get_challenge(struct device *pdev, struct tfa_ctl_data *drvdata)
{
	struct arm_smccc_res res;
	size_t size = SHA256_DIGEST_SIZE;
	void *sjtag_buf;
	phys_addr_t paddr;
	int ret;

	/* Get DMA buffer */
	sjtag_buf = kmalloc(size, GFP_KERNEL | GFP_DMA);
	if (!sjtag_buf)
		return -ENOMEM;

	/* Virt -> phys */
	paddr = __pa(sjtag_buf);

	/* Invoke SMC */
	arm_smccc_smc(MCHP_SIP_SMC_SJTAG_CHALLENGE, paddr, size, 0, 0, 0, 0, 0, &res);
	if (res.a0 != SMCCC_RET_SUCCESS) {
		ret = -EIO;
	} else {
		/* Invalidate to receive data */
		arch_sync_dma_for_cpu(paddr, size, DMA_FROM_DEVICE);

		/* copy data */
		memcpy(drvdata->sjtag_challenge.b, sjtag_buf, size);
	}
	return 0;
}

static int sjtag_send_unlock(struct device *pdev, struct tfa_ctl_data *drvdata)
{
	struct arm_smccc_res res;
	size_t size = SHA256_DIGEST_SIZE;
	void *sjtag_buf;
	phys_addr_t paddr;

	/* Get DMA buffer */
	sjtag_buf = kmalloc(size, GFP_KERNEL | GFP_DMA);
	if (!sjtag_buf)
		return -ENOMEM;

	/* Virt -> phys */
	paddr = __pa(sjtag_buf);

	/* Copy response data */
	memcpy(sjtag_buf, drvdata->sjtag_response.b, size);

	/* Flush data from cache data */
	arch_sync_dma_for_cpu(paddr, size, DMA_TO_DEVICE);

	/* Invoke SMC - Unlock */
	arm_smccc_smc(MCHP_SIP_SMC_SJTAG_UNLOCK, paddr, size, 0, 0, 0, 0, 0, &res);
	dev_info(pdev, "unlock = %08lx\n", res.a0);

	return res.a0 ? -EINVAL : 0;
}

static ssize_t sjtag_key_write(struct file *filp, struct kobject *kobj,
			       struct bin_attribute *attr,
			       char *buf, loff_t off, size_t count)
{
	struct device *pdev = kobj_to_dev(kobj);
	struct tfa_ctl_data *drvdata = dev_get_drvdata(pdev);
	int ret;

	ret = mutex_lock_interruptible(&mchp_tfa_mutex);
	if (ret)
		return -ERESTARTSYS;

	memcpy(drvdata->sjtag_key.b, buf, count);

	mutex_unlock(&mchp_tfa_mutex);

	return count;
}

static ssize_t sjtag_unlock_write(struct file *filp, struct kobject *kobj,
				  struct bin_attribute *attr,
				  char *buf, loff_t off, size_t count)
{
	struct device *pdev = kobj_to_dev(kobj);
	struct tfa_ctl_data *drvdata = dev_get_drvdata(pdev);
	int ret;

	ret = mutex_lock_interruptible(&mchp_tfa_mutex);
	if (ret)
		return -ERESTARTSYS;

	ret = sjtag_get_challenge(pdev, drvdata);
	if (ret) {
		dev_notice(pdev, "challenge failed: %d\n", ret);
	} else {
		sjtag_derive_key(drvdata->sjtag_challenge.b,
				 drvdata->sjtag_key.b, drvdata->sjtag_response.b);
		ret = sjtag_send_unlock(pdev, drvdata);
	}

	mutex_unlock(&mchp_tfa_mutex);

	return ret ?: count;
}

static ssize_t fw_bind_read(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *bin_attr,
			    char *buffer, loff_t offset, size_t count)
{
	struct device *pdev = kobj_to_dev(kobj);
	struct tfa_ctl_data *drvdata = dev_get_drvdata(pdev);
	int ret;

	if (mutex_lock_interruptible(&mchp_tfa_mutex))
		return -ERESTARTSYS;

	if (drvdata->fw_bind_buf)
		ret = memory_read_from_buffer(buffer, count, &offset,
					      drvdata->fw_bind_buf, drvdata->fw_bind_buf_len);
	else
		ret = -EIO;

	mutex_unlock(&mchp_tfa_mutex);

	return ret;
}

static ssize_t fw_bind_write(struct file *filp, struct kobject *kobj,
			     struct bin_attribute *attr,
			     char *buf, loff_t off, size_t count)
{
	struct device *pdev = kobj_to_dev(kobj);
	struct tfa_ctl_data *drvdata = dev_get_drvdata(pdev);
	int ret;

	if (mutex_lock_interruptible(&mchp_tfa_mutex))
		return -ERESTARTSYS;

	if (off == 0) {
		/* Free previous data, if any */
		if (drvdata->fw_bind_buf) {
			kfree(drvdata->fw_bind_buf);
			drvdata->fw_bind_buf = NULL;
			drvdata->fw_bind_buf_len = 0;
			drvdata->fw_bind_data_len = 0;
		}
	}

	/* Truncate? */
	if (count == 0) {
		ret = 0;
		goto out_done;
	}

	/* Contiguous? */
	if (off != drvdata->fw_bind_data_len) {
		dev_err(pdev, "Non-contiguous write at offset %llx, length is %zx\n",
			off, drvdata->fw_bind_data_len);
		ret = -EIO;
		goto out_done;
	}

	/* Can we fit data? */
	if ((drvdata->fw_bind_data_len + count) > drvdata->fw_bind_buf_len) {
		/* Increase buffer len */
		drvdata->fw_bind_buf_len += SZ_1M;
		/* realloc */
		drvdata->fw_bind_buf =
			devm_krealloc(pdev, drvdata->fw_bind_buf,
				      drvdata->fw_bind_buf_len, GFP_KERNEL);
	}

	if (drvdata->fw_bind_buf) {
		/* How much can we fit? */
		count = min(count, drvdata->fw_bind_buf_len - drvdata->fw_bind_data_len);
		/* Data into buffer */
		memcpy(drvdata->fw_bind_buf + drvdata->fw_bind_data_len, buf, count);
		/* Update length */
		drvdata->fw_bind_data_len += count;
		ret = count;
	} else {
		/* Error, truncate length */
		drvdata->fw_bind_data_len = drvdata->fw_bind_buf_len = 0;
		ret = -ENOMEM;
	}

out_done:
	mutex_unlock(&mchp_tfa_mutex);

	return ret;
}

static ssize_t fw_bind_trigger_write(struct file *filp, struct kobject *kobj,
				     struct bin_attribute *attr,
				     char *buf, loff_t off, size_t count)
{
	struct device *pdev = kobj_to_dev(kobj);
	struct tfa_ctl_data *drvdata = dev_get_drvdata(pdev);
	int ret;

	if (mutex_lock_interruptible(&mchp_tfa_mutex))
		return -ERESTARTSYS;

	if (drvdata->fw_bind_buf) {
		void *fw_buf = kmalloc(drvdata->fw_bind_data_len, GFP_KERNEL | GFP_DMA);

		if (!fw_buf) {
			ret = -ENOMEM;
		} else {
			phys_addr_t paddr = __pa(fw_buf);
			struct arm_smccc_res res;

			/* Copy to dma buffer */
			memcpy(fw_buf, drvdata->fw_bind_buf, drvdata->fw_bind_data_len);

			/* Flush Data */
			arch_sync_dma_for_device(paddr, drvdata->fw_bind_data_len, DMA_TO_DEVICE);

			/* Invoke PSCI 'bind firmware' */
			arm_smccc_smc(MCHP_SIP_SMC_FW_BIND, paddr, drvdata->fw_bind_data_len, 0, 0, 0, 0, 0, &res);

			/* How did it go? */
			if ((int)res.a0 != SMCCC_RET_SUCCESS) {
				dev_err(pdev, "tfa_ctl: Bind failed: 0x%08lx\n", res.a0);
				ret = -EIO;
			} else {
				/* Invalidate to receive data */
				arch_sync_dma_for_cpu(paddr, drvdata->fw_bind_data_len, DMA_FROM_DEVICE);

				/* Copy back data */
				memcpy(drvdata->fw_bind_buf, fw_buf, drvdata->fw_bind_data_len);

				/* All went well */
				ret = count;
			}

			/* Dispose DMA buffer */
			kfree(fw_buf);
		}
	}

	mutex_unlock(&mchp_tfa_mutex);

	return ret;
}

BIN_ATTR_RO(sjtag_status, 8);
BIN_ATTR_WO(sjtag_key, SHA256_DIGEST_SIZE);
BIN_ATTR_WO(sjtag_unlock, 0);
BIN_ATTR_RW(fw_bind, 0);
BIN_ATTR_WO(fw_bind_trigger, 0);

static struct bin_attribute *tfa_ctl_attrs[] = {
	&bin_attr_sjtag_status,
	&bin_attr_sjtag_key,
	&bin_attr_sjtag_unlock,
	&bin_attr_fw_bind,
	&bin_attr_fw_bind_trigger,
	NULL,
};

BIN_ATTRIBUTE_GROUPS(tfa_ctl);

static struct miscdevice tfa_ctl_miscdev =
{
	MICROCODE_MINOR,
	"tfa_ctl",
};

static int __init tfa_ctl_init(void)
{
	int ret;

	pr_info("tfa_ctl: PSCI driver v.%s\n", TFA_CTL_VERSION);

	ret = misc_register(&tfa_ctl_miscdev);
	if (!ret) {
		struct device *pdev = tfa_ctl_miscdev.this_device;
		struct tfa_ctl_data *drvdata = devm_kzalloc(pdev, sizeof(*drvdata), GFP_KERNEL);

		if (drvdata)
			dev_set_drvdata(pdev, drvdata);
		else
			ret = -ENOMEM;

		ret = device_add_groups(pdev, tfa_ctl_groups);
	}

	return ret;
}

static void __exit tfa_ctl_exit(void)
{
	struct device *pdev = tfa_ctl_miscdev.this_device;

	device_remove_groups(pdev, tfa_ctl_groups);

	misc_deregister(&tfa_ctl_miscdev);
}

module_init(tfa_ctl_init);
module_exit(tfa_ctl_exit);

MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
MODULE_DESCRIPTION("Firmware binding driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(TFA_CTL_VERSION);
