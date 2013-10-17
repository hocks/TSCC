/*
 * External interface to auto-growing string.
 *
 * Copyright (C) 2002 Pete Wyckoff <pw@osc.edu>
 *
 * $Id: growstr.h 326 2006-01-24 21:35:26Z pw $
 */
typedef struct {
    char *s;
    int len;
    int max;
    int translate_single_quote;
} growstr_t;

extern growstr_t *growstr_init_empty(void);
extern growstr_t *growstr_init(void);
extern void growstr_free(growstr_t *g);
extern void growstr_zero(growstr_t *g);
extern void growstr_append(growstr_t *g, const char *s);
extern void growstr_printf(growstr_t *g, const char *format, ...) ATTR_PRINTF2;
