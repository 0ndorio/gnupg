/* scdaemon.c  -  The GnuPG Smartcard Daemon
 * Copyright (C) 2001, 2002, 2004, 2005, 
 *               2007 Free Software Foundation, Inc.
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
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#ifndef HAVE_W32_SYSTEM
#include <sys/socket.h>
#include <sys/un.h>
#endif /*HAVE_W32_SYSTEM*/
#include <unistd.h>
#include <signal.h>
#include <pth.h>

#define JNLIB_NEED_LOG_LOGV
#include "scdaemon.h"
#include <ksba.h>
#include <gcrypt.h>

#include <assuan.h> /* malloc hooks */

#include "i18n.h"
#include "sysutils.h"
#include "app-common.h"
#include "ccid-driver.h"
#include "mkdtemp.h"
#include "gc-opt-flags.h"


enum cmd_and_opt_values 
{ aNull = 0,
  oCsh		  = 'c',
  oQuiet	  = 'q',
  oSh		  = 's',
  oVerbose	  = 'v',
  
  oNoVerbose = 500,
  aGPGConfList,
  aGPGConfTest,
  oOptions,
  oDebug,
  oDebugAll,
  oDebugLevel,
  oDebugWait,
  oDebugAllowCoreDump,
  oDebugCCIDDriver,
  oNoGreeting,
  oNoOptions,
  oHomedir,
  oNoDetach,
  oNoGrab,
  oLogFile,
  oServer,
  oMultiServer,
  oDaemon,
  oBatch,
  oReaderPort,
  octapiDriver,
  opcscDriver,
  oDisableCCID,
  oDisableOpenSC,
  oDisableKeypad,
  oAllowAdmin,
  oDenyAdmin,
  oDisableApplication,
  oDebugDisableTicker
};



static ARGPARSE_OPTS opts[] = {

  { aGPGConfList, "gpgconf-list", 256, "@" },
  { aGPGConfTest, "gpgconf-test", 256, "@" },
  
  { 301, NULL, 0, N_("@Options:\n ") },

  { oServer,   "server",     0, N_("run in server mode (foreground)") },
  { oMultiServer, "multi-server", 0,
                                N_("run in multi server mode (foreground)") },
  { oDaemon,   "daemon",     0, N_("run in daemon mode (background)") },
  { oVerbose, "verbose",   0, N_("verbose") },
  { oQuiet,	"quiet",     0, N_("be somewhat more quiet") },
  { oSh,	"sh",        0, N_("sh-style command output") },
  { oCsh,	"csh",       0, N_("csh-style command output") },
  { oOptions, "options"  , 2, N_("read options from file")},
  { oDebug,	"debug"     ,4|16, "@"},
  { oDebugAll, "debug-all"     ,0, "@"},
  { oDebugLevel, "debug-level" ,2, "@"},
  { oDebugWait,"debug-wait",1, "@"},
  { oDebugAllowCoreDump, "debug-allow-core-dump", 0, "@" },
  { oDebugCCIDDriver, "debug-ccid-driver", 0, "@"},
  { oDebugDisableTicker, "debug-disable-ticker", 0, "@"},
  { oNoDetach, "no-detach" ,0, N_("do not detach from the console")},
  { oLogFile,  "log-file"   ,2, N_("use a log file for the server")},
  { oReaderPort, "reader-port", 2, N_("|N|connect to reader at port N")},
  { octapiDriver, "ctapi-driver", 2, N_("|NAME|use NAME as ct-API driver")},
  { opcscDriver, "pcsc-driver", 2, N_("|NAME|use NAME as PC/SC driver")},
  { oDisableCCID, "disable-ccid", 0,
#ifdef HAVE_LIBUSB
                                    N_("do not use the internal CCID driver")
#else
                                    "@"
#endif
                                         /* end --disable-ccid */},
  { oDisableKeypad, "disable-keypad", 0, N_("do not use a reader's keypad")},
  { oAllowAdmin, "allow-admin", 0, N_("allow the use of admin card commands")},
  { oDenyAdmin,  "deny-admin",  0, "@" },  
  { oDisableApplication, "disable-application", 2, "@"},

  {0}
};


/* The card driver we use by default for PC/SC.  */
#if defined(HAVE_W32_SYSTEM) || defined(__CYGWIN__)
#define DEFAULT_PCSC_DRIVER "winscard.dll"
#elif defined(__APPLE__)
#define DEFAULT_PCSC_DRIVER "/System/Library/Frameworks/PCSC.framework/PCSC"
#elif defined(__GLIBC__)
#define DEFAULT_PCSC_DRIVER "libpcsclite.so.1"
#else
#define DEFAULT_PCSC_DRIVER "libpcsclite.so"
#endif

