/*
 * Create a squashfs filesystem.  This is a highly compressed read only
 * filesystem.
 *
 * Copyright (c) 2009, 2010, 2012, 2014, 2017, 2019, 2021
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * pseudo.c
 */

#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <ctype.h>

#include "pseudo.h"
#include "mksquashfs_error.h"
#include "progressbar.h"

#define TRUE 1
#define FALSE 0

extern int read_file(char *filename, char *type, int (parse_line)(char *));

struct pseudo_dev **pseudo_file = NULL;
struct pseudo *pseudo = NULL;
int pseudo_count = 0;
static char *destination_file;

static char *get_component(char *target, char **targname)
{
	char *start;

	start = target;
	while(*target != '/' && *target != '\0')
		target ++;

	*targname = strndup(start, target - start);

	while(*target == '/')
		target ++;

	return target;
}


/*
 * Add pseudo device target to the set of pseudo devices.  Pseudo_dev
 * describes the pseudo device attributes.
 */
struct pseudo *add_pseudo(struct pseudo *pseudo, struct pseudo_dev *pseudo_dev,
	char *target, char *alltarget)
{
	char *targname;
	int i;

	target = get_component(target, &targname);

	if(pseudo == NULL) {
		pseudo = malloc(sizeof(struct pseudo));
		if(pseudo == NULL)
			MEM_ERROR();

		pseudo->names = 0;
		pseudo->count = 0;
		pseudo->name = NULL;
	}

	for(i = 0; i < pseudo->names; i++)
		if(strcmp(pseudo->name[i].name, targname) == 0)
			break;

	if(i == pseudo->names) {
		/* allocate new name entry */
		pseudo->names ++;
		pseudo->name = realloc(pseudo->name, (i + 1) *
			sizeof(struct pseudo_entry));
		if(pseudo->name == NULL)
			MEM_ERROR();
		pseudo->name[i].name = targname;

		if(target[0] == '\0') {
			/* at leaf pathname component */
			pseudo->name[i].pseudo = NULL;
			pseudo->name[i].pathname = strdup(alltarget);
			pseudo->name[i].dev = pseudo_dev;
		} else {
			/* recurse adding child components */
			pseudo->name[i].dev = NULL;
			pseudo->name[i].pseudo = add_pseudo(NULL, pseudo_dev,
				target, alltarget);
		}
	} else {
		/* existing matching entry */
		free(targname);

		if(pseudo->name[i].pseudo == NULL) {
			/* No sub-directory which means this is the leaf
			 * component of a pre-existing pseudo file.
			 */
			if(target[0] != '\0') {
				/*
				 * entry must exist as either a 'd' type or
				 * 'm' type pseudo file
				 */
				if(pseudo->name[i].dev->type == 'd' ||
					pseudo->name[i].dev->type == 'm')
					/* recurse adding child components */
					pseudo->name[i].pseudo =
						add_pseudo(NULL, pseudo_dev,
						target, alltarget);
				else {
					ERROR_START("%s already exists as a "
						"non directory.",
						pseudo->name[i].name);
					ERROR_EXIT(".  Ignoring %s!\n",
						alltarget);
				}
			} else if(memcmp(pseudo_dev, pseudo->name[i].dev,
					sizeof(struct pseudo_dev)) != 0) {
				ERROR_START("%s already exists as a different "
					"pseudo definition.", alltarget);
				ERROR_EXIT("  Ignoring!\n");
			} else {
				ERROR_START("%s already exists as an identical "
					"pseudo definition!", alltarget);
				ERROR_EXIT("  Ignoring!\n");
			}
		} else {
			if(target[0] == '\0') {
				/*
				 * sub-directory exists, which means we can only
				 * add a pseudo file of type 'd' or type 'm'
				 */
				if(pseudo->name[i].dev == NULL &&
						(pseudo_dev->type == 'd' ||
						pseudo_dev->type == 'm')) {
					pseudo->name[i].pathname =
						strdup(alltarget);
					pseudo->name[i].dev = pseudo_dev;
				} else {
					ERROR_START("%s already exists as a "
						"different pseudo definition.",
						pseudo->name[i].name);
					ERROR_EXIT("  Ignoring %s!\n",
						alltarget);
				}
			} else
				/* recurse adding child components */
				add_pseudo(pseudo->name[i].pseudo, pseudo_dev,
					target, alltarget);
		}
	}

	return pseudo;
}


