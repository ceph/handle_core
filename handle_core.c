#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE 1024
#define CORE_PREFIX "core."
#define CORE_PREFIX_SZ (sizeof(CORE_PREFIX)-1)
#define MAX_CORE_SCAN 500000

/*
 * Core file handler
 *
 * Example usage:
 * echo "|/sbin/handle_core -e %e -d /var/core -m 10 \
 *		-s '/usr/sbin/sendmail -t sysadmin@example.com'" > \
 *			/proc/sys/kernel/core_pattern
 */

/* Compare two core file names. We want reverse alphabetical order */
static int compare_core_file_names(const void *a, const void *b)
{
	const char *ca = *((const char **)a);
	const char *cb = *((const char **)b);
	return strcmp(cb, ca);
}

/* Step through core_dir and delete core files which have old looking names */
static int limit_core_files(const char *core_dir, int max_cores)
{
	int deleted = 0, i, ret, num_cores = 0, alloc_cores = 64;
	DIR *dp;
	char **cores = malloc(sizeof(char*) * alloc_cores);

	if (cores == NULL) {
		ret = -ENOMEM;
		goto done;
	}
	dp = opendir(core_dir);
	if (!dp) {
		ret = -errno;
		goto done;
	}
	/* Scan through core files. If the number of files we're looking at is
	 * getting too large, we content ourselves with just what we've already
	 * scanned. This does mean we could delete newer files than we really
	 * intend. However, we need to avoid allocating a ridiculous amount of
	 * memory.
	 */
	while (1) {
		struct dirent *de = readdir(dp);
		if (!de)
			break;
		/* ignore non-core files */
		if (strncmp(de->d_name, CORE_PREFIX, CORE_PREFIX_SZ))
			continue;
		cores[num_cores] = strdup(de->d_name);
		if (!cores[num_cores])
			break;
		num_cores++;
		if (num_cores > MAX_CORE_SCAN)
			break;
		if (num_cores == alloc_cores) {
			char **newptr = realloc(cores, sizeof(char*) * alloc_cores * 2);
			if (!newptr)
				break;
			cores = newptr;
			alloc_cores *= 2;
		}
	}
	qsort(cores, num_cores, sizeof(char**), compare_core_file_names);
	/* delete core files which are too old. */
	for (i = num_cores - 1; i >= max_cores; i--) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", core_dir, cores[i]);
		if (unlink(path) == 0) {
			deleted++;
			continue;
		}
		/* ignore ENOENT here. We may be racing with another
		 * handle_core process which deleted the old core first. */
		ret = -errno;
		if (ret != -ENOENT) {
			syslog(LOG_USER | LOG_ERR, "unlink(%s) "
				"error: %d (%s)",
				cores[i], ret, strerror(ret));
			goto done;
		}
	}
	ret = deleted;
done:
	if (cores) {
		for (i = 0; i < num_cores; ++i) {
			if (cores[i])
				free(cores[i]);
		}
		free(cores);
	}
	if (dp)
		closedir(dp);
	return ret;
}

/* Print the new core name into a buffer of size PATH_MAX */
static void get_core_name(const char *core_dir, const char *exe_name,
			  char *core_name)
{
	struct tm *tm;
	struct tm tm_buf;
	time_t now;
	time(&now);
	tm = localtime_r(&now, &tm_buf);
	snprintf(core_name, PATH_MAX, "%s/core.%d-%lld-%lld_%lld.%s", core_dir,
			tm->tm_year + 1900, (long long)tm->tm_mon, (long long)tm->tm_mday,
			(long long)now, exe_name);
}

static void usage(void)
{
	fprintf(stderr, "handle_core: userspace core-file handler for Linux\n\
-d <core_dir>			Directory to write core files into\n\
-e <executable-name>		Name of the executable that is core dumping\n\
-h				This help message\n\
-m <max_cores>			This maximum number of core files to allow\n\
				before deleting older core files.\n\
-s <email_command>		Send email using email_command.\n\
				Example: -s '/usr/sbin/sendmail -t sysadmin@example.com'\n\
");
}

static int parse_options(int argc, char **argv, int *max_cores,
			  char **exe_name, char **core_dir, char **email)
{
	int c;
	*max_cores = 10;
	*exe_name = NULL;
	*core_dir = "/var/core";
	*email = NULL;
	while ((c = getopt(argc, argv, "d:e:hm:s:")) != -1) {
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
				return 1;
			}
			break;
		case 's':
			*email = optarg;
			break;
		default:
			fprintf(stderr, "handle_core: invalid usage\n\n");
			return 1;
			break;
		}
	}
	if (*exe_name == NULL) {
		fprintf(stderr, "handle_core: you must supply the executable "
			"name with -e. Try -h for help.\n");
		return 1;
	}
	return 0;
}

int send_mail(const char *exe_name, const char *core_dir,
	      const char *core_name, const char *email)
{
	char hostname[255];
	struct hostent *fqdn;
	const char *fqdn_name;
	FILE *fp;

	if (!email)
		return 0;
	fp = popen(email, "w");
	if (!fp) {
		int err = errno;
		syslog(LOG_USER | LOG_ERR, "popen(%s) error: %d (%s)",
			email, err, strerror(err));
		return err;
	}
	if (gethostname(hostname, sizeof(hostname))) {
		int err = errno;
		syslog(LOG_USER | LOG_ERR, "gethostname error: %d (%s)",
			err, strerror(err));
		snprintf(hostname, sizeof(hostname), "(unknown-host)");
	}
	fqdn = gethostbyname(hostname);
	if (!fqdn) {
		int err = h_errno;
		syslog(LOG_USER | LOG_ERR, "gethostbyname(%s) error: %d",
			hostname, err);
		fqdn_name = hostname;
	}
	else {
		fqdn_name = fqdn->h_name;
	}
	fprintf(fp, "\
Subject: [core_dump] %s crashed on %s\r\n\r\n\
!!!!! Crash encountered on %s !!!!!!!!!\r\n\
executable name: %s\r\n\
core file name: %s/%s\r\n\
", exe_name, hostname, fqdn_name, exe_name, core_dir, core_name);
	fclose(fp);
	return 0;
}

int main(int argc, char **argv)
{
	int max_cores, deleted, ret, done = 0;
	char *exe_name, *core_dir, *email;
	char core_name[PATH_MAX];
	FILE *fp;

	/* Write the core to a file */
	ret = parse_options(argc, argv, &max_cores, &exe_name, &core_dir,
			    &email);
	if (ret) {
		syslog(LOG_USER | LOG_ERR, "parse_options error\n");
		return 1;
	}
	get_core_name(core_dir, exe_name, core_name);
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
	deleted = limit_core_files(core_dir, max_cores);
	if (deleted < 0) {
		syslog(LOG_USER | LOG_ERR, "error limiting number of core "
			"files: %d", deleted);
	}

	ret = send_mail(exe_name, core_dir, core_name, email);
	if (ret) {
		syslog(LOG_USER | LOG_ERR, "send_mail failed with error "
		       "code %d\n", ret);
	}

	syslog(LOG_USER | LOG_ERR, "wrote core %s. Deleted %d extra core%s\n",
	       core_name, deleted, ((deleted == 1) ? "" : "s"));
	return 0;
}
