#!/usr/bin/python
#
# history_test: tests the history command
# 
# Test the history command 
# 

import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

# prompt commands into shell
sendline("echo never")
expect_exact("never", "could not execute command 'echo never'")

sendline("echo going")
expect_exact("going", "could not execute command 'echo going'")

sendline("echo to give | rev")
expect_exact("evig ot", "could not execute command 'echo to give | rev'")

sendline("echo you")
expect_exact("you", "could not execute command 'echo you'")

sendline("echo up")
expect_exact("up", "could not execute command 'echo up'")

# check to see history outputs properly
sendline("history")
expect("1 echo never")
expect("2 echo going")  
expect("3 echo to give | rev")
expect("4 echo you")
expect("5 echo up")
expect("6 history")

# check to execute the command 2 positions from the current command
sendline("!-2")
expect_exact("up", "could not execute command 'echo up' from history")

# check to execute the previous command
sendline("!!")
expect_exact("up", "could not execute command 'echo up' from history")

# check to execute the most recent command that is echo you
sendline("!echo you")
expect_exact("you", "could not execute command 'echo you' from history")

# check to execute the third command from history
sendline("!3")
expect_exact("evig ot", "could not execute command 'echo to give | rev' from history")

#exit
sendline("exit")
expect_exact("exit\r\n", "Shell output extraneous characters")

test_success()