/*
 * Find subdirectory in pseudo directory referenced by pseudo, matching
 * filename.  If filename doesn't exist or if filename is a leaf file
 * return NULL
 */
struct pseudo *pseudo_subdir(char *filename, struct pseudo *pseudo)
{
	int i;

	if(pseudo == NULL)
		return NULL;

	for(i = 0; i < pseudo->names; i++)
		if(strcmp(filename, pseudo->name[i].name) == 0)
			return pseudo->name[i].pseudo;

	return NULL;
}


struct pseudo_entry *pseudo_readdir(struct pseudo *pseudo)
{
	if(pseudo == NULL)
		return NULL;

	while(pseudo->count < pseudo->names) {
		if(pseudo->name[pseudo->count].dev != NULL)
			return &pseudo->name[pseudo->count++];
		else
			pseudo->count++;
	}

	return NULL;
}


int pseudo_exec_file(struct pseudo_dev *dev, int *child)
{
	int res, pipefd[2];

	res = pipe(pipefd);
	if(res == -1) {
		ERROR("Executing dynamic pseudo file, pipe failed\n");
		return 0;
	}

	*child = fork();
	if(*child == -1) {
		ERROR("Executing dynamic pseudo file, fork failed\n");
		goto failed;
	}

	if(*child == 0) {
		close(pipefd[0]);
		close(STDOUT_FILENO);
		res = dup(pipefd[1]);
		if(res == -1)
			exit(EXIT_FAILURE);

		execl("/bin/sh", "sh", "-c", dev->file->command, (char *) NULL);
		exit(EXIT_FAILURE);
	}

	close(pipefd[1]);
	return pipefd[0];

failed:
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}


int pseudo_exec_date(char *string, unsigned int *mtime)
{
	int res, pipefd[2], child, status;
	int bytes = 0;
	char buffer[11];

	res = pipe(pipefd);
	if(res == -1) {
		ERROR("Error executing date, pipe failed\n");
		return FALSE;
	}

	child = fork();
	if(child == -1) {
		ERROR("Error executing date, fork failed\n");
		goto failed;
	}

	if(child == 0) {
		close(pipefd[0]);
		close(STDOUT_FILENO);
		res = dup(pipefd[1]);
		if(res == -1)
			exit(EXIT_FAILURE);


		execl("/usr/bin/date", "date", "-d", string, "+%s", (char *) NULL);
		exit(EXIT_FAILURE);
	}

	close(pipefd[1]);

	while(1) {
		res = read_bytes(pipefd[0], buffer, 11);
		if(res == -1) {
			ERROR("Error executing date\n");
			goto failed;
		} else if(res == 0)
			break;

		bytes += res;
	}

	while(1) {
		res = waitpid(child, &status, 0);
		if(res != -1)
			break;
		else if(errno != EINTR) {
			ERROR("Error executing data, waitpid failed\n");
			goto failed;
		}
	}

	close(pipefd[0]);

	if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		ERROR("Error executing date, failed to parse date string\n");
		goto failed;
	}

	if(bytes == 0 || bytes > 11) {
		ERROR("Error executing date, unexpected result\n");
		goto failed;
	}

	/* replace trailing newline with string terminator */
	buffer[bytes - 1] = '\0';

	res = sscanf(buffer, "%u", mtime);

	if(res < 1) {
		ERROR("Error, unexpected result from date\n");
		goto failed;
	}

	return TRUE;;
