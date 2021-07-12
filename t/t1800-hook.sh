#!/bin/sh

test_description='git-hook command and config-managed multihooks'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-terminal.sh

setup_hooks () {
	test_config hook.ghi.command "/path/ghi"
	test_config hook.ghi.event pre-commit --add
	test_config hook.ghi.event test-hook --add
	test_config_global hook.def.command "/path/def"
	test_config_global hook.def.event pre-commit --add
}

setup_hookdir () {
	mkdir .git/hooks
	write_script .git/hooks/pre-commit <<-EOF
	echo \"Legacy Hook\"
	EOF
	test_when_finished rm -rf .git/hooks
}

test_expect_success 'git hook usage' '
	test_expect_code 129 git hook &&
	test_expect_code 129 git hook run &&
	test_expect_code 129 git hook run -h &&
	test_expect_code 129 git hook list -h &&
	test_expect_code 129 git hook run --unknown 2>err &&
	test_expect_code 129 git hook list &&
	test_expect_code 129 git hook list -h &&
	grep "unknown option" err
'

test_expect_success 'git hook run: nonexistent hook' '
	cat >stderr.expect <<-\EOF &&
	error: cannot find a hook named test-hook
	EOF
	test_expect_code 1 git hook run test-hook 2>stderr.actual &&
	test_cmp stderr.expect stderr.actual
'

test_expect_success 'git hook run: nonexistent hook with --ignore-missing' '
	git hook run --ignore-missing does-not-exist 2>stderr.actual &&
	test_must_be_empty stderr.actual
'

test_expect_success 'git hook run: basic' '
	test_hook test-hook <<-EOF &&
	echo Test hook
	EOF

	cat >expect <<-\EOF &&
	Test hook
	EOF
	git hook run test-hook 2>actual &&
	test_cmp expect actual
'

test_expect_success 'git hook run: stdout and stderr both write to our stderr' '
	test_hook test-hook <<-EOF &&
	echo >&1 Will end up on stderr
	echo >&2 Will end up on stderr
	EOF

	cat >stderr.expect <<-\EOF &&
	Will end up on stderr
	Will end up on stderr
	EOF
	git hook run test-hook >stdout.actual 2>stderr.actual &&
	test_cmp stderr.expect stderr.actual &&
	test_must_be_empty stdout.actual
'

for code in 1 2 128 129
do
	test_expect_success "git hook run: exit code $code is passed along" '
		test_hook test-hook <<-EOF &&
		exit $code
		EOF

		test_expect_code $code git hook run test-hook
	'
done

test_expect_success 'git hook run arg u ments without -- is not allowed' '
	test_expect_code 129 git hook run test-hook arg u ments
'

test_expect_success 'git hook run -- pass arguments' '
	test_hook test-hook <<-\EOF &&
	echo $1
	echo $2
	EOF

	cat >expect <<-EOF &&
	arg
	u ments
	EOF

	git hook run test-hook -- arg "u ments" 2>actual &&
	test_cmp expect actual
'

test_expect_success 'git hook run: out-of-repo runs execute global hooks' '
	test_config_global hook.global-hook.event test-hook --add &&
	test_config_global hook.global-hook.command "echo no repo no problems" --add &&

	echo "global-hook" >expect &&
	nongit git hook list test-hook >actual &&
	test_cmp expect actual &&

	echo "no repo no problems" >expect &&

	nongit git hook run test-hook 2>actual &&
	test_cmp expect actual
'

test_expect_success 'git -c core.hooksPath=<PATH> hook run' '
	mkdir my-hooks &&
	write_script my-hooks/test-hook <<-\EOF &&
	echo Hook ran $1
	EOF

	cat >expect <<-\EOF &&
	Test hook
	Hook ran one
	Hook ran two
	Hook ran three
	Hook ran four
	EOF

	test_hook test-hook <<-EOF &&
	echo Test hook
	EOF

	# Test various ways of specifying the path. See also
	# t1350-config-hooks-path.sh
	>actual &&
	git hook run test-hook -- ignored 2>>actual &&
	git -c core.hooksPath=my-hooks hook run test-hook -- one 2>>actual &&
	git -c core.hooksPath=my-hooks/ hook run test-hook -- two 2>>actual &&
	git -c core.hooksPath="$PWD/my-hooks" hook run test-hook -- three 2>>actual &&
	git -c core.hooksPath="$PWD/my-hooks/" hook run test-hook -- four 2>>actual &&
	test_cmp expect actual
'

test_hook_tty () {
	cat >expect <<-\EOF
	STDOUT TTY
	STDERR TTY
	EOF

	test_when_finished "rm -rf repo" &&
	git init repo &&

	test_commit -C repo A &&
	test_commit -C repo B &&
	git -C repo reset --soft HEAD^ &&

	test_hook -C repo pre-commit <<-EOF &&
	test -t 1 && echo STDOUT TTY >>actual || echo STDOUT NO TTY >>actual &&
	test -t 2 && echo STDERR TTY >>actual || echo STDERR NO TTY >>actual
	EOF

	test_terminal git -C repo "$@" &&
	test_cmp expect repo/actual
}

