/*
   Copyright (c) 2015,2016 Piotr Stolarz
   Scoped properties configuration library

   Distributed under the 2-clause BSD License (the License)
   see accompanying file LICENSE for details.

   This software is distributed WITHOUT ANY WARRANTY; without even the
   implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the License for more information.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sp_props/parser.h"

/* path separators markers */
#define C_SEP_SCP    '/'
#define C_SEP_TYP    ':'
#define C_SEP_SIND   '@'

/* scope index markers */
#define C_IND_ALL   '*'
#define C_IND_LAST  '$'

#define EXEC_RG(c) if ((ret=(c))!=SPEC_SUCCESS) goto finish;
#define CHK_FSEEK(c) if ((c)!=0) { ret=SPEC_ACCS_ERR; goto finish; }

/* to be used inside callbacks only */
#define CMPLOC_RG(ph, tkn, loc, str, len) { \
    int equ=0; \
    ret = (sp_errc_t)sp_parser_tkn_cmp((ph), (tkn), (loc), (str), (len), &equ); \
    if (ret!=SPEC_SUCCESS) goto finish; \
    if (!equ) goto finish; \
}

/* exported; see header for details */
sp_errc_t sp_check_syntax(
    FILE *in, const sp_loc_t *p_parsc, int *p_line, int *p_col)
{
    sp_errc_t ret=SPEC_SUCCESS;
    sp_parser_hndl_t phndl;

    if (!in) { ret=SPEC_INV_ARG; goto finish; }

    EXEC_RG(sp_parser_hndl_init(
        &phndl, in, p_parsc, NULL, NULL, NULL, EOL_DETECT));
    EXEC_RG(sp_parse(&phndl));

finish:
    if (ret==SPEC_SYNTAX) {
        if (p_line) *p_line = phndl.err.loc.line;
        if (p_col) *p_col = phndl.err.loc.col;
    }
    return ret;
}

typedef struct _path_t
{
    const char *beg;        /* start pointer */
    const char *end;        /* end pointer (exclusive) */
    const char *deftp;      /* default scope type */
} path_t;

typedef struct _lastsc_t
{
    int present;        /* if !=0: the struct describes last scope */
    const char *beg;    /* points to part of original path spec.  related to
                           the scope content (beginning of its path; end of
                           the path as in the original path) */
    sp_loc_t lbody;     /* scope body; for scopes w/o body line/column
                           indexes are zeroed (unset state) */
    sp_loc_t ldef;      /* scope definition */
} lastsc_t;

/* Base struct for all (read-only/update) handlers.

   NOTE: Along with its deriving structs, the struct is copied during
   upward-downward process of following a destination scope path.
 */
typedef struct _base_hndl_t
{
    /* pointer to the finish flag (shared) */
    int *p_finish;

    /* pointer to last scope spec. (shared) */
    lastsc_t *p_lsc;

    /* pointer to split scope tracking index (shared) */
    int *p_sind;

    /* path to the destination scope (not propagated) */
    path_t path;
} base_hndl_t;

/* Initialize base_hndl_t struct.

   NOTE: The function may cut last part of the path and exclude it from the path
   specification. In this case 'p_lprt' shall be !=NULL. A pointer to the last
   part will be written under 'p_lprt'.
 */
static void init_base_hndl(
    base_hndl_t *p_b, int *p_finish, lastsc_t *p_lsc, int *p_sind,
    const char *path, const char **p_lprt, const char *deftp)
{
    p_b->p_finish = p_finish;
    *p_finish = 0;

    p_b->p_lsc = p_lsc;
    memset(p_b->p_lsc, 0, sizeof(*p_b->p_lsc));

    p_b->p_sind = p_sind;
    *p_sind = -1;

    if (p_lprt)
    {
        /* cut last part of the path spec. */
        *p_lprt = strrchr(path, C_SEP_SCP);
        if (*p_lprt) {
            p_b->path.beg = path;
            p_b->path.end = (*p_lprt)++;
        } else {
            *p_lprt = path;
            p_b->path.beg = p_b->path.end = NULL;
        }
    } else {
        p_b->path.beg = path;
        p_b->path.end = (!path ? NULL : p_b->path.beg+strlen(path));
    }

    if (p_b->path.beg && *p_b->path.beg==C_SEP_SCP) p_b->path.beg++;
    p_b->path.deftp = deftp;
}

