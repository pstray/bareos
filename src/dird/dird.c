/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2013 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * BAREOS Director daemon -- this is the main program
 *
 * Kern Sibbald, March MM
 */

#include "bareos.h"
#include "dird.h"
#ifndef HAVE_REGEX_H
#include "lib/bregex.h"
#else
#include <regex.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#define NAMELEN(dirent) (strlen((dirent)->d_name))
#endif
#ifndef HAVE_READDIR_R
int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
#endif

/* Forward referenced subroutines */
void terminate_dird(int sig);
static bool check_resources(CONFIG *config);
static bool initialize_sql_pooling(CONFIG *config);
static void cleanup_old_files();

/* Exported subroutines */
extern "C" void reload_config(int sig);
extern void invalidate_schedules();
extern bool parse_dir_config(CONFIG *config, const char *configfile, int exit_code);

/* Imported subroutines */
void start_UA_server(dlist *addrs);
void stop_UA_server(void);
void init_job_server(int max_workers);
void term_job_server();
void init_device_resources();

static char *runjob = NULL;
static bool background = true;
static void init_reload(void);

/* Globals Exported */
DIRRES *me = NULL;                    /* Our Global resource */
CONFIG *my_config = NULL;             /* Our Global config */
char *configfile = NULL;
void *start_heap;

/* Globals Imported */
extern RES_ITEM job_items[];
#if defined(_MSC_VER)
extern "C" { // work around visual compiler mangling variables
   extern URES res_all;
}
#else
extern URES res_all;
#endif

typedef enum {
   CHECK_CONNECTION,  /* Check catalog connection */
   UPDATE_CATALOG,    /* Ensure that catalog is ok with conf */
   UPDATE_AND_FIX     /* Ensure that catalog is ok, and fix old jobs */
} cat_op;
static bool check_catalog(CONFIG *config, cat_op mode);

#define CONFIG_FILE "bareos-dir.conf" /* default configuration file */

/*
 * This allows the message handler to operate on the database
 * by using a pointer to this function. The pointer is
 * needed because the other daemons do not have access
 * to the database. If the pointer is not defined (other daemons),
 * then writing the database is disabled.
 */
static bool dir_db_log_insert(JCR *jcr, utime_t mtime, char *msg)
{
   int length;
   bool retval;
   char ed1[50];
   char dt[MAX_TIME_LENGTH];
   POOLMEM *cmd, *esc_msg;

   if (!jcr || !jcr->db || !jcr->db->is_connected()) {
      return false;
   }

   cmd = get_pool_memory(PM_MESSAGE);
   esc_msg = get_pool_memory(PM_MESSAGE);

   length = strlen(msg) + 1;

   esc_msg = check_pool_memory_size(esc_msg, length * 2 + 1);
   db_escape_string(jcr, jcr->db, esc_msg, msg, length);

   bstrutime(dt, sizeof(dt), mtime);
   Mmsg(cmd, "INSERT INTO Log (JobId, Time, LogText) VALUES (%s,'%s','%s')",
        edit_int64(jcr->JobId, ed1), dt, esc_msg);

   retval = db_sql_query(jcr->db, cmd);

   free_pool_memory(cmd);
   free_pool_memory(esc_msg);

   return retval;
}

static void usage()
{
   fprintf(stderr, _(
PROG_COPYRIGHT
"\nVersion: %s (%s)\n\n"
"Usage: bareos-dir [-f -s] [-c config_file] [-d debug_level] [config_file]\n"
"       -c <file>   set configuration file to file\n"
"       -d <nn>     set debug level to <nn>\n"
"       -dt         print timestamp in debug output\n"
"       -f          run in foreground (for debugging)\n"
"       -g          groupid\n"
"       -m          print kaboom output (for debugging)\n"
"       -r <job>    run <job> now\n"
"       -s          no signals\n"
"       -t          test - read configuration and exit\n"
"       -u          userid\n"
"       -v          verbose user messages\n"
"       -?          print this message.\n"
"\n"), 2000, VERSION, BDATE);

   exit(1);
}


/*********************************************************************
 *
 *         Main BAREOS Director Server program
 *
 */
#if defined(HAVE_WIN32)
/* For Win32 main() is in src/win32 code ... */
#define main BAREOSMain
#endif

