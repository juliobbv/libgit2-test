/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "repository.h"
#include "commit.h"
#include "tree.h"
#include "reflog.h"
#include "git2/diff.h"
#include "git2/stash.h"
#include "git2/status.h"
#include "git2/checkout.h"

static int create_error(int error, const char *msg)
{
	giterr_set(GITERR_STASH, "Cannot stash changes - %s", msg);
	return error;
}

static int ensure_non_bare_repository(git_repository *repo)
{
	if (!git_repository_is_bare(repo))
		return 0;

	return create_error(GIT_EBAREREPO,
		"Stash related operations require a working directory.");
}

static int retrieve_head(git_reference **out, git_repository *repo)
{
	int error = git_repository_head(out, repo);

	if (error == GIT_EORPHANEDHEAD)
		return create_error(error, "You do not have the initial commit yet.");

	return error;
}

static int append_abbreviated_oid(git_buf *out, const git_oid *b_commit)
{
	char *formatted_oid;

	formatted_oid = git_oid_allocfmt(b_commit);
	GITERR_CHECK_ALLOC(formatted_oid);

	git_buf_put(out, formatted_oid, 7);
	git__free(formatted_oid);

	return git_buf_oom(out) ? -1 : 0;
}

static int append_commit_description(git_buf *out, git_commit* commit)
{
	const char *message;
	int pos = 0, len;

	if (append_abbreviated_oid(out, git_commit_id(commit)) < 0)
		return -1;

	message = git_commit_message(commit);
	len = strlen(message);

	/* TODO: Replace with proper commit short message
	 * when git_commit_message_short() is implemented.
	 */
	while (pos < len && message[pos] != '\n')
		pos++;

	git_buf_putc(out, ' ');
	git_buf_put(out, message, pos);
	git_buf_putc(out, '\n');

	return git_buf_oom(out) ? -1 : 0;
}

static int retrieve_base_commit_and_message(
	git_commit **b_commit,
	git_buf *stash_message,
	git_repository *repo)
{
	git_reference *head = NULL;
	int error;

	if ((error = retrieve_head(&head, repo)) < 0)
		return error;

	error = -1;

	if (strcmp("HEAD", git_reference_name(head)) == 0)
		git_buf_puts(stash_message, "(no branch): ");
	else
		git_buf_printf(
			stash_message,
			"%s: ",
			git_reference_name(head) + strlen(GIT_REFS_HEADS_DIR));

	if (git_commit_lookup(b_commit, repo, git_reference_oid(head)) < 0)
		goto cleanup;

	if (append_commit_description(stash_message, *b_commit) < 0)
		goto cleanup;

	error = 0;

cleanup:
	git_reference_free(head);
	return error;
}

static int build_tree_from_index(git_tree **out, git_index *index)
{
	git_oid i_tree_oid;

	if (git_index_write_tree(&i_tree_oid, index) < 0)
		return -1;

	return git_tree_lookup(out, git_index_owner(index), &i_tree_oid);
}

static int commit_index(
	git_commit **i_commit,
	git_index *index,
	git_signature *stasher,
	const char *message,
	const git_commit *parent)
{
	git_tree *i_tree = NULL;
	git_oid i_commit_oid;
	git_buf msg = GIT_BUF_INIT;
	int error = -1;

	if (build_tree_from_index(&i_tree, index) < 0)
		goto cleanup;

	if (git_buf_printf(&msg, "index on %s\n", message) < 0)
		goto cleanup;

	if (git_commit_create(
		&i_commit_oid,
		git_index_owner(index),
		NULL,
		stasher,
		stasher,
		NULL,
		git_buf_cstr(&msg),
		i_tree,
		1,
		&parent) < 0)
			goto cleanup;

	error = git_commit_lookup(i_commit, git_index_owner(index), &i_commit_oid);

cleanup:
	git_tree_free(i_tree);
	git_buf_free(&msg);
	return error;
}

struct cb_data {
	git_index *index;

	bool include_changed;
	bool include_untracked;
	bool include_ignored;
};

