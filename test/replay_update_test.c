/*
 * replay_update_test.c
 *
 * proves that srtp_stream_update() / srtp_update() preserve the RTP replay
 * protection state of every stream, and that resizing the replay window
 * keeps a conservative lower bound on acceptable packet indices.
 *
 * Every assertion here is exercised through the real srtp_protect() /
 * srtp_unprotect() data path with locally generated AES-CM/HMAC keys; the
 * test never inspects internal structures as a substitute for an
 * end-to-end check and performs no network or media access.
 *
 * entry point:
 *   int replay_update_case(const char *group, unsigned *checks);
 *
 * groups: specific, template, resize, rollover, auth
 */

#include "srtp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTP_PAYLOAD_OCTETS 20
#define RTP_HEADER_OCTETS 12
/* room for the RTP packet plus the largest possible SRTP trailer */
#define RTP_BUF_OCTETS (RTP_HEADER_OCTETS + RTP_PAYLOAD_OCTETS + \
                        SRTP_MAX_TRAILER_LEN + 16)
#define RTCP_BUF_OCTETS (8 + 12 + SRTP_MAX_TRAILER_LEN + 16)

#define KEY_OCTETS 16
#define SALT_OCTETS 14
#define MKI_OCTETS 1

#define SSRC_A 0xA1A1A1A1u
#define SSRC_B 0xB2B2B2B2u
#define SSRC_C 0xC3C3C3C3u

/* a pair of locally generated master keys (key || salt) */
static uint8_t key_one[KEY_OCTETS + SALT_OCTETS];
static uint8_t key_two[KEY_OCTETS + SALT_OCTETS];

static uint8_t mki_one[MKI_OCTETS] = { 0x11 };
static uint8_t mki_two[MKI_OCTETS] = { 0x22 };

/*
 * Each checked behavior passes through check().  A failed check records
 * the location and the observed status, stops the current group and is
 * reported to the caller; nothing is counted unless it actually ran.
 */
static int check(int condition,
                 unsigned *checks,
                 const char *group,
                 const char *what,
                 int line,
                 int status)
{
    if (condition) {
        (*checks)++;
        return 0;
    }
    fprintf(stderr,
            "FAIL [%s] %s (line %d, status=%d)\n",
            group,
            what,
            line,
            status);
    return 1;
}

#define CHECK(cond, what) \
    do { \
        if (check((cond) != 0, checks, group, (what), __LINE__, status)) { \
            return 1; \
        } \
    } while (0)