failed:
	close(pipefd[0]);
	close(pipefd[1]);
	return FALSE;
}


void add_pseudo_file(struct pseudo_dev *dev, char *command)
{
	pseudo_file = realloc(pseudo_file, (pseudo_count + 1) *
		sizeof(struct pseudo_dev *));
	if(pseudo_file == NULL)
		MEM_ERROR();

	dev->file = malloc(sizeof(struct pseudo_dev_com));
	if(dev->file == NULL)
		MEM_ERROR();

	dev->file->command = strdup(command);
	dev->file->pseudo_id = pseudo_count;
	pseudo_file[pseudo_count ++] = dev;
}


struct pseudo_dev *get_pseudo_file(int pseudo_id)
{
	return pseudo_file[pseudo_id];
}


struct pseudo_entry *pseudo_lookup(struct pseudo *pseudo, char *target)
{
	char *targname;
	int i;

	if(pseudo == NULL)
		return NULL;

	target = get_component(target, &targname);

	for(i = 0; i < pseudo->names; i++)
		if(strcmp(pseudo->name[i].name, targname) == 0)
			break;

	free(targname);

	if(i == pseudo->names)
		return NULL;

	if(target[0] == '\0')
		return &pseudo->name[i];

	if(pseudo->name[i].pseudo == NULL)
		return NULL;

	return pseudo_lookup(pseudo->name[i].pseudo, target);
}


static void print_definitions()
{
	ERROR("Pseudo definitions should be of the format\n");
	ERROR("\tfilename d mode uid gid\n");
	ERROR("\tfilename m mode uid gid\n");
	ERROR("\tfilename b mode uid gid major minor\n");
	ERROR("\tfilename c mode uid gid major minor\n");
	ERROR("\tfilename f mode uid gid command\n");
	ERROR("\tfilename s mode uid gid symlink\n");
	ERROR("\tfilename l filename\n");
	ERROR("\tfilename L pseudo_filename\n");
	ERROR("\tfilename D time mode uid gid\n");
	ERROR("\tfilename M time mode uid gid\n");
	ERROR("\tfilename B time mode uid gid major minor\n");
	ERROR("\tfilename C time mode uid gid major minor\n");
	ERROR("\tfilename F time mode uid gid command\n");
	ERROR("\tfilename S time mode uid gid symlink\n");
}


static int read_pseudo_def_pseudo_link(char *orig_def, char *filename, char *name, char *def)
{
	char *linkname, *link;
	int quoted = FALSE;
	struct pseudo_entry *pseudo_ent;

	/*
	 * Scan for filename, don't use sscanf() and "%s" because
	 * that can't handle filenames with spaces.
	 *
	 * Filenames with spaces should either escape (backslash) the
	 * space or use double quotes.
	 */
	linkname = malloc(strlen(def) + 1);
	if(linkname == NULL)
		MEM_ERROR();

	for(link = linkname; (quoted || !isspace(*def)) && *def != '\0';) {
		if(*def == '"') {
			quoted = !quoted;
			def ++;
			continue;
		}

		if(*def == '\\') {
			def ++;
			if (*def == '\0')
				break;
		}
		*link ++ = *def ++;
	}
	*link = '\0';

	/* Skip any leading slashes (/) */
	for(link = linkname; *link == '/'; link ++);

	if(*link == '\0') {
		ERROR("Not enough or invalid arguments in pseudo LINK file "
			"definition \"%s\"\n", orig_def);
		goto error;
	}

	/* Lookup linkname in pseudo definition tree */
	pseudo_ent = pseudo_lookup(pseudo, link);

	if(pseudo_ent == NULL) {
		ERROR("Pseudo LINK file %s doesn't exist\n", linkname);
		goto error;
	}

	if(pseudo_ent->dev->type == 'd' || pseudo_ent->dev->type == 'm') {
		ERROR("Cannot hardlink to a Pseudo directory or modify definition\n");
		goto error;
	}

	pseudo = add_pseudo(pseudo, pseudo_ent->dev, name, name);

	free(filename);
	free(linkname);
	return TRUE;

error:
	print_definitions();
	free(filename);
	free(linkname);
	return FALSE;
}


