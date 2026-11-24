#include "task_support/check.h"
#include "task_support/fixture.h"

#include "dwarf.h"
#include "libdwarf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------- small byte buffers and ELF writer ---------- */

struct blob { unsigned char b[4096]; size_t n; };

static void b8(struct blob *p, unsigned v)
{
    if (p->n < sizeof(p->b)) p->b[p->n++] = (unsigned char)v;
}
static void b16(struct blob *p, unsigned v)
{
    b8(p,v & 255); b8(p,(v >> 8) & 255);
}
static void b32(struct blob *p, uint32_t v)
{
    int i; for (i = 0; i < 4; ++i) b8(p,(unsigned)(v >> (8*i)));
}
static void b64(struct blob *p, uint64_t v)
{
    int i; for (i = 0; i < 8; ++i) b8(p,(unsigned)(v >> (8*i)));
}
static void bbytes(struct blob *p, const void *s, size_t n)
{
    const unsigned char *q = s;
    size_t i;
    for (i = 0; i < n; ++i) b8(p,q[i]);
}
static void buleb(struct blob *p, uint64_t v)
{
    do {
        unsigned char x = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v) x |= 0x80;
        b8(p,x);
    } while (v);
}
static size_t bstr(struct blob *p, const char *s)
{
    size_t off = p->n;
    bbytes(p,s,strlen(s)+1);
    return off;
}

static int write_test_elf_named(struct blob *info,
    struct blob *abbrev, struct blob *loc, int dwarf4,
    const char *path)
{
    struct blob names;
    unsigned char image[32768];
    struct blob *sec[4];
    size_t off[4], shoff, i, cursor = 64;
    uint32_t nameoff[4];
    FILE *out;

    memset(&names,0,sizeof(names));
    b8(&names,0);
    nameoff[0] = (uint32_t)bstr(&names,".debug_info");
    nameoff[1] = (uint32_t)bstr(&names,".debug_abbrev");
    nameoff[2] = (uint32_t)bstr(&names,
        dwarf4 ? ".debug_loc" : ".debug_loclists");
    nameoff[3] = (uint32_t)bstr(&names,".shstrtab");

    sec[0] = info; sec[1] = abbrev; sec[2] = loc; sec[3] = &names;
    memset(image,0,sizeof(image));
    if (info->n + abbrev->n + loc->n + names.n + 1024 > sizeof(image))
        return 2;
    for (i = 0; i < 4; ++i) {
        off[i] = cursor;
        memcpy(image+cursor,sec[i]->b,sec[i]->n);
        cursor += sec[i]->n;
    }
    shoff = (cursor + 7) & ~(size_t)7;
    if (shoff + 5*64 > sizeof(image)) return 3;
    memcpy(image,"\177ELF\2\1\1",7);
    image[4] = 2; /* ELFCLASS64 */
    image[5] = 1; /* little endian */
    image[6] = 1; /* EV_CURRENT */
    {
        unsigned char *h = image;
        h[16]=1; h[18]=62; h[20]=1; /* ET_REL, EM_X86_64 */
        for (i=0;i<8;++i) h[40+i]=(unsigned char)(shoff>>(8*i));
        h[52]=64; h[58]=64; h[60]=5; h[62]=4;
    }
    for (i = 0; i < 4; ++i) {
        unsigned char *s = image + shoff + (i+1)*64;
        size_t j;
        for (j=0;j<4;++j) s[j]=(unsigned char)(nameoff[i]>>(8*j));
        {
            uint32_t type = (i == 3 ? 3 : 1);
            for (j=0;j<4;++j) s[4+j]=(unsigned char)(type>>(8*j));
        }
        for (j=0;j<8;++j) s[24+j]=(unsigned char)(off[i]>>(8*j));
        for (j=0;j<8;++j) s[32+j]=(unsigned char)(sec[i]->n>>(8*j));
        for (j=0;j<8;++j) s[48+j]=(unsigned char)(1>>(8*j));
    }
    out = fopen(path,"wb");
    if (!out) return 4;
    if (fwrite(image,1,shoff+5*64,out) != shoff+5*64) {
        fclose(out); return 5;
    }
    if (fclose(out)) return 6;
    return 0;
}

static int write_test_elf(const char *path, struct blob *info,
    struct blob *abbrev, struct blob *loc)
{
    return write_test_elf_named(info,abbrev,loc,0,path);
}

static int write_test_loc_elf(const char *path, struct blob *info,
    struct blob *abbrev, struct blob *loc)
{
    return write_test_elf_named(info,abbrev,loc,1,path);
}

static void ab_attr(struct blob *a, unsigned attr, unsigned form)
{
    buleb(a,attr); buleb(a,form);
}

static void ab_common_cu5(struct blob *a)
{
    b8(a,1); buleb(a,DW_TAG_compile_unit); b8(a,1);
    ab_attr(a,DW_AT_low_pc,DW_FORM_addr);
    ab_attr(a,DW_AT_loclists_base,DW_FORM_sec_offset);
    buleb(a,0); b8(a,0);
}
static void ab_common_cu4(struct blob *a)
{
    b8(a,1); buleb(a,DW_TAG_compile_unit); b8(a,1);
    buleb(a,0); b8(a,0);
}
static void ab_variable(struct blob *a, unsigned code,
    unsigned locform, int views)
{
    b8(a,code); buleb(a,DW_TAG_variable); b8(a,0);
    ab_attr(a,DW_AT_name,DW_FORM_string);
    ab_attr(a,DW_AT_location,locform);
    if (views) ab_attr(a,DW_AT_GNU_locviews,DW_FORM_sec_offset);
    buleb(a,0); b8(a,0);
}
static void ab_end(struct blob *a) { b8(a,0); }

static void info_header(struct blob *m, int version)
{
    b32(m,0);
    b16(m,version);
    if (version == 5) {
        b8(m,DW_UT_compile);
        b8(m,8);
        b32(m,0);
    } else {
        b32(m,0); /* debug_abbrev_offset */
        b8(m,8);
    }
}
static void info_patch(struct blob *m)
{
    uint32_t len = (uint32_t)m->n - 4;
    m->b[0]=(unsigned char)len;
    m->b[1]=(unsigned char)(len>>8);
    m->b[2]=(unsigned char)(len>>16);
    m->b[3]=(unsigned char)(len>>24);
}
static void info_cu5(struct blob *m)
{
    b8(m,1); b64(m,0x1000); b32(m,12);
}
static void info_var_sec(struct blob *m, unsigned code,
    const char *name, uint32_t loc, int hasviews, uint32_t views)
{
    b8(m,code); bstr(m,name); b32(m,loc);
    if (hasviews) b32(m,views);
}

