/*
 * Regression tests for current/next collection lanes, gap handling and
 * decoder lifetime.  Uses only the public attach/packet_push/callback API.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvbpsi.h"
#include "psi.h"
#include "descriptor.h"
#include "demux.h"
#include "tables/pat.h"
#include "tables/pmt.h"
#include "tables/sdt.h"
#include "tables/nit.h"
#include "task_support/check.h"

/*
 * TASK_CHECK returns 1 from the enclosing function, which only works in the
 * int-returning cases.  Helpers use VERIFY, which records the failure in a
 * per-case flag the case returns at the end.
 */
static int g_failed;
#define VERIFY(x) do { if (!task_check(!!(x), #x, __FILE__, __LINE__)) g_failed = 1; } while (0)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct fixture_s fixture_t;
struct fixture_s
{
    dvbpsi_psi_section_t *p_sections;
    size_t n_sections;
};

static void fixture_reset(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
}

static void fixture_free(fixture_t *f)
{
    dvbpsi_DeletePSISections(f->p_sections);
    f->p_sections = NULL;
    f->n_sections = 0;
}

/* Take ownership of a generated section list. */
static void fixture_set(fixture_t *f, dvbpsi_psi_section_t *p_sections)
{
    fixture_free(f);
    f->p_sections = p_sections;
    while (p_sections)
    {
        f->n_sections++;
        p_sections = p_sections->p_next;
    }
}

static const dvbpsi_psi_section_t *fixture_section(const fixture_t *f, size_t i)
{
    const dvbpsi_psi_section_t *p = f->p_sections;
    while (i-- && p)
        p = p->p_next;
    return p;
}

#define PACKET_PAYLOAD 183

/*
 * Push one complete section as one TS packet.  The section list is only used
 * for its bytes; the caller still owns it.
 */
static void push_section(dvbpsi_t *p, const dvbpsi_psi_section_t *s,
                         uint8_t *counter)
{
    size_t len = 3 + s->i_length;
    VERIFY(len <= PACKET_PAYLOAD);
    uint8_t packet[188];
    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = 0x40;
    packet[2] = 0x00;
    packet[3] = 0x10 | (*counter & 0x0f);
    packet[4] = 0x00;
    memcpy(packet + 5, s->p_data, len);
    VERIFY(dvbpsi_packet_push(p, packet));
    *counter = (uint8_t)((*counter + 1) & 0x0f);
}

/* Re-stamp a generated section as a later section of a bigger table. */
static void restamp(dvbpsi_psi_section_t *s, uint8_t i_number,
                    uint8_t i_last_number)
{
    s->i_number = i_number;
    s->i_last_number = i_last_number;
    s->p_data[6] = i_number;
    s->p_data[7] = i_last_number;
    dvbpsi_CalculateCRC32(s);
}

/* ------------------------------------------------------------------------- */
/* PAT callback capture                                                      */
/* ------------------------------------------------------------------------- */

#define MAX_EVENTS 32

typedef struct pat_event_s
{
    uint16_t ts_id;
    uint8_t version;
    bool current_next;
    uint16_t program_number;
    uint16_t pid;
} pat_event_t;

typedef struct pat_sink_s
{
    pat_event_t events[MAX_EVENTS];
    size_t count;
} pat_sink_t;

static void pat_on_table(void *p_data, dvbpsi_pat_t *p_pat)
{
    pat_sink_t *s = p_data;
    if (s->count < MAX_EVENTS)
    {
        pat_event_t *e = &s->events[s->count++];
        e->ts_id = p_pat->i_ts_id;
        e->version = p_pat->i_version;
        e->current_next = p_pat->b_current_next;
        if (p_pat->p_first_program)
        {
            e->program_number = p_pat->p_first_program->i_number;
            e->pid = p_pat->p_first_program->i_pid;
        }
        else
        {
            e->program_number = 0;
            e->pid = 0;
        }
    }
    dvbpsi_pat_delete(p_pat);
}

static dvbpsi_psi_section_t *make_pat(uint16_t ts_id, uint8_t version,
                                      bool current_next, uint16_t program,
                                      uint16_t pid)
{
    dvbpsi_t *enc = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    VERIFY(enc != NULL);
    dvbpsi_pat_t *p_pat = dvbpsi_pat_new(ts_id, version, current_next);
    VERIFY(p_pat != NULL);
    VERIFY(dvbpsi_pat_program_add(p_pat, program, pid) != NULL);
    dvbpsi_psi_section_t *s = dvbpsi_pat_sections_generate(enc, p_pat, 253);
    dvbpsi_pat_delete(p_pat);
    dvbpsi_delete(enc);
    return s;
}

