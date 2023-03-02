#
# cd_test: tests the cd command
# 
# Test the cd command 
# Requires the following commands to be implemented
# or otherwise usable:
#
#	fg, sleep, ctrl-c control, ctrl-z control
#

import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()
#exit
sendline("exit")
expect_exact("exit\r\n", "Shell output extraneous characters")

test_success()
