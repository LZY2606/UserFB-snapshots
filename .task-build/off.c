#include "dwarf.h"
#include "libdwarf.h"
#include <stdio.h>
int main(int argc,char**argv){
 Dwarf_Debug dbg; Dwarf_Error err=0; int r=dwarf_init_path(argv[1],0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&err);
 if(r!=0){printf("init %d %s\n",r,err?dwarf_errmsg(err):"?");return 1;}
 Dwarf_Unsigned l=0,typeoff=0,next=0,count=0; Dwarf_Half ver=0,addr=0,ls=0,ext=0,unit=0; Dwarf_Off ab=0; Dwarf_Sig8 sig;
 r=dwarf_next_cu_header_d(dbg,1,&l,&ver,&ab,&addr,&ls,&ext,&sig,&typeoff,&next,&unit,&err); printf("cu %d\n",r);
 Dwarf_Die cu=0,var=0; dwarf_siblingof_b(dbg,0,1,&cu,&err); dwarf_child(cu,&var,&err);
 Dwarf_Attribute at=0; r=dwarf_attr(var,DW_AT_location,&at,&err); printf("attr %d\n",r);
 Dwarf_Loc_Head_c h=0; r=dwarf_get_loclist_c(at,&h,&count,&err); printf("loc %d count %llu %s\n",r,(unsigned long long)count,r?dwarf_errmsg(err):"");
 if(r==0) for(Dwarf_Unsigned i=0;i<count;i++){Dwarf_Small code,src; Dwarf_Unsigned raw1,raw2,opcnt,expr,desc,bytecnt; Dwarf_Bool fail; Dwarf_Addr lo,hi; Dwarf_Locdesc_c e;
  dwarf_get_locdesc_entry_e(h,i,&code,&raw1,&raw2,&fail,&lo,&hi,&opcnt,&bytecnt,&e,&src,&expr,&desc,&err);
  printf("%llu code=%u raw=%llx,%llx pc=%llx,%llx exprOff=%llx descOff=%llx byte=%llu src=%u\n",(unsigned long long)i,code,(unsigned long long)raw1,(unsigned long long)raw2,(unsigned long long)lo,(unsigned long long)hi,(unsigned long long)expr,(unsigned long long)desc,(unsigned long long)bytecnt,src);
 }
 return 0;
}