/* ------------------------------------------------------------------------- */
/* PMT callback capture                                                      */
/* ------------------------------------------------------------------------- */

typedef struct pmt_event_s
{
    uint16_t program_number;
    uint8_t version;
    bool current_next;
    uint16_t first_pid;
} pmt_event_t;

typedef struct pmt_sink_s
{
    pmt_event_t events[MAX_EVENTS];
    size_t count;
} pmt_sink_t;

static void pmt_on_table(void *p_data, dvbpsi_pmt_t *p_pmt)
{
    pmt_sink_t *s = p_data;
    if (s->count < MAX_EVENTS)
    {
        pmt_event_t *e = &s->events[s->count++];
        e->program_number = p_pmt->i_program_number;
        e->version = p_pmt->i_version;
        e->current_next = p_pmt->b_current_next;
        e->first_pid = p_pmt->p_first_es ? p_pmt->p_first_es->i_pid : 0;
    }
    dvbpsi_pmt_delete(p_pmt);
}

/* ------------------------------------------------------------------------- */
/* SDT capture                                                               */
/* ------------------------------------------------------------------------- */

typedef struct sdt_event_s
{
    uint8_t table_id;
    uint16_t extension;
    uint8_t version;
    bool current_next;
    uint16_t network_id;
    uint16_t service_id;
} sdt_event_t;

typedef struct sdt_sink_s
{
    sdt_event_t events[MAX_EVENTS];
    size_t count;
    bool delete_in_callback;
} sdt_sink_t;

static void nit_on_table(void *p_data, dvbpsi_nit_t *p_nit)
{
    (void)p_data;
    dvbpsi_nit_delete(p_nit);
}

static void sdt_on_table(void *p_data, dvbpsi_sdt_t *p_sdt)
{
    sdt_sink_t *s = p_data;
    if (s->count < MAX_EVENTS)
    {
        sdt_event_t *e = &s->events[s->count++];
        e->table_id = p_sdt->i_table_id;
        e->extension = p_sdt->i_extension;
        e->version = p_sdt->i_version;
        e->current_next = p_sdt->b_current_next;
        e->network_id = p_sdt->i_network_id;
        e->service_id = p_sdt->p_first_service ? p_sdt->p_first_service->i_service_id : 0;
    }
    dvbpsi_sdt_delete(p_sdt);
}

static dvbpsi_psi_section_t *make_sdt(uint8_t table_id, uint16_t ts_id,
                                      uint8_t version, bool current_next,
                                      uint16_t network_id, uint16_t service)
{
    dvbpsi_t *enc = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    VERIFY(enc != NULL);
    dvbpsi_sdt_t *p_sdt = dvbpsi_sdt_new(table_id, ts_id, version, current_next,
                                         network_id);
    VERIFY(p_sdt != NULL);
    VERIFY(dvbpsi_sdt_service_add(p_sdt, service, false, true, 4, false));
    dvbpsi_psi_section_t *s = dvbpsi_sdt_sections_generate(enc, p_sdt);
    dvbpsi_sdt_delete(p_sdt);
    dvbpsi_delete(enc);
    return s;
}

/*
 * Create a two-section fixture from a single generated section: section 0 is
 * the copy itself, section 1 is a re-stamped copy with a different payload.
 */
static dvbpsi_psi_section_t *clone_section(const dvbpsi_psi_section_t *s)
{
    dvbpsi_psi_section_t *c = dvbpsi_NewPSISection(1024);
    VERIFY(c != NULL);
    c->i_table_id = s->i_table_id;
    c->b_syntax_indicator = s->b_syntax_indicator;
    c->b_private_indicator = s->b_private_indicator;
    c->i_length = s->i_length;
    c->i_extension = s->i_extension;
    c->i_version = s->i_version;
    c->b_current_next = s->b_current_next;
    c->i_number = s->i_number;
    c->i_last_number = s->i_last_number;
    memcpy(c->p_data, s->p_data, 3 + s->i_length);
    c->p_payload_start = c->p_data + (s->p_payload_start - s->p_data);
    c->p_payload_end = c->p_data + (s->p_payload_end - s->p_data);
    return c;
}

/* ------------------------------------------------------------------------- */
/* PAT fixture with two sections                                             */
/* ------------------------------------------------------------------------- */

static dvbpsi_psi_section_t *make_pat_two(uint16_t ts_id, uint8_t version,
                                          bool current_next, uint16_t base_pid)
{
    dvbpsi_psi_section_t *result = make_pat(ts_id, version, current_next,
                                            100, base_pid);
    VERIFY(result != NULL);
    dvbpsi_psi_section_t *second = clone_section(result);
    restamp(second, 1, 1);
    restamp(result, 0, 1);
    result->p_next = second;
    return result;
}

