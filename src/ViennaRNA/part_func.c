/*
                  partiton function for RNA secondary structures

                  Ivo L Hofacker + Ronny Lorenz
                  Vienna RNA package
*/
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>    /* #defines FLT_MAX ... */
#include <limits.h>

#include "ViennaRNA/utils.h"
#include "ViennaRNA/energy_par.h"
#include "ViennaRNA/fold_vars.h"
#include "ViennaRNA/params.h"
#include "ViennaRNA/loop_energies.h"
#include "ViennaRNA/gquad.h"
#include "ViennaRNA/constraints.h"
#include "ViennaRNA/part_func.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/*
#################################
# GLOBAL VARIABLES              #
#################################
*/
PUBLIC  int         st_back = 0;

/*
#################################
# PRIVATE VARIABLES             #
#################################
*/

/* some backward compatibility stuff */
PRIVATE vrna_fold_compound  *backward_compat_compound = NULL;

#ifdef _OPENMP

#pragma omp threadprivate(backward_compat_compound)

#endif

/*
#################################
# PRIVATE FUNCTION DECLARATIONS #
#################################
*/
PRIVATE void  pf_circ(vrna_fold_compound *vc);
PRIVATE void  pf_linear(vrna_fold_compound *vc);
PRIVATE void  pf_create_bppm(vrna_fold_compound *vc, char *structure);
PRIVATE void  backtrack(int i, int j, char *pstruc, vrna_fold_compound *vc);
PRIVATE void  backtrack_qm(int i, int j, char *pstruc, vrna_fold_compound *vc);
PRIVATE void  backtrack_qm1(int i,int j, char *pstruc, vrna_fold_compound *vc);
PRIVATE void  backtrack_qm2(int u, int n, char *pstruc, vrna_fold_compound *vc);

PRIVATE float
wrap_pf_fold( const char *sequence,
              char *structure,
              pf_paramT *parameters,
              int calculate_bppm,
              int is_constrained,
              int is_circular,
              soft_constraintT *sc_p);

PRIVATE void
wrap_update_pf_params(int length,
                      pf_paramT *parameters);

/*
#################################
# BEGIN OF FUNCTION DEFINITIONS #
#################################
*/


PRIVATE float
wrap_pf_fold( const char *sequence,
              char *structure,
              pf_paramT *parameters,
              int calculate_bppm,
              int is_constrained,
              int is_circular,
              soft_constraintT *sc_p){

  vrna_fold_compound  *vc;
  pf_paramT           *exp_params;

  vc                  = NULL;

  /* we need pf_paramT datastructure to correctly init default hard constraints */
  if(parameters)
    exp_params = get_boltzmann_factor_copy(parameters);
  else{
    model_detailsT md;
    set_model_details(&md); /* get global default parameters */
    exp_params = vrna_get_boltzmann_factors(md);
  }
  exp_params->model_details.circ          = is_circular;
  exp_params->model_details.do_backtrack  = calculate_bppm;

  vc = vrna_get_fold_compound(sequence, &(exp_params->model_details), VRNA_OPTION_PF);

  if(parameters){ /* replace exp_params if necessary */
    free(vc->exp_params);
    vc->exp_params = exp_params;
  } else {
    free(exp_params);
  }

  if(is_constrained && structure){
    unsigned int constraint_options = 0;
    constraint_options |= VRNA_CONSTRAINT_DB
                          | VRNA_CONSTRAINT_PIPE
                          | VRNA_CONSTRAINT_DOT
                          | VRNA_CONSTRAINT_X
                          | VRNA_CONSTRAINT_ANG_BRACK
                          | VRNA_CONSTRAINT_RND_BRACK;

    vrna_hc_add(vc, (const char *)structure, constraint_options);
  }

  if(backward_compat_compound)
    destroy_fold_compound(backward_compat_compound);

  backward_compat_compound = vc;
  iindx = backward_compat_compound->iindx;

  return vrna_pf_fold(vc, structure);
}

PUBLIC float
vrna_pf_fold( vrna_fold_compound *vc,
              char *structure){

  FLT_OR_DBL      Q;
  double          free_energy;
  int             n;
  model_detailsT  *md;
  pf_paramT       *params;
  pf_matricesT    *matrices;
  char            *sequemce;

  params    = vc->exp_params;
  n         = vc->length;
  md        = &(params->model_details);
  matrices  = vc->exp_matrices;


#ifdef _OPENMP
/* Explicitly turn off dynamic threads */
  omp_set_dynamic(0);
#endif

#ifdef SUN4
  nonstandard_arithmetic();
#else
#ifdef HP9
  fpsetfastmode(1);
#endif
#endif

  /* do the linear pf fold and fill all matrices  */
  pf_linear(vc);

  if(md->circ)
    pf_circ(vc); /* do post processing step for circular RNAs */

  /* calculate base pairing probability matrix (bppm)  */
  if(md->do_backtrack){
    pf_create_bppm(vc, structure);
    /*
    *  Backward compatibility:
    *  This block may be removed if deprecated functions
    *  relying on the global variable "pr" vanish from within the package!
    */
    pr = matrices->probs;
    /*
     {
      if(pr) free(pr);
      pr = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL) * ((n+1)*(n+2)/2));
      memcpy(pr, probs, sizeof(FLT_OR_DBL) * ((n+1)*(n+2)/2));
    }
    */
  }

  if (md->backtrack_type=='C')
    Q = matrices->qb[vc->iindx[1]-n];
  else if (md->backtrack_type=='M')
    Q = matrices->qm[vc->iindx[1]-n];
  else Q = (md->circ) ? matrices->qo : matrices->q[vc->iindx[1]-n];

  /* ensemble free energy in Kcal/mol              */
  if (Q<=FLT_MIN)
    fprintf(stderr, "pf_scale too large\n");
  free_energy = (-log(Q)-n*log(params->pf_scale))*params->kT/1000.0;
  /* in case we abort because of floating point errors */
  if (n>1600)
    fprintf(stderr, "free energy = %8.2f\n", free_energy);

#ifdef SUN4
  standard_arithmetic();
#else
#ifdef HP9
  fpsetfastmode(0);
#endif
#endif

  return free_energy;
}

