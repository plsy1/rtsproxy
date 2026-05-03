#include "../include/rtp_pipeline.h"
#include "../include/server_config.h"
#include "../include/logger.h"
#include "../include/utils.h"
#include <arpa/inet.h>
#include <cstring>

RtpPipeline::RtpPipeline() {
    reset();
}

RtpPipeline::~RtpPipeline() = default;

void RtpPipeline::reset() {
    wait_for_keyframe_ = ServerConfig::isWaitKeyframe();
}

bool RtpPipeline::process(uint8_t *buf, size_t &len) {
    if (unlikely(len < 12 || (buf[0] & 0xC0) != 0x80)) return false;

    // 1. Startup Keyframe Sync
    if (unlikely(wait_for_keyframe_)) {
        if (check_keyframe(buf, len)) {
            Logger::debug("[Pipeline] Keyframe/PAT found, start forwarding");
            wait_for_keyframe_ = false;
        } else {
            return false; // Drop until keyframe
        }
    }

    // 2. Padding and Null Packet Stripping
    strip_rtp_padding_and_ts_null(buf, len);

    return len > 0;
}

bool RtpPipeline::get_payload_offset(const uint8_t *buf, size_t len, size_t &offset) {
    if (unlikely(len < 12 || (buf[0] & 0xC0) != 0x80)) return false;

    size_t payload_offset = 12 + (buf[0] & 0x0F) * 4;
    if (unlikely(buf[0] & 0x10)) { // Extension
        if (unlikely(payload_offset + 4 > len)) return false;
        uint16_t ext_len = ntohs(*reinterpret_cast<const uint16_t *>(buf + payload_offset + 2));
        payload_offset += 4 + 4 * ext_len;
    }
    
    if (unlikely(payload_offset > len)) return false;
    
    offset = payload_offset;
    return true;
}

void RtpPipeline::strip_rtp_padding_and_ts_null(uint8_t *buf, size_t &len) {
    size_t payload_offset = 0;
    if (!get_payload_offset(buf, len, payload_offset)) return;

    // Strip Padding & TS Null Packets
    if (unlikely(ServerConfig::isStripPadding())) {
        // 1. Strip RTP Padding
        if (unlikely(buf[0] & 0x20)) {
            uint8_t padding_len = buf[len - 1];
            if (likely(padding_len > 0 && padding_len <= (len - payload_offset))) {
                len -= padding_len;
            }
            buf[0] &= ~0x20; // Clear padding bit
        }

        // 2. Strip TS Null Packets (PID 0x1FFF)
        size_t payload_len = len - payload_offset;
        // Check if it's MPEG-TS (starts with 0x47)
        if (likely(payload_len >= 188 && buf[payload_offset] == 0x47)) {
            size_t new_payload_len = 0;
            for (size_t i = 0; i + 188 <= payload_len; i += 188) {
                uint16_t pid = ((buf[payload_offset + i + 1] & 0x1F) << 8) | buf[payload_offset + i + 2];
                if (likely(pid != 0x1FFF)) {
                    if (unlikely(new_payload_len != i)) {
                        memmove(buf + payload_offset + new_payload_len, buf + payload_offset + i, 188);
                    }
                    new_payload_len += 188;
                }
            }
            len = payload_offset + new_payload_len;
            if (new_payload_len == 0) len = 0;
            
            buf[0] &= ~0x20; // Ensure padding bit is clear if we modified the packet
        }
    }
}

bool RtpPipeline::check_keyframe(const uint8_t *buf, size_t len) {
    size_t payload_off = 0;
    if (unlikely(!get_payload_offset(buf, len, payload_off))) return false;
    
    if (likely(payload_off + 188 <= len && buf[payload_off] == 0x47)) {
        for (size_t i = 0; payload_off + i + 188 <= len; i += 188) {
            const uint8_t *ts = buf + payload_off + i;
            uint16_t pid = ((ts[1] & 0x1F) << 8) | ts[2];
            if (pid == 0) return true; // PAT found
            uint8_t afc = (ts[3] & 0x30) >> 4;
            if (afc >= 2 && ts[4] > 0 && (ts[5] & 0x40)) return true; // Keyframe found
        }
    }
    return false;
}
