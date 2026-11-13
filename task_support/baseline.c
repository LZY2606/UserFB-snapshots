/* SPDX-License-Identifier: MIT */
#include "libdwarf.h"
#include "dwarf.h"
#include "fixture.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"baseline failure line=%d %s\n",__LINE__,#x);if(error)fprintf(stderr,"%s\n",dwarf_errmsg(error));return 1;}}while(0)
int main(int argc,char **argv) {
    Dwarf_Debug dbg=0,sup=0,tied=0;
    Dwarf_Die die=0; Dwarf_Error error=0;
    Dwarf_Macro_Context mc=0;
    Dwarf_Unsigned version=0,off=0,count=0,len=0,k=0,lineno=0,index=0,stroff=0;
    Dwarf_Half op=0,forms=0; const Dwarf_Small *form=0; const char *text=0;
    unsigned definitions=0,imports=0;
    char mainpath[4096],suppath[4096];
    REQUIRE(argc==2); REQUIRE(!task_basic_fixtures(argv[1]));
    snprintf(mainpath,sizeof(mainpath),"%s/main.elf",argv[1]);
    snprintf(suppath,sizeof(suppath),"%s/sup.elf",argv[1]);
    REQUIRE(dwarf_init_path(mainpath,0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    REQUIRE(dwarf_init_path(suppath,0,0,DW_GROUPNUMBER_ANY,0,0,&sup,&error)==DW_DLV_OK);
    REQUIRE(dwarf_set_tied_dbg(dbg,sup,&error)==DW_DLV_OK);
    REQUIRE(dwarf_get_tied_dbg(dbg,&tied,&error)==DW_DLV_OK && tied==sup);
    REQUIRE(dwarf_offdie_b(dbg,12,1,&die,&error)==DW_DLV_OK);
    REQUIRE(dwarf_get_macro_context(die,&version,&mc,&off,&count,&len,&error)==DW_DLV_OK);
    REQUIRE(version==5 && off==0);
    for(k=0;k<count;++k) {
        REQUIRE(dwarf_get_macro_op(mc,k,&off,&op,&forms,&form,&error)==DW_DLV_OK);
        if(op==DW_MACRO_define_strp) {
            REQUIRE(dwarf_get_macro_defundef(mc,k,&lineno,&index,&stroff,&forms,&text,&error)==DW_DLV_OK);
            REQUIRE(!strcmp(text,"MAIN_OWNER 1")); ++definitions;
            printf("MAIN_DEFINE text=%s line=%llu\n",text,(unsigned long long)lineno);
        }
        if(op==DW_MACRO_import_sup) {
            REQUIRE(dwarf_get_macro_import(mc,k,&off,&error)==DW_DLV_OK && off==0);
            ++imports; printf("IMPORT_SUP offset=%llu tied=yes\n",(unsigned long long)off);
        }
    }
    REQUIRE(definitions==1 && imports==1);
    { Dwarf_Addr addr=0; Dwarf_Unsigned size=0;
      REQUIRE(dwarf_get_section_info_by_name(sup,".debug_macro",&addr,&size,&error)==DW_DLV_OK && size==10);
      REQUIRE(dwarf_get_section_info_by_name(sup,".debug_info",&addr,&size,&error)==DW_DLV_NO_ENTRY);
      puts("SUPPLEMENTARY macro=yes cu=no"); }
    dwarf_dealloc_macro_context(mc); dwarf_dealloc_die(die);
    REQUIRE(dwarf_finish(dbg)==DW_DLV_OK); REQUIRE(dwarf_finish(sup)==DW_DLV_OK);
    puts("BASELINE_OK existing macro API and tied relation"); return 0;
}