/* Iteration handle

   NOTE: This struct is copied during upward-downward process of following
   a destination scope path, so any changes made on it are not propagated to
   scopes on higher or the same scope level. Therefore, if such propagation is
   required a struct's field must be an immutable pointer to an object
   containing propagated information.
 */
typedef struct _iter_hndl_t
{
    base_hndl_t b;

    /* parsed input file handle (const) */
    FILE *in;

    struct {
        /* argument passed untouched (const) */
        void *arg;

        /* user API callbacks (const) */
        sp_cb_prop_t prop;
        sp_cb_scope_t scope;
    } cb;

    /* buffer 1 (property name/scope type name; const) */
    struct {
        char *ptr;
        size_t sz;
    } buf1;

    /* buffer 2 (property vale/scope name; const) */
    struct {
        char *ptr;
        size_t sz;
    } buf2;
} iter_hndl_t;

/* Extract index value from prop/scope name and write the result under 'p_ind'.
   Number of chars read and constituting the index specification is written under
   'p_ind_len' (0 if such spec. is absent). If the spec. is present but erroneous
   SPEC_INV_PATH is returned.
 */
static sp_errc_t get_ind_from_name(
    const char *name, size_t nm_len, int *p_ind, size_t *p_ind_len)
{
    sp_errc_t ret=SPEC_SUCCESS;
    size_t i;

    *p_ind=0;
    *p_ind_len=0;

    for (i=nm_len, name+=nm_len-1;
        i && *name!=C_SEP_SIND;
        i--, name--);

    if (!i)
        /* no index spec. */
        goto finish;

     *p_ind_len=nm_len-i+1;
     if (*p_ind_len <= 1) {
        /* no chars after marker */
        ret=SPEC_INV_PATH;
        goto finish;
    }

    name++;

    if (*p_ind_len==2 && (*name==C_IND_ALL || *name==C_IND_LAST)) {
        *p_ind = (*name==C_IND_LAST ? IND_LAST : IND_ALL);
    } else {
        /* parse decimal number after marker */
        for (i=1; i<*p_ind_len; i++, name++) {
            if (*name>='0' && *name<='9') {
                *p_ind = *p_ind*10 + (*name-'0');
            } else {
                /* invalid number */
                *p_ind=0;
                ret=SPEC_INV_PATH;
                goto finish;
            }
        }
    }

finish:
    return ret;
}

/* Follow requested path up to the destination scope.

   The function accepts a clone of enclosing scope handle pointed by 'ph_nst'
   which is next updated (actually its base part pointed by 'ph_nstb'), to
   represent nesting scope. The nesting scope is followed if its characteristic
   meets scope criteria provided in the path).
 */
static sp_errc_t follow_scope_path(const sp_parser_hndl_t *p_phndl,
    base_hndl_t *ph_nstb, void *ph_nst, const sp_loc_t *p_ltype,
    const sp_loc_t *p_lname, const sp_loc_t *p_lbody, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;

    int ind, *p_sind=ph_nstb->p_sind;
    size_t typ_len=0, nm_len=0, ind_len;
    const char *type=NULL, *name=NULL;
    const path_t *p_path = &ph_nstb->path;
    const char *beg=p_path->beg, *end=p_path->end;
    const char *col=strchr(beg, C_SEP_TYP), *sl=strchr(beg, C_SEP_SCP);

    if (sl) end=sl;
    if (col>=end) col=NULL;

    if (col) {
        /* type and name specified */
        type = beg;
        typ_len = col-beg;
        name = col+1;
        nm_len = end-name;
    } else
    if (p_path->deftp)
    {
        /* scope with default type */
        type = p_path->deftp;
        typ_len = strlen(type);
        name = beg;
        nm_len = end-beg;
    }

    if (!nm_len) { ret=SPEC_INV_PATH; goto finish; }

    EXEC_RG(get_ind_from_name(name, nm_len, &ind, &ind_len));
    if (!(nm_len-=ind_len)) { ret=SPEC_INV_PATH; goto finish; }
    if (!ind_len) ind=IND_ALL;  /* if not specified, IND_ALL is assumed */

    CMPLOC_RG(p_phndl, SP_TKN_ID, p_ltype, type, typ_len);
    CMPLOC_RG(p_phndl, SP_TKN_ID, p_lname, name, nm_len);

    /* scope with matching name found */

    if (ind!=IND_ALL)
        /* tracking index is updated only if the matched
           scope was provided with an index specification */
        *p_sind += 1;

    if (ind==IND_LAST)
    {
        /* for last scope spec. simply track the scope */
        ph_nstb->p_lsc->present = 1;
        ph_nstb->p_lsc->beg = (!*end ? end : end+1);
        if (p_lbody) {
            ph_nstb->p_lsc->lbody = *p_lbody;
        } else {
            memset(&ph_nstb->p_lsc->lbody, 0, sizeof(ph_nstb->p_lsc->lbody));
        }
        ph_nstb->p_lsc->ldef = *p_ldef;
    } else
    if (ind==IND_ALL || *p_sind==ind)
    {
        /* follow the path for matching index */
        ph_nstb->path.beg = (!*end ? end : end+1);

        if (p_lbody)
        {
            int sind = -1;
            sp_parser_hndl_t phndl;

            if (ind!=IND_ALL)
                /* there is a need to start tracking in the followed scope */
                ph_nstb->p_sind = &sind;

            EXEC_RG(sp_parser_hndl_init(
                &phndl, p_phndl->in, p_lbody, p_phndl->cb.prop,
                p_phndl->cb.scope, ph_nst, p_phndl->lex.eol_typ));
            EXEC_RG(sp_parse(&phndl));
        }
    }

finish:
    return ret;
}

