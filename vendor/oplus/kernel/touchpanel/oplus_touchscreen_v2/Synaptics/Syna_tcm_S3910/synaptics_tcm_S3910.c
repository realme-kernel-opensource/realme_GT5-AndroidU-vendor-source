// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#include <linux/gpio.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include "synaptics_tcm_S3910.h"

#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
#include <linux/platform_data/spi-mt65xx.h>
#endif


#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
const struct mtk_chip_config st_spi_ctrdata = {
	.sample_sel = 0,
	.cs_setuptime = 5000,
	.cs_holdtime = 3000,
	.cs_idletime = 0,
	.tick_delay = 0,
};
#endif
static unsigned char *buf;
static unsigned int buf_size;
static struct spi_transfer *xfer;

static struct syna_tcm_data *g_tcm_info[TP_SUPPORT_MAX] = {NULL};

extern struct device_hcd *syna_remote_device_S3010_init(
	struct syna_tcm_data *tcm_info);
extern int syna_remote_device_S3910_destory(struct syna_tcm_data *tcm_info);
static void syna_main_register(struct seq_file *s, void *chip_data);

static int syna_tcm_write_message(struct syna_tcm_data *tcm_info,
				  unsigned char command, unsigned char *payload,
				  unsigned int length, unsigned char **resp_buf,
				  unsigned int *resp_buf_size, unsigned int *resp_length,
				  unsigned int polling_delay_ms);
static void syna_tcm_test_report(struct syna_tcm_data *tcm_info);
static int syna_tcm_helper(struct syna_tcm_data *tcm_info);
static int syna_tcm_enable_report(struct syna_tcm_data *tcm_info,
				  enum report_type report_type, bool enable);
static void syna_tcm_fw_update_in_bl(void *chip_data);
static int syna_tcm_spi_alloc_mem(struct syna_tcm_data *tcm_hcd,
				  unsigned int count, unsigned int size)
{
	static unsigned int xfer_count;

	if (count > xfer_count) {
		kfree(xfer);
		xfer = kcalloc(count, sizeof(*xfer), GFP_KERNEL);
		if (!xfer) {
			TPD_INFO("Failed to allocate memory for xfer\n");
			xfer_count = 0;
			return -ENOMEM;
		}
		xfer_count = count;
	} else {
		memset(xfer, 0, count * sizeof(*xfer));
	}

	if (size > buf_size) {
		if (buf_size) {
			kfree(buf);
		}
		buf = kmalloc(size, GFP_KERNEL);
		if (!buf) {
			TPD_INFO("Failed to allocate memory for buf\n");
			buf_size = 0;
			return -ENOMEM;
		}
		buf_size = size;
	}

	return 0;
}
/**
 * syna_tcm_read - read data from TP IC
 *
 * @tcm_hcd: IC Information
 * @data: Store read data
 * @length: read data lenght
 * Write data to TP IC through SPI
 */
static inline int syna_tcm_read(struct syna_tcm_data *tcm_hcd,
					unsigned char *data, unsigned int length)
{
	int retval;
	unsigned int idx;
	struct spi_message msg;
	struct spi_device *spi = tcm_hcd->client;
	spi_message_init(&msg);


	if (tcm_hcd->byte_delay_us == 0) {
		retval = syna_tcm_spi_alloc_mem(tcm_hcd, 1, length);
	} else {
		retval = syna_tcm_spi_alloc_mem(tcm_hcd, length, 1);
	}
	if (retval < 0) {
		TPD_INFO("Failed to allocate memory\n");
		goto exit;
	}

	if (tcm_hcd->byte_delay_us == 0) {
		memset(buf, 0xff, length);
		xfer[0].len = length;
		xfer[0].tx_buf = buf;
		xfer[0].rx_buf = data;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0))
		if (tcm_hcd->block_delay_us) {
			xfer[0].delay_usecs = tcm_hcd->block_delay_us;
		}
#endif
		spi_message_add_tail(&xfer[0], &msg);
	} else {
		buf[0] = 0xff;
		for (idx = 0; idx < length; idx++) {
			xfer[idx].len = 1;
			xfer[idx].tx_buf = buf;
			xfer[idx].rx_buf = &data[idx];
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0))
			xfer[idx].delay_usecs = tcm_hcd->byte_delay_us;
			if (tcm_hcd->block_delay_us && (idx == length - 1)) {
				xfer[idx].delay_usecs = tcm_hcd->block_delay_us;
			}
#endif
			spi_message_add_tail(&xfer[idx], &msg);
		}
	}
	retval = spi_sync(spi, &msg);
	if (retval == 0) {
		retval = length;
	} else {
		TPD_INFO("Failed to complete SPI transfer, error = %d\n", retval);
	}
exit:
	return retval;
}

/**
 * syna_tcm_write - write data to TP IC
 *
 * @tcm_hcd: IC Information
 * @data: Store write data
 * @length: write data lenght
 * Write data to TP IC through SPI
 */
static inline int syna_tcm_write(struct syna_tcm_data *tcm_hcd,
	unsigned char *data, unsigned int length)
{
	int retval;
	unsigned int idx;
	struct spi_message msg;
	struct spi_device *spi = tcm_hcd->client;
	spi_message_init(&msg);

	if (tcm_hcd->byte_delay_us == 0) {
		retval = syna_tcm_spi_alloc_mem(tcm_hcd, 1, 0);
	} else {
		retval = syna_tcm_spi_alloc_mem(tcm_hcd, length, 0);
	}
	if (retval < 0) {
		TPD_INFO("Failed to allocate memory\n");
		goto exit;
	}


	if (tcm_hcd->byte_delay_us == 0) {
		xfer[0].len = length;
		xfer[0].tx_buf = data;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0))
		if (tcm_hcd->block_delay_us) {
			xfer[0].delay_usecs = tcm_hcd->block_delay_us;
		}
#endif
		spi_message_add_tail(&xfer[0], &msg);
	} else {
		for (idx = 0; idx < length; idx++) {
			xfer[idx].len = 1;
			xfer[idx].tx_buf = &data[idx];
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0))
			xfer[idx].delay_usecs = tcm_hcd->byte_delay_us;
			if (tcm_hcd->block_delay_us && (idx == length - 1)) {
					  xfer[idx].delay_usecs = tcm_hcd->block_delay_us;
			}
#endif
			spi_message_add_tail(&xfer[idx], &msg);
		}
	}
	retval = spi_sync(spi, &msg);
	if (retval == 0) {
		retval = length;
	} else {
		TPD_INFO("Failed to complete SPI transfer, error = %d\n", retval);
	}
exit:
	return retval;
}
/**
 * syna_get_report_data - Retrieve data from touch report
 *
 * @tcm_info: handle of tcm module
 * @offset: start bit of retrieved data
 * @bits: total bits of retrieved data
 * @data: pointer of data, at most 4 byte
 * Retrieve data from the touch report based on the bit offset and bit length
 * information from the touch report configuration.
 */
static int syna_get_report_data(struct syna_tcm_data *tcm_info,
				unsigned int offset,
				unsigned int bits, unsigned int *data)
{
	unsigned char mask = 0;
	unsigned char byte_data = 0;
	unsigned int output_data = 0;
	unsigned int bit_offset = offset % 8;
	unsigned int byte_offset = offset / 8;
	unsigned int data_bits = 0;
	unsigned int available_bits = 0;
	unsigned int remaining_bits = bits;
	unsigned char *touch_report = tcm_info->report.buffer.buf;
	int retval = 0;

	if (bits == 0 || bits > 32) {
		TPD_DEBUG("larger than 32 bits:%d\n", bits);
		retval = tp_memcpy((unsigned char *)data, bits / 8, &touch_report[byte_offset],
				   bits / 8, bits / 8);

		if (retval < 0) {
			TPD_INFO("Failed to copy report data\n");
			return retval;
		}

		return 0;
	}

	if (offset + bits > tcm_info->report.buffer.data_length * 8) {
		TPD_DEBUG("offset and bits beyond total read length\n");
		*data = 0;
		return 0;
	}

	while (remaining_bits) {
		byte_data = touch_report[byte_offset];
		byte_data >>= bit_offset;

		available_bits = 8 - bit_offset;
		data_bits = MIN(available_bits, remaining_bits);
		mask = 0xff >> (8 - data_bits);

		byte_data &= mask;

		output_data |= byte_data << (bits - remaining_bits);

		bit_offset = 0;
		byte_offset += 1;
		remaining_bits -= data_bits;
	}

	*data = output_data;

	return 0;
}

/**
 * touch_parse_report() - Parse touch report
 *
 * Traverse through the touch report configuration and parse the touch report
 * generated by the device accordingly to retrieve the touch data.
 */
static int syna_parse_report(struct syna_tcm_data *tcm_info)
{
	int i = 0;
	int retval = 0;
	bool active_only = false, num_of_active_objects = false;
	unsigned char code;
	unsigned int size = 0, idx = 0, obj = 0;
	unsigned int next = 0, data = 0, bits = 0, offset = 0, objects = 0;
	unsigned char  grip_data[4];
	unsigned int active_objects = 0;
	unsigned int report_size = 0, config_size = 0;
	unsigned char *config_data = NULL;
	struct touch_hcd *touch_hcd = NULL;
	struct touch_data *touch_data = NULL;
	struct object_data *object_data = NULL;
	static unsigned int end_of_foreach = 0;

	touch_hcd = tcm_info->touch_hcd;
	touch_data = &touch_hcd->touch_data;
	object_data = touch_hcd->touch_data.object_data;
	config_data = tcm_info->config.buf;
	config_size = tcm_info->config.data_length;
	report_size = tcm_info->report.buffer.data_length;
	size = sizeof(*object_data) * touch_hcd->max_objects;
	memset(touch_hcd->touch_data.object_data, 0x00, size);

	while (idx < config_size) {
		code = config_data[idx++];

		switch (code) {
		case TOUCH_END:
			goto exit;

		case TOUCH_FOREACH_ACTIVE_OBJECT:
			obj = 0;
			next = idx;
			active_only = true;
			break;

		case TOUCH_FOREACH_OBJECT:
			obj = 0;
			next = idx;
			active_only = false;
			break;

		case TOUCH_FOREACH_END:
			end_of_foreach = idx;

			if (active_only) {
				if (num_of_active_objects) {
					objects++;

					if (objects < active_objects) {
						idx = next;
					}

				} else if (offset < report_size * 8) {
					idx = next;
				}

			} else {
				obj++;

				if (obj < touch_hcd->max_objects) {
					idx = next;
				}
			}

			break;

		case TOUCH_PAD_TO_NEXT_BYTE:
			offset = ceil_div(offset, 8) * 8;
			break;

		case TOUCH_TIMESTAMP:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get timestamp\n");
				return retval;
			}

			touch_data->timestamp = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_INDEX:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &obj);

			if (retval < 0) {
				TPD_INFO("Failed to get object index\n");
				return retval;
			}

			if (obj >= touch_hcd->max_objects) {
				TPD_INFO("Object index error 0x%0X\n", obj);
				return -1;
			}

			offset += bits;
			break;

		case TOUCH_OBJECT_N_CLASSIFICATION:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object classification\n");
				return retval;
			}
			if (obj >= touch_hcd->max_objects) {
				TPD_INFO("obj> max_obj!! obj[%d]Report Data[%d]:", obj, report_size);
				if (tp_debug != 0) {
					for (i = 0; i < report_size; i++) {
						TPD_INFO("syna data:[0x%2x]", tcm_info->report.buffer.buf[i]);
					}
				}
				return -1;
			}
			object_data[obj].status = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_X_POSITION:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object x position\n");
				return retval;
			}

			object_data[obj].x_pos = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_Y_POSITION:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object y position\n");
				return retval;
			}

			object_data[obj].y_pos = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_Z:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object z\n");
				return retval;
			}

			object_data[obj].z = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_X_WIDTH:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object x width\n");
				return retval;
			}

			object_data[obj].x_width = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_Y_WIDTH:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object y width\n");
				return retval;
			}

			object_data[obj].y_width = data;
			offset += bits;
			break;
		case  TOUCH_REPORT_CUSTOMER_GRIP_INFO:
			bits = config_data[idx++];
			if (retval < 0) {
				TPD_INFO("Failed to get Grip info\n");
				return retval;
			}
			object_data[obj].eywidth  = grip_data[0];
			object_data[obj].exwidth  = grip_data[1];
			object_data[obj].yeratio  = grip_data[2];
			object_data[obj].xeratio  = grip_data[3];
			offset += bits;
			break;
		case TOUCH_OBJECT_N_TX_POSITION_TIXELS:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object tx position\n");
				return retval;
			}

			object_data[obj].tx_pos = data;
			offset += bits;
			break;

		case TOUCH_OBJECT_N_RX_POSITION_TIXELS:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get object rx position\n");
				return retval;
			}

			object_data[obj].rx_pos = data;
			offset += bits;
			break;

		case TOUCH_0D_BUTTONS_STATE:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get 0D buttons state\n");
				return retval;
			}

			touch_data->buttons_state = data;
			offset += bits;
			break;

		case TOUCH_GESTURE_DOUBLE_TAP:
		case TOUCH_REPORT_GESTURE_SWIPE:
		case TOUCH_REPORT_GESTURE_CIRCLE:
		case TOUCH_REPORT_GESTURE_UNICODE:
		case TOUCH_REPORT_GESTURE_VEE:
		case TOUCH_REPORT_GESTURE_TRIANGLE:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get gesture double tap\n");
				return retval;
			}

			touch_data->lpwg_gesture = data;
			offset += bits;
			break;

		case TOUCH_REPORT_GESTURE_INFO:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits,
						      (unsigned int *)(&touch_data->extra_gesture_info[0]));

			if (retval < 0) {
				TPD_INFO("Failed to get gesture double tap\n");
				return retval;
			}

			offset += bits;
			break;

		case TOUCH_REPORT_GESTURE_COORDINATE:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits,
						      (unsigned int *)(&touch_data->data_point[0]));

			if (retval < 0) {
				TPD_INFO("Failed to get gesture double tap\n");
				return retval;
			}

			offset += bits;
			break;

		case TOUCH_FRAME_RATE:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get frame rate\n");
				return retval;
			}

			touch_data->frame_rate = data;
			offset += bits;
			break;

		case TOUCH_POWER_IM:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get power IM\n");
				return retval;
			}

			touch_data->power_im = data;
			offset += bits;
			break;

		case TOUCH_CID_IM:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get CID IM\n");
				return retval;
			}

			touch_data->cid_im = data;
			offset += bits;
			break;

		case TOUCH_RAIL_IM:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get rail IM\n");
				return retval;
			}

			touch_data->rail_im = data;
			offset += bits;
			break;

		case TOUCH_CID_VARIANCE_IM:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get CID variance IM\n");
				return retval;
			}

			touch_data->cid_variance_im = data;
			offset += bits;
			break;

		case TOUCH_NSM_FREQUENCY:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get NSM frequency\n");
				return retval;
			}

			touch_data->nsm_frequency = data;
			offset += bits;
			break;

		case TOUCH_NSM_STATE:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get NSM state\n");
				return retval;
			}

			touch_data->nsm_state = data;
			offset += bits;
			break;

		case TOUCH_NUM_OF_ACTIVE_OBJECTS:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get number of active objects\n");
				return retval;
			}

			active_objects = data;
			num_of_active_objects = true;
			touch_data->num_of_active_objects = data;
			offset += bits;

			if (touch_data->num_of_active_objects == 0) {
				idx = end_of_foreach;
			}

			break;

		case TOUCH_NUM_OF_CPU_CYCLES_USED_SINCE_LAST_FRAME:
			bits = config_data[idx++];
			retval = syna_get_report_data(tcm_info, offset, bits, &data);

			if (retval < 0) {
				TPD_INFO("Failed to get number of CPU cycles used since last frame\n");
				return retval;
			}

			touch_data->num_of_cpu_cycles = data;
			offset += bits;
			break;

		case TOUCH_TUNING_GAUSSIAN_WIDTHS:
			bits = config_data[idx++];
			offset += bits;
			break;

		case TOUCH_TUNING_SMALL_OBJECT_PARAMS:
			bits = config_data[idx++];
			offset += bits;
			break;

		case TOUCH_TUNING_0D_BUTTONS_VARIANCE:
			bits = config_data[idx++];
			offset += bits;
			break;
		}
	}

exit:
	return 0;
}

static int syna_get_input_params(struct syna_tcm_data *tcm_info)
{
	int retval;

	LOCK_BUFFER(tcm_info->config);

	retval = syna_tcm_write_message(tcm_info, CMD_GET_TOUCH_REPORT_CONFIG,
					NULL, 0, &tcm_info->config.buf, &tcm_info->config.buf_size,
					&tcm_info->config.data_length, 0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_GET_TOUCH_REPORT_CONFIG));
		UNLOCK_BUFFER(tcm_info->config);
		return retval;
	}

	UNLOCK_BUFFER(tcm_info->config);

	return 0;
}

static int syna_set_default_report_config(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	int length = 0;

	LOCK_BUFFER(tcm_info->config);

	length = tcm_info->default_config.buf_size;

	if (tcm_info->default_config.buf) {
		retval = syna_tcm_alloc_mem(&tcm_info->config, length);

		if (retval < 0) {
			TPD_INFO("Failed to alloc mem\n");
			goto exit;
		}

		memcpy(tcm_info->config.buf, tcm_info->default_config.buf, length);
		tcm_info->config.buf_size = tcm_info->default_config.buf_size;
		tcm_info->config.data_length = tcm_info->default_config.data_length;
	}

exit:
	UNLOCK_BUFFER(tcm_info->config);

	return retval;
}

static int syna_get_default_report_config(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	unsigned int length;

	length = le2_to_uint(tcm_info->app_info.max_touch_report_config_size);

	LOCK_BUFFER(tcm_info->default_config);

	retval = syna_tcm_write_message(tcm_info,
					CMD_GET_TOUCH_REPORT_CONFIG,
					NULL,
					0,
					&tcm_info->default_config.buf,
					&tcm_info->default_config.buf_size,
					&tcm_info->default_config.data_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_GET_TOUCH_REPORT_CONFIG));
		goto exit;
	}

exit:
	UNLOCK_BUFFER(tcm_info->default_config);
	return retval;
}

static int syna_set_normal_report_config(struct syna_tcm_data *tcm_info)
{
	int retval;
	unsigned int idx = 0;
	unsigned int length;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;

	TPD_DEBUG("%s:set normal report\n", __func__);
	length = le2_to_uint(tcm_info->app_info.max_touch_report_config_size);

	if (length < TOUCH_REPORT_CONFIG_SIZE) {
		TPD_INFO("Invalid maximum touch report config size\n");
		return -EINVAL;
	}

	LOCK_BUFFER(touch_hcd->out);

	retval = syna_tcm_alloc_mem(&touch_hcd->out, length);

	if (retval < 0) {
		TPD_INFO("Failed to allocate memory for touch_hcd->out.buf\n");
		UNLOCK_BUFFER(touch_hcd->out);
		return retval;
	}

	touch_hcd->out.buf[idx++] = TOUCH_GESTURE_DOUBLE_TAP;
	touch_hcd->out.buf[idx++] = 8;

	if (0 == tcm_info->normal_config_version) {
		touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_INFO;
		touch_hcd->out.buf[idx++] = 48;
	}
	touch_hcd->out.buf[idx++] = TOUCH_FOREACH_ACTIVE_OBJECT;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_INDEX;
	touch_hcd->out.buf[idx++] = 4;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_CLASSIFICATION;
	touch_hcd->out.buf[idx++] = 4;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_X_POSITION;
	touch_hcd->out.buf[idx++] = 16;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_Y_POSITION;
	touch_hcd->out.buf[idx++] = 16;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_X_WIDTH;
	touch_hcd->out.buf[idx++] = 12;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_Y_WIDTH;
	touch_hcd->out.buf[idx++] = 12;
	touch_hcd->out.buf[idx++] = TOUCH_REPORT_CUSTOMER_GRIP_INFO;
	touch_hcd->out.buf[idx++] = 32;
	touch_hcd->out.buf[idx++] = TOUCH_FOREACH_END;
	touch_hcd->out.buf[idx++] = TOUCH_END;

	LOCK_BUFFER(touch_hcd->resp);

	retval = syna_tcm_write_message(tcm_info,
					CMD_SET_TOUCH_REPORT_CONFIG,
					touch_hcd->out.buf,
					length,
					&touch_hcd->resp.buf,
					&touch_hcd->resp.buf_size,
					&touch_hcd->resp.data_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_SET_TOUCH_REPORT_CONFIG));
		UNLOCK_BUFFER(touch_hcd->resp);
		UNLOCK_BUFFER(touch_hcd->out);
		return retval;
	}

	UNLOCK_BUFFER(touch_hcd->resp);
	UNLOCK_BUFFER(touch_hcd->out);

	return retval;
}

static int syna_set_gesture_report_config(struct syna_tcm_data *tcm_info)
{
	int retval;
	unsigned int idx = 0;
	unsigned int length;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;

	TPD_DEBUG("%s: set gesture report\n", __func__);
	length = le2_to_uint(tcm_info->app_info.max_touch_report_config_size);

	if (length < TOUCH_REPORT_CONFIG_SIZE) {
		TPD_INFO("Invalid maximum touch report config size\n");
		return -EINVAL;
	}

	LOCK_BUFFER(touch_hcd->out);

	retval = syna_tcm_alloc_mem(&touch_hcd->out, length);

	if (retval < 0) {
		TPD_INFO("Failed to allocate memory for touch_hcd->out.buf\n");
		UNLOCK_BUFFER(touch_hcd->out);
		return retval;
	}

	touch_hcd->out.buf[idx++] = TOUCH_GESTURE_DOUBLE_TAP;
	touch_hcd->out.buf[idx++] = 8;
	/* touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_CIRCLE;*/
	/* touch_hcd->out.buf[idx++] = 1;*/
	/* touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_SWIPE;*/
	/* touch_hcd->out.buf[idx++] = 1;*/
	/* touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_UNICODE;*/
	/* touch_hcd->out.buf[idx++] = 1;*/
	/* touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_VEE;*/
	/* touch_hcd->out.buf[idx++] = 1;*/
	/* touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_TRIANGLE;*/
	/* touch_hcd->out.buf[idx++] = 1;*/
	/* touch_hcd->out.buf[idx++] = TOUCH_PAD_TO_NEXT_BYTE;*/
	touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_INFO;
	touch_hcd->out.buf[idx++] = 48;
	touch_hcd->out.buf[idx++] = TOUCH_REPORT_GESTURE_COORDINATE;
	touch_hcd->out.buf[idx++] = 192;
	touch_hcd->out.buf[idx++] = TOUCH_FOREACH_ACTIVE_OBJECT;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_INDEX;
	touch_hcd->out.buf[idx++] = 4;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_CLASSIFICATION;
	touch_hcd->out.buf[idx++] = 4;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_X_POSITION;
	touch_hcd->out.buf[idx++] = 16;
	touch_hcd->out.buf[idx++] = TOUCH_OBJECT_N_Y_POSITION;
	touch_hcd->out.buf[idx++] = 16;
	touch_hcd->out.buf[idx++] = TOUCH_FOREACH_END;
	touch_hcd->out.buf[idx++] = TOUCH_END;

	LOCK_BUFFER(touch_hcd->resp);

	retval = syna_tcm_write_message(tcm_info,
					CMD_SET_TOUCH_REPORT_CONFIG,
					touch_hcd->out.buf,
					length,
					&touch_hcd->resp.buf,
					&touch_hcd->resp.buf_size,
					&touch_hcd->resp.data_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_SET_TOUCH_REPORT_CONFIG));
		UNLOCK_BUFFER(touch_hcd->resp);
		UNLOCK_BUFFER(touch_hcd->out);
		return retval;
	}

	UNLOCK_BUFFER(touch_hcd->resp);
	UNLOCK_BUFFER(touch_hcd->out);

	return 0;
}

