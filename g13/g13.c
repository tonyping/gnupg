/* g13.c - Disk Key management with GnuPG
 * Copyright (C) 2009 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#include "g13.h"

#include <gcrypt.h>

#include "i18n.h"
#include "sysutils.h"
#include "gc-opt-flags.h"


enum cmd_and_opt_values {
  aNull = 0,
  oQuiet	= 'q',
  oVerbose	= 'v',

  aGPGConfList  = 500,
  aGPGConfTest,
  aCreate,
  aMount,
  aUmount,

  oOptions,
  oDebug,
  oDebugLevel,
  oDebugAll,
  oDebugNone,
  oDebugWait,
  oDebugAllowCoreDump,
  oLogFile,
  oNoLogFile,
  oAuditLog,

  oOutput,

  oAgentProgram,
  oDisplay,
  oTTYname,
  oTTYtype,
  oLCctype,
  oLCmessages,
  oXauthority,

  oStatusFD,
  oLoggerFD,

  oNoVerbose,
  oNoSecmemWarn,
  oNoGreeting,
  oNoTTY,
  oNoOptions,
  oHomedir,
  oWithColons,
  oDryRun,

  oRecipient,

  oNoRandomSeedFile,
  oFakedSystemTime
 };


static ARGPARSE_OPTS opts[] = {

  ARGPARSE_group (300, N_("@Commands:\n ")),

  ARGPARSE_c (aCreate, "create", N_("Create a new file system container")),
  ARGPARSE_c (aMount,  "mount",  N_("Mount a file system container") ),
  ARGPARSE_c (aUmount, "umount", N_("Unmount a file system container") ),

  ARGPARSE_c (aGPGConfList, "gpgconf-list", "@"),
  ARGPARSE_c (aGPGConfTest, "gpgconf-test", "@"),

  ARGPARSE_group (301, N_("@\nOptions:\n ")),

  ARGPARSE_s_s (oRecipient, "recipient", N_("|USER-ID|encrypt for USER-ID")),

  ARGPARSE_s_s (oOutput, "output", N_("|FILE|write output to FILE")),
  ARGPARSE_s_n (oVerbose, "verbose", N_("verbose")),
  ARGPARSE_s_n (oQuiet,	"quiet",  N_("be somewhat more quiet")),
  ARGPARSE_s_n (oNoTTY, "no-tty", N_("don't use the terminal at all")),
  ARGPARSE_s_s (oLogFile, "log-file",  N_("|FILE|write log output to FILE")),
  ARGPARSE_s_n (oNoLogFile, "no-log-file", "@"),
  ARGPARSE_s_i (oLoggerFD, "logger-fd", "@"),

  ARGPARSE_s_s (oAuditLog, "audit-log",
                N_("|FILE|write an audit log to FILE")),
  ARGPARSE_s_n (oDryRun, "dry-run", N_("do not make any changes")),

  ARGPARSE_s_s (oOptions, "options", N_("|FILE|read options from FILE")),

  ARGPARSE_p_u (oDebug, "debug", "@"),
  ARGPARSE_s_s (oDebugLevel, "debug-level",
                N_("|LEVEL|set the debugging level to LEVEL")),
  ARGPARSE_s_n (oDebugAll, "debug-all", "@"),
  ARGPARSE_s_n (oDebugNone, "debug-none", "@"),
  ARGPARSE_s_i (oDebugWait, "debug-wait", "@"),
  ARGPARSE_s_n (oDebugAllowCoreDump, "debug-allow-core-dump", "@"),

  ARGPARSE_s_i (oStatusFD, "status-fd",
                N_("|FD|write status info to this FD")),

  ARGPARSE_group (302, N_(
  "@\n(See the man page for a complete listing of all commands and options)\n"
  )),

  ARGPARSE_group (303, N_("@\nExamples:\n\n"
    " blurb\n"
                          " blurb\n")),

  /* Hidden options. */
  ARGPARSE_s_n (oNoVerbose, "no-verbose", "@"),
  ARGPARSE_s_n (oNoSecmemWarn, "no-secmem-warning", "@"), 
  ARGPARSE_s_n (oNoGreeting, "no-greeting", "@"),
  ARGPARSE_s_n (oNoOptions, "no-options", "@"),
  ARGPARSE_s_s (oHomedir, "homedir", "@"),   
  ARGPARSE_s_s (oAgentProgram, "agent-program", "@"),
  ARGPARSE_s_s (oDisplay,    "display", "@"),
  ARGPARSE_s_s (oTTYname,    "ttyname", "@"),
  ARGPARSE_s_s (oTTYtype,    "ttytype", "@"),
  ARGPARSE_s_s (oLCctype,    "lc-ctype", "@"),
  ARGPARSE_s_s (oLCmessages, "lc-messages", "@"),
  ARGPARSE_s_s (oXauthority, "xauthority", "@"),
  ARGPARSE_s_s (oFakedSystemTime, "faked-system-time", "@"),
  ARGPARSE_s_n (oWithColons, "with-colons", "@"),
  ARGPARSE_s_n (oNoRandomSeedFile,  "no-random-seed-file", "@"),

  /* Command aliases.  */

  ARGPARSE_end ()
};