static int read_pseudo_def_link(char *orig_def, char *filename, char *name, char *def)
{
	char *linkname, *link;
	int quoted = FALSE;
	struct pseudo_dev *dev = NULL;
	static struct stat *dest_buf = NULL;

	/*
	 * Stat destination file.  We need to do this to prevent people
	 * from creating a circular loop, connecting the output to the
	 * input (only needed for appending, otherwise the destination
	 * file will not exist).
	 */
	if(dest_buf == NULL) {
		dest_buf = malloc(sizeof(struct stat));
		if(dest_buf == NULL)
			MEM_ERROR();

		memset(dest_buf, 0, sizeof(struct stat));
		lstat(destination_file, dest_buf);
	}


	/*
	 * Scan for filename, don't use sscanf() and "%s" because
	 * that can't handle filenames with spaces.
	 *
	 * Filenames with spaces should either escape (backslash) the
	 * space or use double quotes.
	 */
	linkname = malloc(strlen(def) + 1);
	if(linkname == NULL)
		MEM_ERROR();

	for(link = linkname; (quoted || !isspace(*def)) && *def != '\0';) {
		if(*def == '"') {
			quoted = !quoted;
			def ++;
			continue;
		}

		if(*def == '\\') {
			def ++;
			if (*def == '\0')
				break;
		}
		*link ++ = *def ++;
	}
	*link = '\0';

	if(*linkname == '\0') {
		ERROR("Not enough or invalid arguments in pseudo link file "
			"definition \"%s\"\n", orig_def);
		goto error;
	}

	dev = malloc(sizeof(struct pseudo_dev));
	if(dev == NULL)
		MEM_ERROR();

	memset(dev, 0, sizeof(struct pseudo_dev));

	dev->linkbuf = malloc(sizeof(struct stat));
	if(dev->linkbuf == NULL)
		MEM_ERROR();

	if(lstat(linkname, dev->linkbuf) == -1) {
		ERROR("Cannot stat pseudo link file %s because %s\n",
			linkname, strerror(errno));
		goto error;
	}

	if(S_ISDIR(dev->linkbuf->st_mode)) {
		ERROR("Pseudo link file %s is a directory, ", linkname);
		ERROR("which cannot be hardlinked to\n");
		goto error;
	}

	if(S_ISREG(dev->linkbuf->st_mode)) {
		/*
		 * Check we're not trying to create a circular loop,
		 * connecting the output destination file to the
		 * input
		 */
		if(memcmp(dev->linkbuf, dest_buf, sizeof(struct stat)) == 0) {
			ERROR("Pseudo link file %s is the ", linkname);
			ERROR("destination output file, which cannot be linked to\n");
			goto error;
		}
	}

	dev->type = 'l';
	dev->linkname = strdup(linkname);

	pseudo = add_pseudo(pseudo, dev, name, name);

	free(filename);
	free(linkname);
	return TRUE;

error:
	print_definitions();
	if(dev)
		free(dev->linkbuf);
	free(dev);
	free(filename);
	free(linkname);
	return FALSE;
}


