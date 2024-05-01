/*
 * bq28z610 fuel gauge driver
 *
 * Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"[bq28z610] %s: " fmt, __func__
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>

#define	FG_UPDATER
#undef  FG_UPDATER

#ifdef FG_UPDATER
#include "bqfs_cmd_type.h"
#include "bqfs_sample.h"
#endif

#define bq_info	pr_info
#define bq_dbg	pr_debug
#define bq_err	pr_err
#define bq_log	pr_err


#define	INVALID_REG_ADDR	0xFF
#define BQFS_UPDATE_KEY		0x8F91

#define BQ_FG_I2C_ROM_ADDR    (0x16 >> 1)
#define BQ_FG_I2C_DEV_ADDR    (0xAA >> 1)

#define FG_FLAGS_FD				BIT(4)
#define	FG_FLAGS_FC				BIT(5)
#define	FG_FLAGS_DSG				BIT(6)
#define FG_FLAGS_RCA				BIT(9)

enum {
	UPDATE_REASON_FG_RESET = 1,
	UPDATE_REASON_NEW_VERSION,
	UPDATE_REASON_FORCED,
};



enum bq_fg_reg_idx {
	BQ_FG_REG_CTRL = 0,
	BQ_FG_REG_TEMP,		/* Battery Temperature */
	BQ_FG_REG_VOLT,		/* Battery Voltage */
	BQ_FG_REG_AI,		/* Average Current */
	BQ_FG_REG_BATT_STATUS,	/* BatteryStatus */
	BQ_FG_REG_TTE,		/* Time to Empty */
	BQ_FG_REG_TTF,		/* Time to Full */
	BQ_FG_REG_FCC,		/* Full Charge Capacity */
	BQ_FG_REG_RM,		/* Remaining Capacity */
	BQ_FG_REG_CC,		/* Cycle Count */
	BQ_FG_REG_SOC,		/* Relative State of Charge */
	BQ_FG_REG_SOH,		/* State of Health */
	BQ_FG_REG_DC,		/* Design Capacity */
	BQ_FG_REG_ALT_MAC,	/* AltManufactureAccess*/
	BQ_FG_REG_MAC_CHKSUM,	/* MACChecksum */
	NUM_REGS,
};

enum bq_fg_mac_cmd {
	FG_MAC_CMD_CTRL_STATUS	= 0x0000,
	FG_MAC_CMD_DEV_TYPE	= 0x0001,
	FG_MAC_CMD_FW_VER	= 0x0002,
	FG_MAC_CMD_HW_VER	= 0x0003,
	FG_MAC_CMD_IF_SIG	= 0x0004,
	FG_MAC_CMD_CHEM_ID	= 0x0006,
	FG_MAC_CMD_GAUGING	= 0x0021,
	FG_MAC_CMD_SEAL		= 0x0030,
	FG_MAC_CMD_DEV_RESET	= 0x0041,
	FG_MAC_CMD_ENTER_ROM	= 0x0F00,
};


enum {
	SEAL_STATE_RSVED,
	SEAL_STATE_UNSEALED,
	SEAL_STATE_SEALED,
	SEAL_STATE_FA,
};


enum bq_fg_device {
	BQ28Z610,
};

#ifdef FG_UPDATER
struct fg_batt_profile {
	const bqfs_cmd_t  *bqfs_image;
	u32		   array_size;
	u8		   version;
};

static const struct fg_batt_profile bqfs_image[] = {
	{ bqfs_coslight, ARRAY_SIZE(bqfs_coslight), 1 },
};

static const unsigned char *update_reason_str[] = {
	"Reset", 
	"New Version", 
	"Force" 
};
#endif

static const unsigned char *device2str[] = {
	"bq28z610",
};

static u8 bq28z610_regs[NUM_REGS] = {
	0x00,	/* CONTROL */
	0x06,	/* TEMP */
	0x08,	/* VOLT */
	0x14,	/* AVG CURRENT */
	0x0A,	/* FLAGS */
	0x16,	/* Time to empty */
	0x18,	/* Time to full */
	0x12,	/* Full charge capacity */
	0x10,	/* Remaining Capacity */
	0x2A,	/* CycleCount */
	0x2C,	/* State of Charge */
	0x2E,	/* State of Health */
	0x3C,	/* Design Capacity */
	0x3E,	/* AltManufacturerAccess*/
	0x60,	/* MACChecksum */
};

struct bq_fg_chip {
	struct device *dev;
	struct i2c_client *client;


	struct mutex i2c_rw_lock;
	struct mutex data_lock;
	struct mutex update_lock;
	struct mutex irq_complete;

	bool irq_waiting;
	bool irq_disabled;
	bool resume_completed;

	int fw_ver;
	int df_ver;

	u8 chip;
	u8 regs[NUM_REGS];

	/* status tracking */

	bool batt_fc;
	bool batt_fd;