/* ------------------------------------------------------------------------- */
/* lanes                                                                     */
/* ------------------------------------------------------------------------- */

/* current0 must not complete with next1 */
static int case_current0_next1_no_callback(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t f; fixture_reset(&f);
    fixture_set(&f, make_pat_two(7, 3, true, 100));
    /* Turn section 0 into a current section and section 1 into next. */
    TASK_CHECK(fixture_section(&f, 0) != NULL && fixture_section(&f, 1) != NULL);
    const dvbpsi_psi_section_t *s0 = fixture_section(&f, 0);
    uint8_t counter = 0;
    push_section(rx, s0, &counter);
    TASK_CHECK(sink.count == 0);

    /* section 1 (next) pushed as a next section */
    fixture_t n; fixture_reset(&n);
    fixture_set(&n, make_pat_two(7, 3, false, 200));
    push_section(rx, fixture_section(&n, 1), &counter);
    TASK_CHECK(sink.count == 0);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&f);
    fixture_free(&n);
    return 0;
}

/* both lanes complete independently and rotations are suppressed */
static int case_lanes_both_complete_and_dedupe(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));
    fixture_t nxt; fixture_reset(&nxt);
    fixture_set(&nxt, make_pat_two(7, 3, false, 200));

    uint8_t counter = 0;
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&nxt, 0), &counter);
    TASK_CHECK(sink.count == 0);
    push_section(rx, fixture_section(&cur, 1), &counter);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(sink.events[0].current_next);
    TASK_CHECK(sink.events[0].pid == 100);
    push_section(rx, fixture_section(&nxt, 1), &counter);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(!sink.events[1].current_next);
    TASK_CHECK(sink.events[1].pid == 200);

    /* Rotate both again: nothing new may be delivered. */
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&cur, 1), &counter);
    push_section(rx, fixture_section(&nxt, 0), &counter);
    push_section(rx, fixture_section(&nxt, 1), &counter);
    TASK_CHECK(sink.count == 2);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur);
    fixture_free(&nxt);
    return 0;
}

/* a new version on one lane resets only that lane */
static int case_single_lane_version_change(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));
    fixture_t nxt; fixture_reset(&nxt);
    fixture_set(&nxt, make_pat_two(7, 3, false, 200));

    uint8_t counter = 0;
    /* next gets section 0 first, current section 0 too */
    push_section(rx, fixture_section(&nxt, 0), &counter);
    push_section(rx, fixture_section(&cur, 0), &counter);
    TASK_CHECK(sink.count == 0);

    /* current changes version: its half table must be dropped, next kept */
    fixture_t cur2; fixture_reset(&cur2);
    fixture_set(&cur2, make_pat_two(7, 4, true, 300));
    push_section(rx, fixture_section(&cur2, 0), &counter);
    TASK_CHECK(sink.count == 0);

    /* completing the old next section 1 must still work */
    push_section(rx, fixture_section(&nxt, 1), &counter);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(!sink.events[0].current_next && sink.events[0].pid == 200);

    /* completing current v4 with section 1 delivers current v4 */
    push_section(rx, fixture_section(&cur2, 1), &counter);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.events[1].current_next && sink.events[1].version == 4);
    TASK_CHECK(sink.events[1].pid == 300);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur); fixture_free(&nxt); fixture_free(&cur2);
    return 0;
}

/* next completes first with b_current_next=false; the same-version current
 * must still be collected on its own and delivered separately. */
static int case_next_then_current_independent(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t nxt; fixture_reset(&nxt);
    fixture_set(&nxt, make_pat_two(7, 6, false, 200));
    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 6, true, 100));

    uint8_t counter = 0;
    push_section(rx, fixture_section(&nxt, 0), &counter);
    push_section(rx, fixture_section(&nxt, 1), &counter);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(!sink.events[0].current_next && sink.events[0].pid == 200);

    /* The delivered next must not be promoted to current and must not
     * suppress the same-version current generation. */
    push_section(rx, fixture_section(&cur, 0), &counter);
    TASK_CHECK(sink.count == 1);
    push_section(rx, fixture_section(&cur, 1), &counter);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.events[1].current_next && sink.events[1].version == 6);
    TASK_CHECK(sink.events[1].pid == 100);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&nxt); fixture_free(&cur);
    return 0;
}