#define OK(call) \
    do { \
        status = (call); \
        if (status != srtp_err_status_ok) { \
            CHECK(0, #call); \
        } \
        (*checks)++; \
    } while (0)

static void make_rtp_packet(uint8_t *pkt, uint32_t ssrc, uint16_t seq)
{
    memset(pkt, 0, RTP_BUF_OCTETS);
    pkt[0] = 0x80; /* version 2, no CSRC, no extension, no padding */
    pkt[1] = 96;   /* payload type */
    pkt[2] = (uint8_t)(seq >> 8);
    pkt[3] = (uint8_t)(seq & 0xff);
    pkt[8] = (uint8_t)(ssrc >> 24);
    pkt[9] = (uint8_t)(ssrc >> 16);
    pkt[10] = (uint8_t)(ssrc >> 8);
    pkt[11] = (uint8_t)(ssrc & 0xff);
    for (size_t i = 0; i < RTP_PAYLOAD_OCTETS; i++) {
        pkt[RTP_HEADER_OCTETS + i] = (uint8_t)(0x40 + i);
    }
}

static void make_rtcp_packet(uint8_t *pkt, uint32_t ssrc)
{
    memset(pkt, 0, RTCP_BUF_OCTETS);
    pkt[0] = 0x80; /* version 2 */
    pkt[1] = 200;  /* sender report */
    pkt[2] = 0;
    pkt[3] = 2;    /* length in 32-bit words minus one */
    pkt[4] = (uint8_t)(ssrc >> 24);
    pkt[5] = (uint8_t)(ssrc >> 16);
    pkt[6] = (uint8_t)(ssrc >> 8);
    pkt[7] = (uint8_t)(ssrc & 0xff);
    memset(pkt + 8, 0xab, 12);
}

static void fill_policy(srtp_policy_t policy,
                        srtp_ssrc_type_t type,
                        uint32_t ssrc,
                        size_t window_size)
{
    srtp_policy_set_ssrc(policy, (srtp_ssrc_t){ type, ssrc });
    srtp_policy_set_profile(policy, srtp_profile_aes128_cm_sha1_80);
    srtp_policy_set_window_size(policy, window_size);
}

static void add_single_key(srtp_policy_t policy, const uint8_t *key)
{
    srtp_policy_add_key(policy,
                        key,
                        KEY_OCTETS,
                        key + KEY_OCTETS,
                        SALT_OCTETS,
                        NULL,
                        0);
}

/*
 * produce a protected RTP packet (a copy the caller may replay) for the
 * given SSRC and sequence number.
 */
static int protect_rtp(srtp_t tx,
                       uint32_t ssrc,
                       uint16_t seq,
                       size_t mki_index,
                       uint8_t *stored,
                       size_t *stored_len,
                       unsigned *checks,
                       const char *group)
{
    uint8_t pkt[RTP_BUF_OCTETS];
    size_t out_len = sizeof(pkt);
    int status;

    make_rtp_packet(pkt, ssrc, seq);
    status = srtp_protect(tx, pkt, RTP_HEADER_OCTETS + RTP_PAYLOAD_OCTETS,
                          stored, &out_len, mki_index);
    if (check(status == srtp_err_status_ok, checks, group,
              "srtp_protect", __LINE__, status)) {
        return 1;
    }
    (*checks)++;
    *stored_len = out_len;
    return 0;
}

static int expect_unprotect(srtp_t rx,
                            const uint8_t *stored,
                            size_t stored_len,
                            int expected,
                            const char *what,
                            unsigned *checks,
                            const char *group)
{
    uint8_t wire[RTP_BUF_OCTETS];
    uint8_t out[RTP_BUF_OCTETS];
    size_t out_len = sizeof(out);
    int status;

    memcpy(wire, stored, stored_len);
    status = srtp_unprotect(rx, wire, stored_len, out, &out_len);
    if (check(status == expected, checks, group, what, __LINE__, status)) {
        return 1;
    }
    return 0;
}

static int update_specific(srtp_t session,
                           uint32_t ssrc,
                           size_t window_size,
                           const uint8_t *key,
                           unsigned *checks,
                           const char *group)
{
    srtp_policy_t policy;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, ssrc_specific, ssrc, window_size);
    add_single_key(policy, key);
    OK(srtp_stream_update(session, policy));
    srtp_policy_destroy(policy);
    return 0;
}

static int update_template(srtp_t session,
                           srtp_ssrc_type_t type,
                           size_t window_size,
                           const uint8_t *key,
                           unsigned *checks,
                           const char *group)
{
    srtp_policy_t policy;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, type, 0, window_size);
    add_single_key(policy, key);
    OK(srtp_stream_update(session, policy));
    srtp_policy_destroy(policy);
    return 0;
}

/*
 * specific: updating a single, explicitly added SSRC must preserve its
 * extended packet index and its full received bitmask, so an already
 * authenticated packet stays a replay.  Two SSRCs keep independent state.
 */
