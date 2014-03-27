/* Last changed Time-stamp: <2012-02-15 18:20:49 ivo> */
/*
                  Ineractive Access to folding Routines

                  c Ivo L Hofacker
                  Vienna RNA package
*/

/** \file
*** \brief RNAfold program source code
***
*** This code provides an interface for MFE and Partition function folding
*** of single linear or circular RNA molecules.
**/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include "ViennaRNA/fold.h"
#include "ViennaRNA/part_func.h"
#include "ViennaRNA/fold_vars.h"
#include "ViennaRNA/PS_dot.h"
#include "ViennaRNA/utils.h"
#include "ViennaRNA/read_epars.h"
#include "ViennaRNA/centroid.h"
#include "ViennaRNA/MEA.h"
#include "ViennaRNA/params.h"
#include "ViennaRNA/constraints.h"
#include "ViennaRNA/file_formats.h"
#include "RNAfold_cmdl.h"



/*@unused@*/
static char UNUSED rcsid[] = "$Id: RNAfold.c,v 1.25 2009/02/24 14:22:21 ivo Exp $";

/*--------------------------------------------------------------------------*/

static void
add_shape_constraints(vrna_fold_compound *vc,
                      const char *shape_method,
                      const char *shape_conversion,
                      const char *shape_file,
                      int verbose,
                      unsigned int constraint_type){

  float p1, p2;
  char method;
  char *sequence;
  double *values;
  int length = vc->length;

  if(!parse_soft_constraints_shape_method(shape_method, &method, &p1, &p2)){
    warn_user("Method for SHAPE reactivity data conversion not recognized!");
    return;
  }

  if(verbose){
    fprintf(stderr, "Using SHAPE method '%c'", method);
    if(method != 'W'){
      if(method == 'Z')
        fprintf(stderr, " with parameter p1=%f", p1);
      else
        fprintf(stderr, " with parameters p1=%f and p2=%f", p1, p2);
    }
    fputc('\n', stderr);
  }

  sequence = space(sizeof(char) * (length + 1));
  values = space(sizeof(double) * (length + 1));
  parse_soft_constraints_file(shape_file, length, method == 'W' ? 0 : -1, sequence, values);

  if(method == 'D'){
    int i;
    for (i = 1; i <= length; ++i)
      values[i] = values[i] < 0 ? 0 : p1 * log(values[i] + 1) + p2;

    vrna_sc_add_sp(vc, values, constraint_type);
  }
  else if(method == 'Z'){
    double *sc_up = space(sizeof(double) * (length + 1));
    double **sc_bp = space(sizeof(double *) * (length + 1));
    int i;

    convert_shape_reactivities_to_probabilities(shape_conversion, values, length, 0.5);

    for(i = 1; i <= length; ++i){
      int j;

      assert(values[i] >= 0 && values[i] <= 1);

      sc_up[i] = p1 * fabs(values[i] - 1);
      sc_bp[i] = space(sizeof(double) * (length + 1));
      for(j = i + TURN + 1; j <= length; ++j)
        sc_bp[i][j] = p1 * (values[i] + values[j]);
    }

    vrna_sc_add_up(vc, sc_up, constraint_type);
    vrna_sc_add_bp(vc, (const double**)sc_bp, constraint_type);

    for(i = 1; i <= length; ++i)
      free(sc_bp[i]);
    free(sc_bp);
    free(sc_up);
  } else {
    assert(method == 'W');
    vrna_sc_add_up(vc, values, constraint_type);
  }

  free(values);
  free(sequence);
}