	bool batt_dsg;
	bool batt_rca;	/* remaining capacity alarm */

	int seal_state;
	int batt_tte;
	int batt_soc;
	int batt_fcc;	/* Full charge capacity */
	int batt_rm;	/* Remaining capacity */
	int batt_dc;	/* Design Capacity */
	int batt_volt;
	int batt_temp;
	int batt_curr;

	int batt_cyclecnt;	/* cycle count */
#ifdef FG_UPDATER
	struct work_struct update_work;
#endif	
	int force_update;	
	int batt_id;

	/* debug */
	int skip_reads;
	int skip_writes;

	int fake_soc;
	int fake_temp;

	struct power_supply *fg_psy;
	struct power_supply_desc fg_psy_d;
};



static int __fg_read_byte(struct i2c_client *client, u8 reg, u8 *val)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		bq_err("i2c read byte fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*val = (u8)ret;

	return 0;
}

#if 0
static int __fg_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(client, reg, val);
	if (ret < 0) {
		bq_err("i2c write byte fail: can't write 0x%02X to reg 0x%02X\n",
				val, reg);
		return ret;
	}

	return 0;
}
#endif

static int __fg_read_word(struct i2c_client *client, u8 reg, u16 *val)
{
	s32 ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0) {
		bq_err("i2c read word fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*val = (u16)ret;

	return 0;
}


static int __fg_write_word(struct i2c_client *client, u8 reg, u16 val)
{
	s32 ret;

	ret = i2c_smbus_write_word_data(client, reg, val);
	if (ret < 0) {
		bq_err("i2c write word fail: can't write 0x%02X to reg 0x%02X\n",
				val, reg);
		return ret;
	}

	return 0;
}

static int __fg_read_block(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{

	int ret;


	ret = i2c_smbus_read_i2c_block_data(client, reg, len, buf);

	return ret;
}

static int __fg_write_block(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{
	int ret;

	ret = i2c_smbus_write_i2c_block_data(client, reg, len, buf);

	return ret;
}


static int fg_read_byte(struct bq_fg_chip *bq, u8 reg, u8 *val)
{
	int ret;

	if (bq->skip_reads) {
		*val = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_byte(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

#if 0
static int fg_write_byte(struct bq_fg_chip *bq, u8 reg, u8 val)
{
	int ret;

	if (bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_byte(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}
#endif

static int fg_read_word(struct bq_fg_chip *bq, u8 reg, u16 *val)
{
	int ret;

	if (bq->skip_reads) {
		*val = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_word(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int fg_write_word(struct bq_fg_chip *bq, u8 reg, u16 val)
{
	int ret;

	if (bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_word(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int fg_read_block(struct bq_fg_chip *bq, u8 reg, u8 *buf, u8 len)
{
	int ret;

	if (bq->skip_reads)
		return 0;
	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_block(bq->client, reg, buf, len);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;

}

static int fg_write_block(struct bq_fg_chip *bq, u8 reg, u8 *data, u8 len)
{
	int ret;

	if (bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_block(bq->client, reg, data, len);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static u8 checksum(u8 *data, u8 len)
{
	u8 i;
	u16 sum = 0;

	for (i = 0; i < len; i++)
		sum += data[i];

	sum &= 0xFF;

	return 0xFF - sum;
}

#if 0
static void fg_print_buf(const char *msg, u8 *buf, u8 len)
{
	int i;
	int idx = 0;
	int num;
	u8 strbuf[128];

	bq_err("%s buf: ", msg);
	for (i = 0; i < len; i++) {
		num = sprintf(&strbuf[idx], "%02X ", buf[i]);
		idx += num;
	}
	bq_err("%s\n", strbuf);
}
#else
static void fg_print_buf(const char *msg, u8 *buf, u8 len)
{}
#endif

#if 0
#define TIMEOUT_INIT_COMPLETED	100
static int fg_check_init_completed(struct bq_fg_chip *bq)
{
	int ret;
	int i = 0;
	u16 status;

	while (i++ < TIMEOUT_INIT_COMPLETED) {
		ret = fg_read_word(bq, bq->regs[BQ_FG_REG_BATT_STATUS],
				&status);
		if (ret >= 0 && (status & 0x0080))
			return 0;
		msleep(100);
	}
	bq_err("wait for FG INITCOMP timeout\n");
	return ret;
}
#endif

#ifdef FG_UPDATER
static int fg_get_seal_state(struct bq_fg_chip *bq)
{
	int ret;
	u16 status;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CTRL], &status);
	if (ret < 0) {
		bq_err("Failed to read control status, ret = %d\n", ret);
		return ret;
	}
	status &= 0x6000;
	status >>= 13;

	if (status == 1)
		bq->seal_state = SEAL_STATE_FA;
	else if (status == 2)
		bq->seal_state = SEAL_STATE_UNSEALED;
	else if (status == 3)
		bq->seal_state = SEAL_STATE_SEALED;

	return 0;
}

static int fg_unseal_send_key(struct bq_fg_chip *bq, u32 key)
{
	int ret;

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_ALT_MAC], key & 0xFFFF);

	if (ret < 0) {
		bq_err("unable to write unseal key step 1, ret = %d\n", ret);
		return ret;
	}

	msleep(5);

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_ALT_MAC],
				(key >> 16) & 0xFFFF);
	if (ret < 0) {
		bq_err("unable to write unseal key step 2, ret = %d\n", ret);
		return ret;
	}

	msleep(100);

	return 0;
}

#define FG_DEFAULT_UNSEAL_KEY	0x80008000
static int fg_unseal(struct bq_fg_chip *bq)
{
	int ret;
	int retry = 0;

	ret = fg_unseal_send_key(bq, FG_DEFAULT_UNSEAL_KEY);
	if (!ret) {
		while (retry++ < 100) {
			ret = fg_get_seal_state(bq);
			if (bq->seal_state == SEAL_STATE_UNSEALED ||
			    bq->seal_state == SEAL_STATE_FA) {
				bq_log("FG is unsealed");
				return 0;
			}
		}
	}

	return -1;
}

#define FG_DEFAULT_UNSEAL_FA_KEY	0x36724614
static int fg_unseal_full_access(struct bq_fg_chip *bq)
{
	int ret;
	int retry = 0;

	ret = fg_unseal_send_key(bq, FG_DEFAULT_UNSEAL_FA_KEY);
	if (!ret) {
		while (retry++ < 100) {
			fg_get_seal_state(bq);
			if (bq->seal_state == SEAL_STATE_FA) {
				bq_log("FG is in full access.");
				return 0;
			}
			msleep(200);
		}
	}

	return -1;
}


static int fg_seal(struct bq_fg_chip *bq)
{
	int ret;
	int retry = 0;

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_ALT_MAC], FG_MAC_CMD_SEAL);

	if (ret < 0) {
		bq_err("Failed to send seal command\n");
		return ret;
	}

	while (retry++ < 100) {
		fg_get_seal_state(bq);
		if (bq->seal_state == SEAL_STATE_SEALED) {
			bq_log("FG is sealed successfully");
			return 0;
		}
		msleep(200);
	}

	return -1;
}

#endif

static int fg_mac_read_block(struct bq_fg_chip *bq, u16 cmd, u8 *buf, u8 len)
{
	int ret;
	u8 cksum_calc, cksum;
	u8 t_buf[40];
	u8 t_len;
	int i;

	t_buf[0] = (u8)(cmd >> 8);
	t_buf[1] = (u8)cmd;
	ret = fg_write_block(bq, bq->regs[BQ_FG_REG_ALT_MAC], t_buf, 2);
	if (ret < 0)
		return ret;

	msleep(100);

	ret = fg_read_block(bq, bq->regs[BQ_FG_REG_ALT_MAC], t_buf, 36);
	if (ret < 0)
		return ret;

	fg_print_buf("mac_read_block", t_buf, 36);

	cksum = t_buf[34];
	t_len = t_buf[35];

	cksum_calc = checksum(t_buf, t_len - 2);
	if (cksum_calc != cksum)
		return 1;

	for (i = 0; i < len; i++)
		buf[i] = t_buf[i+2];

	return 0;
}

#ifdef FG_UPDATER
static int fg_mac_write_block(struct bq_fg_chip *bq, u16 cmd, u8 *data, u8 len)
{
	int ret;
	u8 cksum;
	u8 t_buf[40];
	int i;

	if (len > 32)
		return -1;

	t_buf[0] = (u8)(cmd >> 8);
	t_buf[1] = (u8)cmd;
	for (i = 0; i < len; i++)
		t_buf[i+2] = data[i];

	cksum = checksum(t_buf, len + 2);
	/*write command/addr, data*/
	ret = fg_write_block(bq, bq->regs[BQ_FG_REG_ALT_MAC], t_buf, len + 2);
	if (ret < 0)
		return ret;
	t_buf[0] = cksum;
	t_buf[1] = len + 4; /*buf length, cmd, CRC and len byte itself*/
	/*write checksum and length*/
	ret = fg_write_block(bq, bq->regs[BQ_FG_REG_MAC_CHKSUM], t_buf, 2);

	return ret;
}
#endif

static void fg_read_fw_version(struct bq_fg_chip *bq)
{

	int ret;
	u8 buf[36];

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_ALT_MAC], FG_MAC_CMD_FW_VER);

	if (ret < 0) {
		bq_err("Failed to send firmware version subcommand:%d\n", ret);
		return;
	}

	mdelay(2);

	ret = fg_mac_read_block(bq, bq->regs[BQ_FG_REG_ALT_MAC], buf, 11);
	if (ret < 0) {
		bq_err("Failed to read firmware version:%d\n", ret);
		return;
	}

	bq_log("FW Ver:%04X, Build:%04X\n",
		buf[2] << 8 | buf[3], buf[4] << 8 | buf[5]);
	bq_log("Ztrack Ver:%04X\n", buf[7] << 8 | buf[8]);
}


static int fg_read_status(struct bq_fg_chip *bq)
{
	int ret;
	u16 flags;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_BATT_STATUS], &flags);
	if (ret < 0)
		return ret;

	mutex_lock(&bq->data_lock);
	bq->batt_fc		= !!(flags & FG_FLAGS_FC);
	bq->batt_fd		= !!(flags & FG_FLAGS_FD);
	bq->batt_rca		= !!(flags & FG_FLAGS_RCA);
	bq->batt_dsg		= !!(flags & FG_FLAGS_DSG);
	mutex_unlock(&bq->data_lock);

	return 0;
}


static int fg_read_rsoc(struct bq_fg_chip *bq)
{
	int ret;
	u16 soc = 0;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_SOC], &soc);
	if (ret < 0) {
		bq_err("could not read RSOC, ret = %d\n", ret);
		return ret;
	}

	return soc;

}

static int fg_read_temperature(struct bq_fg_chip *bq)
{
	int ret;
	u16 temp = 0;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_TEMP], &temp);
	if (ret < 0) {
		bq_err("could not read temperature, ret = %d\n", ret);
		return ret;
	}

	return temp - 2730;

}

