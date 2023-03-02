#!/usr/bin/python
#
# cd_test: tests the cd command
# 
# Test the cd command 
# 

import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

# make a directory and cd into it
sendline("mkdir asfnaibapei")
sendline("cd asfnaibapei")
sendline("pwd")
expect_exact(os.getcwd(), "expected " + os.getcwd())

# move out of dir
sendline("cd ../")
sendline("pwd")
expect_exact(os.getcwd(), "expected " + os.getcwd())

# remove new dir
sendline("rmdir asfnaibapei")

# attempt to cd into non existing dir
sendline("cd asfnaibapei")
sendline("pwd")
expect("No such file or directory")

# check that cd changes current directory to home directory
sendline("cd")
sendline("pwd")
home = os.environ["HOME"]
expect_exact(os.path.realpath(home), "cd does not go to home directory")

#exit
sendline("exit")
expect_exact("exit\r\n", "Shell output extraneous characters")

test_success()