int main(int argc, char *argv[]){
  struct          RNAfold_args_info args_info;
  char            *buf, *rec_sequence, *rec_id, **rec_rest, *structure, *cstruc, *orig_sequence;
  char            *shape_file, *shape_method, *shape_conversion;
  char            fname[FILENAME_MAX_LENGTH], ffname[FILENAME_MAX_LENGTH], *ParamFile;
  char            *ns_bases, *c;
  int             i, length, l, cl, sym, istty, pf, noPS, noconv, do_bpp;
  unsigned int    rec_type, read_opt;
  double          energy, min_en, kT, sfact;
  int             doMEA, circular, lucky, with_shapes, verbose;
  double          MEAgamma, bppmThreshold, betaScale;
  char            *outfile;
  int           out_th, out_db, out_hx, out_ct, out_bps;
  paramT          *mfe_parameters;
  pf_paramT       *pf_parameters;
  model_detailsT  md;

  rec_type      = read_opt = 0;
  rec_id        = buf = rec_sequence = structure = cstruc = orig_sequence = NULL;
  rec_rest      = NULL;
  ParamFile     = NULL;
  ns_bases      = NULL;
  pf_parameters = NULL;
  do_bpp        = do_backtrack  = 1;  /* set local (do_bpp) and global (do_backtrack) default for bpp computation */
  pf            = 0;
  sfact         = 1.07;
  noPS          = 0;
  noconv        = 0;
  circular      = 0;
  gquad         = 0;
  cl            = l = length = 0;
  dangles       = 2;
  MEAgamma      = 1.;
  bppmThreshold = 1e-5;
  lucky         = 0;
  doMEA         = 0;
  betaScale     = 1.;
  shape_file    = NULL;
  shape_method  = NULL;
  with_shapes   = 0;
  verbose       = 0;
  max_bp_span   = -1;

  outfile       = NULL;
  out_th = out_db = 1;  /* default to thermodynamic properties and dot bracket string */
  out_hx = out_ct = out_bps = 0;

  /* apply default model details */
  set_model_details(&md);


  /*
  #############################################
  # check the command line parameters
  #############################################
  */
  if(RNAfold_cmdline_parser (argc, argv, &args_info) != 0) exit(1);
  /* temperature */
  if(args_info.temp_given)        md.temperature = temperature = args_info.temp_arg;
  /* structure constraint */
  if(args_info.constraint_given)  fold_constrained=1;
  /* do not take special tetra loop energies into account */
  if(args_info.noTetra_given)     md.special_hp = tetra_loop=0;
  /* set dangle model */
  if(args_info.dangles_given){
    if((args_info.dangles_arg < 0) || (args_info.dangles_arg > 3))
      warn_user("required dangle model not implemented, falling back to default dangles=2");
    else
      md.dangles = dangles = args_info.dangles_arg;
  }
  /* do not allow weak pairs */
  if(args_info.noLP_given)        md.noLP = noLonelyPairs = 1;
  /* do not allow wobble pairs (GU) */
  if(args_info.noGU_given)        md.noGU = noGU = 1;
  /* do not allow weak closing pairs (AU,GU) */
  if(args_info.noClosingGU_given) md.noGUclosure = no_closingGU = 1;
  /* gquadruplex support */
  if(args_info.gquad_given)       md.gquad = gquad = 1;
  /* enforce canonical base pairs in any case? */
  if(args_info.canonicalBPonly_given)       md.canonicalBPonly = canonicalBPonly = 1;
  /* do not convert DNA nucleotide "T" to appropriate RNA "U" */
  if(args_info.noconv_given)      noconv = 1;
  /* set energy model */
  if(args_info.energyModel_given) md.energy_set = energy_set = args_info.energyModel_arg;
  /* take another energy parameter set */
  if(args_info.paramFile_given)   ParamFile = strdup(args_info.paramFile_arg);
  /* Allow other pairs in addition to the usual AU,GC,and GU pairs */
  if(args_info.nsp_given)         ns_bases = strdup(args_info.nsp_arg);
  /* set pf scaling factor */
  if(args_info.pfScale_given)     md.pf_scale = sfact = args_info.pfScale_arg;
  /* assume RNA sequence to be circular */
  if(args_info.circ_given)        md.circ = circular = 1;
  /* always look on the bright side of life */
  if(args_info.ImFeelingLucky_given)  md.uniq_ML = lucky = pf = st_back = 1;
  /* set the bppm threshold for the dotplot */
  if(args_info.bppmThreshold_given)
    bppmThreshold = MIN2(1., MAX2(0.,args_info.bppmThreshold_arg));
  if(args_info.betaScale_given)   md.betaScale = betaScale = args_info.betaScale_arg;
  /* do not produce postscript output */
  if(args_info.noPS_given)        noPS=1;
  /* partition function settings */
  if(args_info.partfunc_given){
    pf = 1;
    if(args_info.partfunc_arg != 1)
      do_bpp = md.compute_bpp = do_backtrack = args_info.partfunc_arg;
  }
  /* MEA (maximum expected accuracy) settings */
  if(args_info.MEA_given){
    pf = doMEA = 1;
    if(args_info.MEA_arg != -1)
      MEAgamma = args_info.MEA_arg;
  }
  /* SHAPE reactivity data */
  if(args_info.shape_given){
    with_shapes = 1;
    shape_file = strdup(args_info.shape_arg);
  }

  shape_method = strdup(args_info.shapeMethod_arg);
  shape_conversion = strdup(args_info.shapeConversion_arg);
  if(args_info.verbose_given){
    verbose = 1;
  }
  if(args_info.maxBPspan_given){
    md.max_bp_span = max_bp_span = args_info.maxBPspan_arg;
  }
  if(args_info.outfile_given){
    outfile = strdup(args_info.outfile_arg);
  }
  if(args_info.format_given){
    char *o,*s;
    out_th = out_db = out_hx = out_ct = 0; /* reset defaults */
    o = strdup(args_info.format_arg);
    /* print thermodynamic properties? */
    s = strchr(o, 'T');
    if(s != NULL)
      out_th = 1;
    /* print dot-parenthesis string? */
    s = strchr(o, 'P');
    if(s != NULL)
      out_db = 1;
    /* print helix list? */
    s = strchr(o, 'H');
    if(s != NULL)
      out_hx = 1;
    /* print connect file? */
    s = strchr(o, 'C');
    if(s != NULL)
      out_ct = 1;
    /* print bpseq file? */
    s = strchr(o, 'B');
    if(s != NULL)
      out_bps = 1;
  }

  /* free allocated memory of command line data structure */
  RNAfold_cmdline_parser_free (&args_info);


  /*
  #############################################
  # begin initializing
  #############################################
  */
  if(circular && gquad){
    nrerror("G-Quadruplex support is currently not available for circular RNA structures");
  }

  if (ParamFile != NULL)
    read_parameter_file(ParamFile);

  if (circular && noLonelyPairs)
    warn_user("depending on the origin of the circular sequence, some structures may be missed when using -noLP\nTry rotating your sequence a few times");

  if (ns_bases != NULL) {
    /* nonstandards = space(33); */
    c=ns_bases;
    i=sym=0;
    if (*c=='-') {
      sym=1; c++;
    }
    while (*c!='\0') {
      if (*c!=',') {
        md.nonstandards[i++]=*c++;
        md.nonstandards[i++]=*c;
        if ((sym)&&(*c!=*(c-1))) {
          md.nonstandards[i++]=*c;
          md.nonstandards[i++]=*(c-1);
        }
      }
      c++;
    }
  }

  istty = isatty(fileno(stdout))&&isatty(fileno(stdin));

  /* print user help if we get input from tty */
  if(istty){
    if(fold_constrained){
      print_tty_constraint_full();
      print_tty_input_seq_str("Input sequence (upper or lower case) followed by structure constraint");
    }
    else print_tty_input_seq();
  }

  mfe_parameters = get_scaled_parameters(temperature, md);

  /* set options we wanna pass to read_record */
  if(istty)             read_opt |= VRNA_INPUT_NOSKIP_BLANK_LINES;
  if(!fold_constrained) read_opt |= VRNA_INPUT_NO_REST;

  /*
  #############################################
  # main loop: continue until end of file
  #############################################
  */
  while(
    !((rec_type = read_record(&rec_id, &rec_sequence, &rec_rest, read_opt))
        & (VRNA_INPUT_ERROR | VRNA_INPUT_QUIT))){

    /*
    ########################################################
    # init everything according to the data we've read
    ########################################################
    */
    if(rec_id){
      if(!istty && !outfile) printf("%s\n", rec_id);
      (void) sscanf(rec_id, ">%" XSTR(FILENAME_ID_LENGTH) "s", fname);
    }
    else fname[0] = '\0';

    length  = (int)strlen(rec_sequence);
    structure = (char *)space(sizeof(char) *(length+1));

    /* parse the rest of the current dataset to obtain a structure constraint */
    if(fold_constrained){
      cstruc = NULL;
      unsigned int coptions = (rec_id) ? VRNA_CONSTRAINT_MULTILINE : 0;
      coptions |= VRNA_CONSTRAINT_ALL;
      getConstraint(&cstruc, (const char **)rec_rest, coptions);
      cl = (cstruc) ? (int)strlen(cstruc) : 0;

      if(cl == 0)           warn_user("structure constraint is missing");
      else if(cl < length)  warn_user("structure constraint is shorter than sequence");
      else if(cl > length)  nrerror("structure constraint is too long");
      if(cstruc) strncpy(structure, cstruc, sizeof(char)*(cl+1));
    }

    /* convert DNA alphabet to RNA if not explicitely switched off */
    if(!noconv) str_DNA2RNA(rec_sequence);
    /* store case-unmodified sequence */
    orig_sequence = strdup(rec_sequence);
    /* convert sequence to uppercase letters only */
    str_uppercase(rec_sequence);

    if(istty) printf("length = %d\n", length);

    /*
    ########################################################
    # begin actual computations
    ########################################################
    */

    vrna_fold_compound *vc = vrna_get_fold_compound(rec_sequence, &md, VRNA_OPTION_MFE | ((pf) ? VRNA_OPTION_PF : 0));

    if(fold_constrained){
      unsigned int constraint_options = 0;
      constraint_options |= VRNA_CONSTRAINT_DB
                            | VRNA_CONSTRAINT_PIPE
                            | VRNA_CONSTRAINT_DOT
                            | VRNA_CONSTRAINT_X
                            | VRNA_CONSTRAINT_ANG_BRACK
                            | VRNA_CONSTRAINT_RND_BRACK;

      vrna_hc_add(vc, (const char *)structure, constraint_options);
    }

    if(with_shapes)
      add_shape_constraints(vc, shape_method, shape_conversion, shape_file, verbose, VRNA_CONSTRAINT_SOFT_MFE);


    min_en = vrna_fold(vc, structure);

    char *th_file_name  = NULL;
    char *db_file_name  = NULL;
    char *hx_file_name  = NULL;
    char *ct_file_name  = NULL;
    char *bps_file_name = NULL;
    char *prefix        = NULL;

    if(outfile){

      /* prepare the file prefix */
      if(fname[0] != '\0'){
        prefix = (char *)space(sizeof(char) * (strlen(fname) + strlen(outfile) + 1));
        strcpy(prefix, outfile);
        strcat(prefix, "_");
        strcat(prefix, fname);
      } else {
        prefix = (char *)space(sizeof(char) * (strlen(outfile) + 1));
        strcpy(prefix, outfile);
      }
    }

    if(!lucky){
      if(outfile){

        if(out_th){
          th_file_name = (char *)space(sizeof(char) * (strlen(prefix) + 8));
          strcpy(th_file_name, prefix);
          strcat(th_file_name, ".th");

          FILE *fp = fopen((const char *)th_file_name, "a"); 
          if(!fp)
            nrerror("Failed to open file for writing");

          fprintf(fp, "sequence = %s\nmfe = %6.2f kcal/mol\n", orig_sequence, min_en);
          fclose(fp);
        }
        
        if(out_db){
          
        }

        if(out_hx){
          hx_file_name = (char *)space(sizeof(char) * (strlen(prefix) + 8));
          strcpy(hx_file_name, prefix);
          strcat(hx_file_name, ".hx");

          FILE *fp = fopen((const char *)hx_file_name, "a");
          if(!fp)
            nrerror("Failed to open file for writing");

          vrna_structure_print_helix_list((const char *)structure, fp);

          fclose(fp);
        }
        
        if(out_ct){
          ct_file_name = (char *)space(sizeof(char) * (strlen(prefix) + 8));
          strcpy(ct_file_name, prefix);
          strcat(ct_file_name, ".ct");

          FILE *fp = fopen((const char *)ct_file_name, "a");
          if(!fp)
            nrerror("Failed to open file for writing");

          vrna_structure_print_ct((const char *)orig_sequence,
                                  (const char *)structure,
                                  min_en,
                                  (const char *)(fname[0] != '\0' ? fname : "MFE-structure"),
                                  fp);

          fclose(fp);
        }

        if(out_bps){
          bps_file_name = (char *)space(sizeof(char) * (strlen(prefix) + 11));
          strcpy(bps_file_name, prefix);
          strcat(bps_file_name, ".bpseq");

          FILE *fp = fopen((const char *)bps_file_name, "a");
          if(!fp)
            nrerror("Failed to open file for writing");

          vrna_structure_print_bpseq( (const char *)orig_sequence,
                                      (const char *)structure,
                                      fp);

          fclose(fp);
        }
      } else {
        printf("%s\n%s", orig_sequence, structure);
        if (istty){
          printf("\n minimum free energy = %6.2f kcal/mol\n", min_en);
        } else {
          printf(" (%6.2f)\n", min_en);
        }
      }
      (void) fflush(stdout);

      if(fname[0] != '\0'){
        strcpy(ffname, fname);
        strcat(ffname, "_ss.ps");
      } else strcpy(ffname, "rna.ps");

      if(gquad){
        if (!noPS) (void) PS_rna_plot_a_gquad(orig_sequence, structure, ffname, NULL, NULL);
      } else {
        if (!noPS) (void) PS_rna_plot_a(orig_sequence, structure, ffname, NULL, NULL);
      }
    }

    if (length>2000)
      vrna_free_mfe_matrices(vc);

    if (pf) {
      char *pf_struc = (char *) space((unsigned) length+1);
      if (mfe_parameters->model_details.dangles==1) {
          mfe_parameters->model_details.dangles=2;   /* recompute with dangles as in pf_fold() */
          min_en = vrna_eval_structure(rec_sequence, structure, mfe_parameters);
          mfe_parameters->model_details.dangles=1;
      }


      kT = (betaScale*((temperature+K0)*GASCONST))/1000.; /* in Kcal */
      pf_scale = exp(-(sfact*min_en)/kT/length);
      if (length>2000) fprintf(stderr, "scaling factor %f\n", pf_scale);

      if (cstruc!=NULL) strncpy(pf_struc, cstruc, length+1);

      /* rescale exp_params according to mfe computed above */
      pf_parameters = get_boltzmann_factors(temperature, betaScale, md, pf_scale);
      vrna_update_pf_params(vc,pf_parameters);

      if(with_shapes)
        add_shape_constraints(vc, shape_method, shape_conversion, shape_file, verbose, VRNA_CONSTRAINT_SOFT_PF);

      energy = vrna_pf_fold(vc, pf_struc);

      /* in case we abort because of floating point errors */
      if (length>1600)
        fprintf(stderr, "free energy = %8.2f\n", energy);

      if(lucky){
        init_rand();
        char *s = vrna_pbacktrack(vc);
        min_en = vrna_eval_structure((const char *)rec_sequence, (const char *)s, mfe_parameters);
        printf("%s\n%s", orig_sequence, s);
        if (istty)
          printf("\n free energy = %6.2f kcal/mol\n", min_en);
        else
          printf(" (%6.2f)\n", min_en);
        (void) fflush(stdout);
        if(fname[0] != '\0'){
          strcpy(ffname, fname);
          strcat(ffname, "_ss.ps");
        } else strcpy(ffname, "rna.ps");

        if (!noPS) (void) PS_rna_plot(orig_sequence, s, ffname);
        free(s);
      }
      else{
      
        if (do_bpp) {
          printf("%s", pf_struc);
          if (!istty) printf(" [%6.2f]\n", energy);
          else printf("\n");
        }
        if ((istty)||(!do_bpp))
          printf(" free energy of ensemble = %6.2f kcal/mol\n", energy);


        if (do_bpp) {
          plist *pl1,*pl2;
          char *cent;
          double dist, cent_en;

          pl1     = vrna_get_plist_from_pr(vc, bppmThreshold);
          pl2     = vrna_get_plist_from_db(structure, 0.95*0.95);
          cent    = vrna_get_centroid_struct(vc, &dist);
          cent_en = vrna_eval_structure((const char *)rec_sequence, (const char *)cent, mfe_parameters);
          printf("%s {%6.2f d=%.2f}\n", cent, cent_en, dist);
          free(cent);
          if (fname[0]!='\0') {
            strcpy(ffname, fname);
            strcat(ffname, "_dp.ps");
          } else strcpy(ffname, "dot.ps");
          (void) PS_dot_plot_list(orig_sequence, ffname, pl1, pl2, "");
          free(pl2);
          if (do_bpp==2) {
            pl2 = stackProb(1e-5);
            if (fname[0]!='\0') {
              strcpy(ffname, fname);
              strcat(ffname, "_dp2.ps");
            } else strcpy(ffname, "dot2.ps");
            PS_dot_plot_list(orig_sequence, ffname, pl1, pl2,
                             "Probabilities for stacked pairs (i,j)(i+1,j-1)");
            free(pl2);
          }
          free(pl1);
          free(pf_struc);
          if(doMEA){
            float mea, mea_en;
            plist *pl = vrna_get_plist_from_pr(vc, 1e-4/(1+MEAgamma));

            if(gquad){
              mea = MEA_seq(pl, rec_sequence, structure, MEAgamma, pf_parameters);
            } else {
              mea = MEA(pl, structure, MEAgamma);
            }
            mea_en = vrna_eval_structure((const char *)rec_sequence, (const char *)structure, mfe_parameters);
            printf("%s {%6.2f MEA=%.2f}\n", structure, mea_en, mea);

            free(pl);
          }
        }
        printf(" frequency of mfe structure in ensemble %g; ", exp((energy-min_en)/kT));
        if (do_bpp)
          printf("ensemble diversity %-6.2f", vrna_mean_bp_distance(vc));
        printf("\n");
      }
//      free_pf_arrays();
      free(pf_parameters);
    }
    (void) fflush(stdout);

    /* clean up */
    vrna_free_fold_compound(vc);
    if(cstruc) free(cstruc);
    if(rec_id) free(rec_id);
    free(rec_sequence);
    free(orig_sequence);
    free(structure);
    /* free the rest of current dataset */
    if(rec_rest){
      for(i=0;rec_rest[i];i++) free(rec_rest[i]);
      free(rec_rest);
    }
    rec_id = rec_sequence = structure = cstruc = NULL;
    rec_rest = NULL;

    if(with_shapes)
      break;

    /* print user help for the next round if we get input from tty */
    if(istty){
      if(fold_constrained){
        print_tty_constraint_full();
        print_tty_input_seq_str("Input sequence (upper or lower case) followed by structure constraint");
      }
      else print_tty_input_seq();
    }
  }
  
  free(mfe_parameters);
  return EXIT_SUCCESS;
}
