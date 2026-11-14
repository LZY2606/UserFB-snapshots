#ifndef FD_PATH_H
#define FD_PATH_H
#include "fadec.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Compact flag bits, not architectural RFLAGS bit positions. */
enum { FD_PATH_CF=1, FD_PATH_PF=2, FD_PATH_AF=4,
       FD_PATH_ZF=8, FD_PATH_SF=16, FD_PATH_OF=32 };
typedef enum {
    FD_PATH_OK=0, FD_PATH_INVALID, FD_PATH_DECODE,
    FD_PATH_UNSUPPORTED, FD_PATH_LIMIT, FD_PATH_NOMEM
} FdPathStatus;
typedef struct {
    /* Array indices are FD_REG_AX .. FD_REG_R15. Only known bits constrain
       the entry value; bits outside known_mask are ignored. */
    uint64_t value[16], known_mask[16];
    uint8_t flags_value, flags_known; /* low six bits only */
} FdPathInput;
typedef struct {
    size_t offset; /* byte offset of the executed logical instruction */
    uint8_t af;    /* its selected undefined AF, either 0 or 1 */
} FdPathChoice;
typedef struct {
    uint64_t entry[16];
    uint8_t entry_flags; /* compact low six bits */
    FdPathChoice *choices; /* one per executed TEST/AND/OR/XOR, path order */
    size_t choice_count;
} FdPathWitness;
typedef struct {
    size_t source, destination;
    uint8_t taken;    /* 0=fallthrough, 1=jump target */
    uint8_t feasible; /* exactly 0 or 1 */
    FdPathWitness witness; /* all zero if infeasible */
} FdPathEdge;
typedef struct {
    FdPathEdge *edges; /* all Jcc sites, source ascending then taken 0,1 */
    size_t edge_count;
    size_t error_offset; /* SIZE_MAX on success or non-position errors */
} FdPathReport;
/* code is a complete linear instruction partition, no embedded data, len>0.
   Decode the entire partition at address 0, in 64-bit mode; structural checks
   cover unreachable instructions too. Only ordinary near RET with no immediate
   is terminal. Only the sixteen ordinary arithmetic Jcc conditions are included
   (not LOOP/JCXZ). No branch or fallthrough may leave [0,len), no cycles or
   overlapping instructions. Prefixes accepted by fd_decode for the supported
   register-only forms have their normal arithmetic semantics.

   report must initially be zero, or be released before reuse. node_limit>0;
   representation budget counts allocated symbolic Boolean nodes, excluding
   the two constants. Different representations may exhaust at different limits;
   budget failures are explicit, never a proof of infeasibility. Include working
   carry/borrow-chain and diamond examples using four free 64-bit GPRs with
   node_limit=1000000; other more complex formulas may exhaust this budget.
   Do not enumerate all input values or all complete paths instead of sharing
   symbolic relations. Memory operands, indirect control, calls and unsupported
   opcodes anywhere return UNSUPPORTED. Bad bytes return DECODE, non-boundary
   targets return INVALID. Other bad arguments return INVALID. No partial edges
   on any failure; error_offset identifies the earliest structural/decode issue
   by byte offset, SIZE_MAX for allocation/budget/argument errors.

   Known register/flag masks constrain all witnesses. Undefined AF choices are
   existential, fresh per logical instruction occurrence, reused until changed,
   and included in witnesses so a concrete interpreter can replay each edge.
   edge witnesses are not required to be lexicographically minimal. Reports own
   all storage, never retain input pointers, and do not execute target bytes.
*/
FdPathStatus fd_path_analyze(const uint8_t *code, size_t len,
                           const FdPathInput *input, size_t node_limit,
                           FdPathReport *report);
void fd_path_free(FdPathReport *report); /* safe for a zero report; reset to zero */
#ifdef __cplusplus
}
#endif
#endif
