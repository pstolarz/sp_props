/*
   Copyright (c) 2015 Piotr Stolarz
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

#define EXEC_RG(c) if ((ret=(c))!=SPEC_SUCCESS) goto finish;

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

    EXEC_RG(sp_parser_hndl_init(&phndl, in, p_parsc, NULL, NULL, NULL));
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
    const char *defsc;      /* default scope */
} path_t;

/* iteration handle - modification specific struct */
typedef struct _mod_iter_hndl_t
{
    /* output file handle storing provided modifications */
    FILE *out;

    /* temporary output file handle used by a scope callback for scope body
       modifications */
    FILE *sc_out;

    /* file offset staring not processed range of the input file; -1: EOF */
    long in_off;
} mod_iter_hndl_t;

/* iteration handle - base struct

   NOTE: This struct is cloned during upward-downward process of following
   a destination scope path, so any changes made on it are not propagated to
   scopes on higher or the same scope level. Therefore, if such propagation is
   required a struct's field must be an immutable pointer to an object
   containing propagated information (virtual inheritance).
 */
typedef struct _iter_hndl_t
{
    /* parsed input file handle */
    FILE *in;

    /* path to the destination scope
       NOTE: mutable object not propagated between scopes. */
    path_t path;

    struct {
        /* argument passed untouched */
        void *arg;

        /* user API callbacks */
        sp_cb_prop_t prop;
        sp_cb_scope_t scope;
    } cb;

    /* buffer 1 (property name/scope type name) */
    struct {
        char *ptr;
        size_t sz;
    } buf1;

    /* buffer 2 (property vale/scope name) */
    struct {
        char *ptr;
        size_t sz;
    } buf2;

    /* modification iteration specific handle */
    mod_iter_hndl_t *p_mihndl;
} iter_hndl_t;

/* Follow requested path up to the destination scope. 'p_pargcpy' is a pointer
   to a clone of a parser's callback arg-object associated with the scope being
   followed. Path object pointed by 'p_path' is modified accordingly to the
   scope being followed.
 */
static sp_errc_t follow_scope_path(const sp_parser_hndl_t *p_phndl,
    path_t *p_path, const sp_loc_t *p_ltype, const sp_loc_t *p_lname,
    const sp_loc_t *p_lbody, void *p_pargcpy)
{
    sp_errc_t ret=SPEC_SUCCESS;
    size_t typ_len=0, nm_len=0;
    const char *type=NULL, *name=NULL, *beg=p_path->beg, *end=p_path->end;
    const char *col=strchr(beg, ':'), *sl=strchr(beg, '/');

    if (sl) end=sl;
    if (col>=end) col=NULL;

    if (col) {
        /* type ':' name */
        type = beg;
        typ_len = col-beg;
        name = col+1;
        nm_len = end-name;
    } else
    if (p_path->defsc)
    {
        /* scope with default type */
        type = p_path->defsc;
        typ_len = strlen(type);
        name = beg;
        nm_len = end-beg;
    }

    if (!nm_len) { ret=SPEC_INV_PATH; goto finish; }

    CMPLOC_RG(p_phndl, SP_TKN_ID, p_ltype, type, typ_len);
    CMPLOC_RG(p_phndl, SP_TKN_ID, p_lname, name, nm_len);

    /* matched scope found; follow the path */
    if (p_lbody) {
        sp_parser_hndl_t phndl;
        p_path->beg = (!*end ? end : end+1);

        EXEC_RG(sp_parser_hndl_init(&phndl, p_phndl->in, p_lbody,
            p_phndl->cb.prop, p_phndl->cb.scope, p_pargcpy));
        EXEC_RG(sp_parse(&phndl));
    }
finish:
    return ret;
}

/* sp_iterate() callback: property */
static sp_errc_t iter_cb_prop(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_lname, const sp_loc_t *p_lval, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    iter_hndl_t *p_ihndl = (iter_hndl_t*)p_phndl->cb.arg;

    /* ignore props until the destination scope */
    if (p_ihndl->path.beg>=p_ihndl->path.end && p_ihndl->cb.prop)
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
    }
finish:
    return ret;
}