static int fg_read_volt(struct bq_fg_chip *bq)
{
	int ret;
	u16 volt = 0;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_VOLT], &volt);
	if (ret < 0) {
		bq_err("could not read voltage, ret = %d\n", ret);
		return ret;
	}

	return volt;

}

static int fg_read_current(struct bq_fg_chip *bq, int *curr)
{
	int ret;
	u16 avg_curr = 0;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_AI], &avg_curr);
	if (ret < 0) {
		bq_err("could not read current, ret = %d\n", ret);
		return ret;
	}
	*curr = (int)((s16)avg_curr);

	return ret;
}

static int fg_read_fcc(struct bq_fg_chip *bq)
{
	int ret;
	u16 fcc;

	if (bq->regs[BQ_FG_REG_FCC] == INVALID_REG_ADDR) {
		bq_err("FCC command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_FCC], &fcc);

	if (ret < 0)
		bq_err("could not read FCC, ret=%d\n", ret);

	return fcc;
}

static int fg_read_dc(struct bq_fg_chip *bq)
{

	int ret;
	u16 dc;

	if (bq->regs[BQ_FG_REG_DC] == INVALID_REG_ADDR) {
		bq_err("DesignCapacity command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_DC], &dc);

	if (ret < 0) {
		bq_err("could not read DC, ret=%d\n", ret);
		return ret;
	}

	return dc;
}


static int fg_read_rm(struct bq_fg_chip *bq)
{
	int ret;
	u16 rm;

	if (bq->regs[BQ_FG_REG_RM] == INVALID_REG_ADDR) {
		bq_err("RemainingCapacity command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_RM], &rm);

	if (ret < 0) {
		bq_err("could not read DC, ret=%d\n", ret);
		return ret;
	}

	return rm;

}

static int fg_read_cyclecount(struct bq_fg_chip *bq)
{
	int ret;
	u16 cc;

	if (bq->regs[BQ_FG_REG_CC] == INVALID_REG_ADDR) {
		bq_err("Cycle Count not supported!\n");
		return -1;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CC], &cc);

	if (ret < 0) {
		bq_err("could not read Cycle Count, ret=%d\n", ret);
		return ret;
	}

	return cc;
}

static int fg_read_tte(struct bq_fg_chip *bq)
{
	int ret;
	u16 tte;

	if (bq->regs[BQ_FG_REG_TTE] == INVALID_REG_ADDR) {
		bq_err("Time To Empty not supported!\n");
		return -1;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_TTE], &tte);

	if (ret < 0) {
		bq_err("could not read Time To Empty, ret=%d\n", ret);
		return ret;
	}

	if (ret == 0xFFFF)
		return -ENODATA;

	return tte;
}

static int fg_get_batt_status(struct bq_fg_chip *bq)
{

	fg_read_status(bq);

	if (bq->batt_fc)
		return POWER_SUPPLY_STATUS_FULL;
	else if (bq->batt_dsg)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	else if (bq->batt_curr > 0)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;

}


static int fg_get_batt_capacity_level(struct bq_fg_chip *bq)
{

	if (bq->batt_fc)
		return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (bq->batt_rca)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (bq->batt_fd)
		return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

}


static enum power_supply_property fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	/*POWER_SUPPLY_PROP_HEALTH,*//*implement it in battery power_supply*/
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_RESISTANCE_ID,
	POWER_SUPPLY_PROP_UPDATE_NOW,
};

static int fg_get_property(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	struct bq_fg_chip *bq = power_supply_get_drvdata(psy);
	int ret;

	mutex_lock(&bq->update_lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = fg_get_batt_status(bq);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = fg_read_volt(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_volt = ret;
		val->intval = bq->batt_volt * 1000;
		mutex_unlock(&bq->data_lock);

		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		mutex_lock(&bq->data_lock);
		fg_read_current(bq, &bq->batt_curr);
		val->intval = -bq->batt_curr * 1000;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		if (bq->fake_soc >= 0) {
			val->intval = bq->fake_soc;
			break;
		}
		ret = fg_read_rsoc(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_soc = ret;
		val->intval = bq->batt_soc;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = fg_get_batt_capacity_level(bq);
		break;

	case POWER_SUPPLY_PROP_TEMP:
		if (bq->fake_temp != -EINVAL) {
			val->intval = bq->fake_temp;
			break;
		}
		ret = fg_read_temperature(bq);
		mutex_lock(&bq->data_lock);
		if (ret > 0)
			bq->batt_temp = ret;
		val->intval = bq->batt_temp;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = fg_read_tte(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_tte = ret;

		val->intval = bq->batt_tte;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = fg_read_fcc(bq);
		mutex_lock(&bq->data_lock);
		if (ret > 0)
			bq->batt_fcc = ret;
		val->intval = bq->batt_fcc * 1000;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = fg_read_dc(bq);
		mutex_lock(&bq->data_lock);
		if (ret > 0)
			bq->batt_dc = ret;
		val->intval = bq->batt_dc * 1000;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = fg_read_cyclecount(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_cyclecnt = ret;
		val->intval = bq->batt_cyclecnt;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;

	case POWER_SUPPLY_PROP_RESISTANCE_ID:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		val->intval = 0;
		break;

	default:
		mutex_unlock(&bq->update_lock);
		return -EINVAL;
	}

	mutex_unlock(&bq->update_lock);

	return 0;
}
static void fg_dump_registers(struct bq_fg_chip *bq);

static int fg_set_property(struct power_supply *psy,
			       enum power_supply_property prop,
			       const union power_supply_propval *val)
{
	struct bq_fg_chip *bq = power_supply_get_drvdata(psy);

	mutex_lock(&bq->update_lock);

	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
		bq->fake_temp = val->intval;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		bq->fake_soc = val->intval;
		power_supply_changed(bq->fg_psy);
		break;
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		fg_dump_registers(bq);
		break;
	default:
		mutex_unlock(&bq->update_lock);
		return -EINVAL;
	}

	mutex_unlock(&bq->update_lock);

	return 0;
}


static int fg_prop_is_writeable(struct power_supply *psy,
				       enum power_supply_property prop)
{
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		ret = 1;
		break;
	default:
		ret = 0;
		break;
	}
	return ret;
}



static int fg_psy_register(struct bq_fg_chip *bq)
{
	struct power_supply_config fg_psy_cfg = {};

	bq->fg_psy_d.name = "bms";
	bq->fg_psy_d.type = POWER_SUPPLY_TYPE_BMS;
	bq->fg_psy_d.properties = fg_props;
	bq->fg_psy_d.num_properties = ARRAY_SIZE(fg_props);
	bq->fg_psy_d.get_property = fg_get_property;
	bq->fg_psy_d.set_property = fg_set_property;
	bq->fg_psy_d.property_is_writeable = fg_prop_is_writeable;

	fg_psy_cfg.drv_data = bq;
	fg_psy_cfg.num_supplicants = 0;
	bq->fg_psy = devm_power_supply_register(bq->dev,
						&bq->fg_psy_d,
						&fg_psy_cfg);
	if (IS_ERR(bq->fg_psy)) {
		bq_err("Failed to register fg_psy");
		return PTR_ERR(bq->fg_psy);
	}
	return 0;
}


static void fg_psy_unregister(struct bq_fg_chip *bq)
{

	power_supply_unregister(bq->fg_psy);
}

static const u8 fg_dump_regs[] = {
	0x00, 0x02, 0x04, 0x06,
	0x08, 0x0A, 0x0C, 0x0E,
	0x10, 0x16, 0x18, 0x1A,
	0x1C, 0x1E, 0x20, 0x28,
	0x2A, 0x2C, 0x2E, 0x30,
	0x66, 0x68, 0x6C, 0x6E,
	0x70,
};

static ssize_t fg_attr_show_Ra_table(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);
	u8 t_buf[40];
	u8 temp_buf[40];
	int ret;
	int i, idx, len;

	mutex_lock(&bq->update_lock);

	ret = fg_mac_read_block(bq, 0x40C0, t_buf, 32);
	if (ret < 0) {
		mutex_unlock(&bq->update_lock);
		return 0;
	}

	idx = 0;
	len = sprintf(temp_buf, "Ra Flag:0x%02X\n", t_buf[0] << 8 | t_buf[1]);
	memcpy(&buf[idx], temp_buf, len);
	idx += len;
	len = sprintf(temp_buf, "RaTable:\n");
	memcpy(&buf[idx], temp_buf, len);
	idx += len;
	for (i = 1; i < 16; i++) {
		len =
		    sprintf(temp_buf, "%d ", t_buf[i*2] << 8 | t_buf[i*2 + 1]);
		memcpy(&buf[idx], temp_buf, len);
		idx += len;
	}

	mutex_unlock(&bq->update_lock);

	return idx;
}

static ssize_t fg_attr_show_Qmax(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);
	int ret;
	u8 t_buf[64];
	int len;

	memset(t_buf, 0, 64);

	mutex_lock(&bq->update_lock);

	ret = fg_mac_read_block(bq, 0x4146, t_buf, 2);
	if (ret < 0) {
		mutex_unlock(&bq->update_lock);
		return 0;
	}

	len =
	    sprintf(buf, "Qmax Cell 0 = %d\n", (t_buf[0] << 8) | t_buf[1]);

	mutex_unlock(&bq->update_lock);

	return len;
}

#ifdef FG_UPDATER
static ssize_t fg_attr_store_update(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{

	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);
	unsigned int key = 0;

	sscanf(buf, "%x", &key);
	if (key == BQFS_UPDATE_KEY) {
		bq->force_update = ~key;
		schedule_work(&bq->update_work);
	}
	return count;	
}
#endif

static DEVICE_ATTR(RaTable, S_IRUGO, fg_attr_show_Ra_table, NULL);
static DEVICE_ATTR(Qmax, S_IRUGO, fg_attr_show_Qmax, NULL);
#ifdef FG_UPDATER
static DEVICE_ATTR(update, S_IWUSR, NULL, fg_attr_store_update);
#endif
static struct attribute *fg_attributes[] = {
	&dev_attr_RaTable.attr,
	&dev_attr_Qmax.attr,
#ifdef FG_UPDATER
	&dev_attr_update.attr,
#endif
	NULL,
};

static const struct attribute_group fg_attr_group = {
	.attrs = fg_attributes,
};

#ifdef FG_UPDATER
static void fg_update_bqfs(struct bq_fg_chip *bq);
static void fg_update_bqfs_workfunc(struct work_struct *work)
{
	struct bq_fg_chip *bq = container_of(work, 
				struct bq_fg_chip, update_work);
	
	fg_update_bqfs(bq);
}
#endif

static void fg_dump_registers(struct bq_fg_chip *bq)
{
	int i;
	int ret;
	u16 val;

	for (i = 0; i < ARRAY_SIZE(fg_dump_regs); i++) {
		msleep(5);
		ret = fg_read_word(bq, fg_dump_regs[i], &val);
		if (!ret)
			bq_err("Reg[%02X] = 0x%04X\n", fg_dump_regs[i], val);
	}
}

static irqreturn_t fg_irq_thread(int irq, void *dev_id)
{
	struct bq_fg_chip *bq = dev_id;

	mutex_lock(&bq->irq_complete);
	bq->irq_waiting = true;
	if (!bq->resume_completed) {
		pr_info("IRQ triggered before device resume\n");
		if (!bq->irq_disabled) {
			disable_irq_nosync(irq);
			bq->irq_disabled = true;
		}
		mutex_unlock(&bq->irq_complete);
		return IRQ_HANDLED;
	}
	bq->irq_waiting = false;

	mutex_lock(&bq->update_lock);

	fg_read_status(bq);
	fg_dump_registers(bq);

	mutex_lock(&bq->data_lock);

	bq->batt_soc = fg_read_rsoc(bq);
	bq->batt_volt = fg_read_volt(bq);
	fg_read_current(bq, &bq->batt_curr);
	bq->batt_temp = fg_read_temperature(bq);
	bq->batt_rm = fg_read_rm(bq);

	mutex_unlock(&bq->data_lock);

	mutex_unlock(&bq->update_lock);

	bq_log("RSOC:%d, Volt:%d, Current:%d, Temperature:%d\n",
		bq->batt_soc, bq->batt_volt, bq->batt_curr, bq->batt_temp);

	return IRQ_HANDLED;
}


#ifdef FG_UPDATER
static bool fg_check_rom_mode(struct bq_fg_chip *bq)
{
	struct i2c_client *client = to_i2c_client(bq->dev);
	u8 val;
	int ret;

	client->addr = BQ_FG_I2C_ROM_ADDR;
	ret = fg_read_byte(bq, 0x66, &val);
	mdelay(2);
	client->addr = BQ_FG_I2C_DEV_ADDR;	/* restore address */
	if (ret < 0)
		return false;

	return true;
}

static bool fg_enter_rom_mode(struct bq_fg_chip *bq)
{
	int ret;

	ret = fg_write_word(bq, BQ_FG_REG_ALT_MAC,
			FG_MAC_CMD_ENTER_ROM);
	mdelay(2);
	if (ret < 0)
		return false;

	return fg_check_rom_mode(bq);
}

#define BQ28Z610_DEVICE_NAME_ADDRESS     0x4080
#define BQ28Z610_DEVICE_NAME_LENGTH      7

static int fg_check_update_necessary(struct bq_fg_chip *bq)
{
	/* TODO:this is application specific, customer determine how to 
	 * identify version and check if need to update gauge parameter*/
	u8 buf[40];
	int ret;

	ret =
	    fg_mac_read_block(bq, BQ28Z610_DEVICE_NAME_ADDRESS, buf,
			     BQ28Z610_DEVICE_NAME_LENGTH);
	if (ret)
		return 0;
	if (strncmp(buf, "TEST001", BQ28Z610_DEVICE_NAME_LENGTH) == 0)
		return 0;
	else
		return UPDATE_REASON_NEW_VERSION;
}

static bool fg_mark_as_updated(struct bq_fg_chip *bq)
{
	/* TODO:this is application specific */
	int ret;

	ret =
	    fg_mac_write_block(bq, BQ28Z610_DEVICE_NAME_ADDRESS, "TEST001",
			      BQ28Z610_DEVICE_NAME_LENGTH);
	if (ret < 0)
		return false;
	else
		return true;
}

static bool fg_update_execute_cmd(struct bq_fg_chip *bq,
					const bqfs_cmd_t *cmd)
{
	int ret;
	uint8_t tmp_buf[CMD_MAX_DATA_SIZE];

	switch (cmd->cmd_type) {
	case CMD_R:
		ret =
		    fg_read_block(bq, cmd->reg, (u8 *) &cmd->data.bytes,
				    cmd->data_len);
		if (ret < 0)
			return false;

		return true;

	case CMD_W:
		ret =
		    fg_write_block(bq, cmd->reg, (u8 *) &cmd->data.bytes,
				     cmd->data_len);
		if (ret < 0)
			return false;

		return true;

	case CMD_C:
		if (fg_read_block(bq, cmd->reg, tmp_buf, cmd->data_len) < 0)
			return false;	/* read fail */

		if (memcmp(tmp_buf, cmd->data.bytes, cmd->data_len)) {
			bq_err("\nCommand C failed at line %d:\n",
				cmd->line_num);
			return false;
		}

		return true;

	case CMD_X:
		mdelay(cmd->data.delay);
		return true;

	default:
		bq_err("Unsupported command at line %d\n",
			cmd->line_num);
		return false;
	}
}

static void fg_update_bqfs(struct bq_fg_chip *bq)
{
	struct i2c_client *client = to_i2c_client(bq->dev);
	u16 i;
	const bqfs_cmd_t *image;
	int reason = 0;

	if (fg_check_rom_mode(bq))
		goto update;	/* already in rom mode */

	if (bq->force_update == ~BQFS_UPDATE_KEY)
		reason = UPDATE_REASON_FORCED;
	else
		reason = fg_check_update_necessary(bq);

	if (!reason) {
		bq_log("Fuel gauge parameter no need update, ignored\n");
		return;
	}

	if (bq->batt_id >= ARRAY_SIZE(bqfs_image) ||
		bq->batt_id < 0) {
		bq_err("batt_id is out of range\n");
		return;
	}

	if (fg_get_seal_state(bq) != SEAL_STATE_FA) {
		if (!fg_unseal(bq))
			return;
		mdelay(10);
		if (!fg_unseal_full_access(bq))
			return;
	}

	if (!fg_enter_rom_mode(bq))
		return;

update:
	client->addr = BQ_FG_I2C_ROM_ADDR;

	bq_log("Fuel Gauge parameter update, reason:%s, version:%d, batt_id=%d Start...\n",
		update_reason_str[reason - 1],
		bqfs_image[bq->batt_id].version,
		bq->batt_id);

	mutex_lock(&bq->update_lock);
	image = bqfs_image[bq->batt_id].bqfs_image;
	for (i = 0; i < bqfs_image[bq->batt_id].array_size; i++) {
		if (!fg_update_execute_cmd(bq, &image[i])) {
			mutex_unlock(&bq->update_lock);
			bq_err("Failed at command:%d\n", i);
			return;
		}
		mdelay(5);
	}

	mutex_unlock(&bq->update_lock);

	bq_log("Done!\n");

	client->addr = BQ_FG_I2C_DEV_ADDR;
	/* mark as updated */
	fg_mark_as_updated(bq);

	/*TODO:*/
	/*fg_seal(bq);*/
}
#endif

static void determine_initial_status(struct bq_fg_chip *bq)
{
	fg_irq_thread(bq->client->irq, bq);
}


#ifdef FG_UPDATER
static int fg_parse_batt_id(struct bq_fg_chip *bq)
{
	//TODO: determine battery id
	//
	
	return 0;
}
#endif

static int bq_fg_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{

	int ret;
	struct bq_fg_chip *bq;
	u8 *regs;

	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);

	if (!bq)
		return -ENOMEM;

	bq->dev = &client->dev;
	bq->client = client;
	bq->chip = id->driver_data;

	bq->batt_soc	= -ENODATA;
	bq->batt_fcc	= -ENODATA;
	bq->batt_rm	= -ENODATA;
	bq->batt_dc	= -ENODATA;
	bq->batt_volt	= -ENODATA;
	bq->batt_temp	= -ENODATA;
	bq->batt_curr	= -ENODATA;
	bq->batt_cyclecnt = -ENODATA;

	bq->fake_soc	= -EINVAL;
	bq->fake_temp	= -EINVAL;

	if (bq->chip == BQ28Z610) {
		regs = bq28z610_regs;
	} else {
		bq_err("unexpected fuel gauge: %d\n", bq->chip);
		regs = bq28z610_regs;
	}

	memcpy(bq->regs, regs, NUM_REGS);

	i2c_set_clientdata(client, bq);

	mutex_init(&bq->i2c_rw_lock);
	mutex_init(&bq->data_lock);
	mutex_init(&bq->update_lock);
	mutex_init(&bq->irq_complete);
#ifdef FG_UPDATER
	INIT_WORK(&bq->update_work, fg_update_bqfs_workfunc);
#endif
	bq->resume_completed = true;
	bq->irq_waiting = false;

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
			fg_irq_thread,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"bq fuel gauge irq", bq);
		if (ret < 0) {
			bq_err("request irq for irq=%d failed, ret = %d\n",
				client->irq, ret);
			goto err_1;
		}
		enable_irq_wake(client->irq);
	}

	device_init_wakeup(bq->dev, 1);

	fg_read_fw_version(bq);

#ifdef	FG_UPDATER
	fg_parse_batt_id(bq);
	fg_update_bqfs(bq);
#endif
	fg_psy_register(bq);

	ret = sysfs_create_group(&bq->dev->kobj, &fg_attr_group);
	if (ret)
		bq_err("Failed to register sysfs, err:%d\n", ret);

	determine_initial_status(bq);

	bq_log("bq fuel gauge probe successfully, %s\n",
			device2str[bq->chip]);

	return 0;

err_1:
	fg_psy_unregister(bq);
	return ret;
}


