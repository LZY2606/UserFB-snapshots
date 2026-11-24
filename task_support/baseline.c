#include "dwarf.h"
#include "libdwarf.h"
#include "task_support/fixture.h"
#include <stdlib.h>
static void must(int rc,Dwarf_Error err,const char *where){if(rc!=DW_DLV_OK){fprintf(stderr,"%s: %s\n",where,err?dwarf_errmsg(err):"NO_ENTRY");exit(1);}}
static int run(const char *path,int views){
 Dwarf_Debug dbg=0;Dwarf_Error err=0;Dwarf_Die cu=0,var=0;Dwarf_Attribute attr=0;Dwarf_Loc_Head_c head=0;
 Dwarf_Unsigned length=0,typeoff=0,next=0,count=0;Dwarf_Off abbrev=0;Dwarf_Half ver=0,addr=0,ls=0,ext=0,unit=0;Dwarf_Sig8 sig;int rc;
 if(fixture_write(path,views))return 1;
 rc=dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&err);must(rc,err,"init");
 rc=dwarf_next_cu_header_d(dbg,1,&length,&ver,&abbrev,&addr,&ls,&ext,&sig,&typeoff,&next,&unit,&err);must(rc,err,"CU header");
 rc=dwarf_siblingof_b(dbg,0,1,&cu,&err);must(rc,err,"CU DIE");rc=dwarf_child(cu,&var,&err);must(rc,err,"variable");rc=dwarf_attr(var,DW_AT_location,&attr,&err);must(rc,err,"location attr");
 rc=dwarf_get_loclist_c(attr,&head,&count,&err);
 if(!views){must(rc,err,"plain list");if(count!=4){fprintf(stderr,"unexpected count %llu\n",(unsigned long long)count);return 1;}printf("LOCATION_BASELINE_OK count=%llu\n",(unsigned long long)count);}
 else {printf("INLINE_STATUS=%s\n",rc==DW_DLV_ERROR?"ERROR":rc==DW_DLV_OK?"OK":"NO_ENTRY");if(rc==DW_DLV_ERROR){printf("INLINE_REASON=%s\n",dwarf_errmsg(err));dwarf_dealloc_error(dbg,err);err=0;}else if(rc==DW_DLV_OK){printf("INLINE_COUNT=%llu\n",(unsigned long long)count);}else return 1;}
 if(head)dwarf_dealloc_loc_head_c(head);dwarf_dealloc_attribute(attr);dwarf_dealloc_die(var);dwarf_dealloc_die(cu);dwarf_finish(dbg);return 0;
}
int main(void){if(run(".task-build/plain.elf",0)||run(".task-build/views.elf",1))return 1;puts("DATA_ONLY_NO_EXECUTION");return 0;}