int main (int argc, char *argv[])
{
   int ch;
   JCR *jcr;
   bool no_signals = false;
   bool test_config = false;
   char *uid = NULL;
   char *gid = NULL;

   start_heap = sbrk(0);
   setlocale(LC_ALL, "");
   bindtextdomain("bareos", LOCALEDIR);
   textdomain("bareos");

   init_stack_dump();
   my_name_is(argc, argv, "bareos-dir");
   init_msg(NULL, NULL);              /* initialize message handler */
   init_reload();
   daemon_start_time = time(NULL);

   console_command = run_console_command;

   while ((ch = getopt(argc, argv, "c:d:fg:mr:stu:v?")) != -1) {
      switch (ch) {
      case 'c':                    /* specify config file */
         if (configfile != NULL) {
            free(configfile);
         }
         configfile = bstrdup(optarg);
         break;

      case 'd':                    /* set debug level */
         if (*optarg == 't') {
            dbg_timestamp = true;
         } else {
            debug_level = atoi(optarg);
            if (debug_level <= 0) {
               debug_level = 1;
            }
         }
         Dmsg1(10, "Debug level = %d\n", debug_level);
         break;

      case 'f':                    /* run in foreground */
         background = false;
         break;

      case 'g':                    /* set group id */
         gid = optarg;
         break;

      case 'm':                    /* print kaboom output */
         prt_kaboom = true;
         break;

      case 'r':                    /* run job */
         if (runjob != NULL) {
            free(runjob);
         }
         if (optarg) {
            runjob = bstrdup(optarg);
         }
         break;

      case 's':                    /* turn off signals */
         no_signals = true;
         break;

      case 't':                    /* test config */
         test_config = true;
         break;

      case 'u':                    /* set uid */
         uid = optarg;
         break;

      case 'v':                    /* verbose */
         verbose++;
         break;

      case '?':
      default:
         usage();

      }
   }
   argc -= optind;
   argv += optind;

   if (!no_signals) {
      init_signals(terminate_dird);
   }

   if (argc) {
      if (configfile != NULL) {
         free(configfile);
      }
      configfile = bstrdup(*argv);
      argc--;
      argv++;
   }
   if (argc) {
      usage();
   }

   if (configfile == NULL) {
      configfile = bstrdup(CONFIG_FILE);
   }

   /*
    * See if we want to drop privs.
    */
   if (geteuid() == 0) {
      drop(uid, gid, false);                    /* reduce privileges if requested */
   }

   my_config = new_config_parser();
   parse_dir_config(my_config, configfile, M_ERROR_TERM);

   if (init_crypto() != 0) {
      Jmsg((JCR *)NULL, M_ERROR_TERM, 0, _("Cryptography library initialization failed.\n"));
   }

   if (!check_resources(my_config)) {
      Jmsg((JCR *)NULL, M_ERROR_TERM, 0, _("Please correct configuration file: %s\n"), configfile);
   }

   if (!test_config) {                /* we don't need to do this block in test mode */
      if (background) {
         daemon_start();
         init_stack_dump();              /* grab new pid */
      }
      /* Create pid must come after we are a daemon -- so we have our final pid */
      create_pid_file(me->pid_directory, "bareos-dir",
                      get_first_port_host_order(me->DIRaddrs));
      read_state_file(me->working_directory, "bareos-dir",
                      get_first_port_host_order(me->DIRaddrs));
   }

   set_jcr_in_tsd(INVALID_JCR);
   set_thread_concurrency(me->MaxConcurrentJobs * 2 +
                          4 /* UA */ + 5 /* sched+watchdog+jobsvr+misc */);
   lmgr_init_thread(); /* initialize the lockmanager stack */

   load_dir_plugins(me->plugin_directory, me->plugin_names);

   /* If we are in testing mode, we don't try to fix the catalog */
   cat_op mode=(test_config)?CHECK_CONNECTION:UPDATE_AND_FIX;

   if (!check_catalog(my_config, mode)) {
      Jmsg((JCR *)NULL, M_ERROR_TERM, 0, _("Please correct configuration file: %s\n"), configfile);
   }

   if (test_config) {
      terminate_dird(0);
   }

   if (!initialize_sql_pooling(my_config)) {
      Jmsg((JCR *)NULL, M_ERROR_TERM, 0, _("Please correct configuration file: %s\n"), configfile);
   }

   my_name_is(0, NULL, me->name());    /* set user defined name */

   cleanup_old_files();

   p_db_log_insert = (db_log_insert_func)dir_db_log_insert;

#if !defined(HAVE_WIN32)
   signal(SIGHUP, reload_config);
#endif

   init_console_msg(working_directory);

   Dmsg0(200, "Start UA server\n");
   start_UA_server(me->DIRaddrs);

   start_watchdog();                  /* start network watchdog thread */

   init_jcr_subsystem();              /* start JCR watchdogs etc. */

   init_job_server(me->MaxConcurrentJobs);

   dbg_jcr_add_hook(db_debug_print); /* used to debug B_DB connexion after fatal signal */

//   init_device_resources();

   Dmsg0(200, "wait for next job\n");
   /* Main loop -- call scheduler to get next job to run */
   while ( (jcr = wait_for_next_job(runjob)) ) {
      run_job(jcr);                   /* run job */
      free_jcr(jcr);                  /* release jcr */
      set_jcr_in_tsd(INVALID_JCR);
      if (runjob) {                   /* command line, run a single job? */
         break;                       /* yes, terminate */
      }
   }

   terminate_dird(0);

   return 0;
}

/* Cleanup and then exit */
void terminate_dird(int sig)
{
   static bool already_here = false;

   if (already_here) {                /* avoid recursive temination problems */
      bmicrosleep(2, 0);              /* yield */
      exit(1);
   }
   already_here = true;
   debug_level = 0;                   /* turn off debug */
   stop_watchdog();
   db_sql_pool_destroy();
   db_flush_backends();
   unload_dir_plugins();
   write_state_file(me->working_directory, "bareos-dir", get_first_port_host_order(me->DIRaddrs));
   delete_pid_file(me->pid_directory, "bareos-dir", get_first_port_host_order(me->DIRaddrs));
   term_scheduler();
   term_job_server();
   if (runjob) {
      free(runjob);
   }
   if (configfile != NULL) {
      free(configfile);
   }
   if (debug_level > 5) {
      print_memory_pool_stats();
   }
   if (my_config) {
      my_config->free_all_resources();
      free(my_config);
      my_config = NULL;
   }
   stop_UA_server();
   term_msg();                        /* terminate message handler */
   cleanup_crypto();
   close_memory_pool();               /* release free memory in pool */
   lmgr_cleanup_main();
   sm_dump(false);
   exit(sig);
}

