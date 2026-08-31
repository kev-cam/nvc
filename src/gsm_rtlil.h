//
//  gsm_rtlil.h — function table for libgsm.so's direct-RTLIL construction
//  facade (gsm_rtlil_* — see sv2ghdl yosys/gen_statemachine.cpp).  nvc never
//  includes a yosys header: the builder is reached through these pointers,
//  resolved by dlsym in model.c (accel_gsm_rtlil_api) and consumed by the
//  vhdl2rtlil walker backend in vhdl2vlog.c.
//
#ifndef _GSM_RTLIL_H
#define _GSM_RTLIL_H

typedef struct {
   int (*begin)(const char *log_path);
   int (*module)(const char *name);
   int (*wire)(const char *name, int width, int dir, const char *init_bits);
   int (*connect)(const char *lhs, const char *rhs);
   int (*cell_bin)(const char *op, const char *name, const char *a,
                   const char *b, const char *y, int is_signed);
   int (*cell_un)(const char *op, const char *name, const char *a,
                  const char *y, int is_signed);
   int (*cell_mux)(const char *name, const char *a, const char *b,
                   const char *s, const char *y);
   int (*cell_inst)(const char *type, const char *name, const char *conns);
   int (*proc)(const char *name);
   int (*sync)(const char *edge, const char *sig);
   int (*sync_assign)(const char *lhs, const char *rhs);
   int (*case_assign)(const char *lhs, const char *rhs);
   int (*switch_begin)(const char *sig);
   int (*case_begin)(const char *compare);
   int (*case_end)(void);
   int (*switch_end)(void);
   unsigned long long (*content_hash)(void);
   int (*synth)(int nargs, const char *const *args);
   void (*abort_session)(void);
} gsm_rtlil_api_t;

// model.c: the resolved api, or NULL when libgsm lacks the surface.
const gsm_rtlil_api_t *accel_gsm_rtlil_api(void);

#endif  // _GSM_RTLIL_H
