/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
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
static void exe_pipelines(struct ast_pipeline *pipee);
static void non_built_in(struct ast_pipeline *pipee, struct ast_command *command);

static void usage(char *progname)
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

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
    DONE,          /* job is exited normally*/
    DELETE,        /*job should be deleted*/
};

struct job
{
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    pid_t pgid;                     /* this is the process group id*/
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */

    /* Add additional fields here if needed. */
    struct PIDs *PID_list; // list of PIDs that a job has
};

/**
 * Struct for an array of PID that a job has
 */
struct PIDs
{
    pid_t *data;      // array of PID
    size_t size;      // max amount of PID
    size_t curr_size; // number of PID
};

static void print_PIDs(struct PIDs *const pPIDs)
{
    printf("THE PIDS ARE\n");
    for (size_t i = 0; i < pPIDs->curr_size; i++)
    {
        printf("%d ", pPIDs->data[i]);
    }
    printf("\n");
}

/**
 * Create PIDs List
 */
static struct PIDs *create_PIDs(size_t cap)
{
    struct PIDs *pPIDs = calloc(1, sizeof(struct PIDs));
    pPIDs->size = cap;
    pPIDs->curr_size = 0;
    pPIDs->data = calloc(cap + 1, sizeof(pid_t));

    return pPIDs;
}

/**
 * Add a pid to a PID List
 */
static void add_PID(struct PIDs *const pPIDs, pid_t pid)
{
    // printf("ADDING A NEW PID at index %ld   %d\n", pPIDs->curr_size, pid);
    pPIDs->data[pPIDs->curr_size] = pid;
    pPIDs->curr_size++;
    // printf("RESULT at index %ld   %d\n", pPIDs->curr_size - 1, pPIDs->data[pPIDs->curr_size - 1]);
}

/**
 * See if a PID is in the PID List
 */
static bool find_PID(struct PIDs *const pPIDs, pid_t pid)
{
    for (size_t i = 0; i < pPIDs->curr_size; i++)
    {
        if (pPIDs->data[i] == pid)
        {
            return true;
        }
    }

    return false;
}
/**
 * Clean the PID
 */
static void clean_PID(struct PIDs *const pPIDs)
{
    free(pPIDs->data);
    free(pPIDs);
}

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];
    return NULL;
}

/* Add a new job to the job list */
static struct job *add_job(struct ast_pipeline *pipe)
{
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
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
static void delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    clean_PID(job->PID_list);
    free(job);
}