static int syna_set_input_reporting(struct syna_tcm_data *tcm_info, bool suspend)
{
	int retval = 0;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;

	TPD_DEBUG("%s: mode 0x%x, state %d\n", __func__, tcm_info->id_info.mode,
		  suspend);

	if (tcm_info->id_info.mode != MODE_APPLICATION
	    || tcm_info->app_status != APP_STATUS_OK) {
		TPD_INFO("Application firmware not running\n");
		return 0;
	}

	touch_hcd->report_touch = false;

	mutex_lock(&touch_hcd->report_mutex);

	if (!suspend) {
		retval = syna_set_normal_report_config(tcm_info);

		if (retval < 0) {
			TPD_INFO("Failed to set report config\n");
			goto default_config;
		}

	} else {
		retval = syna_set_gesture_report_config(tcm_info);

		if (retval < 0) {
			TPD_INFO("Failed to set report config\n");
			goto default_config;
		}
	}

	retval = syna_get_input_params(tcm_info);

	if (retval < 0) {
		TPD_INFO("Failed to get input parameters\n");
	}

	goto exit;

default_config:
	/*if failed to set report config, use default report config */
	retval = syna_set_default_report_config(tcm_info);

	if (retval < 0) {
		TPD_INFO("Failed to set default report config");
	}

exit:
	mutex_unlock(&touch_hcd->report_mutex);

	touch_hcd->report_touch = retval < 0 ? false : true;

	return retval;
}

static void syna_set_trigger_reason(struct syna_tcm_data *tcm_info,
				    irq_reason trigger_reason)
{
	SET_BIT(tcm_info->trigger_reason, trigger_reason);
}

static void syna_tcm_resize_chunk_size(struct syna_tcm_data *tcm_info)
{
	unsigned int max_write_size;

	max_write_size = le2_to_uint(tcm_info->id_info.max_write_size);
	tcm_info->wr_chunk_size = MIN(max_write_size, WR_CHUNK_SIZE);

	if (tcm_info->wr_chunk_size == 0) {
		tcm_info->wr_chunk_size = max_write_size;
	}
}

static int syna_async_work_callback(struct syna_tcm_data *tcm_info)
{
	if (!tcm_info) {
		return 0;
	}

	TPD_INFO("%s: async work enter\n", __func__);
	if (tcm_info->first_sync_flag) {
		tcm_info->first_sync_flag = false;
		return 0;
	}
	/*reinit_completion(&tcm_info->resume_complete);*/
	if (tcm_info->suspend_state == TP_RESUME_COMPLETE) {
		TPD_INFO("%s: *tcm_info->suspend_state %d \n", __func__, tcm_info->suspend_state);
		complete(&tcm_info->resume_complete);
		return 0;
	}

	if (tcm_info->in_test_process) {
		TPD_INFO("%s: In test process, do not switch mode\n", __func__);
		return 0;
	}
	TPD_INFO("%s: async work exit\n", __func__);

	queue_work(tcm_info->async_workqueue, &tcm_info->async_work);
	return 0;
}

/**
 * syna_tcm_dispatch_report() - dispatch report received from device
 *
 * @tcm_info: handle of core module
 *
 * The report generated by the device is forwarded to the synchronous inbox of
 * each registered application module for further processing. In addition, the
 * report notifier thread is woken up for asynchronous notification of the
 * report occurrence.
 */
static void syna_tcm_dispatch_report(struct syna_tcm_data *tcm_info)
{
	int ret = 0;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touch_data *touch_data = &touch_hcd->touch_data;

	LOCK_BUFFER(tcm_info->in);
	LOCK_BUFFER(tcm_info->report.buffer);

	tcm_info->report.buffer.buf = &tcm_info->in.buf[MESSAGE_HEADER_SIZE];
	tcm_info->report.buffer.buf_size = tcm_info->in.buf_size - MESSAGE_HEADER_SIZE;
	tcm_info->report.buffer.data_length = tcm_info->payload_length;
	tcm_info->report.id = tcm_info->report_code;

	if (tcm_info->report.id == REPORT_TOUCH) {
		if (*tcm_info->loading_fw) {
		    TPD_INFO("%s: disable touch when TP loading_fw !\n", __func__);
		    goto exit;
		}
		ret = syna_parse_report(tcm_info);

		if (ret < 0) {
			TPD_INFO("Failed to parse report\n");
			goto exit;
		}

		if (*tcm_info->in_suspend) {
			syna_set_trigger_reason(tcm_info, IRQ_GESTURE);

		} else {
			syna_set_trigger_reason(tcm_info, IRQ_TOUCH);

			if (touch_data->lpwg_gesture == TOUCH_HOLD_UP
			    || touch_data->lpwg_gesture == TOUCH_HOLD_DOWN) {
				syna_set_trigger_reason(tcm_info, IRQ_FINGERPRINT);
			}
		}

	} else if (tcm_info->report.id == REPORT_IDENTIFY) {
		if (tcm_info->id_info.mode == MODE_APPLICATION) {
			syna_async_work_callback(tcm_info);
			syna_set_trigger_reason(tcm_info, IRQ_IGNORE);
		}
	} else if (tcm_info->report.id == REPORT_TOUCH_HOLD) {
		syna_set_trigger_reason(tcm_info, IRQ_FINGERPRINT);

	} else if (tcm_info->report.id == REPORT_LOG) {
		syna_set_trigger_reason(tcm_info, IRQ_FW_HEALTH);

	} else {
		syna_tcm_test_report(tcm_info);
	}

exit:
	UNLOCK_BUFFER(tcm_info->report.buffer);
	UNLOCK_BUFFER(tcm_info->in);
	return;
}


/**
 * syna_tcm_dispatch_response() - dispatch response received from device
 *
 * @tcm_info: handle of core module
 *
 * The response to a command is forwarded to the sender of the command.
 */
static void syna_tcm_dispatch_response(struct syna_tcm_data *tcm_info)
{
	int retval = 0;

	if (atomic_read(&tcm_info->command_status) != CMD_BUSY) {
		tcm_info->trigger_reason = 0;
		return;
	}

	LOCK_BUFFER(tcm_info->resp);

	if (tcm_info->payload_length == 0) {
		UNLOCK_BUFFER(tcm_info->resp);
		atomic_set(&tcm_info->command_status, CMD_IDLE);
		goto exit;
	}

	retval = syna_tcm_alloc_mem(&tcm_info->resp, tcm_info->payload_length);

	if (retval < 0) {
		TPD_INFO("Failed to allocate memory for tcm_info->resp.buf\n");
		UNLOCK_BUFFER(tcm_info->resp);
		atomic_set(&tcm_info->command_status, CMD_ERROR);
		goto exit;
	}

	LOCK_BUFFER(tcm_info->in);

	retval = tp_memcpy(tcm_info->resp.buf, tcm_info->resp.buf_size,
			   &tcm_info->in.buf[MESSAGE_HEADER_SIZE],
			   tcm_info->in.buf_size - MESSAGE_HEADER_SIZE, tcm_info->payload_length);

	if (retval < 0) {
		TPD_INFO("Failed to copy payload\n");
		UNLOCK_BUFFER(tcm_info->in);
		UNLOCK_BUFFER(tcm_info->resp);
		atomic_set(&tcm_info->command_status, CMD_ERROR);
		goto exit;
	}

	tcm_info->resp.data_length = tcm_info->payload_length;

	UNLOCK_BUFFER(tcm_info->in);
	UNLOCK_BUFFER(tcm_info->resp);

	atomic_set(&tcm_info->command_status, CMD_IDLE);

exit:
	complete(&tcm_info->response_complete);
	tcm_info->trigger_reason = 0;

	return;
}

/**
 * syna_tcm_dispatch_message() - dispatch message received from device
 *
 * @tcm_info: handle of core module
 *
 * The information received in the message read in from the device is dispatched
 * to the appropriate destination based on whether the information represents a
 * report or a response to a command.
 */
static void syna_tcm_dispatch_message(struct syna_tcm_data *tcm_info)
{
	int retval;
	unsigned int payload_length;

	if (tcm_info->report_code == REPORT_IDENTIFY) {
		payload_length = tcm_info->payload_length;

		LOCK_BUFFER(tcm_info->in);

		retval = tp_memcpy((unsigned char *)&tcm_info->id_info,
				   sizeof(tcm_info->id_info),
				   &tcm_info->in.buf[MESSAGE_HEADER_SIZE],
				   tcm_info->in.buf_size - MESSAGE_HEADER_SIZE,
				   MIN(sizeof(tcm_info->id_info), payload_length));

		if (retval < 0) {
			TPD_INFO("Failed to copy identification info\n");
			UNLOCK_BUFFER(tcm_info->in);
			return;
		}

		UNLOCK_BUFFER(tcm_info->in);

		syna_tcm_resize_chunk_size(tcm_info);
		TPD_INFO("Received identify report (firmware mode = 0x%02x)\n",
			 tcm_info->id_info.mode);
		if (0x0b == tcm_info->id_info.mode) {
			tcm_info->firmware_mode_count++;
			if (!tcm_info->upload_flag && tcm_info->firmware_mode_count >= FIRMWARE_MODE_BL_MAX) {
				tcm_info->upload_flag = 1;
			}
		}
		if (atomic_read(&tcm_info->command_status) == CMD_BUSY) {
			switch (tcm_info->command) {
			case CMD_RESET:
			case CMD_RUN_BOOTLOADER_FIRMWARE:
			case CMD_RUN_APPLICATION_FIRMWARE:
				atomic_set(&tcm_info->command_status, CMD_IDLE);
				complete(&tcm_info->response_complete);
				break;

			default:
				TPD_INFO("Device has been reset\n");
				atomic_set(&tcm_info->command_status, CMD_ERROR);
				complete(&tcm_info->response_complete);
				break;
			}
		}

		if (tcm_info->id_info.mode == MODE_HOST_DOWNLOAD) {
			return;
		}

		syna_tcm_helper(tcm_info);
	}

	if (tcm_info->report_code >= REPORT_IDENTIFY) {
		syna_tcm_dispatch_report(tcm_info);

	} else {
		syna_tcm_dispatch_response(tcm_info);
	}

	return;
}

/**
 * syna_tcm_continued_read() - retrieve entire payload from device
 *
 * @tcm_info: handle of core module
 *
 * Read transactions are carried out until the entire payload is retrieved from
 * the device and stored in the handle of the core module.
 */
static int syna_tcm_continued_read(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	unsigned char marker = 0, code = 0;
	unsigned int idx = 0, offset = 0, chunks = 0;
	unsigned int chunk_space = 0, xfer_length = 0, total_length = 0,
		     remaining_length = 0;

	total_length = MESSAGE_HEADER_SIZE + tcm_info->payload_length + 1;
	remaining_length = total_length - tcm_info->read_length;

	LOCK_BUFFER(tcm_info->in);

	retval = syna_tcm_realloc_mem(&tcm_info->in, total_length);

	if (retval < 0) {
		TPD_INFO("Failed to reallocate memory for tcm_info->in.buf\n");
		UNLOCK_BUFFER(tcm_info->in);
		return retval;
	}

	/* available chunk space for payload = total chunk size minus header
	 * marker byte and header code byte */
	if (tcm_info->rd_chunk_size == 0) {
		chunk_space = remaining_length;

	} else {
		chunk_space = tcm_info->rd_chunk_size - 2;
	}

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	offset = tcm_info->read_length;

	LOCK_BUFFER(tcm_info->temp);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space) {
			xfer_length = chunk_space;

		} else {
			xfer_length = remaining_length;
		}

		if (xfer_length == 1) {
			tcm_info->in.buf[offset] = MESSAGE_PADDING;
			offset += xfer_length;
			remaining_length -= xfer_length;
			continue;
		}

		retval = syna_tcm_alloc_mem(&tcm_info->temp, xfer_length + 2);

		if (retval < 0) {
			TPD_INFO("Failed to allocate memory for tcm_info->temp.buf\n");
			UNLOCK_BUFFER(tcm_info->temp);
			UNLOCK_BUFFER(tcm_info->in);
			return retval;
		}

		retval = syna_tcm_read(tcm_info, tcm_info->temp.buf,
						  xfer_length + 2);

		if (retval < 0) {
			TPD_INFO("Failed to read from device\n");
			UNLOCK_BUFFER(tcm_info->temp);
			UNLOCK_BUFFER(tcm_info->in);
			return retval;
		}

		marker = tcm_info->temp.buf[0];
		code = tcm_info->temp.buf[1];

		if (marker != MESSAGE_MARKER) {
			TPD_INFO("Incorrect header marker (0x%02x)\n", marker);
			UNLOCK_BUFFER(tcm_info->temp);
			UNLOCK_BUFFER(tcm_info->in);
			return -EIO;
		}

		if (code != STATUS_CONTINUED_READ) {
			TPD_INFO("Incorrect header code (0x%02x)\n", code);
			UNLOCK_BUFFER(tcm_info->temp);
			UNLOCK_BUFFER(tcm_info->in);
			return -EIO;
		}

		retval = tp_memcpy(&tcm_info->in.buf[offset], total_length - offset,
				   &tcm_info->temp.buf[2], xfer_length, xfer_length);

		if (retval < 0) {
			TPD_INFO("Failed to copy payload\n");
			UNLOCK_BUFFER(tcm_info->temp);
			UNLOCK_BUFFER(tcm_info->in);
			return retval;
		}

		offset += xfer_length;

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_info->temp);
	UNLOCK_BUFFER(tcm_info->in);

	return 0;
}

/**
 * syna_tcm_raw_read() - retrieve specific number of data bytes from device
 *
 * @tcm_info: handle of core module
 * @in_buf: buffer for storing data retrieved from device
 * @length: number of bytes to retrieve from device
 *
 * Read transactions are carried out until the specific number of data bytes are
 * retrieved from the device and stored in in_buf.
 */
static int syna_tcm_raw_read(struct syna_tcm_data *tcm_info,
			     unsigned char *in_buf, unsigned int length)
{
	int retval = 0;
	unsigned char code = 0;
	unsigned int idx = 0, offset = 0;
	unsigned int chunks = 0, chunk_space = 0;
	unsigned int xfer_length = 0, remaining_length = 0;

	if (length < 2) {
		TPD_INFO("Invalid length information\n");
		return -EINVAL;
	}

	/* minus header marker byte and header code byte */
	remaining_length = length - 2;

	/* available chunk space for data = total chunk size minus header marker
	 * byte and header code byte */
	if (tcm_info->rd_chunk_size == 0) {
		chunk_space = remaining_length;

	} else {
		chunk_space = tcm_info->rd_chunk_size - 2;
	}

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	offset = 0;

	LOCK_BUFFER(tcm_info->temp);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space) {
			xfer_length = chunk_space;

		} else {
			xfer_length = remaining_length;
		}

		if (xfer_length == 1) {
			in_buf[offset] = MESSAGE_PADDING;
			offset += xfer_length;
			remaining_length -= xfer_length;
			continue;
		}

		retval = syna_tcm_alloc_mem(&tcm_info->temp, xfer_length + 2);

		if (retval < 0) {
			TPD_INFO("Failed to allocate memory for tcm_info->temp.buf\n");
			UNLOCK_BUFFER(tcm_info->temp);
			return retval;
		}

		retval = syna_tcm_read(tcm_info, tcm_info->temp.buf, xfer_length + 2);

		if (retval < 0) {
			TPD_INFO("Failed to read from device\n");
			UNLOCK_BUFFER(tcm_info->temp);
			return retval;
		}

		code = tcm_info->temp.buf[1];

		if (idx == 0) {
			retval = tp_memcpy(&in_buf[0], length, &tcm_info->temp.buf[0], xfer_length + 2,
					   xfer_length + 2);

		} else {
			if (code != STATUS_CONTINUED_READ) {
				TPD_INFO("Incorrect header code (0x%02x)\n", code);
				UNLOCK_BUFFER(tcm_info->temp);
				return -EIO;
			}

			retval = tp_memcpy(&in_buf[offset],
					   length - offset, &tcm_info->temp.buf[2],
					   xfer_length, xfer_length);
		}

		if (retval < 0) {
			TPD_INFO("Failed to copy data\n");
			UNLOCK_BUFFER(tcm_info->temp);
			return retval;
		}

		if (idx == 0) {
			offset += (xfer_length + 2);

		} else {
			offset += xfer_length;
		}

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_info->temp);

	return 0;
}

/**
 * syna_tcm_raw_write() - write command/data to device without receiving
 * response
 *
 * @tcm_info: handle of core module
 * @command: command to send to device
 * @data: data to send to device
 * @length: length of data in bytes
 *
 * A command and its data, if any, are sent to the device.
 */
static int syna_tcm_raw_write(struct syna_tcm_data *tcm_info,
			      unsigned char command,
			      unsigned char *data, unsigned int length)
{
	int retval = 0;
	char *report = NULL;
	unsigned int idx = 0, chunks = 0, chunk_space = 0;
	unsigned int xfer_length = 0, remaining_length = length;

	/* available chunk space for data = total chunk size minus command byte */
	if (tcm_info->wr_chunk_size == 0) {
		chunk_space = remaining_length;

	} else {
		chunk_space = tcm_info->wr_chunk_size - 1;
	}

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	LOCK_BUFFER(tcm_info->out);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space) {
			xfer_length = chunk_space;

		} else {
			xfer_length = remaining_length;
		}

		retval = syna_tcm_alloc_mem(&tcm_info->out, xfer_length + 1);

		if (retval < 0) {
			TPD_INFO("Failed to allocate memory for tcm_info->out.buf\n");
			UNLOCK_BUFFER(tcm_info->out);
			return retval;
		}

		if (idx == 0) {
			tcm_info->out.buf[0] = command;

		} else {
			tcm_info->out.buf[0] = CMD_CONTINUE_WRITE;
		}

		if (xfer_length) {
			retval = tp_memcpy(&tcm_info->out.buf[1],
					   xfer_length,
					   &data[idx * chunk_space],
					   remaining_length,
					   xfer_length);

			if (retval < 0) {
				TPD_INFO("Failed to copy data\n");
				UNLOCK_BUFFER(tcm_info->out);
				return retval;
			}
		}

		retval = syna_tcm_write(tcm_info, tcm_info->out.buf, xfer_length + 1);

		if (retval < 0) {
			report = tp_kzalloc(30, GFP_KERNEL);
			if (report) {
				snprintf(report, 30, "raw_write_err_%2x", command);
				tp_kfree((void **)&report);
			}
			TPD_INFO("Failed to write to device\n");
			UNLOCK_BUFFER(tcm_info->out);
			return retval;
		}

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_info->out);

	return 0;
}

/*add this for debug. remove before pvt*/
/*
static void syna_tcm_debug_message(char *buf, int len)
{
    int i = 0;
    char buffer[161] = {0};

    for (i = 0; i < len; i++) {
        if (i > 32)
            break;

        sprintf(&buffer[5 * i], "0x%02x ", buf[i]);
    }

    if (len > 0)
        TPD_INFO("payload data: %s\n", buffer);
}
*/

/**
 * syna_tcm_read_message() - read message from device
 *
 * @tcm_info: handle of core module
 * @in_buf: buffer for storing data in raw read mode
 * @length: length of data in bytes in raw read mode
 *
 * If in_buf is not NULL, raw read mode is used and syna_tcm_raw_read() is
 * called. Otherwise, a message including its entire payload is retrieved from
 * the device and dispatched to the appropriate destination.
 */
static int syna_tcm_read_message(struct syna_tcm_data *tcm_info,
				 unsigned char *in_buf, unsigned int length)
{
	int retval = 0;
	unsigned int total_length = 0;
	struct syna_tcm_message_header *header = NULL;

	TPD_DEBUG("%s\n", __func__);
	mutex_lock(&tcm_info->rw_mutex);

	if (in_buf != NULL) {
		retval = syna_tcm_raw_read(tcm_info, in_buf, length);
		goto exit;
	}

	LOCK_BUFFER(tcm_info->in);

	retval = syna_tcm_read(tcm_info, tcm_info->in.buf,
					 tcm_info->read_length);

	if (retval < 0) {
		TPD_INFO("Failed to read from device\n");
		UNLOCK_BUFFER(tcm_info->in);
		goto exit;
	}

	header = (struct syna_tcm_message_header *)tcm_info->in.buf;

	if (header->marker != MESSAGE_MARKER) {
		TPD_INFO("wrong header marker:0x%02x\n", header->marker);
		UNLOCK_BUFFER(tcm_info->in);
		retval = -ENXIO;
		goto exit;
	}

	tcm_info->report_code = header->code;
	tcm_info->payload_length = le2_to_uint(header->length);
	TPD_DEBUG("Header code = 0x%02x Payload len = %d\n", tcm_info->report_code,
		  tcm_info->payload_length);

	if (tcm_info->report_code <= STATUS_ERROR
	    || tcm_info->report_code == STATUS_INVALID) {
		switch (tcm_info->report_code) {
		case STATUS_OK:
			break;

		case STATUS_CONTINUED_READ:

		/*TPD_INFO("Out-of-sync continued read\n");*/
		case STATUS_IDLE:
		case STATUS_BUSY:
			tcm_info->payload_length = 0;
			UNLOCK_BUFFER(tcm_info->in);
			retval = 0;
			goto exit;

		default:
			TPD_INFO("Incorrect header code (0x%02x)\n", tcm_info->report_code);

			if (tcm_info->report_code != STATUS_ERROR) {
				UNLOCK_BUFFER(tcm_info->in);
				retval = -EIO;
				goto exit;
			}
		}
	}

	total_length = MESSAGE_HEADER_SIZE + tcm_info->payload_length + 1;

#ifdef PREDICTIVE_READING

	if (total_length <= tcm_info->read_length) {
		goto check_padding;

	} else if (total_length - 1 == tcm_info->read_length) {
		tcm_info->in.buf[total_length - 1] = MESSAGE_PADDING;
		goto check_padding;
	}

#else

	if (tcm_info->payload_length == 0) {
		tcm_info->in.buf[total_length - 1] = MESSAGE_PADDING;
		goto check_padding;
	}

#endif

	UNLOCK_BUFFER(tcm_info->in);

	retval = syna_tcm_continued_read(tcm_info);

	if (retval < 0) {
		TPD_INFO("Failed to do continued read\n");
		goto exit;
	}

	LOCK_BUFFER(tcm_info->in);

	tcm_info->in.buf[0] = MESSAGE_MARKER;

	tcm_info->in.buf[1] = tcm_info->report_code;

	tcm_info->in.buf[2] = (unsigned char)tcm_info->payload_length;

	tcm_info->in.buf[3] = (unsigned char)(tcm_info->payload_length >> 8);

check_padding:
	if (tcm_info->in.buf[total_length - 1] != MESSAGE_PADDING) {
		TPD_INFO("Incorrect message padding byte (0x%02x)\n",
			 tcm_info->in.buf[total_length - 1]);
		UNLOCK_BUFFER(tcm_info->in);
		retval = -EIO;
		goto exit;
	}

	UNLOCK_BUFFER(tcm_info->in);

#ifdef PREDICTIVE_READING
	total_length = MAX(total_length, MIN_READ_LENGTH);
	tcm_info->read_length = MIN(total_length, tcm_info->rd_chunk_size);

	if (tcm_info->rd_chunk_size == 0) {
		tcm_info->read_length = total_length;
	}

#endif

	/*add for debug, remove before pvt*/
	/*if (LEVEL_BASIC != tp_debug) {*/
	/*    syna_tcm_debug_message(&tcm_info->in.buf[4], tcm_info->payload_length);*/
	/*}*/

	syna_tcm_dispatch_message(tcm_info);

	retval = 0;

exit:

	if ((retval < 0) && (atomic_read(&tcm_info->command_status) == CMD_BUSY)) {
		atomic_set(&tcm_info->command_status, CMD_ERROR);
		complete(&tcm_info->response_complete);
	}

	mutex_unlock(&tcm_info->rw_mutex);

	return retval;
}

