/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
#include "spawn.h"
#include "list.h"
extern char **environ;
static void handle_child_status(pid_t pid, int status);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    pid_t pgid;              /* this is the process group id*/
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
    struct list pid;
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];
    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}



static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

    //  struct job* newJobStructure = get_job_from_jid(pid);
    for (struct list_elem* var = list_begin(&job_list); var != list_end(&job_list); var = list_next(var)) {
     struct job* newJobStructure = list_entry(var, struct job, elem);
   
// Checks to see if process was stopped by a signal
    if (WIFSTOPPED(status)) {

        if (WSTOPSIG(status) == SIGTSTP || WSTOPSIG(status) == SIGSTOP) {
            newJobStructure->status = STOPPED;
            // newJobStructure->num_processes_alive--;
            //  termstate_save(newJobStructure);
            //   termstate_give_terminal_back_to_shell();
            
        }
        // else if () {
        //     newJobStructure->status = STOPPED;
        //     newJobStructure->num_processes_alive--;
        //      termstate_save(newJobStructure);
        //       termstate_give_terminal_back_to_shell();

        // }
        else if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN){
             newJobStructure->status = NEEDSTERMINAL;
             
            //   termstate_give_terminal_back_to_shell();
        }
        termstate_save(&newJobStructure->saved_tty_state);
    }
// Checks to see if the process is terminated normally 
    else if (WIFEXITED(status) || WIFSIGNALED(status)) {
        newJobStructure->num_processes_alive--;
        if (newJobStructure->status == FOREGROUND) {
            termstate_sample();
        }
        int term_sig = WTERMSIG(status);
        if (term_sig == SIGKILL || term_sig == SIGTERM) {
            if (newJobStructure->status == FOREGROUND)
                printf("Killed\n");
        }else if (term_sig == SIGFPE && newJobStructure->status == FOREGROUND) {
                printf("Floating Point exception\n");
        }else if (term_sig == SIGSEGV && newJobStructure->status == FOREGROUND) {
                printf("Segmentation Fault\n");
        }else if (term_sig == SIGABRT && newJobStructure->status == FOREGROUND) {
                printf("Aborted\n");
        }
    }
    else {
        printf("Unknown child stats\n");
    }
    // Checks to see if the process was terminated by signal 
    // else if (WIFSIGNALED(status)) {
    //     if (WTERMSIG(status) == SIGINT) {
    //         newJobStructure->status = FOREGROUND;
    //         termstate_save(newJobStructure);
    //         newJobStructure->num_processes_alive--;
    //           termstate_give_terminal_back_to_shell();
    //     }
    //     else if (WTERMSIG(status) ==SIGTERM) {
            
    //     }
    //     else if (WTERMSIG(status) == SIGKILL) {

    //     }
    //     else {

    //     }
    // }
    }

}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }
    
    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;) {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);
    
        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        
        (ast_command_line_print(cline));
        free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }
        
   
    list_front(&cline->pipes);