static void loc5_header(struct blob *l, uint32_t offset_count,
    uint32_t table_value)
{
    b32(l,0); b16(l,5); b8(l,8); b8(l,0); b32(l,offset_count);
    if (offset_count) b32(l,table_value);
}
static void loc_patch_length(struct blob *l)
{
    uint32_t len = (uint32_t)l->n - 4;
    l->b[0]=(unsigned char)len;
    l->b[1]=(unsigned char)(len>>8);
    l->b[2]=(unsigned char)(len>>16);
    l->b[3]=(unsigned char)(len>>24);
}
static void loc5_base(struct blob *l, uint64_t pc)
{
    b8(l,DW_LLE_base_address); b64(l,pc);
}
static void loc5_pair(struct blob *l, uint64_t a, uint64_t b,
    unsigned regop)
{
    b8(l,DW_LLE_offset_pair);
    buleb(l,a); buleb(l,b); buleb(l,1); b8(l,regop);
}
static void loc5_end(struct blob *l) { b8(l,DW_LLE_end_of_list); }
static void ext_pair(struct blob *l, uint64_t a, uint64_t b)
{
    buleb(l,a); buleb(l,b);
}
static void list_same_pc(struct blob *l)
{
    loc5_base(l,0x1000);
    loc5_pair(l,0,0,DW_OP_reg0);
    loc5_pair(l,0,16,DW_OP_reg1);
    loc5_end(l);
}
static void list_base_switch(struct blob *l)
{
    loc5_base(l,0x1000);
    loc5_pair(l,0,0,DW_OP_reg0);
    loc5_base(l,0x2000);
    loc5_pair(l,0,16,DW_OP_reg1);
    loc5_end(l);
}

/* ---------- DWARF traversal / records ---------- */

struct vrec {
    char object[32];
    char name[64];
    Dwarf_Off die_offset;
    Dwarf_Addr lo, hi;
    Dwarf_Bool has;
    Dwarf_Unsigned bv, ev;
    unsigned code;
    unsigned source;
    Dwarf_Unsigned voff, vlen, addroff, exproff;
    Dwarf_Unsigned exprsize;
    unsigned char expr[16];
    char status[48];
    char description[128];
};

static int is_range_code(unsigned code)
{
    return code == DW_LLE_startx_endx ||
        code == DW_LLE_startx_length ||
        code == DW_LLE_offset_pair ||
        code == DW_LLE_start_end ||
        code == DW_LLE_start_length;
}

static void coordinate_status(struct vrec *r)
{
    if (r->lo < r->hi) {
        snprintf(r->status,sizeof(r->status),"nonempty-pc-range");
    } else if (r->lo > r->hi) {
        snprintf(r->status,sizeof(r->status),"reverse-pc");
    } else if (!r->has) {
        snprintf(r->status,sizeof(r->status),
            "empty-same-pc-no-view");
    } else if (r->bv < r->ev) {
        snprintf(r->status,sizeof(r->status),
            "nonempty-same-pc-view");
    } else if (r->bv == r->ev) {
        snprintf(r->status,sizeof(r->status),
            "empty-explicit-view");
    } else {
        snprintf(r->status,sizeof(r->status),"reverse-view");
    }
}

static void describe_expr(struct vrec *r)
{
    char hex[80];
    size_t i, used = 0;

    hex[0] = 0;
    for (i = 0; i < r->exprsize && i < sizeof(r->expr); ++i) {
        int n = snprintf(hex+used,sizeof(hex)-used,"%s%02x",
            i ? " " : "", r->expr[i]);
        if (n < 0 || (size_t)n >= sizeof(hex)-used) break;
        used += (size_t)n;
    }
    if (r->exprsize == 1 && r->expr[0] == DW_OP_reg0) {
        snprintf(r->description,sizeof(r->description),
            "DWARF register 0 (DW_OP_reg0)");
    } else if (r->exprsize == 1 && r->expr[0] == DW_OP_reg1) {
        snprintf(r->description,sizeof(r->description),
            "DWARF register 1 (DW_OP_reg1)");
    } else {
        snprintf(r->description,sizeof(r->description),
            "uninterpreted expression bytes: %s",hex);
    }
}

static int fill_raw_loclists_expr(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c head, Dwarf_Unsigned index,
    Dwarf_Unsigned desc_off, Dwarf_Unsigned *size,
    unsigned char *bytes, Dwarf_Error *error)
{
    Dwarf_Unsigned lle_count=0, version=0, ctxidx=0;
    Dwarf_Unsigned bytes_total=0, overall=0, total_len=0;
    Dwarf_Unsigned off_table=0, off_count=0, listset=0;
    Dwarf_Small lkind=0;
    Dwarf_Half offsize=0, addrsize=0, seg=0;
    Dwarf_Bool basep=0, cu_basep=0, addrbasep=0;
    Dwarf_Unsigned locbase=0, cu_base=0, addrbase=0;
    unsigned int entrylen=0, code=0;
    Dwarf_Unsigned o1=0,o2=0,blocksize=0,expr_off=0;
    Dwarf_Small *exprdata=0;
    Dwarf_Unsigned nctx=0;
    int res;

    *size = 0;
    res = dwarf_load_loclists(dbg,&nctx,error);
    if (res != DW_DLV_OK && res != DW_DLV_NO_ENTRY) return res;
    res = dwarf_get_loclist_head_basics(head,&lkind,&lle_count,
        &version,&ctxidx,&bytes_total,&offsize,&addrsize,&seg,
        &overall,&total_len,&off_table,&off_count,&basep,&locbase,
        &cu_basep,&cu_base,&addrbasep,&addrbase,&listset,error);
    if (res != DW_DLV_OK) return res;
    res = dwarf_get_loclist_lle(dbg,ctxidx,desc_off,
        overall+total_len,&entrylen,&code,&o1,&o2,&blocksize,
        &expr_off,&exprdata,error);
    if (res != DW_DLV_OK) return res;
    if (blocksize > 16) blocksize = 16;
    memcpy(bytes,exprdata,blocksize);
    *size = blocksize;
    return DW_DLV_OK;
}

