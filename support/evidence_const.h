/* evidence_const.h -- Numeric DWARF constants for the evidence tools.

   The form/attribute enums used internally by dwarf.c are file-local.
   The report and test drivers only need a small, fixed subset covered
   by support/FORMAT.md, so those values are named here with distinct
   EVIDENCE_ prefixes to avoid any clash with the library enums.  */

#ifndef EVIDENCE_CONST_H
#define EVIDENCE_CONST_H

#define EVIDENCE_FORM_ADDR		0x01
#define EVIDENCE_FORM_DATA2		0x05
#define EVIDENCE_FORM_DATA4		0x06
#define EVIDENCE_FORM_DATA8		0x07
#define EVIDENCE_FORM_STRING		0x08
#define EVIDENCE_FORM_DATA1		0x0b
#define EVIDENCE_FORM_STRP		0x0e
#define EVIDENCE_FORM_REF_ADDR		0x10
#define EVIDENCE_FORM_REF4		0x13
#define EVIDENCE_FORM_SEC_OFFSET	0x17
#define EVIDENCE_FORM_FLAG_PRESENT	0x19

#define EVIDENCE_AT_NAME		0x03
#define EVIDENCE_AT_LOW_PC		0x11
#define EVIDENCE_AT_HIGH_PC		0x12
#define EVIDENCE_AT_COMP_DIR		0x1b
#define EVIDENCE_AT_ABSTRACT_ORIGIN	0x31
#define EVIDENCE_AT_DECL_COLUMN		0x39
#define EVIDENCE_AT_DECL_FILE		0x3a
#define EVIDENCE_AT_DECL_LINE		0x3b
#define EVIDENCE_AT_SPECIFICATION	0x47
#define EVIDENCE_AT_CALL_COLUMN		0x57
#define EVIDENCE_AT_CALL_FILE		0x58
#define EVIDENCE_AT_CALL_LINE		0x59

#endif