/* Changing last_section_number or table_id_extension resets only that lane. */
static int case_lane_reset_on_last_and_extension_change(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    /* current lane gets section 0 of a 2-section table */
    fixture_t cur; fixture_reset(&cur);
    {
        dvbpsi_psi_section_t *s = make_pat(7, 3, true, 100, 100);
        restamp(s, 0, 1);
        fixture_set(&cur, s);
    }
    /* next lane, same version/extension but a different last number */
    fixture_t nxt; fixture_reset(&nxt);
    {
        dvbpsi_psi_section_t *s = make_pat(7, 3, false, 200, 200);
        restamp(s, 0, 2);
        fixture_set(&nxt, s);
    }
    uint8_t counter = 0;
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&nxt, 0), &counter);
    TASK_CHECK(sink.count == 0);

    /* last number changes on the next lane only: its half is dropped and it
     * still needs both sections of the new identity */
    fixture_t nxt2; fixture_reset(&nxt2);
    {
        dvbpsi_psi_section_t *s = make_pat(7, 3, false, 201, 201);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&nxt2, s);
    }
    push_section(rx, fixture_section(&nxt2, 1), &counter);
    TASK_CHECK(sink.count == 0);
    push_section(rx, fixture_section(&nxt2, 0), &counter);
    TASK_CHECK(sink.count == 1 && !sink.events[0].current_next);

    /* the current lane was untouched by the next lane changes: its earlier
     * section 0 still combines with a section 1 of the same identity */
    fixture_t cur1; fixture_reset(&cur1);
    {
        dvbpsi_psi_section_t *s = make_pat(7, 3, true, 101, 101);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&cur1, s);
    }
    push_section(rx, fixture_section(&cur1, 0), &counter);
    push_section(rx, fixture_section(&cur1, 1), &counter);
    TASK_CHECK(sink.count == 2 && sink.events[1].current_next);
    TASK_CHECK(sink.events[1].pid == 101);

    /* Changing the PAT TSID on the current lane is a new identity too. */
    fixture_t other; fixture_reset(&other);
    fixture_set(&other, make_pat_two(9, 3, true, 300));
    push_section(rx, fixture_section(&other, 0), &counter);
    TASK_CHECK(sink.count == 2);
    push_section(rx, fixture_section(&other, 1), &counter);
    TASK_CHECK(sink.count == 3 && sink.events[2].ts_id == 9);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur); fixture_free(&nxt); fixture_free(&nxt2);
    fixture_free(&cur1); fixture_free(&other);
    return 0;
}

/* version 31 -> 0 must start a new generation, and a changed TSID is not
 * swallowed by the de-duplication history of the same version. */
static int case_version_wrap_and_tsid_change(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    uint8_t counter = 0;
    fixture_t v31; fixture_reset(&v31);
    fixture_set(&v31, make_pat_two(7, 31, true, 100));
    push_section(rx, fixture_section(&v31, 0), &counter);
    push_section(rx, fixture_section(&v31, 1), &counter);
    TASK_CHECK(sink.count == 1 && sink.events[0].version == 31);

    fixture_t v0; fixture_reset(&v0);
    fixture_set(&v0, make_pat_two(7, 0, true, 110));
    push_section(rx, fixture_section(&v0, 0), &counter);
    push_section(rx, fixture_section(&v0, 1), &counter);
    TASK_CHECK(sink.count == 2 && sink.events[1].version == 0);
    TASK_CHECK(sink.events[1].pid == 110);

    /* Same version, new TSID: must be a fresh identity and be delivered. */
    fixture_t tsid; fixture_reset(&tsid);
    fixture_set(&tsid, make_pat_two(9, 0, true, 120));
    push_section(rx, fixture_section(&tsid, 0), &counter);
    push_section(rx, fixture_section(&tsid, 1), &counter);
    TASK_CHECK(sink.count == 3);
    TASK_CHECK(sink.events[2].ts_id == 9 && sink.events[2].version == 0);
    TASK_CHECK(sink.events[2].pid == 120);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&v31); fixture_free(&v0); fixture_free(&tsid);
    return 0;
}

/* section_number greater than last_section_number is dropped */
static int case_section_beyond_last_dropped(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t f; fixture_reset(&f);
    fixture_set(&f, make_pat(7, 3, true, 100, 100));
    restamp(f.p_sections, 2, 1); /* number 2 > last 1 */
    uint8_t counter = 0;
    push_section(rx, f.p_sections, &counter);
    TASK_CHECK(sink.count == 0);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&f);
    return 0;
}

#define CASES_LANES_ENTRIES \
    {"lanes", "current0_next1_no_callback", case_current0_next1_no_callback}, \
    {"lanes", "both_lanes_complete_and_dedupe", case_lanes_both_complete_and_dedupe}, \
    {"lanes", "single_lane_version_change", case_single_lane_version_change}, \
    {"lanes", "next_then_current_independent", case_next_then_current_independent}, \
    {"lanes", "lane_reset_on_last_and_extension_change", case_lane_reset_on_last_and_extension_change}, \
    {"lanes", "version_wrap_and_tsid_change", case_version_wrap_and_tsid_change}, \
    {"lanes", "section_beyond_last_dropped", case_section_beyond_last_dropped},

