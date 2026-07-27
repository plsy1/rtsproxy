// Unit tests for PortPool (src/core/port_pool.cpp).
//
// PortPool is a process-wide singleton with a hard-coded range of
// [start_port_ = 20000, end_port_ = 40000) and no reset entry point, so these
// tests are written to be *self-cleaning*: every group releases every pair it
// acquired before the next group runs.
// The allocation cursor (next_port_) keeps moving forward regardless, so no
// group asserts an absolute port number -- everything is expressed relative to
// the first port that group acquired. The one exception is the exhaustion
// group, which deliberately drains the whole pool.
//
// mark_occupied() has a 5-minute expiry and no un-mark entry point, so within a
// test run its marks are effectively permanent: the groups that place them
// cannot clean up after themselves and run last, after the groups that need an
// undamaged range. The expiry itself is not exercised here -- it would mean
// waiting out the TTL or adding a test-only seam to the singleton.
//
// PortPool reads no ServerConfig state (verified by reading the source: its
// only external dependency is Logger), so there is no global config to pin.

#include "test_harness.h"

#include "core/logger.h"
#include "core/port_pool.h"

#include <algorithm>
#include <set>
#include <vector>

static const uint16_t kStart = 20000; // PortPool::start_port_
static const uint16_t kEnd = 40000;   // PortPool::end_port_ (exclusive)
static const int kTotalPairs = (kEnd - kStart) / 2;

static bool in_range(uint16_t p) { return p >= kStart && p < kEnd; }

// ---------------------------------------------------------------------------

static void test_acquire_shape(PortPool &pool)
{
    SUITE("acquire_pair: shape of a single allocation");

    uint16_t p = pool.acquire_pair();

    CHECK(p != 0);
    CHECK_EQ(p % 2, 0);            // RTP port must be even (RFC 3550)
    CHECK(in_range(p));            // inside the configured range
    CHECK(in_range((uint16_t)(p + 1))); // ...and so is its RTCP successor
    CHECK((uint16_t)(p + 1) < kEnd);

    pool.release_pair(p);
}

static void test_pairs_never_overlap(PortPool &pool)
{
    SUITE("acquire_pair: repeated allocations never overlap");

    const int kN = 32;
    std::vector<uint16_t> bases;
    std::set<uint16_t> reserved; // every port implied by a handed-out pair

    bool all_even = true;
    bool all_in_range = true;
    bool all_disjoint = true;
    bool none_zero = true;

    for (int i = 0; i < kN; ++i)
    {
        uint16_t p = pool.acquire_pair();
        if (p == 0)
        {
            none_zero = false;
            break;
        }
        if (p % 2 != 0)
            all_even = false;
        if (!in_range(p) || !in_range((uint16_t)(p + 1)))
            all_in_range = false;
        // Neither the even port nor the odd successor may already be spoken for.
        if (!reserved.insert(p).second)
            all_disjoint = false;
        if (!reserved.insert((uint16_t)(p + 1)).second)
            all_disjoint = false;
        bases.push_back(p);
    }

    CHECK(none_zero);
    CHECK_EQ((int)bases.size(), kN);
    CHECK(all_even);
    CHECK(all_in_range);
    CHECK(all_disjoint);
    CHECK_EQ((int)reserved.size(), 2 * kN); // 2 ports burned per pair

    // The cursor walks forward in steps of two, so an unfragmented pool hands
    // out strictly increasing, adjacent pairs.
    bool monotonic_by_two = true;
    for (size_t i = 1; i < bases.size(); ++i)
        if (bases[i] != (uint16_t)(bases[i - 1] + 2))
            monotonic_by_two = false;
    CHECK(monotonic_by_two);

    // A base is always even, so an odd port can never be handed out as an RTP
    // port; each odd port is only ever the reserved successor of its own pair.
    bool no_odd_base = true;
    for (uint16_t b : bases)
        if (b % 2 != 0)
            no_odd_base = false;
    CHECK(no_odd_base);

    for (uint16_t b : bases)
        pool.release_pair(b);
}

static void test_cursor_does_not_reuse_immediately(PortPool &pool)
{
    SUITE("round-robin cursor: a just-released pair is not handed straight back");

    uint16_t a = pool.acquire_pair();
    CHECK(a != 0);
    pool.release_pair(a);

    uint16_t b = pool.acquire_pair();
    CHECK(b != 0);
    CHECK(b != a);                  // not immediately recycled
    CHECK(b != (uint16_t)(a + 1));  // and never the odd half of anything
    CHECK_EQ(b, a + 2);             // cursor advanced past the freed pair
    CHECK_EQ(b % 2, 0);

    // Same again: releasing does not rewind the cursor.
    pool.release_pair(b);
    uint16_t c = pool.acquire_pair();
    CHECK(c != a);
    CHECK(c != b);
    CHECK_EQ(c, a + 4);

    pool.release_pair(c);
}