static int process_die(Dwarf_Debug dbg, Dwarf_Die die,
    const char *label, struct vrec *recs, int cap, int *count,
    Dwarf_Error *error)
{
    Dwarf_Attribute attr = 0;
    Dwarf_Loc_Head_c head = 0;
    Dwarf_Unsigned n = 0, i = 0;
    char *name = 0;
    Dwarf_Off dieoff = 0;
    int res;

    res = dwarf_attr(die,DW_AT_location,&attr,error);
    if (res == DW_DLV_ERROR) return DW_DLV_ERROR;
    if (res == DW_DLV_OK && attr) {
        res = dwarf_get_loclist_views_c(die,attr,&head,&n,error);
        if (res == DW_DLV_ERROR) {
            dwarf_dealloc_attribute(attr);
            return DW_DLV_ERROR;
        }
        if (res == DW_DLV_OK) {
            dwarf_diename(die,&name,error);
            dwarf_dieoffset(die,&dieoff,error);
            for (i = 0; i < n && *count < cap; ++i) {
                Dwarf_Small code=0, src=0;
                Dwarf_Unsigned raw1=0,raw2=0,opcount=0;
                Dwarf_Unsigned exproff=0,descoff=0,bytecnt=0;
                Dwarf_Bool unavailable=0;
                Dwarf_Addr lo=0,hi=0;
                Dwarf_Locdesc_c e=0;
                Dwarf_Locview view;
                struct vrec *r = &recs[(*count)++];

                memset(r,0,sizeof(*r));
                snprintf(r->object,sizeof(r->object),"%s",label);
                snprintf(r->name,sizeof(r->name),"%s",
                    name ? name : "<unnamed>");
                r->die_offset = dieoff;
                res = dwarf_get_locdesc_entry_e(head,i,&code,
                    &raw1,&raw2,&unavailable,&lo,&hi,&opcount,
                    &bytecnt,&e,&src,&exproff,&descoff,error);
                if (res != DW_DLV_OK) return DW_DLV_ERROR;
                if (!is_range_code(code)) {
                    --(*count);
                    continue;
                }
                r->lo=lo; r->hi=hi; r->code=code;
                r->addroff=descoff; r->exproff=exproff;
                dwarf_get_locdesc_views(e,&view,error);
                r->has=view.has_views;
                r->bv=view.begin_view; r->ev=view.end_view;
                r->source=view.source_kind;
                r->voff=view.section_offset;
                r->vlen=view.byte_length;
                if (src == DW_LKIND_loclists) {
                    fill_raw_loclists_expr(dbg,head,i,descoff,
                        &r->exprsize,r->expr,error);
                } else if (opcount > 0) {
                    Dwarf_Small op=0;
                    Dwarf_Unsigned a=0,b=0,c=0,bo=0;
                    dwarf_get_location_op_value_c(e,0,&op,
                        &a,&b,&c,&bo,error);
                    r->expr[0]=op; r->exprsize=1;
                }
                describe_expr(r);
                coordinate_status(r);
            }
            dwarf_dealloc_loc_head_c(head);
        }
        dwarf_dealloc_attribute(attr);
    }

    /* Recurse through children and siblings. */
    {
        Dwarf_Die child = 0;
        res = dwarf_child(die,&child,error);
        if (res == DW_DLV_ERROR) return DW_DLV_ERROR;
        if (res == DW_DLV_OK) {
            while (child) {
                Dwarf_Die next = 0;
                int pr = process_die(dbg,child,label,recs,cap,
                    count,error);
                if (pr == DW_DLV_ERROR) {
                    dwarf_dealloc_die(child);
                    return DW_DLV_ERROR;
                }
                res = dwarf_siblingof_b(dbg,child,1,&next,error);
                dwarf_dealloc_die(child);
                if (res != DW_DLV_OK) break;
                child = next;
            }
        }
    }
    return DW_DLV_OK;
}

static int collect_path(const char *path, const char *label,
    struct vrec *recs, int cap, int *count)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Error error = 0;
    int res;

    *count = 0;
    res = dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,0,0,
        &dbg,&error);
    if (res != DW_DLV_OK) {
        fprintf(stderr,"init failed %s: %s\n",path,
            error ? dwarf_errmsg(error) : "?");
        return 1;
    }
    for (;;) {
        Dwarf_Unsigned len=0,typeoff=0,next=0;
        Dwarf_Half ver=0,addrsize=0,locsize=0,extsize=0,unittype=0;
        Dwarf_Off abbrev=0;
        Dwarf_Sig8 sig;
        Dwarf_Die cu=0;

        res = dwarf_next_cu_header_d(dbg,1,&len,&ver,&abbrev,
            &addrsize,&locsize,&extsize,&sig,&typeoff,&next,
            &unittype,&error);
        if (res == DW_DLV_NO_ENTRY) break;
        if (res != DW_DLV_OK) return 2;
        res = dwarf_siblingof_b(dbg,0,1,&cu,&error);
        if (res != DW_DLV_OK) return 3;
        process_die(dbg,cu,label,recs,cap,count,&error);
        dwarf_dealloc_die(cu);
    }
    dwarf_finish(dbg);
    return 0;
}

static void write_tsv(const char *path, struct vrec *r, int n)
{
    FILE *f = fopen(path,"w");
    int i;
    if (!f) return;
    fprintf(f,"object\tvariable_name\tdie_offset\tbegin_pc\tend_pc\t"
        "has_views\tbegin_view\tend_view\texpression_hex\t"
        "location_description\tview_source_kind\t"
        "view_section_offset\tview_byte_length\t"
        "address_entry_section_offset\texpression_section_offset\t"
        "coordinate_check\n");
    for (i=0;i<n;++i) {
        char hex[80]; size_t j, used=0;
        hex[0]=0;
        for (j=0;j<r[i].exprsize && j<sizeof(r[i].expr);++j) {
            int k = snprintf(hex+used,sizeof(hex)-used,"%s%02x",
                j ? " " : "",r[i].expr[j]);
            if (k <= 0 || (size_t)k >= sizeof(hex)-used) break;
            used += (size_t)k;
        }
        fprintf(f,"%s\t%s\t0x%llx\t0x%llx\t0x%llx\t%d\t%llu\t%llu\t"
            "%s\t%s\t%u\t0x%llx\t%llu\t0x%llx\t0x%llx\t%s\n",
            r[i].object,r[i].name,
            (unsigned long long)r[i].die_offset,
            (unsigned long long)r[i].lo,
            (unsigned long long)r[i].hi,
            (int)r[i].has,
            (unsigned long long)r[i].bv,
            (unsigned long long)r[i].ev,
            hex,r[i].description,r[i].source,
            (unsigned long long)r[i].voff,
            (unsigned long long)r[i].vlen,
            (unsigned long long)r[i].addroff,
            (unsigned long long)r[i].exproff,
            r[i].status);
    }
    fclose(f);
}

/* ---------- file patching helpers ---------- */

static int copy_file(const char *src, const char *dst)
{
    FILE *a=fopen(src,"rb"),*b=fopen(dst,"wb");
    unsigned char buf[4096]; size_t n;
    if (!a || !b) return 1;
    while ((n=fread(buf,1,sizeof(buf),a))) {
        if (fwrite(buf,1,n,b)!=n) return 2;
    }
    fclose(a); if (fclose(b)) return 3;
    return 0;
}

static uint64_t rd64(const unsigned char *p)
{
    uint64_t v=0; int i;
    for (i=0;i<8;++i) v |= (uint64_t)p[i] << (8*i);
    return v;
}