static int read_pseudo_def_extended(char type, char *orig_def, char *filename, char *name, char *def)
{
	int n, bytes;
	int quoted = FALSE;
	unsigned int major = 0, minor = 0, mode, mtime;
	char *ptr, *str, *string;
	char suid[100], sgid[100]; /* overflow safe */
	long long uid, gid;
	struct pseudo_dev *dev;
	static int pseudo_ino = 1;

	n = sscanf(def, "%u %o %n", &mtime, &mode, &bytes);

	if(n < 2) {
		/*
		 * Couldn't match date and mode.  Date may not be quoted
		 * and is instead using backslashed spaces (i.e. 1\ jan\ 1980)
		 * where the "1" matched for the integer, but, jan didn't for
		 * the octal number.
		 *
		 * Scan for date string, don't use sscanf() and "%s" because
		 * that can't handle strings with spaces.
		 *
		 * Strings with spaces should either escape (backslash) the
		 * space or use double quotes.
		 */
		string = malloc(strlen(def) + 1);
		if(string == NULL)
			MEM_ERROR();

		for(str = string; (quoted || !isspace(*def)) && *def != '\0';) {
			if(*def == '"') {
				quoted = !quoted;
				def ++;
				continue;
			}

			if(*def == '\\') {
				def ++;
				if (*def == '\0')
					break;
			}
			*str++ = *def ++;
		}
		*str = '\0';

		if(string[0] == '\0') {
			ERROR("Not enough or invalid arguments in pseudo file "
				"definition \"%s\"\n", orig_def);
			free(string);
			goto error;
		}

		n = pseudo_exec_date(string, &mtime);
		if(n == FALSE) {
				ERROR("Couldn't parse time, date string or "
					"unsigned decimal integer "
					"expected\n");
			free(string);
			goto error;
		}

		n = sscanf(def, "%o %99s %99s %n", &mode, suid, sgid, &bytes);
		def += bytes;
		if(n < 3) {
			ERROR("Not enough or invalid arguments in pseudo file "
				"definition \"%s\"\n", orig_def);
			switch(n) {
			case -1:
			/* FALLTHROUGH */
			case 0:
				ERROR("Couldn't parse mode, octal integer expected\n");
				break;
			case 1:
				ERROR("Read filename, type, time and mode, but failed to "
					"read or match uid\n");
				break;
			default:
				ERROR("Read filename, type, time, mode and uid, but failed "
					"to read or match gid\n");
				break;
			}
			goto error;
		}
	} else {
		def += bytes;
		n = sscanf(def, "%99s %99s %n", suid, sgid, &bytes);
		def += bytes;

		if(n < 2) {
			ERROR("Not enough or invalid arguments in pseudo file "
				"definition \"%s\"\n", orig_def);
			switch(n) {
			case -1:
				/* FALLTHROUGH */
			case 0:
				ERROR("Read filename, type, time and mode, but failed to "
					"read or match uid\n");
				break;
			default:
				ERROR("Read filename, type, time, mode and uid, but failed "
					"to read or match gid\n");
				break;
			}
			goto error;
		}
	}

	switch(type) {
	case 'B':
		/* FALLTHROUGH */
	case 'C':
		n = sscanf(def, "%u %u %n", &major, &minor, &bytes);
		def += bytes;

		if(n < 2) {
			ERROR("Not enough or invalid arguments in %s device "
				"pseudo file definition \"%s\"\n", type == 'B' ?
				"block" : "character", orig_def);
			if(n < 1)
				ERROR("Read filename, type, time, mode, uid and "
					"gid, but failed to read or match major\n");
			else
				ERROR("Read filename, type, time, mode, uid, gid "
					"and major, but failed to read  or "
					"match minor\n");
			goto error;
		}

		if(major > 0xfff) {
			ERROR("Major %d out of range\n", major);
			goto error;
		}

		if(minor > 0xfffff) {
			ERROR("Minor %d out of range\n", minor);
			goto error;
		}
		/* FALLTHROUGH */
	case 'D':
		/* FALLTHROUGH */
	case 'M':
		/*
		 * Check for trailing junk after expected arguments
		 */
		if(def[0] != '\0') {
			ERROR("Unexpected tailing characters in pseudo file "
				"definition \"%s\"\n", orig_def);
			goto error;
		}
		break;
	case 'F':
		if(def[0] == '\0') {
			ERROR("Not enough arguments in dynamic file pseudo "
				"definition \"%s\"\n", orig_def);
			ERROR("Expected command, which can be an executable "
				"or a piece of shell script\n");
			goto error;
		}
		break;
	case 'S':
		if(def[0] == '\0') {
			ERROR("Not enough arguments in symlink pseudo "
				"definition \"%s\"\n", orig_def);
			ERROR("Expected symlink\n");
			goto error;
		}

		if(strlen(def) > 65535) {
			ERROR("Symlink pseudo definition %s is greater than 65535"
								" bytes!\n", def);
			goto error;
		}
		break;
	default:
		ERROR("Unsupported type %c\n", type);
		goto error;
	}


	if(mode > 07777) {
		ERROR("Mode %o out of range\n", mode);
		goto error;
	}

	uid = strtoll(suid, &ptr, 10);
	if(*ptr == '\0') {
		if(uid < 0 || uid > ((1LL << 32) - 1)) {
			ERROR("Uid %s out of range\n", suid);
			goto error;
		}
	} else {
		struct passwd *pwuid = getpwnam(suid);
		if(pwuid)
			uid = pwuid->pw_uid;
		else {
			ERROR("Uid %s invalid uid or unknown user\n", suid);
			goto error;
		}
	}

	gid = strtoll(sgid, &ptr, 10);
	if(*ptr == '\0') {
		if(gid < 0 || gid > ((1LL << 32) - 1)) {
			ERROR("Gid %s out of range\n", sgid);
			goto error;
		}
	} else {
		struct group *grgid = getgrnam(sgid);
		if(grgid)
			gid = grgid->gr_gid;
		else {
			ERROR("Gid %s invalid uid or unknown user\n", sgid);
			goto error;
		}
	}

	switch(type) {
	case 'B':
		mode |= S_IFBLK;
		break;
	case 'C':
		mode |= S_IFCHR;
		break;
	case 'D':
		mode |= S_IFDIR;
		break;
	case 'F':
		mode |= S_IFREG;
		break;
	case 'S':
		/* permissions on symlinks are always rwxrwxrwx */
		mode = 0777 | S_IFLNK;
		break;
	}

	dev = malloc(sizeof(struct pseudo_dev));
	if(dev == NULL)
		MEM_ERROR();

	dev->buf = malloc(sizeof(struct pseudo_stat));
	if(dev->buf == NULL)
		MEM_ERROR();

	dev->type = type == 'M' ? 'M' : tolower(type);
	dev->buf->mode = mode;
	dev->buf->uid = uid;
	dev->buf->gid = gid;
	dev->buf->major = major;
	dev->buf->minor = minor;
	dev->buf->mtime = mtime;
	dev->buf->ino = pseudo_ino ++;

	if(type == 'F')
		add_pseudo_file(dev, def);

	if(type == 'S')
		dev->symlink = strdup(def);

	pseudo = add_pseudo(pseudo, dev, name, name);

	free(filename);
	return TRUE;

error:
	print_definitions();
	return FALSE;
}