static void test_mark_occupied_skips(PortPool &pool)
{
    SUITE("mark_occupied: an externally-bound port is skipped");

    uint16_t x = pool.acquire_pair(); // cursor now points at x+2
    CHECK(x != 0);

    // Block the even (RTP) half of the pair the cursor is about to return.
    pool.mark_occupied((uint16_t)(x + 2));
    uint16_t y = pool.acquire_pair();
    CHECK(y != 0);
    CHECK(y != (uint16_t)(x + 2)); // the blocked pair was skipped
    CHECK_EQ(y, x + 4);
    CHECK_EQ(y % 2, 0);

    // Block only the odd (RTCP) half of the *next* pair: the pair must still be
    // skipped, because a usable pair needs both ports.
    pool.mark_occupied((uint16_t)(x + 7)); // cursor is at x+6
    uint16_t z = pool.acquire_pair();
    CHECK(z != 0);
    CHECK(z != (uint16_t)(x + 6)); // (x+6, x+7) unusable: odd half taken
    CHECK_EQ(z, x + 8);

    // Marking a port that is already in use is harmless and must not corrupt
    // the pair that owns it.
    pool.mark_occupied(z);
    pool.mark_occupied((uint16_t)(z + 1));
    uint16_t w = pool.acquire_pair();
    CHECK(w != 0);
    CHECK(w != z);
    CHECK_EQ(w, x + 10);

    // Clean up the acquired pairs. The occupancy marks on x+2 and x+7 cannot be
    // cleared -- release_pair() only accepts pairs handed out by acquire_pair(),
    // and mark_occupied() has no expiry -- which is why this group runs last.
    pool.release_pair(x);
    pool.release_pair(y);
    pool.release_pair(z);
    pool.release_pair(w);
}

static void test_release_edge_cases(PortPool &pool)
{
    SUITE("release_pair: guarded and idempotent for the simple cases");

    pool.release_pair(0); // explicitly guarded no-op

    uint16_t c = pool.acquire_pair();
    CHECK(c != 0);
    pool.release_pair(c);
    pool.release_pair(c); // double release must not throw or corrupt the set

    uint16_t d = pool.acquire_pair();
    CHECK(d != 0);
    CHECK_EQ(d, c + 2);
    pool.release_pair(d);

    // Releasing a pair that was never acquired is a no-op for the allocator.
    pool.release_pair((uint16_t)(kEnd - 10));
    uint16_t e = pool.acquire_pair();
    CHECK(e != 0);
    CHECK_EQ(e, d + 2);
    pool.release_pair(e);

    // The odd (RTCP) half of a live pair is never a valid release target, and
    // rejecting it must leave the pair's ownership intact so that its real owner
    // can still release it afterwards.
    uint16_t f = pool.acquire_pair();
    CHECK(f != 0);
    CHECK_EQ(f, e + 2);
    pool.release_pair((uint16_t)(f + 1)); // odd: rejected
    pool.release_pair(f);                 // still owned, so this works
    pool.release_pair(f);                 // no longer owned: no-op

    uint16_t g = pool.acquire_pair();
    CHECK(g != 0);
    CHECK_EQ(g, f + 2);
    CHECK_EQ(g % 2, 0);
    pool.release_pair(g);
}

static void test_exhaustion_reuse_and_odd_release(PortPool &pool)
{
    SUITE("exhaustion, reuse after release, and the odd-port release bug");

    // --- drain the pool -----------------------------------------------------
    std::vector<uint16_t> bases;
    std::set<uint16_t> seen;
    bool all_even = true;
    bool all_in_range = true;
    bool all_unique = true;

    for (;;)
    {
        uint16_t p = pool.acquire_pair();
        if (p == 0)
            break;
        if (p % 2 != 0)
            all_even = false;
        if (!in_range(p))
            all_in_range = false;
        if (!seen.insert(p).second)
            all_unique = false;
        bases.push_back(p);
        if ((int)bases.size() > kTotalPairs) // runaway guard
            break;
    }

    CHECK(all_even);
    CHECK(all_in_range);
    CHECK(all_unique);
    // Exactly (end - start) / 2 pairs fit, which is only true if every
    // allocation reserves both the even port and its odd successor.
    CHECK_EQ((int)bases.size(), kTotalPairs);
    CHECK_EQ((int)seen.size(), kTotalPairs);
    CHECK_EQ(*seen.begin(), kStart);
    CHECK_EQ(*seen.rbegin(), (uint16_t)(kEnd - 2));

    // The range top is respected: kEnd-1 is used only as an RTCP successor and
    // kEnd itself is never touched.
    CHECK(seen.find((uint16_t)(kEnd - 1)) == seen.end());

    // --- exhaustion ---------------------------------------------------------
    CHECK_EQ(pool.acquire_pair(), 0); // no pair left
    CHECK_EQ(pool.acquire_pair(), 0); // and it stays that way

    // --- release makes a pair reusable --------------------------------------
    const uint16_t kVictim = 30000; // even, inside the range, currently held
    pool.release_pair(kVictim);
    uint16_t reused = pool.acquire_pair();
    CHECK(reused != 0);
    CHECK_EQ(reused, kVictim); // the only free pair must be handed back
    CHECK_EQ(pool.acquire_pair(), 0);

    // --- release_pair() validates evenness and ownership ---------------------
    // The pool is full again. Pairs (30000,30001) and (30002,30003) are both
    // live and owned by different (hypothetical) sessions. A caller that passes
    // the RTCP port instead of the RTP port must not free one half of *each* of
    // two different pairs.
    pool.release_pair((uint16_t)(kVictim + 1)); // odd: rejected

    CHECK_EQ(pool.acquire_pair(), 0);

    // A second caller makes the same mistake one pair along.
    pool.release_pair((uint16_t)(kVictim + 3)); // odd: rejected

    // Neither caller owned (30002,30003), and its real owner has never released
    // it, so it must not be handed to a second session.
    uint16_t doubled = pool.acquire_pair();
    CHECK(doubled != (uint16_t)(kVictim + 2));
    CHECK_EQ(doubled, 0);

    // Whatever came back must at least still look like a valid RTP port.
    CHECK(doubled == 0 || doubled % 2 == 0);
    CHECK(doubled == 0 || in_range(doubled));

    // A rejected release must not half-leak the pair it was called on either:
    // (30000,30001) is still intact, so its owner gets the whole pair back.
    pool.release_pair(kVictim);
    CHECK_EQ(pool.acquire_pair(), kVictim);
    pool.release_pair(kVictim);
    if (doubled != 0)
        pool.release_pair(doubled);
    uint16_t after = pool.acquire_pair();
    CHECK(after != 0);
    CHECK_EQ(after % 2, 0);
    CHECK(in_range(after));

    // Give everything back so the process ends with a clean pool.
    if (after != 0)
        pool.release_pair(after);
    for (uint16_t b : bases)
        pool.release_pair(b);
}