static const char *get_status(enum job_status status)
{
    switch (status)
    {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    case DONE:
        return "Done";
    case DELETE:
        return "";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
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
    if (job->status != DONE)
    {
        printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
        print_cmdline(job->pipe);
        printf(")\n");
    }
}

static void delete_done_jobs()
{
    for (struct list_elem *var = list_begin(&job_list); var != list_end(&job_list);)
    {
        struct job *j = list_entry(var, struct job, elem);
        if (j->status == DELETE)
        {
            list_remove(var);
            delete_job(j);
        }
        if (j->status == DONE)
        {
            j->status = DELETE;
        }
        list_next(var);
    }
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

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
        handle_child_status(child, status);
    }

    // DELETE HERE FOR KILL BUILT IN
    for (struct list_elem *e = list_begin(&job_list); e != list_end(&job_list);)
    {
        struct job *j = list_entry(e, struct job, elem);

        if (j->status == DELETE)
        {
            e = list_remove(e);
            delete_job(j);
        }
        else
        {
            e = list_next(e);
        }
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

    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {
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

    for (struct list_elem *var = list_begin(&job_list); var != list_end(&job_list); var = list_next(var))
    {
        struct job *sjob = list_entry(var, struct job, elem);

        if (!find_PID(sjob->PID_list, pid))
        {
            continue;
        }

        // Checks to see if process was stopped by a signal
        // ctrl z
        if (WIFSTOPPED(status))
        {

            if (WSTOPSIG(status) == SIGTSTP || WSTOPSIG(status) == SIGSTOP)
            {
                sjob->status = STOPPED;
                print_job(sjob);
            }
            else if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN)
            {
                sjob->status = NEEDSTERMINAL;
            }
            termstate_save(&sjob->saved_tty_state);
        }
        // Checks to see if the process is terminated normally

        // process exits via exit()
        else if (WIFEXITED(status))
        {
            sjob->num_processes_alive--;

            if (sjob->status == FOREGROUND && sjob->num_processes_alive == 0)
            {
                sjob->status = DELETE;
                termstate_sample();
            }
            // job is 100% complete here
            if (sjob->num_processes_alive == 0 && sjob->status == BACKGROUND)
            {
                sjob->status = DONE;
            }
        }
        // signal
        else if (WIFSIGNALED(status))
        {
            // sjob->num_processes_alive--;
            // if (sjob->num_processes_alive == 0)
            // {
            //     sjob->status = DELETE;
            // }
            sjob->status = DELETE;

            int term_sig = WTERMSIG(status);
            printf("%s\n", strsignal(term_sig));
        }
        else
        {
            printf("Unknown child stats\n");
        }
    }
}

int main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;)
    {

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
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free(prompt);

        if (cmdline == NULL) /* User typed EOF */
            break;

        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        if (cline == NULL) /* Error in command line */
            continue;

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        /*
        =====================================================================================================
        HANDLE HISTORY HERE
        =====================================================================================================
        */

        using_history();
        char *historyElem;

        int result = history_expand(cmdline, &historyElem);
        if (result)
        {
            fprintf(stderr, "%s\n", historyElem);
            ast_command_line_free(cline);
            cline = ast_parse_command_line(historyElem);
        }

        if (result < 0 || result == 2)
        {
            free(historyElem);
            continue;
        }

        add_history(historyElem);

        /*
        =====================================================================================================
        HANDLE COMMAND LINE HERE
        =====================================================================================================
        */
        if (signal_block(SIGCHLD))
        {
            utils_error("Error blocking SIGCHLD");
        }
        list_front(&cline->pipes);

        // loop through the command line and execute the different pipes
        for (struct list_elem *e = list_begin(&cline->pipes); e != list_end(&cline->pipes);)
        {

            struct ast_pipeline *pipee = list_entry(e, struct ast_pipeline, elem);

            e = list_remove(e);
            exe_pipelines(pipee);
        }

        if (!signal_unblock(SIGCHLD))
        {
            utils_error("Error blocking SIGCHLD");
        }

        for (struct list_elem *e = list_begin(&job_list); e != list_end(&job_list);)
        {
            struct job *j = list_entry(e, struct job, elem);

            if (j->status == DONE)
            {

                printf("[%d]\t%s\n", j->jid, get_status(j->status));

                j->status = DELETE;
            }

            if (j->status == DELETE)
            {
                e = list_remove(e);
                delete_job(j);
            }
            else
            {
                e = list_next(e);
            }
        }
        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        free(cmdline);
        free(historyElem);
        free(cline);
    }

    return 0;
}