/*
 * Save pointer to res_containers and job count
 */
struct RELOAD_TABLE {
   int job_count;
   RES_CONTAINER **res_containers;
};

static const int max_reloads = 32;
static RELOAD_TABLE reload_table[max_reloads];

static void init_reload(void)
{
   int i;

   for (i = 0; i < max_reloads; i++) {
      reload_table[i].job_count = 0;
      reload_table[i].res_containers = NULL;
   }
}

/*
 * This subroutine frees a saved resource table.
 *  It was saved when a new table was created with "reload"
 */
static void free_saved_resources(CONFIG *config, int table)
{
   RES *next, *res;
   int j;
   int num = config->m_r_last - config->m_r_first + 1;
   RES_CONTAINER **res_containers = reload_table[table].res_containers;

   if (res_containers == NULL) {
      Dmsg1(100, "res_tab for table %d already released.\n", table);
      return;
   }

   Dmsg1(100, "Freeing resources for table %d\n", table);

   for (j = 0; j < num; j++) {
      if (res_containers[j]) {
         next = res_containers[j]->head;
         while (next) {
            res = next;
            next = res->res_next;
            free_resource(res, config->m_r_first + j);
         }
         free(res_containers[j]->list);
         free(res_containers[j]);
         res_containers[j] = NULL;
      }
   }

   free(res_containers);
   reload_table[table].job_count = 0;
   reload_table[table].res_containers = NULL;
}

/*
 * Called here at the end of every job that was
 * hooked decrementing the active job_count. When
 * it goes to zero, no one is using the associated
 * resource table, so free it.
 */
static void reload_job_end_cb(JCR *jcr, void *ctx)
{
   int reload_id = (int)((intptr_t)ctx);

   Dmsg3(100, "reload job_end JobId=%d table=%d cnt=%d\n", jcr->JobId,
      reload_id, reload_table[reload_id].job_count);

   lock_jobs();
   LockRes(my_config);

   if (--reload_table[reload_id].job_count <= 0) {
      free_saved_resources(my_config, reload_id);
   }

   UnlockRes(my_config);
   unlock_jobs();
}

static int find_free_reload_table_entry()
{
   int table = -1;
   for (int i=0; i < max_reloads; i++) {
      if (reload_table[i].res_containers == NULL) {
         table = i;
         break;
      }
   }
   return table;
}

/*
 * If we get here, we have received a SIGHUP, which means to
 *    reread our configuration file.
 *
 * The algorithm used is as follows: we count how many jobs are
 *   running and mark the running jobs to make a callback on
 *   exiting. The old config is saved with the reload table
 *   id in a reload table. The new config file is read. Now, as
 *   each job exits, it calls back to the reload_job_end_cb(), which
 *   decrements the count of open jobs for the given reload table.
 *   When the count goes to zero, we release those resources.
 *   This allows us to have pointers into the resource table (from
 *   jobs), and once they exit and all the pointers are released, we
 *   release the old table. Note, if no new jobs are running since the
 *   last reload, then the old resources will be immediately release.
 *   A console is considered a job because it may have pointers to
 *   resources, but a SYSTEM job is not since it *should* not have any
 *   permanent pointers to jobs.
 */
extern "C"
void reload_config(int sig)
{
   static bool already_here = false;
#if !defined(HAVE_WIN32)
   sigset_t set;
#endif
   JCR *jcr;
   int njobs = 0;                     /* number of running jobs */
   int table, rtable;
   bool ok;

   if (already_here) {
      abort();                        /* Oops, recursion -> die */
   }
   already_here = true;

#if !defined(HAVE_WIN32)
   sigemptyset(&set);
   sigaddset(&set, SIGHUP);
   sigprocmask(SIG_BLOCK, &set, NULL);
#endif

   lock_jobs();
   LockRes(my_config);

   table = find_free_reload_table_entry();
   if (table < 0) {
      Jmsg(NULL, M_ERROR, 0, _("Too many open reload requests. Request ignored.\n"));
      goto bail_out;
   }

   /**
    * Flush the sql connection pools.
    */
   db_sql_pool_flush();

   Dmsg1(100, "Reload_config njobs=%d\n", njobs);

   /*
    * Save current res_containers
    */
   reload_table[table].res_containers = my_config->m_res_containers;
   Dmsg1(100, "Saved old config in table %d\n", table);

   /*
    * Create a new res_containers and parse into it
    */
   my_config->new_res_containers();
   ok = parse_dir_config(my_config, configfile, M_ERROR);

   Dmsg0(100, "Reloaded config file\n");
   if (!ok ||
       !check_resources(my_config) ||
       !check_catalog(my_config, UPDATE_CATALOG) ||
       !initialize_sql_pooling(my_config)) {
      rtable = find_free_reload_table_entry();    /* save new, bad table */
      if (rtable < 0) {
         Jmsg(NULL, M_ERROR, 0, _("Please correct configuration file: %s\n"), configfile);
         Jmsg(NULL, M_ERROR_TERM, 0, _("Out of reload table entries. Giving up.\n"));
      } else {
         Jmsg(NULL, M_ERROR, 0, _("Please correct configuration file: %s\n"), configfile);
         Jmsg(NULL, M_ERROR, 0, _("Resetting previous configuration.\n"));
      }

      /*
       * Save broken res_containers pointer
       */
      reload_table[rtable].res_containers = my_config->m_res_containers;

      /*
       * Now restore old resource pointer
       */
      my_config->m_res_containers = reload_table[table].res_containers;
      table = rtable;           /* release new, bad, saved table below */
   } else {
      invalidate_schedules();

      /*
       * Hook all active jobs so that they release this table
       */
      foreach_jcr(jcr) {
         if (jcr->getJobType() != JT_SYSTEM) {
            reload_table[table].job_count++;
            job_end_push(jcr, reload_job_end_cb, (void *)((long int)table));
            njobs++;
         }
      }
      endeach_jcr(jcr);
   }

   /* Reset globals */
   set_working_directory(me->working_directory);
   Dmsg0(10, "Director's configuration file reread.\n");

   /* Now release saved resources, if no jobs using the resources */
   if (njobs == 0) {
      free_saved_resources(my_config, table);
   }

bail_out:
   UnlockRes(my_config);
   unlock_jobs();
#if !defined(HAVE_WIN32)
   sigprocmask(SIG_UNBLOCK, &set, NULL);
   signal(SIGHUP, reload_config);
#endif
   already_here = false;
}