static int read_pseudo_def_original(char type, char *orig_def, char *filename, char *name, char *def)
{
	int n, bytes;
	unsigned int major = 0, minor = 0, mode;
	char *ptr;
	char suid[100], sgid[100]; /* overflow safe */
	long long uid, gid;
	struct pseudo_dev *dev;
	static int pseudo_ino = 1;

	n = sscanf(def, "%o %99s %99s %n", &mode, suid, sgid, &bytes);
	def += bytes;

	if(n < 3) {
		ERROR("Not enough or invalid arguments in pseudo file "
			"definition \"%s\"\n", orig_def);
		switch(n) {
		case -1:
			/* FALLTHROUGH */
		case 0:
			/* FALLTHROUGH */
		case 1:
			ERROR("Couldn't parse filename, type or octal mode\n");
			ERROR("If the filename has spaces, either quote it, or "
				"backslash the spaces\n");
			break;
		case 2:
			ERROR("Read filename, type and mode, but failed to "
				"read or match uid\n");
			break;
		default:
			ERROR("Read filename, type, mode and uid, but failed "
				"to read or match gid\n");
			break; 
		}
		goto error;
	}

	switch(type) {
	case 'b':
		/* FALLTHROUGH */
	case 'c':
		n = sscanf(def, "%u %u %n", &major, &minor, &bytes);
		def += bytes;

		if(n < 2) {
			ERROR("Not enough or invalid arguments in %s device "
				"pseudo file definition \"%s\"\n", type == 'b' ?
				"block" : "character", orig_def);
			if(n < 1)
				ERROR("Read filename, type, mode, uid and gid, "
					"but failed to read or match major\n");
			else
				ERROR("Read filename, type, mode, uid, gid "
					"and major, but failed to read  or "
					"match minor\n");
			goto error;
		}	
		
		if(major > 0xfff) {
			ERROR("Major %d out of range\n", major);
			goto error;
		}

		if(minor > 0xfffff) {
			ERROR("Minor %d out of range\n", minor);
			goto error;
		}
		/* FALLTHROUGH */
	case 'd':
		/* FALLTHROUGH */
	case 'm':
		/*
		 * Check for trailing junk after expected arguments
		 */
		if(def[0] != '\0') {
			ERROR("Unexpected tailing characters in pseudo file "
				"definition \"%s\"\n", orig_def);
			goto error;
		}
		break;
	case 'f':
		if(def[0] == '\0') {
			ERROR("Not enough arguments in dynamic file pseudo "
				"definition \"%s\"\n", orig_def);
			ERROR("Expected command, which can be an executable "
				"or a piece of shell script\n");
			goto error;
		}	
		break;
	case 's':
		if(def[0] == '\0') {
			ERROR("Not enough arguments in symlink pseudo "
				"definition \"%s\"\n", orig_def);
			ERROR("Expected symlink\n");
			goto error;
		}

		if(strlen(def) > 65535) {
			ERROR("Symlink pseudo definition %s is greater than 65535"
								" bytes!\n", def);
			goto error;
		}
		break;
	default:
		ERROR("Unsupported type %c\n", type);
		goto error;
	}


	if(mode > 07777) {
		ERROR("Mode %o out of range\n", mode);
		goto error;
	}

	uid = strtoll(suid, &ptr, 10);
	if(*ptr == '\0') {
		if(uid < 0 || uid > ((1LL << 32) - 1)) {
			ERROR("Uid %s out of range\n", suid);
			goto error;
		}
	} else {
		struct passwd *pwuid = getpwnam(suid);
		if(pwuid)
			uid = pwuid->pw_uid;
		else {
			ERROR("Uid %s invalid uid or unknown user\n", suid);
			goto error;
		}
	}
		
	gid = strtoll(sgid, &ptr, 10);
	if(*ptr == '\0') {
		if(gid < 0 || gid > ((1LL << 32) - 1)) {
			ERROR("Gid %s out of range\n", sgid);
			goto error;
		}
	} else {
		struct group *grgid = getgrnam(sgid);
		if(grgid)
			gid = grgid->gr_gid;
		else {
			ERROR("Gid %s invalid uid or unknown user\n", sgid);
			goto error;
		}
	}

	switch(type) {
	case 'b':
		mode |= S_IFBLK;
		break;
	case 'c':
		mode |= S_IFCHR;
		break;
	case 'd':
		mode |= S_IFDIR;
		break;
	case 'f':
		mode |= S_IFREG;
		break;
	case 's':
		/* permissions on symlinks are always rwxrwxrwx */
		mode = 0777 | S_IFLNK;
		break;
	}

	dev = malloc(sizeof(struct pseudo_dev));
	if(dev == NULL)
		MEM_ERROR();

	dev->buf = malloc(sizeof(struct pseudo_stat));
	if(dev->buf == NULL)
		MEM_ERROR();

	dev->type = type;
	dev->buf->mode = mode;
	dev->buf->uid = uid;
	dev->buf->gid = gid;
	dev->buf->major = major;
	dev->buf->minor = minor;
	dev->buf->mtime = time(NULL);
	dev->buf->ino = pseudo_ino ++;

	if(type == 'f')
		add_pseudo_file(dev, def);

	if(type == 's')
		dev->symlink = strdup(def);

	pseudo = add_pseudo(pseudo, dev, name, name);

	free(filename);
	return TRUE;

error:
	print_definitions();
	free(filename);
	return FALSE;
}


