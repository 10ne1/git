#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "abspath.h"
#include "advice.h"
#include "gettext.h"
#include "hook.h"
#include "path.h"
#include "run-command.h"
#include "config.h"
#include "strbuf.h"
#include "environment.h"
#include "setup.h"
#include "list.h"

static void free_hook(struct hook *ptr)
{
	if (ptr) {
		free((char*)ptr->name);
	}
	free(ptr);
}

/*
 * Walks the linked list at 'head' to check if any hook named 'name'
 * already exists. Returns a pointer to that hook if so, otherwise returns NULL.
 */
static struct hook *find_hook_by_name(struct list_head *head, const char *name)
{
	struct list_head *pos = NULL, *tmp = NULL;
	struct hook *found = NULL;

	list_for_each_safe(pos, tmp, head) {
		struct hook *it = list_entry(pos, struct hook, list);
		if (!strcmp(it->name, name)) {
			list_del(pos);
			found = it;
			break;
		}
	}
	return found;
}

/*
 * Appends a hook to the list, or moves it to the end if it already
 * exists. This function takes ownership of the 'name' string.
 */
static void append_or_move_hook(struct list_head *head, char *name)
{
	struct hook *to_add = NULL;

	if (name) {
		/* find_hook_by_name removes the entry from the list if found */
		to_add = find_hook_by_name(head, name);
	}

	if (!to_add) {
		/* This is a new hook, create it and take ownership of name. */
		to_add = xmalloc(sizeof(*to_add));
		to_add->name = name; /* Takes ownership */
		to_add->feed_pipe_cb_data = NULL;
	} else {
		/* The hook already existed, so we don't need the new name string. */
		free(name);
	}

	list_add_tail(&to_add->list, head);
}

static void remove_hook(struct list_head *to_remove)
{
	struct hook *hook_to_remove = list_entry(to_remove, struct hook, list);
	list_del(to_remove);
	free_hook(hook_to_remove);
}

void clear_hook_list(struct list_head *head)
{
	struct list_head *pos, *tmp;
	list_for_each_safe(pos, tmp, head)
		remove_hook(pos);
	free(head);
}

const char *find_hook(struct repository *r, const char *name)
{
	static struct strbuf path = STRBUF_INIT;

	int found_hook;

	repo_git_path_replace(r, &path, "hooks/%s", name);
	found_hook = access(path.buf, X_OK) >= 0;
#ifdef STRIP_EXTENSION
	if (!found_hook) {
		int err = errno;

		strbuf_addstr(&path, STRIP_EXTENSION);
		found_hook = access(path.buf, X_OK) >= 0;
		if (!found_hook)
			errno = err;
	}
#endif

	if (!found_hook) {
		if (errno == EACCES && advice_enabled(ADVICE_IGNORED_HOOK)) {
			static struct string_list advise_given = STRING_LIST_INIT_DUP;

			if (!string_list_lookup(&advise_given, name)) {
				string_list_insert(&advise_given, name);
				advise(_("The '%s' hook was ignored because "
					 "it's not set as executable.\n"
					 "You can disable this warning with "
					 "`git config set advice.ignoredHook false`."),
				       path.buf);
			}
		}
		return NULL;
	}
	return path.buf;
}

int hook_exists(struct repository *r, const char *name)
{
	int exists = 0;
	struct list_head *hooks = list_hooks(r, name);

	exists = !list_empty(hooks);

	clear_hook_list(hooks);
	return exists;
}

struct hook_config_cb
{
	const char *hook_event;
	struct list_head *list;
};

/*
 * Callback for git_config which adds configured hooks to a hook list.  Hooks
 * can be configured by specifying both hook.<friend-name>.command = <path> and
 * hook.<friendly-name>.event = <hook-event>.
 */
static int hook_config_lookup(const char *key, const char *value,
			      const struct config_context *ctx UNUSED,
			      void *cb_data)
{
	struct hook_config_cb *data = cb_data;
	const char *name, *event_key;
	size_t name_len = 0;

	/*
	 * Don't bother doing the expensive parse if there's no
	 * chance that the config matches 'hook.myhook.event = hook_event'.
	 */
	if (!value || strcmp(value, data->hook_event))
		return 0;

	/* Looking for "hook.friendlyname.event = hook_event" */
	if (parse_config_key(key, "hook", &name, &name_len, &event_key) ||
	    strcmp(event_key, "event"))
		return 0;

	/*
	 * Create a heap-allocated, null-terminated copy of the hook name
	 * and pass ownership of it to append_or_move_hook().
	 */
	append_or_move_hook(data->list, xmemdupz(name, name_len));

	return 0;
}