/* ------------------------------------------------------------------------- */
/* discontinuity                                                             */
/* ------------------------------------------------------------------------- */

/* A gap invalidates both lanes of a direct PAT decoder. */
static int case_pat_gap_drops_half_tables(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));
    fixture_t nxt; fixture_reset(&nxt);
    fixture_set(&nxt, make_pat_two(7, 3, false, 200));

    uint8_t counter = 0;
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&nxt, 0), &counter);
    TASK_CHECK(sink.count == 0);

    /* gap, then section 1 of both lanes: neither may complete */
    counter = (uint8_t)(counter + 2);
    push_section(rx, fixture_section(&cur, 1), &counter);
    TASK_CHECK(sink.count == 0);
    counter = (uint8_t)(counter + 2);
    push_section(rx, fixture_section(&nxt, 1), &counter);
    TASK_CHECK(sink.count == 0);

    /* fresh collection after the gap */
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&cur, 1), &counter);
    TASK_CHECK(sink.count == 1 && sink.events[0].current_next);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur); fixture_free(&nxt);
    return 0;
}

/* Two SDT subdecoders on one demux: a gap must reach both of them and not be
 * used up by whichever subtable arrives first.  Silence on the other
 * subtable's PID is simulated by feeding it only after the gap. */
static int case_sdt_two_subtables_gap(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    TASK_CHECK(dvbpsi_AttachDemux(rx, NULL, NULL));

    sdt_sink_t sink_a, sink_b;
    memset(&sink_a, 0, sizeof(sink_a));
    memset(&sink_b, 0, sizeof(sink_b));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 7, sdt_on_table, &sink_a));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 8, sdt_on_table, &sink_b));

    /* make each two-section fixtures */
    fixture_t fa; fixture_reset(&fa);
    {
        dvbpsi_psi_section_t *s = make_sdt(0x42, 7, 3, true, 8, 42);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&fa, s);
    }
    fixture_t fb; fixture_reset(&fb);
    {
        dvbpsi_psi_section_t *s = make_sdt(0x42, 8, 3, true, 8, 52);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&fb, s);
    }

    uint8_t counter = 0;
    /* half of A is buffered before the gap */
    push_section(rx, fixture_section(&fa, 0), &counter);

    /* Gap on the receiving handle.  A's stale half table must be dropped. */
    counter = (uint8_t)(counter + 2);

    /* The two subdecoders recover in different section orders.  Each starts
     * from its own fresh collection after the gap. */
    push_section(rx, fixture_section(&fb, 1), &counter);
    TASK_CHECK(sink_b.count == 0);
    push_section(rx, fixture_section(&fa, 1), &counter);
    /* A's pre-gap section 0 must not combine with section 1 here. */
    TASK_CHECK(sink_a.count == 0);

    push_section(rx, fixture_section(&fb, 0), &counter);
    TASK_CHECK(sink_b.count == 1 && sink_b.events[0].extension == 8);
    push_section(rx, fixture_section(&fa, 0), &counter);
    TASK_CHECK(sink_a.count == 1 && sink_a.events[0].extension == 7);

    dvbpsi_DetachDemux(rx);
    dvbpsi_delete(rx);
    fixture_free(&fa); fixture_free(&fb);
    return 0;
}

/* Same as above, but an unrelated subtable is interleaved between the gap and
 * the first SDT section.  The unrelated table must not consume the gap for
 * the SDT decoders. */
