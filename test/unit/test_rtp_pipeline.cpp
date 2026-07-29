// Unit tests for RtpPipeline (src/protocol/rtp_pipeline.cpp).
//
// Every RTP packet is assembled byte-by-byte here so the expectations are
// readable against RFC 3550 / ISO 13818-1 rather than against a fixture blob.
//
// No network, no filesystem, no timing: the pipeline is pure buffer surgery.
// ServerConfig is process-wide static state, so every group sets the two flags
// it reads (strip_padding, wait_keyframe) explicitly and rebuilds/resets the
// pipeline, making the groups order-independent.

#include "test_harness.h"

#include "core/server_config.h"
#include "protocol/rtp_pipeline.h"

#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Packet builders
// ---------------------------------------------------------------------------

// Bytes used as inert filler. 0xAA can never form a 00 00 01 start code and is
// never a valid PID byte pattern we care about.
static const uint8_t FILL = 0xAA;

// 12-byte (+ 4*cc) RTP header, version 2.
static std::vector<uint8_t> rtp_header(uint8_t cc = 0, bool ext = false,
                                       bool pad = false)
{
    std::vector<uint8_t> p(12, 0);
    p[0] = static_cast<uint8_t>(0x80 | (pad ? 0x20 : 0) | (ext ? 0x10 : 0) |
                                (cc & 0x0F));
    p[1] = 33;   // PT = MP2T
    p[2] = 0x00; // seq
    p[3] = 0x01;
    p.resize(static_cast<size_t>(12 + cc * 4), 0); // CSRC list
    return p;
}

// Appends a 4-byte RTP extension header carrying `ext_words` 32-bit words.
// The words themselves are NOT appended unless `with_body` is set, which is how
// the hostile-length cases are built.
static void append_ext_header(std::vector<uint8_t> &p, uint16_t ext_words,
                              bool with_body)
{
    p.push_back(0xBE);
    p.push_back(0xDE);
    p.push_back(static_cast<uint8_t>(ext_words >> 8));
    p.push_back(static_cast<uint8_t>(ext_words & 0xFF));
    if (with_body)
        p.insert(p.end(), static_cast<size_t>(ext_words) * 4, FILL);
}

static void append(std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
    a.insert(a.end(), b.begin(), b.end());
}

// A 188-byte TS packet, adaptation_field_control = 1 (payload only).
// `tag` lands at byte 4 so in-place compaction can be traced.
static std::vector<uint8_t> make_ts(uint16_t pid, uint8_t tag = FILL)
{
    std::vector<uint8_t> ts(188, FILL);
    ts[0] = 0x47;
    ts[1] = static_cast<uint8_t>((pid >> 8) & 0x1F);
    ts[2] = static_cast<uint8_t>(pid & 0xFF);
    ts[3] = 0x10; // afc = 1, cc = 0
    ts[4] = tag;
    return ts;
}

// TS packet whose payload contains a 00 00 01 start code followed by `nal`.
// `nal2` is the byte after it: the second byte of an H.265 NAL header
// (nuh_layer_id / nuh_temporal_id_plus1), inert filler otherwise.
static std::vector<uint8_t> make_ts_nal(uint16_t pid, uint8_t nal,
                                        uint8_t nal2 = FILL)
{
    std::vector<uint8_t> ts = make_ts(pid);
    ts[10] = 0x00;
    ts[11] = 0x00;
    ts[12] = 0x01;
    ts[13] = nal;
    ts[14] = nal2;
    return ts;
}

// TS packet with an adaptation field carrying the random access indicator.
static std::vector<uint8_t> make_ts_rai(uint16_t pid)
{
    std::vector<uint8_t> ts(188, FILL);
    ts[0] = 0x47;
    ts[1] = static_cast<uint8_t>((pid >> 8) & 0x1F);
    ts[2] = static_cast<uint8_t>(pid & 0xFF);
    ts[3] = 0x30; // afc = 3 (adaptation field + payload)
    ts[4] = 7;    // adaptation_field_length
    ts[5] = 0x40; // random_access_indicator
    return ts;
}