/* The timer tick used for housekeeping stuff.  For Windows we use a
   longer period as the SetWaitableTimer seems to signal earlier than
   the 2 seconds.  */
#ifdef HAVE_W32_SYSTEM
#define TIMERTICK_INTERVAL    (4)
#else
#define TIMERTICK_INTERVAL    (2)    /* Seconds.  */
#endif


/* Flag to indicate that a shutdown was requested. */
static int shutdown_pending;

/* It is possible that we are currently running under setuid permissions */
static int maybe_setuid = 1;

/* Name of the communication socket */
static char *socket_name;

/* We need to keep track of the server's nonces (these are dummies for
   POSIX systems). */
static assuan_sock_nonce_t socket_nonce;

/* Debug flag to disable the ticker.  The ticker is in fact not
   disabled but it won't perform any ticker specific actions. */
static int ticker_disabled;



static char *create_socket_name (int use_standard_socket,
                                 char *standard_name, char *template);
static gnupg_fd_t create_server_socket (int is_standard_name, const char *name,
                                        assuan_sock_nonce_t *nonce);

static void *start_connection_thread (void *arg);
static void handle_connections (int listen_fd);

/* Pth wrapper function definitions. */
GCRY_THREAD_OPTION_PTH_IMPL;
static int fixed_gcry_pth_init (void)
{
  return pth_self ()? 0 : (pth_init () == FALSE) ? errno : 0;
}



static const char *
my_strusage (int level)
{
  const char *p;
  switch (level)
    {
    case 11: p = "scdaemon (GnuPG)";
      break;
    case 13: p = VERSION; break;
    case 17: p = PRINTABLE_OS_NAME; break;
    case 19: p = _("Please report bugs to <" PACKAGE_BUGREPORT ">.\n");
      break;
    case 1:
    case 40: p =  _("Usage: scdaemon [options] (-h for help)");
      break;
    case 41: p =  _("Syntax: scdaemon [options] [command [args]]\n"
                    "Smartcard daemon for GnuPG\n");
    break;
    
    default: p = NULL;
    }
  return p;
}





/* Setup the debugging.  With a LEVEL of NULL only the active debug
   flags are propagated to the subsystems.  With LEVEL set, a specific
   set of debug flags is set; thus overriding all flags already
   set. */
static void
set_debug (const char *level)
{
  if (!level)
    ;
  else if (!strcmp (level, "none"))
    opt.debug = 0;
  else if (!strcmp (level, "basic"))
    opt.debug = DBG_ASSUAN_VALUE;
  else if (!strcmp (level, "advanced"))
    opt.debug = DBG_ASSUAN_VALUE|DBG_COMMAND_VALUE;
  else if (!strcmp (level, "expert"))
    opt.debug = (DBG_ASSUAN_VALUE|DBG_COMMAND_VALUE
                 |DBG_CACHE_VALUE|DBG_CARD_IO_VALUE);
  else if (!strcmp (level, "guru"))
    opt.debug = ~0;
  else
    {
      log_error (_("invalid debug-level `%s' given\n"), level);
      scd_exit(2);
    }


  if (opt.debug && !opt.verbose)
    opt.verbose = 1;
  if (opt.debug && opt.quiet)
    opt.quiet = 0;

  if (opt.debug & DBG_MPI_VALUE)
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 2);
  if (opt.debug & DBG_CRYPTO_VALUE )
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 1);
  gcry_control (GCRYCTL_SET_VERBOSITY, (int)opt.verbose);
}
 


static void
cleanup (void)
{
  if (socket_name && *socket_name)
    {
      char *p;

      remove (socket_name);
      p = strrchr (socket_name, '/');
      if (p)
        {
          *p = 0;
          rmdir (socket_name);
          *p = '/';
        }
      *socket_name = 0;
    }
}



