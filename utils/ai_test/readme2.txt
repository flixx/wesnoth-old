/*
   AI Batch testing suite
   Copyright (C) 2009 by Yurii Chernyi <terraninfo@terraninfo.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2
   or at your option any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

NOTE:
There are 2 ways to run the test-suite:
1. With Postgresql (ai_test.py)
2. Without Postgresql (ai_test2.py)
In the second case the script will generate a logfile

HOWTO USE ai_test2.py:
Alter the ai_test2.cfg file and run the script via
  python ai_test2.py [-p]

TIPS:
- Before you start testing use the "-p" parameter
  to play a test-game with gui. Then you may want to use
  :inspect to see if the ais are set up correctly.
- 100 tests are nothing, 500 tests are good to see some
  trends. I recommend to run at least 1000 tests. 
- You can make use of your multicores and run multiple
  tests at the same time. In Linux the 'System Monitor'
  is your friend. You can set the process priority there
  to 'Very Low' or pause the process and continue it later.
