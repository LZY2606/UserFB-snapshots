#include <libfdt.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  unsigned char base[4096], overlay[4096];
  assert(!fdt_create_empty_tree(base,sizeof(base)));
  assert(fdt_add_subnode(base,0,"device")>=0);
  assert(!fdt_setprop_u32(base,fdt_path_offset(base,"/device"),"value",7));
  assert(!fdt_create_empty_tree(overlay,sizeof(overlay)));
  assert(fdt_add_subnode(overlay,0,"fragment@0")>=0);
  assert(!fdt_setprop_string(overlay,fdt_path_offset(overlay,"/fragment@0"),"target-path","/device"));
  assert(fdt_add_subnode(overlay,fdt_path_offset(overlay,"/fragment@0"),"__overlay__")>=0);
  assert(!fdt_setprop_u32(overlay,fdt_path_offset(overlay,"/fragment@0/__overlay__"),"value",9));
  assert(!fdt_overlay_apply(base,overlay));
  int len=0; const fdt32_t *value=fdt_getprop(base,fdt_path_offset(base,"/device"),"value",&len);
  assert(value && len==4 && fdt32_to_cpu(*value)==9);
  assert(!fdt_check_full(base,fdt_totalsize(base)));
  puts("FDT_APPLY_VALUE_OK value=9");
}