/*
 * Make a quick check to see that we have all the
 * resources needed.
 *
 *  **** FIXME **** this routine could be a lot more
 *   intelligent and comprehensive.
 */
static bool check_resources(CONFIG *config)
{
   bool OK = true;
   JOBRES *job;
   bool need_tls;

   job = (JOBRES *)config->GetNextRes(R_JOB, NULL);
   me = (DIRRES *)config->GetNextRes(R_DIRECTOR, NULL);
   if (!me) {
      Jmsg(NULL, M_FATAL, 0, _("No Director resource defined in %s\n"
                               "Without that I don't know who I am :-(\n"), configfile);
      OK = false;
   } else {
      set_working_directory(me->working_directory);
      if (!me->messages) {       /* If message resource not specified */
         me->messages = (MSGSRES *)config->GetNextRes(R_MSGS, NULL);
         if (!me->messages) {
            Jmsg(NULL, M_FATAL, 0, _("No Messages resource defined in %s\n"), configfile);
            OK = false;
         }
      }

      /*
       * When the user didn't force use we optimize for size.
       */
      if (!me->optimize_for_size && !me->optimize_for_speed) {
         me->optimize_for_size = true;
      } else if (me->optimize_for_size && me->optimize_for_speed) {
         Jmsg(NULL, M_FATAL, 0, _("Cannot optimize for speed and size define only one in %s\n"), configfile);
         OK = false;
      }

      if (config->GetNextRes(R_DIRECTOR, (RES *)me) != NULL) {
         Jmsg(NULL, M_FATAL, 0, _("Only one Director resource permitted in %s\n"),
            configfile);
         OK = false;
      }

      /*
       * tls_require implies tls_enable
       */
      if (me->tls_require) {
         if (have_tls) {
            me->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in BAREOS.\n"));
            OK = false;
         }
      }

      need_tls = me->tls_enable || me->tls_authenticate;

      if (!me->tls_certfile && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Director \"%s\" in %s.\n"), me->name(), configfile);
         OK = false;
      }

      if (!me->tls_keyfile && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Key\" file not defined for Director \"%s\" in %s.\n"), me->name(), configfile);
         OK = false;
      }

      if ((!me->tls_ca_certfile && !me->tls_ca_certdir) &&
           need_tls && me->tls_verify_peer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\" or \"TLS CA"
              " Certificate Dir\" are defined for Director \"%s\" in %s."
              " At least one CA certificate store is required"
              " when using \"TLS Verify Peer\".\n"),
              me->name(), configfile);
         OK = false;
      }

      /*
       * If everything is well, attempt to initialize our per-resource TLS context
       */
      if (OK && (need_tls || me->tls_require)) {
         /*
          * Initialize TLS context:
          * Args: CA certfile, CA certdir, Certfile, Keyfile,
          * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer
          */
         me->tls_ctx = new_tls_context(me->tls_ca_certfile,
            me->tls_ca_certdir, me->tls_crlfile, me->tls_certfile,
            me->tls_keyfile, NULL, NULL, me->tls_dhfile,
            me->tls_verify_peer);

         if (!me->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for Director \"%s\" in %s.\n"),
                 me->name(), configfile);
            OK = false;
         }
      }
   }

   if (!job) {
      Jmsg(NULL, M_FATAL, 0, _("No Job records defined in %s\n"), configfile);
      OK = false;
   }

   foreach_res(config, job, R_JOB) {
      if (job->jobdefs) {
         /*
          * Handle Storage alists specifically
          */
         JOBRES *jobdefs = job->jobdefs;
         if (jobdefs->storage && !job->storage) {
            STORERES *store;
            job->storage = New(alist(10, not_owned_by_alist));
            foreach_alist(store, jobdefs->storage) {
               job->storage->append(store);
            }
         }

         /*
          * Handle RunScripts alists specifically
          */
         if (jobdefs->RunScripts) {
            RUNSCRIPT *rs, *elt;

            if (!job->RunScripts) {
               job->RunScripts = New(alist(10, not_owned_by_alist));
            }

            foreach_alist(rs, jobdefs->RunScripts) {
               elt = copy_runscript(rs);
               job->RunScripts->append(elt); /* we have to free it */
            }
         }

         /*
          * Transfer default items from JobDefs Resource
          */
         for (int i = 0; job_items[i].name; i++) {
            char **def_svalue, **svalue;   /* string value */
            uint32_t *def_ivalue, *ivalue; /* integer value */
            bool *def_bvalue, *bvalue;     /* bool value */
            uint64_t *def_lvalue, *lvalue;  /* 64 bit values */
            uint32_t offset;

            Dmsg4(1400, "Job \"%s\", field \"%s\" bit=%d def=%d\n",
                job->name(), job_items[i].name,
                bit_is_set(i, job->hdr.item_present),
                bit_is_set(i, job->jobdefs->hdr.item_present));

            if (!bit_is_set(i, job->hdr.item_present) &&
                 bit_is_set(i, job->jobdefs->hdr.item_present)) {
               Dmsg2(400, "Job \"%s\", field \"%s\": getting default.\n",
                     job->name(), job_items[i].name);
               offset = (char *)(job_items[i].value) - (char *)&res_all;
               switch (job_items[i].type) {
               case CFG_TYPE_STR:
               case CFG_TYPE_DIR:
                  /*
                   * Handle strings and directory strings
                   */
                  def_svalue = (char **)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_svalue=%s item %d offset=%u\n",
                        job->name(), job_items[i].name, *def_svalue, i, offset);
                  svalue = (char **)((char *)job + offset);
                  if (*svalue) {
                     free(*svalue);
                  }
                  *svalue = bstrdup(*def_svalue);
                  set_bit(i, job->hdr.item_present);
                  break;
               case CFG_TYPE_RES:
                  /*
                   * Handle resources
                   */
                  def_svalue = (char **)((char *)(job->jobdefs) + offset);
                  Dmsg4(400, "Job \"%s\", field \"%s\" item %d offset=%u\n",
                        job->name(), job_items[i].name, i, offset);
                  svalue = (char **)((char *)job + offset);
                  if (*svalue) {
                     Pmsg1(000, _("Hey something is wrong. p=0x%lu\n"), *svalue);
                  }
                  *svalue = *def_svalue;
                  set_bit(i, job->hdr.item_present);
                  break;
               case CFG_TYPE_ALIST_RES:
                  /*
                   * Handle alist resources
                   */
                  if (bit_is_set(i, job->jobdefs->hdr.item_present)) {
                     set_bit(i, job->hdr.item_present);
                  }
                  break;
               case CFG_TYPE_BIT:
               case CFG_TYPE_PINT32:
               case CFG_TYPE_JOBTYPE:
               case CFG_TYPE_PROTOCOLTYPE:
               case CFG_TYPE_LEVEL:
               case CFG_TYPE_INT32:
               case CFG_TYPE_SIZE32:
               case CFG_TYPE_MIGTYPE:
               case CFG_TYPE_REPLACE:
                  /*
                   * Handle integer fields
                   *    Note, our store_bit does not handle bitmaped fields
                   */
                  def_ivalue = (uint32_t *)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_ivalue=%d item %d offset=%u\n",
                       job->name(), job_items[i].name, *def_ivalue, i, offset);
                  ivalue = (uint32_t *)((char *)job + offset);
                  *ivalue = *def_ivalue;
                  set_bit(i, job->hdr.item_present);
                  break;
               case CFG_TYPE_TIME:
               case CFG_TYPE_SIZE64:
               case CFG_TYPE_INT64:
               case CFG_TYPE_SPEED:
                  /*
                   * Handle 64 bit integer fields
                   */
                  def_lvalue = (uint64_t *)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_lvalue=%" lld " item %d offset=%u\n",
                       job->name(), job_items[i].name, *def_lvalue, i, offset);
                  lvalue = (uint64_t *)((char *)job + offset);
                  *lvalue = *def_lvalue;
                  set_bit(i, job->hdr.item_present);
                  break;
               case CFG_TYPE_BOOL:
                  /*
                   * Handle bool fields
                   */
                  def_bvalue = (bool *)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_bvalue=%d item %d offset=%u\n",
                       job->name(), job_items[i].name, *def_bvalue, i, offset);
                  bvalue = (bool *)((char *)job + offset);
                  *bvalue = *def_bvalue;
                  set_bit(i, job->hdr.item_present);
                  break;
               default:
                  break;
               }
            }
         }
      }

      /*
       * Ensure that all required items are present
       */
      for (int i = 0; job_items[i].name; i++) {
         if (job_items[i].flags & CFG_ITEM_REQUIRED) {
               if (!bit_is_set(i, job->hdr.item_present)) {
                  Jmsg(NULL, M_ERROR_TERM, 0,
                       _("\"%s\" directive in Job \"%s\" resource is required, but not found.\n"),
                       job_items[i].name, job->name());
                  OK = false;
                }
         }

         /*
          * If this triggers, take a look at lib/parse_conf.h
          */
         if (i >= MAX_RES_ITEMS) {
            Emsg0(M_ERROR_TERM, 0, _("Too many items in Job resource\n"));
         }
      }

      /*
       * For Copy and Migrate we can have Jobs without a client or fileset.
       */
      switch (job->JobType) {
      case JT_COPY:
      case JT_MIGRATE:
         break;
      default:
         /*
          * All others must have a client and fileset.
          */
         if (!job->client) {
            Jmsg(NULL, M_ERROR_TERM, 0,
                 _("\"client\" directive in Job \"%s\" resource is required, but not found.\n"),
                 job->name());
            OK = false;
         }

         if (!job->fileset) {
            Jmsg(NULL, M_ERROR_TERM, 0,
                 _("\"fileset\" directive in Job \"%s\" resource is required, but not found.\n"),
                 job->name());
            OK = false;
         }
         break;
      }

      if (!job->storage && !job->pool->storage) {
         Jmsg(NULL, M_FATAL, 0, _("No storage specified in Job \"%s\" nor in Pool.\n"),
            job->name());
         OK = false;
      }
   } /* End loop over Job res */


   /*
    * Loop over Consoles
    */
   CONRES *cons;
   foreach_res(config, cons, R_CONSOLE) {
      /*
       * tls_require implies tls_enable
       */
      if (cons->tls_require) {
         if (have_tls) {
            cons->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in BAREOS.\n"));
            OK = false;
            continue;
         }
      }

      need_tls = cons->tls_enable || cons->tls_authenticate;

      if (!cons->tls_certfile && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Console \"%s\" in %s.\n"),
            cons->name(), configfile);
         OK = false;
      }

      if (!cons->tls_keyfile && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Key\" file not defined for Console \"%s\" in %s.\n"),
            cons->name(), configfile);
         OK = false;
      }

      if ((!cons->tls_ca_certfile && !cons->tls_ca_certdir)
            && need_tls && cons->tls_verify_peer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\" or \"TLS CA"
            " Certificate Dir\" are defined for Console \"%s\" in %s."
            " At least one CA certificate store is required"
            " when using \"TLS Verify Peer\".\n"),
            cons->name(), configfile);
         OK = false;
      }

      /*
       * If everything is well, attempt to initialize our per-resource TLS context
       */
      if (OK && (need_tls || cons->tls_require)) {
         /*
          * Initialize TLS context:
          * Args: CA certfile, CA certdir, Certfile, Keyfile,
          * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer
          */
         cons->tls_ctx = new_tls_context(cons->tls_ca_certfile,
                                         cons->tls_ca_certdir, cons->tls_crlfile, cons->tls_certfile,
                                         cons->tls_keyfile, NULL, NULL,
                                         cons->tls_dhfile, cons->tls_verify_peer);
         if (!cons->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for File daemon \"%s\" in %s.\n"),
               cons->name(), configfile);
            OK = false;
         }
      }

   }

   /*
    * Loop over Clients
    */
   me->subscriptions_used = 0;
   CLIENTRES *client;
   foreach_res(config, client, R_CLIENT) {
      /*
       * Count the number of clients
       *
       * Only used as indication not an enforced limit.
       */
      me->subscriptions_used++;

      /*
       * tls_require implies tls_enable
       */
      if (client->tls_require) {
         if (have_tls) {
            client->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in BAREOS.\n"));
            OK = false;
            continue;
         }
      }
      need_tls = client->tls_enable || client->tls_authenticate;
      if ((!client->tls_ca_certfile && !client->tls_ca_certdir) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
            " or \"TLS CA Certificate Dir\" are defined for File daemon \"%s\" in %s.\n"),
            client->name(), configfile);
         OK = false;
      }

      /*
       * If everything is well, attempt to initialize our per-resource TLS context
       */
      if (OK && (need_tls || client->tls_require)) {
         /*
          * Initialize TLS context:
          * Args: CA certfile, CA certdir, Certfile, Keyfile,
          * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer
          */
         client->tls_ctx = new_tls_context(client->tls_ca_certfile,
                                           client->tls_ca_certdir, client->tls_crlfile, client->tls_certfile,
                                           client->tls_keyfile, NULL, NULL, NULL,
                                           true);
         if (!client->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for File daemon \"%s\" in %s.\n"),
               client->name(), configfile);
            OK = false;
         }
      }
   }

   /*
    * Loop over Storages
    */
   STORERES *store;
   foreach_res(config, store, R_STORAGE) {
      /*
       * tls_require implies tls_enable
       */
      if (store->tls_require) {
         if (have_tls) {
            store->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in BAREOS.\n"));
            OK = false;
            continue;
         }
      }

      need_tls = store->tls_enable || store->tls_authenticate;

      if ((!store->tls_ca_certfile && !store->tls_ca_certdir) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
              " or \"TLS CA Certificate Dir\" are defined for Storage \"%s\" in %s.\n"),
              store->name(), configfile);
         OK = false;
      }

      /*
       * If everything is well, attempt to initialize our per-resource TLS context
       */
      if (OK && (need_tls || store->tls_require)) {
        /*
         * Initialize TLS context:
         * Args: CA certfile, CA certdir, Certfile, Keyfile,
         * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer
         */
         store->tls_ctx = new_tls_context(store->tls_ca_certfile,
                                          store->tls_ca_certdir, store->tls_crlfile, store->tls_certfile,
                                          store->tls_keyfile, NULL, NULL, NULL, true);
         if (!store->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for Storage \"%s\" in %s.\n"),
                 store->name(), configfile);
            OK = false;
         }
      }
   }

   if (OK) {
      close_msg(NULL);                    /* close temp message handler */
      init_msg(NULL, me->messages); /* open daemon message handler */
   }
   return OK;
}

