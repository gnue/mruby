#include "mruby.h"
#include "mruby/proc.h"
#include "mruby/array.h"
#include "mruby/string.h"
#include "mruby/compile.h"
#include "mruby/dump.h"
#include "mruby/variable.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifndef ENABLE_STDIO
static void
p(mrb_state *mrb, mrb_value obj)
{
  obj = mrb_funcall(mrb, obj, "inspect", 0);
  fwrite(RSTRING_PTR(obj), RSTRING_LEN(obj), 1, stdout);
  putc('\n', stdout);
}
#else
#define p(mrb,obj) mrb_p(mrb,obj)
#endif

#define RITEBIN_EXT ".mrb"
#define C_EXT       ".c"
void mrb_show_version(mrb_state *);
void mrb_show_copyright(mrb_state *);
void parser_dump(mrb_state*, struct mrb_ast_node*, int);
void codedump_all(mrb_state*, int);
void mrb_init_libs(mrb_state*);
mrb_state* mrb_open0();
int mirb(mrb_state *);

typedef enum {
  kErrNoFunctionName   = -2,
  kErrUnkownLongOption = -3,
  kErrUnkownOption     = -4
} args_err_t;

struct _args {
  FILE *rfp;
  FILE *wfp;
  char *filename;
  char *initname;
  char *ext;
  char* cmdline;
  int fname        : 1;
  int mrbfile      : 1;
  int check_syntax : 1;
  int verbose      : 1;
  int argc;
  char** argv;
};

static void
usage(const char *name)
{
  static const char *const usage_msg[] = {
  "switches:",
  "-b           load and execute RiteBinary (mrb) file",
  "-c           check syntax only",
  "-e 'command' one line of script",
  "-O           compile",
  "-o<outfile>  place the output into <outfile>",
  "-B<symbol>   binary <symbol> output in C language format",
  "-v           print version number, then run in verbose mode",
  "--verbose    run in verbose mode",
  "--version    print the version",
  "--copyright  print the copyright",
  NULL
  };
  const char *const *p = usage_msg;

  printf("Usage: %s [switches] programfile\n", name);
  while(*p)
  printf("  %s\n", *p++);
}

static char *
get_outfilename(char *infile, char *ext)
{
  char *outfile;
  char *p;

  outfile = (char*)malloc(strlen(infile) + strlen(ext) + 1);
  strcpy(outfile, infile);
  if (*ext) {
    if ((p = strrchr(outfile, '.')) == NULL)
      p = &outfile[strlen(outfile)];
    strcpy(p, ext);
  }

  return outfile;
}