PRIVATE void
pf_linear(vrna_fold_compound *vc){

  int n, i,j,k,l, ij, kl, u,u1,u2,d,ii, type, type_2, tt;

  int         noGUclosure;
  FLT_OR_DBL  expMLstem = 0.;

  FLT_OR_DBL  temp, Qmax=0;
  FLT_OR_DBL  qbt1, *tmp, q_temp;
  FLT_OR_DBL  *qqm = NULL, *qqm1 = NULL, *qq = NULL, *qq1 = NULL;
  FLT_OR_DBL  *q, *qb, *qm, *qm1, *G;
  FLT_OR_DBL  *scale;
  FLT_OR_DBL  *expMLbase;
  short             *S, *S1;
  int         *my_iindx, *jindx;
  char        *ptype, *sequence;

  pf_paramT   *pf_params;
  pf_matricesT *matrices  = vc->exp_matrices;

  pf_params             = vc->exp_params;
  model_detailsT    *md = &(pf_params->model_details);
  hard_constraintT  *hc = vc->hc;
  soft_constraintT  *sc = vc->sc;

  FLT_OR_DBL  expMLclosing = pf_params->expMLclosing;
  double      max_real;
  int         *rtype;
  int         with_gquad  = md->gquad;
  int         circular    = md->circ;

  sequence  = vc->sequence;
  n         = vc->length;
  my_iindx  = vc->iindx;
  jindx     = vc->jindx;
  ptype     = vc->ptype;

  q         = matrices->q;
  qb        = matrices->qb;
  qm        = matrices->qm;
  qm1       = matrices->qm1;
  G         = matrices->G;
  scale     = matrices->scale;
  expMLbase = matrices->expMLbase;

  S         = vc->sequence_encoding2;
  S1        = vc->sequence_encoding;

  int         hc_decompose;
  char        *hard_constraints = hc->matrix;
  int         *hc_up_ext        = hc->up_ext;
  int         *hc_up_hp         = hc->up_hp;
  int         *hc_up_int        = hc->up_int;
  int         *hc_up_ml         = hc->up_ml;

  max_real = (sizeof(FLT_OR_DBL) == sizeof(float)) ? FLT_MAX : DBL_MAX;

  noGUclosure = md->noGUclosure;
  rtype       = &(md->rtype[0]);

  /* allocate memory for helper arrays */
  qq        = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));
  qq1       = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));
  qqm       = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));
  qqm1      = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));


  /*array initialization ; qb,qm,q
    qb,qm,q (i,j) are stored as ((n+1-i)*(n-i) div 2 + n+1-j */

  if(with_gquad)
    expMLstem = exp_E_MLstem(0, -1, -1, pf_params);


  for (d=0; d<=TURN; d++)
    for (i=1; i<=n-d; i++) {
      j=i+d;
      ij = my_iindx[i]-j;
      if(hc_up_ext[i] > d){
        q[ij]=1.0*scale[d+1];

        if(sc){
          if(sc->boltzmann_factors)
            q[ij] *= sc->boltzmann_factors[i][d+1];
        }
      }
      else
        q[ij]=0.;

      qb[ij]=qm[ij]=0.0;
    }
 
  for (i=1; i<=n; i++)
    qq[i]=qq1[i]=qqm[i]=qqm1[i]=0;

  for (j=TURN+2;j<=n; j++) {
    for (i=j-TURN-1; i>=1; i--) {
      /* construction of partition function of segment i,j */
      /* firstly that given i binds j : qb(i,j) */
      u             = j-i-1;
      ij            = my_iindx[i]-j;
      type          = ptype[jindx[j] + i];
      hc_decompose  = hard_constraints[jindx[j] + i];
      qbt1  = 0.;
      q_temp = 0.;

      if(hc_decompose){
        /*hairpin contribution*/
        if(hc_decompose & IN_HP_LOOP){
          if(hc_up_hp[i+1] >= u){
            qbt1 =  exp_E_Hairpin(u, type, S1[i+1], S1[j-1], sequence+i-1, pf_params)
                    * scale[u+2];
            
            if(sc){
              if(sc->boltzmann_factors)
                qbt1 *= sc->boltzmann_factors[i+1][u];

              if(sc->exp_en_basepair)
                qbt1 *= sc->exp_en_basepair[ij];

              if(sc->exp_f)
                qbt1 *= sc->exp_f(i, j, n, n, VRNA_DECOMP_PAIR_HP, sc->data);
            }
          }
        }
        /* interior loops with interior pair k,l */
        if(hc_decompose & IN_INT_LOOP){
          int maxk = i+MAXLOOP+1;
          maxk = MIN2(maxk, j - TURN - 2);
          maxk = MIN2(maxk, i + 1 + hc_up_int[i+1]);
          for (k=i+1; k<=maxk; k++) {
            u1 = k-i-1;

            int minl = MAX2(k+TURN+1,j-1-MAXLOOP+u1);
            kl = my_iindx[k] - j + 1;
            for (u2 = 0, l=j-1; l>=minl; l--, kl++, u2++){
              if(hc_up_int[l+1] < u2) break;
              if(hard_constraints[jindx[l] + k] & IN_INT_LOOP_ENC){
                type_2 = rtype[ptype[jindx[l] + k]];
                q_temp = qb[kl]
                        * scale[u1+u2+2]
                        * exp_E_IntLoop(u1, u2, type, type_2, S1[i+1], S1[j-1], S1[k-1], S1[l+1], pf_params);

                if(sc){
                  if(sc->boltzmann_factors)
                    q_temp *= sc->boltzmann_factors[i+1][u1]
                              * sc->boltzmann_factors[l+1][u2];

                  if(sc->exp_en_basepair)
                    q_temp *= sc->exp_en_basepair[ij];

                  if(sc->exp_f)
                    q_temp *= sc->exp_f(i, j, k, l, VRNA_DECOMP_PAIR_IL, sc->data);

                  if(sc->exp_en_stack)
                    if((i+1 == k) && (j-1 == l)){
                      q_temp *=   sc->exp_en_stack[i]
                                * sc->exp_en_stack[k]
                                * sc->exp_en_stack[l]
                                * sc->exp_en_stack[j];
                    }
                }

                qbt1 += q_temp;
              }
            }
          }
        }

        /*multiple stem loop contribution*/
        if(hc_decompose & IN_MB_LOOP){
          ii = my_iindx[i+1]; /* ii-k=[i+1,k-1] */
          temp = 0.0;
          kl = my_iindx[i+1]-(i+1);
          for (k=i+2; k<=j-1; k++,kl--){
            q_temp = qm[kl] * qqm1[k];

            if(sc){
              if(sc->exp_en_basepair)
                q_temp *= sc->exp_en_basepair[ij];

              if(sc->exp_f)
                q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_PAIR_ML, sc->data);
            }

            temp += q_temp;
          }
          tt = rtype[type];
          qbt1 += temp
                  * expMLclosing
                  * exp_E_MLstem(tt, S1[j-1], S1[i+1], pf_params)
                  * scale[2];

          if(with_gquad){
            qbt1 += exp_E_GQuad_IntLoop(i, j, type, S1, G, my_iindx, pf_params)
                    * scale[2];
          }

        }

      }
      qb[ij] = qbt1;

      /* construction of qqm matrix containing final stem
         contributions to multiple loop partition function
         from segment i,j */
      qqm[i] = 0.;

      if(hc_up_ml[j]){
        q_temp  =  qqm1[i] * expMLbase[1];

        if(sc){
          if(sc->boltzmann_factors)
            q_temp *= sc->boltzmann_factors[j][1];

          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, j-1, n, VRNA_DECOMP_ML_UP_3, sc->data);
        }

        qqm[i] = q_temp;

      }

      if(hc_decompose & IN_MB_LOOP_ENC){
        qbt1 = qb[ij] * exp_E_MLstem(type, ((i>1) || circular) ? S1[i-1] : -1, ((j<n) || circular) ? S1[j+1] : -1, pf_params);
        qqm[i] += qbt1;
      }

      if(with_gquad){
        /*include gquads into qqm*/
        qqm[i] += G[my_iindx[i]-j] * expMLstem;
      }

      if (qm1) qm1[jindx[j]+i] = qqm[i]; /* for stochastic backtracking and circfold */

      /*construction of qm matrix containing multiple loop
        partition function contributions from segment i,j */
      temp = 0.0;
      kl = my_iindx[i] - j + 1; /* ii-k=[i,k-1] */
      for (k=j; k>i; k--, kl++){
        q_temp = qm[kl] * qqm[k];

        if(sc){
          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_ML_ML_ML, sc->data);
        }

        temp += q_temp;
      }

      int maxk = MIN2(i+hc_up_ml[i], j);
      ii = 1; /* length of unpaired stretch */
      for (k=i+1; k<=maxk; k++, ii++){
        q_temp = expMLbase[ii] * qqm[k];

        if(sc){
          if(sc->boltzmann_factors)
            q_temp *= sc->boltzmann_factors[i][ii];

          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_ML_UP_5, sc->data);
        }

        temp += q_temp;
      }

      qm[ij] = (temp + qqm[i]);

      /*auxiliary matrix qq for cubic order q calculation below */
      qbt1 = 0.;

      if(hc_decompose & IN_EXT_LOOP){
        qbt1 = qb[ij] * exp_E_ExtLoop(type, ((i>1) || circular) ? S1[i-1] : -1, ((j<n) || circular) ? S1[j+1] : -1, pf_params);
      }

      if(hc_up_ext[j]){
        q_temp = qq1[i] * scale[1];

        if(sc){
          if(sc->boltzmann_factors)
            q_temp *= sc->boltzmann_factors[j][1];

          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, j-1, n, VRNA_DECOMP_EXT_UP_3, sc->data);
        }

        qbt1 += q_temp;

        if(with_gquad){
          qbt1 += G[ij];
        }

      }
      qq[i] = qbt1;

      /*construction of partition function for segment i,j */
      temp = qq[i];

      /* the whole stretch [i,j] is unpaired */
      if(hc_up_ext[i] >= (j-i+1)){
        q_temp = 1.0 * scale[j-i+1];

        if(sc){
          if(sc->boltzmann_factors)
            q_temp *= sc->boltzmann_factors[i][j-i+1];

          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, n, n, VRNA_DECOMP_EXT_UP, sc->data);
        }

        temp += q_temp;
      }

      kl = my_iindx[i] - i;
      for (k=i; k<j; k++, kl--){
        q_temp = q[kl] * qq[k+1];

        if(sc){
          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_EXT_EXT, sc->data);
        }

        temp += q_temp;
      }
      q[ij] = temp;
      if (temp>Qmax) {
        Qmax = temp;
        if (Qmax>max_real/10.)
          fprintf(stderr, "Q close to overflow: %d %d %g\n", i,j,temp);
      }
      if (temp>=max_real) {
        PRIVATE char msg[128];
        sprintf(msg, "overflow in pf_fold while calculating q[%d,%d]\n"
                     "use larger pf_scale", i,j);
        nrerror(msg);
      }
    }
    tmp = qq1;  qq1 =qq;  qq =tmp;
    tmp = qqm1; qqm1=qqm; qqm=tmp;

  }

  /* clean up */
  free(qq);
  free(qq1);
  free(qqm);
  free(qqm1);

}

/* calculate partition function for circular case */
/* NOTE: this is the postprocessing step ONLY     */
/* You have to call pf_linear first to calculate  */
/* complete circular case!!!                      */
PRIVATE void
pf_circ(vrna_fold_compound *vc){

  int u, p, q, k, l;
  int noGUclosure, with_gquad;
  int n;
  char  *sequence;
  char  *structure;
  int   *my_iindx;
  int   *jindx;
  char  *ptype;
  FLT_OR_DBL  *scale;
  short             *S, *S1;

  pf_paramT     *pf_params = vc->exp_params;
  FLT_OR_DBL    *qb, *qm, *qm1, *qm2, qo, qho, qio, qmo;
  pf_matricesT  *matrices;

  sequence  = vc->sequence;
  n         = vc->length;

  matrices = vc->exp_matrices;
  qb  = matrices->qb;
  qm  = matrices->qm;
  qm1 = matrices->qm1;
  qm2 = matrices->qm2;

  my_iindx  = vc->iindx;
  jindx     = vc->jindx;

  ptype     = vc->ptype;
  scale     = matrices->scale;
  S                 = vc->sequence_encoding2;
  S1                = vc->sequence_encoding;


  FLT_OR_DBL  qot;
  FLT_OR_DBL  expMLclosing  = pf_params->expMLclosing;
  int         *rtype;

  noGUclosure = pf_params->model_details.noGUclosure;
  with_gquad  = pf_params->model_details.gquad;
  rtype       = &(pf_params->model_details.rtype[0]);

  qo = qho = qio = qmo = 0.;

  /* construct qm2 matrix from qm1 entries  */
  for(k=1; k<n-TURN-1; k++){
    qot = 0.;
    for (u=k+TURN+1; u<n-TURN-1; u++)
      qot += qm1[jindx[u]+k]*qm1[jindx[n]+(u+1)];
    qm2[k] = qot;
   }

  for(p = 1; p < n; p++){
    for(q = p + TURN + 1; q <= n; q++){
      int type;
      /* 1. get exterior hairpin contribution  */
      u = n-q + p-1;
      if (u<TURN) continue;
      type = ptype[jindx[q] + p];
      if (!type) continue;
       /* cause we want to calc the exterior loops, we need the reversed pair type from now on  */
      type=rtype[type];

      char loopseq[10];
      if (u<7){
        strcpy(loopseq , sequence+q-1);
        strncat(loopseq, sequence, p);
      }
      qho += (((type==3)||(type==4))&&noGUclosure) ? 0. : qb[my_iindx[p]-q] * exp_E_Hairpin(u, type, S1[q+1], S1[p-1],  loopseq, pf_params) * scale[u];

      /* 2. exterior interior loops, i "define" the (k,l) pair as "outer pair"  */
      /* so "outer type" is rtype[type[k,l]] and inner type is type[p,q]        */
      qot = 0.;
      for(k=q+1; k < n; k++){
        int ln1, lstart;
        ln1 = k - q - 1;
        if(ln1+p-1>MAXLOOP) break;
        lstart = ln1+p-1+n-MAXLOOP;
        if(lstart<k+TURN+1) lstart = k + TURN + 1;
        for(l=lstart;l <= n; l++){
          int ln2, type2;
          ln2 = (p - 1) + (n - l);

          if((ln1+ln2) > MAXLOOP) continue;

          type2 = ptype[jindx[l] + k];
          if(!type2) continue;
          qio += qb[my_iindx[p]-q] * qb[my_iindx[k]-l] * exp_E_IntLoop(ln2, ln1, rtype[type2], type, S1[l+1], S1[k-1], S1[p-1], S1[q+1], pf_params) * scale[ln1+ln2];
        }
      } /* end of kl double loop */
    }
  } /* end of pq double loop */

  /* 3. Multiloops  */
  for(k=TURN+2; k<n-2*TURN-3; k++)
    qmo += qm[my_iindx[1]-k] * qm2[k+1] * expMLclosing;

  /* add an additional pf of 1.0 to take the open chain into account too           */
  qo = qho + qio + qmo + 1.0*scale[n];

  matrices->qo    = qo;
  matrices->qho   = qho;
  matrices->qio   = qio;
  matrices->qmo   = qmo;

}