/* Call follow_scope_path() for cloned nested scope handle 'hndl' and check
   the finish flag afterward. To be used inside scope callbacks only.
 */
#define CALL_FOLLOW_SCOPE_PATH(hndl) \
    ret = follow_scope_path( \
        p_phndl, &hndl.b, &hndl, p_ltype, p_lname, p_lbody, p_ldef); \
    if (ret==SPEC_SUCCESS && *hndl.b.p_finish!=0) \
        ret = SPEC_CB_FINISH;

/* check iteration callback return code */
#define __CHK_ITER_CB_RET() \
    if (ret==SPEC_CB_FINISH) \
        *p_ihndl->b.p_finish = 1; \
    else if ((int)ret<0) \
        ret=SPEC_CB_RET_ERR;

/* sp_iterate() callback: property */
static sp_errc_t iter_cb_prop(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_lname, const sp_loc_t *p_lval, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    iter_hndl_t *p_ihndl = (iter_hndl_t*)p_phndl->cb.arg;

    /* ignore props until the destination scope */
    if ((p_ihndl->b.path.beg >= p_ihndl->b.path.end) && p_ihndl->cb.prop)
    {
        sp_tkn_info_t tkname, tkval;

        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_ID,
            p_lname, p_ihndl->buf1.ptr, p_ihndl->buf1.sz, &tkname.len));
        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_VAL,
            p_lval, p_ihndl->buf2.ptr, p_ihndl->buf2.sz, &tkval.len));

        tkname.loc = *p_lname;
        if (p_lval) tkval.loc = *p_lval;

        ret = p_ihndl->cb.prop(p_ihndl->cb.arg, p_ihndl->in, p_ihndl->buf1.ptr,
            &tkname, p_ihndl->buf2.ptr, (p_lval ? &tkval : NULL), p_ldef);

        __CHK_ITER_CB_RET();
    }
finish:
    return ret;
}

/* sp_iterate() callback: scope */
static sp_errc_t iter_cb_scope(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_ltype, const sp_loc_t *p_lname, const sp_loc_t *p_lbody,
    const sp_loc_t *p_lbdyenc, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    iter_hndl_t *p_ihndl=(iter_hndl_t*)p_phndl->cb.arg;

    if (p_ihndl->b.path.beg < p_ihndl->b.path.end) {
        iter_hndl_t ihndl = *p_ihndl;
        CALL_FOLLOW_SCOPE_PATH(ihndl);
    } else
    if (p_ihndl->cb.scope)
    {
        sp_tkn_info_t tktype, tkname;

        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_ID,
            p_ltype, p_ihndl->buf1.ptr, p_ihndl->buf1.sz, &tktype.len));
        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_ID,
            p_lname, p_ihndl->buf2.ptr, p_ihndl->buf2.sz, &tkname.len));

        if (p_ltype) tktype.loc = *p_ltype;
        tkname.loc = *p_lname;

        ret = p_ihndl->cb.scope(p_ihndl->cb.arg, p_ihndl->in,
            p_ihndl->buf1.ptr, (p_ltype ? &tktype : NULL),
            p_ihndl->buf2.ptr, &tkname, p_lbody, p_lbdyenc, p_ldef);

        __CHK_ITER_CB_RET();
    }