int
main (int argc, char **argv )
{
  ARGPARSE_ARGS pargs;
  int orig_argc;
  gpg_error_t err;
  int may_coredump;
  char **orig_argv;
  FILE *configfp = NULL;
  char *configname = NULL;
  const char *shell;
  unsigned int configlineno;
  int parse_debug = 0;
  const char *debug_level = NULL;
  int default_config =1;
  int greeting = 0;
  int nogreeting = 0;
  int pipe_server = 0;
  int multi_server = 0;
  int is_daemon = 0;
  int nodetach = 0;
  int csh_style = 0;
  char *logfile = NULL;
  int debug_wait = 0;
  int gpgconf_list = 0;
  const char *config_filename = NULL;
  int allow_coredump = 0;
  int standard_socket = 0;

  set_strusage (my_strusage);
  gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
  /* Please note that we may running SUID(ROOT), so be very CAREFUL
     when adding any stuff between here and the call to INIT_SECMEM()
     somewhere after the option parsing */
  log_set_prefix ("scdaemon", 1|4); 

  /* Make sure that our subsystems are ready.  */
  init_common_subsystems ();

  i18n_init ();

  /* Libgcrypt requires us to register the threading model first.
     Note that this will also do the pth_init. */
  gcry_threads_pth.init = fixed_gcry_pth_init;
  err = gcry_control (GCRYCTL_SET_THREAD_CBS, &gcry_threads_pth);
  if (err)
    {
      log_fatal ("can't register GNU Pth with Libgcrypt: %s\n",
                 gpg_strerror (err));
    }

  /* Check that the libraries are suitable.  Do it here because
     the option parsing may need services of the library */
  if (!gcry_check_version (NEED_LIBGCRYPT_VERSION) )
    {
      log_fatal (_("%s is too old (need %s, have %s)\n"), "libgcrypt",
                 NEED_LIBGCRYPT_VERSION, gcry_check_version (NULL) );
    }

  ksba_set_malloc_hooks (gcry_malloc, gcry_realloc, gcry_free);

  assuan_set_malloc_hooks (gcry_malloc, gcry_realloc, gcry_free);
  assuan_set_assuan_log_stream (log_get_stream ());
  assuan_set_assuan_log_prefix (log_get_prefix (NULL));
  assuan_set_assuan_err_source (GPG_ERR_SOURCE_DEFAULT);

  setup_libgcrypt_logging ();
  gcry_control (GCRYCTL_USE_SECURE_RNDPOOL);

  may_coredump = disable_core_dumps ();

  /* Set default options. */
  opt.pcsc_driver = DEFAULT_PCSC_DRIVER; 

#ifdef HAVE_W32_SYSTEM
  standard_socket = 1;  /* Under Windows we always use a standard
                           socket.  */
#endif


  shell = getenv ("SHELL");
  if (shell && strlen (shell) >= 3 && !strcmp (shell+strlen (shell)-3, "csh") )
    csh_style = 1;
  
  opt.homedir = default_homedir ();

  /* Check whether we have a config file on the commandline */
  orig_argc = argc;
  orig_argv = argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags= 1|(1<<6);  /* do not remove the args, ignore version */
  while (arg_parse( &pargs, opts))
    {
      if (pargs.r_opt == oDebug || pargs.r_opt == oDebugAll)
        parse_debug++;
      else if (pargs.r_opt == oOptions)
        { /* yes there is one, so we do not try the default one, but
	     read the option file when it is encountered at the
	     commandline */
          default_config = 0;
	}
	else if (pargs.r_opt == oNoOptions)
          default_config = 0; /* --no-options */
	else if (pargs.r_opt == oHomedir)
          opt.homedir = pargs.r.ret_str;
    }

  /* initialize the secure memory. */
  gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
  maybe_setuid = 0;

  /* 
     Now we are working under our real uid 
  */


  if (default_config)
    configname = make_filename (opt.homedir, "scdaemon.conf", NULL );

  
  argc = orig_argc;
  argv = orig_argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags=  1;  /* do not remove the args */
 next_pass:
  if (configname)
    {
      configlineno = 0;
      configfp = fopen (configname, "r");
      if (!configfp)
        {
          if (default_config)
            {
              if( parse_debug )
                log_info (_("NOTE: no default option file `%s'\n"),
                          configname );
	    }
          else
            {
              log_error (_("option file `%s': %s\n"),
                         configname, strerror(errno) );
              exit(2);
	    }
          xfree (configname); 
          configname = NULL;
	}
      if (parse_debug && configname )
        log_info (_("reading options from `%s'\n"), configname );
      default_config = 0;
    }

  while (optfile_parse( configfp, configname, &configlineno, &pargs, opts) )
    {
      switch (pargs.r_opt)
        {
        case aGPGConfList: gpgconf_list = 1; break;
        case aGPGConfTest: gpgconf_list = 2; break;
        case oQuiet: opt.quiet = 1; break;
        case oVerbose: opt.verbose++; break;
        case oBatch: opt.batch=1; break;

        case oDebug: opt.debug |= pargs.r.ret_ulong; break;
        case oDebugAll: opt.debug = ~0; break;
        case oDebugLevel: debug_level = pargs.r.ret_str; break;
        case oDebugWait: debug_wait = pargs.r.ret_int; break;
        case oDebugAllowCoreDump:
          enable_core_dumps ();
          allow_coredump = 1;
          break;
        case oDebugCCIDDriver: 
#ifdef HAVE_LIBUSB
          ccid_set_debug_level (ccid_set_debug_level (-1)+1);
#endif /*HAVE_LIBUSB*/
          break;
        case oDebugDisableTicker: ticker_disabled = 1; break;

        case oOptions:
          /* config files may not be nested (silently ignore them) */
          if (!configfp)
            {
		xfree(configname);
		configname = xstrdup(pargs.r.ret_str);
		goto next_pass;
	    }
          break;
        case oNoGreeting: nogreeting = 1; break;
        case oNoVerbose: opt.verbose = 0; break;
        case oNoOptions: break; /* no-options */
        case oHomedir: opt.homedir = pargs.r.ret_str; break;
        case oNoDetach: nodetach = 1; break;
        case oLogFile: logfile = pargs.r.ret_str; break;
        case oCsh: csh_style = 1; break;
        case oSh: csh_style = 0; break;
        case oServer: pipe_server = 1; break;
        case oMultiServer: pipe_server = 1; multi_server = 1; break;
        case oDaemon: is_daemon = 1; break;

        case oReaderPort: opt.reader_port = pargs.r.ret_str; break;
        case octapiDriver: opt.ctapi_driver = pargs.r.ret_str; break;
        case opcscDriver: opt.pcsc_driver = pargs.r.ret_str; break;
        case oDisableCCID: opt.disable_ccid = 1; break;
        case oDisableOpenSC: break;

        case oDisableKeypad: opt.disable_keypad = 1; break;

        case oAllowAdmin: opt.allow_admin = 1; break;
        case oDenyAdmin: opt.allow_admin = 0; break;

        case oDisableApplication:
          add_to_strlist (&opt.disabled_applications, pargs.r.ret_str); 
          break;

        default : pargs.err = configfp? 1:2; break;
	}
    }
  if (configfp)
    {
      fclose( configfp );
      configfp = NULL;
      /* Keep a copy of the config name for use by --gpgconf-list. */
      config_filename = configname;
      configname = NULL;
      goto next_pass;
    }
  xfree (configname);
  configname = NULL;
  if (log_get_errorcount(0))
    exit(2);
  if (nogreeting )
    greeting = 0;

  if (greeting)
    {
      fprintf (stderr, "%s %s; %s\n",
                 strusage(11), strusage(13), strusage(14) );
      fprintf (stderr, "%s\n", strusage(15) );
    }
#ifdef IS_DEVELOPMENT_VERSION
  log_info ("NOTE: this is a development version!\n");
#endif

 
  if (atexit (cleanup))
    {
      log_error ("atexit failed\n");
      cleanup ();
      exit (1);
    }

  set_debug (debug_level);

  if (debug_wait && pipe_server)
    {
      log_debug ("waiting for debugger - my pid is %u .....\n",
                 (unsigned int)getpid());
      gnupg_sleep (debug_wait);
      log_debug ("... okay\n");
    }
  
  initialize_module_command ();

  if (gpgconf_list == 2)
    scd_exit (0);
  if (gpgconf_list)
    {
      /* List options and default values in the GPG Conf format.  */
      char *filename = NULL;
      char *filename_esc;

      if (config_filename)
	filename = xstrdup (config_filename);
      else
        filename = make_filename (opt.homedir, "scdaemon.conf", NULL);
      filename_esc = percent_escape (filename, NULL);

      printf ("gpgconf-scdaemon.conf:%lu:\"%s\n",
              GC_OPT_FLAG_DEFAULT, filename_esc);
      xfree (filename_esc);
      xfree (filename);

      printf ("verbose:%lu:\n"
              "quiet:%lu:\n"
              "debug-level:%lu:\"none:\n"
              "log-file:%lu:\n",
              GC_OPT_FLAG_NONE,
              GC_OPT_FLAG_NONE,
              GC_OPT_FLAG_DEFAULT,
              GC_OPT_FLAG_NONE );

      printf ("reader-port:%lu:\n", GC_OPT_FLAG_NONE );
      printf ("ctapi-driver:%lu:\n", GC_OPT_FLAG_NONE );
      printf ("pcsc-driver:%lu:\"%s:\n",
              GC_OPT_FLAG_DEFAULT, DEFAULT_PCSC_DRIVER );
#ifdef HAVE_LIBUSB
      printf ("disable-ccid:%lu:\n", GC_OPT_FLAG_NONE );
#endif
      printf ("allow-admin:%lu:\n", GC_OPT_FLAG_NONE );
      printf ("disable-keypad:%lu:\n", GC_OPT_FLAG_NONE );

      scd_exit (0);
    }

  /* now start with logging to a file if this is desired */
  if (logfile)
    {
      log_set_file (logfile);
      log_set_prefix (NULL, 1|2|4);
    }

  if (pipe_server)
    { 
      /* This is the simple pipe based server */
      ctrl_t ctrl;
      pth_attr_t tattr;
      int fd = -1;

#ifndef HAVE_W32_SYSTEM
      {
        struct sigaction sa;
        
        sa.sa_handler = SIG_IGN;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction (SIGPIPE, &sa, NULL);
      }
#endif

      /* If --debug-allow-core-dump has been given we also need to
         switch the working directory to a place where we can actually
         write. */
      if (allow_coredump)
        {
          if (chdir("/tmp"))
            log_debug ("chdir to `/tmp' failed: %s\n", strerror (errno));
          else
            log_debug ("changed working directory to `/tmp'\n");
        }

      /* In multi server mode we need to listen on an additional
         socket.  Create that socket now before starting the handler
         for the pipe connection.  This allows that handler to send
         back the name of that socket. */
      if (multi_server)
        {
          socket_name = create_socket_name (standard_socket,
                                            "S.scdaemon",
                                            "/tmp/gpg-XXXXXX/S.scdaemon");
          
          fd = FD2INT(create_server_socket (standard_socket,
                                            socket_name, &socket_nonce));
        }

      tattr = pth_attr_new();
      pth_attr_set (tattr, PTH_ATTR_JOINABLE, 0);
      pth_attr_set (tattr, PTH_ATTR_STACK_SIZE, 512*1024);
      pth_attr_set (tattr, PTH_ATTR_NAME, "pipe-connection");

      ctrl = xtrycalloc (1, sizeof *ctrl);
      if ( !ctrl )
        {
          log_error ("error allocating connection control data: %s\n",
                     strerror (errno) );
          scd_exit (2);
        }
      ctrl->thread_startup.fd = GNUPG_INVALID_FD;
      if ( !pth_spawn (tattr, start_connection_thread, ctrl) )
        {
          log_error ("error spawning pipe connection handler: %s\n",
                     strerror (errno) );
          xfree (ctrl);
          scd_exit (2);
        }

      /* We run handle_connection to wait for the shutdown signal and
         to run the ticker stuff.  */
      handle_connections (fd);
      if (fd != -1)
        close (fd);
    }
  else if (!is_daemon)
    {
      log_info (_("please use the option `--daemon'"
                  " to run the program in the background\n"));
    }
  else
    { /* Regular server mode */
      int fd;
#ifndef HAVE_W32_SYSTEM
      pid_t pid;
      int i;
#endif

      /* Create the socket.  */
      socket_name = create_socket_name (standard_socket,
                                        "S.scdaemon",
                                        "/tmp/gpg-XXXXXX/S.scdaemon");

      fd = FD2INT (create_server_socket (standard_socket,
                                         socket_name, &socket_nonce));


      fflush (NULL);
#ifndef HAVE_W32_SYSTEM
      pid = fork ();
      if (pid == (pid_t)-1) 
        {
          log_fatal ("fork failed: %s\n", strerror (errno) );
          exit (1);
        }
      else if (pid) 
        { /* we are the parent */
          char *infostr;
          
          close (fd);
          
          /* create the info string: <name>:<pid>:<protocol_version> */
          if (estream_asprintf (&infostr, "SCDAEMON_INFO=%s:%lu:1",
				socket_name, (ulong) pid) < 0)
            {
              log_error ("out of core\n");
              kill (pid, SIGTERM);
              exit (1);
            }
          *socket_name = 0; /* don't let cleanup() remove the socket -
                               the child should do this from now on */
          if (argc) 
            { /* run the program given on the commandline */
              if (putenv (infostr))
                {
                  log_error ("failed to set environment: %s\n",
                             strerror (errno) );
                  kill (pid, SIGTERM );
                  exit (1);
                }
              execvp (argv[0], argv);
              log_error ("failed to run the command: %s\n", strerror (errno));
              kill (pid, SIGTERM);
              exit (1);
            }
          else
            {
              /* Print the environment string, so that the caller can use
                 shell's eval to set it */
              if (csh_style)
                {
                  *strchr (infostr, '=') = ' ';
                  printf ( "setenv %s\n", infostr);
                }
              else
                {
                  printf ( "%s; export SCDAEMON_INFO;\n", infostr);
                }
              xfree (infostr);
              exit (0); 
            }
          /* NOTREACHED */
        } /* end parent */
      
      /* This is the child. */

      /* Detach from tty and put process into a new session. */
      if (!nodetach )
        {  
          /* Close stdin, stdout and stderr unless it is the log stream. */
          for (i=0; i <= 2; i++) 
            {
              if ( log_test_fd (i) && i != fd)
                close (i);
            }
          if (setsid() == -1)
            {
              log_error ("setsid() failed: %s\n", strerror(errno) );
              cleanup ();
              exit (1);
            }
        }

      {
        struct sigaction sa;
        
        sa.sa_handler = SIG_IGN;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction (SIGPIPE, &sa, NULL);
      }

      if (chdir("/"))
        {
          log_error ("chdir to / failed: %s\n", strerror (errno));
          exit (1);
        }

#endif /*!HAVE_W32_SYSTEM*/

      handle_connections (fd);

      close (fd);
    }
  
  return 0;
}