/* calculate base pairing probs */
PUBLIC void
pf_create_bppm( vrna_fold_compound *vc,
                char *structure){

  int n, i,j,k,l, ij, kl, ii,ll, u1, u2, type, type_2, tt, ov=0;
  FLT_OR_DBL  temp, Qmax=0, prm_MLb;
  FLT_OR_DBL  prmt,prmt1;
  FLT_OR_DBL  *tmp;
  FLT_OR_DBL  tmp2;
  FLT_OR_DBL  expMLclosing;
  FLT_OR_DBL  *q, *qb, *qm, *qm1, *G, *probs, *scale, *expMLbase;
  FLT_OR_DBL  qo;

  char              *ptype;

  double            max_real;
  int               *rtype, with_gquad;
  char              *sequence;
  short             *S, *S1;
  hard_constraintT  *hc;
  soft_constraintT  *sc;
  int               *my_iindx, *jindx;
  int               circular;
  pf_paramT         *pf_params;
  pf_matricesT      *matrices;
  model_detailsT    *md;


  pf_params         = vc->exp_params;
  md                = &(pf_params->model_details);
  S                 = vc->sequence_encoding2;
  S1                = vc->sequence_encoding;
  my_iindx          = vc->iindx;
  jindx             = vc->jindx;
  ptype             = vc->ptype;

  circular          = md->circ;
  with_gquad        = md->gquad;

  hc                = vc->hc;
  sc                = vc->sc;

  matrices          = vc->exp_matrices;

  q                 = matrices->q;
  qb                = matrices->qb;
  qm                = matrices->qm;
  qm1               = matrices->qm1;
  G                 = matrices->G;
  probs             = matrices->probs;
  scale             = matrices->scale;
  expMLbase         = matrices->expMLbase;
  qo                = matrices->qo;

  FLT_OR_DBL  expMLstem         = (with_gquad) ? exp_E_MLstem(0, -1, -1, pf_params) : 0;
  int         hc_decompose;
  char        *hard_constraints = hc->matrix;
  int         *hc_up_ext        = hc->up_ext;
  int         *hc_up_hp         = hc->up_hp;
  int         *hc_up_int        = hc->up_int;
  int         *hc_up_ml         = hc->up_ml;

  max_real      = (sizeof(FLT_OR_DBL) == sizeof(float)) ? FLT_MAX : DBL_MAX;
  sequence      = vc->sequence;

  if((S != NULL) && (S1 != NULL)){

    expMLclosing  = pf_params->expMLclosing;
    with_gquad    = pf_params->model_details.gquad;
    rtype         = &(pf_params->model_details.rtype[0]);
    n             = S[0];

    FLT_OR_DBL *q1k    = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+1));
    FLT_OR_DBL *qln    = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));
    FLT_OR_DBL *prm_l  = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));
    FLT_OR_DBL *prm_l1 = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));
    FLT_OR_DBL *prml   = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));

    Qmax=0;

    for (k=1; k<=n; k++) {
      q1k[k] = q[my_iindx[1] - k];
      qln[k] = q[my_iindx[k] - n];
    }
    q1k[0] = 1.0;
    qln[n+1] = 1.0;

    /* 1. exterior pair i,j and initialization of pr array */
    if(circular){
      for (i=1; i<=n; i++) {
        for (j=i; j<=MIN2(i+TURN,n); j++)
          probs[my_iindx[i]-j] = 0;
        for (j=i+TURN+1; j<=n; j++) {
          ij = my_iindx[i]-j;
          type = ptype[jindx[j] + i];
          if (type&&(qb[ij]>0.)) {
            probs[ij] = 1./qo;
            int rt = rtype[type];

            /* 1.1. Exterior Hairpin Contribution */
            int u = i + n - j -1;
            /* get the loop sequence */
            char loopseq[10];
            if (u<7){
              strcpy(loopseq , sequence+j-1);
              strncat(loopseq, sequence, i);
            }
            tmp2 = exp_E_Hairpin(u, rt, S1[j+1], S1[i-1], loopseq, pf_params) * scale[u];

            /* 1.2. Exterior Interior Loop Contribution                     */
            /* 1.2.1. i,j  delimtis the "left" part of the interior loop    */
            /* (j,i) is "outer pair"                                        */
            for(k=1; k < i-TURN-1; k++){
              int ln1, lstart;
              ln1 = k + n - j - 1;
              if(ln1>MAXLOOP) break;
              lstart = ln1+i-1-MAXLOOP;
              if(lstart<k+TURN+1) lstart = k + TURN + 1;
              for(l=lstart; l < i; l++){
                int ln2, type_2;
                type_2 = ptype[jindx[l] + k];
                if (type_2==0) continue;
                ln2 = i - l - 1;
                if(ln1+ln2>MAXLOOP) continue;
                tmp2 += qb[my_iindx[k] - l]
                        * exp_E_IntLoop(ln1,
                                        ln2,
                                        rt,
                                        rtype[type_2],
                                        S1[j+1],
                                        S1[i-1],
                                        S1[k-1],
                                        S1[l+1],
                                        pf_params)
                        * scale[ln1 + ln2];
              }
            }
            /* 1.2.2. i,j  delimtis the "right" part of the interior loop  */
            for(k=j+1; k < n-TURN; k++){
              int ln1, lstart;
              ln1 = k - j - 1;
              if((ln1 + i - 1)>MAXLOOP) break;
              lstart = ln1+i-1+n-MAXLOOP;
              if(lstart<k+TURN+1) lstart = k + TURN + 1;
              for(l=lstart; l <= n; l++){
                int ln2, type_2;
                type_2 = ptype[jindx[l] + k];
                if (type_2==0) continue;
                ln2 = i - 1 + n - l;
                if(ln1+ln2>MAXLOOP) continue;
                tmp2 += qb[my_iindx[k] - l]
                        * exp_E_IntLoop(ln2,
                                        ln1,
                                        rtype[type_2],
                                        rt,
                                        S1[l+1],
                                        S1[k-1],
                                        S1[i-1],
                                        S1[j+1],
                                        pf_params)
                        * scale[ln1 + ln2];
              }
            }
            /* 1.3 Exterior multiloop decomposition */
            /* 1.3.1 Middle part                    */
            if((i>TURN+2) && (j<n-TURN-1))
              tmp2 += qm[my_iindx[1]-i+1]
                      * qm[my_iindx[j+1]-n]
                      * expMLclosing
                      * exp_E_MLstem(type, S1[i-1], S1[j+1], pf_params);

            /* 1.3.2 Left part  */
            for(k=TURN+2; k < i-TURN-2; k++)
              tmp2 += qm[my_iindx[1]-k]
                      * qm1[jindx[i-1]+k+1]
                      * expMLbase[n-j]
                      * expMLclosing
                      * exp_E_MLstem(type, S1[i-1], S1[j+1], pf_params);

            /* 1.3.3 Right part */
            for(k=j+TURN+2; k < n-TURN-1;k++)
              tmp2 += qm[my_iindx[j+1]-k]
                      * qm1[jindx[n]+k+1]
                      * expMLbase[i-1]
                      * expMLclosing
                      * exp_E_MLstem(type, S1[i-1], S1[j+1], pf_params);

            /* all exterior loop decompositions for pair i,j done  */
            probs[ij] *= tmp2;

          }
          else probs[ij] = 0;
        }
      }
    } /* end if(circular)  */
    else {
      for (i=1; i<=n; i++) {
        for (j=i; j<=MIN2(i+TURN,n); j++)
          probs[my_iindx[i]-j] = 0.;

        for (j=i+TURN+1; j<=n; j++) {
          ij = my_iindx[i]-j;
          if(hard_constraints[jindx[j] + i] & IN_EXT_LOOP){
            type      = ptype[jindx[j] + i];
            probs[ij] = q1k[i-1]*qln[j+1]/q1k[n];
            probs[ij] *= exp_E_ExtLoop(type, (i>1) ? S1[i-1] : -1, (j<n) ? S1[j+1] : -1, pf_params);
          } else
            probs[ij] = 0.;
        }
      }
    } /* end if(!circular)  */

    for (l=n; l>TURN+1; l--) {

      /* 2. bonding k,l as substem of 2:loop enclosed by i,j */
      for (k=1; k<l-TURN; k++) {
        kl      = my_iindx[k]-l;
        type_2  = ptype[jindx[l] + k];
        type_2  = rtype[type_2];

        if (qb[kl]==0.) continue;

        if(hard_constraints[jindx[l] + k] & IN_INT_LOOP_ENC){
          for(i = MAX2(1, k - MAXLOOP - 1); i <= k - 1; i++){
            u1 = k - i - 1;
            if(hc_up_int[i+1] < u1) continue;

            for(j = l + 1; j <= MIN2(l + MAXLOOP - k + i + 2, n); j++){
              u2 = j-l-1;
              if(hc_up_int[l+1] < u2) break;

              ij = my_iindx[i] - j;
              if(hard_constraints[jindx[j] + i] & IN_INT_LOOP){
                type = ptype[jindx[j] + i];
                if(probs[ij] > 0){
                  tmp2 =  probs[ij]
                          * scale[u1 + u2 + 2]
                          * exp_E_IntLoop(u1, u2, type, type_2, S1[i+1], S1[j-1], S1[k-1], S1[l+1], pf_params);

                  if(sc){
                    if(sc->boltzmann_factors)
                      tmp2 *=   sc->boltzmann_factors[i+1][u1]
                              * sc->boltzmann_factors[l+1][u2];

                    if(sc->exp_en_basepair)
                      tmp2 *=   sc->exp_en_basepair[ij];

                    if(sc->exp_en_stack){
                      if((i+1 == k) && (j-1 == l)){
                        tmp2 *=   sc->exp_en_stack[i]
                                * sc->exp_en_stack[k]
                                * sc->exp_en_stack[l]
                                * sc->exp_en_stack[j];
                      }
                    }

                    if(sc->exp_f)
                      tmp2 *= sc->exp_f(i, j, k, l, VRNA_DECOMP_PAIR_IL, sc->data);
                  }

                  probs[kl] += tmp2;
                }
              }
            }
          }
        }
      }

      if(with_gquad){
        /* 2.5. bonding k,l as gquad enclosed by i,j */
        FLT_OR_DBL *expintern = &(pf_params->expinternal[0]);
        FLT_OR_DBL qe;

        if(l < n - 3){
          for(k = 2; k <= l - VRNA_GQUAD_MIN_BOX_SIZE; k++){
            kl = my_iindx[k]-l;
            if (G[kl]==0.) continue;
            tmp2 = 0.;
            i = k - 1;
            for(j = MIN2(l + MAXLOOP + 1, n); j > l + 3; j--){
              ij = my_iindx[i] - j;
              type = ptype[jindx[j] + i];
              if(!type) continue;
              qe = (type > 2) ? pf_params->expTermAU : 1.;
              tmp2 +=   probs[ij]
                      * qe
                      * expintern[j-l-1]
                      * pf_params->expmismatchI[type][S1[i+1]][S1[j-1]]
                      * scale[2];
            }
            probs[kl] += tmp2 * G[kl];
          }
        }

        if (l < n - 1){
          for (k=3; k<=l-VRNA_GQUAD_MIN_BOX_SIZE; k++) {
            kl = my_iindx[k]-l;
            if (G[kl]==0.) continue;
            tmp2 = 0.;
            for (i=MAX2(1,k-MAXLOOP-1); i<=k-2; i++){
              u1 = k - i - 1;
              for (j=l+2; j<=MIN2(l + MAXLOOP - u1 + 1,n); j++) {
                ij = my_iindx[i] - j;
                type = ptype[jindx[j] + i];
                if(!type) continue;
                qe = (type > 2) ? pf_params->expTermAU : 1.;
                tmp2 +=   probs[ij]
                        * qe
                        * expintern[u1+j-l-1]
                        * pf_params->expmismatchI[type][S1[i+1]][S1[j-1]]
                        * scale[2];
              }
            }
            probs[kl] += tmp2 * G[kl];
          }
        }

        if(l < n){
          for(k = 4; k <= l - VRNA_GQUAD_MIN_BOX_SIZE; k++){
            kl = my_iindx[k]-l;
            if (G[kl]==0.) continue;
            tmp2 = 0.;
            j = l + 1;
            for (i=MAX2(1,k-MAXLOOP-1); i < k - 3; i++){
              ij = my_iindx[i] - j;
              type = ptype[jindx[j] + i];
              if(!type) continue;
              qe = (type > 2) ? pf_params->expTermAU : 1.;
              tmp2 +=   probs[ij]
                      * qe
                      * expintern[k - i - 1]
                      * pf_params->expmismatchI[type][S1[i+1]][S1[j-1]]
                      * scale[2];
            }
            probs[kl] += tmp2 * G[kl];
          }
        }
      }

      /* 3. bonding k,l as substem of multi-loop enclosed by i,j */
      prm_MLb = 0.;
      if (l<n)
        for (k=2; k<l-TURN; k++) {
          kl = my_iindx[k]-l;
          prmt = prmt1 = 0.0;
          i = k-1; 

          ii = my_iindx[i];     /* ii-j=[i,j]     */
          ll = my_iindx[l+1];   /* ll-j=[l+1,j-1] */
          tt = ptype[jindx[l+1] + i];
          tt = rtype[tt];
          if(hard_constraints[jindx[l+1] + i] & IN_MB_LOOP){
            if(tt){
              prmt1 = probs[ii-(l+1)] * expMLclosing * exp_E_MLstem(tt, S1[l], S1[i+1], pf_params);
              
              if(sc){
                /* which decompositions are covered here? => (i, l+1) -> enclosing pair, (k,l) -> enclosed pair, */
                if(sc->exp_en_basepair)
                  prmt1 *= sc->exp_en_basepair[ii - (l+1)];

/*
                if(sc->exp_f)
                  prmt1 *= sc->exp_f(i, l+1, k, l, , sc->data);
*/
              }
            }
          }
          int lj;
          FLT_OR_DBL ppp;
          ij = my_iindx[i] - (l+2);
          lj = my_iindx[l+1]-(l+1);
          for (j = l + 2; j<=n; j++, ij--, lj--){
            if(hard_constraints[jindx[j] + i] & IN_MB_LOOP){
              tt = ptype[jindx[j] + i];
              tt = rtype[tt];
              if(tt){
                /* which decomposition is covered here? =>
                  i + 1 = k < l < j:
                  (i,j)       -> enclosing pair
                  (k, l)      -> enclosed pair
                  (l+1, j-1)  -> multiloop part with at least one stem
                  
                */
                ppp = probs[ij] * exp_E_MLstem(tt, S1[j-1], S1[i+1], pf_params) * qm[lj];
                if(sc){
                  if(sc->exp_en_basepair)
                    ppp *= sc->exp_en_basepair[ij];
/*
                  if(sc->exp_f)
                    ppp *= sc->exp_f(i, j, l+1, j-1, , sc->data);
*/
                }
                prmt += ppp;
              }
            }
          }
          prmt *= expMLclosing;

          tt = ptype[jindx[l] + k];

          prml[ i] = prmt;

          /* l+1 is unpaired */
          ppp = prm_l1[i] * expMLbase[1];
          if(sc){
            if(sc->boltzmann_factors)
              ppp *= sc->boltzmann_factors[l+1][1];

/*
            if(sc_exp_f)
              ppp *= sc->exp_f(, sc->data);
*/
          }
          prm_l[i] = ppp+prmt1;

          /* i is unpaired */
          ppp = prm_MLb*expMLbase[1];
          if(sc){
            if(sc->boltzmann_factors)
              ppp *= sc->boltzmann_factors[i][1];

/*
            if(sc->exp_f)
              ppp *= sc->exp_f(, sc->data);
*/
          }
          prm_MLb = ppp + prml[i];
          /* same as:    prm_MLb = 0;
             for (i=1; i<=k-1; i++) prm_MLb += prml[i]*expMLbase[k-i-1]; */

          prml[i] = prml[ i] + prm_l[i];

          if(with_gquad){
            if ((!tt) && (G[kl] == 0.)) continue;
          } else {
            if (qb[kl] == 0.) continue;
          }

          if(hard_constraints[jindx[l] + k] & IN_MB_LOOP_ENC){

            temp = prm_MLb;

            for (i=1;i<=k-2; i++)
              temp += prml[i]*qm[my_iindx[i+1] - (k-1)];

            if(with_gquad){
              if(tt)
                temp    *= exp_E_MLstem(tt, (k>1) ? S1[k-1] : -1, (l<n) ? S1[l+1] : -1, pf_params) * scale[2];
              else
                temp    *= G[kl] * expMLstem * scale[2];
            } else {
              temp    *= exp_E_MLstem(tt, (k>1) ? S1[k-1] : -1, (l<n) ? S1[l+1] : -1, pf_params) * scale[2];
            }

            probs[kl]  += temp;

            if (probs[kl]>Qmax) {
              Qmax = probs[kl];
              if (Qmax>max_real/10.)
                fprintf(stderr, "P close to overflow: %d %d %g %g\n",
                  i, j, probs[kl], qb[kl]);
            }
            if (probs[kl]>=max_real) {
              ov++;
              probs[kl]=FLT_MAX;
            }
          }
        } /* end for (k=..) */
      tmp = prm_l1; prm_l1=prm_l; prm_l=tmp;

    }  /* end for (l=..)   */

    for (i=1; i<=n; i++)
      for (j=i+TURN+1; j<=n; j++) {
        ij = my_iindx[i]-j;

        if(with_gquad){
          if (qb[ij] > 0.){
            probs[ij] *= qb[ij];
          }

          if (G[ij] > 0.){
            probs[ij] += q1k[i-1] * G[ij] * qln[j+1]/q1k[n];
          }
        } else {
          if (qb[ij] > 0.){
            probs[ij] *= qb[ij];
          }
        }
      }

    if (structure!=NULL)
      bppm_to_structure(structure, probs, n);
    if (ov>0) fprintf(stderr, "%d overflows occurred while backtracking;\n"
        "you might try a smaller pf_scale than %g\n",
        ov, pf_params->pf_scale);

    /* clean up */
    free(q1k);
    free(qln);
    free(prm_l);
    free(prm_l1);
    free(prml);

  } /* end if((S != NULL) && (S1 != NULL))  */
  else
    nrerror("bppm calculations have to be done after calling forward recursion\n");
  return;
}

