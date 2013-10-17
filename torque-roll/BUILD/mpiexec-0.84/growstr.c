/*
 * Auto-growing string with optional translation of ' -> '\'' to
 * escape shell * quoting.
 *
 * Copyright (C) 2002 Pete Wyckoff <pw@osc.edu>
 *
 * $Id: growstr.c 326 2006-01-24 21:35:26Z pw $
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "util.h"
#include "growstr.h"

static const int growstr_grow_chunk = 200;

/*
 * Grow the line.
 */
static void
growstr_grow(growstr_t *g)
{
    char *x;

    g->max += growstr_grow_chunk;
    x = Malloc(g->max * sizeof(*x));
    if (g->len) {
	memcpy(x, g->s, g->len);
	free(g->s);
    }
    g->s = x;
    g->s[g->len] = '\0';
}

growstr_t *
growstr_init_empty(void)
{
    growstr_t *g = Malloc(sizeof(*g));
    g->len = 0;
    g->max = 0;
    g->translate_single_quote = 0;
    g->s = NULL;
    return g;
}

/*
 * Init and allocate one chunk, assumes it will be used now.
 */
growstr_t *
growstr_init(void)
{
    growstr_t *g = growstr_init_empty();
    growstr_grow(g);
    return g;
}

void
growstr_zero(growstr_t *g)
{
    g->len = 0;
    if (g->s)
	g->s[0] = '\0';
}

void
growstr_free(growstr_t *g)
{
    if (g->s)
	free(g->s);
    free(g);
}

void
growstr_append(growstr_t *g, const char *s)
{
    const char *cp;

    for (cp=s; *cp; cp++) {
	if (g->len + 5 >= g->max)
	    growstr_grow(g);

	if (*cp == '\'' && g->translate_single_quote) {
	    /* close ongoing single quote string */
	    g->s[g->len++] = '\'';
	    /* literal backslash */
	    g->s[g->len++] = '\\';
	    g->s[g->len++] = '\\';
	    /* literal single-quote */
	    g->s[g->len++] = '\\';
	    g->s[g->len++] = '\'';
	    /* restart ongoing single quote string */
	    g->s[g->len++] = '\'';
	} else
	    g->s[g->len++] = *cp;
    }
    g->s[g->len] = 0;
}

/*
 * Appending printf, does not reinitialize the string.
 */
void
growstr_printf(growstr_t *g, const char *format, ...)
{
    growstr_t *h = growstr_init();

    for (;;) {
	int n;
	va_list ap;

	/* must reinit ap every loop as vsnprintf will change it */
	va_start(ap, format);
	n = vsnprintf(h->s, h->max, format, ap);
	va_end(ap);

	if (n < h->max) {
	    h->len = n;
	    break;
	}
	while (n >= h->max)
	    growstr_grow(h);
    }
    growstr_append(g, h->s);
    growstr_free(h);
}
