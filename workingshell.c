/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name and login ID here>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>



/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;
char prompt[] = "tsh> ";    
int verbose = 0;           
int nextjid = 1;          
char sbuf[MAXLINE];

struct job_t {           
    pid_t pid;            
    int jid;
    int state;
    char cmdline[MAXLINE];
};
struct job_t jobs[MAXJOBS];

/* Function prototypes */

void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);


int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1;

    dup2(1, 2);


    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':            
            usage();
	    break;
        case 'v':           
            verbose = 1;
	    break;
        case 'p':           
            emit_prompt = 0; 
	    break;
	default:
            usage();
	}
    }



    signal(SIGINT,  sigint_handler);   // ctrl-c
    signal(SIGTSTP, sigtstp_handler);  // ctrl-z 
    signal(SIGCHLD, sigchld_handler);  //

    //A clean way to kill the shell */
    signal(SIGQUIT, sigquit_handler); 

    // Initialize the job list
    initjobs(jobs);

    // Execute the shell's read, eval, print loop
    while (1) {
	// Reads command line
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
		
	if (feof(stdin)) {
        // End of file (ctrl-d)
	    fflush(stdout);
	    exit(0);
	}

	// Evaluates the command line
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0);
}
  
/* 
  Evaluate the command line that the user has just typed in
  
  If the user has requested a built-in command (quit, jobs, bg or fg)
  then execute it immediately. Otherwise, forks a child process and
  run the job in the context of the child. If the job is running in
  the foreground, waits for it to terminate and then returns.
*/
void eval(char *cmdline) 
{
  char *argv[MAXARGS]; 
  char buf[MAXLINE];
  int bg;
  pid_t pid;

  // Signal mask
  sigset_t mask_all, mask_one, prev_one; 
  sigfillset(&mask_all);
  sigemptyset(&mask_one);
  sigaddset(&mask_one, SIGCHLD);

  strcpy(buf, cmdline);
  bg = parseline(buf, argv); 
  if (argv[0] == NULL)  
    return;

  if (!builtin_cmd(argv)) {
    sigprocmask(SIG_BLOCK, &mask_one, &prev_one);
    if ((pid = fork()) == 0) {
      sigprocmask(SIG_SETMASK, &prev_one, NULL);
      setpgid(0, 0);
      if (execve(argv[0], argv, environ) < 0) {
        printf("%s: Command not found\n", argv[0]);
        exit(0);
      }
    }
    if (!bg) {
        // job runs in the foreground    
      sigprocmask(SIG_BLOCK, &mask_all, NULL);
      addjob(jobs, pid, FG, cmdline);
      sigprocmask(SIG_SETMASK, &prev_one, NULL);
      waitfg(pid);
    }
    else {
        // job runs in the background
      sigprocmask(SIG_BLOCK, &mask_all, NULL);
      addjob(jobs, pid, BG, cmdline);
      printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
      sigprocmask(SIG_SETMASK, &prev_one, NULL);
    }      
  }
  return;
}


int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE];
    char *buf = array;
    char *delim;
    int argc;
    int bg;

    strcpy(buf, cmdline);
    // ignore trailing and leading white spaces
    buf[strlen(buf)-1] = ' ';
    while (*buf && (*buf == ' '))
	buf++;

    // Build the argv list
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
    // ignore white spaces
	while (*buf && (*buf == ' '))
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    // checks if bg job
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

// A list of build in commands supported
// these are executed immediatly

int builtin_cmd(char **argv) 
{
  if (!strcmp(argv[0], "quit"))
    exit(0);
  if (!strcmp(argv[0], "jobs")) {
    listjobs(jobs);
    return 1;
  }
  if (!strcmp(argv[0], "&"))
    return 1;
  return 0;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
  while (pid == fgpid(jobs))
    sleep(1);
  return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
  int olderrno = errno;
  sigset_t mask_all, prev_all;
  pid_t pid;
  int status;
  
  sigfillset(&mask_all);
  while ((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) {
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block Signals*/
    if (WIFEXITED(status)) {    /* Exit normally */
      deletejob(jobs, pid);
    }
    if (WIFSIGNALED(status)) {  /* C-c SIGINT */
      printf("Job [%d] (%d) terminated by signal 2\n", pid2jid(pid), pid);
      deletejob(jobs, pid);     /* Note: printf first, then deletejob */
    }
    if (WIFSTOPPED(status)) {   /* C-z SIGTSTP */
      printf("Job [%d] (%d) stopped by signal 20\n", pid2jid(pid), pid);
      getjobpid(jobs, pid)->state = ST;
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL); /* Unblock Signals*/
  }

  errno = olderrno;
  return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
  int olderrno = errno;
  pid_t pid = fgpid(jobs);
  if (pid != 0)
    kill(-pid, SIGINT);
  errno = olderrno;
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
  int olderrno = errno;
  pid_t pid = fgpid(jobs);
  if (pid != 0)
    kill(-pid, SIGTSTP);
  errno = olderrno;
  return;
}


/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

// clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

// initjobs - Initialize the job list
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

// maxjid - Returns largest allocated job ID
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

// addjob - Add a job to the job list
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

// deletejob - Deletes a job whose PID=pid from the job list
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

// fgpid - Returns PID of current foreground job, 0 if no such job
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

// getjobpid  - Finds a job (by PID) on the job list
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

// getjobjid  - Finds a job (by JID) on the job list
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

// pid2jid - Maps process ID to job ID
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

// listjobs - Print the job list
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}


// usage - print a help message
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}



// sigquit_handler - The driver program can gracefully terminate the
//    child shell by sending it a SIGQUIT signal.
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