test_expect_success TTY 'git hook run: stdout and stderr are connected to a TTY' '
	test_hook_tty hook run pre-commit
'

test_expect_success TTY 'git commit: stdout and stderr are connected to a TTY' '
	test_hook_tty commit -m"B.new"
'

test_expect_success 'git hook list orders by config order' '
	setup_hooks &&

	cat >expected <<-\EOF &&
	def
	ghi
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list reorders on duplicate event declarations' '
	setup_hooks &&

	# 'def' is usually configured globally; move it to the end by
	# configuring it locally.
	test_config hook.def.event "pre-commit" --add &&

	cat >expected <<-\EOF &&
	ghi
	def
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'hook can be configured for multiple events' '
	setup_hooks &&

	# 'ghi' should be included in both 'pre-commit' and 'test-hook'
	git hook list pre-commit >actual &&
	grep "ghi" actual &&
	git hook list test-hook >actual &&
	grep "ghi" actual
'

test_expect_success 'git hook list shows hooks from the hookdir' '
	setup_hookdir &&

	cat >expected <<-\EOF &&
	hook from hookdir
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'inline hook definitions execute oneliners' '
	test_config hook.oneliner.event "pre-commit" &&
	test_config hook.oneliner.command "echo \"Hello World\"" &&

	echo "Hello World" >expected &&

	# hooks are run with stdout_to_stderr = 1
	git hook run pre-commit 2>actual &&
	test_cmp expected actual
'

test_expect_success 'inline hook definitions resolve paths' '
	write_script sample-hook.sh <<-\EOF &&
	echo \"Sample Hook\"
	EOF

	test_when_finished "rm sample-hook.sh" &&

	test_config hook.sample-hook.event pre-commit &&
	test_config hook.sample-hook.command "\"$(pwd)/sample-hook.sh\"" &&

	echo \"Sample Hook\" >expected &&

	# hooks are run with stdout_to_stderr = 1
	git hook run pre-commit 2>actual &&
	test_cmp expected actual
'

test_expect_success 'hookdir hook included in git hook run' '
	setup_hookdir &&

	echo \"Legacy Hook\" >expected &&

	# hooks are run with stdout_to_stderr = 1
	git hook run pre-commit 2>actual &&
	test_cmp expected actual
'

test_expect_success 'stdin to multiple hooks' '
	test_config hook.stdin-a.event "test-hook" --add &&
	test_config hook.stdin-a.command "xargs -P1 -I% echo a%" --add &&
	test_config hook.stdin-b.event "test-hook" --add &&
	test_config hook.stdin-b.command "xargs -P1 -I% echo b%" --add &&

	cat >input <<-\EOF &&
	1
	2
	3
	EOF

	cat >expected <<-\EOF &&
	a1
	a2
	a3
	b1
	b2
	b3
	EOF

	git hook run --to-stdin=input test-hook 2>actual &&
	test_cmp expected actual
'

test_expect_success 'multiple hooks in series' '
	test_config hook.series-1.event "test-hook" &&
	test_config hook.series-1.command "echo 1" --add &&
	test_config hook.series-2.event "test-hook" &&
	test_config hook.series-2.command "echo 2" --add &&
	mkdir .git/hooks &&
	write_script .git/hooks/test-hook <<-EOF &&
	echo 3
	EOF

	cat >expected <<-\EOF &&
	1
	2
	3
	EOF

	git hook run -j1 test-hook 2>actual &&
	test_cmp expected actual &&

	rm .git/hooks/test-hook
'

test_expect_success 'rejects hooks with no commands configured' '
	test_config hook.broken.event "test-hook" &&

	echo broken >expected &&
	git hook list test-hook >actual &&
	test_cmp expected actual &&
	test_must_fail git hook run test-hook
'

test_expect_success 'git hook run a hook with a bad shebang' '
	test_when_finished "rm -rf bad-hooks" &&
	mkdir bad-hooks &&
	write_script bad-hooks/test-hook "/bad/path/no/spaces" </dev/null &&

	test_expect_code 1 git \
		-c core.hooksPath=bad-hooks \
		hook run test-hook >out 2>err &&
	test_must_be_empty out &&

	# TODO: We should emit the same (or at least a more similar)
	# error on MINGW (essentially Git for Windows) and all other
	# platforms.. See the OS-specific code in start_command()
	grep -E "^(error|fatal): cannot (exec|spawn) .*bad-hooks/test-hook" err
'

test_expect_success 'stdin to hooks' '
	write_script .git/hooks/test-hook <<-\EOF &&
	echo BEGIN stdin
	cat
	echo END stdin
	EOF

	cat >expect <<-EOF &&
	BEGIN stdin
	hello
	END stdin
	EOF

	echo hello >input &&
	git hook run --to-stdin=input test-hook 2>actual &&
	test_cmp expect actual
'

test_done
