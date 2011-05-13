#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE 1024
#define CORE_PATTERN "core.*"

/*
 * Core file handler
 *
 * Example usage:
 * echo "|/usr/bin/handle_core -e %p -d /home/core -m 10" > \
 * 	/proc/sys/kernel/core_pattern
 */

/* Count the number of core files we have. */
static int count_core_files(const char *core_dir)
{
	struct dirent *de;
	int count = 0;
	DIR *dp = opendir(core_dir);
	if (!dp) {
		int err = errno;
		return -err;
	}

	while (1) {
		de = readdir(dp);
		if (!de)
			break;
		if (fnmatch(CORE_PATTERN, de->d_name, 0) == 0) {
			count++;
		}
	}
	closedir(dp);
	return count;
}

/* Unlink the core which sorts the first in the glob sort order. */
static int unlink_core(const char *core_dir)
{
	int ret;
	char path[PATH_MAX];
	glob_t g;

	snprintf(path, PATH_MAX, "%s/%s", core_dir, CORE_PATTERN);
	ret = glob(path, GLOB_ERR, NULL, &g);
	if (ret != 0) {
		return EIO;
	}
	if (g.gl_pathc == 0)
		return ENOENT;
	if (unlink(g.gl_pathv[0])) {
		globfree(&g);
		return errno;
	}
	globfree(&g);
	return 0;
}

/* Print the new core name into a buffer of size PATH_MAX */
static void get_core_name(const char *core_dir, char *core_name)
{
	struct tm *tm;
	struct tm tm_buf;
	time_t now;
	time(&now);
	tm = localtime_r(&now, &tm_buf);
	snprintf(core_name, PATH_MAX, "%s/core.%d-%lld-%lld_%lld", core_dir, 
			tm->tm_year, (long long)tm->tm_mon, (long long)tm->tm_mday,
			(long long)now);
}

static void usage(void)
{
	fprintf(stderr, "handle_core: userspace core-file handler for Linux\n\
-d <core_dir>			Directory to write core files into\n\
-e <executable-name>		Name of the executable that is core dumping\n\
-h				This help message\n\
-m <max_cores>			This maximum number of core files to allow\n\
				before deleting older core files.\n\
");
}

static void parse_options(int argc, char **argv, int *max_cores,
			  char **exe_name, char **core_dir)
{
	int c;
	*max_cores = 10;
	*exe_name = NULL;
	*core_dir = "/var/core";
	while ((c = getopt(argc, argv, "d:hm:")) != -1) {
		switch (c) {
		case 'd':
			*core_dir = optarg;
			break;
		case 'e':
			*exe_name = optarg;
			break;
		case 'h':
			usage();
			exit(0);
			break;
		case 'm':
			*max_cores = atoi(optarg);
			if (*max_cores == 0) {
				fprintf(stderr, "handle_core: invalid argument "
					"for max_cores: %s. Please give a number "
					"greater than 0.\n", optarg);
				exit(1);
			}
			break;
		default:
			fprintf(stderr, "handle_core: invalid usage\n\n");
			usage();
			exit(1);
			break;
		}
	}
	if (*exe_name == NULL) {
		fprintf(stderr, "handle_core: you must supply the executable "
			"name with -e. Try -h for help.\n");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int done = 0;
	char *exe_name, *core_dir;
	char core_name[PATH_MAX];
	FILE *fp;
	int num_coref, num_deleted;
	int max_cores;

	/* Write the core to a file */
	parse_options(argc, argv, &max_cores, &exe_name, &core_dir);
	get_core_name(core_dir, core_name);
	fp = fopen(core_name, "w");
	if (!fp) {
		int err = errno;
		syslog(LOG_USER | LOG_ERR, "unable to open %s: "
		       "error %d (%s)\n", core_name, err, strerror(err));
		return err;
	}
	do {
		size_t res;
		char buf[BUF_SIZE];
		size_t nread = fread(buf, 1, BUF_SIZE, stdin);
		if (nread != BUF_SIZE) {
			if (ferror(fp)) {
				int err = errno;
				syslog(LOG_USER | LOG_ERR, "error reading core "
				       "file from stdin: %d (%s)", err, strerror(err));
				fclose(fp);
				return err;
			}
			else {
				done = 1;
			}
		}
		res = fwrite(buf, 1, nread, fp);
		if (res != nread) {
			int err = errno;
			syslog(LOG_USER | LOG_ERR, "error writing core "
			       "file to %s: %d (%s)", core_name, err, strerror(err));
			fclose(fp);
			return err;
		}
	} while (!done);
	fclose(fp);

	/* Make sure we don't have too many cores sitting around. */
	num_coref = count_core_files(core_dir);
	num_deleted = 0;
	if (num_coref < 0) {
		syslog(LOG_USER | LOG_ERR, "count_core_files failed with error "
		       "code %d\n", num_coref);
		return num_coref;
	}
	while (num_coref > max_cores) {
		int ret = unlink_core(core_dir);
		if (ret) {
			syslog(LOG_USER | LOG_ERR, "unlink_core failed with error "
			       "code %d\n", ret);
		}
		num_coref--;
		num_deleted++;
	}

	syslog(LOG_USER | LOG_ERR, "wrote core %s. Deleted %d extra core%s\n",
	       core_name, num_deleted, ((num_deleted == 1) ? "" : "s"));
	return 0;
}