static void exe_pipelines(struct ast_pipeline *pipee)
{
    // TODO: free ast_pipeline *pipee after a built in command is executed
    // find the fist command and check to see if the command is a built in or requires posix spawn
    struct list_elem *a = list_begin(&pipee->commands);
    struct ast_command *command = list_entry(a, struct ast_command, elem);

    if (strcmp(command->argv[0], "exit") == 0)
    {
        exit(0);
    }
    else if (strcmp(command->argv[0], "jobs") == 0)
    {
        for (struct list_elem *i = list_begin(&job_list); i != list_end(&job_list); i = list_next(i))
        {
            struct job *job_entry = list_entry(i, struct job, elem);
            print_job(job_entry);
        }
        ast_pipeline_free(pipee);
    }
    else if (strcmp(command->argv[0], "bg") == 0)
    {
        // SIGCONT singal will bring the process back to the foreground, bringing it back to a running state??
        //  Crtl + Z will give a SIGTSTP singal to stop the process
        //  Are we suppose to use the kill command in this function?
        //  running in background and stop is not runnning at all
        //  changing the status of the job and continuing but in stop you would send the stop signal
        pid_t id = atoi(command->argv[1]);

        if (jid2job[id] == NULL)
        {
            printf("JOB DOESNT EXIST\n");
        }
        else
        {
            struct job *sjob = jid2job[id];

            if (sjob == NULL)
            {
                printf("Error error");
            }
            else if (sjob->jid == id)
            {
                if (sjob->status == BACKGROUND)
                {
                    printf("already bg\n");
                }
                else
                {
                    sjob->status = BACKGROUND;
                    killpg(sjob->pgid, SIGCONT);
                    printf("[%d] %d\n", sjob->jid, sjob->pgid);
                }
            }
        }
        ast_pipeline_free(pipee);
    }
    else if (strcmp(command->argv[0], "fg") == 0)
    {
        pid_t id = atoi(command->argv[1]);

        if (jid2job[id] == NULL)
        {
            printf("JOB DOESNT EXIST\n");
        }
        else
        {
            struct job *sjob = jid2job[id];

            if (sjob == NULL)
            {
                printf("Error error");
            }
            else if (sjob->jid == id)
            {
                struct termios *state = NULL;
                if (sjob->status != BACKGROUND)
                {
                    state = &sjob->saved_tty_state;
                }
                sjob->status = FOREGROUND;
                print_cmdline(sjob->pipe);
                printf("\n");

                termstate_give_terminal_to(state, sjob->pgid);
                if (killpg(sjob->pgid, SIGCONT))
                {
                    utils_error("Error sending SIGCONT in fg");
                }
                wait_for_job(sjob);
                termstate_give_terminal_back_to_shell();
            }
        }
        ast_pipeline_free(pipee);
    }
    else if (strcmp(command->argv[0], "stop") == 0)
    {
        pid_t id = atoi(command->argv[1]);

        if (jid2job[id] == NULL)
        {
            printf("JOB DOESNT EXIST\n");
        }
        else
        {
            struct job *sjob = jid2job[id];

            if (sjob == NULL)
            {
                printf("Error error");
            }
            else if (sjob->jid == id)
            {
                sjob->status = STOPPED;
                if (killpg(sjob->pgid, SIGSTOP))
                {
                    utils_error("Error dending SIGSTOP in stop");
                }
                termstate_give_terminal_back_to_shell();
            }
        }
        ast_pipeline_free(pipee);
    }
    else if (strcmp(command->argv[0], "kill") == 0)
    {
        pid_t id = atoi(command->argv[1]);

        if (jid2job[id] == NULL)
        {
            printf("JOB DOESNT EXIST\n");
        }
        else
        {
            struct job *sjob = jid2job[id];

            if (sjob == NULL)
            {
                printf("Error error");
            }
            else if (sjob->jid == id)
            {
                if (killpg(sjob->pgid, SIGKILL))
                {
                    utils_error("Error sending SIGKILL in kill");
                }
                termstate_give_terminal_back_to_shell();
            }
        }
        ast_pipeline_free(pipee);
    }
    else if (strcmp(command->argv[0], "cd") == 0)
    {
        char *path = command->argv[1];
        if (!path)
        {
            path = getenv("HOME");
        }
        if (chdir(path) == -1)
        {
            utils_error("cd: %s: No such file or directory\n", path);
        }
        ast_pipeline_free(pipee);
    }
    else if (strcmp(command->argv[0], "history") == 0)
    {
        // Display the command history
        HIST_ENTRY **histList = history_list();
        int history_len = history_length;

        for (int i = 0; i < history_len; i++)
        {
            printf("  %d %s\n", i + 1, histList[i]->line);
        }
        ast_pipeline_free(pipee);
    }

    else
    {
        non_built_in(pipee, command);
    }
}

/**
 * Handles non built in commands given to the command line
 */