/**
 * syna_tcm_write_message() - write message to device and receive response
 *
 * @tcm_info: handle of core module
 * @command: command to send to device
 * @payload: payload of command
 * @length: length of payload in bytes
 * @resp_buf: buffer for storing command response
 * @resp_buf_size: size of response buffer in bytes
 * @resp_length: length of command response in bytes
 * @polling_delay_ms: delay time after sending command before resuming polling
 *
 * If resp_buf is NULL, raw write mode is used and syna_tcm_raw_write() is
 * called. Otherwise, a command and its payload, if any, are sent to the device
 * and the response to the command generated by the device is read in.
 */
static int syna_tcm_write_message(struct syna_tcm_data *tcm_info,
				  unsigned char command, unsigned char *payload,
				  unsigned int length, unsigned char **resp_buf,
				  unsigned int *resp_buf_size, unsigned int *resp_length,
				  unsigned int timeout)
{
	int retval = 0;
	char *report = NULL;
	unsigned int idx = 0, chunks = 0, chunk_space = 0;
	unsigned int xfer_length = 0, remaining_length = 0;
	unsigned int command_status = 0;
	unsigned int timeout_ms = 0;

	mutex_lock(&tcm_info->command_mutex);
	mutex_lock(&tcm_info->rw_mutex);

	if (resp_buf == NULL) {
		retval = syna_tcm_raw_write(tcm_info, command, payload, length);
		mutex_unlock(&tcm_info->rw_mutex);
		goto exit;
	}

	atomic_set(&tcm_info->command_status, CMD_BUSY);
	reinit_completion(&tcm_info->response_complete);
	tcm_info->command = command;

	LOCK_BUFFER(tcm_info->resp);

	tcm_info->resp.buf = *resp_buf;
	tcm_info->resp.buf_size = *resp_buf_size;
	tcm_info->resp.data_length = 0;

	UNLOCK_BUFFER(tcm_info->resp);

	/* adding two length bytes as part of payload */
	remaining_length = length + 2;

	/* available chunk space for payload = total chunk size minus command
	 * byte */
	if (tcm_info->wr_chunk_size == 0) {
		chunk_space = remaining_length;

	} else {
		chunk_space = tcm_info->wr_chunk_size - 1;
	}

	chunks = ceil_div(remaining_length, chunk_space);

	chunks = chunks == 0 ? 1 : chunks;

	TPD_DEBUG("%s:Command = 0x%02x\n", __func__, command);

	LOCK_BUFFER(tcm_info->out);

	for (idx = 0; idx < chunks; idx++) {
		if (remaining_length > chunk_space) {
			xfer_length = chunk_space;

		} else {
			xfer_length = remaining_length;
		}

		retval = syna_tcm_alloc_mem(&tcm_info->out, xfer_length + 1);

		if (retval < 0) {
			TPD_INFO("Failed to allocate memory for tcm_info->out.buf\n");
			UNLOCK_BUFFER(tcm_info->out);
			mutex_unlock(&tcm_info->rw_mutex);
			goto exit;
		}

		if (idx == 0) {
			tcm_info->out.buf[0] = command;
			tcm_info->out.buf[1] = (unsigned char)length;
			tcm_info->out.buf[2] = (unsigned char)(length >> 8);

			if (xfer_length > 2) {
				retval = tp_memcpy(&tcm_info->out.buf[3],
						   xfer_length - 2,
						   payload,
						   remaining_length - 2,
						   xfer_length - 2);

				if (retval < 0) {
					TPD_INFO("Failed to copy payload\n");
					UNLOCK_BUFFER(tcm_info->out);
					mutex_unlock(&tcm_info->rw_mutex);
					goto exit;
				}
			}

		} else {
			tcm_info->out.buf[0] = CMD_CONTINUE_WRITE;

			retval = tp_memcpy(&tcm_info->out.buf[1],
					   xfer_length,
					   &payload[idx * chunk_space - 2],
					   remaining_length,
					   xfer_length);

			if (retval < 0) {
				TPD_INFO("Failed to copy payload\n");
				UNLOCK_BUFFER(tcm_info->out);
				mutex_unlock(&tcm_info->rw_mutex);
				goto exit;
			}
		}

		retval = syna_tcm_write(tcm_info, tcm_info->out.buf, xfer_length + 1);

		if (retval < 0) {
			report = tp_kzalloc(30, GFP_KERNEL);
			if (report) {
				snprintf(report, 30, "write_msg_err_wr%2x", command);
				tp_kfree((void **)&report);
			}
			TPD_INFO("Failed to write to device\n");
			UNLOCK_BUFFER(tcm_info->out);
			mutex_unlock(&tcm_info->rw_mutex);
			goto exit;
		}

		remaining_length -= xfer_length;
	}

	UNLOCK_BUFFER(tcm_info->out);

	mutex_unlock(&tcm_info->rw_mutex);

	if (timeout == 0) {
		timeout_ms = RESPONSE_TIMEOUT_MS_DEFAULT;

	} else {
		timeout_ms = timeout;
	}

	retval = wait_for_completion_timeout(&tcm_info->response_complete,
					     msecs_to_jiffies(timeout_ms));

	if (retval == 0) {
		TPD_INFO("Timed out waiting for response (command 0x%02x)\n",
			 tcm_info->command);
		report = tp_kzalloc(30, GFP_KERNEL);
		if (report) {
			snprintf(report, 30, "write_msg_err_wait%2x", tcm_info->command);
			tp_kfree((void **)&report);
		}
		retval = -EIO;

	} else {
		command_status = atomic_read(&tcm_info->command_status);

		if (command_status != CMD_IDLE ||
		    tcm_info->report_code == STATUS_ERROR) {
			TPD_INFO("Failed to get valid response\n");
			report = tp_kzalloc(30, GFP_KERNEL);
			if (report) {
				snprintf(report, 30, "write_msg_err_resp%2x", tcm_info->command);
				tp_kfree((void **)&report);
			}
			retval = -EIO;
			goto exit;
		}

		retval = 0;
	}

exit:

	if (command_status == CMD_IDLE) {
		LOCK_BUFFER(tcm_info->resp);

		if (tcm_info->report_code == STATUS_ERROR) {
			if (tcm_info->resp.data_length) {
				TPD_INFO("Error code = 0x%02x\n",
					 tcm_info->resp.buf[0]);
			}
		}

		if (resp_buf != NULL) {
			*resp_buf = tcm_info->resp.buf;
			*resp_buf_size = tcm_info->resp.buf_size;
			*resp_length = tcm_info->resp.data_length;
		}

		UNLOCK_BUFFER(tcm_info->resp);
	}

	tcm_info->command = CMD_NONE;
	atomic_set(&tcm_info->command_status, CMD_IDLE);
	mutex_unlock(&tcm_info->command_mutex);

	return retval;
}

static int syna_tcm_get_app_info(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;
	unsigned int timeout = APP_STATUS_POLL_TIMEOUT_MS;

get_app_info:
	retval = syna_tcm_write_message(tcm_info,
					CMD_GET_APPLICATION_INFO,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_GET_APPLICATION_INFO));
		goto exit;
	}

	retval = tp_memcpy((unsigned char *)&tcm_info->app_info,
			   sizeof(tcm_info->app_info),
			   resp_buf,
			   resp_buf_size,
			   MIN(sizeof(tcm_info->app_info), resp_length));

	if (retval < 0) {
		TPD_INFO("Failed to copy application info\n");
		goto exit;
	}

	tcm_info->app_status = le2_to_uint(tcm_info->app_info.status);

	if (tcm_info->app_status == APP_STATUS_BOOTING
	    || tcm_info->app_status == APP_STATUS_UPDATING) {
		if (timeout > 0) {
			msleep(APP_STATUS_POLL_MS);
			timeout -= APP_STATUS_POLL_MS;
			goto get_app_info;
		}
	}

	retval = 0;

exit:
	tp_kfree((void **)&resp_buf);

	return retval;
}

static int syna_tcm_get_boot_info(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;

	retval = syna_tcm_write_message(tcm_info,
					CMD_GET_BOOT_INFO,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_GET_BOOT_INFO));
		goto exit;
	}

	retval = tp_memcpy((unsigned char *)&tcm_info->boot_info,
			   sizeof(tcm_info->boot_info),
			   resp_buf,
			   resp_buf_size,
			   MIN(sizeof(tcm_info->boot_info), resp_length));

	if (retval < 0) {
		TPD_INFO("Failed to copy boot info\n");
		goto exit;
	}

	retval = 0;

exit:
	tp_kfree((void **)&resp_buf);

	return retval;
}

static int syna_tcm_identify(struct syna_tcm_data *tcm_info, bool id)
{
	int retval;
	unsigned char *resp_buf;
	unsigned int resp_buf_size;
	unsigned int resp_length;

	resp_buf = NULL;
	resp_buf_size = 0;

	mutex_lock(&tcm_info->identify_mutex);

	if (!id) {
		goto get_info;
	}

	retval = syna_tcm_write_message(tcm_info,
					CMD_IDENTIFY,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_IDENTIFY));
		goto exit;
	}

	retval = tp_memcpy((unsigned char *)&tcm_info->id_info,
			   sizeof(tcm_info->id_info),
			   resp_buf,
			   resp_buf_size,
			   MIN(sizeof(tcm_info->id_info), resp_length));

	if (retval < 0) {
		TPD_INFO("Failed to copy identification info\n");
		goto exit;
	}

	syna_tcm_resize_chunk_size(tcm_info);

get_info:

	if (tcm_info->id_info.mode == MODE_APPLICATION) {
		retval = syna_tcm_get_app_info(tcm_info);

		if (retval < 0) {
			TPD_INFO("Failed to get application info\n");
			goto exit;
		}

	} else {
		retval = syna_tcm_get_boot_info(tcm_info);

		if (retval < 0) {
			TPD_INFO("Failed to get boot info\n");
			goto exit;
		}
	}

	retval = 0;

exit:
	mutex_unlock(&tcm_info->identify_mutex);

	tp_kfree((void **)&resp_buf);

	return retval;
}

static int syna_tcm_run_application_firmware(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	bool retry = true;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;

retry:
	retval = syna_tcm_write_message(tcm_info,
					CMD_RUN_APPLICATION_FIRMWARE,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_RUN_APPLICATION_FIRMWARE));
		goto exit;
	}

	retval = syna_tcm_identify(tcm_info, false);

	if (retval < 0) {
		TPD_INFO("Failed to do identification\n");
		goto exit;
	}

	if (tcm_info->id_info.mode != MODE_APPLICATION) {
		TPD_INFO("Failed to run application firmware (boot status = 0x%02x)\n",
			 tcm_info->boot_info.status);

		if (retry) {
			retry = false;
			goto retry;
		}

		retval = -EINVAL;
		goto exit;

	} else if (tcm_info->app_status != APP_STATUS_OK) {
		TPD_INFO("Application status = 0x%02x\n", tcm_info->app_status);
	}

	retval = 0;

exit:
	tp_kfree((void **)&resp_buf);

	return retval;
}


static int syna_tcm_run_bootloader_firmware(struct syna_tcm_data *tcm_info)
{
	int retval = 0;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;

	retval = syna_tcm_write_message(tcm_info,
					CMD_RUN_BOOTLOADER_FIRMWARE,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_RUN_BOOTLOADER_FIRMWARE));
		goto exit;
	}

	retval = syna_tcm_identify(tcm_info, false);

	if (retval < 0) {
		TPD_INFO("Failed to do identification\n");
		goto exit;
	}

	if (tcm_info->id_info.mode == MODE_APPLICATION) {
		TPD_INFO("Failed to enter bootloader mode\n");
		retval = -EINVAL;
		goto exit;
	}

	retval = 0;

exit:
	tp_kfree((void **)&resp_buf);

	return retval;
}


static int syna_tcm_switch_mode(struct syna_tcm_data *tcm_info,
				enum firmware_mode mode)
{
	int retval = 0;

	mutex_lock(&tcm_info->reset_mutex);

	switch (mode) {
	case FW_MODE_BOOTLOADER:
		retval = syna_tcm_run_bootloader_firmware(tcm_info);

		if (retval < 0) {
			TPD_INFO("Failed to switch to bootloader mode\n");
			goto exit;
		}

		break;

	case FW_MODE_APPLICATION:
		retval = syna_tcm_run_application_firmware(tcm_info);

		if (retval < 0) {
			TPD_INFO("Failed to switch to application mode\n");
			goto exit;
		}

		break;

	default:
		TPD_INFO("Invalid firmware mode\n");
		retval = -EINVAL;
		goto exit;
	}

exit:
	mutex_unlock(&tcm_info->reset_mutex);

	return retval;
}

static int syna_tcm_get_dynamic_config(struct syna_tcm_data *tcm_info,
				       enum dynamic_config_id id, unsigned short *value)
{
	int retval = 0;
	char *report = NULL;
	unsigned char out_buf = (unsigned char)id;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;

	retval = syna_tcm_write_message(tcm_info,
					CMD_GET_DYNAMIC_CONFIG,
					&out_buf,
					sizeof(out_buf),
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					RESPONSE_TIMEOUT_MS_SHORT);

	if (retval < 0 || resp_length < 2) {
		retval = -EINVAL;
		TPD_INFO("Failed to read dynamic config\n");
		report = tp_kzalloc(30, GFP_KERNEL);
		if (report) {
			snprintf(report, 30, "get_dc_err_%2x", id);
			tp_kfree((void **)&report);
		}
		goto exit;
	}

	*value = (unsigned short)le2_to_uint(resp_buf);
exit:
	tp_kfree((void **)&resp_buf);
	return retval;
}

static int syna_tcm_set_dynamic_config(struct syna_tcm_data *tcm_info,
				       enum dynamic_config_id id, unsigned short value)
{
	int retval = 0;
	char *report = NULL;
	unsigned char out_buf[3] = {0};
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;

	TPD_DEBUG("%s:config 0x%x, value %d\n", __func__, id, value);

	out_buf[0] = (unsigned char)id;
	out_buf[1] = (unsigned char)value;
	out_buf[2] = (unsigned char)(value >> 8);

	retval = syna_tcm_write_message(tcm_info,
					CMD_SET_DYNAMIC_CONFIG,
					out_buf,
					sizeof(out_buf),
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					RESPONSE_TIMEOUT_MS_SHORT);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_SET_DYNAMIC_CONFIG));
		report = tp_kzalloc(30, GFP_KERNEL);
		if (report) {
			snprintf(report, 30, "set_dc_err_%2x", id);
			tp_kfree((void **)&report);
		}
		goto exit;
	}

exit:
	tp_kfree((void **)&resp_buf);

	return retval;
}

static int syna_tcm_sleep(struct syna_tcm_data *tcm_info, bool en)
{
	int retval = 0;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0;
	unsigned char command = en ? CMD_ENTER_DEEP_SLEEP : CMD_EXIT_DEEP_SLEEP;

	TPD_INFO("%s: %s .\n", __func__, en ? "enter" : "exit");

	retval = syna_tcm_write_message(tcm_info,
					command,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n",
			 en ? STR(CMD_ENTER_DEEP_SLEEP) : STR(CMD_EXIT_DEEP_SLEEP));
		goto exit;
	}

exit:
	tp_kfree((void **)&resp_buf);

	return retval;
}

static int syna_report_refresh_switch(void *chip_data, int fps)
{
	int retval = 0;
	unsigned short send_value = 1;
	int i = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	TPD_DEBUG("%s: refresh_switch: %d HZ!\n", __func__, fps);
	if (tcm_info == NULL) {
		return -1;
	}
	tcm_info->display_refresh_rate = fps;

	if (!*tcm_info->in_suspend && !tcm_info->game_mode) {
		for (i = 0; i < tcm_info->fps_report_rate_num; i = i + 2) {
			if (fps == tcm_info->fps_report_rate_array[i]) {
				send_value = tcm_info->fps_report_rate_array[i + 1];
			}
		}
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_SET_REPORT_FRE, send_value);
		if (retval < 0) {
			TPD_INFO("Failed to set dynamic report frequence config\n");
		}
		TPD_INFO("%s: refresh_switch: %d HZ %s!\n", __func__, fps, retval < 0 ? "failed" : "success");
	}
	return retval;
}

static void syna_rate_white_list_ctrl(void *chip_data, int value)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned short send_value = 1;
	int retval = 0;

	if (tcm_info == NULL) {
		return;
	}

	if (*tcm_info->in_suspend || tcm_info->game_mode) {
		return;
	}
	switch (value) {
	case 0: /* 60Hz */
		send_value = 1;
		break;
	case 1: /* 120Hz */
		send_value = 2;
		break;
	case 2: /* 90Hz */
		send_value = 3;
		break;
	default:
		return;
	}

	retval = syna_tcm_set_dynamic_config(tcm_info,
					     DC_SET_REPORT_FRE,
					     send_value);
	if (retval < 0) {
		TPD_INFO("Failed to set dynamic report frequence config\n");
	}
	TPD_INFO("%s: DC_SET_REPORT_FRE: %d  %s!\n",
		 __func__, send_value, retval < 0 ? "failed" : "success");
}

static int synaptics_resetgpio_set(struct hw_resource *hw_res, bool on)
{
	if (gpio_is_valid(hw_res->reset_gpio)) {
		TPD_DEBUG("Set the reset_gpio \n");
		gpio_direction_output(hw_res->reset_gpio, on);
	}

	return 0;
}

static int syna_tcm_reset(void *chip_data)
{
	int retval = 0;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0;
	unsigned int resp_length = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	syna_tcm_fw_update_in_bl(chip_data);

	mutex_lock(&tcm_info->reset_mutex);

	synaptics_resetgpio_set(tcm_info->hw_res, false);
	msleep(POWEWRUP_TO_RESET_TIME);
	synaptics_resetgpio_set(tcm_info->hw_res, true);
	msleep(RESET_TO_NORMAL_TIME);

	retval = syna_tcm_identify(tcm_info, false);

	if (retval < 0) {
		TPD_INFO("Failed to do identification\n");
		goto exit;
	}

	if (tcm_info->id_info.mode == MODE_APPLICATION) {
		goto dispatch_reset;
	}

	retval = syna_tcm_write_message(tcm_info,
					CMD_RUN_APPLICATION_FIRMWARE,
					NULL,
					0,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_RUN_APPLICATION_FIRMWARE));
	}

	retval = syna_tcm_identify(tcm_info, false);

	if (retval < 0) {
		TPD_INFO("Failed to do identification\n");
		goto exit;
	}

dispatch_reset:
	TPD_INFO("Firmware mode = 0x%02x, boot status 0x%02x, app status 0x%02x\n",
		 tcm_info->id_info.mode,
		 tcm_info->boot_info.status,
		 tcm_info->app_status);

exit:
	mutex_unlock(&tcm_info->reset_mutex);

	tp_kfree((void **)&resp_buf);

	return retval;
}

static int syna_get_chip_info(void *chip_data)
{
	int ret = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	TPD_INFO("%s: Enter\n", __func__);

	ret = syna_tcm_reset(tcm_info);  /* reset to get bootloader info or boot info*/

	if (ret < 0) {
		TPD_INFO("failed to reset device\n");
	}

	ret = syna_get_default_report_config(tcm_info);

	if (ret < 0) {
		TPD_INFO("failed to get default report config\n");
	}

	return 0;
}

static int syna_get_vendor(void *chip_data, struct panel_info *panel_data)
{
	char manu_temp[MAX_DEVICE_MANU_LENGTH] = SYNAPTICS_PREFIX;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	tcm_info->iHex_name = panel_data->extra;

	strlcat(manu_temp, panel_data->manufacture_info.manufacture,
		MAX_DEVICE_MANU_LENGTH);
	strncpy(panel_data->manufacture_info.manufacture, manu_temp,
		MAX_DEVICE_MANU_LENGTH);
	TPD_INFO("chip_info->tp_type = %d, panel_data->fw_name = %s\n",
		 panel_data->tp_type, panel_data->fw_name);
	return 0;
}

static u32 syna_trigger_reason(void *chip_data, int gesture_enable,
			       int is_suspended)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	tcm_info->trigger_reason = 0;
	syna_tcm_read_message(tcm_info, NULL, 0);

	return tcm_info->trigger_reason;
}