/* sp_iterate() callback: scope */
static sp_errc_t iter_cb_scope(
    const sp_parser_hndl_t *p_phndl, const sp_loc_t *p_ltype,
    const sp_loc_t *p_lname, const sp_loc_t *p_lbody, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    iter_hndl_t *p_ihndl=(iter_hndl_t*)p_phndl->cb.arg;

    if (p_ihndl->path.beg < p_ihndl->path.end)
    {
        /* clone the handle for the scope being followed */
        iter_hndl_t ihndl = *p_ihndl;
        ret = follow_scope_path(
            p_phndl, &ihndl.path, p_ltype, p_lname, p_lbody, &ihndl);
    } else
    if (p_ihndl->cb.scope)
    {
        sp_tkn_info_t tktype, tkname;
        FILE *sc_out = (p_ihndl->p_mihndl ? p_ihndl->p_mihndl->sc_out : NULL);

        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_ID,
            p_ltype, p_ihndl->buf1.ptr, p_ihndl->buf1.sz, &tktype.len));
        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_ID,
            p_lname, p_ihndl->buf2.ptr, p_ihndl->buf2.sz, &tkname.len));

        if (p_ltype) tktype.loc = *p_ltype;
        tkname.loc = *p_lname;

        ret = p_ihndl->cb.scope(p_ihndl->cb.arg, p_ihndl->in, sc_out,
            p_ihndl->buf1.ptr, (p_ltype ? &tktype : NULL),
            p_ihndl->buf2.ptr, &tkname, p_lbody, p_ldef);
    }
finish:
    return ret;
}

/* Initialize iter_hndl_t handle */
static void init_iter_hndl(iter_hndl_t *p_ihndl, FILE *in, const char *path,
    const char *defsc, sp_cb_prop_t cb_prop, sp_cb_scope_t cb_scope, void *arg,
    char *p_buf1, size_t b1len, char *p_buf2, size_t b2len,
    mod_iter_hndl_t *p_mihndl)
{
    memset(p_ihndl, 0, sizeof(*p_ihndl));

    p_ihndl->in = in;

    if (b1len) {
        p_ihndl->buf1.ptr = p_buf1;
        p_ihndl->buf1.sz = b1len-1;
        p_ihndl->buf1.ptr[p_ihndl->buf1.sz] = 0;
    }
    if (b2len) {
        p_ihndl->buf2.ptr = p_buf2;
        p_ihndl->buf2.sz = b2len-1;
        p_ihndl->buf2.ptr[p_ihndl->buf2.sz] = 0;
    }

    if (path && *path=='/') path++;
    p_ihndl->path.beg = path;
    p_ihndl->path.end = (!path ? NULL : p_ihndl->path.beg+strlen(path));
    p_ihndl->path.defsc = defsc;

    p_ihndl->cb.arg = arg;
    p_ihndl->cb.prop = cb_prop;
    p_ihndl->cb.scope = cb_scope;

    p_ihndl->p_mihndl = p_mihndl;
}

