/* 
   Unix SMB/CIFS implementation.
   Run subunit tests
   Copyright (C) Jelmer Vernooij 2006
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "includes.h"
#include "system/dir.h"
#include "system/network.h"
#include "system/filesys.h"
#include "torture/ui.h"
#include "torture/torture.h"
#include "torture/proto.h"

struct torture_suite *torture_subunit_suite_create(TALLOC_CTX *mem_ctx,
														 const char *path)
{
	struct torture_suite *suite = talloc_zero(mem_ctx, struct torture_suite);

	suite->path = talloc_strdup(suite, path);
	suite->name = talloc_strdup(suite, strrchr(path, '/')?strrchr(path, '/')+1:
									   path);
	suite->description = talloc_asprintf(suite, "Subunit test %s", suite->name);

	return suite;
}

bool torture_subunit_load_testsuites(const char *directory, bool recursive, 
									struct torture_suite *parent)
{
	DIR *dir;
	struct dirent *entry;
	char *filename;
	bool exists;

	if (parent == NULL)
		parent = torture_root;

	dir = opendir(directory);
	if (dir == NULL)
		return true;

	while((entry = readdir(dir))) {
		struct torture_suite *child;
		if (entry->d_name[0] == '.')
			continue;

		filename = talloc_asprintf(NULL, "%s/%s", directory, entry->d_name);
		
		if (!recursive && directory_exist(filename)) {
			talloc_free(filename);
			continue;
		}

		if (directory_exist(filename)) {
			child = torture_find_suite(parent, entry->d_name);
			exists = (child != NULL);
			if (child == NULL)
				child = torture_suite_create(parent, entry->d_name);
			torture_subunit_load_testsuites(filename, true, child);
		} else {
			child = torture_subunit_suite_create(parent, filename);
			exists = false;
		}

		if (!exists) {
			torture_suite_add_suite(parent, child);
		}

		talloc_free(filename);
	}

	closedir(dir);

	return true;
}

static pid_t piped_child(char* const command[], int *f_stdout, int *f_stderr)
{
	pid_t pid;
	int sock_out[2], sock_err[2];

	if (pipe(sock_out) == -1) {
		DEBUG(0, ("socketpair: %s", strerror(errno)));
		return -1;
	}

	if (pipe(sock_err) == -1) {
		DEBUG(0, ("socketpair: %s", strerror(errno)));
		return -1;
	}

	*f_stdout = sock_out[0];
	*f_stderr = sock_err[0];

	pid = fork();

	if (pid == -1) {
		DEBUG(0, ("fork: %s", strerror(errno)));
		return -1;
	}

	if (pid == 0) {
		close(0);
		close(1);
		close(2);
		close(sock_out[0]);
		close(sock_err[0]);

		open("/dev/null", O_RDONLY);
		dup2(sock_out[1], 1);
		dup2(sock_err[1], 2);
		execvp(command[0], command);
		exit(-1);
	}

	close(sock_out[1]);
	close(sock_err[1]);

	return pid;
}

enum subunit_field { SUBUNIT_TEST, SUBUNIT_SUCCESS, SUBUNIT_FAILURE, 
					 SUBUNIT_SKIP };

static void run_subunit_message(struct torture_context *context,
								enum subunit_field field, 
								const char *name, 
								const char *comment)
{
	struct torture_test test;

	ZERO_STRUCT(test);
	test.name = name;

	switch (field) {
	case SUBUNIT_TEST:
		test.description = comment;
		torture_ui_test_start(context, NULL, &test);
		break;
	case SUBUNIT_FAILURE:
		context->active_test = &test;
		torture_ui_test_result(context, TORTURE_FAIL, comment);
		context->active_test = NULL;
		break;
	case SUBUNIT_SUCCESS:
		context->active_test = &test;
		torture_ui_test_result(context, TORTURE_OK, comment);
		context->active_test = NULL;
		break;
	case SUBUNIT_SKIP:
		context->active_test = &test;
		torture_ui_test_result(context, TORTURE_SKIP, comment);
		context->active_test = NULL;
		break;
	}
}

bool torture_subunit_run_suite(struct torture_context *context, 
					   struct torture_suite *suite)
{
	static char *command[2];
	int fd_out, fd_err;
	pid_t pid;
	size_t size;
	char *p, *q;
	char *comment = NULL;
	char *name = NULL;
	enum subunit_field lastfield;
	int status;
	char buffer[4096];
	size_t offset = 0;
	

	command[0] = talloc_strdup(context, suite->path);
	command[1] = NULL;

	pid = piped_child(command, &fd_out, &fd_err);
	if (pid == -1)
		return false;

	while (1) {
		fd_set fds;
		char *eol;

		FD_ZERO(&fds);

		FD_SET(fd_out, &fds);
		FD_SET(fd_err, &fds);

		if (select(MAX(fd_out,fd_err)+1, &fds, NULL, NULL, NULL) <= 0) break;

		if (FD_ISSET(fd_err, &fds)) {
			size = read(fd_err, buffer+offset, sizeof(buffer) - (offset+1));
			if (size <= 0) break;
			write(2, buffer+offset, size);
			continue;
		}

		if (!FD_ISSET(fd_out, &fds)) continue;

		size = read(fd_out, buffer+offset, sizeof(buffer) - (offset+1));

		if (size <= 0) break;

		buffer[offset+size] = '\0';

		write(1, buffer+offset, size);

		for (p = buffer; p; p = eol+1) {
			eol = strchr(p, '\n');
			if (eol == NULL) 
				break;

			*eol = '\0';

			if (comment != NULL && strcmp(p, "]") == 0) {
				run_subunit_message(context, lastfield, name, comment);
				talloc_free(name); name = NULL;
				talloc_free(comment); comment = NULL;
			} else if (comment != NULL) {
				comment = talloc_append_string(context, comment, p);
			} else {
				q = strchr(p, ':');
				if (q == NULL) {
					torture_comment(context, "Invalid line `%s'\n", p);
					continue;
				}

				*q = '\0';
				if (!strcmp(p, "test")) {
					lastfield = SUBUNIT_TEST;
				} else if (!strcmp(p, "failure")) {
					lastfield = SUBUNIT_FAILURE;
				} else if (!strcmp(p, "success")) {
					lastfield = SUBUNIT_SUCCESS;
				} else if (!strcmp(p, "skip")) {
					lastfield = SUBUNIT_SKIP;
				} else {
					torture_comment(context, "Invalid subunit field `%s'\n", p);
					continue;
				}

				p = q+1;

				name = talloc_strdup(context, p+1);

				q = strrchr(p, '[');
				if (q != NULL) {
					*q = '\0';
					comment = talloc_strdup(context, "");
				} else {
					run_subunit_message(context, lastfield, name, NULL);
					talloc_free(name);
					name = NULL;
				}
			}
		}
		
		offset += size-(p-buffer);
		memcpy(buffer, p, offset);
	}

	if (waitpid(pid, &status, 0) == -1) {
		torture_result(context, TORTURE_ERROR, "waitpid(%d) failed\n", pid);
		return false;
	}

	if (WEXITSTATUS(status) != 0) {
		torture_result(context, TORTURE_ERROR, "failed with status %d\n", WEXITSTATUS(status));
		return false;
	}

	if (name != NULL) {
		torture_result(context, TORTURE_ERROR, "Interrupted during %s\n", name);
		return false;
	}

	return true;
}