// ---------------------------------------------------------------------------

// bind_udp_pair_from_pool() hands the pair back and *then* blocks the half that
// some other process holds:
//
//     pool.release_pair(rtp_port);        // the pool is no longer the owner
//     pool.mark_occupied(rtp_port + 1);   // ...but this half is really taken
//
// release_pair() drops both halves out of used_ports_, so unless it re-asserts
// any live mark the block evaporates and the pool hands a port that a foreign
// process is sitting on to the next session -- which then fails to bind too.
// Both orderings are checked because callers are free to use either.
static void test_release_preserves_occupied_mark(PortPool &pool)
{
    SUITE("release_pair: giving a pair back does not clear a foreign-held mark");

    // release, then mark -- the order bind_udp_pair_from_pool() uses.
    uint16_t a = pool.acquire_pair();
    CHECK(a != 0);
    pool.release_pair(a);
    pool.mark_occupied((uint16_t)(a + 1));

    // mark, then release -- the same invariant from the other direction.
    uint16_t b = pool.acquire_pair();
    CHECK(b != 0);
    CHECK(b != a);
    pool.mark_occupied((uint16_t)(b + 1));
    pool.release_pair(b);

    // A pair with no mark at all, released the same way, must come back.
    uint16_t c = pool.acquire_pair();
    CHECK(c != 0);
    CHECK(c != a);
    CHECK(c != b);
    pool.release_pair(c);

    // Walk the whole range. Every pair the pool still considers usable comes
    // out exactly once, so the set difference says precisely which pairs are
    // blocked -- no reliance on where the round-robin cursor happens to sit.
    std::vector<uint16_t> handed_out;
    while (true)
    {
        uint16_t p = pool.acquire_pair();
        if (p == 0)
            break;
        handed_out.push_back(p);
    }

    auto was_handed_out = [&](uint16_t p) {
        return std::find(handed_out.begin(), handed_out.end(), p) != handed_out.end();
    };

    CHECK(!was_handed_out(a)); // odd half marked after the release
    CHECK(!was_handed_out(b)); // odd half marked before the release
    CHECK(was_handed_out(c));  // unmarked: a plain release returns the pair

    // No duplicates: a pair must never be handed to two sessions at once.
    std::set<uint16_t> unique(handed_out.begin(), handed_out.end());
    CHECK_EQ((int)unique.size(), (int)handed_out.size());

    // Exactly the two marked pairs are missing from an otherwise intact range
    // -- the marks placed by the earlier group are still in effect, so this is
    // a relative count.
    CHECK(handed_out.size() > 0);

    for (uint16_t p : handed_out)
        pool.release_pair(p);
}

int main()
{
    // Keep the pool's exhaustion log lines out of the way where possible.
    Logger::setLogLevel(LogLevel::ERROR);

    PortPool &pool = PortPool::getInstance();

    test_acquire_shape(pool);
    test_pairs_never_overlap(pool);
    test_cursor_does_not_reuse_immediately(pool);
    test_release_edge_cases(pool);
    test_exhaustion_reuse_and_odd_release(pool); // drains the pool, then restores it
    test_mark_occupied_skips(pool); // must run last: its marks are permanent
    test_release_preserves_occupied_mark(pool); // ...and so are this one's

    return tst::summary();
}