/* exported; see header for details */
sp_errc_t sp_iterate(FILE *in, const sp_loc_t *p_parsc, const char *path,
    const char *defsc, sp_cb_prop_t cb_prop, sp_cb_scope_t cb_scope,
    void *arg, char *p_buf1, size_t b1len, char *p_buf2, size_t b2len)
{
    sp_errc_t ret=SPEC_SUCCESS;
    iter_hndl_t ihndl;
    sp_parser_hndl_t phndl;

    if (!in || (!cb_prop && !cb_scope)) {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    init_iter_hndl(&ihndl, in, path, defsc, cb_prop, cb_scope, arg,
        p_buf1, b1len, p_buf2, b2len, NULL);

    EXEC_RG(sp_parser_hndl_init(
        &phndl, in, p_parsc, iter_cb_prop, iter_cb_scope, &ihndl));
    EXEC_RG(sp_parse(&phndl));

finish:
    return ret;
}

/* sp_get_prop() iteration handle struct.

   NOTE: This struct is cloned during upward-downward process of following
   a destination scope path.
 */
typedef struct _getprp_hndl_t
{
    /* path to the destination scope
       NOTE: mutable object not propagated between scopes. */
    path_t path;

    /* property name */
    const char *name;

    /* property value buffer */
    struct {
        char *ptr;
        size_t sz;
    } val;

    /* extra info will be written under this address */
    sp_prop_info_ex_t *p_info;
} getprp_hndl_t;

/* sp_get_prop() callback: property */
static sp_errc_t getprp_cb_prop(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_lname, const sp_loc_t *p_lval, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    getprp_hndl_t *p_gphndl = (getprp_hndl_t*)p_phndl->cb.arg;

    /* ignore props until the destination scope */
    if (p_gphndl->path.beg>=p_gphndl->path.end)
    {
        CMPLOC_RG(p_phndl, SP_TKN_ID, p_lname, p_gphndl->name, (size_t)-1);
        EXEC_RG(sp_parser_tkn_cpy(p_phndl, SP_TKN_VAL, p_lval,
            p_gphndl->val.ptr, p_gphndl->val.sz, &p_gphndl->p_info->tkval.len));

        /* property found; done */
        p_gphndl->p_info->tkname.len = strlen(p_gphndl->name);
        p_gphndl->p_info->tkname.loc = *p_lname;
        if (p_lval) {
            p_gphndl->p_info->val_pres = 1;
            p_gphndl->p_info->tkval.loc = *p_lval;
        } else {
            p_gphndl->p_info->val_pres = 0;
        }
        p_gphndl->p_info->ldef = *p_ldef;

        ret = SPEC_CB_FINISH;
    }
finish:
    return ret;
}

/* sp_get_prop() callback: scope */
static sp_errc_t getprp_cb_scope(
    const sp_parser_hndl_t *p_phndl, const sp_loc_t *p_ltype,
    const sp_loc_t *p_lname, const sp_loc_t *p_lbody, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    getprp_hndl_t *p_gphndl=(getprp_hndl_t*)p_phndl->cb.arg;

    if (p_gphndl->path.beg < p_gphndl->path.end)
    {
        /* clone the handle for the scope being followed */
        getprp_hndl_t gphndl = *p_gphndl;
        ret = follow_scope_path(
            p_phndl, &gphndl.path, p_ltype, p_lname, p_lbody, &gphndl);
    }
    return ret;
}

/* exported; see header for details */
sp_errc_t sp_get_prop(FILE *in, const sp_loc_t *p_parsc, const char *name,
    const char *path, const char *defsc, char *p_val, size_t len,
    sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_INV_ARG;
    getprp_hndl_t gphndl;
    sp_parser_hndl_t phndl;

    sp_prop_info_ex_t info;
    memset(&info, 0, sizeof(info));

    if (!in || !len) goto finish;

    /* prepare callback handle */
    memset(&gphndl, 0, sizeof(gphndl));

    gphndl.val.ptr = p_val;
    gphndl.val.sz = len-1;
    gphndl.val.ptr[gphndl.val.sz] = 0;

    if (!name) {
        /* property name provided as part of the path spec. */
        if (!path) goto finish;

        name = strrchr(path, '/');
        if (name) {
            gphndl.path.beg = path;
            gphndl.path.end = name++;
        } else {
            name = path;
            gphndl.path.beg = gphndl.path.end = NULL;
        }

        if (strchr(name, ':')) goto finish;
    } else {
        gphndl.path.beg = path;
        gphndl.path.end = (!path ? NULL : gphndl.path.beg+strlen(path));
    }

    gphndl.name = name;

    if (gphndl.path.beg && *gphndl.path.beg=='/') gphndl.path.beg++;
    gphndl.path.defsc = defsc;

    gphndl.p_info = &info;

    EXEC_RG(sp_parser_hndl_init(
        &phndl, in, p_parsc, getprp_cb_prop, getprp_cb_scope, &gphndl));
    EXEC_RG(sp_parse(&phndl));

    /* line & columns are 1-based, if 0, the location of the property
        definition has not been set, therefore has not been found */
    if (!info.ldef.first_line) ret=SPEC_NOTFOUND;

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
    const char *path, const char *defsc, long *p_val, sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_SUCCESS;
    sp_prop_info_ex_t info;
    char val[80], *end;
    long v=0L;

    EXEC_RG(
        sp_get_prop(in, p_parsc, name, path, defsc, val, sizeof(val), &info));

    if (!info.val_pres || info.tkval.len>=sizeof(val) || !strtrim(val)) {
        ret=SPEC_VAL_ERR;
        goto finish;
    }

    v = strtol(val, &end, 0);
    if ((v==LONG_MIN || v==LONG_MAX) && errno==ERANGE) {
        ret=SPEC_VAL_ERR;
        goto finish;
    }

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
    FILE *in, const sp_loc_t *p_parsc, const char *name, const char *path,
    const char *defsc, const sp_enumval_t *p_evals, int igncase,
    char *p_buf, size_t blen, int *p_val, sp_prop_info_ex_t *p_info)
{
    sp_errc_t ret=SPEC_SUCCESS;
    int v=0;

    sp_prop_info_ex_t info;
    memset(&info, 0, sizeof(info));

    if (!p_evals) { ret=SPEC_INV_ARG; goto finish; }

    EXEC_RG(sp_get_prop(in, p_parsc, name, path, defsc, p_buf, blen, &info));

    /* remove leading/trailing spaces */
    for (; isspace((int)*p_buf); p_buf++);
    strtrim(p_buf);

    for (; p_evals->name; p_evals++)
    {
        if (strlen(p_evals->name) >= blen) { ret=SPEC_SIZE; goto finish; }

        if (!(igncase ? __stricmp(p_evals->name, p_buf) :
            strcmp(p_evals->name, p_buf))) { v=p_evals->val; break; }
    }
    if (!p_evals->name || (info.val_pres && info.tkval.len>=blen))
        ret=SPEC_VAL_ERR;

finish:
    if (p_info) *p_info=info;
    if (p_val) *p_val=v;
    return ret;
}

#define __CHK_MOD_ITER_RET(mb, nb) \
    if ((int)ret>0 || p_ihndl->path.beg<p_ihndl->path.end) goto finish; \
    cb_bf = sp_cb_errc((int)ret); \
    is_mod = cb_bf & (mb); \
    is_del = cb_bf & SP_CBEC_FLG_DEL_DEF; \
    if ((is_mod && is_del) || ((cb_bf & (nb)) && !strlen(name))) \
    { ret=SPEC_CB_RET_ERR; goto finish; }

#define __CHK_FSEEK(c) if ((c)!=0) { ret=SPEC_ACCS_ERR; goto finish; }
#define __CHK_FERR(c) if ((c)<0) { ret=SPEC_ACCS_ERR; goto finish; }

/* Copies input file bytes (from the last input offset) to the output file up to
   'end' offset (exclusive). If end==EOF input is copied up to the end of the
   file.
 */
static sp_errc_t cpy_to_out(iter_hndl_t *p_ihndl, long end)
{
    sp_errc_t ret=SPEC_ACCS_ERR;
    mod_iter_hndl_t *p_mihndl = p_ihndl->p_mihndl;
    long beg = p_mihndl->in_off;
    FILE *in = p_ihndl->in;

    if (beg==EOF) goto finish;

    if (beg<end || end==EOF)
    {
        __CHK_FSEEK(fseek(in, beg, SEEK_SET));
        for (; beg<end || end==EOF; beg++) {
            int c = fgetc(in);
            if (c==EOF && end==EOF) break;
            if (c==EOF || fputc(c, p_mihndl->out)==EOF) goto finish;
        }
        p_mihndl->in_off = end;
    }
    ret = SPEC_SUCCESS;

finish:
    return ret;
}

/* Trim line spaces starting from the last input offset. Trimming is finished on
   the first non-space character or EOL (which may be deleted or not depending
   on 'del_eol'). The function returns !=0 if trimmed spaces constitute a line
   (finished by EOL), 0 otherwise.
 */
static int trim_line(iter_hndl_t *p_ihndl, eol_t eol_typ, int del_eol)
{
    int c, endc=2;  /* 0:non-space, 1:EOL, 2:EOF */
    mod_iter_hndl_t *p_mihndl = p_ihndl->p_mihndl;
    FILE *in = p_ihndl->in;

    if (p_mihndl->in_off==EOF || fseek(in, p_mihndl->in_off, SEEK_SET))
        goto finish;

    for (endc=0; !endc && isspace(c=fgetc(in)); p_mihndl->in_off++)
    {
        if (c!='\r' && c!='\n') continue;

        switch (eol_typ)
        {
        case EOL_LF:
            if (c=='\n') endc=1;
            break;

        case EOL_CR:
        case EOL_CRLF:
            if (c=='\r') {
                if (eol_typ==EOL_CR) endc=1;
                else {
                    c = fgetc(in);
                    if (c!=EOF) {
                        p_mihndl->in_off++;
                        if (c=='\n') endc=1;
                    } else endc=2;
                }
            }
            break;

        /* to make a compiler happy; will never happen */
        default: break;
        }
    }

    if (endc==1 && !del_eol)
        p_mihndl->in_off -= (eol_typ==EOL_CRLF ? 2 : 1);
finish:
    return (endc==1);
}

/* Delete a definition */
static sp_errc_t
    del_loc(iter_hndl_t *p_ihndl, const sp_loc_t *p_loc, eol_t eol_typ)
{
    sp_errc_t ret=SPEC_SUCCESS;
    long org_off, end_off, del_beg=p_loc->beg;
    int is_line;

    /* detect end offset of definition being deleted */
    org_off = p_ihndl->p_mihndl->in_off;
    p_ihndl->p_mihndl->in_off = p_loc->end+1;
    is_line = trim_line(p_ihndl, eol_typ, 0);
    end_off = p_ihndl->p_mihndl->in_off;
    p_ihndl->p_mihndl->in_off = org_off;

    if (is_line) {
        /* remove leading spaces */
        int n_lcol=p_loc->first_column-1, n_lsp=n_lcol, i;

        if (n_lcol && !fseek(p_ihndl->in, p_loc->beg-n_lcol, SEEK_SET)) {
            for (i=n_lcol; i; i--)
                if (!isspace(fgetc(p_ihndl->in))) n_lsp=i-1;
            del_beg -= n_lsp;
        }
        if (n_lsp==n_lcol)
            /* remove empty line */
            end_off += (eol_typ==EOL_CRLF ? 2 : 1);
    }

    EXEC_RG(cpy_to_out(p_ihndl, del_beg));
    p_ihndl->p_mihndl->in_off = end_off;

finish:
    return ret;
}

/* sp_iterate_modify() callback: property */
static sp_errc_t mod_iter_cb_prop(const sp_parser_hndl_t *p_phndl,
    const sp_loc_t *p_lname, const sp_loc_t *p_lval, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    int cb_bf=0, is_mod, is_del;
    iter_hndl_t *p_ihndl = (iter_hndl_t*)p_phndl->cb.arg;
    eol_t eol_typ = p_phndl->lex.eol_typ;

    char *name = p_ihndl->buf1.ptr;
    char *val = p_ihndl->buf2.ptr;

    mod_iter_hndl_t *p_mihndl = p_ihndl->p_mihndl;
    FILE *out = p_mihndl->out;

    /* follow destination path and call property callback */
    ret = iter_cb_prop(p_phndl, p_lname, p_lval, p_ldef);
    __CHK_MOD_ITER_RET(SP_CBEC_FLG_MOD_PROP_NAME|SP_CBEC_FLG_MOD_PROP_VAL,
        SP_CBEC_FLG_MOD_PROP_NAME);

    if (!cb_bf || cb_bf==SP_CBEC_FLG_FINISH)
        /* nothing to modify */
        goto finish;

    if (is_del) {
        EXEC_RG(del_loc(p_ihndl, p_ldef, eol_typ));
    } else
    {
        /* property name
         */
        if (cb_bf & SP_CBEC_FLG_MOD_PROP_NAME) {
            EXEC_RG(cpy_to_out(p_ihndl, p_lname->beg));
            EXEC_RG(sp_parser_tokenize_str(out, SP_TKN_ID, name));
            p_mihndl->in_off = p_lname->end+1;
        }

        /* property value
         */
        if (cb_bf & SP_CBEC_FLG_MOD_PROP_VAL)
        {
            if (!strlen(val)) {
                if (p_lval)
                {
                    /* delete a value */
                    EXEC_RG(cpy_to_out(p_ihndl, p_lname->end+1));
                    __CHK_FERR(fputc(';', out));
                    p_mihndl->in_off = p_ldef->end+1;
                }
            } else {
                if (p_lval)
                {
                    /* modify a value */
                    EXEC_RG(cpy_to_out(p_ihndl, p_lval->beg));
                    EXEC_RG(sp_parser_tokenize_str(out, SP_TKN_VAL, val));
                    p_mihndl->in_off = p_lval->end+1;
                } else
                {
                    /* add a value */
                    EXEC_RG(cpy_to_out(p_ihndl, p_lname->end+1));
#ifdef CUT_VAL_LEADING_SPACES
                    __CHK_FERR(fputc(' ', out)|fputc('=', out)|fputc(' ', out));
#else
                    __CHK_FERR(fputc('=', out));
#endif
                    EXEC_RG(sp_parser_tokenize_str(out, SP_TKN_VAL, val));
                    p_mihndl->in_off = p_ldef->end+1;

#ifndef NO_SEMICOL_ENDS_VAL
                    __CHK_FERR(fputc(';', out));
#else
                    /* write EOL */
                    switch (eol_typ)
                    {
                    default:
                    case EOL_LF:
                        __CHK_FERR(fputc('\n', out));
                        break;
                    case EOL_CR:
                        __CHK_FERR(fputc('\r', out));
                        break;
                    case EOL_CRLF:
                        __CHK_FERR(fputc('\r', out)|fputc('\n', out));
                        break;
                    }
                    trim_line(p_ihndl, eol_typ, 1);
#endif
                }
            }
        }
    }

    ret = (cb_bf & SP_CBEC_FLG_FINISH ? SPEC_CB_FINISH : SPEC_SUCCESS);
finish:
    return ret;
}

/* sp_iterate_modify() callback: scope */
static sp_errc_t mod_iter_cb_scope(
    const sp_parser_hndl_t *p_phndl, const sp_loc_t *p_ltype,
    const sp_loc_t *p_lname, const sp_loc_t *p_lbody, const sp_loc_t *p_ldef)
{
    sp_errc_t ret=SPEC_SUCCESS;
    long sc_out_len=0;
    int cb_bf=0, is_mod, is_del;
    iter_hndl_t *p_ihndl = (iter_hndl_t*)p_phndl->cb.arg;
    eol_t eol_typ = p_phndl->lex.eol_typ;

    char *type = p_ihndl->buf1.ptr;
    char *name = p_ihndl->buf2.ptr;

    mod_iter_hndl_t *p_mihndl = p_ihndl->p_mihndl;
    FILE *in = p_ihndl->in;
    FILE *out = p_mihndl->out;
    FILE *sc_out = p_mihndl->sc_out;

    if (sc_out && p_lbody) {
        __CHK_FSEEK(fseek(sc_out, 0, SEEK_SET));
    }

    /* follow destination path and call scope callback */
    ret = iter_cb_scope(p_phndl, p_ltype, p_lname, p_lbody, p_ldef);
    __CHK_MOD_ITER_RET(SP_CBEC_FLG_MOD_SCOPE_TYPE|SP_CBEC_FLG_MOD_SCOPE_NAME,
        SP_CBEC_FLG_MOD_SCOPE_NAME);

    if (sc_out && p_lbody && !is_del) {
        __CHK_FERR(sc_out_len=ftell(sc_out));
    }

    if (!sc_out_len && (!cb_bf || cb_bf==SP_CBEC_FLG_FINISH))
        /* nothing to modify */
        goto finish;

    if (is_del) {
        EXEC_RG(del_loc(p_ihndl, p_ldef, eol_typ));
    } else
    {
        int c, type_del=0;

        /* scope type
         */
        if (cb_bf & SP_CBEC_FLG_MOD_SCOPE_TYPE)
        {
            if (!strlen(type)) {
                if (p_ltype) {
                    /* delete a type */
                    EXEC_RG(del_loc(p_ihndl, p_ltype, eol_typ));
                    type_del++;
                }
            } else {
                if (p_ltype) {
                    /* modify a type */
                    EXEC_RG(cpy_to_out(p_ihndl, p_ltype->beg));
                    EXEC_RG(sp_parser_tokenize_str(out, SP_TKN_ID, type));
                    p_mihndl->in_off = p_ltype->end+1;
                } else {
                    /* add a type */
                    EXEC_RG(cpy_to_out(p_ihndl, p_ldef->beg));
                    EXEC_RG(sp_parser_tokenize_str(out, SP_TKN_ID, type));
                    __CHK_FERR(fputc(' ', out));
                    p_mihndl->in_off = p_lname->beg;
                }
            }
        }

        /* scope name
         */
        if (cb_bf & SP_CBEC_FLG_MOD_SCOPE_NAME) {
            EXEC_RG(cpy_to_out(p_ihndl, p_lname->beg));
            EXEC_RG(sp_parser_tokenize_str(out, SP_TKN_ID, name));
            p_mihndl->in_off = p_lname->end+1;
        }

        /* scope body
         */
        if (sc_out_len>0)
        {
            /* scope body updated by the callback */
            long off = p_lname->end+1;

            EXEC_RG(cpy_to_out(p_ihndl, off));

            __CHK_FSEEK(fseek(sc_out, off, SEEK_SET));
            for (; off<sc_out_len; off++) {
                __CHK_FERR(c=fgetc(sc_out));
                __CHK_FERR(fputc(c, out));
            }
            p_mihndl->in_off = p_lbody->end+1;
        } else
        if (type_del && !p_lbody)
        {
            /* untyped scope w/o a body must not use alternative
               version of definition to avoid ambiguity */
            EXEC_RG(cpy_to_out(p_ihndl, p_ldef->end));

            __CHK_FERR(c=fgetc(in));
            if (c==';') {
                __CHK_FERR(fputc('{', out)|fputc('}', out));
            } else {
                __CHK_FERR(fputc(c, out));
            }
            p_mihndl->in_off = p_ldef->end+1;
        }
    }

    ret = (cb_bf & SP_CBEC_FLG_FINISH ? SPEC_CB_FINISH : SPEC_SUCCESS);
finish:
    return ret;
}

#undef __CHK_FERR
#undef __CHK_FSEEK
#undef __CHK_MOD_ITER_RET

/* exported; see header for details */
sp_errc_t sp_iterate_modify(
    FILE *in, FILE *out, const sp_loc_t *p_parsc, const char *path,
    const char *defsc, sp_cb_prop_t cb_prop, sp_cb_scope_t cb_scope,
    void *arg, char *p_buf1, size_t b1len, char *p_buf2, size_t b2len)
{
    sp_errc_t ret=SPEC_SUCCESS;
    mod_iter_hndl_t mihndl;
    iter_hndl_t ihndl;
    sp_parser_hndl_t phndl;
    FILE *sc_out = NULL;

    if (!in || !out || in==out || (!cb_prop && !cb_scope)) {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    if (cb_scope) {
        /* scopes modifications goes to temporary file before merging
           them to the final output */
        sc_out = tmpfile();
        if (!sc_out) {
            ret = SPEC_TMP_CREAT_ERR;
            goto finish;
        }
    }

    mihndl.out = out;
    mihndl.sc_out = sc_out;
    mihndl.in_off = 0;

    init_iter_hndl(&ihndl, in, path, defsc, cb_prop, cb_scope,
        arg, p_buf1, b1len, p_buf2, b2len, &mihndl);

    EXEC_RG(sp_parser_hndl_init(
        &phndl, in, p_parsc, mod_iter_cb_prop, mod_iter_cb_scope, &ihndl));
    EXEC_RG(sp_parse(&phndl));

    /* copy untouched last part of the input */
    EXEC_RG(cpy_to_out(&ihndl, (p_parsc ? p_parsc->end+1 : EOF)));

finish:
    if (sc_out) fclose(sc_out);
    return ret;
}