static inline bool is_device_suspended(struct bq_fg_chip *bq)
{
	return !bq->resume_completed;
}


static int bq_fg_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->irq_complete);
	bq->resume_completed = false;
	mutex_unlock(&bq->irq_complete);

	return 0;
}

static int bq_fg_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	if (bq->irq_waiting) {
		pr_err_ratelimited("Aborting suspend, an interrupt was detected while suspending\n");
		return -EBUSY;
	}
	return 0;

}


static int bq_fg_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->irq_complete);
	bq->resume_completed = true;
	if (bq->irq_waiting) {
		bq->irq_disabled = false;
		enable_irq(client->irq);
		mutex_unlock(&bq->irq_complete);
		fg_irq_thread(client->irq, bq);
	} else {
		mutex_unlock(&bq->irq_complete);
	}

	power_supply_changed(bq->fg_psy);

	return 0;


}

static int bq_fg_remove(struct i2c_client *client)
{
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	fg_psy_unregister(bq);

	mutex_destroy(&bq->data_lock);
	mutex_destroy(&bq->i2c_rw_lock);
	mutex_destroy(&bq->update_lock);
	mutex_destroy(&bq->irq_complete);

	sysfs_remove_group(&bq->dev->kobj, &fg_attr_group);

	return 0;

}

