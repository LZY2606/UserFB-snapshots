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
static int pat_seen, pmt_seen, sdt_seen;
static void got_pat(void *x, dvbpsi_pat_t *p) {
    (void)x; assert(p->i_ts_id==7 && p->i_version==3);
    assert(p->p_first_program && p->p_first_program->i_number==42);
    assert(p->p_first_program->i_pid==100 && !p->p_first_program->p_next);
    ++pat_seen; dvbpsi_pat_delete(p);
}
static void got_pmt(void *x, dvbpsi_pmt_t *p) {
    (void)x; assert(p->i_program_number==42 && p->i_pcr_pid==256);
    assert(p->p_first_es && p->p_first_es->i_type==0x1b && p->p_first_es->i_pid==256);
    assert(p->p_first_es->p_next && p->p_first_es->p_next->i_pid==257);
    assert(!p->p_first_es->p_next->p_next);
    ++pmt_seen; dvbpsi_pmt_delete(p);
}
static void got_sdt(void *x, dvbpsi_sdt_t *p) {
    (void)x; assert(p->i_extension==7 && p->i_network_id==8 && p->b_current_next);
    assert(p->p_first_service && p->p_first_service->i_service_id==42);
    assert(!p->p_first_service->p_next);
    assert(p->p_first_service->p_first_descriptor);
    dvbpsi_descriptor_t *d=p->p_first_service->p_first_descriptor;
    assert(d->i_tag==0x80 && d->i_length==3 && !memcmp(d->p_data,"abc",3));
    ++sdt_seen; dvbpsi_sdt_delete(p);
}
static void found_sdt(dvbpsi_t *rx, uint8_t id, uint16_t ext, void *x) {
    (void)x; assert(id==0x42 && ext==7);
    assert(dvbpsi_sdt_attach(rx,id,ext,got_sdt,NULL));
}
/* Small single-section normal fixtures. No fixed-behavior claim for interleaving. */
static void inject_small(dvbpsi_t *rx, const dvbpsi_psi_section_t *s, unsigned pid) {
    uint8_t packet[188]; size_t n=3+s->i_length;
    assert(!s->p_next && n<=183 && dvbpsi_ValidPSISection((dvbpsi_psi_section_t *)s));
    memset(packet,255,sizeof(packet)); packet[0]=0x47;
    packet[1]=0x40|(pid>>8); packet[2]=pid; packet[3]=0x10; packet[4]=0;
    memcpy(packet+5,s->p_data,n); assert(dvbpsi_packet_push(rx,packet));
}
int main(void) {
    dvbpsi_t *tx=dvbpsi_new(NULL,DVBPSI_MSG_NONE);
    dvbpsi_t *rxpat=dvbpsi_new(NULL,DVBPSI_MSG_NONE), *rxpmt=dvbpsi_new(NULL,DVBPSI_MSG_NONE);
    assert(tx && rxpat && rxpmt);
    assert(dvbpsi_pat_attach(rxpat,got_pat,NULL) && dvbpsi_pmt_attach(rxpmt,42,got_pmt,NULL));
    dvbpsi_pat_t *pat=dvbpsi_pat_new(7,3,true); assert(pat);
    assert(dvbpsi_pat_program_add(pat,42,100));
    dvbpsi_pmt_t *pmt=dvbpsi_pmt_new(42,3,true,256); assert(pmt);
    assert(dvbpsi_pmt_es_add(pmt,0x1b,256) && dvbpsi_pmt_es_add(pmt,0x0f,257));
    dvbpsi_psi_section_t *ps=dvbpsi_pat_sections_generate(tx,pat,253);
    dvbpsi_psi_section_t *ms=dvbpsi_pmt_sections_generate(tx,pmt); assert(ps && ms);
    printf("generated PAT bytes=%u PMT bytes=%u\n",3+ps->i_length,3+ms->i_length);
    inject_small(rxpat,ps,0); inject_small(rxpmt,ms,100);
    assert(pat_seen==1 && pmt_seen==1);
    puts("PAT callback: program=42 PMT_PID=100"); puts("PMT callback: PCR_PID=256 ES=256,257");
    dvbpsi_DeletePSISections(ps); dvbpsi_DeletePSISections(ms); dvbpsi_pat_delete(pat); dvbpsi_pmt_delete(pmt);
    dvbpsi_pat_detach(rxpat); dvbpsi_pmt_detach(rxpmt);
    assert(dvbpsi_AttachDemux(rxpat,found_sdt,NULL));
    dvbpsi_sdt_t *sdt=dvbpsi_sdt_new(0x42,7,3,true,8); assert(sdt);
    dvbpsi_sdt_service_t *service=dvbpsi_sdt_service_add(sdt,42,false,true,4,false); assert(service);
    assert(dvbpsi_sdt_service_descriptor_add(service,0x80,3,(uint8_t *)"abc"));
    dvbpsi_psi_section_t *ss=dvbpsi_sdt_sections_generate(tx,sdt); assert(ss);
    inject_small(rxpat,ss,17); assert(sdt_seen==1);
    puts("SDT callback: TSID=7 network=8 service=42 descriptor=abc");
    dvbpsi_DeletePSISections(ss); dvbpsi_sdt_delete(sdt); dvbpsi_DetachDemux(rxpat);
    dvbpsi_delete(rxpat); dvbpsi_delete(rxpmt); dvbpsi_delete(tx);
    puts("BASELINE_COMPLETE PSI generator/packet_push roundtrip"); return 0;
}