static int update_index_cb(
	void *cb_data,
	const git_diff_delta *delta,
	float progress)
{
	int pos;
	struct cb_data *data = (struct cb_data *)cb_data;

	GIT_UNUSED(progress);

	switch (delta->status) {
	case GIT_DELTA_IGNORED:
		if (!data->include_ignored)
			break;

		return git_index_add_from_workdir(data->index, delta->new_file.path);

	case GIT_DELTA_UNTRACKED:
		if (!data->include_untracked)
			break;

		return git_index_add_from_workdir(data->index, delta->new_file.path);

	case GIT_DELTA_ADDED:
		/* Fall through */
	case GIT_DELTA_MODIFIED:
		if (!data->include_changed)
			break;

		return git_index_add_from_workdir(data->index, delta->new_file.path);

	case GIT_DELTA_DELETED:
		if (!data->include_changed)
			break;

		if ((pos = git_index_find(data->index, delta->new_file.path)) < 0)
			return -1;

		if (git_index_remove(data->index, delta->new_file.path, 0) < 0)
			return -1;

	default:
		/* Unimplemented */
		giterr_set(
			GITERR_INVALID,
			"Cannot update index. Unimplemented status kind (%d)",
			delta->status);
		return -1;
	}

	return 0;
}

static int build_untracked_tree(
	git_tree **tree_out,
	git_index *index,
	git_commit *i_commit,
	uint32_t flags)
{
	git_tree *i_tree = NULL;
	git_diff_list *diff = NULL;
	git_diff_options opts = {0};
	struct cb_data data = {0};
	int error = -1;

	git_index_clear(index);

	data.index = index;

	if (flags & GIT_STASH_INCLUDE_UNTRACKED) {
		opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
		data.include_untracked = true;
	}

	if (flags & GIT_STASH_INCLUDE_IGNORED) {
		opts.flags |= GIT_DIFF_INCLUDE_IGNORED;
		data.include_ignored = true;
	}

	if (git_commit_tree(&i_tree, i_commit) < 0)
		goto cleanup;

	if (git_diff_workdir_to_tree(&diff, git_index_owner(index), i_tree, &opts) < 0)
		goto cleanup;

	if (git_diff_foreach(diff, &data, update_index_cb, NULL, NULL) < 0)
		goto cleanup;

	if (build_tree_from_index(tree_out, index) < 0)
		goto cleanup;

	error = 0;

cleanup:
	git_diff_list_free(diff);
	git_tree_free(i_tree);
	return error;
}

static int commit_untracked(
	git_commit **u_commit,
	git_index *index,
	git_signature *stasher,
	const char *message,
	git_commit *i_commit,
	uint32_t flags)
{
	git_tree *u_tree = NULL;
	git_oid u_commit_oid;
	git_buf msg = GIT_BUF_INIT;
	int error = -1;

	if (build_untracked_tree(&u_tree, index, i_commit, flags) < 0)
		goto cleanup;

	if (git_buf_printf(&msg, "untracked files on %s\n", message) < 0)
		goto cleanup;

	if (git_commit_create(
		&u_commit_oid,
		git_index_owner(index),
		NULL,
		stasher,
		stasher,
		NULL,
		git_buf_cstr(&msg),
		u_tree,
		0,
		NULL) < 0)
			goto cleanup;

	error = git_commit_lookup(u_commit, git_index_owner(index), &u_commit_oid);

cleanup:
	git_tree_free(u_tree);
	git_buf_free(&msg);
	return error;
}

static int build_workdir_tree(
	git_tree **tree_out,
	git_index *index,
	git_commit *b_commit)
{
	git_repository *repo = git_index_owner(index);
	git_tree *b_tree = NULL;
	git_diff_list *diff = NULL, *diff2 = NULL;
	git_diff_options opts = {0};
	struct cb_data data = {0};
	int error = -1;

	if (git_commit_tree(&b_tree, b_commit) < 0)
		goto cleanup;

	if (git_diff_index_to_tree(&diff, repo, b_tree, NULL, &opts) < 0)
		goto cleanup;

	if (git_diff_workdir_to_index(&diff2, repo, NULL, &opts) < 0)
		goto cleanup;

	if (git_diff_merge(diff, diff2) < 0)
		goto cleanup;

	data.index = index;
	data.include_changed = true;

	if (git_diff_foreach(diff, &data, update_index_cb, NULL, NULL) < 0)
		goto cleanup;

	if (build_tree_from_index(tree_out, index) < 0)
		goto cleanup;

	error = 0;

cleanup:
	git_diff_list_free(diff);
	git_diff_list_free(diff2);
	git_tree_free(b_tree);
	return error;
}