static int syna_get_touch_points(void *chip_data, struct point_info *points,
				 int max_num)
{
	unsigned int idx = 0, status = 0, i = 0;
	struct object_data *object_data = NULL;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touchpanel_snr *snr = tcm_info->snr;

	if (points == NULL) {
		return tcm_info->obj_attention;
	}

	if (tcm_info->snr_read_support) {
		for (i = 0; i < max_num; i++) {
			snr[i].point_status = 0;
		}
	}
	object_data = touch_hcd->touch_data.object_data;

	for (idx = 0; idx < touch_hcd->max_objects; idx++) {
		status = object_data[idx].status;

		if (status != LIFT) {
			tcm_info->obj_attention |= (0x1 << idx);

		} else {
			if ((~tcm_info->obj_attention) & ((0x1) << idx)) {
				continue;

			} else {
				tcm_info->obj_attention &= (~(0x1 << idx));
			}
		}

		points[idx].x = object_data[idx].x_pos;
		points[idx].y = object_data[idx].y_pos;
		points[idx].touch_major = max(object_data[idx].x_width, object_data[idx].y_width);
		points[idx].width_major = min(object_data[idx].x_width, object_data[idx].y_width);
		points[idx].tx_press = object_data[idx].exwidth;
		points[idx].rx_press = object_data[idx].eywidth;
		points[idx].tx_er = object_data[idx].xeratio;
		points[idx].rx_er = object_data[idx].yeratio;
		points[idx].status = 1;
		if (tcm_info->snr_read_support) {
			if (snr[idx].doing && points[idx].x && points[idx].y) {
				snr[idx].point_status = 1;
				snr[idx].x = points[idx].x;
				snr[idx].y = points[idx].y;
				snr[idx].channel_x = snr[idx].x * tcm_info->hw_res->tx_num / tcm_info->chip_resolution_info->max_x;
				snr[idx].channel_y = snr[idx].y * tcm_info->hw_res->rx_num / tcm_info->chip_resolution_info->max_y;
				TPD_DEBUG("snr: [%d %d, %d] {%d %d} obj 0x%x status %d\n", snr[idx].x, snr[idx].y, idx,
						snr[idx].channel_x, snr[idx].channel_y, tcm_info->obj_attention, status);
			}
		}
	}

	return tcm_info->obj_attention;
}

static int syna_get_touch_points_auto(void *chip_data,
				      struct point_info *points,
				      int max_num,
				      struct resolution_info *resolution_info)
{
	unsigned int idx = 0, status = 0, i = 0;
	struct object_data *object_data = NULL;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touchpanel_snr *snr = tcm_info->snr;

	int max_x_inchip = 0;
	int max_y_inchip = 0;
	int max_x = 0;
	int max_y = 0;

	if (points == NULL) {
		return tcm_info->obj_attention;
	}

	if (tcm_info->snr_read_support) {
		for (i = 0; i < max_num; i++) {
			snr[i].point_status = 0;
		}
	}
	object_data = touch_hcd->touch_data.object_data;

	max_x_inchip = le2_to_uint(tcm_info->app_info.max_x) + 1;
	max_y_inchip = le2_to_uint(tcm_info->app_info.max_y) + 1;
	max_x = resolution_info->max_x;
	max_y = resolution_info->max_y;


	for (idx = 0; idx < touch_hcd->max_objects; idx++) {
		status = object_data[idx].status;
		if (status != LIFT) {
			tcm_info->obj_attention |= (0x1 << idx);
		} else {
			if ((~tcm_info->obj_attention) & ((0x1) << idx)) {
				continue;
			} else {
				tcm_info->obj_attention &= (~(0x1 << idx));
			}
		}

		if (max_x_inchip == max_x) {
			points[idx].x = object_data[idx].x_pos;
		} else {
			points[idx].x = (object_data[idx].x_pos * max_x) / max_x_inchip;
		}
		if (max_y_inchip == max_y) {
			points[idx].y = object_data[idx].y_pos;
		} else {
			points[idx].y = (object_data[idx].y_pos * max_y) / max_y_inchip;
		}
		points[idx].touch_major = max(object_data[idx].x_width, object_data[idx].y_width);
		points[idx].width_major = min(object_data[idx].x_width, object_data[idx].y_width);
		points[idx].tx_press = object_data[idx].exwidth;
		points[idx].rx_press = object_data[idx].eywidth;
		points[idx].tx_er = object_data[idx].xeratio;
		points[idx].rx_er = object_data[idx].yeratio;
		points[idx].status = 1;
		if (tcm_info->snr_read_support) {
			if (snr[idx].doing && points[idx].x && points[idx].y) {
				snr[idx].point_status = 1;
				snr[idx].x = points[idx].x;
				snr[idx].y = points[idx].y;
				snr[idx].channel_x = snr[idx].x * tcm_info->hw_res->tx_num / tcm_info->chip_resolution_info->max_x;
				snr[idx].channel_y = snr[idx].y * tcm_info->hw_res->rx_num / tcm_info->chip_resolution_info->max_y;
				TPD_DEBUG("snr: [%d %d, %d] {%d %d} obj 0x%x status %d\n", snr[idx].x, snr[idx].y, idx,
						snr[idx].channel_x, snr[idx].channel_y, tcm_info->obj_attention, status);
			}
		}
	}

	return tcm_info->obj_attention;
}

static int syna_get_touch_points_help(void *chip_data,
				      struct point_info *points,
				      int max_num,
				      struct resolution_info *resolution_info)
{
	unsigned int idx = 0, status = 0;
	struct object_data *object_data = NULL;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	unsigned int obj_attention = 0x00;
	int max_x_inchip = 0;
	int max_y_inchip = 0;
	int max_x = 0;
	int max_y = 0;

	if (points == NULL) {
		return obj_attention;
	}
	object_data = touch_hcd->touch_data.object_data;

	max_x_inchip = le2_to_uint(tcm_info->app_info.max_x) + 1;
	max_y_inchip = le2_to_uint(tcm_info->app_info.max_y) + 1;
	max_x = resolution_info->max_x;
	max_y = resolution_info->max_y;


	for (idx = 0; idx < touch_hcd->max_objects; idx++) {
		status = object_data[idx].status;
		if (status == LIFT) {
			points[idx].status = 0;
			continue;
		}
		obj_attention |= (0x1 << idx);

		if (max_x_inchip == max_x) {
			points[idx].x = object_data[idx].x_pos;
		} else {
			points[idx].x = (object_data[idx].x_pos * max_x) / max_x_inchip;
		}
		if (max_y_inchip == max_y) {
			points[idx].y = object_data[idx].y_pos;
		} else {
			points[idx].y = (object_data[idx].y_pos * max_y) / max_y_inchip;
		}
		points[idx].touch_major = max(object_data[idx].x_width, object_data[idx].y_width);
		points[idx].width_major = min(object_data[idx].x_width, object_data[idx].y_width);
		points[idx].tx_press = object_data[idx].exwidth;
		points[idx].rx_press = object_data[idx].eywidth;
		points[idx].tx_er = object_data[idx].xeratio;
		points[idx].rx_er = object_data[idx].yeratio;
		points[idx].status = 1;
	}

	return obj_attention;
}

static int syna_tcm_set_gesture_mode(struct syna_tcm_data *tcm_info, int enable)
{
	int retval = 0;
	/*this command may take too much time, if needed can add flag to skip this */
	TPD_INFO("%s: enable(%d), mask 0x%0X\n", __func__, enable,
		 tcm_info->gesture_mask);

	if (enable) {
		retval = syna_tcm_sleep(tcm_info, false);

		if (retval < 0) {
			TPD_INFO("%s: Failed to exit sleep mode\n", __func__);
			return retval;
		}

		retval = syna_set_input_reporting(tcm_info, true);

		if (retval < 0) {
			TPD_INFO("%s: Failed to set input reporting\n", __func__);
			return retval;
		}

		retval = syna_tcm_set_dynamic_config(tcm_info, DC_IN_WAKEUP_GESTURE_MODE, true);

		if (retval < 0) {
			TPD_INFO("%s: Failed to set dynamic gesture config\n", __func__);
			return retval;
		}

		retval = syna_tcm_set_dynamic_config(tcm_info,
						     DC_GESTURE_MASK,
						     tcm_info->gesture_mask);

		if (retval < 0) {
			TPD_INFO("%s: Failed to set dynamic gesture mask config\n", __func__);
			return retval;
		}

		if (tp_debug != LEVEL_DEBUG) {
			retval = syna_tcm_enable_report(tcm_info, REPORT_LOG, false);

			if (retval < 0) {
				TPD_INFO("Failed to set disable log report\n");
				return retval;
			}
		}
	}

	return retval;
}

static void syna_tcm_enable_gesture_mask(void *chip_data, uint32_t enable)
{
	int retval = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	/*this command may take too much time, if needed can add flag to skip this */
	TPD_INFO("%s: enable(%d), mask 0x%0X\n", __func__, enable, tcm_info->gesture_mask);

	if (enable) {
		retval = syna_tcm_set_dynamic_config(tcm_info,
						     DC_GESTURE_MASK,
						     tcm_info->gesture_mask);

		if (retval < 0) {
			TPD_INFO("%s: Failed to set dynamic gesture mask config\n", __func__);
		}

	} else {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_GESTURE_MASK, 0x0000);

		if (retval < 0) {
			TPD_INFO("%s: Failed to set dynamic gesture mask config\n", __func__);
		}
	}
}

static int syna_tcm_set_game_mode(struct syna_tcm_data *tcm_info, int enable)
{
	int retval = 0;
	unsigned short regval = 0;
	uint16_t report_rate = 0;
	struct touchpanel_data *ts = spi_get_drvdata(tcm_info->client);
	/*unsigned short noise_length = 0;*/

	tcm_info->game_mode = !!enable;
	retval = syna_tcm_get_dynamic_config(tcm_info, DC_ERROR_PRIORITY, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get DC_ERROR_PRIORITY val\n");
		return retval;
	}
	TPD_INFO("%s: enable[%d], now reg status[0x%x]\n", __func__, tcm_info->game_mode, regval);

	if (enable) {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_ERROR_PRIORITY, regval|0x01);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic error priority config\n");
			return retval;
		}

		/*noise_length = 0x0A;

		retval = syna_tcm_set_dynamic_config(tcm_info, DC_NOISE_LENGTH, noise_length);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic noise length config\n");
			return retval;
		}*/


		if (tcm_info->switch_game_rate_support) {/*tcm_info->game_rate_switch_support*/
	    	switch (ts->noise_level) {
	    	case 180:
		    report_rate = 3;
		    break;
	    	case 300:
		    report_rate = 1;
		    break;
	    	case 600:
		    report_rate = 9;
		    break;
	    	default:
		    report_rate = tcm_info->game_rate;
		    break;
	    	}
	    	TPD_INFO("%s:set report_rate:%d", __func__, report_rate);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_SET_REPORT_FRE, report_rate);
		if (retval < 0) {
		    TPD_INFO("Failed to set dynamic report frequence config\n");
		    return retval;
		}
	} else {
	    retval = syna_tcm_set_dynamic_config(tcm_info, DC_SET_REPORT_FRE, tcm_info->game_rate);
	    if (retval < 0) {
		TPD_INFO("Failed to set dynamic report frequence config\n");
                return retval;
	    }
	}
	} else {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_ERROR_PRIORITY, regval&0xF0);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic error priority config\n");
			return retval;
		}

		/*retval = syna_tcm_set_dynamic_config(tcm_info, DC_NOISE_LENGTH,
						     tcm_info->default_noise_length);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic noise length config\n");
			return retval;
		}*/

		syna_report_refresh_switch(tcm_info, tcm_info->display_refresh_rate);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic report frequence config\n");
			return retval;
		}
	}

	return retval;
}

static int syna_tcm_set_high_frame_rate(void *chip_data, int level, int time)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned short regval = 0;
	int retval = 0;
	int temp_time = 0;

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_ERROR_PRIORITY, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get high frame config\n");
		return 0;
	}

	if (level > 0) {
		temp_time = time;
		if (0 == temp_time) {
			temp_time = 12;
		} else if (temp_time > 0) {
			temp_time = temp_time / 5;
		}
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_ERROR_PRIORITY, regval|0x02|(temp_time << 8));
		if (retval < 0) {
			TPD_INFO("Failed to enable high frame config\n");
			goto OUT;
		}
	} else {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_ERROR_PRIORITY, regval&0xfffd);
		if (retval < 0) {
			TPD_INFO("Failed to disable high frame config\n");
			goto OUT;
		}
	}

	TPD_INFO("synaptics %s high frame success lv to %d, time to %d",
		level > 0 ? "enable" : "disable", level, time);

OUT:
	return 0;
}


static int syna_tcm_normal_mode(struct syna_tcm_data *tcm_info)
{
	int retval;

	TPD_INFO("%s : enter\n", __func__);
	retval = syna_set_input_reporting(tcm_info, false);

	if (retval < 0) {
		TPD_INFO("Failed to set input reporting\n");
		return retval;
	}

	retval = syna_tcm_set_dynamic_config(tcm_info, DC_IN_WAKEUP_GESTURE_MODE,
					     false);

	if (retval < 0) {
		TPD_INFO("Failed to set dynamic gesture config\n");
		return retval;
	}

	syna_report_refresh_switch(tcm_info, tcm_info->display_refresh_rate);

	retval = syna_tcm_enable_report(tcm_info, REPORT_LOG, true);

	if (retval < 0) {
		TPD_INFO("Failed to set enable log report\n");
		return retval;
	}

	return retval;
}

static int syna_corner_limit_handle(struct syna_tcm_data *tcm_info, int enable)
{
	int ret = -1;

	if (LANDSCAPE_SCREEN_90 == enable) {
		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL,
						  0x01);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_SEL, 0x0F);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_SEL\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_X, 0x0A);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_X\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_Y, 0x0A);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_Y\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_DARK_ZONE_ENABLE, 0x03);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_DARK_ZONE_ENABLE\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_X, 0xFF);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_DARK_ZONE_X\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_Y, 0x44);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_DARK_ZONE_Y\n", __func__);
			return ret;
		}

	} else if (LANDSCAPE_SCREEN_270 == enable) {
		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL,
						  0x01);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_SEL, 0x0F);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_SEL\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_X, 0x0A);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_X\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_Y, 0x0A);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_Y\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_DARK_ZONE_ENABLE, 0x0C);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_DARK_ZONE_ENABLE\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_X, 0xFF);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_DARK_ZONE_X\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_Y, 0x44);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_DARK_ZONE_Y\n", __func__);
			return ret;
		}

	} else if (VERTICAL_SCREEN == enable) {
		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL,
						  0x00);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_SEL, 0x03);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_SEL\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_X, 0x0A);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_X\n", __func__);
			return ret;
		}

		/*ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_U, 0x32);*/
		/*if (ret < 0) {*/
		/*    TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_U\n", __func__);*/
		/*    return ret;*/
		/*}*/
		/*ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_V, 0x64);*/
		/*if (ret < 0) {*/
		/*    TPD_INFO("%s:failed to set DC_GRIP_ABS_DARK_V\n", __func__);*/
		/*    return ret;*/
		/*}*/
		ret = syna_tcm_set_dynamic_config(tcm_info, DC_DARK_ZONE_ENABLE, 0x05);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_DARK_ZONE_ENABLE\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_X, 0x24);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_DARK_ZONE_X\n", __func__);
			return ret;
		}

		ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_Y, 0xF5);

		if (ret < 0) {
			TPD_INFO("%s:failed to set DC_GRIP_DARK_ZONE_Y\n", __func__);
			return ret;
		}
	}

	return ret;
}

static int syna_enable_edge_limit(struct syna_tcm_data *tcm_info, int enable)
{
	int ret = 0;
	TPD_INFO("%s: enter\n", __func__);

	ret = syna_tcm_set_dynamic_config(tcm_info, DC_GRIP_ENABLED, 0x01);

	if (ret < 0) {
		TPD_INFO("%s:failed to enable grip suppression\n", __func__);
		return ret;
	}

	ret = syna_corner_limit_handle(tcm_info, enable);

	if (ret < 0) {
		TPD_INFO("%s:failed to set grip suppression para\n", __func__);
		return ret;
	}

	return ret;
}

static int syna_mode_switch(void *chip_data, work_mode mode, int flag)
{
	int ret = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	switch (mode) {
	case MODE_NORMAL:
		if (!*tcm_info->in_suspend) {
			ret = syna_tcm_normal_mode(tcm_info);

			if (ret < 0) {
				tcm_info->error_state_count++;
				TPD_INFO("normal mode switch failed\n");
				if (tcm_info->error_state_count >= ERROR_STATE_MAX) {
					syna_tcm_reset(tcm_info); /*ic state err, need to reset the IC*/
				}
			}
			tcm_info->error_state_count = 0;
		}

		break;

	case MODE_GESTURE:
		if (*tcm_info->in_suspend) {
			ret = syna_tcm_set_gesture_mode(tcm_info, flag);

			if (ret < 0) {
				TPD_INFO("%s:Failed to set gesture mode\n", __func__);
			}
		}

		break;

	case MODE_SLEEP:
		ret = syna_tcm_sleep(tcm_info, flag);

		if (ret < 0) {
			TPD_INFO("%s: failed to switch to sleep", __func__);
		}

		break;

	case MODE_CHARGE:
		ret = syna_tcm_set_dynamic_config(tcm_info, DC_CHARGER_CONNECTED, flag ? 1 : 0);

		if (ret < 0) {
			TPD_INFO("%s:failed to set charger mode\n", __func__);
		}

		break;

	case MODE_EDGE:
		ret = syna_enable_edge_limit(tcm_info, flag);
		if (ret < 0) {
			TPD_INFO("%s: failed to enable edg limit.\n", __func__);
		}

		break;

	case MODE_GAME:
		ret = syna_tcm_set_game_mode(tcm_info, flag);

		if (ret < 0) {
			TPD_INFO("%s:failed to set game mode\n", __func__);
		}

		break;

	default:
		break;
	}

	return 0;
}

static int syna_ftm_process(void *chip_data)
{
	TPD_INFO("%s: go into sleep\n", __func__);
	syna_get_chip_info(chip_data);
	syna_mode_switch(chip_data, MODE_SLEEP, true);
	return 0;
}

static int syna_tcm_reinit_device(void *chip_data)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	complete_all(&tcm_info->response_complete);
	complete_all(&tcm_info->report_complete);

	return 0;
}

static int syna_power_control(void *chip_data, bool enable)
{
	int ret = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	TPD_DEBUG("%s: %d\n", __func__, enable);

	if (true == enable) {
		ret = tp_powercontrol_avdd(tcm_info->hw_res, true);

		if (ret) {
			return -1;
		}

		ret = tp_powercontrol_vddi(tcm_info->hw_res, true);

		if (ret) {
			return -1;
		}

		synaptics_resetgpio_set(tcm_info->hw_res, false);
		msleep(POWEWRUP_TO_RESET_TIME);
		synaptics_resetgpio_set(tcm_info->hw_res, true);
		msleep(RESET_TO_NORMAL_TIME);

	} else {
		synaptics_resetgpio_set(tcm_info->hw_res, false);

		disable_irq(tcm_info->ts->irq);

		ret = tp_powercontrol_vddi(tcm_info->hw_res, false);

		if (ret) {
			return -1;
		}

		ret = tp_powercontrol_avdd(tcm_info->hw_res, false);

		if (ret) {
			return -1;
		}
	}

	return ret;
}

static fw_check_state syna_fw_check(void *chip_data,
				    struct resolution_info *resolution_info,
				    struct panel_info *panel_data)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	u16 config = 0;
	int retval = 0;
	int ver_len = 0;
	char dev_version[MAX_DEVICE_VERSION_LENGTH] = {0};

	TPD_INFO("fw id %d, custom config id 0x%s\n", panel_data->tp_fw,
		 tcm_info->app_info.customer_config_id);

	if (strlen(tcm_info->app_info.customer_config_id) == 0) {
		return FW_ABNORMAL;
	}

	sscanf(tcm_info->app_info.customer_config_id, "%x", &panel_data->tp_fw);

	if (panel_data->tp_fw == 0) {
		return FW_ABNORMAL;
	}


	if (panel_data->manufacture_info.version) {
		if (panel_data->vid_len == 0) {
			sprintf(panel_data->manufacture_info.version, "0x%s", tcm_info->app_info.customer_config_id);
		} else {
			ver_len = panel_data->vid_len;
			if (ver_len > MAX_DEVICE_VERSION_LENGTH - 4) {
				ver_len = MAX_DEVICE_VERSION_LENGTH - 4;
			}
			snprintf(dev_version, MAX_DEVICE_VERSION_LENGTH  - ver_len,
				 "%s", tcm_info->app_info.customer_config_id);
			strlcpy(&panel_data->manufacture_info.version[ver_len],
				dev_version, MAX_DEVICE_VERSION_LENGTH - ver_len);
		}
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_NOISE_LENGTH, &config);

	if (retval < 0) {
		TPD_INFO("Failed to get default noise length\n");
		return FW_ABNORMAL;
	}

	tcm_info->default_noise_length = config;

	return FW_NORMAL;
}

static int syna_tcm_helper(struct syna_tcm_data *tcm_info)
{
	if (tcm_info->id_info.mode != MODE_APPLICATION
	    && !mutex_is_locked(&tcm_info->reset_mutex)) {
		TPD_INFO("%s: use helper\n", __func__);
		queue_work(tcm_info->helper_workqueue, &tcm_info->helper_work);
	}

	return 0;
}

static void syna_tcm_helper_work(struct work_struct *work)
{
	int retval = 0;
	struct syna_tcm_data *tcm_info = container_of(work, struct syna_tcm_data,
					 helper_work);

	mutex_lock(&tcm_info->reset_mutex);
	retval = syna_tcm_run_application_firmware(tcm_info);

	if (retval < 0) {
		TPD_INFO("Failed to switch to app mode\n");
	}

	mutex_unlock(&tcm_info->reset_mutex);
}

static int syna_tcm_async_work(void *chip_data)
{
	int retval = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	if (!tcm_info || tcm_info->id_info.mode != MODE_APPLICATION) {
		return 0;
	}

	if (tcm_info->boot_flag) {
		tcm_info->boot_flag = false;
		return 0;
	}

	retval = syna_tcm_identify(tcm_info, false);
	if (retval < 0) {
		TPD_INFO("Failed to do identification\n");
		return retval;
	}
	tp_fw_auto_reset_handle(tcm_info->ts);
	/*syna_set_trigger_reason(tcm_info, IRQ_IGNORE);
	 syna_set_trigger_reason(tcm_info, IRQ_FW_AUTO_RESET);*/
	TPD_INFO("%s  exit\n", __func__);
	return 0;
}

static int syna_tcm_enable_report(struct syna_tcm_data *tcm_info,
				  enum report_type report_type, bool enable)
{
	int retval;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char out[2] = {0};
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0;
	unsigned int resp_length = 0;

	test_hcd->report_index = 0;
	test_hcd->report_type = report_type;

	out[0] = test_hcd->report_type;

	retval = syna_tcm_write_message(tcm_info,
					enable ? CMD_ENABLE_REPORT : CMD_DISABLE_REPORT,
					out,
					1,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write message %s\n",
			 enable ? STR(CMD_ENABLE_REPORT) : STR(CMD_DISABLE_REPORT));
	}

	return retval;
}