static int
parse_args(mrb_state *mrb, int argc, char **argv, struct _args *args)
{
  bool output = false;
  char *infile = NULL;
  char *outfile = NULL;
  char **origargv = argv;
  int result = 0;
  static const struct _args args_zero = { 0 };

  *args = args_zero;
  args->ext = RITEBIN_EXT;

  for (argc--,argv++; argc > 0; argc--,argv++) {
    char *item;
    if (argv[0][0] != '-') break;

    if (strlen(*argv) <= 1) {
      argc--; argv++;
      args->filename = infile = "-";
      args->rfp = stdin;
      break;
    }

    item = argv[0] + 1;
    switch (*item++) {
    case 'b':
      args->mrbfile = 1;
      break;
    case 'c':
      args->check_syntax = 1;
      break;
    case 'e':
      args->filename = "-e";
      if (item[0]) {
        goto append_cmdline;
      }
      else if (argc > 1) {
        argc--; argv++;
        item = argv[0];
append_cmdline:
        if (!args->cmdline) {
          char *buf;

          buf = (char *)mrb_malloc(mrb, strlen(item)+1);
          strcpy(buf, item);
          args->cmdline = buf;
        }
        else {
          args->cmdline = (char *)mrb_realloc(mrb, args->cmdline, strlen(args->cmdline)+strlen(item)+2);
          strcat(args->cmdline, "\n");
          strcat(args->cmdline, item);
        }
      }
      else {
        printf("%s: No code specified for -e\n", *origargv);
        goto exit;
      }
      break;
    case 'v':
      mrb_show_version(mrb);
      args->verbose = 1;
      break;
    // compile options
    case 'O':
      output = true;
      break;
    case 'o':
      outfile = get_outfilename((*argv) + 2, "");
      break;
    case 'B':
      args->ext = C_EXT;
      args->initname = (*argv) + 2;
      if (*args->initname == '\0') {
        printf("%s: Function name is not specified.\n", *origargv);
        result = kErrNoFunctionName;
        goto exit;
      }
      break;
    // long options
    case '-':
      if (strcmp((*argv) + 2, "version") == 0) {
        mrb_show_version(mrb);
        exit(0);
      }
      else if (strcmp((*argv) + 2, "verbose") == 0) {
        args->verbose = 1;
        break;
      }
      else if (strcmp((*argv) + 2, "copyright") == 0) {
        mrb_show_copyright(mrb);
        exit(0);
      }
      else {
        result = kErrUnkownLongOption;
        goto exit;
      }
      goto exit;
    default:
      result = kErrUnkownOption;
      goto exit;
    }
  }

  if (args->rfp == NULL && args->cmdline == NULL) {
    if (*argv == NULL) {
      args->filename = infile = "-";
      args->rfp = stdin;
    }
    else {
      args->filename = infile = *argv;
      args->rfp = fopen(argv[0], args->mrbfile ? "rb" : "r");
      if (args->rfp == NULL) {
        printf("%s: Cannot open program file. (%s)\n", *origargv, *argv);
        goto exit;
      }
      args->fname = 1;
      argc--; argv++;
    }
  }
  args->argv = (char **)mrb_realloc(mrb, args->argv, sizeof(char*) * (argc + 1));
  memcpy(args->argv, argv, (argc+1) * sizeof(char*));
  args->argc = argc;

  if (!args->check_syntax) {
    if (output && outfile == NULL) {
      if (infile == NULL || args->cmdline) {
        if (output) outfile = infile = "-";
      } else if (strcmp("-", infile) == 0) {
        outfile = infile;
      }
      else {
        outfile = get_outfilename(infile, args->ext);
      }
    }
    if (outfile == NULL) goto exit;
    if (strcmp("-", outfile) == 0) {
      args->wfp = stdout;
    }
    else if ((args->wfp = fopen(outfile, "wb")) == NULL) {
      printf("%s: Cannot open output file. (%s)\n", *origargv, outfile);
      result = -1;
      goto exit;
    }
  }
 exit:
  if (outfile && infile != outfile) free(outfile);
  return 0;
}

static void
cleanup(mrb_state *mrb, struct _args *args)
{
  if (args->rfp && args->rfp != stdin)
    fclose(args->rfp);
  if (args->wfp)
    fclose(args->wfp);
  if (args->cmdline && !args->fname)
    mrb_free(mrb, args->cmdline);
  if (args->argv)
    mrb_free(mrb, args->argv);
  mrb_close(mrb);
}

static void
showcallinfo(mrb_state *mrb)
{
  mrb_callinfo *ci;
  mrb_int ciidx;
  const char *filename, *method, *sep;
  int i, line;

  printf("trace:\n");
  ciidx = mrb_fixnum(mrb_obj_iv_get(mrb, mrb->exc, mrb_intern(mrb, "ciidx")));
  if (ciidx >= mrb->ciend - mrb->cibase)
    ciidx = 10; /* ciidx is broken... */

  for (i = ciidx; i >= 0; i--) {
    ci = &mrb->cibase[i];
    filename = "(unknown)";
    line = -1;

    if (MRB_PROC_CFUNC_P(ci->proc)) {
      continue;
    }
    else {
      mrb_irep *irep = ci->proc->body.irep;
      if (irep->filename != NULL)
        filename = irep->filename;
      if (irep->lines != NULL) {
	mrb_code *pc;

	if (i+1 <= ciidx) {
	  pc = mrb->cibase[i+1].pc;
	}
	else {
	  pc = (mrb_code*)mrb_voidp(mrb_obj_iv_get(mrb, mrb->exc, mrb_intern(mrb, "lastpc")));
	}
        if (irep->iseq <= pc && pc < irep->iseq + irep->ilen) {
          line = irep->lines[pc - irep->iseq - 1];
        }
      }
    }
    if (line == -1) continue;
    if (ci->target_class == ci->proc->target_class)
      sep = ".";
    else
      sep = "#";

    method = mrb_sym2name(mrb, ci->mid);
    if (method) {
      const char *cn = mrb_class_name(mrb, ci->proc->target_class);
    
      if (cn) {
	printf("\t[%d] %s:%d:in %s%s%s\n",
	       i, filename, line, cn, sep, method);
      }
      else {
	printf("\t[%d] %s:%d:in %s\n",
	       i, filename, line, method);
      }
    }
    else {
      printf("\t[%d] %s:%d\n",
	     i, filename, line);
    }
  }
}

