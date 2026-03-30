/* lumi_msg.c : generated from lumi.idl -- do not edit */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "lumi_msg.h"
#include "microser.h"

#include <string.h>

int
ipc_size_encode(const struct ipc_size *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u16(buf, pos, len, 1, msg->rows);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_u16(buf, pos, len, 2, msg->cols);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_size_decode(struct ipc_size *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u16(buf, pos, end, &msg->rows);
			break;
		case 2:
			pos = ms_read_u16(buf, pos, end, &msg->cols);
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_win_id_encode(const struct ipc_win_id *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u32(buf, pos, len, 1, msg->id);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_win_id_decode(struct ipc_win_id *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u32(buf, pos, end, &msg->id);
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_win_entry_encode(const struct ipc_win_entry *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u32(buf, pos, len, 1, msg->id);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_u8(buf, pos, len, 2, msg->flags);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_bytes(buf, pos, len, 3,
	    (const void *)msg->title, msg->title_len);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_win_entry_decode(struct ipc_win_entry *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u32(buf, pos, end, &msg->id);
			break;
		case 2:
			pos = ms_read_u8(buf, pos, end, &msg->flags);
			break;
		case 3:
			{
				const uint8_t *_tmp;

				pos = ms_read_bytes(buf, pos, end,
				    &_tmp, 65535, &msg->title_len);
				msg->title = (const char *)_tmp;
			}
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_win_resize_encode(const struct ipc_win_resize *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u32(buf, pos, len, 1, msg->id);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_u16(buf, pos, len, 2, msg->rows);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_u16(buf, pos, len, 3, msg->cols);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_win_resize_decode(struct ipc_win_resize *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u32(buf, pos, end, &msg->id);
			break;
		case 2:
			pos = ms_read_u16(buf, pos, end, &msg->rows);
			break;
		case 3:
			pos = ms_read_u16(buf, pos, end, &msg->cols);
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_attr_txn_ok_encode(const struct ipc_attr_txn_ok *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u32(buf, pos, len, 1, msg->txn_id);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_attr_txn_ok_decode(struct ipc_attr_txn_ok *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u32(buf, pos, end, &msg->txn_id);
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_attr_kv_encode(const struct ipc_attr_kv *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u32(buf, pos, len, 1, msg->txn_id);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_bytes(buf, pos, len, 2,
	    (const void *)msg->key, msg->key_len);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_bytes(buf, pos, len, 3,
	    (const void *)msg->value, msg->value_len);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_attr_kv_decode(struct ipc_attr_kv *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u32(buf, pos, end, &msg->txn_id);
			break;
		case 2:
			{
				const uint8_t *_tmp;

				pos = ms_read_bytes(buf, pos, end,
				    &_tmp, 65535, &msg->key_len);
				msg->key = (const char *)_tmp;
			}
			break;
		case 3:
			{
				const uint8_t *_tmp;

				pos = ms_read_bytes(buf, pos, end,
				    &_tmp, 65535, &msg->value_len);
				msg->value = (const char *)_tmp;
			}
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_attr_key_encode(const struct ipc_attr_key *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_u32(buf, pos, len, 1, msg->txn_id);
	if (pos < 0)
		return -1;
	pos = ms_write_tag_bytes(buf, pos, len, 2,
	    (const void *)msg->key, msg->key_len);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_attr_key_decode(struct ipc_attr_key *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			pos = ms_read_u32(buf, pos, end, &msg->txn_id);
			break;
		case 2:
			{
				const uint8_t *_tmp;

				pos = ms_read_bytes(buf, pos, end,
				    &_tmp, 65535, &msg->key_len);
				msg->key = (const char *)_tmp;
			}
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

int
ipc_attr_entries_encode(const struct ipc_attr_entries *msg, uint8_t *buf, int len)
{
	int pos = 2;

	pos = ms_write_tag_bytes(buf, pos, len, 1,
	    (const void *)msg->entries, msg->entries_len);
	if (pos < 0)
		return -1;

	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);
	return pos;
}

int
ipc_attr_entries_decode(struct ipc_attr_entries *msg, const uint8_t *buf, int len)
{
	int end, pos = 2;

	if (len < 2)
		return -1;
	end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;
	if (end > len)
		return -1;
	memset(msg, 0, sizeof(*msg));

	while (pos < end) {
		uint8_t tag = buf[pos++];

		switch (tag >> 3) {
		case 1:
			{
				const uint8_t *_tmp;

				pos = ms_read_bytes(buf, pos, end,
				    &_tmp, 65535, &msg->entries_len);
				msg->entries = (const char *)_tmp;
			}
			break;
		default:
			pos = ms_skip(buf, pos, end, tag & 7);
			break;
		}
		if (pos < 0)
			return -1;
	}
	return end;
}