static int commit_worktree(
	git_oid *w_commit_oid,
	git_index *index,
	git_signature *stasher,
	const char *message,
	git_commit *i_commit,
	git_commit *b_commit,
	git_commit *u_commit)
{
	git_tree *w_tree = NULL, *i_tree = NULL;
	int error = -1;

	const git_commit *parents[] = {	NULL, NULL,	NULL };

	parents[0] = b_commit;
	parents[1] = i_commit;
	parents[2] = u_commit;

	if (git_commit_tree(&i_tree, i_commit) < 0)
		return -1;

	if (git_index_read_tree(index, i_tree) < 0)
		goto cleanup;

	if (build_workdir_tree(&w_tree, index, b_commit) < 0)
		goto cleanup;

	if (git_commit_create(
		w_commit_oid,
		git_index_owner(index),
		NULL,
		stasher,
		stasher,
		NULL,
		message,
		w_tree,
		u_commit ? 3 : 2, parents) < 0)
			goto cleanup;

	error = 0;

cleanup:
	git_tree_free(i_tree);
	git_tree_free(w_tree);
	return error;
}

static int prepare_worktree_commit_message(
	git_buf* msg,
	const char *user_message)
{
	git_buf buf = GIT_BUF_INIT;
	int error = -1;

	git_buf_set(&buf, git_buf_cstr(msg), git_buf_len(msg));
	git_buf_clear(msg);

	if (!user_message)
		git_buf_printf(msg, "WIP on %s", git_buf_cstr(&buf));
	else {
		const char *colon;

		if ((colon = strchr(git_buf_cstr(&buf), ':')) == NULL)
			goto cleanup;

		git_buf_puts(msg, "On ");
		git_buf_put(msg, git_buf_cstr(&buf), colon - buf.ptr);
		git_buf_printf(msg, ": %s\n", user_message);
	}

	error = git_buf_oom(msg) || git_buf_oom(&buf) ? -1 : 0;

cleanup:
	git_buf_free(&buf);
	return error;
}

static int update_reflog(
	git_oid *w_commit_oid,
	git_repository *repo,
	git_signature *stasher,
	const char *message)
{
	git_reference *stash = NULL;
	git_reflog *reflog = NULL;
	int error;

	if ((error = git_reference_create_oid(&stash, repo, GIT_REFS_STASH_FILE, w_commit_oid, 1)) < 0)
		goto cleanup;

	if ((error = git_reflog_read(&reflog, stash)) < 0)
		goto cleanup;

	if ((error = git_reflog_append(reflog, w_commit_oid, stasher, message)) < 0)
		goto cleanup;

	if ((error = git_reflog_write(reflog)) < 0)
		goto cleanup;

	error = 0;

cleanup:
	git_reference_free(stash);
	git_reflog_free(reflog);
	return error;
}

static int is_dirty_cb(const char *path, unsigned int status, void *payload)
{
	GIT_UNUSED(path);
	GIT_UNUSED(status);
	GIT_UNUSED(payload);

	return 1;
}

static int ensure_there_are_changes_to_stash(
	git_repository *repo,
	bool include_untracked_files,
	bool include_ignored_files)
{
	int error;
	git_status_options opts;

	memset(&opts, 0, sizeof(opts));
	opts.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	if (include_untracked_files)
		opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
		GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

	if (include_ignored_files)
		opts.flags = GIT_STATUS_OPT_INCLUDE_IGNORED;

	error = git_status_foreach_ext(repo, &opts, is_dirty_cb, NULL);

	if (error == GIT_EUSER)
		return 0;

	if (!error)
		return create_error(GIT_ENOTFOUND, "There is nothing to stash.");

	return error;
}

static int reset_index_and_workdir(
	git_repository *repo,
	git_commit *commit,
	bool remove_untracked)
{
	git_checkout_opts opts;

	memset(&opts, 0, sizeof(git_checkout_opts));

	opts.checkout_strategy =
		GIT_CHECKOUT_UPDATE_MODIFIED | GIT_CHECKOUT_UPDATE_UNTRACKED;

	if (remove_untracked)
		opts.checkout_strategy |= GIT_CHECKOUT_REMOVE_UNTRACKED;

	return git_checkout_tree(repo, (git_object *)commit, &opts);
}