int
main(int argc, char **argv)
{
  mrb_state *mrb = mrb_open0();
  int n = -1;
  int i;
  struct _args args;
  mrb_value ARGV;

  if (mrb == NULL) {
    fprintf(stderr, "Invalid mrb_state, exiting mruby\n");
    return EXIT_FAILURE;
  }

  n = parse_args(mrb, argc, argv, &args);
  if (n < 0 || (args.cmdline == NULL && args.rfp == NULL)) {
    cleanup(mrb, &args);
    usage(argv[0]);
    return n;
  }

  if (args.wfp == NULL)
     mrb_init_libs(mrb);

  if (args.wfp == NULL && args.rfp == stdin) {
    struct stat st;

    fstat(fileno(stdin), &st);
    if (! S_ISFIFO(st.st_mode) && st.st_size == 0) {
      n = mirb(mrb);
      cleanup(mrb, &args);
      return n;
    }
  }

  ARGV = mrb_ary_new_capa(mrb, args.argc);
  for (i = 0; i < args.argc; i++) {
    mrb_ary_push(mrb, ARGV, mrb_str_new(mrb, args.argv[i], strlen(args.argv[i])));
  }
  mrb_define_global_const(mrb, "ARGV", ARGV);

  if (args.mrbfile) {
    n = mrb_read_irep_file(mrb, args.rfp);
    if (n < 0) {
      fprintf(stderr, "failed to load mrb file: %s\n", args.cmdline);
    }
    else if (!args.check_syntax) {
      mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[n]), mrb_top_self(mrb));
      n = 0;
      if (mrb->exc) {
        showcallinfo(mrb);
        p(mrb, mrb_obj_value(mrb->exc));
        n = -1;
      }
    }
  }
  else {
    mrbc_context *c = mrbc_context_new(mrb);
    mrb_value v;

    if (args.verbose)
      c->dump_result = 1;
    if (args.check_syntax)
      c->no_exec = 1;
    if (args.wfp)
      c->no_exec = 1;

    mrbc_filename(mrb, c, args.filename);
    if (args.rfp) {
      v = mrb_load_file_cxt(mrb, args.rfp, c);
    }
    else {
      v = mrb_load_string_cxt(mrb, args.cmdline, c);
    }
    if (args.wfp && ! args.check_syntax) {
      if (mrb_undef_p(v) || mrb_fixnum(v) < 0) {
        cleanup(mrb, &args);
        return EXIT_FAILURE;
      }
      if (args.initname) {
        n = mrb_bdump_irep(mrb, n, args.wfp, args.initname);
      }
      else {
        n = mrb_dump_irep(mrb, n, args.wfp);
      }
    }
    mrbc_context_free(mrb, c);
    if (mrb->exc) {
      if (!mrb_undef_p(v)) {
        showcallinfo(mrb);
        p(mrb, mrb_obj_value(mrb->exc));
      }
      n = -1;
    }
    else if (args.check_syntax) {
      printf("Syntax OK\n");
    }
  }
  cleanup(mrb, &args);

  return n == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