#if 0
PRIVATE void scale_pf_params(unsigned int length, pf_paramT *parameters){
  unsigned int  i;
  double        scaling_factor;

  if(pf_params) free(pf_params);

  if(parameters){
    pf_params = get_boltzmann_factor_copy(parameters);
  } else {
    model_detailsT md;
    set_model_details(&md);
    pf_params = get_boltzmann_factors(temperature, 1.0, md, pf_scale);
  }

  fill_pair_matrices(&(pf_params->model_details));

  scaling_factor = pf_params->pf_scale;

  /* scaling factors (to avoid overflows) */
  if (scaling_factor == -1) { /* mean energy for random sequences: 184.3*length cal */
    scaling_factor = exp(-(-185+(pf_params->temperature-37.)*7.27)/pf_params->kT);
    if (scaling_factor<1) scaling_factor=1;
    pf_params->pf_scale = scaling_factor;
    pf_scale = pf_params->pf_scale; /* compatibility with RNAup, may be removed sometime */
  }
  scale[0] = 1.;
  scale[1] = 1./scaling_factor;
  expMLbase[0] = 1;
  expMLbase[1] = pf_params->expMLbase/scaling_factor;
  for (i=2; i<=length; i++) {
    scale[i] = scale[i/2]*scale[i-(i/2)];
    expMLbase[i] = pow(pf_params->expMLbase, (double)i) * scale[i];
  }
}
#endif

