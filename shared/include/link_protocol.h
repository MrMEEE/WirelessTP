#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Simple framed protocol for ESP32 <-> RP2040 and ESP32 <-> ESP32 links.
// Start with this transport-safe format, then extend as needed.

#define LP_SYNC_0 0x4c  // 'L'
#define LP_SYNC_1 0x44  // 'D'
#define LP_VERSION 0x01

#define LP_MAX_PAYLOAD 64

typedef enum {
  LP_MSG_HELLO = 0x01,
  LP_MSG_TAG_SET = 0x10,
  LP_MSG_TAG_CLEAR = 0x11,
  LP_MSG_LED_CMD = 0x20,
  LP_MSG_PAIR_SET = 0x30,
  LP_MSG_DEBUG = 0x40,
  LP_MSG_ACK = 0x7f
} lp_msg_type_t;

typedef struct __attribute__((packed)) {
  uint8_t sync0;
  uint8_t sync1;
  uint8_t version;
  uint8_t type;
  uint8_t seq;
  uint8_t length;
} lp_header_t;

typedef struct __attribute__((packed)) {
  lp_header_t header;
  uint8_t payload[LP_MAX_PAYLOAD];
  uint16_t crc16;
} lp_frame_t;

typedef enum {
  LP_PARSE_NONE = 0,
  LP_PARSE_FRAME_OK = 1,
  LP_PARSE_FRAME_BAD_HEADER = 2,
  LP_PARSE_FRAME_BAD_CRC = 3
} lp_parse_result_t;

typedef struct {
  uint8_t buffer[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t index;
  uint16_t expected;
} lp_stream_parser_t;

static inline uint16_t lp_crc16_ccitt(const uint8_t* data, uint16_t len) {
  uint16_t crc = 0xffff;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

static inline uint16_t lp_wire_size_from_payload(uint8_t payload_len) {
  return (uint16_t)(sizeof(lp_header_t) + payload_len + 2);
}

static inline void lp_stream_reset(lp_stream_parser_t* parser) {
  parser->index = 0;
  parser->expected = 0;
}

static inline void lp_stream_init(lp_stream_parser_t* parser) {
  lp_stream_reset(parser);
}

static inline bool lp_encode_frame(uint8_t type, uint8_t seq, const uint8_t* payload,
                                   uint8_t length, uint8_t* out, uint16_t out_cap,
                                   uint16_t* out_len) {
  if (length > LP_MAX_PAYLOAD) {
    return false;
  }

  const uint16_t total = lp_wire_size_from_payload(length);
  if (out_cap < total) {
    return false;
  }

  out[0] = LP_SYNC_0;
  out[1] = LP_SYNC_1;
  out[2] = LP_VERSION;
  out[3] = type;
  out[4] = seq;
  out[5] = length;

  if (length > 0 && payload != NULL) {
    memcpy(&out[6], payload, length);
  }

  const uint16_t crc = lp_crc16_ccitt(out, (uint16_t)(sizeof(lp_header_t) + length));
  out[6 + length] = (uint8_t)(crc & 0xff);
  out[7 + length] = (uint8_t)((crc >> 8) & 0xff);

  *out_len = total;
  return true;
}

static inline uint16_t lp_read_u16_le(const uint8_t* ptr) {
  return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static inline bool lp_validate_frame_bytes(const uint8_t* data, uint16_t length) {
  if (length < (sizeof(lp_header_t) + 2)) {
    return false;
  }
  if (data[0] != LP_SYNC_0 || data[1] != LP_SYNC_1 || data[2] != LP_VERSION) {
    return false;
  }

  const uint8_t payload_len = data[5];
  const uint16_t expected = lp_wire_size_from_payload(payload_len);
  if (expected != length) {
    return false;
  }

  const uint16_t actual_crc = lp_read_u16_le(&data[length - 2]);
  const uint16_t calc_crc =
      lp_crc16_ccitt(data, (uint16_t)(sizeof(lp_header_t) + payload_len));
  return actual_crc == calc_crc;
}

static inline bool lp_decode_frame(const uint8_t* data, uint16_t length,
                                   lp_frame_t* frame_out) {
  if (!lp_validate_frame_bytes(data, length)) {
    return false;
  }

  const uint8_t payload_len = data[5];
  memset(frame_out, 0, sizeof(*frame_out));
  memcpy(&frame_out->header, data, sizeof(lp_header_t));
  if (payload_len > 0) {
    memcpy(frame_out->payload, &data[6], payload_len);
  }
  frame_out->crc16 = lp_read_u16_le(&data[length - 2]);
  return true;
}

static inline lp_parse_result_t lp_stream_push(lp_stream_parser_t* parser, uint8_t byte,
                                                lp_frame_t* frame_out) {
  if (parser->index == 0) {
    if (byte == LP_SYNC_0) {
      parser->buffer[parser->index++] = byte;
    }
    return LP_PARSE_NONE;
  }

  if (parser->index == 1) {
    if (byte == LP_SYNC_1) {
      parser->buffer[parser->index++] = byte;
      return LP_PARSE_NONE;
    }
    parser->index = (byte == LP_SYNC_0) ? 1 : 0;
    if (parser->index == 1) {
      parser->buffer[0] = LP_SYNC_0;
    }
    return LP_PARSE_NONE;
  }

  if (parser->index >= sizeof(parser->buffer)) {
    lp_stream_reset(parser);
    return LP_PARSE_FRAME_BAD_HEADER;
  }

  parser->buffer[parser->index++] = byte;

  if (parser->index == sizeof(lp_header_t)) {
    if (parser->buffer[2] != LP_VERSION || parser->buffer[5] > LP_MAX_PAYLOAD) {
      lp_stream_reset(parser);
      return LP_PARSE_FRAME_BAD_HEADER;
    }
    parser->expected = lp_wire_size_from_payload(parser->buffer[5]);
  }

  if (parser->expected > 0 && parser->index == parser->expected) {
    const uint16_t frame_len = parser->expected;
    const bool ok = lp_decode_frame(parser->buffer, frame_len, frame_out);
    lp_stream_reset(parser);
    return ok ? LP_PARSE_FRAME_OK : LP_PARSE_FRAME_BAD_CRC;
  }

  return LP_PARSE_NONE;
}

// TAG_SET payload (6 bytes)
// [0] slot      (1-7, global across all zones; stable for the toy's lifetime on pad)
// [1] zone      (0=center, 1=left, 2=right)
// [2..5] toy_id_le (32-bit)
//
// When the same toy moves to a different zone, pad-esp32 reuses the same slot
// and sends a new TAG_SET with updated zone.  console-rp2040 detects the zone
// change and emits REMOVE(oldZone, figIdx) + PLACE(newZone, figIdx) so the
// game sees a move rather than two separate toys.

// LED_CMD payload
// [0] zone_id
// [1] r
// [2] g
// [3] b
// [4] effect_id
