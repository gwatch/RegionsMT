#include "Common.h"
#include "Compare.h"
#include "Debug.h"
#include "Density-Report.h"
#include "Sort.h"
#include "Memory.h"
#include "TaskMacros.h"
#include "x86_64/Compare.h"
#include "x86_64/Tools.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

double **selectByThreshold(double *arr, size_t *pcount, double thresh)
{
    size_t cap = 0, cnt = 0;
    double **res = NULL;

    for (size_t i = 0; i < *pcount; i++) if (!isnan(arr[i]) && arr[i] > thresh)
    {
        if (!dynamicArrayTest((void **) &res, &cap, sizeof *res, cnt + 1)) goto ERR();        
        res[cnt++] = &arr[i];
    }

    if (!dynamicArrayFinalize((void **) &res, &cap, sizeof *res, cnt)) goto ERR();
    *pcount = cap;
    return res;

ERR():
    free(res);
    return NULL;
}

void dReportContextDispose(dReportContext *context)
{
    if (!context) return;
    
    free(context->path);
    free(context);
}

static bool densityComp(const double **a, const double **b, void *context)
{
    int8_t res = float64CompDscStable(*a, *b, context);
    if (res > 0 || (!res && *a > *b)) return 1;
    return 0;
}

static bool dReportThreadProc(dReportOut *args, dReportContext *context)
{    
    const char *strings[] =
    {
        __FUNCTION__,
        "ERROR (%s): %s: cannot open specified file \"%s\". %s!\n",
        "ERROR (%s): %s: %s!\n",
        "Unable to create report",
        "Not enough heap memory"
    };

    enum
    {
        STR_FN = 0,
        STR_FR_EI,
        STR_FR_EG,
        STR_M_UNA,
        STR_M_MEM
    };
    
    char tempbuff[TEMP_BUFF] = { '\0' };

    bool succ = 0;
    FILE *f = NULL;
    
    loadDataRes *ldres = LOADDATA_META(args)->res;
    densityRes *densityres = DENSITY_META(args)->res;
    
    if (!bitTest(context->bits, DREPORTCONTEXT_BIT_POS_THRESHOLD)) context->threshold = -DBL_MAX;
    
    size_t cnt = ldres->pvcnt;
    double **index = selectByThreshold(densityres->lpv, &cnt, context->threshold);

    if (!index && cnt) goto ERR(Index);
    quickSort(index, cnt, sizeof *index, (compareCallback) densityComp, NULL);

    f = fopen(context->path, "w");
    if (!f) goto ERR(File);

    uint8_t semi = (uint8_t) bitTest(context->bits, DREPORTCONTEXT_BIT_POS_SEMICOLON);

    const char *head = ((const char *[])
    {
        "\"Rank of Density's P-value\","
        "\"Test index\","
        "\"Chromosome index\","
        "\"SNP index (left)\","
        "\"SNP index (center)\","
        "\"SNP index (right)\","
        "\"Position (left)\","
        "\"Position (center)\","
        "\"Position (right)\","
        "\"SNP count\","
        "\" -log(Density's P-value)\","
        "\"Density's P-value\","
        "\"Density\"\n",

        "Rank of Density's P-value;"
        "Test index;"
        "Chromosome index;"
        "SNP index (left);"
        "SNP index (center);"
        "SNP index (right);"
        "Position (left);"
        "Position (center);"
        "Position (right);"
        "SNP count;"
        " -log(Density's P-value);"
        "Density's P-value;"
        "Density\n",
    })[semi];

    const char *form = ((const char *[]) 
    { 
        "%zu," "%zu," "%zu," "%zu," "%zu," "%zu," "%zu," "%zu," "%zu," "%zu," "%.16e," "%.16e," "%.16e\n",
        "%zu;" "%zu;" "%zu;" "%zu;" "%zu;" "%zu;" "%zu;" "%zu;" "%zu;" "%zu;" "%.16e;" "%.16e;" "%.16e\n",
    })[semi];

    struct tableline
    {
        size_t rank, test, chr, li, ci, ri, lp, cp, rp, cnt;
        double lpv, pv, dns;
    } tln, tl = { 0 };
    
    if (bitTest(context->bits, DREPORTCONTEXT_BIT_POS_LIMIT)) cnt = cnt > context->limit ? context->limit : cnt;
    if (bitTest(context->bits, DREPORTCONTEXT_BIT_POS_HEADER)) fprintf(f, "%s", head);
        
    for (size_t i = 0, rank = 0; i < cnt; i++)
    {
        size_t ind = index[i] - densityres->lpv;
        size_t test = ind / ldres->snpcnt, row = ind % ldres->snpcnt;
        size_t chr = findBound(row, ldres->chroff, ldres->chrcnt);

        tln = (struct tableline)
        {
            rank + 1, test + 1, chr + 1,
            densityres->li[ind] + 1, row + 1, densityres->ri[ind] + 1,
            ldres->pos[densityres->li[ind]], ldres->pos[row], ldres->pos[densityres->ri[ind]],
            densityres->lc[ind] + densityres->rc[ind] + 1,
            densityres->lpv[ind], pow(10, -densityres->lpv[ind]), densityres->dns[ind]
        };

        if (tl.test != tln.test || tl.chr != tln.chr || tl.li != tln.li || tl.ri != tln.ri || tl.lpv != tln.lpv)
        {
            fprintf(f, form, tln.rank, tln.test, tln.chr, tln.li, tln.ci, tln.ri, tln.lp, tln.cp, tln.rp, tln.cnt, tln.lpv, tln.pv, tln.dns);
            tl = tln;
            rank++;
        }
    }

    for (;;)
    {
        succ = 1;
        break;
        
    ERR(Index) :
        logMsg(FRAMEWORK_META(args)->log, strings[STR_FR_EG], strings[STR_FN], strings[STR_M_UNA], strings[STR_M_MEM]);
        break;

    ERR(File):
        strerror_s(tempbuff, sizeof tempbuff, errno);
        logMsg(FRAMEWORK_META(args)->log, strings[STR_FR_EI], strings[STR_FN], strings[STR_M_UNA], context->path, tempbuff);
        break;
    }

    free(index);
    if (f) fclose(f);

    return succ;
}

