#!/bin/sh
# user/shtest.sh -- BusyBox ash on Kestrel: fork/exec, pipes, redirection,
# job control-free background jobs, signals. Installed as /boot/bin/shtest.sh;
# run with kestrel.exec=/bin/sh,/bin/shtest.sh. Exit status 0 = all passed.
fail=0
check() {   # check "description" command...
    d=$1; shift
    if "$@"; then echo "  ok    $d"; else echo "  FAIL  $d"; fail=$((fail + 1)); fi
}

echo "shtest: $(sh -c 'echo $0') pid $$ in $(pwd)"
check "arithmetic"            [ $((6 * 7)) = 42 ]
check "command substitution"  [ "$(echo hi)" = hi ]
check "external command"      [ "$(/bin/echo external)" = external ]
check "pipe of three"         [ "$(echo b a c | tr ' ' '\n' | sort | tr -d '\n')" = abc ]
check "ls /bin finds sh"      sh -c 'ls /bin | grep -qx sh'
echo "line one" > /tmp/shtest.txt
echo "line two" >> /tmp/shtest.txt
check "redirection into /tmp" [ "$(wc -l < /tmp/shtest.txt)" -eq 2 ]
check "grep, sed, cut"        [ "$(grep two /tmp/shtest.txt | sed 's/two/2/' | cut -d' ' -f2)" = 2 ]
rm /tmp/shtest.txt
check "rm"                    [ ! -e /tmp/shtest.txt ]
n=0; for i in 1 2 3 4 5; do n=$((n + i)); done
check "for loop"              [ $n -eq 15 ]
sh -c 'exit 3'; st=$?
check "status of a child"     [ $st -eq 3 ]
sleep 1 & bg=$!
check "background job + wait" wait $bg
check "kill a sleeping job"   sh -c 'sleep 10 & p=$!; kill $p; wait $p; [ $? -eq 143 ]'
check "trap on a signal"      [ "$(sh -c 'trap "echo caught" USR1; kill -USR1 $$; echo after')" = "caught
after" ]
cat <<EOF > /tmp/heredoc
alpha
beta
EOF
check "here-document"         [ "$(tail -n 1 /tmp/heredoc)" = beta ]
check "subshell isolation"    [ "$(x=1; (x=2); echo $x)" = 1 ]
check "uname"                 [ "$(uname -s)" = Kestrel ]
check "/proc/self/stat"       [ "$(cut -d' ' -f1 /proc/self/stat)" -gt 0 ]
check "/proc/PID/cmdline"     grep -q shtest.sh /proc/$$/cmdline
check "readlink /proc/self/fd"  [ "$(readlink /proc/self/fd/0)" = "$(readlink /proc/self/fd/0)" ] && [ -n "$(readlink /proc/self/fd/1)" ]
check "ps lists processes"    sh -c 'ps | grep -q "/bin/sh /bin/shtest.sh"'
/bin/sleep 30 &                               # the external program: ash's own sleep is a builtin
sleep 1                                       # let it exec (until then it is a forked sh)
check "killall by name"       sh -c 'killall sleep'
wait $!; st=$?
check "...the sleep died"     [ $st -eq 143 ]
if [ $fail -eq 0 ]; then echo "shtest: all checks passed"; else echo "shtest: FAILED ($fail)"; fi
exit $fail