int git_stash_save(
	git_oid *out,
	git_repository *repo,
	git_signature *stasher,
	const char *message,
	uint32_t flags)
{
	git_index *index = NULL;
	git_commit *b_commit = NULL, *i_commit = NULL, *u_commit = NULL;
	git_buf msg = GIT_BUF_INIT;
	int error;

	assert(out && repo && stasher);

	if ((error = ensure_non_bare_repository(repo)) < 0)
		return error;

	if ((error = retrieve_base_commit_and_message(&b_commit, &msg, repo)) < 0)
		goto cleanup;

	if ((error = ensure_there_are_changes_to_stash(
		repo,
		(flags & GIT_STASH_INCLUDE_UNTRACKED) == GIT_STASH_INCLUDE_UNTRACKED,
		(flags & GIT_STASH_INCLUDE_IGNORED) == GIT_STASH_INCLUDE_IGNORED)) < 0)
		goto cleanup;

	error = -1;

	if (git_repository_index(&index, repo) < 0)
		goto cleanup;

	if (commit_index(&i_commit, index, stasher, git_buf_cstr(&msg), b_commit) < 0)
		goto cleanup;

	if ((flags & GIT_STASH_INCLUDE_UNTRACKED || flags & GIT_STASH_INCLUDE_IGNORED)
		&& commit_untracked(&u_commit, index, stasher, git_buf_cstr(&msg), i_commit, flags) < 0)
		goto cleanup;

	if (prepare_worktree_commit_message(&msg, message) < 0)
		goto cleanup;

	if (commit_worktree(out, index, stasher, git_buf_cstr(&msg), i_commit, b_commit, u_commit) < 0)
		goto cleanup;

	git_buf_rtrim(&msg);
	if (update_reflog(out, repo, stasher, git_buf_cstr(&msg)) < 0)
		goto cleanup;

	if (reset_index_and_workdir(
		repo,
		((flags & GIT_STASH_KEEP_INDEX) == GIT_STASH_KEEP_INDEX) ?
			i_commit : b_commit,
		(flags & GIT_STASH_INCLUDE_UNTRACKED) == GIT_STASH_INCLUDE_UNTRACKED) < 0)
		goto cleanup;

	error = 0;

cleanup:
	git_buf_free(&msg);
	git_commit_free(i_commit);
	git_commit_free(b_commit);
	git_commit_free(u_commit);
	git_index_free(index);
	return error;
}

int git_stash_foreach(
	git_repository *repo,
	stash_cb callback,
	void *payload)
{
	git_reference *stash;
	git_reflog *reflog = NULL;
	int error;
	size_t i, max;
	const git_reflog_entry *entry;

	error = git_reference_lookup(&stash, repo, GIT_REFS_STASH_FILE);
	if (error == GIT_ENOTFOUND)
		return 0;

	if (error < 0)
		goto cleanup;

	if ((error = git_reflog_read(&reflog, stash)) < 0)
		goto cleanup;

	max = git_reflog_entrycount(reflog);
	for (i = 0; i < max; i++) {
		entry = git_reflog_entry_byindex(reflog, max - i - 1);
		
		if (callback(i,
			git_reflog_entry_msg(entry),
			git_reflog_entry_oidnew(entry),
			payload)) {
				error = GIT_EUSER;
				goto cleanup;
		}
	}

	error = 0;

cleanup:
	git_reference_free(stash);
	git_reflog_free(reflog);
	return error;
}

int git_stash_drop(
	git_repository *repo,
	size_t index)
{
	git_reference *stash;
	git_reflog *reflog = NULL;
	size_t max;
	int error;

	if ((error = git_reference_lookup(&stash, repo, GIT_REFS_STASH_FILE)) < 0)
		return error;

	if ((error = git_reflog_read(&reflog, stash)) < 0)
		goto cleanup;

	max = git_reflog_entrycount(reflog);

	if (index > max - 1) {
		error = GIT_ENOTFOUND;
		giterr_set(GITERR_STASH, "No stashed state at position %" PRIuZ, index);
		goto cleanup;
	}

	if ((error = git_reflog_drop(reflog, max - index - 1, true)) < 0)
		goto cleanup;

	if ((error = git_reflog_write(reflog)) < 0)
		goto cleanup;

	if (max == 1) {
		error = git_reference_delete(stash);
		stash = NULL;
	}

cleanup:
	git_reference_free(stash);
	git_reflog_free(reflog);
	return error;
}
