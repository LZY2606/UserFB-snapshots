/*
  Copyright (c) 2025. All rights reserved.

  Implementation of GNU location-view association and the public
  Dwarf_Locview accessor.
*/

#include <config.h>

#include <string.h>

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_loc.h"

static Dwarf_Bool
view_desc_is_range(Dwarf_Loc_Head_c head, Dwarf_Locdesc_c desc)
{
    if (!head || !desc) {
        return FALSE;
    }
    if (head->ll_lkind == DW_LKIND_loclists) {
        switch (desc->ld_lle_value) {
        case DW_LLE_startx_endx:
        case DW_LLE_startx_length:
        case DW_LLE_offset_pair:
        case DW_LLE_start_end:
        case DW_LLE_start_length:
            return TRUE;
        default:
            return FALSE;
        }
    }
    if (head->ll_lkind == DW_LKIND_loclist ||
        head->ll_lkind == DW_LKIND_GNU_exp_list) {
        return desc->ld_lle_value == DW_LLE_offset_pair ||
            desc->ld_lle_value == DW_LLEX_offset_pair_entry ||
            desc->ld_lle_value == DW_LLEX_start_end_entry ||
            desc->ld_lle_value == DW_LLEX_start_length_entry;
    }
    return FALSE;
}

DW_API int
dwarf_get_locdesc_views(Dwarf_Locdesc_c locdesc,
    Dwarf_Locview *view,
    Dwarf_Error *error)
{
    if (!locdesc) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_NULL,
            "DW_DLE_ATTR_NULL: NULL Dwarf_Locdesc_c passed to "
            "dwarf_get_locdesc_views()");
        return DW_DLV_ERROR;
    }
    if (!view) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_NULL,
            "DW_DLE_ATTR_NULL: NULL Dwarf_Locview passed to "
            "dwarf_get_locdesc_views()");
        return DW_DLV_ERROR;
    }
    memset(view, 0, sizeof(*view));
    view->has_views = locdesc->ld_view_present ? TRUE : FALSE;
    view->begin_view = locdesc->ld_begin_view;
    view->end_view = locdesc->ld_end_view;
    view->source_kind = locdesc->ld_view_source;
    view->section_offset = locdesc->ld_view_section_offset;
    view->byte_length = locdesc->ld_view_byte_length;
    return DW_DLV_OK;
}

static int
attach_external_views(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c head,
    Dwarf_Unsigned views_offset,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s *section = 0;
    Dwarf_Small *section_data = 0;
    Dwarf_Unsigned section_size = 0;
    Dwarf_Unsigned list_start = head->ll_llearea_offset;
    Dwarf_Small *cur = 0;
    Dwarf_Small *table_end = 0;
    Dwarf_Unsigned i = 0;
    int loadres = DW_DLV_OK;

    if (head->ll_lkind == DW_LKIND_loclists) {
        section = &dbg->de_debug_loclists;
    } else if (head->ll_lkind == DW_LKIND_loclist ||
        head->ll_lkind == DW_LKIND_GNU_exp_list) {
        section = &dbg->de_debug_loc;
    } else {
        _dwarf_error_string(dbg, error, DW_DLE_LOCLISTS_ERROR,
            "DW_DLE_LOCLISTS_ERROR: DW_AT_GNU_locviews requires "
            "a .debug_loc or .debug_loclists list");
        return DW_DLV_ERROR;
    }
    if (!section->dss_data) {
        loadres = _dwarf_load_section(dbg, section, error);
        if (loadres != DW_DLV_OK) {
            return loadres;
        }
    }
    section_data = section->dss_data;
    section_size = section->dss_size;
    if (list_start > section_size || views_offset > list_start) {
        _dwarf_error_string(dbg, error, DW_DLE_LOCLIST_OFFSET_BAD,
            "DW_DLE_LOCLIST_OFFSET_BAD: external GNU view table "
            "is not immediately before its location list");
        return DW_DLV_ERROR;
    }
    cur = section_data + views_offset;
    table_end = section_data + list_start;

    for (i = 0; i < head->ll_locdesc_count; ++i) {
        Dwarf_Locdesc_c desc = head->ll_locdesc + i;
        Dwarf_Unsigned begin_view = 0;
        Dwarf_Unsigned end_view = 0;
        Dwarf_Unsigned first_len = 0;
        Dwarf_Unsigned second_len = 0;
        Dwarf_Unsigned pair_offset = 0;

        if (!view_desc_is_range(head, desc)) {
            continue;
        }
        pair_offset = (Dwarf_Unsigned)(cur - section_data);
        DECODE_LEB128_UWORD_LEN_CK(cur, begin_view, first_len,
            dbg, error, table_end);
        DECODE_LEB128_UWORD_LEN_CK(cur, end_view, second_len,
            dbg, error, table_end);
        desc->ld_view_present = TRUE;
        desc->ld_begin_view = begin_view;
        desc->ld_end_view = end_view;
        desc->ld_view_source = DW_LOCVIEW_SOURCE_EXTERNAL;
        desc->ld_view_section_offset = pair_offset;
        desc->ld_view_byte_length = first_len + second_len;
    }
    if (cur != table_end) {
        _dwarf_error_string(dbg, error, DW_DLE_LOCLISTS_ERROR,
            "DW_DLE_LOCLISTS_ERROR: external GNU view table does "
            "not end exactly at the location-list start");
        return DW_DLV_ERROR;
    }
    return DW_DLV_OK;
}