void
scd_exit (int rc)
{
#if 0
#warning no update_random_seed_file
  update_random_seed_file();
#endif
#if 0
  /* at this time a bit annoying */
  if (opt.debug & DBG_MEMSTAT_VALUE)
    {
      gcry_control( GCRYCTL_DUMP_MEMORY_STATS );
      gcry_control( GCRYCTL_DUMP_RANDOM_STATS );
    }
  if (opt.debug)
    gcry_control (GCRYCTL_DUMP_SECMEM_STATS );
#endif
  gcry_control (GCRYCTL_TERM_SECMEM );
  rc = rc? rc : log_get_errorcount(0)? 2 : 0;
  exit (rc);
}


static void
scd_init_default_ctrl (ctrl_t ctrl)
{
  ctrl->reader_slot = -1;
}

static void
scd_deinit_default_ctrl (ctrl_t ctrl)
{

}


/* Return the name of the socket to be used to connect to this
   process.  If no socket is available, return NULL. */
const char *
scd_get_socket_name ()
{
  if (socket_name && *socket_name)
    return socket_name;
  return NULL;
}


static void
handle_signal (int signo)
{
  switch (signo)
    {
#ifndef HAVE_W32_SYSTEM
    case SIGHUP:
      log_info ("SIGHUP received - "
                "re-reading configuration and resetting cards\n");
/*       reread_configuration (); */
      break;
      
    case SIGUSR1:
      log_info ("SIGUSR1 received - printing internal information:\n");
      pth_ctrl (PTH_CTRL_DUMPSTATE, log_get_stream ());
      app_dump_state ();
      break;

    case SIGUSR2:
      log_info ("SIGUSR2 received - no action defined\n");
      break;

    case SIGTERM:
      if (!shutdown_pending)
        log_info ("SIGTERM received - shutting down ...\n");
      else
        log_info ("SIGTERM received - still %ld running threads\n",
                  pth_ctrl( PTH_CTRL_GETTHREADS ));
      shutdown_pending++;
      if (shutdown_pending > 2)
        {
          log_info ("shutdown forced\n");
          log_info ("%s %s stopped\n", strusage(11), strusage(13) );
          cleanup ();
          scd_exit (0);
	}
      break;
        
    case SIGINT:
      log_info ("SIGINT received - immediate shutdown\n");
      log_info( "%s %s stopped\n", strusage(11), strusage(13));
      cleanup ();
      scd_exit (0);
      break;
#endif /*!HAVE_W32_SYSTEM*/

    default:
      log_info ("signal %d received - no action defined\n", signo);
    }
}