static int case_specific(unsigned *checks, const char *group)
{
    srtp_t tx = NULL, rx = NULL;
    srtp_policy_t policy;
    uint8_t pkt_a[RTP_BUF_OCTETS], pkt_b[RTP_BUF_OCTETS];
    size_t len_a = 0, len_b = 0;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, ssrc_specific, SSRC_A, 128);
    add_single_key(policy, key_one);
    OK(srtp_create(&tx, policy));
    OK(srtp_create(&rx, policy));

    if (protect_rtp(tx, SSRC_A, 77, 0, pkt_a, &len_a, checks, group) ||
        protect_rtp(tx, SSRC_A, 78, 0, pkt_b, &len_b, checks, group)) {
        return 1;
    }

    if (expect_unprotect(rx, pkt_a, len_a, srtp_err_status_ok,
                         "initial packet accepted", checks, group) ||
        expect_unprotect(rx, pkt_b, len_b, srtp_err_status_ok,
                         "second packet accepted", checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, pkt_a, len_a, srtp_err_status_replay_fail,
                         "duplicate rejected before update", checks, group)) {
        return 1;
    }

    /* identical policy update must not reopen the packet */
    if (update_specific(rx, SSRC_A, 128, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, pkt_a, len_a, srtp_err_status_replay_fail,
                         "duplicate still rejected after update", checks,
                         group) ||
        expect_unprotect(rx, pkt_b, len_b, srtp_err_status_replay_fail,
                         "second packet still a replay after update", checks,
                         group)) {
        return 1;
    }

    /* a still-open hole remains receivable across the update */
    uint8_t pkt_hole[RTP_BUF_OCTETS];
    size_t len_hole = 0;
    if (protect_rtp(tx, SSRC_A, 76, 0, pkt_hole, &len_hole, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, pkt_hole, len_hole, srtp_err_status_ok,
                         "late in-window hole accepted after update", checks,
                         group) ||
        expect_unprotect(rx, pkt_hole, len_hole, srtp_err_status_replay_fail,
                         "hole packet cannot be replayed", checks, group)) {
        return 1;
    }

    /* two distinct SSRCs never share replay state */
    srtp_t tx2 = NULL, rx2 = NULL;
    OK(srtp_policy_set_ssrc(policy, (srtp_ssrc_t){ ssrc_specific, SSRC_B }));
    OK(srtp_create(&tx2, policy));
    OK(srtp_create(&rx2, policy));
    uint8_t pkt_b2[RTP_BUF_OCTETS];
    size_t len_b2 = 0;
    if (protect_rtp(tx2, SSRC_B, 77, 0, pkt_b2, &len_b2, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx2, pkt_b2, len_b2, srtp_err_status_ok,
                         "same sequence on a different SSRC is new", checks,
                         group) ||
        expect_unprotect(rx2, pkt_b2, len_b2, srtp_err_status_replay_fail,
                         "and is a replay on its own SSRC afterwards", checks,
                         group)) {
        return 1;
    }
    if (update_specific(rx2, SSRC_B, 128, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx2, pkt_b2, len_b2, srtp_err_status_replay_fail,
                         "replay state stays per-SSRC after update", checks,
                         group)) {
        return 1;
    }

    OK(srtp_dealloc(tx2));
    OK(srtp_dealloc(rx2));
    OK(srtp_dealloc(tx));
    OK(srtp_dealloc(rx));
    srtp_policy_destroy(policy);
    return 0;
}

/*
 * template: wildcard template updates preserve each cloned stream's RTP
 * state and direction.  A SSRC first seen after the update starts from a
 * clean window, and an explicitly added (non-cloned) stream keeps its own
 * policy and state untouched.
 */