static int read_pseudo_def(char *def)
{
	int n, bytes;
	int quoted = 0;
	char type;
	char *filename, *name;
	char *orig_def = def;

	/*
	 * Scan for filename, don't use sscanf() and "%s" because
	 * that can't handle filenames with spaces.
	 *
	 * Filenames with spaces should either escape (backslash) the
	 * space or use double quotes.
	 */
	filename = malloc(strlen(def) + 1);
	if(filename == NULL)
		MEM_ERROR();

	for(name = filename; (quoted || !isspace(*def)) && *def != '\0';) {
		if(*def == '"') {
			quoted = !quoted;
			def ++;
			continue;
		}

		if(*def == '\\') {
			def ++;
			if (*def == '\0')
				break;
		}
		*name ++ = *def ++;
	}
	*name = '\0';

	/* Skip any leading slashes (/) */
	for(name = filename; *name == '/'; name ++);

	if(*name == '\0') {
		ERROR("Not enough or invalid arguments in pseudo file "
			"definition \"%s\"\n", orig_def);
		goto error;
	}

	n = sscanf(def, " %c %n", &type, &bytes);
	def += bytes;

	if(n < 1) {
		ERROR("Not enough or invalid arguments in pseudo file "
			"definition \"%s\"\n", orig_def);
		goto error;
	}

	if(type == 'l')
		return read_pseudo_def_link(orig_def, filename, name, def);
	else if(type == 'L')
		return read_pseudo_def_pseudo_link(orig_def, filename, name, def);
	else if(isupper(type))
		return read_pseudo_def_extended(type, orig_def, filename, name, def);
	else
		return read_pseudo_def_original(type, orig_def, filename, name, def);

error:
	print_definitions();
	free(filename);
	return FALSE;
}