static void
handle_tick (void)
{
  if (!ticker_disabled)
    scd_update_reader_status_file ();
}


/* Create a name for the socket.  With USE_STANDARD_SOCKET given as
   true using STANDARD_NAME in the home directory or if given has
   false from the mkdir type name TEMPLATE.  In the latter case a
   unique name in a unique new directory will be created.  In both
   cases check for valid characters as well as against a maximum
   allowed length for a unix domain socket is done.  The function
   terminates the process in case of an error.  Retunrs: Pointer to an
   allcoated string with the absolute name of the socket used.  */
static char *
create_socket_name (int use_standard_socket,
		    char *standard_name, char *template)
{
  char *name, *p;

  if (use_standard_socket)
    name = make_filename (opt.homedir, standard_name, NULL);
  else
    {
      name = xstrdup (template);
      p = strrchr (name, '/');
      if (!p)
	BUG ();
      *p = 0;
      if (!mkdtemp (name))
	{
	  log_error (_("can't create directory `%s': %s\n"),
		     name, strerror (errno));
	  scd_exit (2);
	}
      *p = '/';
    }

  if (strchr (name, PATHSEP_C))
    {
      log_error (("`%s' are not allowed in the socket name\n"), PATHSEP_S);
      scd_exit (2);
    }
  if (strlen (name) + 1 >= DIMof (struct sockaddr_un, sun_path) )
    {
      log_error (_("name of socket too long\n"));
      scd_exit (2);
    }
  return name;
}



