#!/usr/bin/env bash
# Run test_*.sh tests, continue on error, no logging, print pass/fail summary.
#
# This script requires the user to be in the sudo group.
#
# The 'pattern' variable determines which test files are executed.
# By default, run all the tests in the test_*.sh files in this directory.

: ${pattern:=test_*.sh}

# You can specify tests files to exclude using the 'SKIPLIST' variable in a
# site.sh file. See example in README.md. Files excluded by the 'SKIPLIST'
# variable have precedence over files included by the 'pattern' variable.

# This script does not take arguments, so if arguments exist, print the Usage
# in http://docopt.org/ format.
if [ $# -gt 0 ]; then 
    echo "Usage:"
    echo "    ${0}                             # Run all 'test_*.sh' files"
    echo "    pattern=<Bash glob pattern> ${0} # Run only files matching the pattern"
    echo ""
    echo "Example:"
    echo "    ${0} 2>&1 | tee test.log         # Run all tests, output to 'test.log'"
    echo "    pattern=test_feature.sh ${0}     # Run only the tests in 'test_feature.sh'"
    exit -1
fi

let sumerr=0 # error counter
for testfile in $pattern; do # For lib.sh the variable must be called testfile
    echo "Running $testfile"
    ./$testfile  > /dev/null 2>&1
    errcode=$?
    if [ $errcode -ne 0 ]; then
        let sumerr++
	echo -e "\e[31mError in $testfile errcode=$errcode"
	echo -ne "\e[0m"
    fi
done
if [ $sumerr -eq 0 ]; then 
    echo "OK"
else
    echo -e "\e[31m${sumerr} Errors"
    echo -ne "\e[0m"
    exit -1
fi