struct list_head *list_hooks(struct repository *r, const char *hookname)
{
	struct list_head *hook_head = xmalloc(sizeof(struct list_head));
	struct hook_config_cb cb_data = {
		.hook_event = hookname,
		.list = hook_head,
	};

	INIT_LIST_HEAD(hook_head);

	if (!hookname)
		BUG("null hookname was provided to hook_list()!");

	/* Add the hooks from the config, e.g. hook.myhook.event = pre-commit */
	repo_config(r, hook_config_lookup, &cb_data);

	/* Add the hook from the hookdir. The placeholder makes it easier to
	 * allocate work in pick_next_hook. */
	if (find_hook(r, hookname))
		append_or_move_hook(hook_head, NULL);

	return hook_head;
}

static int pick_next_hook(struct child_process *cp,
			  struct strbuf *out UNUSED,
			  void *pp_cb,
			  void **pp_task_cb)
{
	struct hook_cb_data *hook_cb = pp_cb;
	struct hook *to_run = hook_cb->options->run_me;

	if (!to_run)
		return 0;

	cp->no_stdin = 1;
	strvec_pushv(&cp->env, hook_cb->options->env.v);

	if (hook_cb->options->path_to_stdin && hook_cb->options->feed_pipe)
		BUG("options path_to_stdin and feed_pipe are mutually exclusive");

	/* reopen the file for stdin; run_command closes it. */
	if (hook_cb->options->path_to_stdin) {
		cp->no_stdin = 0;
		cp->in = xopen(hook_cb->options->path_to_stdin, O_RDONLY);
	}

	if (hook_cb->options->feed_pipe) {
		cp->no_stdin = 0;
		/* start_command() will allocate a pipe / stdin fd for us */
		cp->in = -1;
	}

	cp->stdout_to_stderr = hook_cb->options->stdout_to_stderr;
	cp->trace2_hook_name = hook_cb->hook_name;
	cp->dir = hook_cb->options->dir;

	/*
	 * to enable oneliners, let config-specified hooks run in shell.
	 * config-specified hooks have a name.
	 */
	cp->use_shell = !!to_run->name;

	/* add command */
	if (to_run->name) {
		/* ...from config */
		struct strbuf cmd_key = STRBUF_INIT;
		char *command = NULL;

		strbuf_addf(&cmd_key, "hook.%s.command", to_run->name);
		if (repo_config_get_string(hook_cb->repository,
					   cmd_key.buf, &command)) {
			die(_("'hook.%s.command' must be configured "
			      "or 'hook.%s.event' must be removed; aborting.\n"),
			    to_run->name, to_run->name);
		}

		strvec_push(&cp->args, command);
		free(command);
		strbuf_release(&cmd_key);
	} else {
		/* ...from hookdir. */
		const char *hook_path = find_hook(hook_cb->repository,
						  hook_cb->hook_name);
		if (!hook_path)
			BUG("hookdir hook in hook list but no hookdir hook present in filesystem");

		if (hook_cb->options->dir)
			hook_path = absolute_path(hook_path);

		strvec_push(&cp->args, hook_path);
	}

	/*
	 * Provide the hook itself for easy access to its internal state, so hook
	 * callbacks don't have to go through hook_cb->options.
	 */
	*pp_task_cb = to_run;

	/*
	 * Add passed-in argv, without expanding - let the user get back
	 * exactly what they put in.
	 */
	strvec_pushv(&cp->args, hook_cb->options->args.v);

	/* Get the next entry ready */
	if (hook_cb->options->run_me->list.next == hook_cb->head)
		hook_cb->options->run_me = NULL;
	else
		hook_cb->options->run_me = list_entry(hook_cb->options->run_me->list.next,
						      struct hook, list);

	return 1;
}

static int notify_start_failure(struct strbuf *out,
				void *pp_cb,
				void *pp_task_cb)
{
	struct hook_cb_data *hook_cb = pp_cb;
	struct hook *hook = pp_task_cb;

	if (hook_cb)
		hook_cb->rc |= 1;

	if (out) {
		if (hook && hook->name)
			strbuf_addf(out, _("Couldn't start hook '%s'\n"),
				    hook->name);
		else
			strbuf_addstr(out, _("Couldn't start hook from hooks directory\n"));
	}

	return 1;
}