/*
 * Initialize the sql pooling.
 */
static bool initialize_sql_pooling(CONFIG *config)
{
   bool retval = true;
   CATRES *catalog;

   foreach_res(config, catalog, R_CATALOG) {
      if (!db_sql_pool_initialize(catalog->db_driver,
                                  catalog->db_name,
                                  catalog->db_user,
                                  catalog->db_password.value,
                                  catalog->db_address,
                                  catalog->db_port,
                                  catalog->db_socket,
                                  catalog->disable_batch_insert,
                                  catalog->pooling_min_connections,
                                  catalog->pooling_max_connections,
                                  catalog->pooling_increment_connections,
                                  catalog->pooling_idle_timeout,
                                  catalog->pooling_validate_timeout)) {
         Jmsg(NULL, M_FATAL, 0, _("Could not setup sql pooling for Catalog \"%s\", database \"%s\".\n"),
              catalog->name(), catalog->db_name);
         retval = false;
         continue;
      }
   }

   return retval;
}

/*
 * In this routine,
 *  - we can check the connection (mode=CHECK_CONNECTION)
 *  - we can synchronize the catalog with the configuration (mode=UPDATE_CATALOG)
 *  - we can synchronize, and fix old job records (mode=UPDATE_AND_FIX)
 */
static bool check_catalog(CONFIG *config, cat_op mode)
{
   bool OK = true;

   /* Loop over databases */
   CATRES *catalog;
   foreach_res(config, catalog, R_CATALOG) {
      B_DB *db;
      /*
       * Make sure we can open catalog, otherwise print a warning
       * message because the server is probably not running.
       */
      db = db_init_database(NULL,
                            catalog->db_driver,
                            catalog->db_name,
                            catalog->db_user,
                            catalog->db_password.value,
                            catalog->db_address,
                            catalog->db_port,
                            catalog->db_socket,
                            catalog->mult_db_connections,
                            catalog->disable_batch_insert);
      if (!db || !db_open_database(NULL, db)) {
         Pmsg2(000, _("Could not open Catalog \"%s\", database \"%s\".\n"),
              catalog->name(), catalog->db_name);
         Jmsg(NULL, M_FATAL, 0, _("Could not open Catalog \"%s\", database \"%s\".\n"),
              catalog->name(), catalog->db_name);
         if (db) {
            Jmsg(NULL, M_FATAL, 0, _("%s"), db_strerror(db));
            Pmsg1(000, "%s", db_strerror(db));
            db_close_database(NULL, db);
         }
         OK = false;
         continue;
      }

      /* Display a message if the db max_connections is too low */
      if (!db_check_max_connections(NULL, db, me->MaxConcurrentJobs)) {
         Pmsg1(000, "Warning, settings problem for Catalog=%s\n", catalog->name());
         Pmsg1(000, "%s", db_strerror(db));
      }

      /* we are in testing mode, so don't touch anything in the catalog */
      if (mode == CHECK_CONNECTION) {
         db_close_database(NULL, db);
         continue;
      }

      /* Loop over all pools, defining/updating them in each database */
      POOLRES *pool;
      foreach_res(config, pool, R_POOL) {
         /*
          * If the Pool has a catalog resource create the pool only
          *   in that catalog.
          */
         if (!pool->catalog || pool->catalog == catalog) {
            create_pool(NULL, db, pool, POOL_OP_UPDATE);  /* update request */
         }
      }

      /* Once they are created, we can loop over them again, updating
       * references (RecyclePool)
       */
      foreach_res(config, pool, R_POOL) {
         /*
          * If the Pool has a catalog resource update the pool only
          *   in that catalog.
          */
         if (!pool->catalog || pool->catalog == catalog) {
            update_pool_references(NULL, db, pool);
         }
      }

      /* Ensure basic client record is in DB */
      CLIENTRES *client;
      foreach_res(config, client, R_CLIENT) {
         CLIENT_DBR cr;
         /* Create clients only if they use the current catalog */
         if (client->catalog != catalog) {
            Dmsg3(500, "Skip client=%s with cat=%s not catalog=%s\n",
                  client->name(), client->catalog->name(), catalog->name());
            continue;
         }
         Dmsg2(500, "create cat=%s for client=%s\n",
               client->catalog->name(), client->name());
         memset(&cr, 0, sizeof(cr));
         bstrncpy(cr.Name, client->name(), sizeof(cr.Name));
         db_create_client_record(NULL, db, &cr);
      }

      /* Ensure basic storage record is in DB */
      STORERES *store;
      foreach_res(config, store, R_STORAGE) {
         STORAGE_DBR sr;
         MEDIATYPE_DBR mtr;
         memset(&sr, 0, sizeof(sr));
         memset(&mtr, 0, sizeof(mtr));
         if (store->media_type) {
            bstrncpy(mtr.MediaType, store->media_type, sizeof(mtr.MediaType));
            mtr.ReadOnly = 0;
            db_create_mediatype_record(NULL, db, &mtr);
         } else {
            mtr.MediaTypeId = 0;
         }
         bstrncpy(sr.Name, store->name(), sizeof(sr.Name));
         sr.AutoChanger = store->autochanger;
         if (!db_create_storage_record(NULL, db, &sr)) {
            Jmsg(NULL, M_FATAL, 0, _("Could not create storage record for %s\n"),
                 store->name());
            OK = false;
         }
         store->StorageId = sr.StorageId;   /* set storage Id */
         if (!sr.created) {                 /* if not created, update it */
            sr.AutoChanger = store->autochanger;
            if (!db_update_storage_record(NULL, db, &sr)) {
               Jmsg(NULL, M_FATAL, 0, _("Could not update storage record for %s\n"),
                    store->name());
               OK = false;
            }
         }
      }

      /* Loop over all counters, defining them in each database */
      /* Set default value in all counters */
      COUNTERRES *counter;
      foreach_res(config, counter, R_COUNTER) {
         /* Write to catalog? */
         if (!counter->created && counter->Catalog == catalog) {
            COUNTER_DBR cr;
            bstrncpy(cr.Counter, counter->name(), sizeof(cr.Counter));
            cr.MinValue = counter->MinValue;
            cr.MaxValue = counter->MaxValue;
            cr.CurrentValue = counter->MinValue;
            if (counter->WrapCounter) {
               bstrncpy(cr.WrapCounter, counter->WrapCounter->name(), sizeof(cr.WrapCounter));
            } else {
               cr.WrapCounter[0] = 0;  /* empty string */
            }
            if (db_create_counter_record(NULL, db, &cr)) {
               counter->CurrentValue = cr.CurrentValue;
               counter->created = true;
               Dmsg2(100, "Create counter %s val=%d\n", counter->name(), counter->CurrentValue);
            }
         }
         if (!counter->created) {
            counter->CurrentValue = counter->MinValue;  /* default value */
         }
      }
      /* cleanup old job records */
      if (mode == UPDATE_AND_FIX) {
         db_sql_query(db, cleanup_created_job);
         db_sql_query(db, cleanup_running_job);
      }

      /* Set type in global for debugging */
      set_db_type(db_get_type(db));

      db_close_database(NULL, db);
   }

   return OK;
}