static int patch_view_uleb(const char *path,
    const char *section, Dwarf_Unsigned view_offset,
    unsigned first, unsigned second)
{
    unsigned char image[16384];
    FILE *f=fopen(path,"r+b");
    size_t n;
    uint64_t shoff;
    unsigned short shnum, shstrndx, shentsize;
    uint64_t shstr_off;
    unsigned i;

    if (!f) return 1;
    n=fread(image,1,sizeof(image),f);
    if (n < 64) { fclose(f); return 2; }
    shoff=rd64(image+0x28);
    shentsize=(unsigned short)(image[0x3a] | image[0x3b]<<8);
    shnum=(unsigned short)(image[0x3c] | image[0x3d]<<8);
    shstrndx=(unsigned short)(image[0x3e] | image[0x3f]<<8);
    if (shoff+(uint64_t)(shstrndx+1)*shentsize > n) {
        fclose(f); return 3;
    }
    shstr_off=rd64(image+shoff+(uint64_t)shstrndx*shentsize+24);
    for (i=0;i<shnum;++i) {
        uint64_t sh=shoff+(uint64_t)i*shentsize;
        uint32_t name=(uint32_t)(image[sh] | image[sh+1]<<8 |
            image[sh+2]<<16 | (uint32_t)image[sh+3]<<24);
        uint64_t off=rd64(image+sh+24);
        const char *sname=(const char*)(image+shstr_off+name);
        if (strcmp(sname,section)==0) {
            long p1=(long)(off+view_offset+1);
            long p2=(long)(off+view_offset+2);
            if (fseek(f,p1,SEEK_SET) || fputc(first,f)==EOF ||
                fseek(f,p2,SEEK_SET) || fputc(second,f)==EOF) {
                fclose(f); return 4;
            }
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return 5;
}

/* ---------- lookup and direct API checks ---------- */

static Dwarf_Die find_named_var(Dwarf_Debug dbg, const char *want,
    Dwarf_Error *error)
{
    Dwarf_Die cu=0, found=0;
    Dwarf_Unsigned len=0,typeoff=0,next=0;
    Dwarf_Half ver=0,as=0,ls=0,es=0,ut=0;
    Dwarf_Off ab=0; Dwarf_Sig8 sig;

    if (dwarf_next_cu_header_d(dbg,1,&len,&ver,&ab,&as,&ls,&es,
        &sig,&typeoff,&next,&ut,error) != DW_DLV_OK) return 0;
    if (dwarf_siblingof_b(dbg,0,1,&cu,error) != DW_DLV_OK) return 0;
    {
        Dwarf_Die child=0;
        if (dwarf_child(cu,&child,error)==DW_DLV_OK) {
            while (child) {
                char *nm=0;
                Dwarf_Die nxt=0;
                if (dwarf_diename(child,&nm,error)==DW_DLV_OK &&
                    nm && strcmp(nm,want)==0) {
                    found=child;
                    break;
                }
                if (dwarf_siblingof_b(dbg,child,1,&nxt,error)
                    != DW_DLV_OK) break;
                child=nxt;
            }
        }
    }
    dwarf_dealloc_die(cu);
    return found;
}

static int open_value_head(const char *path, int withviewsapi,
    Dwarf_Debug *dbg, Dwarf_Die *die, Dwarf_Attribute *attr,
    Dwarf_Loc_Head_c *head, Dwarf_Unsigned *count)
{
    Dwarf_Error error=0;
    int res;
    *dbg=0; *die=0; *attr=0; *head=0; *count=0;
    res=dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,0,0,dbg,&error);
    if (res != DW_DLV_OK) {
        fprintf(stderr,"init %s: %s\n",path,
            error?dwarf_errmsg(error):"?");
        return 1;
    }
    *die=find_named_var(*dbg,"value",&error);
    if (!*die) return 2;
    res=dwarf_attr(*die,DW_AT_location,attr,&error);
    if (res != DW_DLV_OK) return 3;
    if (withviewsapi)
        res=dwarf_get_loclist_views_c(*die,*attr,head,count,&error);
    else
        res=dwarf_get_loclist_c(*attr,head,count,&error);
    if (res != DW_DLV_OK) {
        fprintf(stderr,"loclist %s: %s\n",path,
            error?dwarf_errmsg(error):"?");
        return 4;
    }
    return 0;
}

static int get_view_record(Dwarf_Loc_Head_c head,
    Dwarf_Unsigned idx, Dwarf_Unsigned *descoff,
    Dwarf_Unsigned *exproff, Dwarf_Locview *v)
{
    Dwarf_Error error=0;
    Dwarf_Small code=0,src=0;
    Dwarf_Unsigned raw1=0,raw2=0,opcnt=0,bytecnt=0;
    Dwarf_Bool fail=0; Dwarf_Addr lo=0,hi=0;
    Dwarf_Locdesc_c e=0;
    int r=dwarf_get_locdesc_entry_e(head,idx,&code,&raw1,&raw2,
        &fail,&lo,&hi,&opcnt,&bytecnt,&e,&src,exproff,descoff,
        &error);
    if (r != DW_DLV_OK) return r;
    return dwarf_get_locdesc_views(e,v,&error);
}

/* ---------- task cases ---------- */

static int inline_views_fixture(void)
{
    Dwarf_Debug dbg=0; Dwarf_Die die=0; Dwarf_Attribute attr=0;
    Dwarf_Loc_Head_c head=0; Dwarf_Unsigned count=0;
    Dwarf_Locview v1,v2; Dwarf_Unsigned d1=0,e1=0,d2=0,e2=0;
    Dwarf_Error error=0;
    Dwarf_Unsigned nctx=0;
    unsigned elen=0,ecode=0;
    Dwarf_Unsigned o1=0,o2=0,bs=0,eoff=0;
    Dwarf_Small *ed=0;

    mkdir(".task-build",0755);
    TASK_CHECK(fixture_write(".task-build/plain.elf",0)==0);
    TASK_CHECK(fixture_write(".task-build/views.elf",1)==0);

    /* Plain object: four retained records, no fabricated views. */
    TASK_CHECK(open_value_head(".task-build/plain.elf",0,
        &dbg,&die,&attr,&head,&count)==0);
    TASK_CHECK(count==4);
    TASK_CHECK(get_view_record(head,1,&d1,&e1,&v1)==DW_DLV_OK);
    TASK_CHECK(v1.has_views==0);
    TASK_CHECK(v1.source_kind==DW_LOCVIEW_SOURCE_NONE);
    TASK_CHECK(d1==0x15);
    TASK_CHECK(e1==0x19);
    TASK_CHECK(get_view_record(head,2,&d2,&e2,&v2)==DW_DLV_OK);
    TASK_CHECK(d2==0x1a);
    TASK_CHECK(e2==0x1e);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);

    /* View object: prefixes are consumed but not counted. */
    TASK_CHECK(open_value_head(".task-build/views.elf",1,
        &dbg,&die,&attr,&head,&count)==0);
    TASK_CHECK(count==4);
    TASK_CHECK(get_view_record(head,1,&d1,&e1,&v1)==DW_DLV_OK);
    TASK_CHECK(v1.has_views==1);
    TASK_CHECK(v1.begin_view==1 && v1.end_view==2);
    TASK_CHECK(v1.source_kind==DW_LOCVIEW_SOURCE_INLINE);
    TASK_CHECK(v1.section_offset==0x15 && v1.byte_length==3);
    TASK_CHECK(d1==0x18 && e1==0x1c);
    TASK_CHECK(get_view_record(head,2,&d2,&e2,&v2)==DW_DLV_OK);
    TASK_CHECK(v2.has_views==1);
    TASK_CHECK(v2.begin_view==0 && v2.end_view==0);
    TASK_CHECK(v2.source_kind==DW_LOCVIEW_SOURCE_INLINE);
    TASK_CHECK(v2.section_offset==0x1d && v2.byte_length==3);
    TASK_CHECK(d2==0x20 && e2==0x24);

    /* Raw API reports the view_pair itself truthfully. */
    TASK_CHECK(dwarf_load_loclists(dbg,&nctx,&error)==DW_DLV_OK);
    TASK_CHECK(nctx==1);
    TASK_CHECK(dwarf_get_loclist_lle(dbg,0,0x15,0x26,
        &elen,&ecode,&o1,&o2,&bs,&eoff,&ed,&error)==DW_DLV_OK);
    TASK_CHECK(ecode==DW_LLE_GNU_view_pair);
    TASK_CHECK(o1==1 && o2==2 && elen==3);
    TASK_CHECK(dwarf_get_loclist_lle(dbg,0,0x18,0x26,
        &elen,&ecode,&o1,&o2,&bs,&eoff,&ed,&error)==DW_DLV_OK);
    TASK_CHECK(ecode==DW_LLE_offset_pair && elen==5);

    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);
    return 0;
}