static void bq_fg_shutdown(struct i2c_client *client)
{
	pr_info("bq fuel gauge driver shutdown!\n");
}

static const struct of_device_id bq_fg_match_table[] = {
	{.compatible = "ti,bq28z610",},
	{},
};
MODULE_DEVICE_TABLE(of, bq_fg_match_table);

static const struct i2c_device_id bq_fg_id[] = {
	{ "bq28z610", BQ28Z610 },
	{},
};
MODULE_DEVICE_TABLE(i2c, bq_fg_id);

static const struct dev_pm_ops bq_fg_pm_ops = {
	.resume		= bq_fg_resume,
	.suspend_noirq = bq_fg_suspend_noirq,
	.suspend	= bq_fg_suspend,
};

static struct i2c_driver bq_fg_driver = {
	.driver	= {
		.name   = "bq_fg",
		.owner  = THIS_MODULE,
		.of_match_table = bq_fg_match_table,
		.pm     = &bq_fg_pm_ops,
	},
	.id_table       = bq_fg_id,

	.probe          = bq_fg_probe,
	.remove		= bq_fg_remove,
	.shutdown	= bq_fg_shutdown,

};

module_i2c_driver(bq_fg_driver);

MODULE_DESCRIPTION("TI BQ28Z610 Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Texas Instruments");