/*---------------------------------------------------------------------------*/

PRIVATE void
wrap_update_pf_params(int length,
                      pf_paramT *parameters){

  pf_paramT *p = NULL;
  if(parameters == NULL){
    model_detailsT md;
    set_model_details(&md);
    p = get_boltzmann_factors(temperature, 1.0, md, pf_scale);
  }
  vrna_update_pf_params(backward_compat_compound, p);
  free(p);
}

PUBLIC void
vrna_update_pf_params(vrna_fold_compound *vc,
                      pf_paramT *params){

  if(vc){
    if(params){
      if(vc->exp_params)
        free(vc->exp_params);
      vc->exp_params = get_boltzmann_factor_copy(params);
    }

    /* fill additional helper arrays for scaling etc. */
    int i;
    double scaling_factor = vc->exp_params->pf_scale;
    if (scaling_factor == -1) { /* mean energy for random sequences: 184.3*length cal */
      scaling_factor = exp(-(-185+(vc->exp_params->temperature-37.)*7.27)/vc->exp_params->kT);
      if (scaling_factor<1) scaling_factor=1;
      vc->exp_params->pf_scale = scaling_factor;
      pf_scale = vc->exp_params->pf_scale; /* compatibility with RNAup, may be removed sometime */
    }
    vc->exp_matrices->scale[0] = 1.;
    vc->exp_matrices->scale[1] = 1./scaling_factor;
    vc->exp_matrices->expMLbase[0] = 1;
    vc->exp_matrices->expMLbase[1] = vc->exp_params->expMLbase/scaling_factor;
    for (i=2; i<=vc->length; i++) {
      vc->exp_matrices->scale[i] = vc->exp_matrices->scale[i/2]*vc->exp_matrices->scale[i-(i/2)];
      vc->exp_matrices->expMLbase[i] = pow(vc->exp_params->expMLbase, (double)i) * vc->exp_matrices->scale[i];
    }

  }
}

/*---------------------------------------------------------------------------*/

PUBLIC char
bppm_symbol(const float *x){

/*  if( ((x[1]-x[2])*(x[1]-x[2]))<0.1&&x[0]<=0.677) return '|'; */
  if( x[0] > 0.667 )  return '.';
  if( x[1] > 0.667 )  return '(';
  if( x[2] > 0.667 )  return ')';
  if( (x[1]+x[2]) > x[0] ) {
    if( (x[1]/(x[1]+x[2])) > 0.667) return '{';
    if( (x[2]/(x[1]+x[2])) > 0.667) return '}';
    else return '|';
  }
  if( x[0] > (x[1]+x[2]) ) return ',';
  return ':';
}

PUBLIC void
bppm_to_structure(char *structure,
                  FLT_OR_DBL *p,
                  unsigned int length){

  int    i, j;
  int   *index = get_iindx(length);
  float  P[3];   /* P[][0] unpaired, P[][1] upstream p, P[][2] downstream p */

  for( j=1; j<=length; j++ ) {
    P[0] = 1.0;
    P[1] = P[2] = 0.0;
    for( i=1; i<j; i++) {
      P[2] += p[index[i]-j];    /* j is paired downstream */
      P[0] -= p[index[i]-j];    /* j is unpaired */
    }
    for( i=j+1; i<=length; i++ ) {
      P[1] += p[index[j]-i];    /* j is paired upstream */
      P[0] -= p[index[j]-i];    /* j is unpaired */
    }
    structure[j-1] = bppm_symbol(P);
  }
  structure[length] = '\0';
  free(index);
}


/*
  stochastic backtracking in pf_fold arrays
  returns random structure S with Boltzman probabilty
  p(S) = exp(-E(S)/kT)/Z
*/
PUBLIC char *
vrna_pbacktrack(vrna_fold_compound *vc){

  return vrna_pbacktrack5(vc, vc->length);
}

PUBLIC char *
vrna_pbacktrack5( vrna_fold_compound *vc,
                  int length){

  double            r, qt, q_temp, qkl;
  int               i,j,ij, n, k, start, type;
  char              *sequence, *pstruc;
  int               *my_iindx, *jindx, hc_decompose;
  FLT_OR_DBL        *q, *qb, *scale;
  char              *ptype, *hard_constraints;
  short             *S, *S1;
  pf_matricesT      *matrices;
  hard_constraintT  *hc;
  soft_constraintT  *sc;
  pf_paramT         *pf_params;

  sequence  = vc->sequence;
  n         = vc->length;

  pf_params = vc->exp_params;
  my_iindx  = vc->iindx;
  jindx     = vc->jindx;
  matrices  = vc->exp_matrices;

  hc        = vc->hc;
  sc        = vc->sc;
  ptype     = vc->ptype;
  S         = vc->sequence_encoding2;
  S1        = vc->sequence_encoding;

  q         = matrices->q;
  qb        = matrices->qb;
  scale     = matrices->scale;

  hard_constraints  = hc->matrix;

  FLT_OR_DBL *q1k   = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+1));
  FLT_OR_DBL *qln   = (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(n+2));

  if(length > n)
    nrerror("part_func.c@pbacktrack5: 3'-end exceeds sequence length");
  else if(length < 1)
    nrerror("part_func.c@pbacktrack5: 3'-end too small");

/*
  if (init_length<1)
    nrerror("can't backtrack without pf arrays.\n"
            "Call pf_fold() before pbacktrack()");
*/

  pstruc = space((length+1)*sizeof(char));

  for (i=0; i<length; i++)
    pstruc[i] = '.';

  for (k=1; k<=length; k++) {
    q1k[k] = q[my_iindx[1] - k];
    qln[k] = q[my_iindx[k] - length];
  }
  q1k[0] = 1.0;
  qln[length+1] = 1.0;

  start = 1;
  while (start<length) {
  /* find i position of first pair */
    for (i=start; i<length; i++) {
      r = urn() * qln[i];
      q_temp = qln[i+1]*scale[1];

      if(sc){
        if (sc->boltzmann_factors)  
          q_temp *= sc->boltzmann_factors[i][1];

        if(sc->exp_f)
          q_temp *= sc->exp_f(i, length, i+1, n, VRNA_DECOMP_EXT_UP_5, sc->data);
      }

      if (r > q_temp)  break; /* i is paired */
    }
    if (i>=length) break; /* no more pairs */
    /* now find the pairing partner j */
    r = urn() * (qln[i] - q_temp);
    for (qt=0, j=i+1; j<=length; j++) {
      ij            = my_iindx[i]-j;
      type          = ptype[jindx[j] + i];
      hc_decompose  = hard_constraints[jindx[j] + i];
      if (hc_decompose & IN_EXT_LOOP) {
        qkl = qb[ij] * exp_E_ExtLoop(type, (i>1) ? S1[i-1] : -1, (j<n) ? S1[j+1] : -1, pf_params);

        if (j<length){
          qkl *= qln[j+1];
          if(sc){
            if(sc->exp_f)
              qkl *= sc->exp_f(i, length, j, length, VRNA_DECOMP_EXT_EXT, sc->data);
          }
        } else {
          if(sc){
            if(sc->exp_f)
              qkl *= sc->exp_f(i, length, j, length, VRNA_DECOMP_EXT_STEM_UP, sc->data);
          }
        }

        qt += qkl;
        if (qt > r) break; /* j is paired */
      }
    }
    if (j==length+1) nrerror("backtracking failed in ext loop");
    start = j+1;
    backtrack(i,j, pstruc, vc);
  }

  /* clean up */
  free(q1k);
  free(qln);
  return pstruc;
}

PUBLIC char *
pbacktrack_circ(char *seq){

  return vrna_pbacktrack_circ(backward_compat_compound);

}