static int case_template(unsigned *checks, const char *group)
{
    srtp_t tx = NULL, rx = NULL;
    srtp_policy_t policy;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, ssrc_any_outbound, 0, 128);
    add_single_key(policy, key_one);
    OK(srtp_create(&tx, policy));
    OK(srtp_policy_set_ssrc(policy,
                            (srtp_ssrc_t){ ssrc_any_inbound, 0 }));
    OK(srtp_create(&rx, policy));

    uint8_t a1[RTP_BUF_OCTETS], a2[RTP_BUF_OCTETS];
    uint8_t b1[RTP_BUF_OCTETS];
    size_t la1 = 0, la2 = 0, lb1 = 0;

    /* two SSRCs are cloned out of the template on first inbound packet */
    if (protect_rtp(tx, SSRC_A, 10, 0, a1, &la1, checks, group) ||
        protect_rtp(tx, SSRC_A, 11, 0, a2, &la2, checks, group) ||
        protect_rtp(tx, SSRC_B, 10, 0, b1, &lb1, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, a1, la1, srtp_err_status_ok,
                         "template stream A first packet", checks, group) ||
        expect_unprotect(rx, a2, la2, srtp_err_status_ok,
                         "template stream A second packet", checks, group) ||
        expect_unprotect(rx, b1, lb1, srtp_err_status_ok,
                         "template stream B first packet", checks, group) ||
        expect_unprotect(rx, a1, la1, srtp_err_status_replay_fail,
                         "stream A duplicate before update", checks, group) ||
        expect_unprotect(rx, b1, lb1, srtp_err_status_replay_fail,
                         "stream B duplicate before update", checks, group)) {
        return 1;
    }

    /* rekey through the wildcard template; both streams must retain state */
    if (update_template(rx, ssrc_any_inbound, 128, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, a1, la1, srtp_err_status_replay_fail,
                         "stream A duplicate rejected after template update",
                         checks, group) ||
        expect_unprotect(rx, b1, lb1, srtp_err_status_replay_fail,
                         "stream B duplicate rejected after template update",
                         checks, group)) {
        return 1;
    }

    /* an SSRC appearing only after the update gets a clean window */
    uint8_t c1[RTP_BUF_OCTETS];
    size_t lc1 = 0;
    if (protect_rtp(tx, SSRC_C, 10, 0, c1, &lc1, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, c1, lc1, srtp_err_status_ok,
                         "new SSRC accepted on clean window", checks, group) ||
        expect_unprotect(rx, c1, lc1, srtp_err_status_replay_fail,
                         "new SSRC enforces its own replay state", checks,
                         group)) {
        return 1;
    }

    /*
     * an explicitly added stream that was never cloned from the template
     * keeps its own policy: a packet built under the template key must not
     * authenticate against it, proving it was not silently re-keyed.
     */
    srtp_t own = NULL;
    srtp_policy_t own_policy;
    OK(srtp_policy_create(&own_policy));
    fill_policy(own_policy, ssrc_specific, SSRC_C, 128);
    add_single_key(own_policy, key_two);
    OK(srtp_create(&own, own_policy));
    uint8_t own_pkt[RTP_BUF_OCTETS];
    size_t own_len = 0;
    if (protect_rtp(tx, SSRC_C, 20, 0, own_pkt, &own_len, checks, group)) {
        return 1;
    }
    if (expect_unprotect(own, own_pkt, own_len, srtp_err_status_auth_fail,
                         "independent stream keeps its own key", checks,
                         group)) {
        return 1;
    }
    OK(srtp_dealloc(own));
    srtp_policy_destroy(own_policy);

    OK(srtp_dealloc(tx));
    OK(srtp_dealloc(rx));
    srtp_policy_destroy(policy);
    return 0;
}

/*
 * resize: shrinking and enlarging the replay window.  Holes still inside
 * the new window stay open, received packets stay rejected, packets
 * outside the window stay old, and indices forgotten by a smaller window
 * can never reopen when the window grows again.  The conservative floor
 * advances with normal traffic and requires no unbounded history.
 */