static int
validate_view_order(Dwarf_Debug dbg, Dwarf_Loc_Head_c head,
    Dwarf_Error *error)
{
    Dwarf_Unsigned i = 0;

    for (i = 0; i < head->ll_locdesc_count; ++i) {
        Dwarf_Locdesc_c desc = head->ll_locdesc + i;

        if (!view_desc_is_range(head, desc) || desc->ld_index_failed) {
            continue;
        }
        if (desc->ld_lopc > desc->ld_highpc) {
            _dwarf_error_string(dbg, error, DW_DLE_LOCLISTS_ERROR,
                "DW_DLE_LOCLISTS_ERROR: location range is in "
                "reverse PC order");
            return DW_DLV_ERROR;
        }
        if (desc->ld_view_present &&
            desc->ld_lopc == desc->ld_highpc &&
            desc->ld_begin_view > desc->ld_end_view) {
            _dwarf_error_string(dbg, error, DW_DLE_LOCLISTS_ERROR,
                "DW_DLE_LOCLISTS_ERROR: location views are in "
                "reverse lexicographic order");
            return DW_DLV_ERROR;
        }
    }
    return DW_DLV_OK;
}

DW_API int
dwarf_get_loclist_views_c(Dwarf_Die die,
    Dwarf_Attribute attr,
    Dwarf_Loc_Head_c *head_out,
    Dwarf_Unsigned *count_out,
    Dwarf_Error *error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Attribute views_attr = 0;
    Dwarf_Loc_Head_c head = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Off views_offset = 0;
    int res = 0;
    int attrres = DW_DLV_OK;

    if (head_out) {
        *head_out = 0;
    }
    if (count_out) {
        *count_out = 0;
    }
    if (!die || !attr) {
        _dwarf_error_string(NULL, error, DW_DLE_DIE_NULL,
            "DW_DLE_DIE_NULL: NULL Dwarf_Die or Dwarf_Attribute "
            "passed to dwarf_get_loclist_views_c()");
        return DW_DLV_ERROR;
    }
    dbg = die->di_cu_context ? die->di_cu_context->cc_dbg : 0;
    CHECK_DBG(dbg, error, "dwarf_get_loclist_views_c()");

    res = dwarf_get_loclist_c(attr, &head, &count, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    attrres = dwarf_attr(die, DW_AT_GNU_locviews, &views_attr, error);
    if (attrres == DW_DLV_ERROR) {
        dwarf_dealloc_loc_head_c(head);
        return DW_DLV_ERROR;
    }
    if (attrres == DW_DLV_OK) {
        Dwarf_Debug view_dbg = head->ll_dbg ? head->ll_dbg : dbg;

        res = dwarf_global_formref(views_attr, &views_offset, error);
        if (res == DW_DLV_OK) {
            res = attach_external_views(view_dbg, head,
                (Dwarf_Unsigned)views_offset, error);
        }
        dwarf_dealloc_attribute(views_attr);
        if (res != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(head);
            return res;
        }
    }
    res = validate_view_order(head->ll_dbg ? head->ll_dbg : dbg,
        head, error);
    if (res != DW_DLV_OK) {
        dwarf_dealloc_loc_head_c(head);
        return res;
    }
    if (head_out) {
        *head_out = head;
    }
    if (count_out) {
        *count_out = count;
    }
    return DW_DLV_OK;
}