PUBLIC char *
vrna_pbacktrack_circ(vrna_fold_compound *vc){

  double r, qt;
  int i, j, k, l, n;
  pf_paramT   *pf_params;
  FLT_OR_DBL  qo, qho, qio, qmo;
  FLT_OR_DBL  *scale, *qb, *qm, *qm2;
  char        *sequence, *ptype, *pstruc;
  int         *my_iindx, *jindx;
  short             *S, *S1;

  pf_matricesT  *matrices;

  pf_params     = vc->exp_params;
  matrices      = vc->exp_matrices;
  ptype         = vc->ptype;
  my_iindx      = vc->iindx;
  jindx         = vc->jindx;
  S             = vc->sequence_encoding2;
  S1            = vc->sequence_encoding;

  qo            = matrices->qo;
  qho           = matrices->qho;
  qio           = matrices->qio;
  qmo           = matrices->qmo;
  qb            = matrices->qb;
  qm            = matrices->qm;
  qm2           = matrices->qm2;
  scale         = matrices->scale;

  FLT_OR_DBL  expMLclosing  = pf_params->expMLclosing;
  int         *rtype        = &(pf_params->model_details.rtype[0]);

  sequence  = vc->sequence;
  n         = vc->length;

/*
  if (init_length<1)
    nrerror("can't backtrack without pf arrays.\n"
      "Call pf_circ_fold() before pbacktrack_circ()");
*/

  pstruc = space((n+1)*sizeof(char));

  /* initialize pstruct with single bases  */
  for (i=0; i<n; i++) pstruc[i] = '.';

  qt = 1.0*scale[n];
  r = urn() * qo;

  /* open chain? */
  if(qt > r) return pstruc;

  for(i=1; (i < n); i++){
    for(j=i+TURN+1;(j<=n); j++){

      int type, u;
      /* 1. first check, wether we can do a hairpin loop  */
      u = n-j + i-1;
      if (u<TURN) continue;

      type = ptype[jindx[j] + i];
      if (!type) continue;

      type=rtype[type];

      char loopseq[10];
      if (u<7){
        strcpy(loopseq , sequence+j-1);
        strncat(loopseq, sequence, i);
      }

      qt += qb[my_iindx[i]-j] * exp_E_Hairpin(u, type, S1[j+1], S1[i-1],  loopseq, pf_params) * scale[u];
      /* found a hairpin? so backtrack in the enclosed part and we're done  */
      if(qt>r){ backtrack(i,j, pstruc, vc); return pstruc;}

      /* 2. search for (k,l) with which we can close an interior loop  */
      for(k=j+1; (k < n); k++){
        int ln1, lstart;
        ln1 = k - j - 1;
        if(ln1+i-1>MAXLOOP) break;

        lstart = ln1+i-1+n-MAXLOOP;
        if(lstart<k+TURN+1) lstart = k + TURN + 1;
        for(l=lstart; (l <= n); l++){
            int ln2, type2;
            ln2 = (i - 1) + (n - l);
            if((ln1+ln2) > MAXLOOP) continue;

            type2 = ptype[jindx[l] + k];
            if(!type) continue;
            type2 = rtype[type2];
            qt += qb[my_iindx[i]-j] * qb[my_iindx[k]-l] * exp_E_IntLoop(ln2, ln1, type2, type, S1[l+1], S1[k-1], S1[i-1], S1[j+1], pf_params) * scale[ln1 + ln2];
            /* found an exterior interior loop? also this time, we can go straight  */
            /* forward and backtracking the both enclosed parts and we're done      */
            if(qt>r){ backtrack(i,j, pstruc, vc); backtrack(k,l, pstruc, vc); return pstruc;}
        }
      } /* end of kl double loop */
    }
  } /* end of ij double loop  */
  {
    /* as we reach this part, we have to search for our barrier between qm and qm2  */
    qt = 0.;
    r = urn()*qmo;
    for(k=TURN+2; k<n-2*TURN-3; k++){
      qt += qm[my_iindx[1]-k] * qm2[k+1] * expMLclosing;
      /* backtrack in qm and qm2 if we've found a valid barrier k  */
      if(qt>r){ backtrack_qm(1,k, pstruc, vc); backtrack_qm2(k+1,n, pstruc, vc); return pstruc;}
    }
  }
  /* if we reach the actual end of this function, an error has occured  */
  /* cause we HAVE TO find an exterior loop or an open chain!!!         */
  nrerror("backtracking failed in exterior loop");
  return pstruc;
}

PRIVATE void
backtrack_qm( int i,
              int j,
              char *pstruc,
              vrna_fold_compound *vc){

  /* divide multiloop into qm and qm1  */
  double qmt, r, q_temp;
  int k, n, nomorepairs = 0;
  FLT_OR_DBL  *qm, *qm1, *expMLbase, *scale;
  int         *my_iindx, *jindx;
  hard_constraintT  *hc;
  soft_constraintT  *sc;

  n = j;
  pf_matricesT  *matrices = vc->exp_matrices;

  my_iindx  = vc->iindx;
  jindx     = vc->jindx;

  hc        = vc->hc;
  sc        = vc->sc;

  qm        = matrices->qm;
  qm1       = matrices->qm1;
  expMLbase = matrices->expMLbase;
  scale     = matrices->scale;


  while(j>i){
    /* now backtrack  [i ... j] in qm[] */
    r = urn() * qm[my_iindx[i] - j];
    qmt = qm1[jindx[j]+i]; k=i;
    if(qmt<r)
      for(k=i+1; k<=j; k++){
        q_temp = expMLbase[k-i] * qm1[jindx[j]+k];

        if(sc){
          if(sc->boltzmann_factors)
            q_temp *= sc->boltzmann_factors[i][k-i];

          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_ML_UP_5, sc->data);
        }

        qmt += q_temp;

        q_temp = qm[my_iindx[i]-(k-1)] * qm1[jindx[j]+k];

        if(sc){
          if(sc->exp_f)
            q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_ML_ML_ML, sc->data);
        }

        qmt += q_temp;

        if(qmt >= r){ break;}
      }
    if(k>j) nrerror("backtrack failed in qm");

    backtrack_qm1(k, j, pstruc, vc);

    if(k<i+TURN) break; /* no more pairs */
    
    q_temp = expMLbase[k-i];

    if(sc){
      if(sc->boltzmann_factors)
        q_temp *= sc->boltzmann_factors[i][k-i];

      if(sc->exp_f)
        q_temp *= sc->exp_f(i, k-1, n, n, VRNA_DECOMP_ML_UP, sc->data);
    }

    r = urn() * (qm[my_iindx[i]-(k-1)] + q_temp);
    if(q_temp >= r) break;

    j = k-1;
  }
}

PRIVATE void
backtrack_qm1(int i,
              int j,
              char *pstruc,
              vrna_fold_compound *vc){

  /* i is paired to l, i<l<j; backtrack in qm1 to find l */
  int ii, l, type, n;
  double qt, r, q_temp;
  FLT_OR_DBL  *qm1, *qb, *scale, *expMLbase;
  pf_matricesT  *matrices;
  int           *my_iindx, *jindx;
  char          *ptype;
  short             *S, *S1;
  hard_constraintT  *hc;
  soft_constraintT  *sc;
  pf_paramT         *pf_params;


  pf_params = vc->exp_params;
  my_iindx  = vc->iindx;
  jindx     = vc->jindx;

  ptype     = vc->ptype;

  hc        = vc->hc;
  sc        = vc->sc;

  matrices  = vc->exp_matrices;
  qb        = matrices->qb;
  qm1       = matrices->qm1;
  scale     = matrices->scale;
  expMLbase = matrices->expMLbase;
  S         = vc->sequence_encoding2;
  S1        = vc->sequence_encoding;


  n = j;
  r = urn() * qm1[jindx[j]+i];
  ii = my_iindx[i];
  for (qt=0., l=i+TURN+1; l<=j; l++) {
    type = ptype[jindx[l] + i];
    if (type){
      q_temp =  qb[ii-l]
                * exp_E_MLstem(type, S1[i-1], S1[l+1], pf_params)
                * expMLbase[j-l];

      if(sc){
        if(sc->boltzmann_factors)
          q_temp *= sc->boltzmann_factors[l+1][j-l];

        if(sc->exp_f)
          q_temp *= sc->exp_f(i, j, l, n, VRNA_DECOMP_ML_UP_3, sc->data);
      }

      qt += q_temp;
    }
    if (qt>=r) break;
  }
  if (l>j) nrerror("backtrack failed in qm1");
  backtrack(i, l, pstruc, vc);
}

PRIVATE void
backtrack_qm2(int k,
              int n,
              char *pstruc,
              vrna_fold_compound *vc){

  double qom2t, r;
  int u;
  FLT_OR_DBL *qm1, *qm2;
  int *my_iindx, *jindx;

  my_iindx = vc->iindx;
  jindx     = vc->jindx;
  qm1       = vc->exp_matrices->qm1;
  qm2       = vc->exp_matrices->qm2;

  r= urn()*qm2[k];
  /* we have to search for our barrier u between qm1 and qm1  */
  for (qom2t = 0.,u=k+TURN+1; u<n-TURN-1; u++){
    qom2t += qm1[jindx[u]+k]*qm1[jindx[n]+(u+1)];
    if(qom2t > r) break;
  }
  if(u==n-TURN) nrerror("backtrack failed in qm2");
  backtrack_qm1(k, u, pstruc, vc);
  backtrack_qm1(u+1, n, pstruc, vc);
}