static void cleanup_old_files()
{
   DIR* dp;
   struct dirent *entry, *result;
   int rc, name_max;
   int my_name_len = strlen(my_name);
   int len = strlen(me->working_directory);
   POOLMEM *cleanup = get_pool_memory(PM_MESSAGE);
   POOLMEM *basename = get_pool_memory(PM_MESSAGE);
   regex_t preg1;
   char prbuf[500];
   const int nmatch = 30;
   regmatch_t pmatch[nmatch];
   berrno be;

   /* Exclude spaces and look for .mail or .restore.xx.bsr files */
   const char *pat1 = "^[^ ]+\\.(restore\\.[^ ]+\\.bsr|mail)$";

   /* Setup working directory prefix */
   pm_strcpy(basename, me->working_directory);
   if (len > 0 && !IsPathSeparator(me->working_directory[len-1])) {
      pm_strcat(basename, "/");
   }

   /* Compile regex expressions */
   rc = regcomp(&preg1, pat1, REG_EXTENDED);
   if (rc != 0) {
      regerror(rc, &preg1, prbuf, sizeof(prbuf));
      Pmsg2(000,  _("Could not compile regex pattern \"%s\" ERR=%s\n"),
           pat1, prbuf);
      goto get_out2;
   }

   name_max = pathconf(".", _PC_NAME_MAX);
   if (name_max < 1024) {
      name_max = 1024;
   }

   if (!(dp = opendir(me->working_directory))) {
      berrno be;
      Pmsg2(000, "Failed to open working dir %s for cleanup: ERR=%s\n",
            me->working_directory, be.bstrerror());
      goto get_out1;
      return;
   }

   entry = (struct dirent *)malloc(sizeof(struct dirent) + name_max + 1000);
   while (1) {
      if ((readdir_r(dp, entry, &result) != 0) || (result == NULL)) {
         break;
      }
      /* Exclude any name with ., .., not my_name or containing a space */
      if (strcmp(result->d_name, ".") == 0 || strcmp(result->d_name, "..") == 0 ||
          strncmp(result->d_name, my_name, my_name_len) != 0) {
         Dmsg1(500, "Skipped: %s\n", result->d_name);
         continue;
      }

      /* Unlink files that match regexes */
      if (regexec(&preg1, result->d_name, nmatch, pmatch,  0) == 0) {
         pm_strcpy(cleanup, basename);
         pm_strcat(cleanup, result->d_name);
         Dmsg1(100, "Unlink: %s\n", cleanup);
         unlink(cleanup);
      }
   }

   free(entry);
   closedir(dp);
/* Be careful to free up the correct resources */
get_out1:
   regfree(&preg1);
get_out2:
   free_pool_memory(cleanup);
   free_pool_memory(basename);
}