static int case_sdt_gap_not_consumed_by_other_table(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    TASK_CHECK(dvbpsi_AttachDemux(rx, NULL, NULL));

    sdt_sink_t sink_a, sink_b;
    memset(&sink_a, 0, sizeof(sink_a));
    memset(&sink_b, 0, sizeof(sink_b));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 7, sdt_on_table, &sink_a));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 8, sdt_on_table, &sink_b));

    /* Unrelated NIT subtable on the same demux. */
    dvbpsi_t *enc = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    TASK_CHECK(enc != NULL);
    dvbpsi_nit_t *p_nit_enc = dvbpsi_nit_new(0x40, 1, 1, 2, true);
    TASK_CHECK(p_nit_enc != NULL);
    TASK_CHECK(dvbpsi_nit_ts_add(p_nit_enc, 1, 1) != NULL);
    dvbpsi_psi_section_t *nit = dvbpsi_nit_sections_generate(enc, p_nit_enc, 0x40);
    dvbpsi_nit_delete(p_nit_enc);
    dvbpsi_delete(enc);
    TASK_CHECK(nit != NULL);
    TASK_CHECK(dvbpsi_nit_attach(rx, 0x40, 1, nit_on_table, NULL));

    fixture_t fa; fixture_reset(&fa);
    {
        dvbpsi_psi_section_t *s = make_sdt(0x42, 7, 3, true, 8, 42);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&fa, s);
    }
    fixture_t fb; fixture_reset(&fb);
    {
        dvbpsi_psi_section_t *s = make_sdt(0x42, 8, 3, true, 8, 52);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&fb, s);
    }

    uint8_t counter = 0;
    /* A's first half, then a gap, then the unrelated NIT. */
    push_section(rx, fixture_section(&fa, 0), &counter);
    counter = (uint8_t)(counter + 2);
    push_section(rx, nit, &counter);

    /* The other table must not consume the gap for the SDT decoders: A's
     * stale half table is dropped and B sees a fresh start. */
    push_section(rx, fixture_section(&fa, 1), &counter);
    TASK_CHECK(sink_a.count == 0);
    push_section(rx, fixture_section(&fb, 0), &counter);
    push_section(rx, fixture_section(&fb, 1), &counter);
    TASK_CHECK(sink_b.count == 1);

    /* A completes a fresh pair. */
    push_section(rx, fixture_section(&fa, 0), &counter);
    push_section(rx, fixture_section(&fa, 1), &counter);
    TASK_CHECK(sink_a.count == 1);

    dvbpsi_DeletePSISections(nit);
    dvbpsi_DetachDemux(rx);
    dvbpsi_delete(rx);
    fixture_free(&fa); fixture_free(&fb);
    return 0;
}

/* A continuity counter wrap (15 -> 0) is not a discontinuity. */
static int case_cc_wrap_is_not_gap(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));

    uint8_t counter = 14;
    push_section(rx, fixture_section(&cur, 0), &counter); /* 14 */
    push_section(rx, fixture_section(&cur, 1), &counter); /* 15 */
    TASK_CHECK(sink.count == 1);

    /* Continue with 0 after a wrap: no gap, and a rotated copy is ignored. */
    push_section(rx, fixture_section(&cur, 0), &counter); /* 0 */
    push_section(rx, fixture_section(&cur, 1), &counter); /* 1 */
    TASK_CHECK(sink.count == 1);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur);
    return 0;
}

/* Bad CRC is rejected by the receive path and does not advance any lane. */
static int case_bad_crc_rejected(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));
    dvbpsi_psi_section_t *bad = (dvbpsi_psi_section_t *)fixture_section(&cur, 0);
    bad->p_data[3 + bad->i_length - 1] ^= 0xff; /* break CRC */

    uint8_t counter = 0;
    push_section(rx, bad, &counter);
    TASK_CHECK(sink.count == 0);
    push_section(rx, fixture_section(&cur, 1), &counter);
    TASK_CHECK(sink.count == 0);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur);
    return 0;
}

/* After a gap, both PAT lanes must start over; next and then current can be
 * completed independently and in any order. */
static int case_pat_gap_both_lanes_recover(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 5, true, 100));
    fixture_t nxt; fixture_reset(&nxt);
    fixture_set(&nxt, make_pat_two(7, 5, false, 200));

    uint8_t counter = 0;
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&nxt, 0), &counter);

    /* gap, then next's second half arrives before current's */
    counter = (uint8_t)(counter + 2);
    push_section(rx, fixture_section(&nxt, 1), &counter);
    TASK_CHECK(sink.count == 0);

    /* fresh next pair is needed; current can't be completed by it */
    push_section(rx, fixture_section(&nxt, 0), &counter);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(!sink.events[0].current_next && sink.events[0].pid == 200);
    push_section(rx, fixture_section(&cur, 1), &counter);
    TASK_CHECK(sink.count == 1);
    push_section(rx, fixture_section(&cur, 0), &counter);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.events[1].current_next && sink.events[1].pid == 100);

    dvbpsi_pat_detach(rx);
    dvbpsi_delete(rx);
    fixture_free(&cur); fixture_free(&nxt);
    return 0;
}

#define CASES_DISCONTINUITY_ENTRIES \
    {"discontinuity", "pat_gap_drops_half_tables", case_pat_gap_drops_half_tables}, \
    {"discontinuity", "pat_gap_both_lanes_recover", case_pat_gap_both_lanes_recover}, \
    {"discontinuity", "sdt_two_subtables_gap", case_sdt_two_subtables_gap}, \
    {"discontinuity", "sdt_gap_not_consumed_by_other_table", case_sdt_gap_not_consumed_by_other_table}, \
    {"discontinuity", "cc_wrap_is_not_gap", case_cc_wrap_is_not_gap}, \
    {"discontinuity", "bad_crc_rejected", case_bad_crc_rejected},