PRIVATE void
backtrack(int i,
          int j,
          char *pstruc,
          vrna_fold_compound *vc){

  char              *ptype, *sequence;
  pf_paramT         *pf_params;
  FLT_OR_DBL        *qb, *qm, *qm1, *scale;
  pf_matricesT      *matrices;
  int               *my_iindx, *jindx;
  hard_constraintT  *hc;
  soft_constraintT  *sc;
  short             *S, *S1;

  sequence    = vc->sequence;
  pf_params   = vc->exp_params;
  ptype       = vc->ptype;
  S           = vc->sequence_encoding2;
  S1          = vc->sequence_encoding;
  my_iindx    = vc->iindx;
  jindx       = vc->jindx;

  hc          = vc->hc;
  sc          = vc->sc;

  matrices    = vc->exp_matrices;
  qb          = matrices->qb;
  qm          = matrices->qm;
  qm1         = matrices->qm1;
  scale       = matrices->scale;

  int noGUclosure = pf_params->model_details.noGUclosure;
  int   *rtype    = &(pf_params->model_details.rtype[0]);
  int n;
  double r, qbt1, qt, q_temp;
  n = j;
  do {
    int k, l, type, u, u1;

    pstruc[i-1] = '('; pstruc[j-1] = ')';

    r = urn() * qb[my_iindx[i]-j];
    type = ptype[jindx[j] + i];
    u = j-i-1;
    /*hairpin contribution*/
    if (((type==3)||(type==4))&&noGUclosure) qbt1 = 0;
    else{
      q_temp = exp_E_Hairpin(u, type, S1[i+1], S1[j-1], sequence+i-1, pf_params) * scale[u+2];

      if(sc){
        if(sc->boltzmann_factors)
          q_temp *= sc->boltzmann_factors[i+1][u];

        if(sc->exp_f)
          q_temp *= sc->exp_f(i, j, n, n, VRNA_DECOMP_PAIR_HP, sc->data);
      }

      qbt1 = q_temp;

    }
    if (qbt1>=r) return; /* found the hairpin we're done */

    for (k=i+1; k<=MIN2(i+MAXLOOP+1,j-TURN-2); k++) {
      u1 = k-i-1;
      for (l=MAX2(k+TURN+1,j-1-MAXLOOP+u1); l<j; l++) {
        int type_2 = ptype[jindx[l] + k];
        if (type_2) {
          int u2 = j-l-1;
          type_2 = rtype[type_2];
          /* add *scale[u1+u2+2] */
          q_temp = qb[my_iindx[k]-l]
                   * scale[u1+u2+2]
                   * exp_E_IntLoop(u1, u2, type, type_2, S1[i+1], S1[j-1], S1[k-1], S1[l+1], pf_params);

          if(sc){
            if(sc->boltzmann_factors)
              q_temp *=   sc->boltzmann_factors[i+1][u1]
                        * sc->boltzmann_factors[l+1][u2];

            if(sc->exp_en_stack)
              if((i + 1 == k) && (j - 1 == l))
                q_temp *=   sc->exp_en_stack[i]
                          * sc->exp_en_stack[k]
                          * sc->exp_en_stack[l]
                          * sc->exp_en_stack[j];

            if(sc->exp_f)
              q_temp *= sc->exp_f(i, j, k, l, VRNA_DECOMP_PAIR_IL, sc->data);
          }

          qbt1 += q_temp;
        }
        if (qbt1 > r) break;
      }
      if (qbt1 > r) break;
    }
    if (l<j) {
      i=k; j=l;
    }
    else break;
  } while (1);

  /* backtrack in multi-loop */
  {
    int k, ii, jj, ij;

    i++; j--;
    /* find the first split index */
    ii = my_iindx[i]; /* ii-j=[i,j] */
    jj = jindx[j]; /* jj+i=[j,i] */
    for (qt=0., k=i+1; k<j; k++){
      q_temp = qm[ii-(k-1)] * qm1[jj+k];

      if(sc){
        if(sc->exp_f)
          q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_ML_ML_ML, sc->data);
      }

      qt += q_temp;
    }
    r = urn() * qt;
    for (qt=0., k=i+1; k<j; k++) {
      q_temp = qm[ii-(k-1)] * qm1[jj+k];

      if(sc){
        if(sc->exp_f)
          q_temp *= sc->exp_f(i, j, k, n, VRNA_DECOMP_ML_ML_ML, sc->data);
      }

      qt += q_temp;

      if (qt>=r) break;
    }
    if (k>=j) nrerror("backtrack failed, can't find split index ");

    backtrack_qm1(k, j, pstruc, vc);

    j = k-1;
    backtrack_qm(i, j, pstruc, vc);
  }
}

PUBLIC void
assign_plist_from_pr( plist **pl,
                      FLT_OR_DBL *probs,
                      int length,
                      double cut_off){

  int i, j, n, count, *index;
  count = 0;
  n     = 2;

  index = get_iindx(length);

  /* first guess of the size needed for pl */
  *pl = (plist *)space(n*length*sizeof(plist));

  for (i=1; i<length; i++) {
    for (j=i+1; j<=length; j++) {
      /* skip all entries below the cutoff */
      if (probs[index[i]-j] < cut_off) continue;
      /* do we need to allocate more memory? */
      if (count == n * length - 1){
        n *= 2;
        *pl = (plist *)xrealloc(*pl, n * length * sizeof(plist));
      }
      (*pl)[count].i      = i;
      (*pl)[count].j      = j;
      (*pl)[count].p      = probs[index[i] - j];
      (*pl)[count++].type = 0;
    }
  }
  /* mark the end of pl */
  (*pl)[count].i      = 0;
  (*pl)[count].j      = 0;
  (*pl)[count].p      = 0.;
  (*pl)[count++].type = 0;
  /* shrink memory to actual size needed */
  *pl = (plist *)xrealloc(*pl, count * sizeof(plist));

  free(index);
}

/* this doesn't work if free_pf_arrays() is called before */
PUBLIC void
assign_plist_gquad_from_pr( plist **pl,
                            int length,
                            double cut_off){

  int i, j, k, n, count, *index;
  FLT_OR_DBL  *probs, *G, *scale;
  pf_matricesT  *matrices;
  short         *S;
  pf_paramT     *pf_params;

  if(!backward_compat_compound){
    *pl = NULL;
    return;
  } else if( !backward_compat_compound->exp_matrices->probs){
    *pl = NULL;
    return;
  }

  pf_params = backward_compat_compound->exp_params;
  S         = backward_compat_compound->sequence_encoding2;
  matrices  = backward_compat_compound->exp_matrices;
  probs     = matrices->probs;
  G         = matrices->G;
  scale     = matrices->scale;
  index     = backward_compat_compound->iindx;

  count = 0;
  n     = 2;

  /* first guess of the size needed for pl */
  *pl = (plist *)space(n*length*sizeof(plist));

  for (i=1; i<length; i++) {
    for (j=i+1; j<=length; j++) {
      /* skip all entries below the cutoff */
      if (probs[index[i]-j] < cut_off) continue;

      /* do we need to allocate more memory? */
      if (count == n * length - 1){
        n *= 2;
        *pl = (plist *)xrealloc(*pl, n * length * sizeof(plist));
      }

      /* check for presence of gquadruplex */
      if((S[i] == 3) && (S[j] == 3)){
          /* add probability of a gquadruplex at position (i,j)
             for dot_plot
          */
          (*pl)[count].i      = i;
          (*pl)[count].j      = j;
          (*pl)[count].p      = probs[index[i] - j];
          (*pl)[count++].type = 1;
          /* now add the probabilies of it's actual pairing patterns */
          plist *inner, *ptr;
          inner = get_plist_gquad_from_pr(S, i, j, G, probs, scale, pf_params);
          for(ptr=inner; ptr->i != 0; ptr++){
              if (count == n * length - 1){
                n *= 2;
                *pl = (plist *)xrealloc(*pl, n * length * sizeof(plist));
              }
              /* check if we've already seen this pair */
              for(k = 0; k < count; k++)
                if(((*pl)[k].i == ptr->i) && ((*pl)[k].j == ptr->j))
                  break;
              (*pl)[k].i      = ptr->i;
              (*pl)[k].j      = ptr->j;
              (*pl)[k].type = 0;
              if(k == count){
                (*pl)[k].p  = ptr->p;
                count++;
              } else
                (*pl)[k].p  += ptr->p;
          }
      } else {
          (*pl)[count].i      = i;
          (*pl)[count].j      = j;
          (*pl)[count].p      = probs[index[i] - j];
          (*pl)[count++].type = 0;
      }
    }
  }
  /* mark the end of pl */
  (*pl)[count].i    = 0;
  (*pl)[count].j    = 0;
  (*pl)[count].type = 0;
  (*pl)[count++].p  = 0.;
  /* shrink memory to actual size needed */
  *pl = (plist *)xrealloc(*pl, count * sizeof(plist));
  free(index);
}



PUBLIC plist *
stackProb(double cutoff){

  plist *pl;
  int i,j,plsize=256;
  int num = 0;
  pf_paramT *pf_params;
  FLT_OR_DBL    *qb, *probs, *scale;
  pf_matricesT  *matrices;
  char          *ptype;

  if(!backward_compat_compound){
    nrerror("stackProb: run vrna_pf_fold() first!");
  } else if( !backward_compat_compound->exp_matrices->probs){
    nrerror("stackProb: probs==NULL!");
  }

  pf_params   = backward_compat_compound->exp_params;
  int length  = backward_compat_compound->length;
  int *index  = backward_compat_compound->iindx;
  int *jindx  = backward_compat_compound->jindx;
  int *rtype  = &(pf_params->model_details.rtype[0]);

  ptype       = backward_compat_compound->ptype;
  matrices    = backward_compat_compound->exp_matrices;
  qb          = matrices->qb;
  probs       = matrices->probs;
  scale       = matrices->scale;

  pl = (plist *) space(plsize*sizeof(plist));

  for (i=1; i<length; i++)
    for (j=i+TURN+3; j<=length; j++) {
      double p;
      if((p=probs[index[i]-j]) < cutoff) continue;
      if (qb[index[i+1]-(j-1)]<FLT_MIN) continue;
      p *= qb[index[i+1]-(j-1)]/qb[index[i]-j];
      p *= exp_E_IntLoop(0,0,ptype[jindx[j]+i],rtype[ptype[jindx[j-1] + i+1]],
                         0,0,0,0, pf_params)*scale[2];/* add *scale[u1+u2+2] */
      if (p>cutoff) {
        pl[num].i     = i;
        pl[num].j     = j;
        pl[num].type  = 0;
        pl[num++].p   = p;
        if (num>=plsize) {
          plsize *= 2;
          pl = xrealloc(pl, plsize*sizeof(plist));
        }
      }
    }
  pl[num].i=0;
  free(index);
  return pl;
}

/* get the free energy of a subsequence from the q[] array */
PUBLIC double
get_subseq_F( int i,
              int j){

  if(backward_compat_compound)
    if(backward_compat_compound->exp_matrices)
      if(backward_compat_compound->exp_matrices->q){
        int       *my_iindx   = backward_compat_compound->iindx;
        pf_paramT *pf_params  = backward_compat_compound->exp_params;
        FLT_OR_DBL  *q        = backward_compat_compound->exp_matrices->q;
        return ((-log(q[my_iindx[i]-j])-(j-i+1)*log(pf_params->pf_scale))*pf_params->kT/1000.0);
      }

  nrerror("call pf_fold() to fill q[] array before calling get_subseq_F()");
}