finish:
    return ret;
}

#undef __CHK_ITER_CB_RET

#define __PARSE_WITH_LSC_HANDLING(hndl) \
    for (;;) { \
        sp_loc_t lsc_bdy; \
        EXEC_RG(sp_parse(&phndl)); \
        if (lsc.present && !f_finish) { \
            /* last scope spec. detected; need to re-parse the last scope */ \
            if (!lsc.lbody.first_column) { \
                break; /* empty scope; skip further processing */ \
            } \
            hndl.b.path.beg = lsc.beg; \
            lsc_bdy = lsc.lbody; \
            memset(&lsc, 0, sizeof(lsc)); \
            sind = -1; \
            EXEC_RG(sp_parser_hndl_init(&phndl, phndl.in, &lsc_bdy, \
                phndl.cb.prop, phndl.cb.scope, &hndl, phndl.lex.eol_typ)); \
        } else break; \
    }

/* exported; see header for details */
sp_errc_t sp_iterate(FILE *in, const sp_loc_t *p_parsc, const char *path,
    const char *deftp, sp_cb_prop_t cb_prop, sp_cb_scope_t cb_scope,
    void *arg, char *buf1, size_t b1len, char *buf2, size_t b2len)
{
    sp_errc_t ret=SPEC_SUCCESS;
    iter_hndl_t ihndl;
    sp_parser_hndl_t phndl;

    /* processing finish flag (shared);
       set to 1 if user callback requested to stop further iteration */
    int f_finish;
    /* last scope spec. (shared) */
    lastsc_t lsc;
    /* split scope tracking index (shared) */
    int sind;

    if (!in || (!cb_prop && !cb_scope)) {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    EXEC_RG(sp_parser_hndl_init(
        &phndl, in, p_parsc, iter_cb_prop, iter_cb_scope, &ihndl, EOL_DETECT));

    /* prepare callback handle */
    memset(&ihndl, 0, sizeof(ihndl));
    init_base_hndl(&ihndl.b, &f_finish, &lsc, &sind, path, NULL, deftp);

    ihndl.in = in;

    ihndl.cb.arg = arg;
    ihndl.cb.prop = cb_prop;
    ihndl.cb.scope = cb_scope;

    if (b1len) {
        ihndl.buf1.ptr = buf1;
        ihndl.buf1.sz = b1len-1;
        ihndl.buf1.ptr[ihndl.buf1.sz] = 0;
    }
    if (b2len) {
        ihndl.buf2.ptr = buf2;
        ihndl.buf2.sz = b2len-1;
        ihndl.buf2.ptr[ihndl.buf2.sz] = 0;
    }

    __PARSE_WITH_LSC_HANDLING(ihndl);

finish:
    return ret;
}

typedef struct _prop_dsc_t
{
    const char *name;
    size_t nm_len;

    int ind;
} prop_dsc_t;

typedef struct _scope_dsc_t
{
    const char *type;
    size_t typ_len;

    const char *name;
    size_t nm_len;

    int ind;
} scope_dsc_t;

/* sp_get_prop() iteration handle struct.

   NOTE: This struct is copied during upward-downward process of following
   a destination scope path.
 */
typedef struct _getprp_hndl_t
{
    base_hndl_t b;

    /* property desc. (const) */
    prop_dsc_t prop;

    /* matched props tracking index (shared) */
    int *p_pind;

    /* property value buffer (const) */
    struct {
        char *ptr;
        size_t sz;
    } val;

    /* extra info will be written under this address (shared) */
    sp_prop_info_ex_t *p_info;
} getprp_hndl_t;

/* sp_get_prop() callback: property */
static sp_errc_t getprp_cb_prop(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_lname, const sp_loc_t *p_lval, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    getprp_hndl_t *p_gphndl = (getprp_hndl_t*)p_phndl->cb.arg;

    /* ignore props until the destination scope */
    if (p_gphndl->b.path.beg >= p_gphndl->b.path.end)
    {
        CMPLOC_RG(p_phndl, SP_TKN_ID,
            p_lname, p_gphndl->prop.name, p_gphndl->prop.nm_len);
        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_VAL, p_lval,
            p_gphndl->val.ptr, p_gphndl->val.sz, &p_gphndl->p_info->tkval.len));

        /* matching property found */
        *p_gphndl->p_pind += 1;

        if (p_gphndl->prop.ind==*p_gphndl->p_pind ||
            p_gphndl->prop.ind==IND_LAST)
        {
            p_gphndl->p_info->tkname.len = p_gphndl->prop.nm_len;
            p_gphndl->p_info->tkname.loc = *p_lname;
            if (p_lval) {
                p_gphndl->p_info->val_pres = 1;
                p_gphndl->p_info->tkval.loc = *p_lval;
            } else {
                p_gphndl->p_info->val_pres = 0;
            }
            p_gphndl->p_info->ldef = *p_ldef;

            /* done if there is no need to track last property */
            if (p_gphndl->prop.ind!=IND_LAST) {
                ret = SPEC_CB_FINISH;
                *p_gphndl->b.p_finish = 1;
            }
        }
    }