static int notify_hook_finished(int result,
				struct strbuf *out UNUSED,
				void *pp_cb,
				void *pp_task_cb UNUSED)
{
	struct hook_cb_data *hook_cb = pp_cb;
	struct run_hooks_opt *opt = hook_cb->options;

	hook_cb->rc |= result;

	if (opt->invoked_hook)
		*opt->invoked_hook = 1;

	return 0;
}

static void run_hooks_opt_clear(struct run_hooks_opt *options)
{
	strvec_clear(&options->env);
	strvec_clear(&options->args);
}

/*
 * Determines how many jobs to use for hook execution.
 * The priority is as follows:
 *   1. 'struct run_hooks_opt.jobs' parameter is used directly if non-zero, e.g.
 *      RUN_HOOKS_OPT_INIT_SERIAL forces jobs == 1 for serial execution of hooks
 *      unsafe to parallelize, overriding any 'hook.jobs' user configuration.
 *   2. The 'hook.jobs' configuration is used if set.
 *   3. The number of online CPUs is used as a final fallback.
 * Returns:
 *   The number of jobs to use for parallel execution, or 1 for serial.
 */
static unsigned int get_hook_jobs(struct repository *r, struct run_hooks_opt *options)
{
	unsigned int jobs = options->jobs;

	if (!jobs && repo_config_get_uint(r, "hook.jobs", &jobs))
		jobs = online_cpus(); /* fallback if config is unset */

	return jobs;
}

int run_hooks_opt(struct repository *r, const char *hook_name,
		  struct run_hooks_opt *options)
{
	struct strbuf abs_path = STRBUF_INIT;
	struct hook_cb_data cb_data = {
		.rc = 0,
		.hook_name = hook_name,
		.options = options,
		.head = list_hooks(r, hook_name),
		.repository = r,
	};
	int ret = 0;
	const struct run_process_parallel_opts opts = {
		.tr2_category = "hook",
		.tr2_label = hook_name,

		.processes = get_hook_jobs(r, options),
		.ungroup = options->ungroup,

		.get_next_task = pick_next_hook,
		.start_failure = notify_start_failure,
		.feed_pipe = options->feed_pipe,
		.consume_output = options->consume_output,
		.task_finished = notify_hook_finished,

		.data = &cb_data,
	};

	if (options->run_me && options->run_me->feed_pipe_cb_data) {
		struct list_head *pos;
		list_for_each(pos, cb_data.head) {
			struct hook *h = list_entry(pos, struct hook, list);
			if (options->copy_feed_pipe_cb_data)
				h->feed_pipe_cb_data = options->copy_feed_pipe_cb_data(options->run_me->feed_pipe_cb_data);
			else
				h->feed_pipe_cb_data = options->run_me->feed_pipe_cb_data;
		}
	}
	cb_data.options->run_me = list_first_entry(cb_data.head, struct hook, list);

	if (!options)
		BUG("a struct run_hooks_opt must be provided to run_hooks");

	if (options->path_to_stdin && options->feed_pipe)
		BUG("options path_to_stdin and feed_pipe are mutually exclusive");

	if (options->invoked_hook)
		*options->invoked_hook = 0;

	if (list_empty(cb_data.head) && !options->error_if_missing)
		goto cleanup;

	if (list_empty(cb_data.head)) {
		ret = error("cannot find a hook named %s", hook_name);
		goto cleanup;
	}

	run_processes_parallel(&opts);
	ret = cb_data.rc;
cleanup:
	if (options->free_feed_pipe_cb_data) {
		struct list_head *pos;
		list_for_each(pos, cb_data.head) {
			struct hook *h = list_entry(pos, struct hook, list);
			if (h->feed_pipe_cb_data)
				options->free_feed_pipe_cb_data(h->feed_pipe_cb_data);
		}
	}
	clear_hook_list(cb_data.head);
	strbuf_release(&abs_path);
	run_hooks_opt_clear(options);
	return ret;
}

int run_hooks(struct repository *r, const char *hook_name)
{
	struct run_hooks_opt opt = RUN_HOOKS_OPT_INIT_PARALLEL;

	/* All use-cases of this API require ungrouping. */
	opt.ungroup = 1;

	return run_hooks_opt(r, hook_name, &opt);
}

int run_hooks_l(struct repository *r, const char *hook_name, ...)
{
	struct run_hooks_opt opt = RUN_HOOKS_OPT_INIT_PARALLEL;
	va_list ap;
	const char *arg;

	va_start(ap, hook_name);
	while ((arg = va_arg(ap, const char *)))
		strvec_push(&opt.args, arg);
	va_end(ap);

	return run_hooks_opt(r, hook_name, &opt);
}