/* ------------------------------------------------------------------------- */
/* lifetime                                                                  */
/* ------------------------------------------------------------------------- */

/* Callback deletes the table it receives; later rotation must not touch it. */
static int case_callback_owns_table(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    sdt_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.delete_in_callback = true;
    TASK_CHECK(dvbpsi_AttachDemux(rx, NULL, NULL));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 7, sdt_on_table, &sink));

    fixture_t f; fixture_reset(&f);
    {
        dvbpsi_psi_section_t *s = make_sdt(0x42, 7, 3, true, 8, 42);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&f, s);
    }
    uint8_t counter = 0;
    push_section(rx, fixture_section(&f, 0), &counter);
    push_section(rx, fixture_section(&f, 1), &counter);
    TASK_CHECK(sink.count == 1);

    /* Push the same carousel twice more; the decoder only keeps its own
     * delivered identities, never the freed table. */
    for (int i = 0; i < 2; i++)
    {
        push_section(rx, fixture_section(&f, 0), &counter);
        push_section(rx, fixture_section(&f, 1), &counter);
    }
    TASK_CHECK(sink.count == 1);

    dvbpsi_DetachDemux(rx);
    dvbpsi_delete(rx);
    fixture_free(&f);
    return 0;
}

/* Detaching while both lanes are incomplete releases their sections/tables. */
static int case_detach_with_half_tables(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_pat_attach(rx, pat_on_table, &sink));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));
    fixture_t nxt; fixture_reset(&nxt);
    fixture_set(&nxt, make_pat_two(7, 3, false, 200));

    uint8_t counter = 0;
    push_section(rx, fixture_section(&cur, 0), &counter);
    push_section(rx, fixture_section(&nxt, 0), &counter);
    TASK_CHECK(sink.count == 0);

    dvbpsi_pat_detach(rx);   /* must free both half tables */
    dvbpsi_delete(rx);
    fixture_free(&cur); fixture_free(&nxt);
    return 0;
}

/* An SDT subdecoder detached and reattached must not inherit old halves. */
static int case_sdt_detach_reattach(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    TASK_CHECK(dvbpsi_AttachDemux(rx, NULL, NULL));

    sdt_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 7, sdt_on_table, &sink));

    fixture_t f; fixture_reset(&f);
    {
        dvbpsi_psi_section_t *s = make_sdt(0x42, 7, 3, true, 8, 42);
        restamp(s, 0, 1);
        dvbpsi_psi_section_t *s2 = clone_section(s);
        restamp(s2, 1, 1);
        s->p_next = s2;
        fixture_set(&f, s);
    }
    uint8_t counter = 0;
    push_section(rx, fixture_section(&f, 0), &counter);
    TASK_CHECK(sink.count == 0);

    dvbpsi_sdt_detach(rx, 0x42, 7);
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 7, sdt_on_table, &sink));

    /* The new decoder starts from scratch: old section 1 alone can't finish. */
    push_section(rx, fixture_section(&f, 1), &counter);
    TASK_CHECK(sink.count == 0);
    push_section(rx, fixture_section(&f, 0), &counter);
    push_section(rx, fixture_section(&f, 1), &counter);
    TASK_CHECK(sink.count == 1);

    dvbpsi_sdt_detach(rx, 0x42, 7);
    dvbpsi_DetachDemux(rx);
    dvbpsi_delete(rx);
    fixture_free(&f);
    return 0;
}

/* An independent handle is unaffected by another handle's gap. */
static int case_independent_handle_unaffected(void)
{
    g_failed = 0;
    dvbpsi_t *rx1 = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    dvbpsi_t *rx2 = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pat_sink_t sink1, sink2;
    memset(&sink1, 0, sizeof(sink1));
    memset(&sink2, 0, sizeof(sink2));
    TASK_CHECK(dvbpsi_pat_attach(rx1, pat_on_table, &sink1));
    TASK_CHECK(dvbpsi_pat_attach(rx2, pat_on_table, &sink2));

    fixture_t cur; fixture_reset(&cur);
    fixture_set(&cur, make_pat_two(7, 3, true, 100));

    uint8_t c1 = 0, c2 = 0;
    push_section(rx1, fixture_section(&cur, 0), &c1);
    push_section(rx2, fixture_section(&cur, 0), &c2);

    /* Gap only on rx1. */
    c1 = (uint8_t)(c1 + 2);
    push_section(rx1, fixture_section(&cur, 1), &c1);
    TASK_CHECK(sink1.count == 0);
    TASK_CHECK(sink2.count == 0);

    /* rx2 is not affected and finishes normally. */
    push_section(rx2, fixture_section(&cur, 1), &c2);
    TASK_CHECK(sink2.count == 1);
    TASK_CHECK(sink1.count == 0);

    /* rx1 needs a fresh pair after the gap. */
    push_section(rx1, fixture_section(&cur, 0), &c1);
    push_section(rx1, fixture_section(&cur, 1), &c1);
    TASK_CHECK(sink1.count == 1);

    dvbpsi_pat_detach(rx1); dvbpsi_pat_detach(rx2);
    dvbpsi_delete(rx1); dvbpsi_delete(rx2);
    fixture_free(&cur);
    return 0;
}