// Runs the gate on a single-TS-packet RTP datagram and reports whether the
// packet was forwarded (i.e. whether the keyframe gate opened).
static bool gate_opens_for(const std::vector<uint8_t> &ts)
{
    ServerConfig::setStripPadding(false);
    ServerConfig::setWaitKeyframe(true);
    RtpPipeline pipe;
    pipe.reset();

    std::vector<uint8_t> buf = rtp_header();
    append(buf, ts);
    size_t len = buf.size();
    return pipe.process(buf.data(), len);
}

// ---------------------------------------------------------------------------

int main()
{
    // -----------------------------------------------------------------
    SUITE("rtp_pipeline: version bits and minimum length");
    {
        ServerConfig::setStripPadding(false);
        ServerConfig::setWaitKeyframe(false);
        RtpPipeline pipe;
        pipe.reset();

        size_t off = 0;

        // Too short: an RTP header is 12 bytes minimum.
        std::vector<uint8_t> shortpkt(11, 0);
        shortpkt[0] = 0x80;
        size_t len = shortpkt.size();
        CHECK(!RtpPipeline::get_payload_offset(shortpkt.data(), len, off));
        CHECK(!pipe.process(shortpkt.data(), len));

        // Version must be 2 -> top two bits == 0b10.
        for (uint8_t v : {uint8_t(0x00), uint8_t(0x40), uint8_t(0xC0)})
        {
            std::vector<uint8_t> p = rtp_header();
            p[0] = static_cast<uint8_t>(v | (p[0] & 0x3F));
            size_t l = p.size();
            CHECK(!RtpPipeline::get_payload_offset(p.data(), l, off));
            CHECK(!pipe.process(p.data(), l));
        }

        // Version 2 with no payload is a well-formed (if empty) packet.
        std::vector<uint8_t> ok = rtp_header();
        size_t okl = ok.size();
        off = 12345;
        CHECK(RtpPipeline::get_payload_offset(ok.data(), okl, off));
        CHECK_EQ(off, size_t(12));
        CHECK(pipe.process(ok.data(), okl));
        CHECK_EQ(okl, size_t(12));
    }

    // -----------------------------------------------------------------
    SUITE("get_payload_offset: CSRC count");
    {
        size_t off = 0;

        std::vector<uint8_t> cc1 = rtp_header(1);
        cc1.insert(cc1.end(), 4, FILL);
        size_t l1 = cc1.size();
        CHECK(RtpPipeline::get_payload_offset(cc1.data(), l1, off));
        CHECK_EQ(off, size_t(16));

        std::vector<uint8_t> cc4 = rtp_header(4);
        cc4.insert(cc4.end(), 8, FILL);
        size_t l4 = cc4.size();
        CHECK(RtpPipeline::get_payload_offset(cc4.data(), l4, off));
        CHECK_EQ(off, size_t(28));

        // cc = 15 -> header is exactly 72 bytes; a 72-byte packet is legal but
        // carries no payload.
        std::vector<uint8_t> cc15 = rtp_header(15);
        size_t l15 = cc15.size();
        CHECK_EQ(l15, size_t(72));
        CHECK(RtpPipeline::get_payload_offset(cc15.data(), l15, off));
        CHECK_EQ(off, size_t(72));

        // One byte short of the declared CSRC list must be rejected.
        size_t l15short = 71;
        CHECK(!RtpPipeline::get_payload_offset(cc15.data(), l15short, off));
    }

    // -----------------------------------------------------------------
    SUITE("get_payload_offset: extension header (incl. hostile lengths)");
    {
        size_t off = 0;

        // ext with 0 words -> payload begins right after the 4-byte ext header.
        std::vector<uint8_t> e0 = rtp_header(0, true);
        append_ext_header(e0, 0, true);
        e0.insert(e0.end(), 4, FILL);
        size_t le0 = e0.size();
        CHECK(RtpPipeline::get_payload_offset(e0.data(), le0, off));
        CHECK_EQ(off, size_t(16));

        // ext with 1 word.
        std::vector<uint8_t> e1 = rtp_header(0, true);
        append_ext_header(e1, 1, true);
        e1.insert(e1.end(), 4, FILL);
        size_t le1 = e1.size();
        CHECK(RtpPipeline::get_payload_offset(e1.data(), le1, off));
        CHECK_EQ(off, size_t(20));

        // CSRC list and extension combined: 12 + 2*4 + 4 + 3*4 = 36.
        std::vector<uint8_t> ec = rtp_header(2, true);
        append_ext_header(ec, 3, true);
        ec.insert(ec.end(), 8, FILL);
        size_t lec = ec.size();
        CHECK(RtpPipeline::get_payload_offset(ec.data(), lec, off));
        CHECK_EQ(off, size_t(36));

        // Extension that consumes the whole packet: offset == len is allowed
        // (zero-length payload), not an error.
        std::vector<uint8_t> efull = rtp_header(0, true);
        append_ext_header(efull, 2, true);
        size_t lef = efull.size();
        CHECK_EQ(lef, size_t(24));
        CHECK(RtpPipeline::get_payload_offset(efull.data(), lef, off));
        CHECK_EQ(off, size_t(24));

        // Extension bit set but the 4-byte extension header is truncated: the
        // length field itself is out of bounds, so this must be rejected
        // *before* it is read.
        std::vector<uint8_t> etrunc = rtp_header(0, true);
        etrunc.push_back(0xBE);
        etrunc.push_back(0xDE);
        size_t let = etrunc.size();
        CHECK_EQ(let, size_t(14));
        CHECK(!RtpPipeline::get_payload_offset(etrunc.data(), let, off));

        // Hostile: declared extension length far exceeds the datagram. The
        // buffer is heap-allocated at its exact size, so any out-of-bounds read
        // is a real one; the packet must simply be rejected.
        for (uint16_t words : {uint16_t(0x0002), uint16_t(0x0100),
                               uint16_t(0x4000), uint16_t(0xFFFF)})
        {
            std::vector<uint8_t> h = rtp_header(0, true);
            append_ext_header(h, words, false); // body deliberately absent
            h.insert(h.end(), 4, FILL);
            size_t lh = h.size();
            CHECK_EQ(lh, size_t(20));
            off = 0xDEAD;
            CHECK(!RtpPipeline::get_payload_offset(h.data(), lh, off));
            CHECK_EQ(off, size_t(0xDEAD)); // untouched on failure
        }

        // A packet whose extension header cannot be parsed is malformed and
        // should not be relayed to the client at all.
        {
            ServerConfig::setStripPadding(true);
            ServerConfig::setWaitKeyframe(false);
            RtpPipeline pipe;
            pipe.reset();

            std::vector<uint8_t> h = rtp_header(0, true);
            append_ext_header(h, 0xFFFF, false);
            h.insert(h.end(), 4, FILL);
            size_t lh = h.size();
            XCHECK(!pipe.process(h.data(), lh),
                   "process() only validates version/length; a packet with an "
                   "unparseable extension header is forwarded verbatim");
        }
    }

    // -----------------------------------------------------------------
    SUITE("RTP padding: valid padding_len");
    {
        ServerConfig::setStripPadding(true);
        ServerConfig::setWaitKeyframe(false);
        RtpPipeline pipe;
        pipe.reset();

        // 10 payload bytes, the last 4 of which are padding.
        std::vector<uint8_t> p = rtp_header(0, false, true);
        p.insert(p.end(), 10, FILL);
        p[p.size() - 1] = 4; // padding_len
        size_t len = p.size();
        CHECK_EQ(len, size_t(22));

        CHECK(pipe.process(p.data(), len));
        CHECK_EQ(len, size_t(18));       // 22 - 4
        CHECK_EQ(p[0] & 0x20, 0);        // P bit cleared once padding is gone
        CHECK_EQ(p[0] & 0xC0, 0x80);     // version untouched

        // padding_len covering the entire payload leaves an empty payload.
        std::vector<uint8_t> q = rtp_header(0, false, true);
        q.insert(q.end(), 10, FILL);
        q[q.size() - 1] = 10;
        size_t qlen = q.size();
        CHECK(pipe.process(q.data(), qlen));
        CHECK_EQ(qlen, size_t(12));
        CHECK_EQ(q[0] & 0x20, 0);
    }

    // -----------------------------------------------------------------
    SUITE("RTP padding: malformed padding_len");
    {
        ServerConfig::setStripPadding(true);
        ServerConfig::setWaitKeyframe(false);
        RtpPipeline pipe;
        pipe.reset();

        // padding_len == 0 is illegal (RFC 3550: the last octet counts itself,
        // so it is >= 1). Nothing is stripped -- correct -- but the P bit must
        // not be cleared, or the receiver will decode the padding as payload.
        std::vector<uint8_t> z = rtp_header(0, false, true);
        z.insert(z.end(), 10, FILL);
        z[z.size() - 1] = 0;
        size_t zlen = z.size();
        CHECK(pipe.process(z.data(), zlen));
        CHECK_EQ(zlen, size_t(22)); // length correctly left alone
        CHECK_EQ(z[0] & 0x20, 0x20); // ...and the P bit still describes the packet

        // padding_len larger than the payload is equally illegal.
        std::vector<uint8_t> b = rtp_header(0, false, true);
        b.insert(b.end(), 10, FILL);
        b[b.size() - 1] = 200;
        size_t blen = b.size();
        CHECK(pipe.process(b.data(), blen));
        CHECK_EQ(blen, size_t(22)); // no under-run of the buffer
        CHECK_EQ(b[0] & 0x20, 0x20);

        // Null-packet stripping in the same datagram must not clear the P bit
        // either: the padding octets are still in there.
        std::vector<uint8_t> c = rtp_header(0, false, true);
        append(c, make_ts(0x1FFF, 0x01));
        append(c, make_ts(0x0100, 0x02));
        c[c.size() - 1] = 0; // illegal padding_len
        size_t clen = c.size();
        CHECK(pipe.process(c.data(), clen));
        CHECK_EQ(clen, size_t(12 + 188));
        CHECK_EQ(c[0] & 0x20, 0x20);

        // With stripping disabled the packet must be passed through untouched,
        // padding bit included.
        ServerConfig::setStripPadding(false);
        RtpPipeline pass;
        pass.reset();
        std::vector<uint8_t> k = rtp_header(0, false, true);
        k.insert(k.end(), 10, FILL);
        k[k.size() - 1] = 4;
        size_t klen = k.size();
        CHECK(pass.process(k.data(), klen));
        CHECK_EQ(klen, size_t(22));
        CHECK_EQ(k[0] & 0x20, 0x20);
    }

    // -----------------------------------------------------------------
    SUITE("TS null-packet (PID 0x1FFF) stripping");
    {
        ServerConfig::setStripPadding(true);
        ServerConfig::setWaitKeyframe(false);
        RtpPipeline pipe;
        pipe.reset();

        // real / null / real -> the two real packets must be compacted to the
        // front, in order.
        std::vector<uint8_t> m = rtp_header();
        append(m, make_ts(0x0100, 0x11));
        append(m, make_ts(0x1FFF, 0x22));
        append(m, make_ts(0x0101, 0x33));
        size_t mlen = m.size();
        CHECK_EQ(mlen, size_t(12 + 3 * 188));
        CHECK(pipe.process(m.data(), mlen));
        CHECK_EQ(mlen, size_t(12 + 2 * 188));
        CHECK_EQ(m[12 + 4], uint8_t(0x11));       // first real packet stays put
        CHECK_EQ(m[12 + 188 + 4], uint8_t(0x33)); // third moved into slot 2
        CHECK_EQ(m[12 + 188 + 2], uint8_t(0x01)); // ...with its PID intact
        CHECK_EQ(m[12 + 1] & 0x1F, 0x01);

        // A single real packet is untouched.
        std::vector<uint8_t> s = rtp_header();
        append(s, make_ts(0x0100, 0x44));
        size_t slen = s.size();
        CHECK(pipe.process(s.data(), slen));
        CHECK_EQ(slen, size_t(12 + 188));
        CHECK_EQ(s[12 + 4], uint8_t(0x44));

        // An all-null datagram collapses to nothing and must be dropped.
        std::vector<uint8_t> n = rtp_header();
        append(n, make_ts(0x1FFF, 0x55));
        append(n, make_ts(0x1FFF, 0x66));
        size_t nlen = n.size();
        CHECK(!pipe.process(n.data(), nlen));
        CHECK_EQ(nlen, size_t(0));

        // Not MPEG-TS (no 0x47 sync byte): leave the payload alone even though
        // it happens to be 188-byte aligned.
        std::vector<uint8_t> nt = rtp_header();
        std::vector<uint8_t> raw = make_ts(0x1FFF, 0x77);
        raw[0] = 0x48; // broken sync byte
        append(nt, raw);
        size_t ntlen = nt.size();
        CHECK(pipe.process(nt.data(), ntlen));
        CHECK_EQ(ntlen, size_t(12 + 188));

        // Stripping disabled: nulls survive.
        ServerConfig::setStripPadding(false);
        RtpPipeline keep;
        keep.reset();
        std::vector<uint8_t> d = rtp_header();
        append(d, make_ts(0x1FFF, 0x88));
        append(d, make_ts(0x0100, 0x99));
        size_t dlen = d.size();
        CHECK(keep.process(d.data(), dlen));
        CHECK_EQ(dlen, size_t(12 + 2 * 188));
        CHECK_EQ(d[12 + 4], uint8_t(0x88));
    }

    // -----------------------------------------------------------------
    SUITE("TS stripping: payload not a multiple of 188");
    {
        ServerConfig::setStripPadding(true);
        ServerConfig::setWaitKeyframe(false);
        RtpPipeline pipe;
        pipe.reset();

        std::vector<uint8_t> t = rtp_header();
        append(t, make_ts(0x0100, 0x12));
        t.insert(t.end(), 100, 0x5A); // trailing partial TS packet
        size_t tlen = t.size();
        CHECK_EQ(tlen, size_t(12 + 188 + 100));

        CHECK(pipe.process(t.data(), tlen));
        CHECK_EQ(t[12 + 4], uint8_t(0x12)); // the whole packet is preserved
        CHECK_EQ(tlen, size_t(12 + 188 + 100)); // ...and so is the partial tail

        // Same shape but with the real packet nulled out: only the null packet
        // goes, the partial tail moves down to take its place.
        std::vector<uint8_t> u = rtp_header();
        append(u, make_ts(0x1FFF, 0x13));
        u.insert(u.end(), 100, 0x5A);
        size_t ulen = u.size();
        CHECK(pipe.process(u.data(), ulen));
        CHECK_EQ(ulen, size_t(12 + 100));
        CHECK_EQ(u[12], uint8_t(0x5A));
        CHECK_EQ(u[12 + 99], uint8_t(0x5A));

        // Null packet between two real ones, plus a tail: compaction keeps the
        // order and the tail lands right behind the second real packet.
        std::vector<uint8_t> w = rtp_header();
        append(w, make_ts(0x0100, 0x14));
        append(w, make_ts(0x1FFF, 0x15));
        append(w, make_ts(0x0101, 0x16));
        w.insert(w.end(), 50, 0x5B);
        size_t wlen = w.size();
        CHECK(pipe.process(w.data(), wlen));
        CHECK_EQ(wlen, size_t(12 + 2 * 188 + 50));
        CHECK_EQ(w[12 + 4], uint8_t(0x14));
        CHECK_EQ(w[12 + 188 + 4], uint8_t(0x16));
        CHECK_EQ(w[12 + 2 * 188], uint8_t(0x5B));

        // Payload shorter than one TS packet is left alone (no 188-byte unit).
        std::vector<uint8_t> v = rtp_header();
        v.insert(v.end(), 100, 0x47);
        size_t vlen = v.size();
        CHECK(pipe.process(v.data(), vlen));
        CHECK_EQ(vlen, size_t(12 + 100));
    }

    // -----------------------------------------------------------------
    SUITE("padding + TS stripping combined");
    {
        ServerConfig::setStripPadding(true);
        ServerConfig::setWaitKeyframe(false);
        RtpPipeline pipe;
        pipe.reset();

        // 2 TS packets (one null) plus 6 padding octets.
        std::vector<uint8_t> p = rtp_header(0, false, true);
        append(p, make_ts(0x1FFF, 0x01));
        append(p, make_ts(0x0100, 0x02));
        p.insert(p.end(), 6, 0x00);
        p[p.size() - 1] = 6; // padding_len
        size_t len = p.size();
        CHECK_EQ(len, size_t(12 + 376 + 6));

        CHECK(pipe.process(p.data(), len));
        CHECK_EQ(len, size_t(12 + 188)); // padding gone, null packet gone
        CHECK_EQ(p[12 + 4], uint8_t(0x02));
        CHECK_EQ(p[0] & 0x20, 0);
    }

    // -----------------------------------------------------------------
    SUITE("keyframe gate: genuine keyframes open it");
    {
        // H.264: IDR (5), SPS (7), PPS (8). nal_ref_idc = 3 -> 0x60 | type.
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x65))); // IDR slice
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x67))); // SPS
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x68))); // PPS

        // H.265: nal type lives in bits 1..6. VPS=32, SPS=33, PPS=34, IDR_W_RADL=19.
        // The parameter sets are only accepted with a base-layer second header
        // byte (nuh_layer_id 0, nuh_temporal_id_plus1 1 -> 0x01).
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x40, 0x01))); // VPS
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x42, 0x01))); // SPS
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x44, 0x01))); // PPS
        CHECK(gate_opens_for(make_ts_nal(0x0100, 0x26))); // IDR_W_RADL

        // Adaptation-field random access indicator.
        CHECK(gate_opens_for(make_ts_rai(0x0100)));

        // Once open, the gate stays open for ordinary inter frames.
        ServerConfig::setStripPadding(false);
        ServerConfig::setWaitKeyframe(true);
        RtpPipeline pipe;
        pipe.reset();

        std::vector<uint8_t> inter = rtp_header();
        append(inter, make_ts(0x0100, 0x01));
        size_t ilen = inter.size();
        CHECK(!pipe.process(inter.data(), ilen)); // dropped before the IDR

        std::vector<uint8_t> idr = rtp_header();
        append(idr, make_ts_nal(0x0100, 0x65));
        size_t dlen = idr.size();
        CHECK(pipe.process(idr.data(), dlen));

        size_t ilen2 = inter.size();
        CHECK(pipe.process(inter.data(), ilen2)); // now forwarded

        // reset() re-arms the gate from ServerConfig.
        pipe.reset();
        size_t ilen3 = inter.size();
        CHECK(!pipe.process(inter.data(), ilen3));

        // ...and reset() with the feature off leaves the gate permanently open.
        ServerConfig::setWaitKeyframe(false);
        pipe.reset();
        size_t ilen4 = inter.size();
        CHECK(pipe.process(inter.data(), ilen4));
    }

    // -----------------------------------------------------------------
    SUITE("keyframe gate: ordinary data keeps it closed");
    {
        // Plain TS payload, no start codes, no RAI.
        CHECK(!gate_opens_for(make_ts(0x0100, 0x01)));

        // H.264 non-IDR slice with nal_ref_idc = 3 (0x61): type 1.
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0x61)));

        // H.264 SEI (6) and AUD (9) are not random access points.
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0x06)));
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0x09)));

        // Adaptation field present but the random access indicator is clear.
        {
            std::vector<uint8_t> ts = make_ts_rai(0x0100);
            ts[5] = 0x00;
            CHECK(!gate_opens_for(ts));
        }
        // Adaptation field length 0 -> nothing to read, no RAI.
        {
            std::vector<uint8_t> ts = make_ts_rai(0x0100);
            ts[4] = 0;
            CHECK(!gate_opens_for(ts));
        }

        // Not MPEG-TS at all.
        {
            std::vector<uint8_t> ts = make_ts_nal(0x0100, 0x65);
            ts[0] = 0x48; // broken sync byte
            CHECK(!gate_opens_for(ts));
        }

        // Shorter than one TS packet: nothing to scan.
        {
            ServerConfig::setStripPadding(false);
            ServerConfig::setWaitKeyframe(true);
            RtpPipeline pipe;
            pipe.reset();
            std::vector<uint8_t> p = rtp_header();
            std::vector<uint8_t> ts = make_ts_nal(0x0100, 0x65);
            p.insert(p.end(), ts.begin(), ts.begin() + 100);
            size_t len = p.size();
            CHECK(!pipe.process(p.data(), len));
            CHECK_EQ(len, size_t(112)); // len untouched when dropped
        }
    }

    // -----------------------------------------------------------------
    SUITE("keyframe gate: false keyframes are rejected");
    {
        // 0x41 is an H.264 non-IDR slice (nal_ref_idc=2, type=1) -- by far the
        // most common byte after a start code in a live stream. It also reads as
        // H.265 nal type (0x41 >> 1) & 0x3F == 32 (VPS), so the parameter-set
        // test must look at the second NAL header byte before believing it.
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0x41)));

        // 0xC0..0xDF are the MPEG-1/2 audio PES stream_ids -- they follow a
        // 00 00 01 PES start code in every audio packet and map to nal type 32
        // as well. The forbidden_zero_bit rules the whole range out.
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0xC0)));
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0xC1)));
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0xDF)));
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0xE0))); // video PES stream_id

        // Even with a plausible second byte the forbidden_zero_bit still wins.
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0xC0, 0x01)));

        // A real H.265 parameter set byte with a non base-layer second byte is
        // not enough on its own.
        CHECK(!gate_opens_for(make_ts_nal(0x0100, 0x40, 0x00)));

        // A PAT carries no video at all; joining on it means the client still
        // has to wait for the next real IDR, and meanwhile gets broken frames.
        {
            std::vector<uint8_t> pat = make_ts(0x0000, 0x01);
            CHECK(!gate_opens_for(pat));
        }

        // Same, buried behind an ordinary payload packet.
        {
            ServerConfig::setStripPadding(false);
            ServerConfig::setWaitKeyframe(true);
            RtpPipeline pipe;
            pipe.reset();
            std::vector<uint8_t> p = rtp_header();
            append(p, make_ts(0x0100, 0x01)); // ordinary payload
            append(p, make_ts(0x0000, 0x02)); // PAT
            size_t len = p.size();
            CHECK(!pipe.process(p.data(), len));
        }

        // afc = 2 is an adaptation field with no payload at all. A start code
        // pattern inside the stuffing is not video and must not be scanned.
        {
            std::vector<uint8_t> ts = make_ts(0x0100, FILL);
            ts[3] = 0x20; // afc = 2
            ts[4] = 10;   // adaptation_field_length
            ts[5] = 0x00; // no random access indicator
            ts[20] = 0x00;
            ts[21] = 0x00;
            ts[22] = 0x01;
            ts[23] = 0x65; // H.264 IDR pattern, inside the stuffing
            CHECK(!gate_opens_for(ts));
        }

        // Off-by-one at the tail of the TS payload: with afc=1 the scan window
        // is data[0..183], so a start code occupying the final four payload
        // bytes must still be examined.
        {
            std::vector<uint8_t> ts = make_ts(0x0100, FILL);
            ts[184] = 0x00;
            ts[185] = 0x00;
            ts[186] = 0x01;
            ts[187] = 0x65; // H.264 IDR
            CHECK(gate_opens_for(ts));
        }

        // Same position, but an H.265 parameter set: its second header byte is
        // past the end of the packet, so it cannot be confirmed and is ignored.
        {
            std::vector<uint8_t> ts = make_ts(0x0100, FILL);
            ts[184] = 0x00;
            ts[185] = 0x00;
            ts[186] = 0x01;
            ts[187] = 0x40; // H.265 VPS, second header byte missing
            CHECK(!gate_opens_for(ts));
        }

        // Leave the shared config in a neutral state for anything that follows.
        ServerConfig::setWaitKeyframe(false);
        ServerConfig::setStripPadding(false);
    }

    // -----------------------------------------------------------------
    SUITE("keyframe gate: bounded fallback");
    {
        // Dropping forever is worse than starting mid-GOP: a stream whose
        // keyframes the scanner cannot see (unknown codec, scrambled payload)
        // must still start playing after a bounded number of packets.
        ServerConfig::setStripPadding(false);
        ServerConfig::setWaitKeyframe(true);
        RtpPipeline pipe;
        pipe.reset();

        std::vector<uint8_t> p = rtp_header();
        append(p, make_ts(0x0100, 0x01)); // never a keyframe

        uint32_t opened_at = 0;
        for (uint32_t i = 1; i <= RtpPipeline::KEYFRAME_WAIT_LIMIT; ++i)
        {
            size_t len = p.size();
            if (pipe.process(p.data(), len))
            {
                opened_at = i;
                break;
            }
        }
        CHECK_EQ(opened_at, RtpPipeline::KEYFRAME_WAIT_LIMIT);

        // Once forced open it stays open.
        size_t len2 = p.size();
        CHECK(pipe.process(p.data(), len2));

        // reset() re-arms both the gate and the fallback counter.
        pipe.reset();
        size_t len3 = p.size();
        CHECK(!pipe.process(p.data(), len3));

        ServerConfig::setWaitKeyframe(false);
    }

    return tst::summary();
}