/* Global variable to keep an error count. */
int g13_errors_seen = 0;

/* It is possible that we are currently running under setuid permissions.  */
static int maybe_setuid = 1;

/* Helper to implement --debug-level and --debug.  */
static const char *debug_level;
static unsigned int debug_value;

static void set_cmd (enum cmd_and_opt_values *ret_cmd,
                     enum cmd_and_opt_values new_cmd );

static void emergency_cleanup (void);


static const char *
my_strusage( int level )
{
  const char *p;

  switch (level)
    {
    case 11: p = "g13 (GnuPG)";
      break;
    case 13: p = VERSION; break;
    case 17: p = PRINTABLE_OS_NAME; break;
    case 19: p = _("Please report bugs to <" PACKAGE_BUGREPORT ">.\n");
      break;
    case 1:
    case 40: p = _("Usage: g13 [options] [files] (-h for help)");
      break;
    case 41:
      p = _("Syntax: g13 [options] [files]\n"
            "Create, mount or unmount an encrypted file system container\n");
      break;

    case 31: p = "\nHome: "; break;
    case 32: p = opt.homedir; break;
     
    default: p = NULL; break;
    }
  return p;
}


static void
wrong_args (const char *text)
{
  fputs (_("usage: g13 [options] "), stderr);
  fputs (text, stderr);
  putc ('\n', stderr);
  g13_exit (2);
}


/* Setup the debugging.  With a DEBUG_LEVEL of NULL only the active
   debug flags are propagated to the subsystems.  With DEBUG_LEVEL
   set, a specific set of debug flags is set; and individual debugging
   flags will be added on top.  */
static void
set_debug (void)
{
  if (!debug_level)
    ;
  else if (!strcmp (debug_level, "none"))
    opt.debug = 0;
  else if (!strcmp (debug_level, "basic"))
    opt.debug = DBG_ASSUAN_VALUE|DBG_MOUNT_VALUE;
  else if (!strcmp (debug_level, "advanced"))
    opt.debug = DBG_ASSUAN_VALUE|DBG_MOUNT_VALUE;
  else if (!strcmp (debug_level, "expert"))
    opt.debug = (DBG_ASSUAN_VALUE|DBG_MOUNT_VALUE|DBG_CRYPTO_VALUE);
  else if (!strcmp (debug_level, "guru"))
    opt.debug = ~0;
  else
    {
      log_error (_("invalid debug-level `%s' given\n"), debug_level);
      g13_exit(2);
    }

  opt.debug |= debug_value;

  if (opt.debug && !opt.verbose)
    opt.verbose = 1;
  if (opt.debug)
    opt.quiet = 0;

  if (opt.debug & DBG_CRYPTO_VALUE )
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 1);
  gcry_control (GCRYCTL_SET_VERBOSITY, (int)opt.verbose);
}
 


static void
set_cmd (enum cmd_and_opt_values *ret_cmd, enum cmd_and_opt_values new_cmd)
{
  enum cmd_and_opt_values cmd = *ret_cmd;

  if (!cmd || cmd == new_cmd)
    cmd = new_cmd;
  else 
    {
      log_error (_("conflicting commands\n"));
      g13_exit (2);
    }

  *ret_cmd = cmd;
}