PUBLIC double
mean_bp_distance(int length){

  if(backward_compat_compound)
    if(backward_compat_compound->exp_matrices)
      if(backward_compat_compound->exp_matrices->probs)
        return mean_bp_distance_pr(backward_compat_compound->length, backward_compat_compound->exp_matrices->probs);

  nrerror("mean_bp_distance: you need to call vrna_pf_fold first");
}

PUBLIC double
mean_bp_distance_pr(int length,
                    FLT_OR_DBL *p){

  /* compute the mean base pair distance in the thermodynamic ensemble */
  /* <d> = \sum_{a,b} p_a p_b d(S_a,S_b)
     this can be computed from the pair probs p_ij as
     <d> = \sum_{ij} p_{ij}(1-p_{ij}) */
  int i,j;
  double d=0;
  int *index = get_iindx((unsigned int) length);

  if (p==NULL)
    nrerror("p==NULL. You need to supply a valid probability matrix for mean_bp_distance_pr()");

  for (i=1; i<=length; i++)
    for (j=i+TURN+1; j<=length; j++)
      d += p[index[i]-j] * (1-p[index[i]-j]);

  free(index);
  return 2*d;
}


/*###########################################*/
/*# deprecated functions below              #*/
/*###########################################*/

/* this function is deprecated since it is not threadsafe */
PUBLIC char *
centroid( int length,
          double *dist) {

  if (pr==NULL)
    nrerror("pr==NULL. You need to call pf_fold() before centroid()");

  return vrna_get_centroid_struct_pr(length, dist, pr);
}


/* This function is deprecated since it uses the global array pr for calculations */
PUBLIC double
mean_bp_dist(int length) {

  /* compute the mean base pair distance in the thermodynamic ensemble */
  /* <d> = \sum_{a,b} p_a p_b d(S_a,S_b)
     this can be computed from the pair probs p_ij as
     <d> = \sum_{ij} p_{ij}(1-p_{ij}) */
  int i,j;
  double d=0;

  if (pr==NULL)
    nrerror("pr==NULL. You need to call pf_fold() before mean_bp_dist()");

  int *my_iindx = get_iindx(length);

  for (i=1; i<=length; i++)
    for (j=i+TURN+1; j<=length; j++)
      d += pr[my_iindx[i]-j] * (1-pr[my_iindx[i]-j]);

  free(my_iindx);
  return 2*d;
}

/*----------------------------------------------------------------------*/
PUBLIC double
expHairpinEnergy( int u,
                  int type,
                  short si1,
                  short sj1,
                  const char *string) {

/* compute Boltzmann weight of a hairpin loop, multiply by scale[u+2] */

  pf_paramT *pf_params = backward_compat_compound->exp_params;

  double q, kT;
  kT = pf_params->kT;   /* kT in cal/mol  */
  if(u <= 30)
    q = pf_params->exphairpin[u];
  else
    q = pf_params->exphairpin[30] * exp( -(pf_params->lxc*log( u/30.))*10./kT);
  if ((tetra_loop)&&(u==4)) {
    char tl[7]={0}, *ts;
    strncpy(tl, string, 6);
    if ((ts=strstr(pf_params->Tetraloops, tl)))
      return (pf_params->exptetra[(ts-pf_params->Tetraloops)/7]);
  }
  if ((tetra_loop)&&(u==6)) {
    char tl[9]={0}, *ts;
    strncpy(tl, string, 6);
    if ((ts=strstr(pf_params->Hexaloops, tl)))
      return  (pf_params->exphex[(ts-pf_params->Hexaloops)/9]);
  }
  if (u==3) {
    char tl[6]={0}, *ts;
    strncpy(tl, string, 5);
    if ((ts=strstr(pf_params->Triloops, tl)))
      return (pf_params->exptri[(ts-pf_params->Triloops)/6]);
    if (type>2)
      q *= pf_params->expTermAU;
  }
  else /* no mismatches for tri-loops */
    q *= pf_params->expmismatchH[type][si1][sj1];

  return q;
}

PUBLIC double
expLoopEnergy(int u1,
              int u2,
              int type,
              int type2,
              short si1,
              short sj1,
              short sp1,
              short sq1) {

/* compute Boltzmann weight of interior loop,
   multiply by scale[u1+u2+2] for scaling */
  double z=0;
  int no_close = 0;
  pf_paramT *pf_params = backward_compat_compound->exp_params;


  if ((no_closingGU) && ((type2==3)||(type2==4)||(type==2)||(type==4)))
    no_close = 1;

  if ((u1==0) && (u2==0)) /* stack */
    z = pf_params->expstack[type][type2];
  else if (no_close==0) {
    if ((u1==0)||(u2==0)) { /* bulge */
      int u;
      u = (u1==0)?u2:u1;
      z = pf_params->expbulge[u];
      if (u2+u1==1) z *= pf_params->expstack[type][type2];
      else {
        if (type>2) z *= pf_params->expTermAU;
        if (type2>2) z *= pf_params->expTermAU;
      }
    }
    else {     /* interior loop */
      if (u1+u2==2) /* size 2 is special */
        z = pf_params->expint11[type][type2][si1][sj1];
      else if ((u1==1) && (u2==2))
        z = pf_params->expint21[type][type2][si1][sq1][sj1];
      else if ((u1==2) && (u2==1))
        z = pf_params->expint21[type2][type][sq1][si1][sp1];
      else if ((u1==2) && (u2==2))
        z = pf_params->expint22[type][type2][si1][sp1][sq1][sj1];
      else if (((u1==2)&&(u2==3))||((u1==3)&&(u2==2))){ /*2-3 is special*/
        z = pf_params->expinternal[5]*
          pf_params->expmismatch23I[type][si1][sj1]*
          pf_params->expmismatch23I[type2][sq1][sp1];
        z *= pf_params->expninio[2][1];
      }
      else if ((u1==1)||(u2==1)) {  /*1-n is special*/
        z = pf_params->expinternal[u1+u2]*
          pf_params->expmismatch1nI[type][si1][sj1]*
          pf_params->expmismatch1nI[type2][sq1][sp1];
        z *= pf_params->expninio[2][abs(u1-u2)];
      }
      else {
        z = pf_params->expinternal[u1+u2]*
          pf_params->expmismatchI[type][si1][sj1]*
          pf_params->expmismatchI[type2][sq1][sp1];
        z *= pf_params->expninio[2][abs(u1-u2)];
      }
    }
  }
  return z;
}

PUBLIC void
init_pf_circ_fold(int length){
/* DO NOTHING */
}

PUBLIC void
init_pf_fold(int length){
/* DO NOTHING */
}

/**
*** Allocate memory for all matrices and other stuff
**/
PUBLIC void
free_pf_arrays(void){

  if(backward_compat_compound){
    destroy_fold_compound(backward_compat_compound);
    backward_compat_compound = NULL;
    iindx = NULL;
  }
}

PUBLIC FLT_OR_DBL *
export_bppm(void){

  if(backward_compat_compound)
    if(backward_compat_compound->exp_matrices)
      if(backward_compat_compound->exp_matrices->probs)
        return backward_compat_compound->exp_matrices->probs;

  return NULL;
}

/*-------------------------------------------------------------------------*/
/* make arrays used for pf_fold available to other routines */
PUBLIC int
get_pf_arrays(short **S_p,
              short **S1_p,
              char **ptype_p,
              FLT_OR_DBL **qb_p,
              FLT_OR_DBL **qm_p,
              FLT_OR_DBL **q1k_p,
              FLT_OR_DBL **qln_p){

  if(backward_compat_compound){
    if(backward_compat_compound->exp_matrices)
      if(backward_compat_compound->exp_matrices->qb){
        *S_p      = backward_compat_compound->sequence_encoding2;
        *S1_p     = backward_compat_compound->sequence_encoding;
        *ptype_p  = backward_compat_compound->ptype_pf_compat;
        *qb_p     = backward_compat_compound->exp_matrices->qb;
        *qm_p     = backward_compat_compound->exp_matrices->qm;
        *q1k_p    = backward_compat_compound->exp_matrices->q1k;
        *qln_p    = backward_compat_compound->exp_matrices->qln;
        return 1;
      }
  }
  return 0;
}

/*-----------------------------------------------------------------*/
PUBLIC float
pf_fold(const char *sequence,
        char *structure){

  return wrap_pf_fold(sequence, structure, NULL, do_backtrack, fold_constrained, 0, NULL);
}

PUBLIC float
pf_circ_fold( const char *sequence,
              char *structure){

  return wrap_pf_fold(sequence, structure, NULL, do_backtrack, fold_constrained, 1, NULL);
}

PUBLIC float
pf_fold_par(const char *sequence,
            char *structure,
            pf_paramT *parameters,
            int calculate_bppm,
            int is_constrained,
            int is_circular){

  return wrap_pf_fold(sequence, structure, parameters, calculate_bppm, is_constrained, is_circular, NULL);
}

PUBLIC char *
pbacktrack(char *seq){

  int n = (int)strlen(seq);
  return vrna_pbacktrack5(backward_compat_compound, n);
}

PUBLIC char *
pbacktrack5(char *seq,
            int length){

  /* the seq parameter must no differ to the one stored globally anyway, so we just ignore it */
  return vrna_pbacktrack5(backward_compat_compound, length);
}

PUBLIC void
update_pf_params(int length){

  wrap_update_pf_params(length, NULL);
}

PUBLIC void
update_pf_params_par( int length,
                      pf_paramT *parameters){

  wrap_update_pf_params(length, parameters);
}

PUBLIC char *
get_centroid_struct_gquad_pr( int length,
                              double *dist){

  return vrna_get_centroid_struct(backward_compat_compound, dist);
}

