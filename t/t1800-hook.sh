#!/bin/sh

test_description='git-hook command'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-terminal.sh

test_expect_success 'git hook usage' '
	test_expect_code 129 git hook &&
	test_expect_code 129 git hook run &&
	test_expect_code 129 git hook run -h &&
	test_expect_code 129 git hook run --unknown 2>err &&
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

test_expect_success 'git hook run -- out-of-repo runs excluded' '
	test_hook test-hook <<-EOF &&
	echo Test hook
	EOF

	nongit test_must_fail git hook run test-hook
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

check_stdout_separate_from_stderr () {
	for hook in "$@"
	do
		test_grep ! "Hook $hook stdout" stderr.actual &&
		test_grep ! "Hook $hook stderr" stdout.actual &&
		test_grep "Hook $hook stderr" stderr.actual &&
		test_grep "Hook $hook stdout" stdout.actual || return 1
	done
}

check_stdout_merged_to_stderr () {
	test_grep ! "Hook .* stdout" stdout.actual &&
	test_grep ! "Hook .* stderr" stdout.actual &&
	for hook in "$@"
	do
		test_grep "Hook $hook stdout" stderr.actual &&
		test_grep "Hook $hook stderr" stderr.actual || return 1
	done
}

test_expect_success 'client pre-push hook expects separate stdout and stderr' '
	test_when_finished "rm -f stdout.actual stderr.actual" &&
	git init --bare remote &&
	git remote add origin remote &&
	test_commit A &&

	hook=pre-push &&
	test_hook $hook <<-EOF &&
	echo >&1 Hook $hook stdout
	echo >&2 Hook $hook stderr
	EOF

	git push origin HEAD:main >stdout.actual 2>stderr.actual &&
	check_stdout_separate_from_stderr pre-push
'

test_expect_success 'client hooks expect stdout redirected to stderr' '
	test_when_finished "rm -f stdout.actual stderr.actual" &&
	for hook in pre-commit post-commit post-checkout pre-merge-commit \
		prepare-commit-msg commit-msg post-merge post-rewrite reference-transaction \
		applypatch-msg pre-applypatch post-applypatch pre-rebase post-index-change
	do
		test_hook $hook <<-EOF || return 1
		echo >&1 Hook $hook stdout
		echo >&2 Hook $hook stderr
		EOF
	done &&

	git checkout -B main &&
	git checkout -b branch-a &&
	test_commit commit-on-branch-a &&

	# Trigger pre-commit, prepare-commit-msg, commit-msg, post-commit, reference-transaction
	git commit --allow-empty -m "Test" >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr pre-commit prepare-commit-msg commit-msg post-commit reference-transaction &&

	# Trigger post-checkout, reference-transaction
	git checkout -b new-branch main >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr post-checkout reference-transaction &&

	# Trigger pre-merge-commit, post-merge, reference-transaction
	test_commit new-branch-commit &&
	git merge --no-ff branch-a >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr pre-merge-commit post-merge reference-transaction &&

	# Trigger post-rewrite, reference-transaction
	git commit --amend --allow-empty --no-edit >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr post-rewrite reference-transaction &&

	# Trigger applypatch-msg, pre-applypatch, post-applypatch
	git checkout -b branch-b main &&
	test_commit branch-b &&
	git format-patch -1 --stdout >patch &&
	git checkout -b branch-c main &&
	git am patch >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr applypatch-msg pre-applypatch post-applypatch &&

	# Trigger pre-rebase
	git checkout -b branch-d main &&
	test_commit branch-d &&
	git checkout main &&
	test_commit diverge-main &&
	git checkout branch-d &&
	git rebase main >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr pre-rebase &&

	# Trigger post-index-change
	oid=$(git hash-object -w --stdin </dev/null) &&
	git update-index --add --cacheinfo 100644 $oid new-file >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr post-index-change
'

test_expect_success 'server hooks expect stdout redirected to stderr' '
	test_when_finished "rm -f stdout.actual stderr.actual" &&
	git init --bare remote-server &&
	git remote add origin-server remote-server &&

	for hook in pre-receive update post-receive post-update
	do
		write_script remote-server/hooks/$hook <<-EOF || return 1
		echo >&1 Hook $hook stdout
		echo >&2 Hook $hook stderr
		EOF
	done &&

	# Trigger pre-receive update post-receive post-update
	git push origin-server HEAD:new-branch >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr pre-receive update post-receive post-update
'

test_expect_success 'server push-to-checkout hook expects stdout redirected to stderr' '
	test_when_finished "rm -f stdout.actual stderr.actual" &&
	git init server &&
	git -C server checkout -b main &&
	test_config -C server receive.denyCurrentBranch updateInstead &&
	git remote add origin-server-2 server &&

	write_script server/.git/hooks/push-to-checkout <<-EOF &&
	echo >&1 Hook push-to-checkout stdout
	echo >&2 Hook push-to-checkout stderr
	EOF

	# Trigger push-to-checkout
	git push origin-server-2 HEAD:main >stdout.actual 2>stderr.actual &&
	check_stdout_merged_to_stderr push-to-checkout
'

test_expect_success 'receive-pack hooks sideband output works' '
	git init --bare target-sideband.git &&
	test_commit sideband-a &&
	git remote add origin-sideband ./target-sideband.git &&

	# pre-receive hook
	test_hook -C target-sideband.git pre-receive <<-\EOF &&
	echo "stdout pre-receive"
	echo "stderr pre-receive" >&2
	EOF

	git push origin-sideband HEAD:refs/heads/pre-receive 2>actual &&
	test_grep "remote: stdout pre-receive" actual &&
	test_grep "remote: stderr pre-receive" actual &&

	# update hook
	test_hook -C target-sideband.git update <<-\EOF &&
	echo "stdout update"
	echo "stderr update" >&2
	EOF

	test_commit sideband-b &&
	git push origin-sideband HEAD:refs/heads/update 2>actual &&
	test_grep "remote: stdout update" actual &&
	test_grep "remote: stderr update" actual &&

	# post-receive hook
	test_hook -C target-sideband.git post-receive <<-\EOF &&
	echo >&1 "stdout post-receive"
	echo >&2 "stderr post-receive"
	EOF

	test_commit sideband-c &&
	git push origin-sideband HEAD:refs/heads/post-receive 2>actual &&
	test_grep "remote: stdout post-receive" actual &&
	test_grep "remote: stderr post-receive" actual &&

	# post-update hook
	test_hook -C target-sideband.git post-update <<-\EOF &&
	echo >&1 "stdout post-update"
	echo >&2 "stderr post-update"
	EOF

	test_commit sideband-d &&
	git push origin-sideband HEAD:refs/heads/post-update 2>actual &&
	test_grep "remote: stdout post-update" actual &&
	test_grep "remote: stderr post-update" actual
'

test_done