/* PMT current/next split works like PAT. */
static int case_pmt_current_next_split(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    pmt_sink_t sink;
    memset(&sink, 0, sizeof(sink));

    dvbpsi_t *enc = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    dvbpsi_pmt_t *p_pmt = dvbpsi_pmt_new(42, 5, true, 256);
    TASK_CHECK(p_pmt != NULL);
    TASK_CHECK(dvbpsi_pmt_es_add(p_pmt, 0x1b, 256) != NULL);
    dvbpsi_psi_section_t *cur = dvbpsi_pmt_sections_generate(enc, p_pmt);
    dvbpsi_pmt_delete(p_pmt);
    TASK_CHECK(cur != NULL);
    dvbpsi_pmt_t *p_next = dvbpsi_pmt_new(42, 5, false, 300);
    TASK_CHECK(p_next != NULL);
    TASK_CHECK(dvbpsi_pmt_es_add(p_next, 0x1b, 300) != NULL);
    dvbpsi_psi_section_t *nxt = dvbpsi_pmt_sections_generate(enc, p_next);
    dvbpsi_pmt_delete(p_next);
    dvbpsi_delete(enc);
    TASK_CHECK(nxt != NULL);

    TASK_CHECK(dvbpsi_pmt_attach(rx, 42, pmt_on_table, &sink));
    uint8_t counter = 0;
    push_section(rx, cur, &counter);
    TASK_CHECK(sink.count == 1 && sink.events[0].current_next);
    TASK_CHECK(sink.events[0].first_pid == 256);
    push_section(rx, nxt, &counter);
    TASK_CHECK(sink.count == 2 && !sink.events[1].current_next);
    TASK_CHECK(sink.events[1].first_pid == 300);

    /* rotation dedupe */
    push_section(rx, cur, &counter);
    push_section(rx, nxt, &counter);
    TASK_CHECK(sink.count == 2);

    dvbpsi_pmt_detach(rx);
    dvbpsi_delete(rx);
    dvbpsi_DeletePSISections(cur);
    dvbpsi_DeletePSISections(nxt);
    return 0;
}

/* Descriptor content still survives when a table is split over sections. */
static int case_descriptor_content_preserved(void)
{
    g_failed = 0;
    dvbpsi_t *rx = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    TASK_CHECK(dvbpsi_AttachDemux(rx, NULL, NULL));
    sdt_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    TASK_CHECK(dvbpsi_sdt_attach(rx, 0x42, 7, sdt_on_table, &sink));

    dvbpsi_psi_section_t *s = make_sdt(0x42, 7, 3, true, 8, 42);
    restamp(s, 0, 1);
    /* the descriptor was generated by make_sdt's service; decode path keeps
     * it and the callback reports the service id. */
    dvbpsi_psi_section_t *s2 = clone_section(s);
    restamp(s2, 1, 1);
    s->p_next = s2;
    uint8_t counter = 0;
    push_section(rx, s, &counter);
    push_section(rx, s2, &counter);
    TASK_CHECK(sink.count == 1 && sink.events[0].service_id == 42);

    dvbpsi_DeletePSISections(s);
    dvbpsi_DetachDemux(rx);
    dvbpsi_delete(rx);
    return 0;
}

#define CASES_LIFETIME_ENTRIES \
    {"lifetime", "callback_owns_table", case_callback_owns_table}, \
    {"lifetime", "detach_with_half_tables", case_detach_with_half_tables}, \
    {"lifetime", "sdt_detach_reattach", case_sdt_detach_reattach}, \
    {"lifetime", "independent_handle_unaffected", case_independent_handle_unaffected}, \
    {"lifetime", "pmt_current_next_split", case_pmt_current_next_split}, \
    {"lifetime", "descriptor_content_preserved", case_descriptor_content_preserved},

/* ------------------------------------------------------------------------- */

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

const struct task_case task_cases[] = {
    CASES_LANES_ENTRIES
    CASES_DISCONTINUITY_ENTRIES
    CASES_LIFETIME_ENTRIES
};

const size_t task_case_count = COUNT(task_cases);