/* Create a Unix domain socket with NAME.  IS_STANDARD_NAME indicates
   whether a non-random socket is used.  Returns the file descriptor
   or terminates the process in case of an error. */
static gnupg_fd_t
create_server_socket (int is_standard_name, const char *name,
                      assuan_sock_nonce_t *nonce)
{
  struct sockaddr_un *serv_addr;
  socklen_t len;
  gnupg_fd_t fd;
  int rc;

  fd = assuan_sock_new (AF_UNIX, SOCK_STREAM, 0);
  if (fd == GNUPG_INVALID_FD)
    {
      log_error (_("can't create socket: %s\n"), strerror (errno));
      scd_exit (2);
    }

  serv_addr = xmalloc (sizeof (*serv_addr)); 
  memset (serv_addr, 0, sizeof *serv_addr);
  serv_addr->sun_family = AF_UNIX;
  assert (strlen (name) + 1 < sizeof (serv_addr->sun_path));
  strcpy (serv_addr->sun_path, name);
  len = (offsetof (struct sockaddr_un, sun_path)
	 + strlen (serv_addr->sun_path) + 1);

  rc = assuan_sock_bind (fd, (struct sockaddr*) serv_addr, len);
  if (is_standard_name && rc == -1 && errno == EADDRINUSE)
    {
      remove (name);
      rc = assuan_sock_bind (fd, (struct sockaddr*) serv_addr, len);
    }
  if (rc != -1 
      && (rc=assuan_sock_get_nonce ((struct sockaddr*)serv_addr, len, nonce)))
    log_error (_("error getting nonce for the socket\n"));
 if (rc == -1)
    {
      log_error (_("error binding socket to `%s': %s\n"),
		 serv_addr->sun_path,
                 gpg_strerror (gpg_error_from_syserror ()));
      assuan_sock_close (fd);
      scd_exit (2);
    }

  if (listen (FD2INT(fd), 5 ) == -1)
    {
      log_error (_("listen() failed: %s\n"),
                 gpg_strerror (gpg_error_from_syserror ()));
      assuan_sock_close (fd);
      scd_exit (2);
    }
          
  if (opt.verbose)
    log_info (_("listening on socket `%s'\n"), serv_addr->sun_path);

  return fd;
}