static int inline_views_mutation(void)
{
    Dwarf_Debug dbg=0; Dwarf_Die die=0; Dwarf_Attribute attr=0;
    Dwarf_Loc_Head_c head=0; Dwarf_Unsigned count=0;
    Dwarf_Locview v; Dwarf_Unsigned d=0,e=0;

    TASK_CHECK(copy_file(".task-build/views.elf",
        ".task-build/views-zero.elf")==0);
    TASK_CHECK(patch_view_uleb(".task-build/views-zero.elf",
        ".debug_loclists",0x15,0,0)==0);
    TASK_CHECK(open_value_head(".task-build/views-zero.elf",1,
        &dbg,&die,&attr,&head,&count)==0);
    TASK_CHECK(get_view_record(head,1,&d,&e,&v)==DW_DLV_OK);
    TASK_CHECK(v.has_views==1);
    TASK_CHECK(v.begin_view==0 && v.end_view==0);
    TASK_CHECK(v.section_offset==0x15 && v.byte_length==3);
    TASK_CHECK(d==0x18 && e==0x1c);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);
    {
        struct vrec zr[8];
        int zn=0;
        TASK_CHECK(collect_path(".task-build/views-zero.elf",
            "zero",zr,8,&zn)==0);
        TASK_CHECK(zn==2);
        TASK_CHECK(strstr(zr[0].status,"empty-explicit-view")!=0);
        TASK_CHECK(zr[1].bv==0 && zr[1].ev==0);
        TASK_CHECK(strstr(zr[1].status,"nonempty-pc-range")!=0);
    }

    TASK_CHECK(copy_file(".task-build/views.elf",
        ".task-build/views-reverse.elf")==0);
    TASK_CHECK(patch_view_uleb(".task-build/views-reverse.elf",
        ".debug_loclists",0x15,2,1)==0);
    {
        Dwarf_Error error=0;
        int rc=dwarf_init_path(".task-build/views-reverse.elf",0,0,
            DW_GROUPNUMBER_ANY,0,0,&dbg,&error);
        TASK_CHECK(rc==DW_DLV_OK);
        die=find_named_var(dbg,"value",&error);
        TASK_CHECK(die!=0);
        TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)
            ==DW_DLV_OK);
        rc=dwarf_get_loclist_views_c(die,attr,&head,&count,&error);
        TASK_CHECK(rc==DW_DLV_ERROR);
        dwarf_dealloc_attribute(attr);
        dwarf_finish(dbg);
    }
    return 0;
}

