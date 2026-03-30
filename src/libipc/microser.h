/* microser.h : minimal tagged serialization for embedded systems */
/* Originally from Tiny IDL (public domain / MIT-0)
 * Extended with wire type 4 (length-prefixed bytes) for lumimux.
 * Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

/*
 * No heap allocation. No dependencies beyond <stdint.h> and <string.h>.
 *
 * Wire format:
 *   Message = uint16le(payload_length) | field...
 *   Field   = tag_byte | value_bytes
 *   Tag     = (field_number << 3) | wire_type
 *
 * Wire types:
 *   0 = 1 byte  (uint8, int8)
 *   1 = 2 bytes (uint16, int16) little-endian
 *   2 = 4 bytes (uint32, int32) little-endian
 *   3 = 8 bytes (uint64, int64) little-endian
 *   4 = length-prefixed bytes: uint16le(len) + len bytes
 *
 * Field numbers 1-31. Unknown fields are skippable by wire type.
 */

#ifndef MICROSER_H
#define MICROSER_H

#include <stdint.h>
#include <string.h>

#define MS_WIRE_8	0
#define MS_WIRE_16	1
#define MS_WIRE_32	2
#define MS_WIRE_64	3
#define MS_WIRE_BYTES	4

/* --- Write helpers: return new position, or -1 on overflow --- */

static inline int
ms_write_tag_u8(uint8_t *buf, int pos, int len, uint8_t field, uint8_t val)
{
	if (pos + 2 > len)
		return -1;
	buf[pos] = (uint8_t)((field << 3) | MS_WIRE_8);
	buf[pos + 1] = val;
	return pos + 2;
}

static inline int
ms_write_tag_i8(uint8_t *buf, int pos, int len, uint8_t field, int8_t val)
{
	return ms_write_tag_u8(buf, pos, len, field, (uint8_t)val);
}

static inline int
ms_write_tag_u16(uint8_t *buf, int pos, int len, uint8_t field, uint16_t val)
{
	if (pos + 3 > len)
		return -1;
	buf[pos] = (uint8_t)((field << 3) | MS_WIRE_16);
	buf[pos + 1] = (uint8_t)(val & 0xff);
	buf[pos + 2] = (uint8_t)((val >> 8) & 0xff);
	return pos + 3;
}

static inline int
ms_write_tag_i16(uint8_t *buf, int pos, int len, uint8_t field, int16_t val)
{
	return ms_write_tag_u16(buf, pos, len, field, (uint16_t)val);
}

static inline int
ms_write_tag_u32(uint8_t *buf, int pos, int len, uint8_t field, uint32_t val)
{
	if (pos + 5 > len)
		return -1;
	buf[pos] = (uint8_t)((field << 3) | MS_WIRE_32);
	buf[pos + 1] = (uint8_t)(val & 0xff);
	buf[pos + 2] = (uint8_t)((val >> 8) & 0xff);
	buf[pos + 3] = (uint8_t)((val >> 16) & 0xff);
	buf[pos + 4] = (uint8_t)((val >> 24) & 0xff);
	return pos + 5;
}

static inline int
ms_write_tag_i32(uint8_t *buf, int pos, int len, uint8_t field, int32_t val)
{
	return ms_write_tag_u32(buf, pos, len, field, (uint32_t)val);
}

/* Write a length-prefixed bytes field.
 * Format: tag byte + uint16le(dlen) + dlen bytes of data. */
static inline int
ms_write_tag_bytes(uint8_t *buf, int pos, int len, uint8_t field,
    const void *data, uint16_t dlen)
{
	if (pos + 3 + dlen > len)
		return -1;
	buf[pos] = (uint8_t)((field << 3) | MS_WIRE_BYTES);
	buf[pos + 1] = (uint8_t)(dlen & 0xff);
	buf[pos + 2] = (uint8_t)((dlen >> 8) & 0xff);
	if (dlen > 0)
		memcpy(buf + pos + 3, data, dlen);
	return pos + 3 + dlen;
}

/* --- Read helpers: return new position, or -1 on underflow --- */

static inline int
ms_read_u8(const uint8_t *buf, int pos, int end, uint8_t *val)
{
	if (pos + 1 > end)
		return -1;
	*val = buf[pos];
	return pos + 1;
}

static inline int
ms_read_i8(const uint8_t *buf, int pos, int end, int8_t *val)
{
	uint8_t tmp;
	int ret;

	ret = ms_read_u8(buf, pos, end, &tmp);
	if (ret >= 0)
		*val = (int8_t)tmp;
	return ret;
}

static inline int
ms_read_u16(const uint8_t *buf, int pos, int end, uint16_t *val)
{
	if (pos + 2 > end)
		return -1;
	*val = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
	return pos + 2;
}

static inline int
ms_read_i16(const uint8_t *buf, int pos, int end, int16_t *val)
{
	uint16_t tmp;
	int ret;

	ret = ms_read_u16(buf, pos, end, &tmp);
	if (ret >= 0)
		*val = (int16_t)tmp;
	return ret;
}

static inline int
ms_read_u32(const uint8_t *buf, int pos, int end, uint32_t *val)
{
	if (pos + 4 > end)
		return -1;
	*val = (uint32_t)buf[pos]
	    | ((uint32_t)buf[pos + 1] << 8)
	    | ((uint32_t)buf[pos + 2] << 16)
	    | ((uint32_t)buf[pos + 3] << 24);
	return pos + 4;
}

static inline int
ms_read_i32(const uint8_t *buf, int pos, int end, int32_t *val)
{
	uint32_t tmp;
	int ret;

	ret = ms_read_u32(buf, pos, end, &tmp);
	if (ret >= 0)
		*val = (int32_t)tmp;
	return ret;
}

/* Read a length-prefixed bytes field.
 * Stores pointer into buf (zero-copy) and length.
 * Returns new position, or -1 on underflow or if dlen exceeds dmax. */
static inline int
ms_read_bytes(const uint8_t *buf, int pos, int end,
    const uint8_t **data, uint16_t dmax, uint16_t *out_len)
{
	uint16_t dlen;

	if (pos + 2 > end)
		return -1;
	dlen = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
	pos += 2;
	if (pos + dlen > end || dlen > dmax)
		return -1;
	*data = buf + pos;
	*out_len = dlen;
	return pos + dlen;
}

/* Skip an unknown field by wire type */
static inline int
ms_skip(const uint8_t *buf, int pos, int end, uint8_t wire)
{
	uint16_t blen;

	if (wire <= 3) {
		static const int sizes[] = { 1, 2, 4, 8 };

		pos += sizes[wire];
		return pos <= end ? pos : -1;
	}
	if (wire == MS_WIRE_BYTES) {
		if (pos + 2 > end)
			return -1;
		blen = (uint16_t)buf[pos]
		    | ((uint16_t)buf[pos + 1] << 8);
		pos += 2 + blen;
		return pos <= end ? pos : -1;
	}
	return -1;
}

#endif /* MICROSER_H */