bool dReportPrologue(dReportIn *in, dReportOut **pout, dReportContext *context)
{
    dReportOut *out = *pout = malloc(sizeof *out);
    if (!out) goto ERR();

    *out = (dReportOut) { .meta = DREPORT_META_INIT(in, out, context) };
    if (!outInfoSetNext(FRAMEWORK_META(in)->out, out)) goto ERR();

    task *tsk = tasksInfoNextTask(FRAMEWORK_META(in)->tasks);
    if (!tsk) goto ERR();

    if (!pnumTest(FRAMEWORK_META(out)->pnum, DREPORTSUPP_STAT_BIT_CNT)) goto ERR();    
    *tsk = TASK_BIT_2_INIT(dReportThreadProc, bitTestMem, out, context, DENSITY_META(in)->stat, out->supp.stat, pnumGet(FRAMEWORK_META(out)->pnum, DENSITYSUPP_STAT_BIT_POS_TASK_SUCC), pnumGet(FRAMEWORK_META(out)->pnum, DREPORTSUPP_STAT_BIT_POS_TASK_COMP));
    
    sizeIncInterlocked(DENSITY_META(in)->hold, NULL);
    return  1;

ERR() :
    logError(FRAMEWORK_META(in)->log, __FUNCTION__, errno);
    return 0;
}

static bool dReportThreadClose(dReportOut *args, void *context)
{
    (void) args;
    (void) context;

    return 1;
}

bool dReportEpilogue(dReportIn *in, dReportOut *out, void *context)
{
    if (!out) return 0;
    (void) context;

    task *tsk = tasksInfoNextTask(FRAMEWORK_META(in)->tasks);
    if (!tsk) goto ERR();
    
    *tsk = (task)
    {
        .callback = (taskCallback) dReportThreadClose,
        .cond = (conditionCallback) bitTestMem,
        .asucc = (aggregatorCallback) sizeDecInterlocked,
        .afail = (aggregatorCallback) sizeDecInterlocked,
        .arg = out,
        .condmem = &out->supp,
        .asuccmem = DENSITY_META(in)->hold,
        .afailmem = DENSITY_META(in)->hold,
        .condarg = pnumGet(FRAMEWORK_META(out)->pnum, DREPORTSUPP_STAT_BIT_POS_TASK_COMP)
    };

    return 1;

ERR():
    logError(FRAMEWORK_META(in)->log, __FUNCTION__, errno);

    return 0;
}