static int external_views_dwarf5(void)
{
    struct blob info, ab, loc;
    Dwarf_Debug dbg; Dwarf_Error error=0;
    Dwarf_Die die; Dwarf_Attribute attr;
    Dwarf_Loc_Head_c head; Dwarf_Unsigned count;
    Dwarf_Locview v1,v2; Dwarf_Unsigned d1=0,e1=0;
    size_t va=12, la;
    size_t vb, lb, vc, lc;

    memset(&info,0,sizeof(info));
    memset(&ab,0,sizeof(ab));
    memset(&loc,0,sizeof(loc));
    loc5_header(&loc,0,0);
    /* DIE a: external table then same-PC list. */
    ext_pair(&loc,1,2); ext_pair(&loc,0,0);
    la=loc.n; list_same_pc(&loc);
    /* DIE b: same list bytes, different companion views. */
    vb=loc.n; ext_pair(&loc,7,8); ext_pair(&loc,9,10);
    lb=loc.n; list_same_pc(&loc);
    /* DIE c: base switching. */
    vc=loc.n; ext_pair(&loc,11,12); ext_pair(&loc,13,14);
    lc=loc.n; list_base_switch(&loc);
    loc_patch_length(&loc);

    ab_common_cu5(&ab);
    ab_variable(&ab,2,DW_FORM_sec_offset,1);
    ab_end(&ab);
    info_header(&info,5); info_cu5(&info);
    info_var_sec(&info,2,"ext_a",(uint32_t)la,1,(uint32_t)va);
    info_var_sec(&info,2,"ext_b",(uint32_t)lb,1,(uint32_t)vb);
    info_var_sec(&info,2,"base_sw",(uint32_t)lc,1,(uint32_t)vc);
    b8(&info,0); info_patch(&info);
    TASK_CHECK(write_test_elf(".task-build/external5.elf",
        &info,&ab,&loc)==0);
    {
        struct vrec rs[32];
        int rn=0, found=0, i;
        TASK_CHECK(collect_path(".task-build/external5.elf",
            "ext5",rs,32,&rn)==0);
        TASK_CHECK(rn==6);
        for (i=0;i<rn;++i) {
            if (!strcmp(rs[i].name,"ext_a") && rs[i].bv==1) {
                found=1;
                TASK_CHECK(rs[i].lo==0x1000 && rs[i].hi==0x1000);
                TASK_CHECK(rs[i].has && rs[i].ev==2);
                TASK_CHECK(rs[i].source==2);
                TASK_CHECK(rs[i].voff==(Dwarf_Unsigned)va);
                TASK_CHECK(rs[i].vlen==2);
                TASK_CHECK(strstr(rs[i].status,
                    "nonempty-same-pc-view")!=0);
            }
        }
        TASK_CHECK(found);
    }

    TASK_CHECK(dwarf_init_path(".task-build/external5.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"ext_a",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    TASK_CHECK(dwarf_get_loclist_views_c(die,attr,&head,&count,
        &error)==0);
    TASK_CHECK(count==4);
    TASK_CHECK(get_view_record(head,1,&d1,&e1,&v1)==0);
    TASK_CHECK(v1.has_views && v1.source_kind==2);
    TASK_CHECK(v1.begin_view==1 && v1.end_view==2);
    TASK_CHECK(v1.section_offset==va && v1.byte_length==2);
    TASK_CHECK(get_view_record(head,2,&d1,&e1,&v2)==0);
    TASK_CHECK(v2.has_views && v2.begin_view==0 && v2.end_view==0);
    TASK_CHECK(v2.source_kind==2 && v2.section_offset==va+2);
    TASK_CHECK(v2.byte_length==2);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);

    TASK_CHECK(dwarf_init_path(".task-build/external5.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"ext_b",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    TASK_CHECK(dwarf_get_loclist_views_c(die,attr,&head,&count,
        &error)==0);
    TASK_CHECK(get_view_record(head,1,&d1,&e1,&v1)==0);
    TASK_CHECK(v1.begin_view==7 && v1.end_view==8);
    TASK_CHECK(v1.section_offset==vb);
    TASK_CHECK(get_view_record(head,2,&d1,&e1,&v2)==0);
    TASK_CHECK(v2.begin_view==9 && v2.end_view==10);
    TASK_CHECK(v2.section_offset==vb+2);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);

    TASK_CHECK(dwarf_init_path(".task-build/external5.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"base_sw",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    TASK_CHECK(dwarf_get_loclist_views_c(die,attr,&head,&count,
        &error)==0);
    TASK_CHECK(count==5); /* two bases, two ranges, end */
    TASK_CHECK(get_view_record(head,1,&d1,&e1,&v1)==0);
    TASK_CHECK(v1.begin_view==11 && v1.end_view==12);
    TASK_CHECK(get_view_record(head,3,&d1,&e1,&v2)==0);
    TASK_CHECK(v2.begin_view==13 && v2.end_view==14);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);
    return 0;
}

static int external_views_dwarf4(void)
{
    struct blob info, ab, loc;
    Dwarf_Debug dbg; Dwarf_Error error=0;
    Dwarf_Die die; Dwarf_Attribute attr;
    Dwarf_Loc_Head_c head; Dwarf_Unsigned count;
    Dwarf_Locview v1,v2; Dwarf_Unsigned d1=0,e1=0;

    memset(&info,0,sizeof(info));
    memset(&ab,0,sizeof(ab));
    memset(&loc,0,sizeof(loc));
    ext_pair(&loc,1,2); ext_pair(&loc,0,0); /* table [0,4) */
    /* Equal actual PCs form a non-end range. */
    b64(&loc,0x1000); b64(&loc,0x1000);
    b16(&loc,1); b8(&loc,DW_OP_reg0);
    b64(&loc,0x1000); b64(&loc,0x1010);
    b16(&loc,1); b8(&loc,DW_OP_reg1);
    b64(&loc,0); b64(&loc,0);

    ab_common_cu4(&ab);
    ab_variable(&ab,2,DW_FORM_sec_offset,1);
    ab_end(&ab);
    info_header(&info,4);
    b8(&info,1); /* CU abbrev, no attributes */
    info_var_sec(&info,2,"ext4",4,1,0);
    b8(&info,0); info_patch(&info);
    TASK_CHECK(write_test_loc_elf(".task-build/external4.elf",
        &info,&ab,&loc)==0);
    {
        struct vrec rs[8];
        int rn=0;
        TASK_CHECK(collect_path(".task-build/external4.elf",
            "ext4",rs,8,&rn)==0);
        TASK_CHECK(rn==2);
        TASK_CHECK(rs[0].lo==0x1000 && rs[0].hi==0x1000);
        TASK_CHECK(rs[0].has && rs[0].bv==1 && rs[0].ev==2);
        TASK_CHECK(rs[0].source==2 && rs[0].voff==0);
        TASK_CHECK(rs[0].vlen==2 && rs[0].addroff==4);
        TASK_CHECK(strstr(rs[0].status,
            "nonempty-same-pc-view")!=0);
    }

    TASK_CHECK(dwarf_init_path(".task-build/external4.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"ext4",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    TASK_CHECK(dwarf_get_loclist_views_c(die,attr,&head,&count,
        &error)==0);
    TASK_CHECK(count==2); /* DWARF4 count historically omits end */
    TASK_CHECK(get_view_record(head,0,&d1,&e1,&v1)==0);
    TASK_CHECK(v1.has_views && v1.source_kind==2);
    TASK_CHECK(v1.begin_view==1 && v1.end_view==2);
    TASK_CHECK(v1.section_offset==0 && v1.byte_length==2);
    TASK_CHECK(d1==4 && e1==22);
    TASK_CHECK(get_view_record(head,1,&d1,&e1,&v2)==0);
    TASK_CHECK(v2.has_views && v2.begin_view==0 && v2.end_view==0);
    TASK_CHECK(v2.section_offset==2 && v2.byte_length==2);
    TASK_CHECK(e1==41);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);
    return 0;
}