/* Helper to add recipients to a list. */
static int
add_encryption_key (ctrl_t ctrl, const char *name,
                    void /*FIXME*/ *keylist, int is_cms)
{
  /* FIXME: Decide whether to add a CMS or OpenPGP key and then add
     the key to a list.  */
  /* int rc = foo_add_to_certlist (ctrl, name, 0, recplist, is_encrypt_to); */
  /* if (rc) */
  /*   { */
  /*     if (recp_required) */
  /*       { */
  /*         log_error ("can't encrypt to `%s': %s\n", name, gpg_strerror (rc)); */
  /*         gpgsm_status2 (ctrl, STATUS_INV_RECP, */
  /*                        get_inv_recpsgnr_code (rc), name, NULL); */
  /*       } */
  /*     else */
  /*       log_info (_("NOTE: won't be able to encrypt to `%s': %s\n"), */
  /*                 name, gpg_strerror (rc)); */
  /*   } */
  return 0; /* Key is good.  */
}


int
main ( int argc, char **argv)
{
  ARGPARSE_ARGS pargs;
  int orig_argc;
  char **orig_argv;
  const char *fname;
  int may_coredump;
  FILE *configfp = NULL;
  char *configname = NULL;
  unsigned configlineno;
  int parse_debug = 0;
  int no_more_options = 0;
  int default_config =1;
  char *logfile = NULL;
  char *auditlog = NULL;
  int greeting = 0;
  int nogreeting = 0;
  int debug_wait = 0;
  int use_random_seed = 1;
  int nokeysetup = 0;
  enum cmd_and_opt_values cmd = 0;
  struct server_control_s ctrl;
  estream_t auditfp = NULL;
  strlist_t recipients = NULL;

  /*mtrace();*/

  gnupg_reopen_std ("g13");
  set_strusage (my_strusage);
  gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
  gcry_control (GCRYCTL_DISABLE_INTERNAL_LOCKING);

  log_set_prefix ("g13", 1);

  /* Make sure that our subsystems are ready.  */
  i18n_init();
  init_common_subsystems ();

  /* Check that the Libgcrypt is suitable.  */
  if (!gcry_check_version (NEED_LIBGCRYPT_VERSION) )
    log_fatal (_("%s is too old (need %s, have %s)\n"), "libgcrypt", 
               NEED_LIBGCRYPT_VERSION, gcry_check_version (NULL) );

  /* Take extra care of the random pool.  */
  gcry_control (GCRYCTL_USE_SECURE_RNDPOOL);

  may_coredump = disable_core_dumps ();
  
  gnupg_init_signals (0, emergency_cleanup);
  
  create_dotlock (NULL); /* Register locking cleanup.  */

  opt.homedir = default_homedir ();

  /* First check whether we have a config file on the commandline.  */
  orig_argc = argc;
  orig_argv = argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags= 1|(1<<6);  /* Do not remove the args, ignore version.  */
  while (arg_parse( &pargs, opts))
    {
      if (pargs.r_opt == oDebug || pargs.r_opt == oDebugAll)
        parse_debug++;
      else if (pargs.r_opt == oOptions)
        { /* Yes, there is one, so we do not try the default one but
             read the config file when it is encountered at the
             commandline.  */
          default_config = 0;
	}
      else if (pargs.r_opt == oNoOptions)
        default_config = 0; /* --no-options */
      else if (pargs.r_opt == oHomedir)
        opt.homedir = pargs.r.ret_str;
    }

  /* Initialize the secure memory. */
  gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
  maybe_setuid = 0;

  /* 
     Now we are now working under our real uid 
  */


  /* Setup a default control structure for command line mode.  */
  memset (&ctrl, 0, sizeof ctrl);
  g13_init_default_ctrl (&ctrl);

  /* Set the default option file */
  if (default_config )
    configname = make_filename (opt.homedir, "g13.conf", NULL);
  
  argc        = orig_argc;
  argv        = orig_argv;
  pargs.argc  = &argc;
  pargs.argv  = &argv;
  pargs.flags =  1;  /* Do not remove the args.  */

 next_pass:
  if (configname) {
    configlineno = 0;
    configfp = fopen (configname, "r");
    if (!configfp)
      {
        if (default_config)
          {
            if (parse_debug)
              log_info (_("NOTE: no default option file `%s'\n"), configname);
          }
        else 
          {
            log_error (_("option file `%s': %s\n"), configname, strerror(errno));
            g13_exit(2);
          }
        xfree (configname);
        configname = NULL;
      }
    if (parse_debug && configname)
      log_info (_("reading options from `%s'\n"), configname);
    default_config = 0;
  }

  while (!no_more_options 
         && optfile_parse (configfp, configname, &configlineno, &pargs, opts))
    {
      switch (pargs.r_opt)
        {
	case aGPGConfList: 
	case aGPGConfTest: 
          set_cmd (&cmd, pargs.r_opt);
          nogreeting = 1;
          nokeysetup = 1;
          break;

        case aMount:
        case aUmount:
          nokeysetup = 1;
        case aCreate:
          set_cmd (&cmd, pargs.r_opt);
          break;

        case oOutput: opt.outfile = pargs.r.ret_str; break;

        case oQuiet: opt.quiet = 1; break;
        case oNoGreeting: nogreeting = 1; break;
        case oNoTTY:  break;

        case oDryRun: opt.dry_run = 1; break;

        case oVerbose:
          opt.verbose++;
          gcry_control (GCRYCTL_SET_VERBOSITY, (int)opt.verbose);
          break;
        case oNoVerbose:
          opt.verbose = 0;
          gcry_control (GCRYCTL_SET_VERBOSITY, (int)opt.verbose);
          break;

        case oLogFile: logfile = pargs.r.ret_str; break;
        case oNoLogFile: logfile = NULL; break;          

        case oAuditLog: auditlog = pargs.r.ret_str; break;

        case oDebug: debug_value |= pargs.r.ret_ulong; break;
        case oDebugAll: debug_value = ~0; break;
        case oDebugNone: debug_value = 0; break;
        case oDebugLevel: debug_level = pargs.r.ret_str; break;
        case oDebugWait: debug_wait = pargs.r.ret_int; break;
        case oDebugAllowCoreDump:
          may_coredump = enable_core_dumps ();
          break;

        case oStatusFD: ctrl.status_fd = pargs.r.ret_int; break;
        case oLoggerFD: log_set_fd (pargs.r.ret_int ); break;

        case oNoOptions: break; /* no-options */
        case oOptions:
          /* Config files may not be nested (silently ignore them).  */
          if (!configfp)
            {
              xfree(configname);
              configname = xstrdup (pargs.r.ret_str);
              goto next_pass;
	    }
          break;

        case oHomedir: opt.homedir = pargs.r.ret_str; break;

        case oAgentProgram: opt.agent_program = pargs.r.ret_str;  break;
        case oDisplay: opt.display = xstrdup (pargs.r.ret_str); break;
        case oTTYname: opt.ttyname = xstrdup (pargs.r.ret_str); break;
        case oTTYtype: opt.ttytype = xstrdup (pargs.r.ret_str); break;
        case oLCctype: opt.lc_ctype = xstrdup (pargs.r.ret_str); break;
        case oLCmessages: opt.lc_messages = xstrdup (pargs.r.ret_str); break;
        case oXauthority: opt.xauthority = xstrdup (pargs.r.ret_str); break;
          
        case oFakedSystemTime:
          {
            time_t faked_time = isotime2epoch (pargs.r.ret_str); 
            if (faked_time == (time_t)(-1))
              faked_time = (time_t)strtoul (pargs.r.ret_str, NULL, 10);
            gnupg_set_time (faked_time, 0);
          }
          break;

        case oNoSecmemWarn: gcry_control (GCRYCTL_DISABLE_SECMEM_WARN); break;

        case oNoRandomSeedFile: use_random_seed = 0; break;

        case oRecipient: /* Store the encryption key.  */
          add_to_strlist (&recipients, pargs.r.ret_str);
          break;


        default: 
          pargs.err = configfp? ARGPARSE_PRINT_WARNING:ARGPARSE_PRINT_ERROR; 
          break;
	}
    }

  if (configfp)
    {
      fclose (configfp);
      configfp = NULL;
      /* Keep a copy of the config filename. */
      opt.config_filename = configname;
      configname = NULL;
      goto next_pass;
    }
  xfree (configname);
  configname = NULL;

  if (!opt.config_filename)
    opt.config_filename = make_filename (opt.homedir, "g13.conf", NULL);

  if (log_get_errorcount(0))
    g13_exit(2);

  /* Now that we have the options parsed we need to update the default
     control structure.  */
  g13_init_default_ctrl (&ctrl);

  if (nogreeting)
    greeting = 0;
  
  if (greeting)
    {
      fprintf(stderr, "%s %s; %s\n",
              strusage(11), strusage(13), strusage(14) );
      fprintf(stderr, "%s\n", strusage(15) );
    }

  if (may_coredump && !opt.quiet)
    log_info (_("WARNING: program may create a core file!\n"));

  if (logfile)
    {
      log_set_file (logfile);
      log_set_prefix (NULL, 1|2|4);
    }

  if (gnupg_faked_time_p ())
    {
      gnupg_isotime_t tbuf;

      log_info (_("WARNING: running with faked system time: "));
      gnupg_get_isotime (tbuf);
      dump_isotime (tbuf);
      log_printf ("\n");
    }

  /* Print any pending secure memory warnings.  */
  gcry_control (GCRYCTL_RESUME_SECMEM_WARN);

  /* Setup the debug flags for all subsystems.  */
  set_debug ();

  /* Install a regular exit handler to make real sure that the secure
     memory gets wiped out.  */
  if (atexit (emergency_cleanup))
    {
      log_error ("atexit failed\n");
      g13_exit (2);
    }

  /* Terminate if we found any error until now.  */
  if (log_get_errorcount(0))
    g13_exit (2);
  
  /* Set the standard GnuPG random seed file.  */
  if (use_random_seed) 
    {
      char *p = make_filename (opt.homedir, "random_seed", NULL);
      gcry_control (GCRYCTL_SET_RANDOM_SEED_FILE, p);
      xfree(p);
    }
  
  /* Store given filename into FNAME. */
  fname = argc? *argv : NULL;

  /* Parse all given encryption keys.  This does a lookup of the keys
     and stops if any of the given keys was not found. */
  if (!nokeysetup)
    {
      strlist_t sl;
      int failed = 0;
      
      for (sl = recipients; sl; sl = sl->next)
        if (add_encryption_key (&ctrl, sl->d, NULL /* FIXME*/, 0))
          failed = 1;
      if (failed)
        g13_exit (1);
    }
  
  /* Dispatch command.  */
  switch (cmd)
    {
    case aGPGConfList: 
      { /* List options and default values in the GPG Conf format.  */
	char *config_filename_esc = percent_escape (opt.config_filename, NULL);

        printf ("gpgconf-g13.conf:%lu:\"%s\n",
                GC_OPT_FLAG_DEFAULT, config_filename_esc);
        xfree (config_filename_esc);

        printf ("verbose:%lu:\n", GC_OPT_FLAG_NONE);
	printf ("quiet:%lu:\n", GC_OPT_FLAG_NONE);
	printf ("debug-level:%lu:\"none:\n", GC_OPT_FLAG_DEFAULT);
	printf ("log-file:%lu:\n", GC_OPT_FLAG_NONE);
      }
      break;
    case aGPGConfTest:
      /* This is merely a dummy command to test whether the
         configuration file is valid.  */
      break;

    case aCreate: /* Create a new container. */
      {
        if (argc != 1) 
          wrong_args ("--create filename");
        
      }
      break;

    default:
        log_error (_("invalid command (there is no implicit command)\n"));
	break;
    }

  /* Print the audit result if needed.  */
  if (auditlog && auditfp)
    {
      audit_print_result (ctrl.audit, auditfp, 0);
      audit_release (ctrl.audit);
      ctrl.audit = NULL;
      es_fclose (auditfp);
    }
  
  /* Cleanup.  */
  g13_exit (0);
  return 8; /*NOTREACHED*/
}

/* Note: This function is used by signal handlers!. */
static void
emergency_cleanup (void)
{
  gcry_control (GCRYCTL_TERM_SECMEM );
}


void
g13_exit (int rc)
{
  gcry_control (GCRYCTL_UPDATE_RANDOM_SEED_FILE);
  if (opt.debug & DBG_MEMSTAT_VALUE)
    {
      gcry_control( GCRYCTL_DUMP_MEMORY_STATS );
      gcry_control( GCRYCTL_DUMP_RANDOM_STATS );
    }
  if (opt.debug)
    gcry_control (GCRYCTL_DUMP_SECMEM_STATS );
  emergency_cleanup ();
  rc = rc? rc : log_get_errorcount(0)? 2 : g13_errors_seen? 1 : 0;
  exit (rc);
}


void
g13_init_default_ctrl (struct server_control_s *ctrl)
{
  (void)ctrl;
}