static void syna_tcm_enable_fingerprint(void *chip_data, uint32_t enable)
{
	int retval = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	TPD_INFO("%s: enable(%d)\n", __func__, enable);

	if (enable) {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_TOUCH_HOLD,
						     *tcm_info->in_suspend ? 0x01 : 0x02);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic touch and hold config\n");
			return;
		}

		/*retval = syna_tcm_enable_report(tcm_info, REPORT_TOUCH_HOLD,
						*tcm_info->in_suspend ? false : true);

		if (retval < 0) {
			TPD_INFO("Failed to set enable touch and hold report\n");
			return;
		}*/

	} else {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_TOUCH_HOLD, 0x00);

		if (retval < 0) {
			TPD_INFO("Failed to set dynamic touch and hold config\n");
			return;
		}

		/*retval = syna_tcm_enable_report(tcm_info, REPORT_TOUCH_HOLD, false);

		if (retval < 0) {
			TPD_INFO("Failed to set disable touch and hold report\n");
			return;
		}*/
	}

	return;
}

static void syna_tcm_fingerprint_info(void *chip_data,
				      struct fp_underscreen_info *fp_tpinfo)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touch_data *touch_data = &touch_hcd->touch_data;
	u8 *fp_buf = touch_data->extra_gesture_info;

	if (!fp_tpinfo) {
		return;
	}

	if (tcm_info->report.buffer.data_length < 8
	    && touch_data->lpwg_gesture == TOUCH_HOLD_DOWN) {
		TPD_INFO("%s: invalid fingerprint buf length\n", __func__);
		return;
	}

	if (touch_data->lpwg_gesture == TOUCH_HOLD_DOWN) {
		fp_tpinfo->touch_state = FINGERPRINT_DOWN_DETECT;
		fp_tpinfo->x = fp_buf[0] | fp_buf[1] << 8;
		fp_tpinfo->y = fp_buf[2] | fp_buf[3] << 8;
		fp_tpinfo->area_rate = fp_buf[4] | fp_buf[5] << 8;

	} else if (touch_data->lpwg_gesture == TOUCH_HOLD_UP) {
		fp_tpinfo->touch_state = FINGERPRINT_UP_DETECT;
	}

	return;
}

static void syna_tcm_fingerprint_info_auto(void *chip_data,
		struct fp_underscreen_info *fp_tpinfo,
		struct resolution_info *resolution_info)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touch_data *touch_data = &touch_hcd->touch_data;
	u8 *fp_buf = touch_data->extra_gesture_info;

	int max_x_inchip = 0;
	int max_y_inchip = 0;
	int max_x = 0;
	int max_y = 0;

	if (!fp_tpinfo) {
		return;
	}

	if (tcm_info->report.buffer.data_length < 8
	    && touch_data->lpwg_gesture == TOUCH_HOLD_DOWN) {
		TPD_INFO("%s: invalid fingerprint buf length\n", __func__);
		return;
	}

	if (touch_data->lpwg_gesture != TOUCH_HOLD_DOWN
	    && touch_data->lpwg_gesture != TOUCH_HOLD_UP) {
		return;
	}

	max_x_inchip = le2_to_uint(tcm_info->app_info.max_x) + 1;
	max_y_inchip = le2_to_uint(tcm_info->app_info.max_y) + 1;
	max_x = resolution_info->LCD_WIDTH;
	max_y = resolution_info->LCD_HEIGHT;

	if (touch_data->lpwg_gesture == TOUCH_HOLD_DOWN) {
		fp_tpinfo->touch_state = FINGERPRINT_DOWN_DETECT;
		fp_tpinfo->x = fp_buf[0] | fp_buf[1] << 8;
		fp_tpinfo->y = fp_buf[2] | fp_buf[3] << 8;
		fp_tpinfo->area_rate = fp_buf[4] | fp_buf[5] << 8;
	} else if (touch_data->lpwg_gesture == TOUCH_HOLD_UP) {
		fp_tpinfo->touch_state = FINGERPRINT_UP_DETECT;
		fp_tpinfo->x = fp_buf[0] | fp_buf[1] << 8;
		fp_tpinfo->y = fp_buf[2] | fp_buf[3] << 8;
		fp_tpinfo->area_rate = fp_buf[4] | fp_buf[5] << 8;
	}

	if (max_x_inchip != max_x) {
		fp_tpinfo->x = (fp_tpinfo->x * max_x) / max_x_inchip;
	}

	if (max_y_inchip != max_y) {
		fp_tpinfo->y = (fp_tpinfo->y * max_y) / max_y_inchip;
	}

	return;
}

static void syna_tcm_get_health_info(void *chip_data,
				     struct monitor_data *mon_data)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct health_info *health_info = (struct health_info *)
					  tcm_info->report.buffer.buf;
	int data_length = tcm_info->report.buffer.data_length;
	struct health_info *health_local = &tcm_info->health_info;
	int i = 0;

	if (data_length < 20) {
		TPD_INFO("%s: invalid health debug buf length\n", __func__);
		return;
	}



	if (health_info->baseline_err != 0
	    && health_local->baseline_err != health_info->baseline_err) {
		switch (health_info->baseline_err) {
		case BASE_NEGATIVE_FINGER:
			break;

		case BASE_MUTUAL_SELF_CAP:
			break;

		case BASE_ENERGY_RATIO:
			break;

		case BASE_RXABS_BASELINE:
			break;

		case BASE_TXABS_BASELINE:
			break;

		default:
			break;
		}
	}


	if (health_info->shield_mode != 0
	    && health_local->shield_mode != health_info->shield_mode) {
		switch (health_info->shield_mode) {
		case SHIELD_PALM:
			break;

		case SHIELD_GRIP:
			break;

		case SHIELD_METAL:
			break;

		case SHIELD_MOISTURE:
			break;

		case SHIELD_ESD:
			break;

		default:
			break;
		}
	}

	if (health_info->reset_reason != 0) {
		switch (health_info->reset_reason) {
		case RST_HARD:
			break;

		case RST_INST:
			break;

		case RST_PARITY:
			break;

		case RST_WD:
			break;

		case RST_OTHER:
			break;
		}
	}

	tp_memcpy(health_local, sizeof(struct health_info), health_info,
		  sizeof(struct health_info), sizeof(struct health_info));

	if (tp_debug != 0) {
		for (i = 0; i < data_length; i++) {
			TPD_INFO("[0x%x], ", tcm_info->report.buffer.buf[i]);
		}
	}
}

static int syna_tcm_erase_flash(struct syna_tcm_data *tcm_info,
				unsigned int page_start, unsigned int page_count)
{
	int ret = 0;
	unsigned char out_buf[4] = {0};
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0;
	unsigned int resp_length = 0;
	unsigned int cmd_length = 0;

	TPD_INFO("start page %d, page count %d\n", page_start, page_count);

	if (page_start > 0xff || page_count > 0xff) {
		cmd_length = 4;
		out_buf[0] = (unsigned char)(page_start & 0xff);
		out_buf[1] = (unsigned char)((page_start >> 8) & 0xff);
		out_buf[2] = (unsigned char)(page_count & 0xff);
		out_buf[3] = (unsigned char)((page_count >> 8) & 0xff);

	} else {
		cmd_length = 2;
		out_buf[0] = (unsigned char)page_start;
		out_buf[1] = (unsigned char)page_count;
	}

	ret = syna_tcm_write_message(tcm_info,  CMD_ERASE_FLASH, out_buf, cmd_length,
				     &resp_buf, &resp_buf_size, &resp_length, ERASE_FLASH_DELAY_MS);

	if (ret < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_ERASE_FLASH));
	}

	tp_kfree((void **)&resp_buf);
	return ret;
}

static int syna_tcm_write_flash(struct syna_tcm_data *tcm_info,
				struct reflash_hcd *reflash_hcd,
				unsigned int address, const unsigned char *data, unsigned int datalen)
{
	int retval;
	unsigned int w_len, xfer_len, remaining_len;
	unsigned int flash_addr, block_addr;
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0, resp_length = 0, offset = 0;
	struct syna_tcm_buffer out;

	memset(&out, 0, sizeof(out));
	INIT_BUFFER(out, false);

	w_len = tcm_info->wr_chunk_size - 5;
	w_len = w_len - (w_len % reflash_hcd->write_block_size);
	w_len = MIN(w_len, reflash_hcd->max_write_payload_size);

	remaining_len = datalen;

	while (remaining_len) {
		if (remaining_len > w_len) {
			xfer_len = w_len;

		} else {
			xfer_len = remaining_len;
		}

		retval = syna_tcm_alloc_mem(&out, xfer_len + 2);

		if (retval < 0) {
			TPD_INFO("Failed to alloc memory\n");
			break;
		}

		flash_addr = address + offset;
		block_addr = flash_addr / reflash_hcd->write_block_size;
		out.buf[0] = (unsigned char)block_addr;
		out.buf[1] = (unsigned char)(block_addr >> 8);

		retval = tp_memcpy(&out.buf[2],
				   xfer_len,
				   (void *)&data[offset],
				   datalen - offset,
				   xfer_len);

		if (retval < 0) {
			TPD_INFO("Failed to copy write data\n");
			break;
		}

		retval = syna_tcm_write_message(tcm_info, CMD_WRITE_FLASH,
						out.buf,
						xfer_len + 2,
						&resp_buf,
						&resp_buf_size,
						&resp_length,
						WRITE_FLASH_DELAY_MS);

		if (retval < 0) {
			TPD_INFO("Failed to write message %s, Addr 0x%08x, Len 0x%d\n",
				 STR(CMD_WRITE_FLASH), flash_addr, xfer_len);
			break;
		}

		offset += xfer_len;
		remaining_len -= xfer_len;
	}

	RELEASE_BUFFER(out);
	tp_kfree((void **)&resp_buf);
	return 0;
}

static fw_update_state syna_tcm_fw_update(void *chip_data,
		const struct firmware *fw, bool force)
{
	int ret = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct image_info image_info;
	unsigned int image_fw_id, device_fw_id;
	unsigned char *device_config_id = NULL;
	unsigned char *image_config_id = NULL;
	struct app_config_header *header = NULL;
	int temp = 0, page_start = 0, page_count = 0;
	unsigned int size = 0, flash_addr = 0, device_addr = 0, device_size = 0;
	const unsigned char *data;
	struct reflash_hcd reflash_hcd;

	memset(&image_info, 0, sizeof(struct image_info));
	if (tcm_info->fwupdate_bootloader) {
		if (fw) {
			if (tcm_info->g_fw_buf && fw->size < FW_BUF_SIZE) {
				tcm_info->g_fw_len = fw->size;
				memcpy(tcm_info->g_fw_buf, fw->data, fw->size);
				tcm_info->g_fw_sta = true;
			} else {
				TPD_INFO("fw->size:%d is less than %d\n", fw->size, FW_BUF_SIZE);
				return FW_UPDATE_FATAL;
			}
		}
		if (tcm_info->g_fw_sta) {
			ret = synaptics_parse_header_v2(&image_info, tcm_info->g_fw_buf);
			if (ret < 0) {
				TPD_INFO("Failed to parse fw image\n");
				return FW_UPDATE_FATAL;
			}
		} else {
			if (!fw) {
				TPD_INFO("fw is null\n");
				return FW_UPDATE_FATAL;
			} else {
				ret = synaptics_parse_header_v2(&image_info, fw->data);
				if (ret < 0) {
					TPD_INFO("Failed to parse fw image\n");
					return FW_UPDATE_FATAL;
				}
			}
		}
	} else {
		ret = synaptics_parse_header_v2(&image_info, fw->data);
		if (ret < 0) {
			TPD_INFO("Failed to parse fw image\n");
			return FW_UPDATE_FATAL;
		}
	}
	header = (struct app_config_header *)image_info.app_config.data;

	image_fw_id = le4_to_uint(header->build_id);
	device_fw_id = le4_to_uint(tcm_info->id_info.build_id);
	TPD_INFO("image build id %d, device build id %d\n", image_fw_id, device_fw_id);

	image_config_id = header->customer_config_id;
	device_config_id = tcm_info->app_info.customer_config_id;
	TPD_INFO("image config id 0x%s, device config id 0x%s\n", image_config_id,
		 device_config_id);

	if (!force) {
		if ((image_fw_id == device_fw_id)
		    && (strncmp(image_config_id, device_config_id, 16) == 0)) {
			TPD_INFO("same firmware/config id, no need to update\n");
			return FW_NO_NEED_UPDATE;
		}
	}

	ret = syna_tcm_identify(tcm_info, true);

	if (ret < 0) {
		return FW_UPDATE_ERROR;
	}

	if (tcm_info->id_info.mode == MODE_APPLICATION) {
		ret = syna_tcm_switch_mode(tcm_info, FW_MODE_BOOTLOADER);

		if (ret < 0) {
			TPD_INFO("Failed to switch to bootloader mode\n");
			return FW_UPDATE_ERROR;
		}
	}

	temp = tcm_info->boot_info.write_block_size_words;
	reflash_hcd.write_block_size = temp * 2;

	temp = le2_to_uint(tcm_info->boot_info.erase_page_size_words);
	reflash_hcd.page_size = temp * 2;

	temp = le2_to_uint(tcm_info->boot_info.max_write_payload_size);
	reflash_hcd.max_write_payload_size = temp;

	TPD_INFO("Write block size %d, page size %d, payload_size %d\n",
		 reflash_hcd.write_block_size,
		 reflash_hcd.page_size,
		 reflash_hcd.max_write_payload_size);

	if (reflash_hcd.write_block_size > (tcm_info->wr_chunk_size - 5)) {
		TPD_INFO("write block size is exceed\n");
		return FW_UPDATE_ERROR;
	}

	if (image_info.app_firmware.size == 0) {
		TPD_INFO("no application firmware in image\n\n");
		return FW_UPDATE_ERROR;
	}

	/* erase application firmware */
	page_start = image_info.app_firmware.flash_addr / reflash_hcd.page_size;
	page_count = ceil_div(image_info.app_firmware.size, reflash_hcd.page_size);
	ret = syna_tcm_erase_flash(tcm_info, page_start, page_count);

	if (ret < 0) {
		TPD_INFO("Failed to erase firmware\n");
		return FW_UPDATE_ERROR;
	}

	/* write application firmware */
	data = image_info.app_firmware.data;
	size = image_info.app_firmware.size;
	flash_addr = image_info.app_firmware.flash_addr;

	ret = syna_tcm_write_flash(tcm_info, &reflash_hcd, flash_addr, data, size);

	if (ret < 0) {
		TPD_INFO("Failed to write flash \n");
		return FW_UPDATE_ERROR;
	}

	/* update app config start */
	data = image_info.app_config.data;
	size = image_info.app_config.size;
	flash_addr = image_info.app_config.flash_addr;

	temp = le2_to_uint(tcm_info->app_info.app_config_start_write_block);
	device_addr = temp * reflash_hcd.write_block_size;
	device_size = le2_to_uint(tcm_info->app_info.app_config_size);

	TPD_INFO("Config Device addr/size 0x%x/%d, flash addr/size 0x%x/%d\n",
		 device_addr, device_size, flash_addr, size);

	page_start = image_info.app_config.flash_addr / reflash_hcd.page_size;
	page_count = ceil_div(image_info.app_config.size, reflash_hcd.page_size);

	ret = syna_tcm_erase_flash(tcm_info, page_start, page_count);

	if (ret < 0) {
		TPD_INFO("Failed to erase config\n");
		return FW_UPDATE_ERROR;
	}

	ret = syna_tcm_write_flash(tcm_info, &reflash_hcd, flash_addr, data, size);

	if (ret < 0) {
		TPD_INFO("Failed to write config \n");
		return FW_UPDATE_ERROR;
	}

	TPD_INFO("end of config update\n");
	/* update app config end */

	return FW_UPDATE_SUCCESS;
}

static void syna_tcm_fw_update_in_bl(void *chip_data)
{
	int ret = -1;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;


	if (!tcm_info) {
		TPD_INFO("%s: tcm_info is null\n", __func__);
		return;
	}

	if (!tcm_info->fwupdate_bootloader) {
		TPD_INFO("%s not support.\n", __func__);
		return;
	}

	if (*tcm_info->loading_fw) {
		TPD_INFO("%s not support when TP loading fw.\n", __func__);
		return;
	}

	if (tcm_info->probe_done && tcm_info->firmware_mode_count % FWUPDATE_BL_MAX == 0 && tcm_info->firmware_mode_count) {
		tcm_info->firmware_mode_count = 0;
		ret = syna_tcm_fw_update(tcm_info, NULL, 0);
		if (ret > 0) {
			TPD_INFO("g_fw_buf update failed!\n");
		}
	}
	return;
}

static int syna_get_gesture_info(void *chip_data, struct gesture_info *gesture)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touch_data *touch_data = &touch_hcd->touch_data;

	gesture->clockwise = 2;

	switch (touch_data->lpwg_gesture) {
	case DTAP_DETECT:
		gesture->gesture_type = DOU_TAP;
		break;

	case CIRCLE_DETECT:
		gesture->gesture_type = CIRCLE_GESTURE;

		if (touch_data->extra_gesture_info[2] == 0x10) {
			gesture->clockwise = 1;

		} else if (touch_data->extra_gesture_info[2] == 0x20) {
			gesture->clockwise = 0;
		}

		break;

	case SWIPE_DETECT:
		if (touch_data->extra_gesture_info[4] == 0x41) { /*x+*/
			gesture->gesture_type = LEFT2RIGHT_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x42) { /*x-*/
			gesture->gesture_type = RIGHT2LEFT_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x44) { /*y+*/
			gesture->gesture_type = UP2DOWN_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x48) { /*y-*/
			gesture->gesture_type = DOWN2UP_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x81) { /*2x-*/
			gesture->gesture_type = DOU_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x82) { /*2x+*/
			gesture->gesture_type = DOU_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x84) { /*2y+*/
			gesture->gesture_type = DOU_SWIP;

		} else if (touch_data->extra_gesture_info[4] == 0x88) { /*2y-*/
			gesture->gesture_type = DOU_SWIP;
		}

		break;

	case M_UNICODE:
		gesture->gesture_type = M_GESTRUE;
		break;

	case W_UNICODE:
		gesture->gesture_type = W_GESTURE;
		break;

	case VEE_DETECT:
		if (touch_data->extra_gesture_info[2] == 0x02) { /*up*/
			gesture->gesture_type = UP_VEE;

		} else if (touch_data->extra_gesture_info[2] == 0x01) { /*down*/
			gesture->gesture_type = DOWN_VEE;

		} else if (touch_data->extra_gesture_info[2] == 0x08) { /*left*/
			gesture->gesture_type = LEFT_VEE;

		} else if (touch_data->extra_gesture_info[2] == 0x04) { /*right*/
			gesture->gesture_type = RIGHT_VEE;
		}

		break;

	case TOUCH_HOLD_DOWN:
		gesture->gesture_type = FINGER_PRINTDOWN;
		break;

	case TOUCH_HOLD_UP:
		gesture->gesture_type = FRINGER_PRINTUP;
		break;

	case HEART_DETECT:
		gesture->gesture_type = HEART;

		if (touch_data->extra_gesture_info[2] == 0x10) {
			gesture->clockwise = 1;

		} else if (touch_data->extra_gesture_info[2] == 0x20) {
			gesture->clockwise = 0;
		}
		break;

	case STAP_DETECT:
		gesture->gesture_type = SINGLE_TAP;
		break;

	case S_UNICODE:
		gesture->gesture_type = S_GESTURE;
		break;

	case TRIANGLE_DETECT:
	default:
		TPD_DEBUG("not support\n");
		break;
	}

	if (gesture->gesture_type != UNKOWN_GESTURE) {
		gesture->Point_start.x = (touch_data->data_point[0] | (touch_data->data_point[1]
					  << 8));
		gesture->Point_start.y = (touch_data->data_point[2] | (touch_data->data_point[3]
					  << 8));
		gesture->Point_end.x    = (touch_data->data_point[4] |
					   (touch_data->data_point[5] << 8));
		gesture->Point_end.y    = (touch_data->data_point[6] |
					   (touch_data->data_point[7] << 8));
		gesture->Point_1st.x    = (touch_data->data_point[8] |
					   (touch_data->data_point[9] << 8));
		gesture->Point_1st.y    = (touch_data->data_point[10] |
					   (touch_data->data_point[11] << 8));
		gesture->Point_2nd.x    = (touch_data->data_point[12] |
					   (touch_data->data_point[13] << 8));
		gesture->Point_2nd.y    = (touch_data->data_point[14] |
					   (touch_data->data_point[15] << 8));
		gesture->Point_3rd.x    = (touch_data->data_point[16] |
					   (touch_data->data_point[17] << 8));
		gesture->Point_3rd.y    = (touch_data->data_point[18] |
					   (touch_data->data_point[19] << 8));
		gesture->Point_4th.x    = (touch_data->data_point[20] |
					   (touch_data->data_point[21] << 8));
		gesture->Point_4th.y    = (touch_data->data_point[22] |
					   (touch_data->data_point[23] << 8));
	}

	if (gesture->gesture_type == SINGLE_TAP) {
		gesture->Point_start.x = (touch_data->extra_gesture_info[0] | (touch_data->extra_gesture_info[1] << 8));
		gesture->Point_start.y = (touch_data->extra_gesture_info[2] | (touch_data->extra_gesture_info[3] << 8));
	}

	TPD_INFO("lpwg:0x%x, type:%d, clockwise: %d, points: (%d, %d)(%d, %d)(%d, %d)(%d, %d)(%d, %d)(%d, %d)\n",
		 touch_data->lpwg_gesture, gesture->gesture_type, gesture->clockwise, \
		 gesture->Point_start.x, gesture->Point_start.y, \
		 gesture->Point_end.x, gesture->Point_end.y, \
		 gesture->Point_1st.x, gesture->Point_1st.y, \
		 gesture->Point_2nd.x, gesture->Point_2nd.y, \
		 gesture->Point_3rd.x, gesture->Point_3rd.y, \
		 gesture->Point_4th.x, gesture->Point_4th.y);

	return 0;
}