static int inline_views_bad_prefixes(void)
{
    int mode;
    for (mode = 0; mode < 3; ++mode) {
        struct blob info, ab, loc;
        Dwarf_Debug dbg; Dwarf_Error error=0;
        Dwarf_Die die; Dwarf_Attribute attr;
        Dwarf_Loc_Head_c head=0; Dwarf_Unsigned count=0;
        int rc;
        const char *path =
            mode == 0 ? ".task-build/dupview.elf" :
            mode == 1 ? ".task-build/baseview.elf" :
                        ".task-build/endview.elf";

        memset(&info,0,sizeof(info));
        memset(&ab,0,sizeof(ab));
        memset(&loc,0,sizeof(loc));
        loc5_header(&loc,0,0);
        if (mode == 1) {
            b8(&loc,DW_LLE_GNU_view_pair);
            buleb(&loc,1); buleb(&loc,2);
            loc5_base(&loc,0x1000);
            loc5_pair(&loc,0,0,DW_OP_reg0);
        } else {
            loc5_base(&loc,0x1000);
            if (mode == 0) {
                b8(&loc,DW_LLE_GNU_view_pair);
                buleb(&loc,1); buleb(&loc,2);
                b8(&loc,DW_LLE_GNU_view_pair);
                buleb(&loc,3); buleb(&loc,4);
            }
            loc5_pair(&loc,0,0,DW_OP_reg0);
            if (mode == 2) {
                b8(&loc,DW_LLE_GNU_view_pair);
                buleb(&loc,0); buleb(&loc,0);
            }
        }
        loc5_end(&loc);
        loc_patch_length(&loc);

        ab_common_cu5(&ab);
        ab_variable(&ab,2,DW_FORM_sec_offset,0);
        ab_end(&ab);
        info_header(&info,5); info_cu5(&info);
        info_var_sec(&info,2,"badprefix",12,0,0);
        b8(&info,0); info_patch(&info);
        TASK_CHECK(write_test_elf(path,&info,&ab,&loc)==0);
        TASK_CHECK(dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,0,0,
            &dbg,&error)==DW_DLV_OK);
        die=find_named_var(dbg,"badprefix",&error);
        TASK_CHECK(die!=0);
        TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
        rc=dwarf_get_loclist_c(attr,&head,&count,&error);
        TASK_CHECK(rc==DW_DLV_ERROR);
        if (head) dwarf_dealloc_loc_head_c(head);
        dwarf_dealloc_attribute(attr);
        dwarf_finish(dbg);
    }
    return 0;
}

static int external_views_default_skips(void)
{
    struct blob info, ab, loc;
    Dwarf_Debug dbg; Dwarf_Error error=0;
    Dwarf_Die die; Dwarf_Attribute attr;
    Dwarf_Loc_Head_c head; Dwarf_Unsigned count;
    Dwarf_Locview v; Dwarf_Unsigned d=0,e=0;

    memset(&info,0,sizeof(info));
    memset(&ab,0,sizeof(ab));
    memset(&loc,0,sizeof(loc));
    loc5_header(&loc,0,0);
    ext_pair(&loc,4,5); /* external table [12,14) */
    b8(&loc,DW_LLE_default_location);
    buleb(&loc,1); b8(&loc,DW_OP_reg0);
    loc5_pair(&loc,0,16,DW_OP_reg1);
    loc5_end(&loc); /* list starts at 14 */
    loc_patch_length(&loc);

    ab_common_cu5(&ab);
    ab_variable(&ab,2,DW_FORM_sec_offset,1);
    ab_end(&ab);
    info_header(&info,5); info_cu5(&info);
    info_var_sec(&info,2,"defview",14,1,12);
    b8(&info,0); info_patch(&info);
    TASK_CHECK(write_test_elf(".task-build/defaultview.elf",
        &info,&ab,&loc)==0);
    TASK_CHECK(dwarf_init_path(".task-build/defaultview.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"defview",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    TASK_CHECK(dwarf_get_loclist_views_c(die,attr,&head,&count,
        &error)==0);
    TASK_CHECK(count==3); /* default, range, end */
    TASK_CHECK(get_view_record(head,1,&d,&e,&v)==0);
    TASK_CHECK(v.has_views && v.begin_view==4 && v.end_view==5);
    TASK_CHECK(v.section_offset==12 && v.byte_length==2);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);
    return 0;
}

static int external_views_forms_and_bad(void)
{
    struct blob info, ab, loc;
    Dwarf_Debug dbg; Dwarf_Error error=0;
    Dwarf_Die die; Dwarf_Attribute attr;
    Dwarf_Loc_Head_c head; Dwarf_Unsigned count;
    Dwarf_Locview v; Dwarf_Unsigned d=0,e=0;
    int rc;

    /* DW_FORM_loclistx: offset table resolves to the real list. */
    memset(&info,0,sizeof(info));
    memset(&ab,0,sizeof(ab));
    memset(&loc,0,sizeof(loc));
    loc5_header(&loc,1,8); /* offsets table at 12; target 12+8=20 */
    ext_pair(&loc,5,6); ext_pair(&loc,7,8); /* 16..19 */
    list_same_pc(&loc); /* starts at 20 */
    loc_patch_length(&loc);
    ab_common_cu5(&ab);
    ab_variable(&ab,4,DW_FORM_loclistx,1);
    ab_end(&ab);
    info_header(&info,5); info_cu5(&info);
    b8(&info,4); bstr(&info,"x"); buleb(&info,0); b32(&info,16);
    b8(&info,0); info_patch(&info);
    TASK_CHECK(write_test_elf(".task-build/loclistx.elf",
        &info,&ab,&loc)==0);
    TASK_CHECK(dwarf_init_path(".task-build/loclistx.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"x",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    TASK_CHECK(dwarf_get_loclist_views_c(die,attr,&head,&count,
        &error)==0);
    TASK_CHECK(get_view_record(head,1,&d,&e,&v)==0);
    TASK_CHECK(v.source_kind==2 && v.begin_view==5 && v.end_view==6);
    TASK_CHECK(v.section_offset==16 && v.byte_length==2);
    TASK_CHECK(d==0x1d && e==0x21);
    dwarf_dealloc_loc_head_c(head);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);

    /* Truncated external ULEB immediately before the list. */
    memset(&info,0,sizeof(info));
    memset(&ab,0,sizeof(ab));
    memset(&loc,0,sizeof(loc));
    loc5_header(&loc,0,0);
    b8(&loc,0x80); /* offset 12, and list starts at 13 */
    list_same_pc(&loc);
    loc_patch_length(&loc);
    ab_common_cu5(&ab);
    ab_variable(&ab,2,DW_FORM_sec_offset,1);
    ab_end(&ab);
    info_header(&info,5); info_cu5(&info);
    info_var_sec(&info,2,"baduleb",13,1,12);
    b8(&info,0); info_patch(&info);
    TASK_CHECK(write_test_elf(".task-build/baduleb.elf",
        &info,&ab,&loc)==0);
    TASK_CHECK(dwarf_init_path(".task-build/baduleb.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"baduleb",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    rc=dwarf_get_loclist_views_c(die,attr,&head,&count,&error);
    TASK_CHECK(rc==DW_DLV_ERROR);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);

    /* Extra external byte means consumption does not reach list start. */
    memset(&info,0,sizeof(info));
    memset(&ab,0,sizeof(ab));
    memset(&loc,0,sizeof(loc));
    loc5_header(&loc,0,0);
    ext_pair(&loc,1,2); b8(&loc,9); /* table [12,15), extra byte */
    list_same_pc(&loc); /* starts 15 */
    loc_patch_length(&loc);
    ab_common_cu5(&ab);
    ab_variable(&ab,2,DW_FORM_sec_offset,1);
    ab_end(&ab);
    info_header(&info,5); info_cu5(&info);
    info_var_sec(&info,2,"extra",15,1,12);
    b8(&info,0); info_patch(&info);
    TASK_CHECK(write_test_elf(".task-build/extraviews.elf",
        &info,&ab,&loc)==0);
    TASK_CHECK(dwarf_init_path(".task-build/extraviews.elf",0,0,
        DW_GROUPNUMBER_ANY,0,0,&dbg,&error)==DW_DLV_OK);
    die=find_named_var(dbg,"extra",&error);
    TASK_CHECK(die!=0);
    TASK_CHECK(dwarf_attr(die,DW_AT_location,&attr,&error)==0);
    rc=dwarf_get_loclist_views_c(die,attr,&head,&count,&error);
    TASK_CHECK(rc==DW_DLV_ERROR);
    dwarf_dealloc_attribute(attr);
    dwarf_finish(dbg);
    return 0;
}