finish:
    return ret;
}

/* sp_get_prop() callback: scope */
static sp_errc_t getprp_cb_scope(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_ltype, const sp_loc_t *p_lname, const sp_loc_t *p_lbody,
    const sp_loc_t *p_lbdyenc, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    getprp_hndl_t *p_gphndl=(getprp_hndl_t*)p_phndl->cb.arg;

    if (p_gphndl->b.path.beg < p_gphndl->b.path.end) {
        getprp_hndl_t gphndl = *p_gphndl;
        CALL_FOLLOW_SCOPE_PATH(gphndl);
    }
    return ret;
}

/* exported; see header for details */
sp_errc_t sp_get_prop(FILE *in, const sp_loc_t *p_parsc, const char *name,
    int ind, const char *path, const char *deftp, char *val, size_t len,
    sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_SUCCESS;
    getprp_hndl_t gphndl;
    sp_parser_hndl_t phndl;

    /* processing finish flag (shared)
       set to 1 if requested property has been found
       (doesn't apply for the last prop referenced by IND_LAST) */
    int f_finish;
    /* last scope spec. (shared) */
    lastsc_t lsc;
    /* split scope tracking index (shared) */
    int sind;
    /* matched props tracking index (shared) */
    int pind;

    /* line & columns are 1-based, therefore 0 has
       a special meaning to distinguish unset state */
    sp_prop_info_ex_t info;
    memset(&info, 0, sizeof(info));

    if (!in || !len ||
        (!name && !path) ||
        (ind<0 && ind!=IND_LAST && ind!=IND_INPROP))
    {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    EXEC_RG(sp_parser_hndl_init(&phndl, in,
        p_parsc, getprp_cb_prop, getprp_cb_scope, &gphndl, EOL_DETECT));

    /* prepare callback handle */
    memset(&gphndl, 0, sizeof(gphndl));
    init_base_hndl(
        &gphndl.b, &f_finish, &lsc, &sind, path, (!name ? &name : NULL), deftp);

    gphndl.prop.name = name;
    gphndl.prop.nm_len = strlen(name);

    if (ind==IND_INPROP) {
        size_t ind_len;

        EXEC_RG(get_ind_from_name(name, gphndl.prop.nm_len, &ind, &ind_len));
        if (ind==IND_ALL || !(gphndl.prop.nm_len-=ind_len)) {
            ret=SPEC_INV_PATH;
            goto finish;
        }
        /* if not specified, 0 is assumed */
        if (!ind_len) ind=0;
    }

    gphndl.prop.ind = ind;
    pind = -1;
    gphndl.p_pind = &pind;

    gphndl.val.ptr = val;
    gphndl.val.sz = len-1;
    gphndl.val.ptr[gphndl.val.sz] = 0;

    gphndl.p_info = &info;

    __PARSE_WITH_LSC_HANDLING(gphndl);

    /* check if the location of the property definition
       has not been set, therefore has not been found */
    if (!info.ldef.first_column) ret=SPEC_NOTFOUND;

finish:
    if (p_info) *p_info=info;
    return ret;

}

/* Update 'str' by trimming trailing spaces. Updated string length is returned.
 */
static size_t strtrim(char *str)
{
    size_t len = strlen(str);
    for (; len && isspace((int)str[len-1]); len--) str[len-1]=0;
    return len;
}

/* exported; see header for details */
sp_errc_t sp_get_prop_int(FILE *in, const sp_loc_t *p_parsc, const char *name,
    int ind, const char *path, const char *deftp, long *p_val,
    sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_SUCCESS;
    sp_prop_info_ex_t info;
    char val[80], *end;
    long v=0L;

    EXEC_RG(sp_get_prop(
        in, p_parsc, name, ind, path, deftp, val, sizeof(val), &info));
    if (!info.val_pres || info.tkval.len>=sizeof(val) || !strtrim(val)) {
        ret=SPEC_VAL_ERR;
        goto finish;
    }

    errno = 0;
    v = strtol(val, &end, 0);
    if (errno==ERANGE) { ret=SPEC_VAL_ERR; goto finish; }

    if (*end) ret=SPEC_VAL_ERR;

finish:
    if (p_info) *p_info=info;
    if (p_val) *p_val=v;
    return ret;
}

/* exported; see header for details */
sp_errc_t sp_get_prop_float(FILE *in, const sp_loc_t *p_parsc, const char *name,
    int ind, const char *path, const char *deftp, double *p_val,
    sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_SUCCESS;
    sp_prop_info_ex_t info;
    char val[80], *end;
    double v=0.0;

    EXEC_RG(sp_get_prop(
        in, p_parsc, name, ind, path, deftp, val, sizeof(val), &info));
    if (!info.val_pres || info.tkval.len>=sizeof(val) || !strtrim(val)) {
        ret=SPEC_VAL_ERR;
        goto finish;
    }

    errno = 0;
    v = strtod(val, &end);
    if (errno==ERANGE) { ret=SPEC_VAL_ERR; goto finish; }

    if (*end) ret=SPEC_VAL_ERR;

finish:
    if (p_info) *p_info=info;
    if (p_val) *p_val=v;
    return ret;
}

static int __stricmp(const char *str1, const char *str2)
{
    int ret;
    size_t i=0;

    for (; !((ret=tolower((int)str1[i])-tolower((int)str2[i]))) &&
        str1[i] && str2[i]; i++);
    return ret;
}

/* exported; see header for details */
sp_errc_t sp_get_prop_enum(
    FILE *in, const sp_loc_t *p_parsc, const char *name, int ind,
    const char *path, const char *deftp, const sp_enumval_t *p_evals,
    int igncase, char *buf, size_t blen, int *p_val, sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_SUCCESS;
    int v=0;

    sp_prop_info_ex_t info;
    memset(&info, 0, sizeof(info));

    if (!p_evals) { ret=SPEC_INV_ARG; goto finish; }

    EXEC_RG(sp_get_prop(in, p_parsc, name, ind, path, deftp, buf, blen, &info));

    /* remove leading/trailing spaces */
    for (; isspace((int)*buf); buf++);
    strtrim(buf);

    for (; p_evals->name; p_evals++)
    {
        if (strlen(p_evals->name) >= blen) { ret=SPEC_SIZE; goto finish; }

        if (!(igncase ? __stricmp(p_evals->name, buf) :
            strcmp(p_evals->name, buf))) { v=p_evals->val; break; }
    }
    if (!p_evals->name || (info.val_pres && info.tkval.len>=blen))
        ret=SPEC_VAL_ERR;

finish:
    if (p_info) *p_info=info;
    if (p_val) *p_val=v;
    return ret;
}

typedef struct _addh_frst_sc_t
{
    sp_loc_t lbdyenc;
    sp_loc_t ldef;
} addh_frst_sc_t;

/* Base struct for update handlers.

   NOTE: Along with its deriving structs, the struct is copied during
   upward-downward process of following a destination scope path.
 */
typedef struct _base_updt_hndl_t
{
    /* input/output file handles (const) */
    FILE *in;
    FILE *out;

    /* file offset staring not processed range of the input (shared) */
    long *p_in_off;

    /* type of EOL detected (const) */
    eol_t eol_typ;
} base_updt_hndl_t;

/* Initialize base_updt_hndl_t struct.
 */
static void init_base_updt_hndl(
    base_updt_hndl_t *p_bu, FILE *in, FILE *out, long *p_in_off,
    const sp_parser_hndl_t *p_phndl)
{
    p_bu->in = in;
    p_bu->out = out;
    p_bu->p_in_off = p_in_off;
    *p_in_off = 0;

    p_bu->eol_typ = (p_phndl->lex.eol_typ!=EOL_UNDEF ?  p_phndl->lex.eol_typ :
#if defined(_WIN32) || defined(_WIN64)
        EOL_CRLF
#else
        EOL_LF
#endif
        );
}

/* Copies input file bytes (from the offset staring not processed range) to
   the output file up to 'end' offset (exclusive). If end==EOF input is copied
   up to the end of the file. In case of success (and there is something to
   copy) the file offset is set at 'end'.
 */
static sp_errc_t cpy_to_out(base_updt_hndl_t *p_bu, long end)
{
    sp_errc_t ret=SPEC_SUCCESS;
    long beg = *p_bu->p_in_off;

    if (beg<end || end==EOF) {
        CHK_FSEEK(fseek(p_bu->in, beg, SEEK_SET));
        for (; beg<end || end==EOF; beg++) {
            int c = fgetc(p_bu->in);
            if (c==EOF && end==EOF) break;
            if (c==EOF || fputc(c, p_bu->out)==EOF) {
                ret=SPEC_ACCS_ERR;
                goto finish;
            }
        }
        *p_bu->p_in_off = beg;
    }
finish:
    return ret;
}

/* Cut input file spaces (from the offset starting not processed range) up to
   EOL (inclusive) or a first non-space character. The function returns !=0 if
   cut spaces constitute a line (EOL finished), 0 otherwise.
 */
static int cutsp_to_eol(base_updt_hndl_t *p_bu)
{
    /* endc - 0:non-space, 1:EOL, 2:EOF */
    int c, endc;

    if (fseek(p_bu->in, *p_bu->p_in_off, SEEK_SET)) {
        endc=2;
        goto finish;
    }

    for (endc=0;
        !endc && isspace(c=fgetc(p_bu->in)) && c!='\v' && c!='\f';
        (*p_bu->p_in_off)++)
    {
        if (c!='\r' && c!='\n') continue;
        else endc=1;

        /* in case of EOL mismatch, cut mismatching char and treat it as EOL */
        if (p_bu->eol_typ==EOL_CRLF && c=='\r')
            if ((c=fgetc(p_bu->in))!=EOF && c=='\n') (*p_bu->p_in_off)++;
    }
    if (c==EOF) endc=2;

finish:
    return (endc==1);
}

/* Property/scope addition iteration handle struct.

   NOTE: This struct is copied during upward-downward process of following
   a destination scope path.
 */
typedef struct _add_hndl_t
{
    base_hndl_t b;
    base_updt_hndl_t bu;

    /* element position number (const) */
    int n_elem;
    /* element position number tracking index (shared) */
    int *p_eind;

    /* first, non-global scope matching the path;
       zero initialized - unset (shared) */
    addh_frst_sc_t *p_frst_sc;

    /* definition's location of an element associated with requested
       position; if unset - the position was not found (shared) */
    sp_loc_t *p_ldef_elem;
} add_hndl_t;

#define __TRACK_ELEM_POSITION() \
    /* count element in the matched scope */ \
    *p_ahndl->p_eind += 1; \
    if (p_ahndl->n_elem==*p_ahndl->p_eind || \
        p_ahndl->n_elem==IND_LAST) \
    { \
        /* save element's ldef associated with requested position */ \
        *p_ahndl->p_ldef_elem = *p_ldef; \
        /* done if there is no need to track last position */ \
        if (p_ahndl->n_elem!=IND_LAST) { \
            ret = SPEC_CB_FINISH; \
            *p_ahndl->b.p_finish = 1; \
        } \
    }

/* Property/scope addition callback: property */
static sp_errc_t add_cb_prop(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_lname, const sp_loc_t *p_lval, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    add_hndl_t *p_ahndl=(add_hndl_t*)p_phndl->cb.arg;

    if (p_ahndl->b.path.beg >= p_ahndl->b.path.end) {
        __TRACK_ELEM_POSITION();
    }
    return ret;
}

/* Property/scope addition callback: scope */
static sp_errc_t add_cb_scope(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_ltype, const sp_loc_t *p_lname, const sp_loc_t *p_lbody,
    const sp_loc_t *p_lbdyenc, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    add_hndl_t *p_ahndl=(add_hndl_t*)p_phndl->cb.arg;

    if (p_ahndl->b.path.beg < p_ahndl->b.path.end)
    {
        add_hndl_t ahndl = *p_ahndl;
        CALL_FOLLOW_SCOPE_PATH(ahndl);

        if (ahndl.b.p_lsc->present &&
            ahndl.b.p_lsc->beg >= ahndl.b.path.end &&
            ahndl.b.p_lsc->ldef.beg==p_ldef->beg &&
            ahndl.b.p_lsc->ldef.end==p_ldef->end)
        {
            /* track last scope and write under 'p_frst_sc'
               if the scope finishes the path */
            p_ahndl->p_frst_sc->lbdyenc = *p_lbdyenc;
            p_ahndl->p_frst_sc->ldef = *p_ldef;
        } else
        if (ahndl.b.path.beg >= ahndl.b.path.end &&
            !p_ahndl->p_frst_sc->ldef.first_column)
        {
            /* mark first matching, non-global scope */
            p_ahndl->p_frst_sc->lbdyenc = *p_lbdyenc;
            p_ahndl->p_frst_sc->ldef = *p_ldef;
        }
    } else {
        __TRACK_ELEM_POSITION();
    }
    return ret;
}

#undef __TRACK_ELEM_POSITION

/* Add prop/scope element.
 */
/* static */ sp_errc_t add_elem(FILE *in, FILE *out, const sp_loc_t *p_parsc,
    const char *prop_nm, const char *prop_val, const char *sc_typ,
    const char *sc_nm, int n_elem, const char *path, const char *deftp,
    unsigned flags)
{
    sp_errc_t ret=SPEC_SUCCESS;
    add_hndl_t ahndl;
    sp_parser_hndl_t phndl;

    /* processing finish flag (shared)
       set to 1 if requested element position has been found
       (doesn't apply for the last position referenced by IND_LAST) */
    int f_finish;
    /* last scope spec. (shared) */
    lastsc_t lsc;
    /* split scope tracking index (shared) */
    int sind;
    /* element position number tracking index (shared) */
    int eind = 0;
    /* first scope matching the path (shared) */
    addh_frst_sc_t frst_sc = {};
    /* ldef of an element associated with requested position (shared) */
    sp_loc_t ldef_elem = {};
    /* file offset staring not processed range of the input (shared) */
    long in_off;

    if (!in || !out ||
        (!prop_nm && !sc_nm) ||
        (n_elem<0 && n_elem!=IND_LAST))
    {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    EXEC_RG(sp_parser_hndl_init(
        &phndl, in, p_parsc, add_cb_prop, add_cb_scope, &ahndl, EOL_DETECT));

    /* prepare callback handle */
    memset(&ahndl, 0, sizeof(ahndl));
    init_base_hndl(&ahndl.b, &f_finish, &lsc, &sind, path, NULL, deftp);
    init_base_updt_hndl(&ahndl.bu, in, out, &in_off, &phndl);

    ahndl.n_elem = n_elem;
    ahndl.p_eind = &eind;
    ahndl.p_frst_sc = &frst_sc;
    ahndl.p_ldef_elem = &ldef_elem;

    __PARSE_WITH_LSC_HANDLING(ahndl);

    if ((ahndl.b.path.beg < ahndl.b.path.end) && !frst_sc.ldef.first_column)
    {
        /* path specified but the destination not found */
        ret=SPEC_NOTFOUND;
        goto finish;
    }

    if ((n_elem && n_elem!=IND_LAST) && !ldef_elem.first_column)
    {
        /* requested position not found */
        ret=SPEC_NOTFOUND;
        goto finish;
    }

    if (n_elem && (n_elem!=IND_LAST || ldef_elem.first_column))
    {
        /* add after n-th elem */
// @@@
printf("adding after element; %d.%d|%d.%d\n",
    ldef_elem.first_line, ldef_elem.first_column, ldef_elem.last_line, ldef_elem.last_column);
    } else {
        if (frst_sc.ldef.first_column)
        {
            /* add at scope beginning */
// @@@
printf("adding at scope beg; %d.%d|%d.%d\n",
    frst_sc.ldef.first_line, frst_sc.ldef.first_column, frst_sc.ldef.last_line, frst_sc.ldef.last_column);
        } else
        {
            /* add at file beginning */
// @@@
printf("adding at file beg\n");
        }
    }

finish:
    return ret;
}

#undef __PARSE_WITH_LSC_HANDLING