static int syna_get_gesture_info_auto(void *chip_data,
				      struct gesture_info *gesture,
				      struct resolution_info *resolution_info)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	int max_x_inchip = 0;
	int max_y_inchip = 0;
	int max_x = 0;
	int max_y = 0;

	max_x_inchip = le2_to_uint(tcm_info->app_info.max_x) + 1;
	max_y_inchip = le2_to_uint(tcm_info->app_info.max_y) + 1;
	max_x = resolution_info->max_x;
	max_y = resolution_info->max_y;


	syna_get_gesture_info(chip_data, gesture);

	if (max_x_inchip == max_x && max_y_inchip == max_y) {
		return 0;
	}

	if (gesture->gesture_type == UNKOWN_GESTURE) {
		return 0;
	}

	if (max_x_inchip != max_x) {
		gesture->Point_start.x = (gesture->Point_start.x * max_x) / max_x_inchip;
		gesture->Point_end.x    = (gesture->Point_end.x * max_x) / max_x_inchip;
		gesture->Point_1st.x    = (gesture->Point_1st.x * max_x) / max_x_inchip;
		gesture->Point_2nd.x    = (gesture->Point_2nd.x * max_x) / max_x_inchip;
		gesture->Point_3rd.x    = (gesture->Point_3rd.x * max_x) / max_x_inchip;
		gesture->Point_4th.x    = (gesture->Point_4th.x * max_x) / max_x_inchip;
	}

	if (max_y_inchip != max_y) {
		gesture->Point_start.y = (gesture->Point_start.y * max_y) / max_y_inchip;
		gesture->Point_end.y    = (gesture->Point_end.y * max_y) / max_y_inchip;
		gesture->Point_1st.y    = (gesture->Point_1st.y * max_y) / max_y_inchip;
		gesture->Point_2nd.y    = (gesture->Point_2nd.y * max_y) / max_y_inchip;
		gesture->Point_3rd.y    = (gesture->Point_3rd.y * max_y) / max_y_inchip;
		gesture->Point_4th.y    = (gesture->Point_4th.y * max_y) / max_y_inchip;
	}

	TPD_INFO("changed points: (%d, %d)(%d, %d)(%d, %d)(%d, %d)(%d, %d)(%d, %d)\n",
		 gesture->Point_start.x, gesture->Point_start.y, \
		 gesture->Point_end.x, gesture->Point_end.y, \
		 gesture->Point_1st.x, gesture->Point_1st.y, \
		 gesture->Point_2nd.x, gesture->Point_2nd.y, \
		 gesture->Point_3rd.x, gesture->Point_3rd.y, \
		 gesture->Point_4th.x, gesture->Point_4th.y);

	return 0;
}


static void store_to_file(void *fp, size_t max_count,
			  size_t *pos, char *format, ...)
{
	va_list args;
	char buf[64] = {0};

	va_start(args, format);
	vsnprintf(buf, 64, format, args);
	va_end(args);

	if (!IS_ERR_OR_NULL(fp)) {
		tp_test_write(fp, max_count, buf, strlen(buf), pos);
	}
}

static int testing_run_prod_test_item(struct syna_tcm_data *tcm_info,
				      enum test_item_bit test_code)
{
	int retval = 0;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;

	if (tcm_info->id_info.mode != MODE_APPLICATION
	    || tcm_info->app_status != APP_STATUS_OK) {
		TPD_INFO("Application firmware not running\n");
		return -ENODEV;
	}

	LOCK_BUFFER(test_hcd->test_out);

	retval = syna_tcm_alloc_mem(&test_hcd->test_out, 1);

	if (retval < 0) {
		TPD_INFO("Failed to allocate memory for test_hcd->test_out.buf\n");
		UNLOCK_BUFFER(test_hcd->test_out);
		return retval;
	}

	test_hcd->test_out.buf[0] = test_code;

	LOCK_BUFFER(test_hcd->test_resp);
	retval = syna_tcm_write_message(tcm_info,
					CMD_PRODUCTION_TEST,
					test_hcd->test_out.buf,
					1,
					&test_hcd->test_resp.buf,
					&test_hcd->test_resp.buf_size,
					&test_hcd->test_resp.data_length,
					RESPONSE_TIMEOUT_MS_LONG);

	if (retval < 0) {
		TPD_INFO("Failed to write command %s\n", STR(CMD_PRODUCTION_TEST));
		UNLOCK_BUFFER(test_hcd->test_resp);
		UNLOCK_BUFFER(test_hcd->test_out);
		return retval;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	UNLOCK_BUFFER(test_hcd->test_out);

	return 0;
}

static int syna_trx_short_test(struct seq_file *s, void *chip_data,
			       struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	uint8_t u_data8 = 0;
	int i = 0, j = 0, ret = 0;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned int checked_bits = 0, total_bits = 0;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	total_bits = syna_testdata->tx_num + syna_testdata->rx_num;

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_TRX_SHORT);