int read_pseudo_definition(char *filename, char *destination)
{
	destination_file = destination;

	return read_pseudo_def(filename);
}


int read_pseudo_file(char *filename, char *destination)
{
	destination_file = destination;

	return read_file(filename, "pseudo", read_pseudo_def);
}


struct pseudo *get_pseudo()
{
	return pseudo;
}


#ifdef SQUASHFS_TRACE
static void dump_pseudo(struct pseudo *pseudo, char *string)
{
	int i, res;
	char *path;

	for(i = 0; i < pseudo->names; i++) {
		struct pseudo_entry *entry = &pseudo->name[i];
		if(string) {
			res = asprintf(&path, "%s/%s", string, entry->name);
			if(res == -1)
				BAD_ERROR("asprintf failed in dump_pseudo\n");
		} else
			path = entry->name;
		if(entry->dev)
			ERROR("%s %c 0%o %d %d %d %d\n", path, entry->dev->type,
				entry->dev->mode & ~S_IFMT, entry->dev->uid,
				entry->dev->gid, entry->dev->major,
				entry->dev->minor);
		if(entry->pseudo)
			dump_pseudo(entry->pseudo, path);
		if(string)
			free(path);
	}
}


void dump_pseudos()
{
    if (pseudo)
        dump_pseudo(pseudo, NULL);
}
#else
void dump_pseudos()
{
}
#endif