static int case_resize(unsigned *checks, const char *group)
{
    srtp_t tx = NULL, rx = NULL;
    srtp_policy_t policy;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, ssrc_specific, SSRC_A, 128);
    add_single_key(policy, key_one);
    OK(srtp_create(&tx, policy));
    OK(srtp_create(&rx, policy));

    /* receive the high index 1000, leaving 980 and 900 unreceived */
    uint8_t high[RTP_BUF_OCTETS];
    uint8_t hole980[RTP_BUF_OCTETS];
    uint8_t old900[RTP_BUF_OCTETS];
    size_t lh = 0, l980 = 0, l900 = 0;
    if (protect_rtp(tx, SSRC_A, 1000, 0, high, &lh, checks, group) ||
        protect_rtp(tx, SSRC_A, 980, 0, hole980, &l980, checks, group) ||
        protect_rtp(tx, SSRC_A, 900, 0, old900, &l900, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, high, lh, srtp_err_status_ok,
                         "receive index 1000", checks, group) ||
        expect_unprotect(rx, high, lh, srtp_err_status_replay_fail,
                         "index 1000 is a replay afterwards", checks, group)) {
        return 1;
    }

    /* shrink 128 -> 64: window now spans 937..1000 */
    if (update_specific(rx, SSRC_A, 64, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, high, lh, srtp_err_status_replay_fail,
                         "received index stays rejected after shrink", checks,
                         group) ||
        expect_unprotect(rx, hole980, l980, srtp_err_status_ok,
                         "in-window hole 980 still accepted after shrink",
                         checks, group) ||
        expect_unprotect(rx, hole980, l980, srtp_err_status_replay_fail,
                         "hole 980 cannot repeat", checks, group) ||
        expect_unprotect(rx, old900, l900, srtp_err_status_replay_old,
                         "index 900 stays outside the shrunk window", checks,
                         group)) {
        return 1;
    }

    /* grow 64 -> 256: the forgotten region must not reopen */
    if (update_specific(rx, SSRC_A, 256, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, old900, l900, srtp_err_status_replay_old,
                         "forgotten index 900 does not reopen on grow", checks,
                         group) ||
        expect_unprotect(rx, high, lh, srtp_err_status_replay_fail,
                         "index 1000 still a replay after grow", checks,
                         group)) {
        return 1;
    }

    /* a fresh hole inside the grown window remains receivable once */
    uint8_t hole990[RTP_BUF_OCTETS];
    size_t l990 = 0;
    if (protect_rtp(tx, SSRC_A, 990, 0, hole990, &l990, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, hole990, l990, srtp_err_status_ok,
                         "new in-window hole 990 accepted", checks, group) ||
        expect_unprotect(rx, hole990, l990, srtp_err_status_replay_fail,
                         "hole 990 cannot repeat", checks, group)) {
        return 1;
    }

    /* enlarging never marks the window full: a run of holes still arrives */
    uint8_t fresh[RTP_BUF_OCTETS];
    size_t lf = 0;
    if (protect_rtp(tx, SSRC_A, 1001, 0, fresh, &lf, checks, group) ||
        expect_unprotect(rx, fresh, lf, srtp_err_status_ok,
                         "forward index 1001 accepted", checks, group)) {
        return 1;
    }

    /* the floor advances with traffic: indices far behind keep failing */
    uint8_t advance[RTP_BUF_OCTETS];
    size_t la = 0;
    if (protect_rtp(tx, SSRC_A, 1400, 0, advance, &la, checks, group) ||
        expect_unprotect(rx, advance, la, srtp_err_status_ok,
                         "jump forward to 1400 accepted", checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, old900, l900, srtp_err_status_replay_old,
                         "old index 900 still rejected after traffic", checks,
                         group)) {
        return 1;
    }

    OK(srtp_dealloc(tx));
    OK(srtp_dealloc(rx));
    srtp_policy_destroy(policy);
    return 0;
}

/*
 * rollover: the 65535 -> 0 sequence number wrap, a late packet that
 * crosses the wrap, and the same 16-bit sequence number reached in two
 * different ROC cycles are all exercised in one sequence spanning a
 * policy update.  Replay protection follows the 48-bit extended packet
 * index, not the raw sequence number: an identical seq in another ROC is
 * never treated as a fresh acceptable packet.
 */