	if (ret < 0) {
		TPD_INFO("run trx short test failed.\n");

		if (!error_count) {
			seq_printf(s, "run trx short test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "trx_short:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		u_data8 = buf[i];
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "0x%02x, ", u_data8);

		for (j = 0; j < 8; j++) {
			if (1 == (u_data8 & (1 << j))) {
				TPD_INFO("trx short test failed at %d bits.\n", checked_bits + 1);

				if (!error_count) {
					seq_printf(s, "trx short test failed at %d bits.\n", checked_bits + 1);
				}

				error_count++;
			}

			checked_bits++;

			if (checked_bits >= total_bits) {
				goto full_out;
			}
		}

		i += 1;
	}

full_out:
	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_trx_open_test(struct seq_file *s, void *chip_data,
			      struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	uint8_t u_data8 = 0;
	int i = 0, j = 0, ret = 0;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned int checked_bits = 0, total_bits = 0;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	total_bits = syna_testdata->tx_num + syna_testdata->rx_num;

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_TRX_OPEN);

	if (ret < 0) {
		TPD_INFO("run trx open test failed.\n");

		if (!error_count) {
			seq_printf(s, "run trx open test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "tx_tx_open:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		u_data8 = buf[i];
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "0x%02x, ", u_data8);

		for (j = 0; j < 8; j++) {
			if (0 == (u_data8 & (1 << j))) {
				TPD_INFO("trx open test failed at %d bits.\n", checked_bits + 1);

				if (!error_count) {
					seq_printf(s, "trx open test failed at %d bits.\n", checked_bits + 1);
				}

				error_count++;
			}

			checked_bits++;

			if (checked_bits >= total_bits) {
				goto full_out;
			}
		}

		i += 1;
	}

full_out:
	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_trx_gndshort_test(struct seq_file *s, void *chip_data,
				  struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	uint8_t u_data8 = 0;
	int i = 0, j = 0, ret = 0;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned int checked_bits = 0, total_bits = 0;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	total_bits = syna_testdata->tx_num + syna_testdata->rx_num;

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_TRXGND_SHORT);

	if (ret < 0) {
		TPD_INFO("run trx gndshort test failed.\n");

		if (!error_count) {
			seq_printf(s, "run trx gndshort test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "tx_tx_gndshort:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		u_data8 = buf[i];
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "0x%02x, ", u_data8);

		for (j = 0; j < 8; j++) {
			if (0 == (u_data8 & (1 << j))) {
				TPD_INFO("trx gndshort test failed at %d bits.\n", checked_bits + 1);

				if (!error_count) {
					seq_printf(s, "trx gndshort test failed at %d bits.\n", checked_bits + 1);
				}

				error_count++;
			}

			checked_bits++;

			if (checked_bits >= total_bits) {
				goto full_out;
			}
		}

		i += 1;
	}

full_out:
	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp,  syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_full_rawcap_test(struct seq_file *s, void *chip_data,
				 struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	uint16_t u_data16 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_mutual_p = NULL, *p_mutual_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data + p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_TX_RX_DATA) {
		p_mutual_p = (int32_t *)(syna_testdata->fw->data + item_header->top_limit_offset);
		p_mutual_n = (int32_t *)(syna_testdata->fw->data + item_header->floor_limit_offset);

	} else {
		TPD_INFO("full rawcap test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "full rawcap test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_FULLRAW_CAP);

	if (ret < 0) {
		TPD_INFO("run full rawcap test failed.\n");

		if (!error_count) {
			seq_printf(s, "run full rawcap test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp,  syna_testdata->length,
		      syna_testdata->pos, "full_rawcap:");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		u_data16 = (buf[i] | (buf[i + 1] << 8));

		if (0 == index % (syna_testdata->rx_num))
			store_to_file(syna_testdata->fp, syna_testdata->length,
				      syna_testdata->pos, "\n");

		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%04d, ", u_data16);

		if ((u_data16 < p_mutual_n[index]) || (u_data16 > p_mutual_p[index])) {
			TPD_INFO("full rawcap test failed at node[%d]=%d [%d %d].\n", index, u_data16,
				 p_mutual_n[index], p_mutual_p[index]);

			if (!error_count) {
				seq_printf(s, "full rawcap test failed at node[%d]=%d [%d %d].\n", index,
					   u_data16, p_mutual_n[index], p_mutual_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_delta_noise_test(struct seq_file *s, void *chip_data,
				 struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	int16_t data16 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_mutual_p = NULL, *p_mutual_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data +
			p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_TX_RX_DATA) {
		p_mutual_p = (int32_t *)(syna_testdata->fw->data + item_header->top_limit_offset);
		p_mutual_n = (int32_t *)(syna_testdata->fw->data + item_header->floor_limit_offset);

	} else {
		TPD_INFO("delta noise test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "delta noise test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_DELTA_NOISE);

	if (ret < 0) {
		TPD_INFO("run delta noise rawcap test failed.\n");

		if (!error_count) {
			seq_printf(s, "run delta noise test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "delta_noise:");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		data16 = (buf[i] | (buf[i + 1] << 8));

		if (0 == index % (syna_testdata->rx_num))
			store_to_file(syna_testdata->fp, syna_testdata->length,
				      syna_testdata->pos, "\n");

		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%04d, ", data16);

		if ((data16 < p_mutual_n[index]) || (data16 > p_mutual_p[index])) {
			TPD_INFO("delta noise test failed at node[%d]=%d [%d %d].\n", index, data16,
				 p_mutual_n[index], p_mutual_p[index]);

			if (!error_count) {
				seq_printf(s, "delta noise test failed at node[%d]=%d [%d %d].\n", index,
					   data16, p_mutual_n[index], p_mutual_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_hybrid_rawcap_test(struct seq_file *s, void *chip_data,
				   struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	int32_t data32 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 4;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_hybridcap_p = NULL, *p_hybridcap_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data + p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_SLEF_TX_RX_DATA) {
		p_hybridcap_p = (int32_t *)(syna_testdata->fw->data + item_header->top_limit_offset);
		p_hybridcap_n = (int32_t *)(syna_testdata->fw->data + item_header->floor_limit_offset);
	} else {
		TPD_INFO("hybrid_rawcap test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "hybrid_rawcap test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_HYBRIDRAW_CAP);

	if (ret < 0) {
		TPD_INFO("run hybrid rawcap test failed.\n");

		if (!error_count) {
			seq_printf(s, "run hybrid rawcap test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "hybrid_rawcap:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		data32 = (buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) | (buf[i + 3] << 24));
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%08d, ", data32);

		if ((data32 < p_hybridcap_n[index]) || (data32 > p_hybridcap_p[index])) {
			TPD_INFO("hybrid rawcap test failed at node[%d]=%d [%d %d].\n", index, data32,
				 p_hybridcap_n[index], p_hybridcap_p[index]);

			if (!error_count) {
				seq_printf(s, "hybrid rawcap test failed at node[%d]=%d [%d %d].\n", index,
					   data32, p_hybridcap_n[index], p_hybridcap_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_rawcap_test(struct seq_file *s, void *chip_data,
			    struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	int16_t data16 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_mutual_p = NULL, *p_mutual_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data + p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_TX_RX_DATA) {
		p_mutual_p = (int32_t *)(syna_testdata->fw->data + item_header->top_limit_offset);
		p_mutual_n = (int32_t *)(syna_testdata->fw->data + item_header->floor_limit_offset);

	} else {
		TPD_INFO("raw cap test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "raw cap test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_RAW_CAP);

	if (ret < 0) {
		TPD_INFO("run raw cap test failed.\n");

		if (!error_count) {
			seq_printf(s, "run raw cap test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "raw_cap:");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		data16 = (buf[i] | (buf[i + 1] << 8));

		if (0 == index % (syna_testdata->rx_num))
			store_to_file(syna_testdata->fp, syna_testdata->length,
				      syna_testdata->pos, "\n");

		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%04d, ", data16);

		if ((data16 < p_mutual_n[index]) || (data16 > p_mutual_p[index])) {
			TPD_INFO("rawcap test failed at node[%d]=%d [%d %d].\n", index, data16,
				 p_mutual_n[index], p_mutual_p[index]);

			if (!error_count) {
				seq_printf(s, "rawcap test failed at node[%d]=%d [%d %d].\n", index, data16,
					   p_mutual_n[index], p_mutual_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_trex_shortcustom_test(struct seq_file *s, void *chip_data,
				      struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	uint16_t u_data16 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_tx_p = NULL, *p_tx_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data + p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_SLEF_TX_RX_DATA) {
		p_tx_p = (int32_t *)(syna_testdata->fw->data + item_header->top_limit_offset);
		p_tx_n = (int32_t *)(syna_testdata->fw->data + item_header->floor_limit_offset);

	} else {
		TPD_INFO("trex short custom test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "trex short custom test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_TREXSHORT_CUSTOM);

	if (ret < 0) {
		TPD_INFO("run trex short custom test failed.\n");

		if (!error_count) {
			seq_printf(s, "run trex short custom test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "trex_shorcustom:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		u_data16 = (buf[i] | (buf[i + 1] << 8));
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%04d, ", u_data16);

		if ((u_data16 < p_tx_n[index]) || (u_data16 > p_tx_p[index])) {
			TPD_INFO("trex_shorcustom test failed at node[%d]=%d [%d %d].\n", index,
				 u_data16, p_tx_n[index], p_tx_p[index]);

			if (!error_count) {
				seq_printf(s, "trex_shorcustom test failed at node[%d]=%d [%d %d].\n", index,
					   u_data16, p_tx_n[index], p_tx_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_hybrid_diffcbc_test(struct seq_file *s, void *chip_data,
				    struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	uint16_t u_data16 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_selfdata_p = NULL, *p_selfdata_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data +
			p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_SLEF_TX_RX_DATA) {
		p_selfdata_p = (int32_t *)(syna_testdata->fw->data +
					   item_header->top_limit_offset);
		p_selfdata_n = (int32_t *)(syna_testdata->fw->data +
					   item_header->floor_limit_offset);

	} else {
		TPD_INFO("hybrid diffcbc test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "hybrid diffcbc test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_HYBRIDABS_DIFF_CBC);

	if (ret < 0) {
		TPD_INFO("run hybrid diffcbc test failed.\n");

		if (!error_count) {
			seq_printf(s, "run hybrid diffcbc test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "hybrid_diffwithcbc:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		u_data16 = (buf[i] | (buf[i + 1] << 8));
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%04d, ", u_data16);

		if ((u_data16 < p_selfdata_n[index]) || (u_data16 > p_selfdata_p[index])) {
			TPD_INFO("hybrid diffcbc test failed at node[%d]=%d [%d %d].\n", index,
				 u_data16, p_selfdata_n[index], p_selfdata_p[index]);

			if (!error_count) {
				seq_printf(s, "hybrid diffcbc test failed at node[%d]=%d [%d %d].\n", index,
					   u_data16, p_selfdata_n[index], p_selfdata_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_hybrid_absnoise_test(struct seq_file *s, void *chip_data,
				     struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	int16_t data16 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_selfdata_p = NULL, *p_selfdata_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data +
			p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_SLEF_TX_RX_DATA) {
		p_selfdata_p = (int32_t *)(syna_testdata->fw->data +
					   item_header->top_limit_offset);
		p_selfdata_n = (int32_t *)(syna_testdata->fw->data +
					   item_header->floor_limit_offset);

	} else {
		TPD_INFO("hybrid abs noise test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "hybrid abs noise test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);

	syna_tcm_reset(tcm_info); /* before the test, need to reset the IC*/

	ret = testing_run_prod_test_item(tcm_info, TYPE_HYBRIDABS_NOSIE);

	if (ret < 0) {
		TPD_INFO("run hybrid abs noise test failed.\n");

		if (!error_count) {
			seq_printf(s, "run hybrid abs noise test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "hybrid_absnoise:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		data16 = (buf[i] | (buf[i + 1] << 8));
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%04d, ", data16);

		if ((data16 < p_selfdata_n[index]) || (data16 > p_selfdata_p[index])) {
			TPD_INFO("hybrid abs noise test failed at node[%d]=%d [%d %d].\n", index,
				 data16, p_selfdata_n[index], p_selfdata_p[index]);

			if (!error_count) {
				seq_printf(s, "hybrid abs noise test failed at node[%d]=%d [%d %d].\n", index,
					   data16, p_selfdata_n[index], p_selfdata_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static int syna_hybrid_rawcap_test_ad(struct seq_file *s, void *chip_data,
				   struct auto_testdata *syna_testdata, struct test_item_info *p_test_item_info)
{
	int32_t data32 = 0;
	int i = 0, ret = 0, index = 0, byte_cnt = 2;
	int error_count = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct auto_test_item_header *item_header = NULL;
	int32_t *p_hybridcap_p = NULL, *p_hybridcap_n = NULL;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char *buf = NULL;

	item_header = (struct auto_test_item_header *)(syna_testdata->fw->data + p_test_item_info->item_offset);
	if (item_header->item_limit_type == LIMIT_TYPE_SLEF_TX_RX_DATA) {
		p_hybridcap_p = (int32_t *)(syna_testdata->fw->data + item_header->top_limit_offset);
		p_hybridcap_n = (int32_t *)(syna_testdata->fw->data + item_header->floor_limit_offset);
	} else {
		TPD_INFO("hybrid_rawcap test limit type(%2x) is wrong.\n",
			 item_header->item_limit_type);

		if (!error_count) {
			seq_printf(s, "hybrid_rawcap test limit type(%2x) is wrong.\n",
				   item_header->item_limit_type);
		}

		error_count++;
		return error_count;
	}

	TPD_INFO("%s start.\n", __func__);
	ret = testing_run_prod_test_item(tcm_info, TYPE_HYBRIDRAW_CAP_WITH_AD + 24);

	if (ret < 0) {
		TPD_INFO("run hybrid rawcap test failed.\n");

		if (!error_count) {
			seq_printf(s, "run hybrid rawcap test failed.\n");
		}

		error_count++;
		return error_count;
	}

	LOCK_BUFFER(test_hcd->test_resp);
	buf = test_hcd->test_resp.buf;
	TPD_INFO("%s read data size:%d\n", __func__, test_hcd->test_resp.data_length);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "hybrid_rawcap:\n");

	for (i = 0; i < test_hcd->test_resp.data_length;) {
		index = i / byte_cnt;
		data32 = (buf[i] | (buf[i + 1] << 8));
		store_to_file(syna_testdata->fp, syna_testdata->length,
			      syna_testdata->pos, "%08d, ", data32);

		if ((data32 < p_hybridcap_n[index]) || (data32 > p_hybridcap_p[index])) {
			TPD_INFO("hybrid rawcap test failed at node[%d]=%d [%d %d].\n", index, data32,
				 p_hybridcap_n[index], p_hybridcap_p[index]);

			if (!error_count) {
				seq_printf(s, "hybrid rawcap test failed at node[%d]=%d [%d %d].\n", index,
					   data32, p_hybridcap_n[index], p_hybridcap_p[index]);
			}

			error_count++;
		}

		i += byte_cnt;
	}

	UNLOCK_BUFFER(test_hcd->test_resp);
	store_to_file(syna_testdata->fp, syna_testdata->length,
		      syna_testdata->pos, "\n");

	return error_count;
}

static struct syna_auto_test_operations syna_tcm_test_ops = {
	.test1       =  syna_trx_short_test,
	.test2       =  syna_trx_open_test,
	.test3       =  syna_trx_gndshort_test,
	.test4       =  syna_full_rawcap_test,
	.test5       =  syna_delta_noise_test,
	.test6       =  syna_hybrid_rawcap_test,
	.test7       =  syna_rawcap_test,
	.test8       =  syna_trex_shortcustom_test,
	.test9       =  syna_hybrid_diffcbc_test,
	.test10      =  syna_hybrid_absnoise_test,
	.test11       =  syna_hybrid_rawcap_test_ad,
	/*.syna_auto_test_enable_irq    =  synaptics_test_enable_interrupt,*/
	/*.syna_auto_test_preoperation  =  synaptics_auto_test_preoperation,*/
	/*.syna_auto_test_endoperation  =  synaptics_auto_test_endoperation,*/
};

static struct engineer_test_operations syna_tcm_engineer_test_ops = {
	.auto_test                  = synaptics_auto_test,
};

static int syna_tcm_collect_reports(struct syna_tcm_data *tcm_info,
				    enum report_type report_type, unsigned int num_of_reports)
{
	int retval;
	bool completed = false;
	unsigned int timeout;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	unsigned char out[2] = {0};
	unsigned char *resp_buf = NULL;
	unsigned int resp_buf_size = 0;
	unsigned int resp_length = 0;

	test_hcd->report_index = 0;
	test_hcd->report_type = report_type;
	test_hcd->num_of_reports = num_of_reports;

	reinit_completion(&tcm_info->report_complete);

	out[0] = test_hcd->report_type;

	retval = syna_tcm_write_message(tcm_info,
					CMD_ENABLE_REPORT,
					out,
					1,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write message %s\n", STR(CMD_ENABLE_REPORT));
		completed = false;
		goto exit;
	}

	timeout = REPORT_TIMEOUT_MS * num_of_reports;

	retval = wait_for_completion_timeout(&tcm_info->report_complete,
					     msecs_to_jiffies(timeout));

	if (retval == 0) {
		TPD_INFO("Timed out waiting for report collection\n");

	} else {
		completed = true;
	}

	out[0] = test_hcd->report_type;

	retval = syna_tcm_write_message(tcm_info,
					CMD_DISABLE_REPORT,
					out,
					1,
					&resp_buf,
					&resp_buf_size,
					&resp_length,
					0);

	if (retval < 0) {
		TPD_INFO("Failed to write message %s\n", STR(CMD_DISABLE_REPORT));
	}

	if (!completed) {
		retval = -EIO;
	}

exit:

	return retval;
}

static void syna_tcm_test_report(struct syna_tcm_data *tcm_info)
{
	int retval;
	unsigned int offset, report_size;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;

	if (tcm_info->report.id != test_hcd->report_type) {
		TPD_INFO("Not request report type\n");
		return;
	}

	report_size = tcm_info->report.buffer.data_length;
	LOCK_BUFFER(test_hcd->report);

	if (test_hcd->report_index == 0) {
		retval = syna_tcm_alloc_mem(&test_hcd->report,
					    report_size * test_hcd->num_of_reports);

		if (retval < 0) {
			TPD_INFO("Failed to allocate memory\n");
			UNLOCK_BUFFER(test_hcd->report);
			return;
		}
	}

	if (test_hcd->report_index < test_hcd->num_of_reports) {
		offset = report_size * test_hcd->report_index;
		retval = tp_memcpy(test_hcd->report.buf + offset,
				   test_hcd->report.buf_size - offset,
				   tcm_info->report.buffer.buf,
				   tcm_info->report.buffer.buf_size,
				   tcm_info->report.buffer.data_length);

		if (retval < 0) {
			TPD_INFO("Failed to copy report data\n");

			UNLOCK_BUFFER(test_hcd->report);
			return;
		}

		test_hcd->report_index++;
		test_hcd->report.data_length += report_size;
	}

	UNLOCK_BUFFER(test_hcd->report);

	if (test_hcd->report_index == test_hcd->num_of_reports) {
		complete(&tcm_info->report_complete);
	}

	return;
}

static void syna_tcm_format_print(struct seq_file *s,
				  struct syna_tcm_data *tcm_info, char *buffer)
{
	unsigned int row, col;
	unsigned int rows, cols;
	unsigned int cnt = 0;
	short *pdata_16;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;

	rows = le2_to_uint(tcm_info->app_info.num_of_image_rows);
	cols = le2_to_uint(tcm_info->app_info.num_of_image_cols);

	TPD_INFO("report size:%d\n", test_hcd->report.data_length);

	if (buffer == NULL) {
		pdata_16 = (short *)&test_hcd->report.buf[0];

	} else {
		pdata_16 = (short *)buffer;
	}

	for (row = 0; row < rows; row++) {
		seq_printf(s, "[%02d] ", row);

		for (col = 0; col < cols; col++) {
			seq_printf(s, "%5d ", *pdata_16);
			pdata_16++;
		}

		seq_printf(s, "\n");
	}

	if (test_hcd->report.data_length == rows * cols * 2 + (rows + cols) * 2) {
		for (cnt = 0; cnt < rows + cols; cnt++) {
			seq_printf(s, "%5d ", *pdata_16);
			pdata_16++;
		}
	}

	seq_printf(s, "\n");

	return;
}

static void syna_tcm_format_print_snr(struct seq_file *s, struct syna_tcm_data *tcm_info, int index)
{
	unsigned int rows, cols;
	unsigned int i = 0;
	short *pdata_16;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;
	struct touch_hcd *touch_hcd = tcm_info->touch_hcd;
	struct touchpanel_snr *snr = tcm_info->snr;
	int rx_num = tcm_info->hw_res->rx_num;

	rows = le2_to_uint(tcm_info->app_info.num_of_image_rows);
	cols = le2_to_uint(tcm_info->app_info.num_of_image_cols);

	pdata_16 = (short *)&test_hcd->report.buf[0];

	for (i = 0; i < touch_hcd->max_objects; i++) {
		if (snr[i].point_status) {
			if (index) {
				snr[i].max = *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y) > snr[i].max ?
						*(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y) : snr[i].max;
				snr[i].min = *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y) < snr[i].min ?
						*(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y) : snr[i].min;
			} else {
				snr[i].max = *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y);
				snr[i].min = *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y);
			}
			snr[i].sum += *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y);

			TPD_DETAIL("snr-cover [%d %d] %d.. %d += %d\n", snr[i].channel_x, snr[i].channel_y, *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y),
					snr[i].sum, *(pdata_16 + snr[i].channel_x * rx_num + snr[i].channel_y));
		}
	}
	return;
}

static void syna_tcm_format_unsigned_print(struct seq_file *s,
		struct syna_tcm_data *tcm_info, char *buffer)
{
	unsigned int row, col;
	unsigned int rows, cols;
	unsigned int cnt = 0;
	unsigned short *pdata_16;
	struct syna_tcm_test *test_hcd = tcm_info->test_hcd;

	rows = le2_to_uint(tcm_info->app_info.num_of_image_rows);
	cols = le2_to_uint(tcm_info->app_info.num_of_image_cols);

	TPD_INFO("report size:%d\n", test_hcd->report.data_length);

	if (buffer == NULL) {
		pdata_16 = (unsigned short *)&test_hcd->report.buf[0];

	} else {
		pdata_16 = (unsigned short *)buffer;
	}

	for (row = 0; row < rows; row++) {
		seq_printf(s, "[%02d] ", row);

		for (col = 0; col < cols; col++) {
			seq_printf(s, "%5d ", *pdata_16);
			pdata_16++;
		}

		seq_printf(s, "\n");
	}

	if (test_hcd->report.data_length == rows * cols * 2 + (rows + cols) * 2) {
		for (cnt = 0; cnt < rows + cols; cnt++) {
			seq_printf(s, "%5d ", *pdata_16);
			pdata_16++;
		}
	}

	seq_printf(s, "\n");

	return;
}
static void syna_main_register(struct seq_file *s, void *chip_data)
{
	int retval = 0;
	unsigned short config = 0;
	unsigned int temp = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	retval = syna_tcm_get_dynamic_config(tcm_info,
					     DC_IN_WAKEUP_GESTURE_MODE,
					     &config);

	if (retval < 0) {
		TPD_INFO("gesture mode : ERROR\n");
		seq_printf(s, "gesture mode : ERROR\n");

	} else {
		TPD_INFO("gesture mode : %d\n", config);
		seq_printf(s, "gesture mode : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_ERROR_PRIORITY,
					     &config);

	if (retval < 0) {
		TPD_INFO("error priority(1:finger,0:error): ERROR\n");
		seq_printf(s, "error priority(1:finger,0:error): ERROR\n");

	} else {
		TPD_INFO("error priority(1:finger,0:error): 0x%0X\n", config);
		seq_printf(s, "error priority(1:finger,0:error): 0x%0X\n",
			   config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_NOISE_LENGTH,
					     &config);

	if (retval < 0) {
		TPD_INFO("noise length : ERROR\n");
		seq_printf(s, "noise length : ERROR\n");

	} else {
		TPD_INFO("noise length : %d\n", config);
		seq_printf(s, "noise length : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_SET_REPORT_FRE,
					     &config);

	if (retval < 0) {
		TPD_INFO("report rate(1:120HZ,2:240HZ,3:180HZ): ERROR\n");
		seq_printf(s, "report rate(1:120HZ,2:240HZ,3:180HZ): ERROR\n");

	} else {
		TPD_INFO("report rate(1:120HZ,2:240HZ,3:180HZ): %d\n", config);
		seq_printf(s, "report rate(1:120HZ,2:240HZ,3:180HZ): %d\n",
			   config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_CHARGER_CONNECTED,
					     &config);

	if (retval < 0) {
		TPD_INFO("charger mode : ERROR\n");
		seq_printf(s, "charger mode : ERROR\n");

	} else {
		TPD_INFO("charger mode : %d\n", config);
		seq_printf(s, "charger mode : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_TOUCH_HOLD, &config);

	if (retval < 0) {
		TPD_INFO("fingerprint mode : ERROR\n");
		seq_printf(s, "fingerprint mode : ERROR\n");

	} else {
		TPD_INFO("fingerprint mode : %d\n", config);
		seq_printf(s, "fingerprint mode : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_ENABLED,
					     &config);

	if (retval < 0) {
		TPD_INFO("grip enable : ERROR\n");
		seq_printf(s, "grip enable : ERROR\n");

	} else {
		TPD_INFO("grip enable : 0x%0X\n", config);
		seq_printf(s, "grip enable : 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info,
					     DC_GRIP_ROATE_TO_HORIZONTAL_LEVEL,
					     &config);

	if (retval < 0) {
		TPD_INFO("grip direction(1:ver 0:hor): ERROR\n");
		seq_printf(s, "grip direction(1:ver 0:hor): ERROR\n");

	} else {
		TPD_INFO("grip direction(1:ver 0:hor): 0x%0X\n", config);
		seq_printf(s, "grip direction(0:ver 1:hor): 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_DARK_ZONE_ENABLE,
					     &config);

	if (retval < 0) {
		TPD_INFO("dark zone enable : ERROR\n");
		seq_printf(s, "dark zone enable : ERROR\n");

	} else {
		TPD_INFO("dark zone enable : 0x%0X\n", config);
		seq_printf(s, "dark zone enable : 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_X,
					     &config);

	if (retval < 0) {
		TPD_INFO("dark zone x : ERROR\n");
		seq_printf(s, "dark zone x : ERROR\n");

	} else {
		TPD_INFO("dark zone x : 0x%0X\n", config);
		seq_printf(s, "dark zone x : 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_DARK_ZONE_Y,
					     &config);

	if (retval < 0) {
		TPD_INFO("dark zone y : ERROR\n");
		seq_printf(s, "dark zone y : ERROR\n");

	} else {
		TPD_INFO("dark zone y : 0x%0X\n", config);
		seq_printf(s, "dark zone y : 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_SEL,
					     &config);

	if (retval < 0) {
		TPD_INFO("abs dark sel : ERROR\n");
		seq_printf(s, "abs dark sel : ERROR\n");

	} else {
		TPD_INFO("abs dark sel : 0x%0X\n", config);
		seq_printf(s, "abs dark sel : 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_X,
					     &config);

	if (retval < 0) {
		TPD_INFO("abs dark zone x : ERROR\n");
		seq_printf(s, "abs dark zone x : ERROR\n");

	} else {
		TPD_INFO("abs dark zone x : %d\n", config);
		seq_printf(s, "abs dark zone x : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_Y,
					     &config);

	if (retval < 0) {
		TPD_INFO("abs dark zone y : ERROR\n");
		seq_printf(s, "abs dark zone y : ERROR\n");

	} else {
		TPD_INFO("abs dark zone y : %d\n", config);
		seq_printf(s, "abs dark zone y : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_U,
					     &config);

	if (retval < 0) {
		TPD_INFO("abs dark zone U : ERROR\n");
		seq_printf(s, "abs dark zone U : ERROR\n");

	} else {
		TPD_INFO("abs dark zone U : %d\n", config);
		seq_printf(s, "abs dark zone U : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_ABS_DARK_V,
					     &config);

	if (retval < 0) {
		TPD_INFO("abs dark zone V : ERROR\n");
		seq_printf(s, "abs dark zone V : ERROR\n");

	} else {
		TPD_INFO("abs dark zone V : %d\n", config);
		seq_printf(s, "abs dark zone V : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_CONDTION_ZONE,
					     &config);

	if (retval < 0) {
		TPD_INFO("condtion zone : ERROR\n");
		seq_printf(s, "condtion zone : ERROR\n");

	} else {
		TPD_INFO("condtion zone : %d\n", config);
		seq_printf(s, "condtion zone : 0x%0X\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_SPECIAL_ZONE_X,
					     &config);

	if (retval < 0) {
		TPD_INFO("special zone x : ERROR\n");
		seq_printf(s, "special zone x : ERROR\n");

	} else {
		TPD_INFO("special zone x : %d\n", config);
		seq_printf(s, "special zone x : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_SPECIAL_ZONE_Y,
					     &config);

	if (retval < 0) {
		TPD_INFO("special zone y : ERROR\n");
		seq_printf(s, "special zone y : ERROR\n");

	} else {
		TPD_INFO("special zone y : %d\n", config);
		seq_printf(s, "special zone y : %d\n", config);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_GRIP_SPECIAL_ZONE_L,
					     &config);

	if (retval < 0) {
		TPD_INFO("special zone len : ERROR\n");
		seq_printf(s, "special zone len : ERROR\n");

	} else {
		TPD_INFO("special zone len : %d\n", config);
		seq_printf(s, "special zone len : %d\n", config);
	}

	TPD_INFO("Buid ID:%d, Custom ID:0x%s\n",
		 le4_to_uint(tcm_info->id_info.build_id),
		 tcm_info->app_info.customer_config_id);

	seq_printf(s, "Buid ID:%d, Custom ID:0x%s\n",
		   le4_to_uint(tcm_info->id_info.build_id),
		   tcm_info->app_info.customer_config_id);

	TPD_INFO("APP info : version:%d\n",
		 le2_to_uint(tcm_info->app_info.version));

	TPD_INFO("APP info : status:%d\n",
		 le2_to_uint(tcm_info->app_info.status));

	TPD_INFO("APP info : max_touch_report_config_size:%d\n",
		 le2_to_uint(tcm_info->app_info.max_touch_report_config_size));
	TPD_INFO("APP info : max_touch_report_payload_size:%d\n",
		 le2_to_uint(tcm_info->app_info.max_touch_report_payload_size));

	TPD_INFO("APP info : customer_config_id:%d\n",
		 le2_to_uint(tcm_info->app_info.customer_config_id));
	TPD_INFO("APP info : max_x:%d\n",
		 le2_to_uint(tcm_info->app_info.max_x));

	TPD_INFO("APP info : max_y:%d\n",
		 le2_to_uint(tcm_info->app_info.max_y));

	TPD_INFO("APP info : num_of_image_rows:%d\n",
		 le2_to_uint(tcm_info->app_info.num_of_image_rows));

	TPD_INFO("APP info : num_of_image_cols:%d\n",
		 le2_to_uint(tcm_info->app_info.num_of_image_cols));

	seq_printf(s, "APP info : version:%d\n",
		   le2_to_uint(tcm_info->app_info.version));

	seq_printf(s, "APP info : status:%d\n",
		   le2_to_uint(tcm_info->app_info.status));

	temp = le2_to_uint(tcm_info->app_info.max_touch_report_config_size);
	seq_printf(s, "APP info : max_touch_report_config_size:%d\n",
		   temp);

	temp = le2_to_uint(tcm_info->app_info.max_touch_report_payload_size);
	seq_printf(s, "APP info : max_touch_report_payload_size:%d\n",
		   temp);

	seq_printf(s, "APP info : customer_config_id:%d\n",
		   le2_to_uint(tcm_info->app_info.customer_config_id));

	seq_printf(s, "APP info : max_x:%d\n",
		   le2_to_uint(tcm_info->app_info.max_x));

	seq_printf(s, "APP info : max_y:%d\n",
		   le2_to_uint(tcm_info->app_info.max_y));

	seq_printf(s, "APP info : num_of_image_rows:%d\n",
		   le2_to_uint(tcm_info->app_info.num_of_image_rows));

	seq_printf(s, "APP info : num_of_image_cols:%d\n",
		   le2_to_uint(tcm_info->app_info.num_of_image_cols));

	if (tcm_info->default_config.data_length > 0) {
		seq_printf(s, "default_config:%*ph\n", tcm_info->default_config.data_length, tcm_info->default_config.buf);
		TPD_INFO("default_config:%*ph\n", tcm_info->default_config.data_length, tcm_info->default_config.buf);
	}

	if (tcm_info->config.data_length > 0) {
		seq_printf(s, "config:%*ph\n", tcm_info->config.data_length, tcm_info->config.buf);
		TPD_INFO("config:%*ph\n", tcm_info->config.data_length, tcm_info->config.buf);
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_LOW_TEMP_ENABLE, &config);
	if (retval < 0) {
		TPD_INFO("Failed to get temperature config\n");
	}
	seq_printf(s, "DC_LOW_TEMP_ENABLE:%d\n", config);
	TPD_INFO("DC_LOW_TEMP_ENABLE:%d\n", config);
	return;
}

static void syna_delta_read(struct seq_file *s, void *chip_data)
{
	int retval;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 1);

	if (retval < 0) {
		TPD_INFO("Failed to exit doze\n");
	}

	msleep(20); /* delay 20ms*/

	retval = syna_tcm_collect_reports(tcm_info, REPORT_DELTA, 1);

	if (retval < 0) {
		seq_printf(s, "Failed to read delta data\n");
		return;
	}

	syna_tcm_format_print(s, tcm_info, NULL);

	/*set normal doze*/
	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 0);

	if (retval < 0) {
		TPD_INFO("Failed to switch to normal\n");
	}

	return;
}

static void syna_baseline_read(struct seq_file *s, void *chip_data)
{
	int retval;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 1);

	if (retval < 0) {
		TPD_INFO("Failed to exit doze\n");
	}

	msleep(20); /* delay 20ms*/

	retval = syna_tcm_collect_reports(tcm_info, REPORT_RAW, 1);

	if (retval < 0) {
		seq_printf(s, "Failed to read baseline data\n");
		return;
	}

	syna_tcm_format_unsigned_print(s, tcm_info, NULL);

	/*set normal doze*/
	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 0);

	if (retval < 0) {
		TPD_INFO("Failed to switch to normal\n");
	}

	return;
}

/*static struct synaptics_proc_operations syna_proc_ops = {*/
/*.auto_test     = syna_auto_test,*/
/*};*/

static  void syna_reserve_read(struct seq_file *s, void *chip_data)
{
	int retval;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 1);

	if (retval < 0) {
		TPD_INFO("Failed to exit doze\n");
	}

	msleep(20); /* delay 20ms*/

	retval = syna_tcm_collect_reports(tcm_info, REPORT_DEBUG, 1);

	if (retval < 0) {
		seq_printf(s, "Failed to read delta data\n");
		return;
	}

	syna_tcm_format_unsigned_print(s, tcm_info, NULL);

	/*set normal doze*/
	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 0);

	if (retval < 0) {
		TPD_INFO("Failed to switch to normal\n");
	}

	return;
}

static void syna_delta_snr_read(struct seq_file *s, void *chip_data, uint32_t count)
{
	int retval;
	int i = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	struct touchpanel_snr *snr = tcm_info->snr;

	if (!tcm_info->snr_read_support) {
		seq_printf(s, "snr read not support! \n");
		return;
	}
	if (!snr[0].doing) {
		seq_printf(s, "snr doing zero! \n");
		return;
	}

	for (i = 0; i < count; i++) {
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 1);
		if (retval < 0) {
			TPD_INFO("Failed to exit doze\n");
		}

		msleep(20); /* delay 20ms */

		retval = syna_tcm_collect_reports(tcm_info, REPORT_DELTA, 1);
		if (retval < 0) {
			seq_printf(s, "Failed to read delta data\n");
			return;
		}

		syna_tcm_format_print_snr(s, tcm_info, i);

		/*set normal doze*/
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 0);
		if (retval < 0) {
			TPD_INFO("Failed to switch to normal\n");
		}
	}
	for (i = 0; i < 10; i++) {
		if (snr[i].point_status) {
			snr[i].noise = snr[i].max - snr[i].min;
			seq_printf(s, "%d|%d|", snr[i].channel_x, snr[i].channel_y);
			seq_printf(s, "%d|", snr[i].max);
			seq_printf(s, "%d|", snr[i].min);
			seq_printf(s, "%d|", snr[i].sum / count);
			seq_printf(s, "%d\n", snr[i].noise);
			SNR_RESET(snr[i]);
			TPD_DETAIL("snr-cover [%d %d] %d %d %d\n", snr[i].channel_x, snr[i].channel_y, snr[i].max, snr[i].min, snr[i].sum);
		}
	}
	return;
}

static struct debug_info_proc_operations syna_debug_proc_ops = {
	.delta_read    = syna_delta_read,
	.baseline_read = syna_baseline_read,
	.main_register_read = syna_main_register,
	.reserve_read  = syna_reserve_read,
	.delta_snr_read = syna_delta_snr_read,
};

static void syna_start_aging_test(void *chip_data)
{
	int ret = -1;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	TPD_INFO("%s: start aging test \n", __func__);
	ret = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 1);
	if (ret < 0) {
		TPD_INFO("%s: start aging test failed!\n", __func__);
	}
}

static void syna_finish_aging_test(void *chip_data)
{
	int ret = -1;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	TPD_INFO("%s: finish aging test \n", __func__);
	ret = syna_tcm_set_dynamic_config(tcm_info, DC_NO_DOZE, 0);
	if (ret < 0) {
		TPD_INFO("%s: finish aging test failed!\n", __func__);
	}
}

static struct aging_test_proc_operations aging_test_proc_ops = {
	.start_aging_test = syna_start_aging_test,
	.finish_aging_test = syna_finish_aging_test,
};


static int syna_device_report_touch(struct syna_tcm_data *tcm_info)
{
	int ret = syna_parse_report(tcm_info);

	if (ret < 0) {
		TPD_INFO("Failed to parse report\n");
		return -EINVAL;
	}

	syna_set_trigger_reason(tcm_info, IRQ_TOUCH);
	return 0;
}

static int syna_resume_prepare(void *chip_data)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	reinit_completion(&tcm_info->resume_complete);
	return 0;
}

static int syna_specific_resume_operate(void *chip_data, struct specific_resume_data *p)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	int timed_out = 0;

	tcm_info->suspend_state = p->suspend_state;
	tcm_info->in_test_process = p->in_test_process;
	TPD_INFO("enter state : %d,in_test_process:%d\n", __func__, p->suspend_state, p->in_test_process);

	timed_out = wait_for_completion_timeout(&tcm_info->resume_complete, 0.5 * HZ); /* wait resume over for 0.5s */

	if ((0 == timed_out) || (tcm_info->resume_complete.done)) {
		TPD_INFO("resume state, timed_out:%d, done:%d\n", timed_out, tcm_info->resume_complete.done);
	}
	return 0;
}

static void syna_set_touch_direction(void *chip_data, uint8_t dir)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	tcm_info->touch_direction = dir;
}

static uint8_t syna_get_touch_direction(void *chip_data)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	return tcm_info->touch_direction;
}

static void syna_freq_hop_trigger(void *chip_data)
{
	int retval = 0;
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	TPD_INFO("%s : send cmd to tigger frequency hopping here!!!\n", __func__);

	switch (tcm_info->freq_point) {
	case 0:
		TPD_INFO("%s : Hop to frequency : %d\n", __func__, tcm_info->freq_point);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_FREQUENCE_HOPPING, tcm_info->freq_point);
		if (retval < 0) {
			TPD_INFO("Failed to hop frequency\n");
		}
		tcm_info->freq_point = 1;
		break;

	case 1:
		TPD_INFO("%s : Hop to frequency : %d\n", __func__, tcm_info->freq_point);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_FREQUENCE_HOPPING, tcm_info->freq_point);
		if (retval < 0) {
			TPD_INFO("Failed to hop frequency\n");
		}
		tcm_info->freq_point = 2;
		break;

	case 2:
		TPD_INFO("%s : Hop to frequency : %d\n", __func__, tcm_info->freq_point);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_FREQUENCE_HOPPING, tcm_info->freq_point);
		if (retval < 0) {
			TPD_INFO("Failed to hop frequency\n");
		}
		tcm_info->freq_point = 3;
		break;
	case 3:
		TPD_INFO("%s : Hop to frequency : %d\n", __func__, tcm_info->freq_point);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_FREQUENCE_HOPPING, tcm_info->freq_point);
		if (retval < 0) {
			TPD_INFO("Failed to hop frequency\n");
		}
		tcm_info->freq_point = 4;
		break;

	case 4:
		TPD_INFO("%s : Hop to frequency : %d\n", __func__, tcm_info->freq_point);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_FREQUENCE_HOPPING,
						     tcm_info->freq_point);

		if (retval < 0) {
			TPD_INFO("Failed to hop frequency\n");
		}

		tcm_info->freq_point = 5;
		break;

	case 5:
		TPD_INFO("%s : Hop to frequency : %d\n", __func__, tcm_info->freq_point);
		retval = syna_tcm_set_dynamic_config(tcm_info, DC_FREQUENCE_HOPPING,
						     tcm_info->freq_point);

		if (retval < 0) {
			TPD_INFO("Failed to hop frequency\n");
		}

		tcm_info->freq_point = 0;
		break;

	default:
		break;
	}
}

static int syna_tcm_smooth_lv_set(void *chip_data, int level)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned short regval = 0;
	int retval = 0;

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_ERROR_PRIORITY, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get smooth config\n");
		tcm_info->error_state_count++;
		return 0;
	}
	tcm_info->error_state_count = 0;

	retval = syna_tcm_set_dynamic_config(tcm_info, DC_ERROR_PRIORITY, (level << 4) | (regval & 0x01));
	if (retval < 0) {
		TPD_INFO("Failed to set smooth config\n");
		return 0;
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_ERROR_PRIORITY, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get smooth config\n");
		return 0;
	}
	TPD_INFO("OK synaptics smooth lv to %d, now reg_val:0x%x", level, regval);
	return 0;
}

static int syna_tcm_sensitive_lv_set(void *chip_data, int level)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned short regval = 0;
	int retval = 0;

	retval = syna_tcm_set_dynamic_config(tcm_info, DC_NOISE_LENGTH, level);
	if (retval < 0) {
		TPD_INFO("Failed to set sensitive config\n");
		tcm_info->error_state_count++;
		return 0;
	}
	tcm_info->error_state_count = 0;

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_NOISE_LENGTH, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get sensitive config\n");
		return 0;
	}
	TPD_INFO("OK synaptics sensitive lv to %d, now reg_val:%d", level, regval);

	return 0;
}


static int syna_tcm_send_temperature(void *chip_data, int temp, bool status)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	unsigned short regval = 0;
	int retval = 0;
	unsigned short temp_mode = 0;

	/*enable low tempreratue*/
	if (temp <= tcm_info->syna_tempepratue[0]) {
		temp_mode = 1;
		tcm_info->syna_low_temp_enable++;
		tcm_info->syna_low_temp_disable = 0;
	/*disable low tempreratue*/
	} else if (temp > tcm_info->syna_tempepratue[1]) {
		temp_mode = 0;
		tcm_info->syna_low_temp_enable = 0;
		tcm_info->syna_low_temp_disable++;
	} else {
		if (tcm_info->syna_low_temp_enable > 0) {
			temp_mode = 1;
			tcm_info->syna_low_temp_enable++;
			tcm_info->syna_low_temp_disable = 0;
		} else {
			temp_mode = 0;
			tcm_info->syna_low_temp_enable = 0;
			tcm_info->syna_low_temp_disable++;
		}
	}

	if (tcm_info->syna_low_temp_enable > 1 || tcm_info->syna_low_temp_disable > 1) {
		TPD_DEBUG("enable or disable low temp mode is more than one time\n");
		return 0;
	}
	retval = syna_tcm_get_dynamic_config(tcm_info, DC_LOW_TEMP_ENABLE, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get temperature config\n");
		return 0;
	}


	if (1 == temp_mode)  {
		temp_mode = regval | 0x02;
	} else {
		temp_mode = regval & 0xfd;
	}
	retval = syna_tcm_set_dynamic_config(tcm_info, DC_LOW_TEMP_ENABLE, temp_mode);
	if (retval < 0) {
		TPD_INFO("Failed to set temperature config\n");
		return 0;
	}

	retval = syna_tcm_get_dynamic_config(tcm_info, DC_LOW_TEMP_ENABLE, &regval);
	if (retval < 0) {
		TPD_INFO("Failed to get temperature config\n");
		return 0;
	}
	TPD_INFO("OK synaptics temperature to %d, now reg_val:%d", temp_mode, regval);

	return retval;
}

static void syna_set_gesture_state(void *chip_data, int state)
{
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;

	TPD_INFO("%s:state:%d!\n", __func__, state);
	tcm_info->gesture_state = state;
}

static struct oplus_touchpanel_operations syna_tcm_ops = {
	.ftm_process			= syna_ftm_process,
	.get_vendor			= syna_get_vendor,
	.get_chip_info			= syna_get_chip_info,
	.get_touch_points		= syna_get_touch_points,
	.get_gesture_info		= syna_get_gesture_info,
	.power_control			= syna_power_control,
	.reset				= syna_tcm_reset,
	.trigger_reason			= syna_trigger_reason,
	.mode_switch			= syna_mode_switch,
	.fw_check			= syna_fw_check,
	.fw_update			= syna_tcm_fw_update,
	.reinit_device			= syna_tcm_reinit_device,
	.enable_fingerprint		= syna_tcm_enable_fingerprint,
	.screenon_fingerprint_info	= syna_tcm_fingerprint_info,
	.health_report			= syna_tcm_get_health_info,
	.set_touch_direction		= syna_set_touch_direction,
	.get_touch_direction		= syna_get_touch_direction,
	.freq_hop_trigger		= syna_freq_hop_trigger,
	.enable_gesture_mask		= syna_tcm_enable_gesture_mask,
	.speed_up_resume_prepare	= syna_resume_prepare,
	.specific_resume_operate	= syna_specific_resume_operate,
	.smooth_lv_set			= syna_tcm_smooth_lv_set,
	.sensitive_lv_set		= syna_tcm_sensitive_lv_set,
	.get_touch_points_auto		= syna_get_touch_points_auto,
	.get_gesture_info_auto		= syna_get_gesture_info_auto,
	.screenon_fingerprint_info_auto	= syna_tcm_fingerprint_info_auto,
	.tp_refresh_switch		= syna_report_refresh_switch,
	.rate_white_list_ctrl		= syna_rate_white_list_ctrl,
	.set_gesture_state         	= syna_set_gesture_state,
	.get_touch_points_help		= syna_get_touch_points_help,
	.set_high_frame_rate            = syna_tcm_set_high_frame_rate,
	.send_temperature		= syna_tcm_send_temperature,
};

static void syna_async_work_lock(struct work_struct *work)
{
	struct syna_tcm_data *tcm_info = container_of(work, struct syna_tcm_data,
					 async_work);

	syna_tcm_async_work(tcm_info);
}

static void init_chip_dts(struct device *dev, void *chip_data)
{
	int rc;
	int ret = 0;
	struct device_node *np;
	struct device_node *chip_np;
	int i = 0;
	int temp_array[FPS_REPORT_NUM];
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)chip_data;
	np = dev->of_node;

	tcm_info->snr_read_support = of_property_read_bool(np, "snr_read_support");

	chip_np = of_get_child_by_name(np, "S3910");

	if (!chip_np) {
		tcm_info->display_refresh_rate = 60;
		tcm_info->game_rate = 1;
		tcm_info->default_gesture_mask = 0xFFFF;
		tcm_info->gesture_mask = tcm_info->default_gesture_mask;
		/*default :0:125hz 1:240hz 2:360HZ is for 21641*/
		tcm_info->fps_report_rate_num = 6;
		tcm_info->fps_report_rate_array[0] = 60;
		tcm_info->fps_report_rate_array[1] = 0;
		tcm_info->fps_report_rate_array[2] = 90;
		tcm_info->fps_report_rate_array[3] = 1;
		tcm_info->fps_report_rate_array[4] = 120;
		tcm_info->fps_report_rate_array[5] = 2;
		tcm_info->syna_tempepratue[0] = 5;
		tcm_info->syna_tempepratue[1] = 15;
		tcm_info->syna_low_temp_enable = 0;
		tcm_info->syna_low_temp_disable = 1;
		tcm_info->fwupdate_bootloader = 0;
		tcm_info->normal_config_version = 0;
		return;
	}

	rc = of_property_read_u32(chip_np, "report_rate_default", &tcm_info->display_refresh_rate);
	if (rc < 0) {
		tcm_info->display_refresh_rate = 60;
	}
	TPD_INFO("default rate %d\n", tcm_info->display_refresh_rate);

	rc = of_property_read_u32(chip_np, "report_rate_game_value", &ret);
	if (rc < 0) {
		ret = 1;
	}
	TPD_INFO("default game value %d\n", ret);
	tcm_info->game_rate = ret;
	tcm_info->switch_game_rate_support = of_property_read_bool(chip_np, "switch_report_rate");
	rc = of_property_read_u32(chip_np, "default_gesture_mask", &ret);
	if (rc < 0) {
		ret = 0xFFFF;
	}
	TPD_INFO("default gesture mask value %d\n", ret);
	tcm_info->default_gesture_mask = (uint16_t)(ret & 0xFFFF);
	tcm_info->gesture_mask = tcm_info->default_gesture_mask;
	rc = of_property_count_u32_elems(chip_np, "fps_report_rate");
	tcm_info->fps_report_rate_num = rc;

	if (tcm_info->fps_report_rate_num > 0 && tcm_info->fps_report_rate_num <= FPS_REPORT_NUM
		&& !(tcm_info->fps_report_rate_num % 2)) {
		rc = of_property_read_u32_array(chip_np, "fps_report_rate", temp_array, tcm_info->fps_report_rate_num);
		if (rc) {
			TPD_INFO("fps_report_rate not specified %d\n", rc);
		} else {
			for (i = 0; i < tcm_info->fps_report_rate_num; i++) {
				tcm_info->fps_report_rate_array[i] = temp_array[i];
				TPD_INFO("fps_report_rate is: %d\n", tcm_info->fps_report_rate_array[i]);
			}
		}
	} else {
	    /*default :0:125hz 1:240hz 2:360HZ is for 21641*/
	    tcm_info->fps_report_rate_num = 6;
		tcm_info->fps_report_rate_array[0] = 60;
		tcm_info->fps_report_rate_array[1] = 0;
		tcm_info->fps_report_rate_array[2] = 90;
		tcm_info->fps_report_rate_array[3] = 1;
		tcm_info->fps_report_rate_array[4] = 120;
		tcm_info->fps_report_rate_array[5] = 2;
		TPD_INFO("fps_report_rate is not dubole %d\n", tcm_info->fps_report_rate_num);
	}
	rc = of_property_read_u32(chip_np, "fwupdate_bootloader", &tcm_info->fwupdate_bootloader);
	if (rc < 0) {
		tcm_info->fwupdate_bootloader = 0;
		TPD_INFO("fwupdate_bootloader %d\n", tcm_info->fwupdate_bootloader);
	}
	tcm_info->syna_tempepratue[0] = 5;
	tcm_info->syna_tempepratue[1] = 15;
	tcm_info->syna_low_temp_enable = 0;
	tcm_info->syna_low_temp_disable = 1;
	rc = of_property_read_u32(chip_np, "normal_config_version", &tcm_info->normal_config_version);
	if (rc < 0) {
		/* latest projects' default normal config is version-2, old projects with version-0 need to config in dts */
		tcm_info->normal_config_version = 2;
		TPD_INFO("normal_config_version %d\n", tcm_info->normal_config_version);
	}
}

static int syna_tcm_probe(struct spi_device *spi)
{
	int retval = 0;
	struct syna_tcm_data *tcm_info = NULL;
	struct touchpanel_data *ts = NULL;
	struct device_hcd *device_hcd = NULL;

	TPD_INFO("%s: enter\n", __func__);


	/*1. alloc mem for tcm_data*/
	tcm_info = kzalloc(sizeof(*tcm_info), GFP_KERNEL);

	if (!tcm_info) {
		TPD_INFO("no more memory\n");
		return -ENOMEM;
	}

	/*2. alloc mem for touchpanel_data */
	ts = common_touch_data_alloc();

	if (ts == NULL) {
		TPD_INFO("failed to alloc common data\n");
		goto ts_alloc_failed;
	}

	/*3. init member of ts*/
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
	spi->controller_data = (void *)&st_spi_ctrdata;
#endif
	ts->dev = &spi->dev;
	ts->s_client  = spi;
	ts->irq = spi->irq;
	ts->chip_data = tcm_info;
	ts->s_client->chip_select = 0; /*modify reg=0 for more tp vendor share same spi interface*/
	spi_set_drvdata(spi, ts);
	ts->ts_ops = &syna_tcm_ops;
	ts->engineer_ops = &syna_tcm_engineer_test_ops;
	ts->com_test_data.chip_test_ops = &syna_tcm_test_ops;
	ts->debug_info_ops = &syna_debug_proc_ops;
	ts->aging_test_ops = &aging_test_proc_ops;
	ts->bus_type = TP_BUS_SPI;
	tcm_info->ts = ts;

	/*4. init member of tcm_info*/
	tcm_info->client = spi;
	tcm_info->hw_res = &ts->hw_res;
	tcm_info->ubl_addr = 0x2c;
	tcm_info->rd_chunk_size = RD_CHUNK_SIZE;
	tcm_info->wr_chunk_size = WR_CHUNK_SIZE;
	tcm_info->read_length = MIN_READ_LENGTH;
	tcm_info->in_suspend = &ts->is_suspended;
	tcm_info->loading_fw = &ts->loading_fw;
	/*tcm_info->display_refresh_rate = 90;*/
	tcm_info->game_mode = false;
	tcm_info->boot_flag = true;
	tcm_info->first_sync_flag = true;
	tcm_info->snr = ts->snr;

	atomic_set(&tcm_info->command_status, CMD_IDLE);
	mutex_init(&tcm_info->reset_mutex);
	mutex_init(&tcm_info->rw_mutex);
	mutex_init(&tcm_info->command_mutex);
	mutex_init(&tcm_info->identify_mutex);
	init_completion(&tcm_info->response_complete);
	init_completion(&tcm_info->report_complete);

	init_completion(&tcm_info->resume_complete);
	tcm_info->async_workqueue = create_singlethread_workqueue("syna_async");

	if (!tcm_info->async_workqueue) {
		retval = -ENOMEM;
		goto err_async_workqueue;
	}

	INIT_WORK(&tcm_info->async_work, syna_async_work_lock);

	INIT_BUFFER(tcm_info->in, false);
	INIT_BUFFER(tcm_info->out, false);
	INIT_BUFFER(tcm_info->resp, true);
	INIT_BUFFER(tcm_info->temp, false);
	INIT_BUFFER(tcm_info->config, false);
	INIT_BUFFER(tcm_info->default_config, false);
	INIT_BUFFER(tcm_info->report.buffer, true);

	/*5. alloc mem for reading in buffer*/
	LOCK_BUFFER(tcm_info->in);
	retval = syna_tcm_alloc_mem(&tcm_info->in, MAX_READ_LENGTH);
	TPD_INFO("%s read_length:%d\n", __func__, tcm_info->read_length);

	if (retval < 0) {
		TPD_INFO("Failed to allocate memory for tcm_info->in.buf\n");
		UNLOCK_BUFFER(tcm_info->in);
		goto err_malloc_inbuffer;
	}

	UNLOCK_BUFFER(tcm_info->in);

	/*6. create workqueue and init helper work*/
	tcm_info->helper_workqueue = create_singlethread_workqueue("syna_tcm_helper");
	if (!tcm_info->helper_workqueue) {
		retval = -ENOMEM;
		goto err_helper_workqueue;
	}

	INIT_WORK(&tcm_info->helper_work, syna_tcm_helper_work);

	/*7. alloc mem for touch_hcd and init it's member*/
	tcm_info->touch_hcd = (struct touch_hcd *)tp_devm_kzalloc(ts->dev,
			      sizeof(struct touch_hcd), GFP_KERNEL);

	if (!tcm_info->touch_hcd) {
		retval = -ENOMEM;
		goto err_malloc_touchhcd;
	}

	INIT_BUFFER(tcm_info->touch_hcd->out, false);
	INIT_BUFFER(tcm_info->touch_hcd->resp, false);
	mutex_init(&tcm_info->touch_hcd->report_mutex);
	of_property_read_u32(ts->dev->of_node, "touchpanel,max-num-support",
			     &tcm_info->touch_hcd->max_objects);
	tcm_info->touch_hcd->touch_data.object_data =
		(struct object_data *)tp_devm_kzalloc(ts->dev,
				sizeof(struct object_data) * tcm_info->touch_hcd->max_objects, GFP_KERNEL);

	if (!tcm_info->touch_hcd->touch_data.object_data) {
		retval = -ENOMEM;
		goto err_malloc_object_data;
	}

	/*8. alloc mem for test_hcd and it's member*/
	tcm_info->test_hcd = (struct syna_tcm_test *)tp_devm_kzalloc(ts->dev,
			     sizeof(struct syna_tcm_test), GFP_KERNEL);

	if (!tcm_info->test_hcd) {
		retval = -ENOMEM;
		goto err_malloc_test;
	}

	INIT_BUFFER(tcm_info->test_hcd->report, false);
	INIT_BUFFER(tcm_info->test_hcd->test_resp, false);
	INIT_BUFFER(tcm_info->test_hcd->test_out, false);

	/*9. register common part of touchpanel driver*/
	retval = register_common_touch_device(ts);

	if (retval < 0 && (retval != -EFTM)) {
		TPD_INFO("Failed to init device information\n");
		goto err_malloc_register;
	}

	tcm_info->monitor_data = &ts->monitor_data;
	tcm_info->tp_index = ts->tp_index;
	init_chip_dts(ts->dev, tcm_info);
	tcm_info->black_gesture_indep = ts->black_gesture_indep_support;

	tcm_info->chip_resolution_info = &ts->resolution_info;

	/*11. create synaptics common file*/
	synaptics_create_proc(ts, tcm_info->syna_ops);

	/*12. create remote device file and init it's callback*/
	device_hcd = syna_remote_device_S3010_init(tcm_info);

	if (device_hcd) {
		device_hcd->irq = tcm_info->client->irq;
		device_hcd->read_message = syna_tcm_read_message;
		device_hcd->write_message = syna_tcm_write_message;
		device_hcd->reset = syna_tcm_reset;
		device_hcd->report_touch = syna_device_report_touch;
		device_hcd->tp_index = ts->tp_index;
	}
	tcm_info->fw_update_app_support = &ts->fw_update_app_support;
	tcm_info->loading_fw  = &ts->loading_fw;

	if (tcm_info->fwupdate_bootloader) {
		tcm_info->g_fw_buf = vmalloc(FW_BUF_SIZE);
		if (tcm_info->g_fw_buf == NULL) {
			TPD_INFO("fw buf vmalloc error\n");
			goto err_malloc_register;
		}
		tcm_info->g_fw_sta = false;
		tcm_info->probe_done = 1;
	}



	g_tcm_info[tcm_info->tp_index] = tcm_info;

	return 0;

err_malloc_register:
err_malloc_test:
err_malloc_object_data:
err_malloc_touchhcd:
	cancel_work_sync(&tcm_info->helper_work);
	flush_workqueue(tcm_info->helper_workqueue);
	destroy_workqueue(tcm_info->helper_workqueue);

err_helper_workqueue:
	RELEASE_BUFFER(tcm_info->in);

err_malloc_inbuffer:
	cancel_work_sync(&tcm_info->async_work);
	flush_workqueue(tcm_info->async_workqueue);
	destroy_workqueue(tcm_info->async_workqueue);

err_async_workqueue:
	spi_set_drvdata(spi, NULL);
	common_touch_data_free(ts);
	ts = NULL;

ts_alloc_failed:
	if (tcm_info) {
		kfree(tcm_info);
		tcm_info = NULL;
	}
	return retval;
}

static void syna_tcm_tp_shutdown(struct spi_device *s_client)
{
	struct touchpanel_data *ts = spi_get_drvdata(s_client);

	TPD_INFO("%s is called\n", __func__);

	tp_shutdown(ts);
}

static int syna_tcm_remove(struct spi_device *s_client)
{
	struct touchpanel_data *ts =  spi_get_drvdata(s_client);
	struct syna_tcm_data *tcm_info = (struct syna_tcm_data *)ts->chip_data;

	RELEASE_BUFFER(tcm_info->report.buffer);
	RELEASE_BUFFER(tcm_info->config);
	RELEASE_BUFFER(tcm_info->temp);
	RELEASE_BUFFER(tcm_info->resp);
	RELEASE_BUFFER(tcm_info->out);
	RELEASE_BUFFER(tcm_info->in);

	if (tcm_info->fwupdate_bootloader && tcm_info->g_fw_buf) {
		vfree(tcm_info->g_fw_buf);
	}

	if (ts) {
		unregister_common_touch_device(ts);
		common_touch_data_free(ts);
		tp_kfree((void **)&ts);
	}

	tp_kfree((void **)&tcm_info);
	spi_set_drvdata(s_client, NULL);

	return 0;
}

static int syna_spi_suspend(struct device *dev)
{
	struct touchpanel_data *ts = dev_get_drvdata(dev);

	TPD_INFO("%s: is called\n", __func__);
	tp_pm_suspend(ts);
	return 0;
}

static int syna_spi_resume(struct device *dev)
{
	struct touchpanel_data *ts = dev_get_drvdata(dev);

	TPD_INFO("%s is called\n", __func__);
	tp_pm_resume(ts);
	return 0;
}

static const struct dev_pm_ops syna_pm_ops = {
	.suspend = syna_spi_suspend,
	.resume = syna_spi_resume,
};

static const struct spi_device_id syna_tmc_id[] = {
        { TPD_DEVICE, 0},
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
        { "oplus,tp_noflash", 0 },
#endif
        { }
};

static struct of_device_id syna_match_table[] = {
        { .compatible = TPD_DEVICE, },
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
        { .compatible = "oplus,tp_noflash", },
#endif
        { .compatible = "synaptics-s3910", },
        { }
};

static struct spi_driver syna_spi_driver = {
	.probe      = syna_tcm_probe,
	.remove     = syna_tcm_remove,
	.id_table   = syna_tmc_id,
	.shutdown   = syna_tcm_tp_shutdown,
	.driver = {
		.name = TPD_DEVICE,
		.owner = THIS_MODULE,
		.of_match_table =  syna_match_table,
		.pm = &syna_pm_ops,
	},
};


static int __init tp_driver_init_syna_tcm(void)
{
	TPD_INFO("%s is called\n", __func__);

	if (!tp_judge_ic_match(TPD_DEVICE)) {
		TPD_INFO("tp_judge_ic_match fail\n");
		return 0;
	}
	TPD_INFO("spi_register_driver\n");
	if (spi_register_driver(&syna_spi_driver) != 0) {
		TPD_INFO("unable to add spi driver.\n");
		return -1;
	}
	TPD_INFO("spi_register_driver OK\n");
	return 0;
}

static void __exit tp_driver_exit_syna_tcm(void)
{
	TPD_INFO("%s is called\n", __func__);

	spi_unregister_driver(&syna_spi_driver);
	return;
}

#ifdef CONFIG_TOUCHPANEL_LATE_INIT
late_initcall(tp_driver_init_syna_tcm);
#else
module_init(tp_driver_init_syna_tcm);
#endif
module_exit(tp_driver_exit_syna_tcm);

MODULE_DESCRIPTION("Touchscreen Synaptics tcm oncell Driver");
MODULE_LICENSE("GPL");