/* This is the standard connection thread's main function.  */
static void *
start_connection_thread (void *arg)
{
  ctrl_t ctrl = arg;

  if (ctrl->thread_startup.fd != GNUPG_INVALID_FD
      && assuan_sock_check_nonce (ctrl->thread_startup.fd, &socket_nonce))
    {
      log_info (_("error reading nonce on fd %d: %s\n"), 
                FD2INT(ctrl->thread_startup.fd), strerror (errno));
      assuan_sock_close (ctrl->thread_startup.fd);
      xfree (ctrl);
      return NULL;
    }

  scd_init_default_ctrl (ctrl);
  if (opt.verbose)
    log_info (_("handler for fd %d started\n"),
              FD2INT(ctrl->thread_startup.fd));

  scd_command_handler (ctrl, FD2INT(ctrl->thread_startup.fd));

  if (opt.verbose)
    log_info (_("handler for fd %d terminated\n"),
              FD2INT (ctrl->thread_startup.fd));

  /* If this thread is the pipe connection thread, flag that a
     shutdown is required.  With the next ticker event and given that
     no other connections are running the shutdown will then
     happen. */
  if (ctrl->thread_startup.fd == GNUPG_INVALID_FD)
    shutdown_pending = 1;
  
  scd_deinit_default_ctrl (ctrl);
  xfree (ctrl);
  return NULL;
}


/* Connection handler loop.  Wait for connection requests and spawn a
   thread after accepting a connection.  LISTEN_FD is allowed to be -1
   in which case this code will only do regular timeouts and handle
   signals. */
