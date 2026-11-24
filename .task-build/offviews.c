#include "dwarf.h"
#include "libdwarf.h"
#include <stdio.h>
int main(int argc,char**argv){
 Dwarf_Debug dbg; Dwarf_Error err=0; int r=dwarf_init_path(argv[1],0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&err);
 if(r){printf("init %s\n",dwarf_errmsg(err));return 1;}
 Dwarf_Unsigned l=0,typeoff=0,next=0,count=0; Dwarf_Half ver=0,addr=0,ls=0,ext=0,unit=0; Dwarf_Off ab=0; Dwarf_Sig8 sig; Dwarf_Die cu=0,var=0; Dwarf_Attribute at=0;
 dwarf_next_cu_header_d(dbg,1,&l,&ver,&ab,&addr,&ls,&ext,&sig,&typeoff,&next,&unit,&err);
 dwarf_siblingof_b(dbg,0,1,&cu,&err); dwarf_child(cu,&var,&err); dwarf_attr(var,DW_AT_location,&at,&err);
 Dwarf_Loc_Head_c h=0; r=dwarf_get_loclist_views_c(var,at,&h,&count,&err); printf("views %d count %llu %s\n",r,(unsigned long long)count,r?dwarf_errmsg(err):"");
 if(!r) for(Dwarf_Unsigned i=0;i<count;i++){
  Dwarf_Small code=0,src=0; Dwarf_Unsigned raw1=0,raw2=0,opcnt=0,expr=0,desc=0,bytecnt=0; Dwarf_Bool fail=0; Dwarf_Addr lo=0,hi=0; Dwarf_Locdesc_c e=0; Dwarf_Locview v;
  dwarf_get_locdesc_entry_e(h,i,&code,&raw1,&raw2,&fail,&lo,&hi,&opcnt,&bytecnt,&e,&src,&expr,&desc,&err);
  dwarf_get_locdesc_views(e,&v,&err);
  printf("%llu code=%u pc=%llx,%llx desc=%llx expr=%llx view h=%d be=%llu,%llu src=%u off=%llx len=%llu\n",(unsigned long long)i,(unsigned)code,(unsigned long long)lo,(unsigned long long)hi,(unsigned long long)desc,(unsigned long long)expr,(int)v.has_views,(unsigned long long)v.begin_view,(unsigned long long)v.end_view,(unsigned)v.source_kind,(unsigned long long)v.section_offset,(unsigned long long)v.byte_length);
 }
 return 0;
}