static int consumer_views_generate(void)
{
    struct vrec recs[64];
    int pn=0, vn=0, n=0, i;
    char command[1024];
    FILE *f;
    char line[1400];
    int saw_plain_empty=0, saw_view_nonempty=0;
    int saw_reg0=0, saw_reg1=0, saw_tsv_view=0;
    int dwarfdump_rc;

    mkdir("analysis",0755);
    TASK_CHECK(fixture_write(".task-build/plain.elf",0)==0);
    TASK_CHECK(fixture_write(".task-build/views.elf",1)==0);

    TASK_CHECK(collect_path(".task-build/plain.elf","plain.elf",
        recs,32,&pn)==0);
    TASK_CHECK(pn==2);
    TASK_CHECK(collect_path(".task-build/views.elf","views.elf",
        recs+pn,64-pn,&vn)==0);
    n=pn+vn;
    TASK_CHECK(vn==2 && n==4);
    write_tsv("analysis/location-views.tsv",recs,n);

    for (i=0;i<n;++i) {
        if (!strcmp(recs[i].object,"plain.elf") && i==0) {
            TASK_CHECK(strcmp(recs[i].name,"value")==0);
            TASK_CHECK(recs[i].lo==0x1000 && recs[i].hi==0x1000);
            TASK_CHECK(!recs[i].has);
            TASK_CHECK(strstr(recs[i].status,
                "empty-same-pc-no-view")!=0);
            TASK_CHECK(recs[i].addroff==0x15);
            TASK_CHECK(recs[i].exproff==0x19);
            saw_plain_empty=1;
        }
        if (!strcmp(recs[i].object,"views.elf") &&
            recs[i].bv==1 && recs[i].ev==2) {
            TASK_CHECK(recs[i].lo==0x1000 && recs[i].hi==0x1000);
            TASK_CHECK(recs[i].source==1);
            TASK_CHECK(recs[i].voff==0x15 && recs[i].vlen==3);
            TASK_CHECK(recs[i].addroff==0x18);
            TASK_CHECK(recs[i].exproff==0x1c);
            TASK_CHECK(strstr(recs[i].status,
                "nonempty-same-pc-view")!=0);
            saw_view_nonempty=1;
        }
        if (recs[i].exprsize==1 && recs[i].expr[0]==0x50)
            saw_reg0=1;
        if (recs[i].exprsize==1 && recs[i].expr[0]==0x51)
            saw_reg1=1;
    }
    TASK_CHECK(saw_plain_empty && saw_view_nonempty);
    TASK_CHECK(saw_reg0 && saw_reg1);

    f=fopen("analysis/location-views.tsv","r");
    TASK_CHECK(f!=0);
    while (f && fgets(line,sizeof(line),f)) {
        if (strstr(line,"views.elf") && strstr(line,"\t1\t2\t"))
            saw_tsv_view=1;
    }
    if (f) fclose(f);
    TASK_CHECK(saw_tsv_view);

    snprintf(command,sizeof(command),
        "(.task-build/src/bin/dwarfdump/dwarfdump -i "
        ".task-build/plain.elf > analysis/location-views.txt 2>&1 && "
        ".task-build/src/bin/dwarfdump/dwarfdump -i "
        ".task-build/views.elf >> analysis/location-views.txt 2>&1)");
    dwarfdump_rc=system(command);
    TASK_CHECK(dwarfdump_rc==0);
    f=fopen("analysis/location-views.txt","r");
    TASK_CHECK(f!=0);
    {
        int saw_dd_view=0, saw_dd_reg0=0, saw_dd_reg1=0;
        int saw_dd_source=0;
        while (fgets(line,sizeof(line),f)) {
            if (strstr(line,
                "views=[(0x1000,1),(0x1000,2))"))
                saw_dd_view=1;
            if (strstr(line,"view_offset=0x15"))
                saw_dd_source=1;
            if (strstr(line,"DW_OP_reg0")) saw_dd_reg0=1;
            if (strstr(line,"DW_OP_reg1")) saw_dd_reg1=1;
        }
        fclose(f);
        TASK_CHECK(saw_dd_view && saw_dd_source);
        TASK_CHECK(saw_dd_reg0 && saw_dd_reg1);
    }

    snprintf(command,sizeof(command),
        ".task-build/src/bin/dwarfdump/dwarfdump "
        "--print-raw-loclists .task-build/views.elf > "
        ".task-build/raw-loclists.txt 2>&1");
    TASK_CHECK(system(command)==0);
    f=fopen(".task-build/raw-loclists.txt","r");
    TASK_CHECK(f!=0);
    {
        int saw_raw_view=0;
        while (fgets(line,sizeof(line),f)) {
            if (strstr(line,"DW_LLE_GNU_view_pair") &&
                strstr(line,"0x00000001") &&
                strstr(line,"0x00000002"))
                saw_raw_view=1;
        }
        fclose(f);
        TASK_CHECK(saw_raw_view);
    }
    return 0;
}

const struct task_case task_cases[] = {
    {"inline_views","fixture_inline_views",inline_views_fixture},
    {"inline_views","mutate_first_uleb_pair",inline_views_mutation},
    {"inline_views","reject_duplicate_or_misplaced_prefix",
        inline_views_bad_prefixes},
    {"external_views","dwarf5_companion_tables",external_views_dwarf5},
    {"external_views","dwarf4_companion_table",external_views_dwarf4},
    {"external_views","default_record_consumes_no_view",
        external_views_default_skips},
    {"external_views","loclistx_truncated_and_extra",
        external_views_forms_and_bad},
    {"consumer_views","analysis_tsv_and_dwarfdump",consumer_views_generate}
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
