Student Information
-------------------
John Tomaas
pid: jtomas

Kabeer Baig
pid: kabeerb

How to execute the shell
------------------------
head to src directory
    cd src/
use the make file to ensure that cush is updated
    make
then launch the custom shell with
    ./cush

Important Notes
---------------
<Any important notes about your system>
Our shell implements cd and history as functionality.
The tests are located within the src file with the names:
    history_test.py
    cd_test.py
Which can be run with the stdriver.py script with the .tst file:
    custom_tests.tst

Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

jobs:
    We loop through the job_list struct that acts as a linked list and use the
    print_job() function, we print any jobs that do not have their status as DONE.

fg:
    Using the argument provided for fg as the job id, we see if there is a job struct that is 
    assigned to the job id. If it exists, then we assign the process group id as a foreground process
    , give it access to the terminal, and then send a signal SIGCONT to continue the process.
    Now as a foreground process, we wait for the job to complete while it has access 
    to the terminal.

bg:
    Using the argument provided for fg as the job id, we see if there is a job struct that is 
    assigned to the job id. If it exists, then we look at the status of the job.
    If it is already in the background, then we do not need to do anything.
    Else, we assign the status to the background, and send a signal to the process group id to continue,
    except it will continue in the background.

kill:
    Using the argument provided for fg as the job id, we see if there is a job struct that is 
    assigned to the job id. If it exists, then we send a signal to the process group id to KILL
    the process. Once killed, we supply the terminal back to the shell. In the sigchld_handler,
    we clean any jobs that have already been notified as DONE to the user.

stop:
    Using the argument provided for fg as the job id, we see if there is a job struct that is 
    assigned to the job id. If it exists, then we change the status of the job to STOPPED and send
    a signal to the process group id to STOP the process. We then give the terminal back to the shell

\^C:
    Receiving a CTRL-C will end the current foreground process that is running. This is handled
    by the sigchld_handler. Being notified with WIFSIGNALED, we set the job to be ready to be deleted.
    When there is no current foreground process running, the shell will be considered the process 
    and it wil be terminated.

\^Z:
    Receiving a CTRL-Z will stop the current foreground process that is running. This is handled
    by the sigchld_handler. Being notified with WIFSTOPPED, we check to see if the process is either
    SIGSTP or SIGSTOP to handle being stopped. When stopped, we change the status of the job
    to STOPPED and then print the job to notify the user of the stoppage. We then save the
    the current terminal state of the process, so that it may be used again if the process
    is continued.


Description of Extended Functionality
-------------------------------------
<describe your IMPLEMENTATION of the following functionality:
I/O, Pipes, Exclusive Access >
I/O:
    Our shell manages fundamental I/O redirection. We are able to reroute standard input by using 
    arrows ("<" ">" "<<" ">>"). We are also able to redirect standard output using the same arrows. 
    With the provided functions we were able to read from a spawned processes standard input.
    We were also able to write to standard output, standard error, and open/close the proper ends
    of the pipe in order to get our redirection working properly. 
Pipes:
    If processes are piped, then we wire the commands piped to each other so that the 
    standard output will be wired into the next command's standard input. The final process
    will proceed to output to the terminal, completing the pipe. 

Exclusive Access:
    Foreground processes will always have access to the terminal until the process is completed.
    When all foreground processes are completed, then we give the terminal back to the shell.
    If a process is stopped, then we save the state of the terminal, so that it can be used 
    later on. We also handle jobs when they are stopped because they need terminal access and 
    give change the status of the job to reflect this. All of these combine ensure that we 
    exclusive access to the terminal.


List of Additional Builtins Implemented
---------------------------------------
<cd, history>

cd:
    When a user uses cd without any arguments, than we change the directory to the HOME directory.
    Else, we attempt to change the directory to what the user provided. If successful, it will change
    the directory and if not then we give an error stating that there is no such file or directory.

history:
    We use the GNU History Library to implement history functions. When a user inputs this command,
    then it will show the previous commands that the user has inputted and an id that is 
    associated with it.
    We do this by adding the command line to the history before it is parsed and executed.
    If it is an event designator, then we take the command line that the event designator commands
    get and set it as the new command line that needs to be parsed and executed.
    We can also use the features commonly provided by GNU history, such as event designators.

    !!: executes the previous command in the history.
    !n: executes the nth command in the history.
    !-n: executes the command n positions before the current command in the history.
    !string: executes the most recent command in the history that starts with the specified string.

    the up arrow and down arrow will also work to select previously inputted commands


(Written by Your Team)
<builtin name>
<description>