static void non_built_in(struct ast_pipeline *pipee, struct ast_command *command)
{
    struct job *cur_job = add_job(pipee);

    posix_spawn_file_actions_t child_file_attr;
    posix_spawnattr_t child_spawn_attr;

    if (posix_spawn_file_actions_init(&child_file_attr))
    {
        utils_error("Error initializing child file attr");
    }
    if (posix_spawnattr_init(&child_spawn_attr))
    {
        utils_error("Error initializing child spawn attr");
    }

    // posix_spawnattr_setflags // flags will defer depending on if the job is foreground or background
    //  if its a foreground setpgroup and tcsetgroup
    // posix_spawnattr_tc

    // posix spawnattr tcsetpgrp np; use this to give child terminal access
    //  takes two different agrumetnts, child and file descriptor look for termstat state tty ft only for foreground

    // new process sets gpid as its own pid
    pid_t gpid;
    if (posix_spawnattr_setpgroup(&child_spawn_attr, 0))
    {
        utils_error("Error storing child spawn attr pgroup");
    }

    // set up for foreground process
    if (!pipee->bg_job)
    {
        if (posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd()))
        {
            utils_error("Error in terminal access setup");
        }

        if (posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK | POSIX_SPAWN_TCSETPGROUP))
        {
            utils_error("Error could not set proper flags for child spawn attr");
        }
        cur_job->status = FOREGROUND;
    }
    else
    {
        if (posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK))
        {
            utils_error("Error could not set proper flags for child spawn attr");
        }
        cur_job->status = BACKGROUND;
    }

    // Manages of the input pipe
    if (pipee->iored_input)
    {
        if (posix_spawn_file_actions_addopen(&child_file_attr, fileno(stdin), pipee->iored_input, O_RDWR, 0666))
        {
            utils_error("Error chould not open child file arrt I/O");
        }
    }

    // set up pipes
    // get the number of pipes
    int num_pipes = list_size(&pipee->commands) - 1;
    if (num_pipes == 0)
    {
        cur_job->PID_list = create_PIDs(1);
    }
    else
    {
        cur_job->PID_list = create_PIDs(num_pipes);
    }

    // create an array for the pipes
    int *pipe_array = NULL;
    int index = 0;
    // if needed to pipe, wire stdin and stdout
    if (num_pipes != 0)
    {
        pipe_array = calloc((num_pipes)*2, sizeof(int));

        pipe2(&pipe_array[0], O_CLOEXEC);

        if (posix_spawn_file_actions_adddup2(&child_file_attr, pipe_array[1], fileno(stdout)))
        {
            utils_error("Error calling dup2 on file descriptors");
        }
        if (command->dup_stderr_to_stdout)
        {
            if (posix_spawn_file_actions_adddup2(&child_file_attr, pipe_array[1], fileno(stderr)))
            {
                utils_error("Error calling dup2 on file descriptors");
            }
        }
        index++;
    }
    // check to see if the pipe is null or only has one command. sets up pipes demendind on the current pipe state
    else if (pipee->iored_output)
    {
        int term;
        if (pipee->append_to_output)
        {
            term = O_APPEND;
        }
        else
        {
            term = O_TRUNC;
        }
        if (posix_spawn_file_actions_addopen(&child_file_attr, fileno(stdout), pipee->iored_output, O_WRONLY | term | O_CREAT, 0666))
        {
            utils_error("Error chould not open child file arrt I/O");
        }

        if (command->dup_stderr_to_stdout)
        {
            if (posix_spawn_file_actions_adddup2(&child_file_attr, fileno(stdout), fileno(stderr)))
            {
                utils_error("Error calling dup2 on file descriptors");
            }
        }
    }

    // create the first process, this is considered the gpid
    // if successful, update job
    if (posix_spawnp(&gpid, command->argv[0], &child_file_attr, &child_spawn_attr, command->argv, environ) != 0)
    {
        utils_error("%s: No such file or directory\n", command->argv[0]);
        // clean the parent process attr and file
        posix_spawn_file_actions_destroy(&child_file_attr);
        posix_spawnattr_destroy(&child_spawn_attr);

        // close the parent process pipes
        if (pipe_array != NULL)
        {
            close(pipe_array[1]);
            close(pipe_array[0]);
        }

        struct list_elem *to_be_removed = list_back(&job_list);
        list_remove(to_be_removed);

        delete_job(cur_job);

        termstate_give_terminal_back_to_shell();
        return;
    }
    else
    {
        // printf("%d is the parent process\n", gpid);
        add_PID(cur_job->PID_list, gpid);
        cur_job->pgid = gpid;
        cur_job->num_processes_alive++;
    }

    // clean the parent process attr and file
    if (posix_spawn_file_actions_destroy(&child_file_attr))
    {
        utils_error("Error destroying file actions");
    }
    if (posix_spawnattr_destroy(&child_spawn_attr))
    {
        utils_error("Error destroying attr");
    }

    // close the parent process pipes
    if (pipe_array != NULL)
    {
        close(pipe_array[1]);
    }

    // create other processes
    struct list_elem *pipeCommand = list_begin(&pipee->commands);
    pipeCommand = list_next(pipeCommand);

    struct list_elem *end = list_back(&pipee->commands);
    struct ast_command *end_command = list_entry(end, struct ast_command, elem);

    // loop to go through other commands
    while (command != end_command)
    {
        // get the command and get next
        command = list_entry(pipeCommand, struct ast_command, elem);
        pipeCommand = list_next(pipeCommand);

        // set up for spawning new process
        pid_t spawnPID;
        posix_spawn_file_actions_t child_file_attr;
        posix_spawnattr_t child_spawn_attr;

        if (posix_spawn_file_actions_init(&child_file_attr) != 0)
        {
            utils_error("failure initalizing file actions");
        }
        if (posix_spawnattr_init(&child_spawn_attr))
        {
            utils_error("failure initalizing child spawn attr");
        }

        // set gpid
        if (posix_spawnattr_setpgroup(&child_spawn_attr, gpid))
        {
            utils_error("Error setting pgroup");
        }
        if (posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP))
        {
            utils_error("Error setting flags");
        }

        // get where the pipes input and output are going to be in the pipe array
        int input = (index - 1) * 2;
        int output = (index * 2) + 1;

        // wire process inputs
        if (posix_spawn_file_actions_adddup2(&child_file_attr, pipe_array[input], fileno(stdin)))
        {
            utils_error("Error calling dup2 on file descriptors");
        }

        // if not the last command, then need to wire to other pipe
        if (command != end_command)
        {
            // printf("Current pipes input: %d, output: %d being wired to: %d\n", input, output, (index * 2));
            pipe2(&pipe_array[index * 2], O_CLOEXEC);
            if (posix_spawn_file_actions_adddup2(&child_file_attr, pipe_array[output], fileno(stdout)))
            {
                utils_error("Error calling dup2 on file descriptors");
            }
            if (command->dup_stderr_to_stdout)
            {
                if (posix_spawn_file_actions_adddup2(&child_file_attr, pipe_array[output], fileno(stderr)))
                {
                    utils_error("Error calling dup2 on file descriptors");
                }
            }
            index++;
        }
        else if (pipee->iored_output)
        {
            int term;
            if (pipee->append_to_output)
            {
                term = O_APPEND;
            }
            else
            {
                term = O_TRUNC;
            }
            if (posix_spawn_file_actions_addopen(&child_file_attr, fileno(stdout), pipee->iored_output, O_WRONLY | term | O_CREAT, 0666))
            {
                utils_error("Error opening file actions");
            }

            if (command->dup_stderr_to_stdout)
            {
                if (posix_spawn_file_actions_adddup2(&child_file_attr, fileno(stdout), fileno(stderr)))
                {
                    utils_error("Error calling dup2 on file descriptors");
                }
            }
        }

        // if spawn successful, update job
        if (posix_spawnp(&spawnPID, command->argv[0], &child_file_attr, &child_spawn_attr, command->argv, environ))
        {
            utils_error("%s: No such file or directory\n", command->argv[0]);
            // clean the parent process attr and file
            posix_spawn_file_actions_destroy(&child_file_attr);
            posix_spawnattr_destroy(&child_spawn_attr);

            struct list_elem *to_be_removed = list_back(&job_list);
            list_remove(to_be_removed);

            delete_job(cur_job);

            termstate_give_terminal_back_to_shell();
            return;
        }
        else
        {
            cur_job->num_processes_alive++;
            add_PID(cur_job->PID_list, spawnPID);
            // printf("%d is the child process\n", spawnPID);
        }

        // clean the process attr and file
        if (posix_spawnattr_destroy(&child_spawn_attr))
        {
            utils_error("Error destroying attr");
        }
        if (posix_spawn_file_actions_destroy(&child_file_attr))
        {
            utils_error("Error destroying file actions");
        }

        // close pipes after use
        close(pipe_array[input]);
        if (command != end_command)
        {
            close(pipe_array[output]);
        }
    }

    // wait for the job to finish
    if (!pipee->bg_job)
    {
        wait_for_job(cur_job);
        termstate_give_terminal_back_to_shell();
    }
    else
    {
        printf("[%d] %d\n", cur_job->jid, cur_job->pgid);
    }

    // clean the pipe array after use
    if (pipe_array)
    {
        free(pipe_array);
    }
}