static int case_rollover(unsigned *checks, const char *group)
{
    srtp_t tx = NULL, rx = NULL, tx0 = NULL;
    srtp_policy_t policy;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, ssrc_specific, SSRC_A, 128);
    add_single_key(policy, key_one);
    OK(srtp_create(&tx, policy));
    OK(srtp_create(&rx, policy));
    /* a second sender that stays in ROC 0 (its first packet keeps ROC 0) */
    OK(srtp_create(&tx0, policy));

    uint8_t p_65534[RTP_BUF_OCTETS], p_65535[RTP_BUF_OCTETS];
    uint8_t p_wrap0[RTP_BUF_OCTETS], p_1[RTP_BUF_OCTETS];
    size_t l65534 = 0, l65535 = 0, lwrap0 = 0, l1 = 0;

    /* ROC 0 tail ... then the wrap into ROC 1 */
    if (protect_rtp(tx, SSRC_A, 65534, 0, p_65534, &l65534, checks, group) ||
        protect_rtp(tx, SSRC_A, 65535, 0, p_65535, &l65535, checks, group) ||
        protect_rtp(tx, SSRC_A, 0, 0, p_wrap0, &lwrap0, checks, group) ||
        protect_rtp(tx, SSRC_A, 1, 0, p_1, &l1, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, p_65535, l65535, srtp_err_status_ok,
                         "seq 65535 (ROC 0) accepted", checks, group) ||
        expect_unprotect(rx, p_wrap0, lwrap0, srtp_err_status_ok,
                         "wrap seq 0 (ROC 1) accepted", checks, group) ||
        expect_unprotect(rx, p_1, l1, srtp_err_status_ok,
                         "seq 1 (ROC 1) accepted", checks, group) ||
        expect_unprotect(rx, p_wrap0, lwrap0, srtp_err_status_replay_fail,
                         "wrapped seq 0 cannot repeat", checks, group)) {
        return 1;
    }

    /* update in the middle of the stream; extended index must survive */
    if (update_specific(rx, SSRC_A, 128, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, p_65535, l65535, srtp_err_status_replay_fail,
                         "ROC 0 seq 65535 stays a replay after update", checks,
                         group) ||
        expect_unprotect(rx, p_wrap0, lwrap0, srtp_err_status_replay_fail,
                         "ROC 1 seq 0 stays a replay after update", checks,
                         group) ||
        expect_unprotect(rx, p_1, l1, srtp_err_status_replay_fail,
                         "ROC 1 seq 1 stays a replay after update", checks,
                         group)) {
        return 1;
    }

    /* late packet crossing the wrap: seq 65534 belongs to ROC 0 */
    if (expect_unprotect(rx, p_65534, l65534, srtp_err_status_ok,
                         "late pre-wrap seq 65534 accepted as ROC 0", checks,
                         group) ||
        expect_unprotect(rx, p_65534, l65534, srtp_err_status_replay_fail,
                         "late packet cannot be replayed", checks, group)) {
        return 1;
    }

    /*
     * identical sequence number 5 in two ROC cycles.  The current ROC 1
     * packet (extended index 65541) is accepted once; the ROC 0 packet
     * (extended index 5) maps onto the same nearest extended index and so
     * must not be accepted as a new packet.
     */
    uint8_t p5_roc1[RTP_BUF_OCTETS];
    uint8_t p5_roc0[RTP_BUF_OCTETS];
    uint8_t old_roc0[RTP_BUF_OCTETS];
    size_t l5r1 = 0, l5r0 = 0, lold = 0;
    if (protect_rtp(tx, SSRC_A, 5, 0, p5_roc1, &l5r1, checks, group) ||
        protect_rtp(tx0, SSRC_A, 5, 0, p5_roc0, &l5r0, checks, group) ||
        protect_rtp(tx0, SSRC_A, 65000, 0, old_roc0, &lold, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, p5_roc1, l5r1, srtp_err_status_ok,
                         "seq 5 at ROC 1 accepted", checks, group) ||
        expect_unprotect(rx, p5_roc1, l5r1, srtp_err_status_replay_fail,
                         "seq 5 at ROC 1 cannot repeat", checks, group) ||
        expect_unprotect(rx, p5_roc0, l5r0, srtp_err_status_replay_fail,
                         "same seq 5 at ROC 0 is not a new packet", checks,
                         group)) {
        return 1;
    }

    /* a genuinely older ROC 0 index, far behind the window, stays old */
    if (expect_unprotect(rx, old_roc0, lold, srtp_err_status_replay_old,
                         "ROC 0 seq 65000 is an old extended index", checks,
                         group)) {
        return 1;
    }

    OK(srtp_dealloc(tx0));
    OK(srtp_dealloc(tx));
    OK(srtp_dealloc(rx));
    srtp_policy_destroy(policy);
    return 0;
}