static void
handle_connections (int listen_fd)
{
  pth_attr_t tattr;
  pth_event_t ev, time_ev;
  sigset_t sigs;
  int signo;
  struct sockaddr_un paddr;
  socklen_t plen;
  fd_set fdset, read_fdset;
  int ret;
  int fd;
  int nfd;

  tattr = pth_attr_new();
  pth_attr_set (tattr, PTH_ATTR_JOINABLE, 0);
  pth_attr_set (tattr, PTH_ATTR_STACK_SIZE, 512*1024);

#ifndef HAVE_W32_SYSTEM /* fixme */
  sigemptyset (&sigs );
  sigaddset (&sigs, SIGHUP);
  sigaddset (&sigs, SIGUSR1);
  sigaddset (&sigs, SIGUSR2);
  sigaddset (&sigs, SIGINT);
  sigaddset (&sigs, SIGTERM);
  pth_sigmask (SIG_UNBLOCK, &sigs, NULL);
  ev = pth_event (PTH_EVENT_SIGS, &sigs, &signo);
#else
  sigs = 0;
  ev = pth_event (PTH_EVENT_SIGS, &sigs, &signo);
#endif
  time_ev = NULL;

  FD_ZERO (&fdset);
  nfd = 0;
  if (listen_fd != -1)
    {
      FD_SET (listen_fd, &fdset);
      nfd = listen_fd;
    }

  for (;;)
    {
      sigset_t oldsigs;
      
      if (shutdown_pending)
        {
          if (pth_ctrl (PTH_CTRL_GETTHREADS) == 1)
            break; /* ready */

          /* Do not accept anymore connections but wait for existing
             connections to terminate. We do this by clearing out all
             file descriptors to wait for, so that the select will be
             used to just wait on a signal or timeout event. */
          FD_ZERO (&fdset);
	}

      /* Create a timeout event if needed. */
      if (!time_ev)
        time_ev = pth_event (PTH_EVENT_TIME,
                             pth_timeout (TIMERTICK_INTERVAL, 0));

      /* POSIX says that fd_set should be implemented as a structure,
         thus a simple assignment is fine to copy the entire set.  */
      read_fdset = fdset;

      if (time_ev)
        pth_event_concat (ev, time_ev, NULL);
      ret = pth_select_ev (nfd+1, &read_fdset, NULL, NULL, NULL, ev);
      if (time_ev)
        pth_event_isolate (time_ev);

      if (ret == -1)
	{
          if (pth_event_occurred (ev)
              || (time_ev && pth_event_occurred (time_ev)))
            {
              if (pth_event_occurred (ev))
                handle_signal (signo);
              if (time_ev && pth_event_occurred (time_ev))
                {
                  pth_event_free (time_ev, PTH_FREE_ALL);
                  time_ev = NULL;
                  handle_tick ();
                }
              continue;
            }
          log_error (_("pth_select failed: %s - waiting 1s\n"),
                     strerror (errno));
          pth_sleep (1);
	  continue;
	}

      if (pth_event_occurred (ev))
        {
          handle_signal (signo);
        }

      if (time_ev && pth_event_occurred (time_ev))
        {
          pth_event_free (time_ev, PTH_FREE_ALL);
          time_ev = NULL;
          handle_tick ();
        }

      /* We now might create new threads and because we don't want any
         signals - we are handling here - to be delivered to a new
         thread. Thus we need to block those signals. */
      pth_sigmask (SIG_BLOCK, &sigs, &oldsigs);

      if (listen_fd != -1 && FD_ISSET (listen_fd, &read_fdset))
	{
          ctrl_t ctrl;

          plen = sizeof paddr;
	  fd = pth_accept (listen_fd, (struct sockaddr *)&paddr, &plen);
	  if (fd == -1)
	    {
	      log_error ("accept failed: %s\n", strerror (errno));
	    }
          else if ( !(ctrl = xtrycalloc (1, sizeof *ctrl)) )
            {
              log_error ("error allocating connection control data: %s\n",
                         strerror (errno) );
              close (fd);
            }
          else
            {
              char threadname[50];

              snprintf (threadname, sizeof threadname-1, "conn fd=%d", fd);
              threadname[sizeof threadname -1] = 0;
              pth_attr_set (tattr, PTH_ATTR_NAME, threadname);
              ctrl->thread_startup.fd = INT2FD (fd);
              if (!pth_spawn (tattr, start_connection_thread, ctrl))
                {
                  log_error ("error spawning connection handler: %s\n",
                             strerror (errno) );
                  xfree (ctrl);
                  close (fd);
                }
            }
          fd = -1;
	}

      /* Restore the signal mask. */
      pth_sigmask (SIG_SETMASK, &oldsigs, NULL);

    }

  pth_event_free (ev, PTH_FREE_ALL);
  if (time_ev)
    pth_event_free (time_ev, PTH_FREE_ALL);
  cleanup ();
  log_info (_("%s %s stopped\n"), strusage(11), strusage(13));
}


