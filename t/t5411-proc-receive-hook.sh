#!/bin/sh
#
# Copyright (c) 2020 Jiang Xin
#

test_description='Test proc-receive hook'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

. "$TEST_DIRECTORY"/t5411/common-functions.sh

setup_upstream_and_workbench () {
	# Refs of upstream : main(A)
	# Refs of workbench: main(A)  tags/v123
	test_expect_success "setup upstream and workbench" '
		rm -rf upstream.git &&
		rm -rf workbench &&
		git init --bare upstream.git &&
		git init workbench &&
		create_commits_in workbench A B &&
		(
			cd workbench &&
			# Try to make a stable fixed width for abbreviated commit ID,
			# this fixed-width oid will be replaced with "<OID>".
			git config core.abbrev 7 &&
			git tag -m "v123" v123 $A &&
			git remote add origin ../upstream.git &&
			git push origin main &&
			git update-ref refs/heads/main $A $B &&
			git -C ../upstream.git update-ref \
				refs/heads/main $A $B
		) &&
		TAG=$(git -C workbench rev-parse v123) &&

		# setup pre-receive hook
		test_hook --setup -C upstream.git pre-receive <<-\EOF &&
		exec >&2
		echo "# pre-receive hook"
		while read old new ref
		do
			echo "pre-receive< $old $new $ref"
		done
		EOF

		# setup post-receive hook
		test_hook --setup -C upstream.git post-receive <<-\EOF &&
		exec >&2
		echo "# post-receive hook"
		while read old new ref
		do
			echo "post-receive< $old $new $ref"
		done
		EOF

		upstream=upstream.git
	'
}

run_proc_receive_hook_test() {
	case $1 in
	http)
		PROTOCOL="HTTP protocol"
		URL_PREFIX="http://.*"
		;;
	local)
		PROTOCOL="builtin protocol"
		URL_PREFIX="\.\."
		;;
	esac

	# Include test cases for both file and HTTP protocol
	for t in  "$TEST_DIRECTORY"/t5411/test-*.sh
	do
		. "$t"
	done
}

# Initialize the upstream repository and local workbench.
setup_upstream_and_workbench

# Load test cases that only need to be executed once.
for t in  "$TEST_DIRECTORY"/t5411/once-*.sh
do
	. "$t"
done

# Initialize the upstream repository and local workbench.
setup_upstream_and_workbench

# Run test cases for 'proc-receive' hook on local file protocol.
run_proc_receive_hook_test local

# A self-contained check that 'proc-receive' is also resolved (and its
# bidirectional protocol driven) when defined through configuration
# (hook.<name>.event = proc-receive) rather than as a hook file. This
# exercises the hook.h resolution + duplex path with no hook file present.
test_expect_success 'proc-receive: works as a config-defined hook' '
	test_when_finished "rm -rf cfg-upstream.git cfg-work" &&
	git init --bare cfg-upstream.git &&
	git init cfg-work &&
	create_commits_in cfg-work M &&
	git -C cfg-work push ../cfg-upstream.git HEAD:refs/heads/main &&

	git -C cfg-upstream.git config receive.procReceiveRefs refs/for &&
	git -C cfg-upstream.git config hook.proc.event proc-receive &&
	git -C cfg-upstream.git config hook.proc.command \
		"test-tool proc-receive -v -r \"ok refs/for/main/topic\"" &&
	test_path_is_missing cfg-upstream.git/hooks/proc-receive &&

	git -C cfg-work push ../cfg-upstream.git HEAD:refs/for/main/topic \
		>out 2>&1 &&
	grep "remote: proc-receive< .* refs/for/main/topic" out &&
	grep "remote: proc-receive> ok refs/for/main/topic" out &&
	grep "new reference.*HEAD -> refs/for/main/topic" out &&

	# The configured hook handled the special ref; receive-pack did not
	# create it on its own.
	test_must_fail git -C cfg-upstream.git \
		rev-parse --verify refs/for/main/topic
'

# 'proc-receive' drives a bidirectional protocol, so only one hook can be run
# for it; resolving both a hook file and a configured hook is ambiguous and
# must be rejected rather than replaying the exchange twice.
test_expect_success 'proc-receive: refuses multiple configured hooks' '
	test_when_finished "rm -rf multi-upstream.git multi-work" &&
	git init --bare multi-upstream.git &&
	git init multi-work &&
	create_commits_in multi-work M &&
	git -C multi-work push ../multi-upstream.git HEAD:refs/heads/main &&

	git -C multi-upstream.git config receive.procReceiveRefs refs/for &&
	# A hook file ...
	test_hook -C multi-upstream.git proc-receive <<-\EOF &&
	test-tool proc-receive -v -r "ok refs/for/main/topic"
	EOF
	# ... and a configured hook for the same event.
	git -C multi-upstream.git config hook.proc.event proc-receive &&
	git -C multi-upstream.git config hook.proc.command \
		"test-tool proc-receive -v -r \"ok refs/for/main/topic\"" &&

	test_must_fail git -C multi-work push ../multi-upstream.git \
		HEAD:refs/for/main/topic >out 2>&1 &&
	grep "only a single .proc-receive. hook is supported" out &&

	# Nothing was applied.
	test_must_fail git -C multi-upstream.git \
		rev-parse --verify refs/for/main/topic
'

ROOT_PATH="$PWD"
. "$TEST_DIRECTORY"/lib-gpg.sh
. "$TEST_DIRECTORY"/lib-httpd.sh
. "$TEST_DIRECTORY"/lib-terminal.sh
start_httpd

# Re-initialize the upstream repository and local workbench.
setup_upstream_and_workbench

# Refs of upstream : main(A)
# Refs of workbench: main(A)  tags/v123
test_expect_success "setup for HTTP protocol" '
	git -C upstream.git config http.receivepack true &&
	upstream="$HTTPD_DOCUMENT_ROOT_PATH/upstream.git" &&
	mv upstream.git "$upstream" &&
	git -C workbench remote set-url origin "$HTTPD_URL/auth-push/smart/upstream.git" &&
	set_askpass user@host pass@host
'

setup_askpass_helper

# Run test cases for 'proc-receive' hook on HTTP protocol.
run_proc_receive_hook_test http

test_done