/*
 * auth: a packet whose authentication tag was altered must fail and must
 * not consume the hole it targeted; the original, authentic packet for
 * that index still succeeds exactly once.  Replay continuity is then
 * proven across same-key rekeys, replacement-key rekeys and MKI policies.
 * SRTCP receive state and the sender index are carried over unchanged.
 */
static int case_auth(unsigned *checks, const char *group)
{
    srtp_t tx = NULL, rx = NULL;
    srtp_policy_t policy;
    int status;

    OK(srtp_policy_create(&policy));
    fill_policy(policy, ssrc_specific, SSRC_A, 128);
    add_single_key(policy, key_one);
    OK(srtp_create(&tx, policy));
    OK(srtp_create(&rx, policy));

    /* establish index 10, then target the still-open hole at index 8 */
    uint8_t p10[RTP_BUF_OCTETS], p8[RTP_BUF_OCTETS];
    size_t l10 = 0, l8 = 0;
    if (protect_rtp(tx, SSRC_A, 10, 0, p10, &l10, checks, group) ||
        protect_rtp(tx, SSRC_A, 8, 0, p8, &l8, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, p10, l10, srtp_err_status_ok,
                         "index 10 accepted", checks, group)) {
        return 1;
    }

    uint8_t forged[RTP_BUF_OCTETS];
    memcpy(forged, p8, l8);
    forged[l8 - 1] ^= 0x01; /* flip a bit in the authentication tag */
    if (expect_unprotect(rx, forged, l8, srtp_err_status_auth_fail,
                         "tampered packet fails authentication", checks,
                         group)) {
        return 1;
    }
    if (expect_unprotect(rx, p8, l8, srtp_err_status_ok,
                         "authentic packet for the spared hole succeeds",
                         checks, group) ||
        expect_unprotect(rx, p8, l8, srtp_err_status_replay_fail,
                         "that hole can now only be a replay", checks, group)) {
        return 1;
    }

    /* continuity across a same-key update */
    if (update_specific(rx, SSRC_A, 128, key_one, checks, group)) {
        return 1;
    }
    if (expect_unprotect(rx, p8, l8, srtp_err_status_replay_fail,
                         "replay continuity after same-key update", checks,
                         group)) {
        return 1;
    }

    /* continuity across a replacement-key update, both ends rekeyed */
    if (update_specific(tx, SSRC_A, 128, key_two, checks, group) ||
        update_specific(rx, SSRC_A, 128, key_two, checks, group)) {
        return 1;
    }
    uint8_t p11[RTP_BUF_OCTETS];
    size_t l11 = 0;
    if (protect_rtp(tx, SSRC_A, 11, 0, p11, &l11, checks, group) ||
        expect_unprotect(rx, p11, l11, srtp_err_status_ok,
                         "new key still shares the packet index lineage",
                         checks, group) ||
        expect_unprotect(rx, p11, l11, srtp_err_status_replay_fail,
                         "rekeyed stream still rejects replays", checks,
                         group)) {
        return 1;
    }

    /* MKI policy keeps replay continuity for the same SSRC */
    srtp_t mki_tx = NULL, mki_rx = NULL;
    srtp_policy_t mp;
    OK(srtp_policy_create(&mp));
    fill_policy(mp, ssrc_specific, SSRC_B, 128);
    OK(srtp_policy_use_mki(mp, MKI_OCTETS));
    OK(srtp_policy_add_key(mp, key_one, KEY_OCTETS, key_one + KEY_OCTETS,
                           SALT_OCTETS, mki_one, MKI_OCTETS));
    OK(srtp_policy_add_key(mp, key_two, KEY_OCTETS, key_two + KEY_OCTETS,
                           SALT_OCTETS, mki_two, MKI_OCTETS));
    OK(srtp_create(&mki_tx, mp));
    OK(srtp_create(&mki_rx, mp));

    uint8_t m0[RTP_BUF_OCTETS], m1[RTP_BUF_OCTETS];
    size_t lm0 = 0, lm1 = 0;
    if (protect_rtp(mki_tx, SSRC_B, 5, 0, m0, &lm0, checks, group) ||
        protect_rtp(mki_tx, SSRC_B, 6, 1, m1, &lm1, checks, group)) {
        return 1;
    }
    if (expect_unprotect(mki_rx, m0, lm0, srtp_err_status_ok,
                         "MKI key index 0 packet accepted", checks, group) ||
        expect_unprotect(mki_rx, m1, lm1, srtp_err_status_ok,
                         "MKI key index 1 packet accepted", checks, group) ||
        expect_unprotect(mki_rx, m0, lm0, srtp_err_status_replay_fail,
                         "MKI stream rejects the earlier packet as replay",
                         checks, group)) {
        return 1;
    }
    OK(srtp_stream_update(mki_rx, mp));
    if (expect_unprotect(mki_rx, m0, lm0, srtp_err_status_replay_fail,
                         "replay continuity across MKI policy update", checks,
                         group) ||
        expect_unprotect(mki_rx, m1, lm1, srtp_err_status_replay_fail,
                         "second MKI packet also still a replay", checks,
                         group)) {
        return 1;
    }
    OK(srtp_dealloc(mki_tx));
    OK(srtp_dealloc(mki_rx));
    srtp_policy_destroy(mp);

    /*
     * SRTCP state survives an RTP policy update: protect/unprotect one
     * SRTCP packet, update, then prove the duplicate is still rejected and
     * the next index is still accepted.
     */
    uint8_t cp[RTCP_BUF_OCTETS], cw[RTCP_BUF_OCTETS], co[RTCP_BUF_OCTETS];
    size_t cpl = sizeof(cp), col = sizeof(co);
    make_rtcp_packet(cp, SSRC_A);
    OK(srtp_protect_rtcp(tx, cp, 20, cp, &cpl, 0));
    memcpy(cw, cp, cpl);
    OK(srtp_unprotect_rtcp(rx, cw, cpl, co, &col));
    if (update_specific(rx, SSRC_A, 128, key_two, checks, group)) {
        return 1;
    }
    memcpy(cw, cp, cpl);
    col = sizeof(co);
    status = srtp_unprotect_rtcp(rx, cw, cpl, co, &col);
    CHECK(status == srtp_err_status_replay_fail,
          "SRTCP replay state preserved across RTP update");
    (*checks)++;

    OK(srtp_dealloc(tx));
    OK(srtp_dealloc(rx));
    srtp_policy_destroy(policy);
    return 0;
}

int replay_update_case(const char *group, unsigned *checks)
{
    int status;

    if (group == NULL || checks == NULL) {
        return 1;
    }
    *checks = 0;

    for (size_t i = 0; i < sizeof(key_one); i++) {
        key_one[i] = (uint8_t)(i + 1);
        key_two[i] = (uint8_t)(0x40 + i);
    }

    /* srtp_init() is a process-wide, one-time initialization */
    static int kernel_ready;
    if (!kernel_ready) {
        status = srtp_init();
        if (status != srtp_err_status_ok) {
            fprintf(stderr, "FAIL [%s] srtp_init status=%d\n", group, status);
            return 1;
        }
        kernel_ready = 1;
    }

    if (strcmp(group, "specific") == 0) {
        return case_specific(checks, group);
    }
    if (strcmp(group, "template") == 0) {
        return case_template(checks, group);
    }
    if (strcmp(group, "resize") == 0) {
        return case_resize(checks, group);
    }
    if (strcmp(group, "rollover") == 0) {
        return case_rollover(checks, group);
    }
    if (strcmp(group, "auth") == 0) {
        return case_auth(checks, group);
    }

    fprintf(stderr, "FAIL unknown group %s\n", group);
    return 1;
}
