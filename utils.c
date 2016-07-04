/*
   Copyright (c) 2016 Piotr Stolarz
   Scoped properties configuration library

   Distributed under the 2-clause BSD License (the License)
   see accompanying file LICENSE for details.

   This software is distributed WITHOUT ANY WARRANTY; without even the
   implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the License for more information.
 */

#include "sprops/utils.h"

#define CHK_FSEEK(c) if ((c)!=0) { ret=SPEC_ACCS_ERR; goto finish; }

/* exported; see header for details */
sp_errc_t sp_util_cpy_to_out(FILE *in, FILE *out, long beg, long end, long *p_n)
{
    int ret=SPEC_SUCCESS;
    long off=beg;

    if (!in || !out) {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    if (p_n) *p_n=0;

    if (off<end || end==EOF)
    {
        CHK_FSEEK(fseek(in, off, SEEK_SET));

        for (; off<end || end==EOF; off++) {
            int c = fgetc(in);
            if (c==EOF && end==EOF) break;
            if (c==EOF || fputc(c, out)==EOF) {
                ret=SPEC_ACCS_ERR;
                goto finish;
            }
        }
    }

    if (p_n) *p_n=off-beg;
finish:
    return ret;
}

/* exported; see header for details */
sp_errc_t sp_util_detect_eol(FILE *in, sp_eol_t *p_eol_typ)
{
    sp_errc_t ret=SPEC_SUCCESS;
    int c;

    if (!in || !p_eol_typ) {
        ret=SPEC_INV_ARG;
        goto finish;
    }

    *p_eol_typ = EOL_NDETECT;

    CHK_FSEEK(fseek(in, 0, SEEK_SET));
    while ((c=fgetc(in))!=EOF)
    {
        if (c=='\n') {
            *p_eol_typ=EOL_LF;
            break;
        } else
        if (c=='\r') {
            *p_eol_typ=EOL_CR;
            if (fgetc(in)=='\n') *p_eol_typ=EOL_CRLF;
            break;
        }
    }
finish:
    return ret;
}
