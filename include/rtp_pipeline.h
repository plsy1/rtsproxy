#pragma once
#include <cstdint>
#include <cstddef>

class RtpPipeline {
public:
    RtpPipeline();
    ~RtpPipeline();

    // Processes an RTP packet.
    // Returns true if the packet should be forwarded, false if it should be dropped.
    // 'len' may be modified if padding or null packets are stripped.
    bool process(uint8_t *buf, size_t &len);

    // Helper to get the offset of the payload (skips RTP header and extensions)
    static bool get_payload_offset(const uint8_t *buf, size_t len, size_t &offset);

    // Reset the pipeline state (e.g. for a new stream/play)
    void reset();

private:
    void strip_rtp_padding_and_ts_null(uint8_t *buf, size_t &len);
    bool check_keyframe(const uint8_t *buf, size_t len);

    bool wait_for_keyframe_;
};