// This is where the loop starts to find the first command passed in from the cush
// We loop through the pipeline to find the fist command and check to see if the command is a built in or requires posix spawn
    struct list_elem *e;

      for (e = list_begin (&cline->pipes); e != list_end (&cline->pipes); e = list_next (e))
        {
             struct ast_pipeline *pipee = list_entry (e, struct ast_pipeline, elem);
             struct list_elem *a;
             //add job here
            struct job* curJob = add_job(pipee);
            if (curJob->pipe->bg_job == true) {
                curJob->status = BACKGROUND;
            }else {
                curJob->status = FOREGROUND;

            }
            for (a = list_begin (&pipee->commands); a != list_end (&pipee->commands); a = list_next (a)) {
            struct ast_command *command = list_entry(a,  struct ast_command, elem);
            
            
                if (strcmp(command->argv[0], "exit") == 0) {
                    exit(0);
                }
                else if (strcmp(command->argv[0], "jobs") == 0) {
                    pid_t id;
                    int stat;

                    while ((id = waitpid(-1, &stat, WNOHANG)) > 0) {

                        if (WIFSIGNALED(stat)) {
                            printf("This was the cause: %d\n", WTERMSIG(stat));
                        }
                        else if (WIFEXITED(stat)) {
                            printf("This was the cause: %d\n",WEXITSTATUS(stat));
                        }
                        else if (WIFSTOPPED(stat)) {
                            printf("This was the cause: %d\n", WSTOPSIG(stat));
                        }
                        else {
                            printf("No errors");
                        }
                    }
                
                }
                else if (strcmp(command->argv[0], "bg") == 0) {
                    //SIGCONT singal will bring the process back to the foreground, bringing it back to a running state??
                    // Crtl + Z will give a SIGTSTP singal to stop the process
                    // Are we suppose to use the kill command in this function? 
                    // running in background and stop is not runnning at all
                    // changing the status of the job and continuing but in stop you would send the stop signal 

                }
                else if (strcmp(command->argv[0], "fg") == 0) {
                    // Do I have to make a new job struct here?
                    // need to recover the job structure 
                    pid_t id = atoi(command->argv[1]);
                    // receive in a struct variable 
                    // if (id == NULL) {
                    // printf("Error with the id passed in.");
                    // }
                    if (jid2job[id] == NULL) {
                        printf("JOB DOESNT EXIT\n");
                    }
                    else {
                         struct job* sjob = jid2job[id];
                    // need to access the saved tty state to get terimal 
                    // NEed a couple of check before running this, flag syntax 
                   // pid_t pid = sjob->jid;
                    //pid_t pgid = getpgid(pid);
                    if (sjob == NULL) {
                        printf("Error error");
                    } 
                    else{
                        sjob->status = FOREGROUND;
                    
                        
                    termstate_give_terminal_to(&sjob->saved_tty_state, sjob->pgid);

                    }
                   wait_for_job(sjob);
                    }
                     
                }
                // else if () {

                // }
                // else if () {

                // }
 
                else {
                   posix_spawn_file_actions_t child_file_attr;
                   posix_spawnattr_t child_spawn_attr;
                   //posix_spawnattr_init(&child_file_attr);
                    posix_spawnattr_init(&child_spawn_attr);
                   posix_spawn_file_actions_init(&child_file_attr);
                    //posix_spawnattr_setflags // flags will defer depending on if the job is foreground or background
                    // if its a foreground setpgroup and tcsetgroup
                    //posix_spawnattr_tc

                    //posix spawnattr tcsetpgrp np; use this to give child terminal access 
                    // takes two different agrumetnts, child and file descriptor look for termstat state tty ft only for foreground
    
                    posix_spawnattr_setpgroup(&child_spawn_attr, 0); // not sure if I did this right
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK);
                   // need to incremrment when process is sucessfull
                    pid_t pid; // 0
                    if (posix_spawnp(&pid, command->argv[0],&child_file_attr, &child_spawn_attr, command->argv, environ ) == 0) {
                        //printf("Here is the child pid: %d", (int)pid);
                          if (curJob->status == BACKGROUND) {
                        printf("BG job detected\n");
                    }
                    else if (curJob->status == FOREGROUND){
                         printf("FG job detected\n");
                         posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
                         
                        posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP);
                        signal_block(SIGCHLD);
                        wait_for_job(curJob);
                        signal_unblock(SIGCHLD);
                        if (posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP)) {
            utils_fatal_error(
                "POSIX_SPAWN_TCSETPGROUP flag could not be set\n");
        }
                        // termstate_give_terminal_back_to_shell();
                    }
                    else {
                        printf("ERROR!! within spawn status");
                    }
                    }
                    // pid == 18975;
                    else {
                        printf("ERROR!! Error with posix_spawn");
                    }


                    //  printf("Group id: %d", gpid);
                    // int pipefd[2];
                    // pipefd[0]; //is the read end of the file
                    // pipefd[1]; //is the write end of the file
                    // if (pipe(pipefd) == -1) {
                    //     perror("Error with the pipe!");
                    //     exit(EXIT_FAILURE);
                    // }

                    // posix_spawn_file_actions_addclose();
                    // posix_spawn_file_actions_adddup2();
                   // printf("Here is the pid: %d\n", pid);
                    // struct job* pjob = get_job_from_jid(pid);
                    
                  

                    //handle child next
                    // if (pjob == NULL) {
                    //     printf("Erroe");
                    // }
                    // else {
                    //     printf("You did it right!");
                    // }
                
                   // wait_for_job(pjob);
                    
                    
                    // need to add to job list 
                    //wait for job at some point
                   
                    
                    // wiritng up  multiple pipe

                // if (pid == 0) {

                // }
                // need to store the status if it is 0
                // if it is a bg process you would give terminal back to process 
                // fg wait_for_job will wait for that specific job to be completed
                // need to set pgroup value, depends on pip
                // need to give it terminal access...
                }
            } 
            
          

        }
    
        //taking input from shell and printing it back, so from here you want to write logic of spawning a new process and handeling it down
        //ast_command_line_run
        //assigning process group ids, assigning pipes 
        //lots of built ins as well, gcc based function or kill,...
        //not spawn child for specific command kill, jobs, come a bit later in the code with just a if else check
        //keep track of pid in array 
        //first command redirect standard out when it is a regular command
       // ast_command_line_print(cline);      /* Output a representation of
                                               //the entered command line */
        

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        ast_command_line_free(cline);
    }
    
    return 0;
}
// static struct job *  get_job_from_pid(pid_t pid) {
//     struct list_elem *i;
    
    
//     for (i = list_begin(&job_list); i != list_end(&job_list); i = list_next(i)) {
//        // jid2job[MAXJOBS]->pipe;

//         struct list_elem *e;

//         for (e = list_begin(&jid2job[MAXJOBS]); i != list_end(&jid2job[MAXJOBS]); i = list_next(e)) {
                

//         }

//     }
// }

// for (e = list_begin (&cline->pipes); e != list_end (&cline->pipes); e = list_next (e))
//         {
            //  struct ast_pipeline *pipee = list_entry (e, struct ast_pipeline, elem);
            //  struct list_elem *a;
            //  //add job here
            // struct job* curJob = add_job(pipee);
            // if (curJob->pipe->bg_job == true) {
            //     curJob->status = BACKGROUND;
            // }else {
            //     curJob->status = FOREGROUND;

            // }
            // for (a = list_begin (&pipee->commands); a != list_end (&pipee->commands); a = list_next (a)) {
            // struct ast_command *command = list_entry(a,  struct ast_command, elem);
            

        // need to loop through job list
        // then need to loop jid2job 
        // Need to compare and see if pid passed in equals to job2jobs
        // then update the list in jobs 
        // return the jobs struct 
    

//work on exit first; work on ls, wc(posix_spawn) second (hold of on duping the pipes); expect user to only type on command at a type
//access through the strut, ast_command_line 

// void kill (pid_t pid, int sig) {
// do I have to create a new job struct?
// if (pid > 0) {
//    // signal(SIGTERM, );

// }
// else if (pid == 0) {

// }
// else if (pid < -1) {

// }
// else {
    
// }


// }