/*
 * sar and sadf common routines.
 * (C) 1999-2025 by Sebastien GODARD (sysstat <at> orange.fr)
 *
 ***************************************************************************
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published  by  the *
 * Free Software Foundation; either version 2 of the License, or (at  your *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it  will  be  useful,  but *
 * WITHOUT ANY WARRANTY; without the implied warranty  of  MERCHANTABILITY *
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License *
 * for more details.                                                       *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA              *
 ***************************************************************************
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>	/* For STDOUT_FILENO, among others */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <float.h>

#include "version.h"
#include "sa.h"

#ifdef USE_NLS
#include <locale.h>
#include <libintl.h>
#define _(string) gettext(string)
#else
#define _(string) (string)
#endif

int default_file_used = FALSE;
extern struct act_bitmap cpu_bitmap;
extern unsigned int dm_major;

unsigned int hdr_types_nr[] = {FILE_HEADER_ULL_NR, FILE_HEADER_UL_NR, FILE_HEADER_U_NR};
unsigned int act_types_nr[] = {FILE_ACTIVITY_ULL_NR, FILE_ACTIVITY_UL_NR, FILE_ACTIVITY_U_NR};
unsigned int rec_types_nr[] = {RECORD_HEADER_ULL_NR, RECORD_HEADER_UL_NR, RECORD_HEADER_U_NR};
unsigned int extra_desc_types_nr[] = {EXTRA_DESC_ULL_NR, EXTRA_DESC_UL_NR, EXTRA_DESC_U_NR};

/*
 ***************************************************************************
 * Look for activity in array.
 *
 * IN:
 * @act		Array of activities.
 * @act_flag	Activity flag to look for.
 * @stop	TRUE if sysstat should exit when activity is not found.
 *
 * RETURNS:
 * Position of activity in array, or -1 if not found (this may happen when
 * reading data from a system activity file created by another version of
 * sysstat).
 ***************************************************************************
 */
int get_activity_position(struct activity *act[], unsigned int act_flag, int stop)
{
	int i;

	for (i = 0; i < NR_ACT; i++) {
		if (act[i]->id == act_flag)
			return i;
	}

	if (stop) {
		PANIC((int) act_flag);
	}

	return -1;
}

/*
 ***************************************************************************
 * Count number of activities with given option.
 *
 * IN:
 * @act		Array of activities.
 * @option	Option that activities should have to be counted
 *		(eg. AO_COLLECTED...)
 * @count	Set to COUNT_OUTPUTS if each output should be counted for
 *		activities with	multiple outputs.
 *
 * RETURNS:
 * Number of selected activities
 ***************************************************************************
 */
int get_activity_nr(struct activity *act[], unsigned int option, enum count_mode count)
{
	int i, n = 0;
	unsigned int msk;

	for (i = 0; i < NR_ACT; i++) {
		if ((act[i]->options & option) == option) {

			if (HAS_MULTIPLE_OUTPUTS(act[i]->options) && (count == COUNT_OUTPUTS)) {
				for (msk = 1; msk < 0x100; msk <<= 1) {
					if ((act[i]->opt_flags & 0xff) & msk) {
						n++;
					}
				}
			}
			else {
				n++;
			}
		}
	}

	return n;
}

/*
 ***************************************************************************
 * Look for the most recent of saDD and saYYYYMMDD to decide which one to
 * use. If neither exists then use saDD by default.
 *
 * IN:
 * @sa_dir	Directory where standard daily data files are saved.
 * @rectime	Structure containing the current date.
 *
 * OUT:
 * @sa_name	0 to use saDD data files,
 * 		1 to use saYYYYMMDD data files.
 ***************************************************************************
 */
void guess_sa_name(char *sa_dir, struct tm *rectime, int *sa_name)
{
	char filename[MAX_FILE_LEN];
	struct stat sb;
	time_t sa_mtime;
	long nsec;

	/* Use saDD by default */
	*sa_name = 0;

	/* Look for saYYYYMMDD */
	snprintf(filename, sizeof(filename),
		 "%s/sa%04d%02d%02d", sa_dir,
		 rectime->tm_year + 1900,
		 rectime->tm_mon + 1,
		 rectime->tm_mday);
	filename[sizeof(filename) - 1] = '\0';

	if (stat(filename, &sb) < 0)
		/* Cannot find or access saYYYYMMDD, so use saDD */
		return;
	sa_mtime = sb.st_mtime;
#ifndef __APPLE__
	nsec = sb.st_mtim.tv_nsec;
#else
	nsec = sb.st_mtimespec.tv_nsec;
#endif

	/* Look for saDD */
	snprintf(filename, sizeof(filename),
		 "%s/sa%02d", sa_dir,
		 rectime->tm_mday);
	filename[sizeof(filename) - 1] = '\0';

	if (stat(filename, &sb) < 0) {
		/* Cannot find or access saDD, so use saYYYYMMDD */
		*sa_name = 1;
		return;
	}

	if ((sa_mtime > sb.st_mtime) ||
#ifndef __APPLE__
	    ((sa_mtime == sb.st_mtime) && (nsec > sb.st_mtim.tv_nsec))
#else
	    ((sa_mtime == sb.st_mtime) && (nsec > sb.st_mtimespec.tv_nsec))
#endif
	) {
		/* saYYYYMMDD is more recent than saDD, so use it */
		*sa_name = 1;
	}
}

/*
 ***************************************************************************
 * Set current daily data file name.
 *
 * IN:
 * @datafile	If not an empty string then this is the alternate directory
 *		location where daily data files will be saved.
 * @d_off	Day offset (number of days to go back in the past).
 * @sa_name	0 for saDD data files,
 * 		1 for saYYYYMMDD data files,
 * 		-1 if unknown. In this case, will look for the most recent
 * 		of saDD and saYYYYMMDD and use it.
 *
 * OUT:
 * @datafile	Name of daily data file.
 ***************************************************************************
 */
void set_default_file(char *datafile, int d_off, int sa_name)
{
	char sa_dir[MAX_FILE_LEN];
	struct tm rectime = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL};
	int err = 0;

	/* Set directory where daily data files will be saved */
	if (datafile[0]) {
		strncpy(sa_dir, datafile, sizeof(sa_dir));
	}
	else {
		strncpy(sa_dir, SA_DIR, sizeof(sa_dir));
	}
	sa_dir[sizeof(sa_dir) - 1] = '\0';

	get_time(&rectime, d_off);
	if (sa_name < 0) {
		/*
		 * Look for the most recent of saDD and saYYYYMMDD
		 * and use it. If neither exists then use saDD.
		 * sa_name is set accordingly.
		 */
		guess_sa_name(sa_dir, &rectime, &sa_name);
	}
	if (sa_name) {
		/* Using saYYYYMMDD data files */
		err = snprintf(datafile, MAX_FILE_LEN,
			       "%s/sa%04d%02d%02d", sa_dir,
			       rectime.tm_year + 1900,
			       rectime.tm_mon + 1,
			       rectime.tm_mday);
	}
	else {
		/* Using saDD data files */
		err = snprintf(datafile, MAX_FILE_LEN,
			       "%s/sa%02d", sa_dir,
			       rectime.tm_mday);
	}
	datafile[MAX_FILE_LEN - 1] = '\0';

	if ((err < 0) || (err >= MAX_FILE_LEN)) {
		fprintf(stderr, "%s: %s\n", __FUNCTION__, datafile);
		exit(1);
	}

	default_file_used = TRUE;

#ifdef DEBUG
	fprintf(stderr, "%s: Datafile: %s\n", __FUNCTION__, datafile);
#endif
}

/*
 ***************************************************************************
 * Check data file type. If it is a directory then this is the alternate
 * location where daily data files will be saved.
 *
 * IN:
 * @datafile	Name of the daily data file. May be a directory.
 * @d_off	Day offset (number of days to go back in the past).
 * @sa_name	0 for saDD data files,
 * 		1 for saYYYYMMDD data files,
 * 		-1 if unknown. In this case, will look for the most recent
 * 		of saDD and saYYYYMMDD and use it.
 *
 *
 * OUT:
 * @datafile	Name of the daily data file. This is now a plain file, not
 * 		a directory.
 *
 * RETURNS:
 * 1 if @datafile was a directory, and 0 otherwise.
 ***************************************************************************
 */
int check_alt_sa_dir(char *datafile, int d_off, int sa_name)
{
	if (check_dir(datafile)) {
		/*
		 * This is a directory: So append
		 * the default file name to it.
		 */
		set_default_file(datafile, d_off, sa_name);
		return 1;
	}

	return 0;
}

/*
 ***************************************************************************
 * Display sysstat version used to create system activity data file.
 *
 * IN:
 * @st		Output stream (stderr or stdout).
 * @file_magic	File magic header.
 ***************************************************************************
 */
void display_sa_file_version(FILE *st, struct file_magic *file_magic)
{
	fprintf(st, _("File created by sar/sadc from sysstat version %d.%d.%d"),
		file_magic->sysstat_version,
		file_magic->sysstat_patchlevel,
		file_magic->sysstat_sublevel);

	if (file_magic->sysstat_extraversion) {
		fprintf(st, ".%d", file_magic->sysstat_extraversion);
	}
	fprintf(st, "\n");
}

/*
 ***************************************************************************
 * An invalid system activity file has been opened for reading.
 * If this file was created by an old version of sysstat, tell it to the
 * user...
 *
 * IN:
 * @fd		Descriptor of the file that has been opened.
 * @file_magic	file_magic structure filled with file magic header data.
 *		May contain invalid data.
 * @file	Name of the file being read.
 * @n		Number of bytes read while reading file magic header.
 * 		This function may also be called after failing to read file
 *		standard header, or if CPU activity has not been found in
 *		file. In this case, n is set to 0.
 ***************************************************************************
 */
void handle_invalid_sa_file(int fd, struct file_magic *file_magic, char *file,
			    int n)
{
	fprintf(stderr, _("Invalid system activity file: %s\n"), file);

	if (n == FILE_MAGIC_SIZE) {
		if ((file_magic->sysstat_magic == SYSSTAT_MAGIC) || (file_magic->sysstat_magic == SYSSTAT_MAGIC_SWAPPED)) {
			unsigned short fmt_magic;

			/* This is a sysstat file, but this file has an old format */
			display_sa_file_version(stderr, file_magic);

			fmt_magic = file_magic->sysstat_magic == SYSSTAT_MAGIC ?
				    file_magic->format_magic : __builtin_bswap16(file_magic->format_magic);
			fprintf(stderr,
				_("Current sysstat version cannot read the format of this file (%#x)\n"),
				fmt_magic);
			if (fmt_magic >= FORMAT_MAGIC_2171) {
				fprintf(stderr,
					_("Try to convert it to current format. Enter:\n\n"));
				fprintf(stderr, "sadf -c %s > %s.new\n\n", file, file);
				fprintf(stderr,
					_("You should then be able to read the new file created (%s.new)\n"),
					file);
			}
		}
	}

	close (fd);
	exit(3);
}

/*
 ***************************************************************************
 * Display an error message then exit.
 ***************************************************************************
 */
void print_collect_error(void)
{
	fprintf(stderr, _("Requested activities not available\n"));
	exit(1);
}

/*
 ***************************************************************************
 * Fill system activity file magic header.
 *
 * IN:
 * @file_magic	System activity file magic header.
 ***************************************************************************
 */
void enum_version_nr(struct file_magic *fm)
{
	char *v;
	char version[16];

	fm->sysstat_extraversion = 0;

	strcpy(version, VERSION);

	/* Get version number */
	if ((v = strtok(version, ".")) == NULL)
		return;
	fm->sysstat_version = atoi(v) & 0xff;

	/* Get patchlevel number */
	if ((v = strtok(NULL, ".")) == NULL)
		return;
	fm->sysstat_patchlevel = atoi(v) & 0xff;

	/* Get sublevel number */
	if ((v = strtok(NULL, ".")) == NULL)
		return;
	fm->sysstat_sublevel = atoi(v) & 0xff;

	/* Get extraversion number. Don't necessarily exist */
	if ((v = strtok(NULL, ".")) == NULL)
		return;
	fm->sysstat_extraversion = atoi(v) & 0xff;
}

/*
 ***************************************************************************
 * Write data to file. If the write() call was interrupted by a signal, try
 * again so that the whole buffer can be written.
 *
 * IN:
 * @fd		Output file descriptor.
 * @buf		Data buffer.
 * @nr_bytes	Number of bytes to write.
 *
 * RETURNS:
 * Number of bytes written to file, or -1 on error.
 ***************************************************************************
 */
int write_all(int fd, const void *buf, int nr_bytes)
{
	int block, offset = 0;
	char *buffer = (char *) buf;

	while (nr_bytes > 0) {

		block = write(fd, &buffer[offset], nr_bytes);

		if (block < 0) {
			if (errno == EINTR)
				continue;
			return block;
		}
		if (block == 0)
			return offset;

		offset += block;
		nr_bytes -= block;
	}

	return offset;
}

#ifndef SOURCE_SADC
/*
 * **************************************************************************
 * Init buffers for min and max values.
 *
 * IN:
 * @a		Activity for which buffers are to be initialized.
 * @start_slot	First slot to init.
 * @nr_alloc	Number of slots to init.
 ***************************************************************************
 */
void init_minmax_buf(struct activity *a, size_t start_slot, size_t nr_alloc)
{
	int j;
	double *val;

	for (j = start_slot * a->nr2 * a->xnr;
	     j < nr_alloc * a->nr2 * a->xnr; j++) {
		val = (double *) (a->spmin + j);
		*val = DBL_MAX;
		val = (double *) (a->spmax + j);
		*val = -DBL_MAX;
	     }
}

/*
 * **************************************************************************
 * Allocate buffers for min and max values.
 *
 * IN:
 * @a		Activity for which buffers are to be initialized.
 * @nr_alloc	Number of slots to allocate.
 * @flags	Flags for common options and system state.
 ***************************************************************************
 */
void allocate_minmax_buf(struct activity *a, size_t nr_alloc, uint64_t flags)
{
	/* nr_alloc should be greater than a->nr_spalloc */
	if (nr_alloc <= a->nr_spalloc) {
#ifdef DEBUG
		fprintf(stderr, "%s: %s: alloc=%zu allocated=%d\n",
			__FUNCTION__, a->name, nr_alloc, a->nr_spalloc);
#endif
		return;
	}

#ifdef DEBUG
	if (nr_alloc < a->nr_allocated) {
		/* Should never happen */
		fprintf(stderr, "%s: %s: spalloc=%zu allocated=%d\n",
			__FUNCTION__, a->name, nr_alloc, a->nr_allocated);
		exit(4);
	}
#endif

	if (DISPLAY_MINMAX(flags) && a->xnr) {

		/* Allocate arrays for min and max values... */
		SREALLOC(a->spmin, void,
			 mul_check_overflow4(nr_alloc, (size_t) a->nr2, (size_t) a->xnr, sizeof(double)));
		SREALLOC(a->spmax, void,
			 mul_check_overflow4(nr_alloc, (size_t) a->nr2, (size_t) a->xnr, sizeof(double)));

		/* ... and init them */
		init_minmax_buf(a, a->nr_spalloc, nr_alloc);
		a->nr_spalloc = nr_alloc;
	}
}

/*
 ***************************************************************************
 * Allocate buffers for one activity.
 *
 * IN:
 * @a		Activity for which buffers are to be initialized.
 * @nr_alloc	Number of structures to allocate.
 * @flags	Flags for common options and system state.
 ***************************************************************************
 */
void allocate_buffers(struct activity *a, size_t nr_alloc, uint64_t flags)
{
	int j;

	/* nr_alloc should always be greater than a->nr_allocated */
	if (nr_alloc <= a->nr_allocated) {
#ifdef DEBUG
		fprintf(stderr, "%s: %s: alloc=%zu allocated=%d\n",
			__FUNCTION__, a->name, nr_alloc, a->nr_allocated);
#endif
		return;
	}

	for (j = 0; j < 3; j++) {
		SREALLOC(a->buf[j], void,
			 mul_check_overflow3((size_t) a->msize, nr_alloc, (size_t) a->nr2));

		/* If its a reallocation then init additional space which has been allocated */
		if (a->nr_allocated) {
			memset((char *) a->buf[j] + a->msize * a->nr_allocated * a->nr2, 0,
			       (size_t) a->msize * (size_t) (nr_alloc - a->nr_allocated) * (size_t) a->nr2);
		}
	}
	a->nr_allocated = nr_alloc;

	/* Allocate buffers for min and max values if necessary */
	allocate_minmax_buf(a, nr_alloc, flags);
}

/*
 * **************************************************************************
 * Allocate structures for all activities.
 *
 * IN:
 * @act		Array of activities.
 * @flags	Flags for common options and system state.
 ***************************************************************************
 */
void allocate_structures(struct activity *act[], uint64_t flags)
{
	int i;

	for (i = 0; i < NR_ACT; i++) {

		if (act[i]->nr_ini > 0) {
			allocate_buffers(act[i], (size_t) act[i]->nr_ini, flags);
		}
	}
}

/*
 ***************************************************************************
 * Free structures.
 *
 * IN:
 * @act	Array of activities.
 ***************************************************************************
 */
void free_structures(struct activity *act[])
{
	int i, j;

	for (i = 0; i < NR_ACT; i++) {

		if (act[i]->nr_allocated > 0) {
			for (j = 0; j < 3; j++) {
				if (act[i]->buf[j]) {
					free(act[i]->buf[j]);
					act[i]->buf[j] = NULL;
				}
			}
			act[i]->nr_allocated = 0;
		}

		if (act[i]->nr_spalloc > 0) {
			if (act[i]->spmin) {
				free(act[i]->spmin);
				act[i]->spmin = NULL;
			}
			if (act[i]->spmax) {
				free(act[i]->spmax);
				act[i]->spmax = NULL;
			}
			act[i]->nr_spalloc = 0;
		}
	}
}

/*
 * **************************************************************************
 * Reallocate buffers for min/max values.
 *
 * IN:
 * @a		Activity whose buffers need to be reallocated.
 * @nr_min	Minimum number of items that the new buffers should be able
 *		to receive.
 * @flags	Flags for common options and system state.
 ***************************************************************************
 */
void reallocate_minmax_buf(struct activity *a, __nr_t nr_min, uint64_t flags)
{
	size_t nr_realloc;

	if (nr_min <= 0) {
		nr_min = 1;
	}
	if (!a->nr_spalloc) {
		nr_realloc = nr_min;
	}
	else {
		nr_realloc = a->nr_spalloc;
		do {
			nr_realloc = nr_realloc * 2;
		}
		while (nr_realloc < nr_min);
	}

	/* Reallocate buffers for current activity */
	allocate_minmax_buf(a, nr_realloc, flags);
}

/*
 ***************************************************************************
 * Reallocate all the buffers for a given activity (main buffers and
 * spmin/spmax buffers).
 *
 * IN:
 * @a		Activity whose buffers need to be reallocated.
 * @nr_min	Minimum number of items that the new buffers should be able
 *		to receive.
 * @flags	Flags for common options and system state.
 ***************************************************************************
 */
void reallocate_buffers(struct activity *a, __nr_t nr_min, uint64_t flags)
{
	size_t nr_realloc;

	if (nr_min <= 0) {
		nr_min = 1;
	}
	if (!a->nr_allocated) {
		nr_realloc = nr_min;
	}
	else {
		nr_realloc = a->nr_allocated;
		do {
			nr_realloc = nr_realloc * 2;
		}
		while (nr_realloc < nr_min);
	}

	/* Reallocate buffers for current activity */
	allocate_buffers(a, nr_realloc, flags);
}

/*
 ***************************************************************************
 * Check if we are close enough to desired interval.
 *
 * IN:
 * @uptime_ref	Uptime used as reference. This is the system uptime for the
 *		first sample statistics, or the first system uptime after a
 *		LINUX RESTART (in 1/100th of a second).
 * @uptime	Current system uptime (in 1/100th of a second).
 * @reset	TRUE if @last_uptime should be reset with @uptime_ref.
 * @interval	Interval of time.
 *
 * RETURNS:
 * TRUE if we are actually close enough to desired interval, FALSE otherwise.
 ***************************************************************************
*/
int next_slice(unsigned long long uptime_ref, unsigned long long uptime,
	       int reset, long interval)
{
	unsigned long file_interval, entry;
	static unsigned long long last_uptime = 0;
	int min, max, pt1, pt2;
	double f;

	/* uptime is expressed in 1/100th of a second */
	if (!last_uptime || reset) {
		last_uptime = uptime_ref;
	}

	/* Interval cannot be greater than 0xffffffff here */
	f = ((double) ((uptime - last_uptime) & 0xffffffff)) / 100;
	file_interval = (unsigned long) f;
	if ((f * 10) - (file_interval * 10) >= 5) {
		file_interval++; /* Rounding to correct value */
	}

	last_uptime = uptime;

	if (interval == 1)
		/* Smallest time interval: Always close enough to desired interval */
		return TRUE;

	/*
	 * A few notes about the "algorithm" used here to display selected entries
	 * from the system activity file (option -f with -i flag):
	 * Let Iu be the interval value given by the user on the command line,
	 *     In the interval between current and previous sample,
	 * and En the current sample (identified by its time stamp) in the file.
	 * En will ne displayed if there is an integer p so that:
	 * p * Iu belongs to [En - In/2, En + In/2[.
	 */
	f = ((double) ((uptime - uptime_ref) & 0xffffffff)) / 100;
	entry = (unsigned long) f;
	if ((f * 10) - (entry * 10) >= 5) {
		entry++;
	}

	min = entry - (file_interval / 2);
	max = entry + (file_interval / 2) + (file_interval & 0x1);
	pt1 = (entry / interval) * interval;
	pt2 = ((entry / interval) + 1) * interval;

	return (((pt1 >= min) && (pt1 < max)) || ((pt2 >= min) && (pt2 < max)));
}

/*
 ***************************************************************************
 * Use time stamp to fill tstamp_ext structure.
 *
 * IN:
 * @timestamp	Timestamp to decode (format: HH:MM:SS).
 *
 * OUT:
 * @tse		Structure containing the decoded timestamp.
 *
 * RETURNS:
 * 0 if the timestamp has been successfully decoded, 1 otherwise.
 ***************************************************************************
 */
int decode_timestamp(char timestamp[], struct tstamp_ext *tse)
{
	timestamp[2] = timestamp[5] = '\0';

	if ((strspn(timestamp, DIGITS) != 2) ||
	    (strspn(&timestamp[3], DIGITS) != 2) ||
	    (strspn(&timestamp[6], DIGITS) != 2))
		return 1;

	tse->tm_time.tm_sec  = atoi(&timestamp[6]);
	tse->tm_time.tm_min  = atoi(&timestamp[3]);
	tse->tm_time.tm_hour = atoi(timestamp);

	if ((tse->tm_time.tm_sec < 0) || (tse->tm_time.tm_sec > 59) ||
	    (tse->tm_time.tm_min < 0) || (tse->tm_time.tm_min > 59) ||
	    (tse->tm_time.tm_hour < 0) || (tse->tm_time.tm_hour > 23)) {
		tse->use = NO_TIME;
		return 1;
	}

	tse->use = USE_HHMMSS_T;

	return 0;
}

/*
 ***************************************************************************
 * Use time stamp to fill tstamp_ext structure.
 *
 * IN:
 * @timestamp	Epoch time to decode (format: number of seconds since
 *		Januray 1st 1970 00:00:00 UTC).
 * @flags	Flags for common options and system state.
 *
 * OUT:
 * @tse		Structure containing the decoded epoch time.
 *
 * RETURNS:
 * 0 if the epoch time has been successfully decoded, 1 otherwise.
 ***************************************************************************
 */
int decode_epoch(char timestamp[], struct tstamp_ext *tse, uint64_t flags)
{
	tse->epoch_time = atol(timestamp);

	if (!tse->epoch_time) {
		tse->use = NO_TIME;
		return 1;
	}

	tse->use = USE_EPOCH_T;

	return 0;
}

/*
 ***************************************************************************
 * Compare two timestamps.
 *
 * IN:
 * @rectime	Date and time for current sample.
 * @tse		Timestamp used as reference.
 * @cross_day	TRUE if a new day has been started.
 *
 * RETURNS:
 * A positive value if @rectime is greater than @tse,
 * a negative one otherwise.
 * Also returns 0 if no valid time is saved in @tse.
 ***************************************************************************
 */
int datecmp(struct tstamp_ext *rectime, struct tstamp_ext *tse, int cross_day)
{
	int tm_hour;

	switch (tse->use) {

		case USE_HHMMSS_T:
			/*
			 * This is necessary if we want to properly handle something like:
			 * sar -s time_start -e time_end with
			 * time_start(day D) > time_end(day D+1)
			*/
			tm_hour = rectime->tm_time.tm_hour + (24 * (cross_day != 0));

			if (tm_hour == tse->tm_time.tm_hour) {
				if (rectime->tm_time.tm_min == tse->tm_time.tm_min)
				return (rectime->tm_time.tm_sec - tse->tm_time.tm_sec);
				else
					return (rectime->tm_time.tm_min - tse->tm_time.tm_min);
			}
			else
				return (tm_hour - tse->tm_time.tm_hour);

		case USE_EPOCH_T:
			return (rectime->epoch_time - tse->epoch_time);

		default:	/* NO_TIME */
			return 0;
	}
}

/*
 ***************************************************************************
 * Parse a timestamp entered on the command line (hh:mm[:ss] or number of
 * seconds since the Epoch) and decode it.
 *
 * IN:
 * @argv		Arguments list.
 * @opt			Index in the arguments list.
 * @def_timestamp	Default timestamp to use.
 * @flags		Flags for common options and system state.
 *
 * OUT:
 * @tse			Structure containing the decoded timestamp.
 *
 * RETURNS:
 * 0 if the timestamp has been successfully decoded, 1 otherwise.
 ***************************************************************************
 */
int parse_timestamp(char *argv[], int *opt, struct tstamp_ext *tse,
		    const char *def_timestamp, uint64_t flags)
{
	char timestamp[11];
	int ok = FALSE;

	if (argv[++(*opt)] && strncmp(argv[*opt], "-", 1)) {
		switch (strlen(argv[*opt])) {

			case 5:
				if (argv[*opt][2] != ':')
					break;
				strncpy(timestamp, argv[(*opt)++], 5);
				timestamp[5] = '\0';
				strcat(timestamp, ":00");
				ok = TRUE;
				break;

			case 8:
				if ((argv[*opt][2] != ':') || (argv[*opt][5] != ':'))
					break;
				strncpy(timestamp, argv[(*opt)++], 8);
				ok = TRUE;
				break;

			case 10:
				if (strspn(argv[*opt], DIGITS) == 10) {
					/* This is actually a timestamp */
					strncpy(timestamp, argv[(*opt)++], 10);
					timestamp[10] = '\0';

					return decode_epoch(timestamp, tse, flags);
				}
				break;
		}
	}

	if (!ok) {
		strncpy(timestamp, def_timestamp, 8);
	}
	timestamp[8] = '\0';

	return decode_timestamp(timestamp, tse);
}

/*
 ***************************************************************************
 * Set interval value.
 *
 * IN:
 * @record_hdr_curr	Record with current sample statistics.
 * @record_hdr_prev	Record with previous sample statistics.
 *
 * OUT:
 * @itv			Interval of time in 1/100th of a second.
 ***************************************************************************
 */
void get_itv_value(struct record_header *record_hdr_curr,
		   struct record_header *record_hdr_prev,
		   unsigned long long *itv)
{
	/* Interval value in jiffies */
	*itv = get_interval(record_hdr_prev->uptime_cs,
			    record_hdr_curr->uptime_cs);
}

/*
 ***************************************************************************
 * Fill the tm_time structure with the file's creation date, based on file's
 * time data saved in file header.
 * The resulting timestamp is expressed in the locale of the file creator or
 * in the user's own locale, depending on whether option -t has been used
 * or not.
 *
 * IN:
 * @flags	Flags for common options and system state.
 * @file_hdr	System activity file standard header.
 *
 * OUT:
 * @tm_time	Date (and possibly time) from file header. Only the date,
 * 		not the time, should be used by the caller.
 ***************************************************************************
 */
void get_file_timestamp_struct(uint64_t flags, struct tm *tm_time,
			       struct file_header *file_hdr)
{
	time_t t = file_hdr->sa_ust_time;

	if (PRINT_TRUE_TIME(flags)) {
		/* Get local time. This is just to fill fields with a default value. */
		get_time(tm_time, 0);

		tm_time->tm_mday = file_hdr->sa_day;
		tm_time->tm_mon  = file_hdr->sa_month;
		tm_time->tm_year = file_hdr->sa_year;
		/*
		 * Call mktime() to set DST (Daylight Saving Time) flag.
		 * Has anyone a better way to do it?
		 */
		tm_time->tm_hour = tm_time->tm_min = tm_time->tm_sec = 0;
		mktime(tm_time);
	}
	else {
		localtime_r(&t, tm_time);
	}
}

/*
 ***************************************************************************
 * Print report header.
 *
 * IN:
 * @flags	Flags for common options and system state.
 * @file_hdr	System activity file standard header.
 *
 * OUT:
 * @tm_time	Date and time from file header.
 ***************************************************************************
 */
void print_report_hdr(uint64_t flags, struct tm *tm_time,
		      struct file_header *file_hdr)
{

	/* Get date of file creation */
	get_file_timestamp_struct(flags, tm_time, file_hdr);

	/*
	 * Display the header.
	 * NB: Number of CPU (value in [1, NR_CPUS + 1]).
	 * 	1 means that there is only one proc and non SMP kernel.
	 *	2 means one proc and SMP kernel. Etc.
	 */
	print_gal_header(tm_time, file_hdr->sa_sysname, file_hdr->sa_release,
			 file_hdr->sa_nodename, file_hdr->sa_machine,
			 file_hdr->sa_cpu_nr > 1 ? file_hdr->sa_cpu_nr - 1 : 1,
			 PLAIN_OUTPUT);
}

/*
 ***************************************************************************
 * Network interfaces may now be registered (and unregistered) dynamically.
 * This is what we try to guess here.
 *
 * IN:
 * @a		Activity structure with statistics.
 * @curr	Index in array for current sample statistics.
 * @ref		Index in array for sample statistics used as reference.
 * @pos		Index on current network interface.
 *
 * RETURNS:
 * Position of current network interface in array of sample statistics used
 * as reference.
 * -1 if it is a new interface (it was not present in array of stats used
 * as reference).
 * -2 if it is a known interface but which has been unregistered then
 * registered again on the interval.
 ***************************************************************************
 */
int check_net_dev_reg(struct activity *a, int curr, int ref, int pos)
{
	struct stats_net_dev *sndc, *sndp;
	int j0, j = pos;

	if (!a->nr[ref])
		/*
		 * No items found in previous iteration:
		 * Current interface is necessarily new.
		 */
		return -1;

	if (j >= a->nr[ref]) {
		j = a->nr[ref] - 1;
	}
	j0 = j;

	sndc = (struct stats_net_dev *) ((char *) a->buf[curr] + pos * a->msize);

	do {
		sndp = (struct stats_net_dev *) ((char *) a->buf[ref] + j * a->msize);

		if (!strcmp(sndc->interface, sndp->interface)) {
			/*
			 * Network interface found.
			 * If a counter has decreased, then we may assume that the
			 * corresponding interface was unregistered, then registered again.
			 */
			if ((sndc->rx_packets    < sndp->rx_packets)    ||
			    (sndc->tx_packets    < sndp->tx_packets)    ||
			    (sndc->rx_bytes      < sndp->rx_bytes)      ||
			    (sndc->tx_bytes      < sndp->tx_bytes)      ||
			    (sndc->rx_compressed < sndp->rx_compressed) ||
			    (sndc->tx_compressed < sndp->tx_compressed) ||
			    (sndc->multicast     < sndp->multicast)) {

				/*
				 * Special processing for rx_bytes (_packets) and
				 * tx_bytes (_packets) counters: If the number of
				 * bytes (packets) has decreased, whereas the number of
				 * packets (bytes) has increased, then assume that the
				 * relevant counter has met an overflow condition, and that
				 * the interface was not unregistered, which is all the
				 * more plausible that the previous value for the counter
				 * was > ULLONG_MAX/2.
				 * NB: the average value displayed will be wrong in this case...
				 *
				 * If such an overflow is detected, just set the flag. There is no
				 * need to handle this in a special way: the difference is still
				 * properly calculated if the result is of the same type (i.e.
				 * unsigned long) as the two values.
				 */
				int ovfw = FALSE;

				if ((sndc->rx_bytes   < sndp->rx_bytes)   &&
				    (sndc->rx_packets > sndp->rx_packets) &&
				    (sndp->rx_bytes   > (~0ULL >> 1))) {
					ovfw = TRUE;
				}
				if ((sndc->tx_bytes   < sndp->tx_bytes)   &&
				    (sndc->tx_packets > sndp->tx_packets) &&
				    (sndp->tx_bytes   > (~0ULL >> 1))) {
					ovfw = TRUE;
				}
				if ((sndc->rx_packets < sndp->rx_packets) &&
				    (sndc->rx_bytes   > sndp->rx_bytes)   &&
				    (sndp->rx_packets > (~0ULL >> 1))) {
					ovfw = TRUE;
				}
				if ((sndc->tx_packets < sndp->tx_packets) &&
				    (sndc->tx_bytes   > sndp->tx_bytes)   &&
				    (sndp->tx_packets > (~0ULL >> 1))) {
					ovfw = TRUE;
				}

				if (!ovfw)
					/*
					 * OK: Assume here that the device was
					 * actually unregistered.
					 */
					return -2;
			}
			return j;
		}
		if (++j >= a->nr[ref]) {
			j = 0;
		}
	}
	while (j != j0);

	/* This is a newly registered interface */
	return -1;
}

/*
 ***************************************************************************
 * Network interfaces may now be registered (and unregistered) dynamically.
 * This is what we try to guess here.
 *
 * IN:
 * @a		Activity structure with statistics.
 * @curr	Index in array for current sample statistics.
 * @ref		Index in array for sample statistics used as reference.
 * @pos		Index on current network interface.
 *
 * RETURNS:
 * Position of current network interface in array of sample statistics used
 * as reference.
 * -1 if it is a newly registered interface.
 * -2 if it is a known interface but which has been unregistered then
 * registered again on the interval.
 ***************************************************************************
 */
int check_net_edev_reg(struct activity *a, int curr, int ref, int pos)
{
	struct stats_net_edev *snedc, *snedp;
	int j0, j = pos;

	if (!a->nr[ref])
		/*
		 * No items found in previous iteration:
		 * Current interface is necessarily new.
		 */
		return -1;

	if (j >= a->nr[ref]) {
		j = a->nr[ref] - 1;
	}
	j0 = j;

	snedc = (struct stats_net_edev *) ((char *) a->buf[curr] + pos * a->msize);

	do {
		snedp = (struct stats_net_edev *) ((char *) a->buf[ref] + j * a->msize);

		if (!strcmp(snedc->interface, snedp->interface)) {
			/*
			 * Network interface found.
			 * If a counter has decreased, then we may assume that the
			 * corresponding interface was unregistered, then registered again.
			 */
			if ((snedc->tx_errors         < snedp->tx_errors)         ||
			    (snedc->collisions        < snedp->collisions)        ||
			    (snedc->rx_dropped        < snedp->rx_dropped)        ||
			    (snedc->tx_dropped        < snedp->tx_dropped)        ||
			    (snedc->tx_carrier_errors < snedp->tx_carrier_errors) ||
			    (snedc->rx_frame_errors   < snedp->rx_frame_errors)   ||
			    (snedc->rx_fifo_errors    < snedp->rx_fifo_errors)    ||
			    (snedc->tx_fifo_errors    < snedp->tx_fifo_errors))
				/*
				 * OK: assume here that the device was
				 * actually unregistered.
				 */
				return -2;

			return j;
		}
		if (++j >= a->nr[ref]) {
			j = 0;
		}
	}
	while (j != j0);

	/* This is a newly registered interface */
	return -1;
}

/*
 ***************************************************************************
 * Disks may be registered dynamically (true in /proc/diskstats file).
 * This is what we try to guess here.
 *
 * IN:
 * @a		Activity structure with statistics.
 * @curr	Index in array for current sample statistics.
 * @ref		Index in array for sample statistics used as reference.
 * @pos		Index on current disk.
 *
 * RETURNS:
 * Position of current disk in array of sample statistics used as reference
 * -1 if it is a newly registered device.
 * -2 if it is a known device but which has been unregistered then registered
 * again on the interval.
 ***************************************************************************
 */
int check_disk_reg(struct activity *a, int curr, int ref, int pos)
{
	struct stats_disk *sdc, *sdp;
	int j0, j = pos;

	if (!a->nr[ref])
		/*
		 * No items found in previous iteration:
		 * Current interface is necessarily new.
		 */
		return -1;

	if (j >= a->nr[ref]) {
		j = a->nr[ref] - 1;
	}
	j0 = j;

	sdc = (struct stats_disk *) ((char *) a->buf[curr] + pos * a->msize);

	do {
		sdp = (struct stats_disk *) ((char *) a->buf[ref] + j * a->msize);

		if ((sdc->major == sdp->major) &&
		    (sdc->minor == sdp->minor)) {
			/*
			 * Disk found.
			 * If all the counters have decreased then the likelyhood
			 * is that the disk has been unregistered and a new disk inserted.
			 * If only one or two have decreased then the likelyhood
			 * is that the counter has simply wrapped.
			 * Don't take into account a counter if its previous value was 0
			 * (this may be a read-only device, or a kernel that doesn't
			 * support discard stats yet...)
			 */
			if ((sdc->nr_ios < sdp->nr_ios) &&
			    (!sdp->rd_sect || (sdc->rd_sect < sdp->rd_sect)) &&
			    (!sdp->wr_sect || (sdc->wr_sect < sdp->wr_sect)) &&
			    (!sdp->dc_sect || (sdc->dc_sect < sdp->dc_sect)))
				/* Same device registered again */
				return -2;

			return j;
		}
		if (++j >= a->nr[ref]) {
			j = 0;
		}
	}
	while (j != j0);

	/* This is a newly registered device */
	return -1;
}

/*
 ***************************************************************************
 * Allocate bitmaps for activities that have one.
 *
 * IN:
 * @act		Array of activities.
 ***************************************************************************
 */
void allocate_bitmaps(struct activity *act[])
{
	int i;

	for (i = 0; i < NR_ACT; i++) {
		/*
		 * If current activity has a bitmap which has not already
		 * been allocated, then allocate it.
		 * Note that a same bitmap may be used by several activities.
		 */
		if (act[i]->bitmap && !act[i]->bitmap->b_array) {
			SREALLOC(act[i]->bitmap->b_array, unsigned char,
				 BITMAP_SIZE(act[i]->bitmap->b_size));
		}
	}
}

/*
 ***************************************************************************
 * Free bitmaps for activities that have one.
 *
 * IN:
 * @act		Array of activities.
 ***************************************************************************
 */
void free_bitmaps(struct activity *act[])
{
	int i;

	for (i = 0; i < NR_ACT; i++) {
		if (act[i]->bitmap && act[i]->bitmap->b_array) {
			free(act[i]->bitmap->b_array);
			/* Set pointer to NULL to prevent it from being freed again */
			act[i]->bitmap->b_array = NULL;
		}
	}
}

/*
 ***************************************************************************
 * Select all activities, even if they have no associated items.
 *
 * IN:
 * @act		Array of activities.
 *
 * OUT:
 * @act		Array of activities, all of the being selected.
 ***************************************************************************
 */
void select_all_activities(struct activity *act[])
{
	int i;

	for (i = 0; i < NR_ACT; i++) {
		act[i]->options |= AO_SELECTED;
	}
}

/*
 ***************************************************************************
 * Select CPU activity if no other activities have been explicitly selected.
 * Also select CPU "all" if no other CPU has been selected.
 *
 * IN:
 * @act		Array of activities.
 *
 * OUT:
 * @act		Array of activities with CPU activity selected if needed.
 ***************************************************************************
 */
void select_default_activity(struct activity *act[])
{
	int p;

	p = get_activity_position(act, A_CPU, EXIT_IF_NOT_FOUND);

	/* Default is CPU activity... */
	if (!get_activity_nr(act, AO_SELECTED, COUNT_ACTIVITIES)) {
		/*
		 * Yet A_CPU activity may not be available in file
		 * since the user can choose not to collect it.
		 */
		act[p]->options |= AO_SELECTED;
	}

	/*
	 * If no CPU's have been selected then select CPU "all".
	 * cpu_bitmap bitmap may be used by several activities (A_CPU, A_PWR_CPU...)
	 */
	if (!count_bits(cpu_bitmap.b_array, BITMAP_SIZE(cpu_bitmap.b_size))) {
		cpu_bitmap.b_array[0] |= 0x01;
	}
}

/*
 ***************************************************************************
 * Swap bytes for every numerical field in structure. Used to convert from
 * one endianness type (big-endian or little-endian) to the other.
 *
 * IN:
 * @types_nr	Number of fields in structure for each following types:
 *		unsigned long long, unsigned long and int.
 * @ps		Pointer on structure.
 * @is64bit	TRUE if data come from a 64-bit machine.
 ***************************************************************************
 */
void swap_struct(const unsigned int types_nr[], void *ps, int is64bit)
{
	int i;
	uint64_t *x;
	uint32_t *y;

	x = (uint64_t *) ps;
	/* For each field of type long long (or double) */
	for (i = 0; i < types_nr[0]; i++) {
		*x = __builtin_bswap64(*x);
		x = (uint64_t *) ((char *) x + ULL_ALIGNMENT_WIDTH);
	}

	y = (uint32_t *) x;
	/* For each field of type long */
	for (i = 0; i < types_nr[1]; i++) {
		if (is64bit) {
			*x = __builtin_bswap64(*x);
			x = (uint64_t *) ((char *) x + UL_ALIGNMENT_WIDTH);
		}
		else {
			*y = __builtin_bswap32(*y);
			y = (uint32_t *) ((char *) y + UL_ALIGNMENT_WIDTH);
		}
	}

	if (is64bit) {
		y = (uint32_t *) x;
	}
	/* For each field of type int */
	for (i = 0; i < types_nr[2]; i++) {
		*y = __builtin_bswap32(*y);
		y = (uint32_t *) ((char *) y + U_ALIGNMENT_WIDTH);
	}
}

/*
 ***************************************************************************
 * Map the fields of a structure containing statistics read from a file to
 * those of the structure known by current sysstat version.
 * Each structure (either read from file or from current sysstat version)
 * is described by 3 values: The number of [unsigned] long long integers,
 * the number of [unsigned] long integers following in the structure, and
 * last the number of [unsigned] integers.
 * We assume that those numbers will *never* decrease with newer sysstat
 * versions.
 *
 * IN:
 * @gtypes_nr	Structure description as expected for current sysstat version.
 * @ftypes_nr	Structure description as read from file.
 * @ps		Pointer on structure containing statistics.
 * @f_size	Size of the structure containing statistics. This is the
 *		size of the structure *read from file*.
 * @g_size	Size of the structure expected by current sysstat version.
 * @b_size	Size of the buffer pointed by @ps.
 *
 * RETURNS:
 * -1 if an error has been encountered, or 0 otherwise.
 ***************************************************************************
 */
int remap_struct(const unsigned int gtypes_nr[], const unsigned int ftypes_nr[],
		 void *ps, unsigned int f_size, unsigned int g_size, size_t b_size)
{
	int d;
	size_t n;

	/* Sanity check */
	if (MAP_SIZE(ftypes_nr) > f_size)
		return -1;

	/* Remap [unsigned] long fields */
	d = gtypes_nr[0] - ftypes_nr[0];
	if (d) {
		if (ftypes_nr[0] * ULL_ALIGNMENT_WIDTH < ftypes_nr[0])
			/* Overflow */
			return -1;

		n = MINIMUM(f_size - ftypes_nr[0] * ULL_ALIGNMENT_WIDTH,
			    g_size - gtypes_nr[0] * ULL_ALIGNMENT_WIDTH);
		if ((ftypes_nr[0] * ULL_ALIGNMENT_WIDTH >= b_size) ||
		    (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH + n > b_size) ||
		    (ftypes_nr[0] * ULL_ALIGNMENT_WIDTH + n > b_size))
			return -1;

		memmove(((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH,
			((char *) ps) + ftypes_nr[0] * ULL_ALIGNMENT_WIDTH, n);
		if (d > 0) {
			memset(((char *) ps) + ftypes_nr[0] * ULL_ALIGNMENT_WIDTH,
			       0, d * ULL_ALIGNMENT_WIDTH);
		}
	}
	/* Remap [unsigned] int fields */
	d = gtypes_nr[1] - ftypes_nr[1];
	if (d) {
		if (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		    ftypes_nr[1] * UL_ALIGNMENT_WIDTH < ftypes_nr[1])
			/* Overflow */
			return -1;

		n = MINIMUM(f_size - ftypes_nr[0] * ULL_ALIGNMENT_WIDTH
				   - ftypes_nr[1] * UL_ALIGNMENT_WIDTH,
			    g_size - gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
				   - gtypes_nr[1] * UL_ALIGNMENT_WIDTH);
		if ((gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		     ftypes_nr[1] * UL_ALIGNMENT_WIDTH >= b_size) ||
		    (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		     gtypes_nr[1] * UL_ALIGNMENT_WIDTH + n > b_size) ||
		    (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		     ftypes_nr[1] * UL_ALIGNMENT_WIDTH + n > b_size))
			return -1;

		memmove(((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
				      + gtypes_nr[1] * UL_ALIGNMENT_WIDTH,
			((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
				      + ftypes_nr[1] * UL_ALIGNMENT_WIDTH, n);
		if (d > 0) {
			memset(((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
					     + ftypes_nr[1] * UL_ALIGNMENT_WIDTH,
			       0, d * UL_ALIGNMENT_WIDTH);
		}
	}
	/* Remap possible fields (like strings of chars) following int fields */
	d = gtypes_nr[2] - ftypes_nr[2];
	if (d) {
		if (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		    gtypes_nr[1] * UL_ALIGNMENT_WIDTH +
		    ftypes_nr[2] * U_ALIGNMENT_WIDTH < ftypes_nr[2])
			/* Overflow */
			return -1;

		n = MINIMUM(f_size - ftypes_nr[0] * ULL_ALIGNMENT_WIDTH
				   - ftypes_nr[1] * UL_ALIGNMENT_WIDTH
				   - ftypes_nr[2] * U_ALIGNMENT_WIDTH,
			    g_size - gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
				   - gtypes_nr[1] * UL_ALIGNMENT_WIDTH
				   - gtypes_nr[2] * U_ALIGNMENT_WIDTH);
		if ((gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		     gtypes_nr[1] * UL_ALIGNMENT_WIDTH +
		     ftypes_nr[2] * U_ALIGNMENT_WIDTH >= b_size) ||
		    (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		     gtypes_nr[1] * UL_ALIGNMENT_WIDTH +
		     gtypes_nr[2] * U_ALIGNMENT_WIDTH + n > b_size) ||
		    (gtypes_nr[0] * ULL_ALIGNMENT_WIDTH +
		     gtypes_nr[1] * UL_ALIGNMENT_WIDTH +
		     ftypes_nr[2] * U_ALIGNMENT_WIDTH + n > b_size))
			return -1;

		memmove(((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
				      + gtypes_nr[1] * UL_ALIGNMENT_WIDTH
				      + gtypes_nr[2] * U_ALIGNMENT_WIDTH,
			((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
				      + gtypes_nr[1] * UL_ALIGNMENT_WIDTH
				      + ftypes_nr[2] * U_ALIGNMENT_WIDTH, n);
		if (d > 0) {
			memset(((char *) ps) + gtypes_nr[0] * ULL_ALIGNMENT_WIDTH
					     + gtypes_nr[1] * UL_ALIGNMENT_WIDTH
					     + ftypes_nr[2] * U_ALIGNMENT_WIDTH,
			       0, d * U_ALIGNMENT_WIDTH);
		}
	}
	return 0;
}

/*
 ***************************************************************************
 * Read data from a system activity data file.
 *
 * IN:
 * @ifd		Input file descriptor.
 * @buffer	Buffer where data are read.
 * @size	Number of bytes to read.
 * @mode	If set to HARD_SIZE, indicate that an EOF should be considered
 * 		as an error.
 * @oneof	Set to UEOF_CONT if an unexpected end of file should not make
 *		sadf stop. Default behavior is to stop on unexpected EOF.
 *
 * RETURNS:
 * 1 if EOF has been reached,
 * 2 if an unexpected EOF has been reached (and sadf was told to continue),
 * 0 otherwise.
 ***************************************************************************
 */
int sa_fread(int ifd, void *buffer, size_t size, enum size_mode mode, enum on_eof oneof)
{
	ssize_t n;

	if ((n = read(ifd, buffer, size)) < 0) {
		fprintf(stderr, _("Error while reading system activity file: %s\n"),
			strerror(errno));
		close(ifd);
		exit(2);
	}

	if (!n && (mode == SOFT_SIZE))
		return 1;	/* EOF */

	if (n < size) {
		fprintf(stderr, _("End of system activity file unexpected\n"));
		if (oneof == UEOF_CONT)
			return 2;
		close(ifd);
		exit(2);
	}

	return 0;
}

/*
 ***************************************************************************
 * Skip unknown extra structures present in file.
 *
 * IN:
 * @ifd		System activity data file descriptor.
 * @endian_mismatch
 *		TRUE if file's data don't match current machine's endianness.
 * @arch_64	TRUE if file's data come from a 64 bit machine.
 *
 * RETURNS:
 * -1 on error, 0 otherwise.
 ***************************************************************************
 */
int skip_extra_struct(int ifd, int endian_mismatch, int arch_64)
{
	int i;
	struct extra_desc xtra_d;

	do {
		/* Read extra structure description */
		sa_fread(ifd, &xtra_d, EXTRA_DESC_SIZE, HARD_SIZE, UEOF_STOP);

		/*
		 * We don't need to remap as the extra_desc structure won't change,
		 * but we may need to normalize endianness anyway.
		 */
		if (endian_mismatch) {
			swap_struct(extra_desc_types_nr, &xtra_d, arch_64);
		}

		/* Check values consistency */
		if (MAP_SIZE(xtra_d.extra_types_nr) > xtra_d.extra_size) {
#ifdef DEBUG
			fprintf(stderr, "%s: extra_size=%u types=%u,%u,%u\n",
				__FUNCTION__, xtra_d.extra_size,
				xtra_d.extra_types_nr[0], xtra_d.extra_types_nr[1], xtra_d.extra_types_nr[2]);
#endif
			return -1;
		}

		if ((xtra_d.extra_nr > MAX_EXTRA_NR) || (xtra_d.extra_size > MAX_EXTRA_SIZE)) {
#ifdef DEBUG
			fprintf(stderr, "%s: extra_size=%u extra_nr=%u\n",
				__FUNCTION__, xtra_d.extra_size, xtra_d.extra_size);
#endif
			return -1;
		}

		/* Ignore current unknown extra structures */
		for (i = 0; i < xtra_d.extra_nr; i++) {
			if (lseek(ifd, xtra_d.extra_size, SEEK_CUR) < xtra_d.extra_size)
				return -1;
		}
	}
	while (xtra_d.extra_next);

	return 0;
}

/*
 ***************************************************************************
 * Read the record header of current sample and process it.
 *
 * IN:
 * @ifd		Input file descriptor.
 * @buffer	Buffer where data will be read.
 * @record_hdr	Structure where record header will be saved.
 * @file_hdr	file_hdr structure containing data read from file standard
 *		header.
 * @arch_64	TRUE if file's data come from a 64-bit machine.
 * @endian_mismatch
 *		TRUE if data read from file don't match current machine's
 *		endianness.
 * @oneof	Set to EOF_CONT if an unexpected end of file should not make
 *		sadf stop. Default behavior is to stop on unexpected EOF.
 * @b_size	@buffer size.
 * @flags	Flags for common options and system state.
 * @ofmt	Pointer on report output format structure.
 *
 * OUT:
 * @record_hdr	Record header for current sample.
 *
 * RETURNS:
 * 1 if EOF has been reached, 0 otherwise.
 ***************************************************************************
 */
int read_record_hdr(int ifd, void *buffer, struct record_header *record_hdr,
		    struct file_header *file_hdr, int arch_64, int endian_mismatch,
		    int oneof, size_t b_size, uint64_t flags, struct report_format *ofmt)
{
	int rc;

	do {
		if ((rc = sa_fread(ifd, buffer, (size_t) file_hdr->rec_size, SOFT_SIZE, oneof)) != 0)
			/* End of sa data file */
			return rc;

		/* Remap record header structure to that expected by current version */
		if (remap_struct(rec_types_nr, file_hdr->rec_types_nr, buffer,
				 file_hdr->rec_size, RECORD_HEADER_SIZE, b_size) < 0)
			goto invalid_data;
		memcpy(record_hdr, buffer, RECORD_HEADER_SIZE);

		/* Normalize endianness */
		if (endian_mismatch) {
			swap_struct(rec_types_nr, record_hdr, arch_64);
		}

		/* Raw output in debug mode */
		if (DISPLAY_DEBUG_MODE(flags) && (ofmt->id == F_RAW_OUTPUT)) {
			char out[128];

			sprintf(out, "# uptime_cs; %llu; ust_time; %llu; extra_next; %u; record_type; %d; HH:MM:SS; %02d:%02d:%02d\n",
			       record_hdr->uptime_cs, record_hdr->ust_time,
			       record_hdr->extra_next, record_hdr->record_type,
			       record_hdr->hour, record_hdr->minute, record_hdr->second);
			cprintf_s(IS_COMMENT, "%s", out);
		}

		/* Sanity checks */
		if (!record_hdr->record_type || (record_hdr->record_type > R_EXTRA_MAX) ||
		    (record_hdr->hour > 23) || (record_hdr->minute > 59) || (record_hdr->second > 60) || (record_hdr->ust_time < 1000000000)) {
#ifdef DEBUG
			fprintf(stderr, "%s: record_type=%d HH:MM:SS=%02d:%02d:%02d (%llu)\n",
				__FUNCTION__, record_hdr->record_type,
				record_hdr->hour, record_hdr->minute, record_hdr->second,
				record_hdr->ust_time);
#endif
			goto invalid_data;
		}

		/*
		 * Skip unknown extra structures if present.
		 * This will be done later for R_COMMENT and R_RESTART records, as extra structures
		 * are saved after the comment or the number of CPU.
		 */
		if ((record_hdr->record_type != R_COMMENT) && (record_hdr->record_type != R_RESTART) &&
		    record_hdr->extra_next && (skip_extra_struct(ifd, endian_mismatch, arch_64) < 0))
			goto invalid_data;
	}
	while ((record_hdr->record_type >= R_EXTRA_MIN) && (record_hdr->record_type <= R_EXTRA_MAX)) ;

	return 0;

invalid_data:
	fprintf(stderr, _("Invalid data read\n"));
	exit(2);
}

/*
 ***************************************************************************
 * Move structures data.
 *
 * IN:
 * @act		Array of activities.
 * @id_seq	Activity sequence in file.
 * @record_hdr	Current record header.
 * @dest	Index in array where stats have to be copied to.
 * @src		Index in array where stats to copy are.
 ***************************************************************************
 */
void copy_structures(struct activity *act[], unsigned int id_seq[],
		     struct record_header record_hdr[], int dest, int src)
{
	int i, p;

	memcpy(&record_hdr[dest], &record_hdr[src], RECORD_HEADER_SIZE);

	for (i = 0; i < NR_ACT; i++) {

		if (!id_seq[i])
			continue;

		p = get_activity_position(act, id_seq[i], EXIT_IF_NOT_FOUND);

		memcpy(act[p]->buf[dest], act[p]->buf[src],
		       (size_t) act[p]->msize * (size_t) act[p]->nr_allocated * (size_t) act[p]->nr2);
		act[p]->nr[dest] = act[p]->nr[src];
	}
}

/*
 ***************************************************************************
 * Read an __nr_t value from file.
 * Such a value can be the new number of CPU saved after a RESTART record,
 * or the number of structures to read saved before the structures containing
 * statistics for an activity with a varying number of items in file.
 *
 * IN:
 * @ifd		Input file descriptor.
 * @file	Name of file being read.
 * @file_magic	file_magic structure filled with file magic header data.
 * @endian_mismatch
 *		TRUE if file's data don't match current machine's endianness.
 * @arch_64	TRUE if file's data come from a 64 bit machine.
 * @non_zero	TRUE if value should not be zero.
 * @maxv	Maximum value allowed.
 *
 * RETURNS:
 * __nr_t value, as read from file.
 ***************************************************************************
 */
__nr_t read_nr_value(int ifd, char *file, struct file_magic *file_magic,
		     int endian_mismatch, int arch_64, int non_zero, __nr_t maxv)
{
	__nr_t value;
	unsigned int nr_types_nr[]  = {0, 0, 1};

	sa_fread(ifd, &value, sizeof(__nr_t), HARD_SIZE, UEOF_STOP);

	/* Normalize endianness for file_activity structures */
	if (endian_mismatch) {
		swap_struct(nr_types_nr, &value, arch_64);
	}

	if ((non_zero && !value) || (value < 0) || (value > maxv)) {
#ifdef DEBUG
		fprintf(stderr, "%s: Value=%d Max=%d\n",
			__FUNCTION__, value, maxv);
#endif
		/* Value number cannot be zero or negative */
		handle_invalid_sa_file(ifd, file_magic, file, 0);
	}

	return value;
}

/*
 ***************************************************************************
 * Read varying part of the statistics from a daily data file.
 *
 * IN:
 * @act		Array of activities.
 * @curr	Index in array for current sample statistics.
 * @ifd		Input file descriptor.
 * @act_nr	Number of activities in file.
 * @file_actlst	Activity list in file.
 * @endian_mismatch
 *		TRUE if file's data don't match current machine's endianness.
 * @arch_64	TRUE if file's data come from a 64 bit machine.
 * @dfile	Name of system activity data file.
 * @file_magic	file_magic structure containing data read from file magic
 *		header.
 * @oneof	Set to UEOF_CONT if an unexpected end of file should not make
 *		sadf stop. Default behavior is to stop on unexpected EOF.
 * @flags	Flags for common options and system state.
 *
 * RETURNS:
 * 2 if an error has been encountered (e.g. unexpected EOF),
 * 0 otherwise.
 ***************************************************************************
 */
int read_file_stat_bunch(struct activity *act[], int curr, int ifd, int act_nr,
			 struct file_activity *file_actlst, int endian_mismatch,
			 int arch_64, char *dfile, struct file_magic *file_magic,
			 enum on_eof oneof, uint64_t flags)
{
	int i, j, p;
	struct file_activity *fal = file_actlst;
	off_t offset;
	__nr_t nr_value;

	for (i = 0; i < act_nr; i++, fal++) {

		/* Read __nr_t value preceding statistics structures if it exists */
		if (fal->has_nr) {
			nr_value = read_nr_value(ifd, dfile, file_magic,
						 endian_mismatch, arch_64, FALSE, NR_MAX);
		}
		else {
			nr_value = fal->nr;
		}

		if (nr_value > NR_MAX) {
#ifdef DEBUG
			fprintf(stderr, "%s: Value=%d Max=%d\n", __FUNCTION__, nr_value, NR_MAX);
#endif
			handle_invalid_sa_file(ifd, file_magic, dfile, 0);
		}

		if (((p = get_activity_position(act, fal->id, RESUME_IF_NOT_FOUND)) < 0) ||
		    (act[p]->magic != fal->magic)) {
			/*
			 * Ignore current activity in file, which is unknown to
			 * current sysstat version or has an unknown format.
			 */
			if (nr_value) {
				offset = (off_t) fal->size * (off_t) nr_value * (off_t) fal->nr2;
				if (lseek(ifd, offset, SEEK_CUR) < offset) {
					close(ifd);
					perror("lseek");
					if (oneof == UEOF_CONT)
						return 2;
					exit(2);
				}
			}
			continue;
		}

		if (nr_value > act[p]->nr_max) {
#ifdef DEBUG
			fprintf(stderr, "%s: %s: Value=%d Max=%d\n",
				__FUNCTION__, act[p]->name, nr_value, act[p]->nr_max);
#endif
			handle_invalid_sa_file(ifd, file_magic, dfile, 0);
		}
		act[p]->nr[curr] = nr_value;

		/* Reallocate buffers if needed */
		if (nr_value > act[p]->nr_allocated) {
			reallocate_buffers(act[p], nr_value, flags);
		}

		/*
                 * For persistent activities, we must make sure that no statistics
                 * from a previous iteration remain, especially if the number
                 * of structures read is smaller than @nr_ini.
                 */
		if (HAS_PERSISTENT_VALUES(act[p]->options)) {
                    memset(act[p]->buf[curr], 0,
                           (size_t) act[p]->msize * (size_t) act[p]->nr_ini * (size_t) act[p]->nr2);
                }

		/* OK, this is a known activity: Read the stats structures */
		if ((nr_value > 0) &&
		    ((nr_value > 1) || (act[p]->nr2 > 1)) &&
		    (act[p]->msize > act[p]->fsize)) {

			for (j = 0; j < (nr_value * act[p]->nr2); j++) {
				if (sa_fread(ifd, (char *) act[p]->buf[curr] + j * act[p]->msize,
					 (size_t) act[p]->fsize, HARD_SIZE, oneof) > 0)
					/* Unexpected EOF */
					return 2;
			}
		}
		else if (nr_value > 0) {
			/*
			 * Note: If msize was smaller than fsize,
			 * then it has been set to fsize in check_file_actlst().
			 */
			if (sa_fread(ifd, act[p]->buf[curr],
				 (size_t) act[p]->fsize * (size_t) nr_value * (size_t) act[p]->nr2,
				 HARD_SIZE, oneof) > 0)
				/* Unexpected EOF */
				return 2;
		}
		else {
			/* nr_value == 0: Nothing to read */
			continue;
		}

		/* Normalize endianness for current activity's structures */
		if (endian_mismatch) {
			for (j = 0; j < (nr_value * act[p]->nr2); j++) {
				swap_struct(act[p]->ftypes_nr, (char *) act[p]->buf[curr] + j * act[p]->msize,
					    arch_64);
			}
		}

		/* Remap structure's fields to those known by current sysstat version */
		for (j = 0; j < (nr_value * act[p]->nr2); j++) {
			if (remap_struct(act[p]->gtypes_nr, act[p]->ftypes_nr,
					 (char *) act[p]->buf[curr] + j * act[p]->msize,
					 act[p]->fsize, act[p]->msize, act[p]->msize) < 0)
				return 2;
		}
	}

	return 0;
}

/*
 ***************************************************************************
 * Open a sysstat activity data file and read its magic structure.
 *
 * IN:
 * @dfile	Name of system activity data file.
 * @ignore	Set to 1 if a true sysstat activity file but with a bad
 *		format should not yield an error message. Useful with
 *		sadf -H and sadf -c.
 *
 * OUT:
 * @fd		System activity data file descriptor.
 * @file_magic	file_magic structure containing data read from file magic
 *		header.
 * @endian_mismatch
 *		TRUE if file's data don't match current machine's endianness.
 * @do_swap	TRUE if endianness should be normalized for sysstat_magic
 *		and format_magic numbers.
 *
 * RETURNS:
 * -1 if data file is a sysstat file with an old format (which we cannot
 * read), 0 otherwise.
 ***************************************************************************
 */
int sa_open_read_magic(int *fd, char *dfile, struct file_magic *file_magic,
		       int ignore, int *endian_mismatch, int do_swap)
{
	int n;
	unsigned int fm_types_nr[] = {FILE_MAGIC_ULL_NR, FILE_MAGIC_UL_NR, FILE_MAGIC_U_NR};

	/* Open sa data file */
	if ((*fd = open(dfile, O_RDONLY)) < 0) {
		int saved_errno = errno;

		fprintf(stderr, _("Cannot open %s: %s\n"), dfile, strerror(errno));

		if ((saved_errno == ENOENT) && default_file_used) {
			fprintf(stderr, _("Please check if data collecting is enabled\n"));
		}
		exit(2);
	}

	/* Read file magic data */
	n = read(*fd, file_magic, FILE_MAGIC_SIZE);

	if ((n != FILE_MAGIC_SIZE) ||
	    ((file_magic->sysstat_magic != SYSSTAT_MAGIC) && (file_magic->sysstat_magic != SYSSTAT_MAGIC_SWAPPED)) ||
	    ((file_magic->format_magic != FORMAT_MAGIC) && (file_magic->format_magic != FORMAT_MAGIC_SWAPPED) && !ignore)) {
#ifdef DEBUG
		fprintf(stderr, "%s: Bytes read=%d sysstat_magic=%x format_magic=%x\n",
			__FUNCTION__, n, file_magic->sysstat_magic, file_magic->format_magic);
#endif
		/* Display error message and exit */
		handle_invalid_sa_file(*fd, file_magic, dfile, n);
	}

	*endian_mismatch = (file_magic->sysstat_magic != SYSSTAT_MAGIC);
	if (*endian_mismatch) {
		if (do_swap) {
			/* Swap bytes for file_magic fields */
			file_magic->sysstat_magic = SYSSTAT_MAGIC;
			file_magic->format_magic  = __builtin_bswap16(file_magic->format_magic);
		}
		/*
		 * Start swapping at field "header_size" position.
		 * May not exist for older versions but in this case, it won't be used.
		 */
		swap_struct(fm_types_nr, &file_magic->header_size, 0);
	}

	if ((file_magic->sysstat_version > 10) ||
	    ((file_magic->sysstat_version == 10) && (file_magic->sysstat_patchlevel >= 3))) {
		/* header_size field exists only for sysstat versions 10.3.1 and later */
		if ((file_magic->header_size <= MIN_FILE_HEADER_SIZE) ||
		    (file_magic->header_size > MAX_FILE_HEADER_SIZE)) {
#ifdef DEBUG
			fprintf(stderr, "%s: header_size=%u\n",
				__FUNCTION__, file_magic->header_size);
#endif
			/* Display error message and exit */
			handle_invalid_sa_file(*fd, file_magic, dfile, n);
		}
	}
	if ((file_magic->sysstat_version > 11) ||
	    ((file_magic->sysstat_version == 11) && (file_magic->sysstat_patchlevel >= 7))) {
		/* hdr_types_nr field exists only for sysstat versions 11.7.1 and later */
		if (MAP_SIZE(file_magic->hdr_types_nr) > file_magic->header_size) {
#ifdef DEBUG
			fprintf(stderr, "%s: map_size=%u header_size=%u\n",
				__FUNCTION__, MAP_SIZE(file_magic->hdr_types_nr), file_magic->header_size);
#endif
			handle_invalid_sa_file(*fd, file_magic, dfile, n);
		}
	}

	if ((file_magic->format_magic != FORMAT_MAGIC) &&
	    (file_magic->format_magic != FORMAT_MAGIC_SWAPPED))
		/*
		 * This is an old (or new) sa datafile format to
		 * be read by sadf (since @ignore was set to TRUE).
		 */
		return -1;

	return 0;
}

/*
 ***************************************************************************
 * Open a data file, and perform various checks before reading.
 * NB: This is called only when reading a datafile (sar and sadf), never
 * when writing or appending data to a datafile.
 *
 * IN:
 * @dfile	Name of system activity data file.
 * @act		Array of activities.
 * @flags	Flags for common options and system state.
 *
 * OUT:
 * @ifd		System activity data file descriptor.
 * @file_magic	file_magic structure containing data read from file magic
 *		header.
 * @file_hdr	file_hdr structure containing data read from file standard
 *		header.
 * @file_actlst	Acvtivity list in file.
 * @id_seq	Activity sequence.
 * @endian_mismatch
 *		TRUE if file's data don't match current machine's endianness.
 * @arch_64	TRUE if file's data come from a 64 bit machine.
 ***************************************************************************
 */
void check_file_actlst(int *ifd, char *dfile, struct activity *act[], uint64_t flags,
		       struct file_magic *file_magic, struct file_header *file_hdr,
		       struct file_activity **file_actlst, unsigned int id_seq[],
		       int *endian_mismatch, int *arch_64)
{
	int i, j, k, p, skip;
	struct file_activity *fal;
	void *buffer = NULL;
	size_t bh_size = FILE_HEADER_SIZE;
	size_t ba_size = FILE_ACTIVITY_SIZE;

	/* Open sa data file and read its magic structure */
	if (sa_open_read_magic(ifd, dfile, file_magic,
			       DISPLAY_HDR_ONLY(flags), endian_mismatch, TRUE) < 0)
		/*
		 * Not current sysstat's format.
		 * Return now so that sadf -H can display at least
		 * file's version and magic number.
		 */
		return;

	/*
	 * We know now that we have a *compatible* sysstat datafile format
	 * (correct FORMAT_MAGIC value), and in this case, we should have
	 * checked header_size value. Anyway, with a corrupted datafile,
	 * this may not be the case. So check again.
	 */
	if ((file_magic->header_size <= MIN_FILE_HEADER_SIZE) ||
	    (file_magic->header_size > MAX_FILE_HEADER_SIZE)) {
#ifdef DEBUG
		fprintf(stderr, "%s: header_size=%u\n",
			__FUNCTION__, file_magic->header_size);
#endif
		goto format_error;
	}

	/* Allocate buffer for file_header structure */
	if (file_magic->header_size > FILE_HEADER_SIZE) {
		bh_size = file_magic->header_size;
	}
	SREALLOC(buffer, char, bh_size);

	/* Read sa data file standard header and allocate activity list */
	sa_fread(*ifd, buffer, (size_t) file_magic->header_size, HARD_SIZE, UEOF_STOP);
	/*
	 * Data file header size (file_magic->header_size) may be greater or
	 * smaller than FILE_HEADER_SIZE. Remap the fields of the file header
	 * then copy its contents to the expected structure.
	 */
	if (remap_struct(hdr_types_nr, file_magic->hdr_types_nr, buffer,
			 file_magic->header_size, FILE_HEADER_SIZE, bh_size) < 0)
		goto format_error;

	memcpy(file_hdr, buffer, FILE_HEADER_SIZE);
	free(buffer);
	buffer = NULL;

	/* Tell that data come from a 64 bit machine */
	*arch_64 = (file_hdr->sa_sizeof_long == SIZEOF_LONG_64BIT);

	/* Normalize endianness for file_hdr structure */
	if (*endian_mismatch) {
		swap_struct(hdr_types_nr, file_hdr, *arch_64);
	}

	/*
	 * Sanity checks.
	 * NB: Compare against MAX_NR_ACT and not NR_ACT because
	 * we are maybe reading a datafile from a future sysstat version
	 * with more activities than known today.
	 */
	if ((file_hdr->sa_act_nr > MAX_NR_ACT) ||
	    (file_hdr->act_size > MAX_FILE_ACTIVITY_SIZE) ||
	    (file_hdr->rec_size > MAX_RECORD_HEADER_SIZE) ||
	    (MAP_SIZE(file_hdr->act_types_nr) > file_hdr->act_size) ||
	    (MAP_SIZE(file_hdr->rec_types_nr) > file_hdr->rec_size)) {
#ifdef DEBUG
		fprintf(stderr, "%s: sa_act_nr=%u act_size=%u rec_size=%u map_size(act)=%u map_size(rec)=%u\n",
			__FUNCTION__, file_hdr->sa_act_nr, file_hdr->act_size, file_hdr->rec_size,
			MAP_SIZE(file_hdr->act_types_nr), MAP_SIZE(file_hdr->rec_types_nr));
#endif
		/* Maybe a "false positive" sysstat datafile? */
		goto format_error;
	}

	/* Allocate buffer for file_activity structures */
	if (file_hdr->act_size > FILE_ACTIVITY_SIZE) {
		ba_size = file_hdr->act_size;
	}
	SREALLOC(buffer, char, ba_size);
	SREALLOC(*file_actlst, struct file_activity, FILE_ACTIVITY_SIZE * file_hdr->sa_act_nr);
	fal = *file_actlst;

	/* Read activity list */
	j = 0;
	for (i = 0; i < file_hdr->sa_act_nr; i++, fal++) {

		/* Read current file_activity structure from file */
		sa_fread(*ifd, buffer, (size_t) file_hdr->act_size, HARD_SIZE, UEOF_STOP);

		/*
		 * Data file_activity size (file_hdr->act_size) may be greater or
		 * smaller than FILE_ACTIVITY_SIZE. Remap the fields of the file's structure
		 * then copy its contents to the expected structure.
		 */
		if (remap_struct(act_types_nr, file_hdr->act_types_nr, buffer,
			     file_hdr->act_size, FILE_ACTIVITY_SIZE, ba_size) < 0)
			goto format_error;
		memcpy(fal, buffer, FILE_ACTIVITY_SIZE);

		/* Normalize endianness for file_activity structures */
		if (*endian_mismatch) {
			swap_struct(act_types_nr, fal, *arch_64);
		}

		/*
		 * Every activity, known or unknown, should have
		 * at least one item and sub-item, and a size value in
		 * a defined range.
		 * Also check that the number of items and sub-items
		 * doesn't exceed a max value. This is necessary
		 * because we will use @nr and @nr2 to
		 * allocate memory to read the file contents. So we
		 * must make sure the file is not corrupted.
		 * NB: Another check will be made below for known
		 * activities which have each a specific max value.
		 */
		if ((fal->nr < 1) || (fal->nr2 < 1) ||
		    (fal->nr > NR_MAX) || (fal->nr2 > NR2_MAX) ||
		    (fal->size <= 0) || (fal->size > MAX_ITEM_STRUCT_SIZE)) {
#ifdef DEBUG
			fprintf(stderr, "%s: id=%u nr=%d nr2=%d size=%d\n",
				__FUNCTION__, fal->id, fal->nr, fal->nr2, fal->size);
#endif
			goto format_error;
		}

		if ((p = get_activity_position(act, fal->id, RESUME_IF_NOT_FOUND)) < 0)
			/* Unknown activity */
			continue;

		if ((fal->magic != act[p]->magic) && !DISPLAY_HDR_ONLY(flags)) {
			skip = TRUE;
		}
		else {
			skip = FALSE;
		}

		/* Check max value for known activities */
		if (fal->nr > act[p]->nr_max) {
#ifdef DEBUG
			fprintf(stderr, "%s: id=%u nr=%d nr_max=%d\n",
				__FUNCTION__, fal->id, fal->nr, act[p]->nr_max);
#endif
			goto format_error;
		}

		/*
		 * Number of fields of each type ("long long", or "long"
		 * or "int") composing the structure with statistics may
		 * only increase with new sysstat versions, unless we change
		 * the activity's magic number. Here, we may
		 * be reading a file created by current sysstat version,
		 * or by an older or a newer version.
		 */
		if (!(((fal->types_nr[0] >= act[p]->gtypes_nr[0]) &&
		     (fal->types_nr[1] >= act[p]->gtypes_nr[1]) &&
		     (fal->types_nr[2] >= act[p]->gtypes_nr[2]))
		     ||
		     ((fal->types_nr[0] <= act[p]->gtypes_nr[0]) &&
		     (fal->types_nr[1] <= act[p]->gtypes_nr[1]) &&
		     (fal->types_nr[2] <= act[p]->gtypes_nr[2]))) &&
		     (fal->magic == act[p]->magic) && !DISPLAY_HDR_ONLY(flags)) {
#ifdef DEBUG
			fprintf(stderr, "%s: id=%u file=%u,%u,%u activity=%u,%u,%u\n",
				__FUNCTION__, fal->id, fal->types_nr[0], fal->types_nr[1], fal->types_nr[2],
				act[p]->gtypes_nr[0], act[p]->gtypes_nr[1], act[p]->gtypes_nr[2]);
#endif
			goto format_error;
		}

		if (MAP_SIZE(fal->types_nr) > fal->size) {
#ifdef DEBUG
		fprintf(stderr, "%s: id=%u size=%d map_size=%u\n",
			__FUNCTION__, fal->id, fal->size, MAP_SIZE(fal->types_nr));
#endif
			goto format_error;
		}

		for (k = 0; k < 3; k++) {
			act[p]->ftypes_nr[k] = fal->types_nr[k];
		}

		if (fal->size > act[p]->msize) {
			act[p]->msize = fal->size;
		}

		act[p]->nr_ini = fal->nr;
		act[p]->nr2    = fal->nr2;
		act[p]->fsize  = fal->size;

		/*
		 * This is a known activity with a known format
		 * (magical number). Only such activities will be displayed.
		 * (Well, this may also be an unknown format if we have entered sadf -H.)
		 */
		if (!skip) {
			id_seq[j++] = fal->id;
		}
	}

	while (j < NR_ACT) {
		id_seq[j++] = 0;
	}

	free(buffer);
	buffer = NULL;

	/* Check that at least one activity selected by the user is available in file */
	for (i = 0; i < NR_ACT; i++) {

		if (!IS_SELECTED(act[i]->options))
			continue;

		/* Here is a selected activity: Does it exist in file? */
		fal = *file_actlst;
		for (j = 0; j < file_hdr->sa_act_nr; j++, fal++) {
			if (act[i]->id == fal->id)
				break;
		}
		if (j == file_hdr->sa_act_nr) {
			/* No: Unselect it */
			act[i]->options &= ~AO_SELECTED;
		}
	}

	/*
	 * None of selected activities exist in file: Abort.
	 * NB: Error is ignored if we only want to display
	 * datafile header (sadf -H).
	 */
	if (!get_activity_nr(act, AO_SELECTED, COUNT_ACTIVITIES) && !DISPLAY_HDR_ONLY(flags)) {
		fprintf(stderr, _("Requested activities not available in file %s\n"),
			dfile);
		close(*ifd);
		exit(1);
	}

	/*
	 * Check if there are some extra structures.
	 * We will just skip them as they are unknown for now.
	 */
	if (file_hdr->extra_next && (skip_extra_struct(*ifd, *endian_mismatch, *arch_64) < 0))
		goto format_error;

	return;

format_error:
	if (buffer) {
		free(buffer);
	}
	handle_invalid_sa_file(*ifd, file_magic, dfile, 0);
}

/*
 ***************************************************************************
 * Look for item in list.
 *
 * IN:
 * @list	Pointer on the start of the linked list.
 * @item_name	Item name to look for.
 *
 * RETURNS:
 * Pointer on item in list if found, or NULL otherwise.
 ***************************************************************************
 */
struct sa_item *search_list_item(struct sa_item *list, char *item_name)
{
	while (list != NULL) {
		if (!strcmp(list->item_name, item_name))
			return list;	/* Item found in list */
		list = list->next;
	}

	/* Item not found */
	return NULL;
}

/*
 ***************************************************************************
 * Add item to the list.
 *
 * IN:
 * @list	Address of pointer on the start of the linked list.
 * @item_name	Name of the item.
 * @max_len	Max length of an item.
 *
 * OUT:
 * @pos		If not NULL, contains the position of the item in list.
 *		Not modified if item name was too long.
 *
 * RETURNS:
 * 1 if item has been added to the list (since it was not previously there),
 * and 0 otherwise (item already in list or item name too long).
 ***************************************************************************
 */
int add_list_item(struct sa_item **list, char *item_name, int max_len, int *pos)
{
	struct sa_item *e;
	int len;

	if ((len = strnlen(item_name, max_len)) == max_len)
		/* Item too long */
		return 0;

	if (pos) {
		*pos = 0;
	}

	while (*list != NULL) {
		e = *list;
		if (!strcmp(e->item_name, item_name))
			return 0;	/* Item found in list */
		list = &(e->next);
		if (pos) {
			(*pos)++;
		}
	}

	/* Item not found: Add it to the list */
	SREALLOC(*list, struct sa_item, sizeof(struct sa_item));
	e = *list;
	if ((e->item_name = (char *) malloc(len + 1)) == NULL) {
		perror("malloc");
		exit(4);
	}
	strcpy(e->item_name, item_name);

	return 1;
}

/*
 * **************************************************************************
 * Free a linked list.
 *
 * IN:
 * @list	Address of the pointer on the start of the linked list.
 ***************************************************************************
 */
void free_item_list(struct sa_item **item_list)
{
	struct sa_item *l, *list = *item_list;

	while (list) {
		l = list->next;
		if (list->item_name) {
			free(list->item_name);
		}
		free(list);
		list = l;
	}
	*item_list = NULL;
}

/*
 ***************************************************************************
 * Parse sar activities options (also used by sadf).
 *
 * IN:
 * @argv	Arguments list.
 * @opt		Index in list of arguments.
 * @caller	Indicate whether it's sar or sadf that called this function.
 *
 * OUT:
 * @act		Array of selected activities.
 * @flags	Common flags and system state.
 *
 * RETURNS:
 * 0 on success.
 ***************************************************************************
 */
int parse_sar_opt(char *argv[], int *opt, struct activity *act[],
		  uint64_t *flags, int caller)
{
	int i, p;

	for (i = 1; *(argv[*opt] + i); i++) {
		/*
		 * Note: argv[*opt] contains something like "-BruW"
		 *     *(argv[*opt] + i) will contain 'B', 'r', etc.
		 */

		switch (*(argv[*opt] + i)) {

		case 'A':
			select_all_activities(act);
			*flags |= S_F_OPTION_A;

			/*
			 * Force '-r ALL -u ALL -F'.
			 * Setting -F is compulsory because corresponding activity
			 * has AO_MULTIPLE_OUTPUTS flag set.
			 * -P ALL will be set only if corresponding option has
			 * not been exlicitly entered on the command line.
			 */
			p = get_activity_position(act, A_MEMORY, EXIT_IF_NOT_FOUND);
			act[p]->opt_flags |= AO_F_MEMORY + AO_F_SWAP + AO_F_MEM_ALL;

			p = get_activity_position(act, A_CPU, EXIT_IF_NOT_FOUND);
			act[p]->opt_flags = AO_F_CPU_ALL;

			p = get_activity_position(act, A_FS, EXIT_IF_NOT_FOUND);
			act[p]->opt_flags = AO_F_FILESYSTEM;
			break;

		case 'B':
			SELECT_ACTIVITY(A_PAGE);
			break;

		case 'b':
			SELECT_ACTIVITY(A_IO);
			break;

		case 'C':
			*flags |= S_F_COMMENT;
			break;

		case 'd':
			SELECT_ACTIVITY(A_DISK);
			break;

		case 'F':
			p = get_activity_position(act, A_FS, EXIT_IF_NOT_FOUND);
			act[p]->options |= AO_SELECTED;
			if (!*(argv[*opt] + i + 1) && argv[*opt + 1] && !strcmp(argv[*opt + 1], K_MOUNT)) {
				(*opt)++;
				act[p]->opt_flags |= AO_F_MOUNT;
				return 0;
			}
			else {
				act[p]->opt_flags |= AO_F_FILESYSTEM;
			}
			break;

		case 'H':
			SELECT_ACTIVITY(A_HUGE);
			break;

		case 'h':
			/* Option -h is equivalent to --pretty --human */
			*flags |= S_F_PRETTY + S_F_UNIT;
			break;

		case 'I':
			p = get_activity_position(act, A_IRQ, EXIT_IF_NOT_FOUND);
			act[p]->options |= AO_SELECTED;

			if (!*(argv[*opt] + i + 1) && argv[*opt + 1] &&
			    (!strcmp(argv[*opt + 1], K_ALL) || !strcmp(argv[*opt + 1], K_SUM))) {
				(*opt)++;
				/* Select int "sum". Keyword ALL is ignored */
				if (!strcmp(argv[*opt], K_SUM)) {
					act[p]->item_list_sz += add_list_item(&(act[p]->item_list), K_LOWERSUM, MAX_SA_IRQ_LEN, NULL);
					act[p]->options |= AO_LIST_ON_CMDLINE;
				}
				return 0;
			}
			break;

		case 'j':
			if (!argv[*opt + 1]) {
				return 1;
			}
			(*opt)++;
			if (!strcmp(argv[*opt], K_SID)) {
				*flags |= S_F_DEV_SID + S_F_PRETTY;
				return 0;
			}

			if (strnlen(argv[*opt], sizeof(persistent_name_type)) >= sizeof(persistent_name_type) - 1)
				return 1;

			strncpy(persistent_name_type, argv[*opt], sizeof(persistent_name_type) - 1);
			persistent_name_type[sizeof(persistent_name_type) - 1] = '\0';
			strtolower(persistent_name_type);
			if (!get_persistent_type_dir(persistent_name_type)) {
				fprintf(stderr, _("Invalid type of persistent device name\n"));
				return 2;
			}
			/* Pretty print report (option -j implies option -p) */
			*flags |= S_F_PERSIST_NAME + S_F_PRETTY;
			return 0;
			break;

		case 'p':
			*flags |= S_F_PRETTY;
			break;

		case 'q':
			/* Option -q grouped with other ones */
			SELECT_ACTIVITY(A_QUEUE);
			break;

		case 'r':
			p = get_activity_position(act, A_MEMORY, EXIT_IF_NOT_FOUND);
			act[p]->options   |= AO_SELECTED;
			act[p]->opt_flags |= AO_F_MEMORY;
			if (!*(argv[*opt] + i + 1) && argv[*opt + 1] && !strcmp(argv[*opt + 1], K_ALL)) {
				(*opt)++;
				act[p]->opt_flags |= AO_F_MEM_ALL;
				return 0;
			}
			break;

		case 'S':
			p = get_activity_position(act, A_MEMORY, EXIT_IF_NOT_FOUND);
			act[p]->options   |= AO_SELECTED;
			act[p]->opt_flags |= AO_F_SWAP;
			break;

		case 't':
			/*
			 * Check sar option -t here (as it can be combined
			 * with other ones, eg. "sar -rtu ..."
			 * But sadf option -t is checked in sadf.c as it won't
			 * be entered as a sar option after "--".
			 */
			if (caller != C_SAR) {
				return 1;
			}
			*flags |= S_F_TRUE_TIME;
			break;

		case 'u':
			p = get_activity_position(act, A_CPU, EXIT_IF_NOT_FOUND);
			act[p]->options |= AO_SELECTED;
			if (!*(argv[*opt] + i + 1) && argv[*opt + 1] && !strcmp(argv[*opt + 1], K_ALL)) {
				(*opt)++;
				act[p]->opt_flags = AO_F_CPU_ALL;
				return 0;
			}
			else {
				act[p]->opt_flags = AO_F_CPU_DEF;
			}
			break;

		case 'v':
			SELECT_ACTIVITY(A_KTABLES);
			break;

		case 'w':
			SELECT_ACTIVITY(A_PCSW);
			break;

		case 'W':
			SELECT_ACTIVITY(A_SWAP);
			break;

		case 'x':
			/*
			 * Check sar option -x here (as it can be combined
			 * with other ones. This options is not used by sadf.
			 * Display min and max values.
			 */
			if (caller == C_SAR) {
				*flags |= S_F_MINMAX;
			}
			break;

		case 'y':
			SELECT_ACTIVITY(A_SERIAL);
			break;

		case 'z':
			*flags |= S_F_ZERO_OMIT;
			break;

		default:
			return 1;
		}
	}
	return 0;
}

/*
 ***************************************************************************
 * Parse sar "-m" option.
 *
 * IN:
 * @argv	Arguments list.
 * @opt		Index in list of arguments.
 *
 * OUT:
 * @act		Array of selected activities.
 *
 * RETURNS:
 * 0 on success, 1 otherwise.
 ***************************************************************************
 */
int parse_sar_m_opt(char *argv[], int *opt, struct activity *act[])
{
	char *t;

	for (t = strtok(argv[*opt], ","); t; t = strtok(NULL, ",")) {
		if (!strcmp(t, K_CPU)) {
			SELECT_ACTIVITY(A_PWR_CPU);
		}
		else if (!strcmp(t, K_FAN)) {
			SELECT_ACTIVITY(A_PWR_FAN);
		}
		else if (!strcmp(t, K_IN)) {
			SELECT_ACTIVITY(A_PWR_IN);
		}
		else if (!strcmp(t, K_TEMP)) {
			SELECT_ACTIVITY(A_PWR_TEMP);
		}
		else if (!strcmp(t, K_FREQ)) {
			SELECT_ACTIVITY(A_PWR_FREQ);
		}
		else if (!strcmp(t, K_USB)) {
			SELECT_ACTIVITY(A_PWR_USB);
		}
		else if (!strcmp(t, K_BAT)) {
			SELECT_ACTIVITY(A_PWR_BAT);
		}
		else if (!strcmp(t, K_ALL)) {
			SELECT_ACTIVITY(A_PWR_CPU);
			SELECT_ACTIVITY(A_PWR_FAN);
			SELECT_ACTIVITY(A_PWR_IN);
			SELECT_ACTIVITY(A_PWR_TEMP);
			SELECT_ACTIVITY(A_PWR_FREQ);
			SELECT_ACTIVITY(A_PWR_USB);
			SELECT_ACTIVITY(A_PWR_BAT);
		}
		else
			return 1;
	}

	(*opt)++;
	return 0;
}

/*
 ***************************************************************************
 * Parse sar "-n" option.
 *
 * IN:
 * @argv	Arguments list.
 * @opt		Index in list of arguments.
 *
 * OUT:
 * @act		Array of selected activities.
 *
 * RETURNS:
 * 0 on success, 1 otherwise.
 ***************************************************************************
 */
int parse_sar_n_opt(char *argv[], int *opt, struct activity *act[])
{
	char *t;

	for (t = strtok(argv[*opt], ","); t; t = strtok(NULL, ",")) {
		if (!strcmp(t, K_DEV)) {
			SELECT_ACTIVITY(A_NET_DEV);
		}
		else if (!strcmp(t, K_EDEV)) {
			SELECT_ACTIVITY(A_NET_EDEV);
		}
		else if (!strcmp(t, K_SOCK)) {
			SELECT_ACTIVITY(A_NET_SOCK);
		}
		else if (!strcmp(t, K_NFS)) {
			SELECT_ACTIVITY(A_NET_NFS);
		}
		else if (!strcmp(t, K_NFSD)) {
			SELECT_ACTIVITY(A_NET_NFSD);
		}
		else if (!strcmp(t, K_IP)) {
			SELECT_ACTIVITY(A_NET_IP);
		}
		else if (!strcmp(t, K_EIP)) {
			SELECT_ACTIVITY(A_NET_EIP);
		}
		else if (!strcmp(t, K_ICMP)) {
			SELECT_ACTIVITY(A_NET_ICMP);
		}
		else if (!strcmp(t, K_EICMP)) {
			SELECT_ACTIVITY(A_NET_EICMP);
		}
		else if (!strcmp(t, K_TCP)) {
			SELECT_ACTIVITY(A_NET_TCP);
		}
		else if (!strcmp(t, K_ETCP)) {
			SELECT_ACTIVITY(A_NET_ETCP);
		}
		else if (!strcmp(t, K_UDP)) {
			SELECT_ACTIVITY(A_NET_UDP);
		}
		else if (!strcmp(t, K_SOCK6)) {
			SELECT_ACTIVITY(A_NET_SOCK6);
		}
		else if (!strcmp(t, K_IP6)) {
			SELECT_ACTIVITY(A_NET_IP6);
		}
		else if (!strcmp(t, K_EIP6)) {
			SELECT_ACTIVITY(A_NET_EIP6);
		}
		else if (!strcmp(t, K_ICMP6)) {
			SELECT_ACTIVITY(A_NET_ICMP6);
		}
		else if (!strcmp(t, K_EICMP6)) {
			SELECT_ACTIVITY(A_NET_EICMP6);
		}
		else if (!strcmp(t, K_UDP6)) {
			SELECT_ACTIVITY(A_NET_UDP6);
		}
		else if (!strcmp(t, K_FC)) {
			SELECT_ACTIVITY(A_NET_FC);
		}
		else if (!strcmp(t, K_SOFT)) {
			SELECT_ACTIVITY(A_NET_SOFT);
		}
		else if (!strcmp(t, K_ALL)) {
			SELECT_ACTIVITY(A_NET_DEV);
			SELECT_ACTIVITY(A_NET_EDEV);
			SELECT_ACTIVITY(A_NET_SOCK);
			SELECT_ACTIVITY(A_NET_NFS);
			SELECT_ACTIVITY(A_NET_NFSD);
			SELECT_ACTIVITY(A_NET_IP);
			SELECT_ACTIVITY(A_NET_EIP);
			SELECT_ACTIVITY(A_NET_ICMP);
			SELECT_ACTIVITY(A_NET_EICMP);
			SELECT_ACTIVITY(A_NET_TCP);
			SELECT_ACTIVITY(A_NET_ETCP);
			SELECT_ACTIVITY(A_NET_UDP);
			SELECT_ACTIVITY(A_NET_SOCK6);
			SELECT_ACTIVITY(A_NET_IP6);
			SELECT_ACTIVITY(A_NET_EIP6);
			SELECT_ACTIVITY(A_NET_ICMP6);
			SELECT_ACTIVITY(A_NET_EICMP6);
			SELECT_ACTIVITY(A_NET_UDP6);
			SELECT_ACTIVITY(A_NET_FC);
			SELECT_ACTIVITY(A_NET_SOFT);
		}
		else
			return 1;
	}

	(*opt)++;
	return 0;
}

/*
 ***************************************************************************
 * Parse sar "-q" option.
 *
 * IN:
 * @argv	Arguments list.
 * @opt		Index in list of arguments.
 *
 * OUT:
 * @act		Array of selected activities.
 *
 * RETURNS:
 * 0 on success, 1 otherwise.
 ***************************************************************************
 */
int parse_sar_q_opt(char *argv[], int *opt, struct activity *act[])
{
	char *t;

	for (t = strtok(argv[*opt], ","); t; t = strtok(NULL, ",")) {
		if (!strcmp(t, K_LOAD)) {
			SELECT_ACTIVITY(A_QUEUE);
		}
		else if (!strcmp(t, K_PSI_CPU)) {
			SELECT_ACTIVITY(A_PSI_CPU);
		}
		else if (!strcmp(t, K_PSI_IO)) {
			SELECT_ACTIVITY(A_PSI_IO);
		}
		else if (!strcmp(t, K_PSI_MEM)) {
			SELECT_ACTIVITY(A_PSI_MEM);
		}
		else if (!strcmp(t, K_PSI)) {
			SELECT_ACTIVITY(A_PSI_CPU);
			SELECT_ACTIVITY(A_PSI_IO);
			SELECT_ACTIVITY(A_PSI_MEM);
		}
		else if (!strcmp(t, K_ALL)) {
			SELECT_ACTIVITY(A_QUEUE);
			SELECT_ACTIVITY(A_PSI_CPU);
			SELECT_ACTIVITY(A_PSI_IO);
			SELECT_ACTIVITY(A_PSI_MEM);
		}
		else
			return 1;
	}

	(*opt)++;
	return 0;
}

/*
 ***************************************************************************
 * Parse sar and sadf "-P" option.
 *
 * IN:
 * @argv	Arguments list.
 * @opt		Index in list of arguments.
 * @act		Array of activities.
 *
 * OUT:
 * @flags	Common flags and system state.
 * @act		Array of activities, with CPUs selected.
 *
 * RETURNS:
 * 0 on success, 1 otherwise.
 ***************************************************************************
 */
int parse_sa_P_opt(char *argv[], int *opt, uint64_t *flags, struct activity *act[])
{
	int p;

	p = get_activity_position(act, A_CPU, EXIT_IF_NOT_FOUND);

	if (argv[++(*opt)]) {
		if (parse_values(argv[*opt], act[p]->bitmap->b_array,
			     act[p]->bitmap->b_size, K_LOWERALL))
			return 1;
		(*opt)++;
		*flags |= S_F_OPTION_P;
		return 0;
	}

	return 1;
}

/*
 ***************************************************************************
 * If option -A has been used, force -P ALL only if corresponding
 * option has not been explicitly entered on the command line.
 *
 * IN:
 * @flags	Common flags and system state.
 *
 * OUT:
 * @act		Array of selected activities.
 ***************************************************************************
 */
void set_bitmaps(struct activity *act[], uint64_t *flags)
{
	if (!USE_OPTION_P(*flags)) {
		/* Force -P ALL */
		int p = get_activity_position(act, A_CPU, EXIT_IF_NOT_FOUND);
		memset(act[p]->bitmap->b_array, ~0,
		       BITMAP_SIZE(act[p]->bitmap->b_size));
	}
}

/*
 ***************************************************************************
 * Parse devices entered on the command line and save them in activity's
 * list.
 *
 * IN:
 * @argv	Argument with list of devices.
 * @a		Activity for which devices are entered on the command line.
 * @max_len	Max length of a device name.
 * @opt		Index in list of arguments.
 * @pos		Position is string where is located the first device.
 * @max_val	If > 0 then ranges of values are allowed (e.g. 3-5,9-, etc.)
 *		Values are in range [0..@max_val].
 *
 * OUT:
 * @opt		Index on next argument.
 ***************************************************************************
 */
void parse_sa_devices(char *argv, struct activity *a, int max_len, int *opt, int pos,
		      int max_val)
{
	int i, val_low, val;
	char *t;
	char svalue[9];

	for (t = strtok(argv + pos, ","); t; t = strtok(NULL, ",")) {

		/* Test ranges of values, if allowed */
		if ((max_val > 0) && (strlen(t) <= 16) && (strspn(t, XDIGITS) == strlen(t))) {
			if (parse_range_values(t, max_val, &val_low, &val) == 0) {
				/* This is a real range of values: Save each if its values */
				for (i = val_low; i <= val; i++) {
					snprintf(svalue, sizeof(svalue), "%d", i);
					svalue[sizeof(svalue) - 1] = '\0';
					a->item_list_sz += add_list_item(&(a->item_list), svalue, max_len, NULL);
				}
				continue;
			}
		}
		a->item_list_sz += add_list_item(&(a->item_list), t, max_len, NULL);
	}
	if (a->item_list_sz) {
		a->options |= AO_LIST_ON_CMDLINE;
	}
	(*opt)++;
}

/*
 ***************************************************************************
 * Compute network interface utilization.
 *
 * IN:
 * @st_net_dev	Structure with network interface stats.
 * @rx		Number of bytes received per second.
 * @tx		Number of bytes transmitted per second.
 *
 * RETURNS:
 * NIC utilization (0-100%).
 ***************************************************************************
 */
double compute_ifutil(struct stats_net_dev *st_net_dev, double rx, double tx)
{
	if (st_net_dev->speed) {
		unsigned long long speed = (unsigned long long) st_net_dev->speed * 1000000;

		if (st_net_dev->duplex == C_DUPLEX_FULL) {
			/* Full duplex */
			if (rx > tx) {
				return (rx * 800 / speed);
			}
			else {
				return (tx * 800 / speed);
			}
		}
		else {
			/* Half duplex */
			return ((rx + tx) * 800 / speed);
		}
	}

	return 0;
}

/*
 ***************************************************************************
 * Read and replace unprintable characters in comment with ".".
 *
 * IN:
 * @ifd		Input file descriptor.
 * @comment	Comment.
 ***************************************************************************
 */
void replace_nonprintable_char(int ifd, char *comment)
{
	int i;

	/* Read comment */
	sa_fread(ifd, comment, MAX_COMMENT_LEN, HARD_SIZE, UEOF_STOP);
	comment[MAX_COMMENT_LEN - 1] = '\0';

	/* Replace non printable chars */
	for (i = 0; i < strlen(comment); i++) {
		if (!isprint(comment[i]))
			comment[i] = '.';
	}
}

/*
 ***************************************************************************
 * Fill the rectime and loctime structures with current record's date and
 * time, based on current record's "number of seconds since the epoch" saved
 * in file.
 * For loctime (if given): The timestamp is expressed in local time.
 * For rectime: The timestamp is expressed in UTC, in local time, or in the
 * time of the file's creator depending on options entered by the user on the
 * command line.
 *
 * IN:
 * @l_flags	Flags indicating the type of time expected by the user.
 * 		S_F_LOCAL_TIME means time should be expressed in local time.
 * 		S_F_TRUE_TIME means time should be expressed in time of
 * 		file's creator.
 * 		Default is time expressed in UTC (except for sar, where it
 * 		is local time).
 * @record_hdr	Record header containing the number of seconds since the
 * 		epoch, and the HH:MM:SS of the file's creator.
 *
 * OUT:
 * @rectime	Structure where timestamp for current record has been saved
 * 		(in local time, in UTC or in time of file's creator
 * 		depending on options used).
 *
 * RETURNS:
 * 1 if an error was detected, or 0 otherwise.
 ***************************************************************************
*/
int sa_get_record_timestamp_struct(uint64_t l_flags, struct record_header *record_hdr,
				   struct tstamp_ext *rectime)
{
	struct tm *ltm;
	time_t t = (time_t) record_hdr->ust_time;
	int rc = 0;

	rectime->epoch_time = record_hdr->ust_time;

	if (!PRINT_LOCAL_TIME(l_flags) && !PRINT_TRUE_TIME(l_flags)) {
		/*
		 * Get time in UTC
		 * (the user doesn't want local time nor time of file's creator).
		 */
		ltm = gmtime_r(&t, &(rectime->tm_time));
	}
	else {
		/*
		* Fill generic rectime structure in local time.
		* Done so that we have some default values.
		*/
		ltm = localtime_r(&t, &(rectime->tm_time));
		rectime->tm_time.tm_gmtoff = TRUE;
	}

	if (!ltm) {
		rc = 1;
	}

	if (PRINT_TRUE_TIME(l_flags)) {
		/* Time of file's creator */
		rectime->tm_time.tm_hour = record_hdr->hour;
		rectime->tm_time.tm_min  = record_hdr->minute;
		rectime->tm_time.tm_sec  = record_hdr->second;
	}

	return rc;
}

/*
 ***************************************************************************
 * Set current record's timestamp strings (date and time) using the time
 * data saved in @rectime structure. The string may be the number of seconds
 * since the epoch if flag S_F_SEC_EPOCH has been set.
 *
 * IN:
 * @l_flags	Flags indicating the type of time expected by the user.
 * 		S_F_SEC_EPOCH means the time should be expressed in seconds
 * 		since the epoch (01/01/1970).
 * @cur_date	String where timestamp's date will be saved. May be NULL.
 * @cur_time	String where timestamp's time will be saved.
 * @len		Maximum length of timestamp strings.
 * @rectime	Structure with current timestamp (expressed in local time or
 *		in UTC depending on whether options -T or -t have been used
 * 		or not) that should be broken down in date and time strings.
 *
 * OUT:
 * @cur_date	Timestamp's date string (if expected).
 * @cur_time	Timestamp's time string. May contain the number of seconds
 *		since the epoch (01-01-1970) if corresponding option has
 * 		been used.
 ***************************************************************************
*/
void set_record_timestamp_string(uint64_t l_flags, char *cur_date, char *cur_time, int len,
				 struct tstamp_ext *rectime)
{
	/* Set cur_time date value */
	if (PRINT_SEC_EPOCH(l_flags) && cur_date) {
		sprintf(cur_time, "%llu", rectime->epoch_time);
		strcpy(cur_date, "");
	}
	else {
		/*
		 * If options -T or -t have been used then cur_time is
		 * expressed in local time. Else it is expressed in UTC.
		 */
		if (cur_date) {
			strftime(cur_date, len, "%Y-%m-%d", &(rectime->tm_time));
		}
		if (USE_PREFD_TIME_OUTPUT(l_flags)) {
			strftime(cur_time, len, "%X", &(rectime->tm_time));
		}
		else {
			strftime(cur_time, len, "%H:%M:%S", &(rectime->tm_time));
		}
	}
}

/*
 ***************************************************************************
 * Print contents of a special (RESTART or COMMENT) record.
 * Note: This function is called only when reading a file.
 *
 * IN:
 * @record_hdr	Current record header.
 * @l_flags	Flags for common options.
 * @tm_start	Structure filled when option -s has been used.
 * @tm_end	Structure filled when option -e has been used.
 * @rtype	Record type (R_RESTART or R_COMMENT).
 * @ifd		Input file descriptor.
 * @rectime	Structure where timestamp (expressed in local time or in UTC
 *		depending on whether options -T/-t have been used or not) can
 *		be saved for current record.
 * @file	Name of file being read.
 * @tab		Number of tabulations to print.
 * @my_tz	Current timezone.
 * @file_magic	file_magic structure filled with file magic header data.
 * @file_hdr	System activity file standard header.
 * @act		Array of activities.
 * @ofmt	Pointer on report output format structure.
 * @endian_mismatch
 *		TRUE if file's data don't match current machine's endianness.
 * @arch_64	TRUE if file's data come from a 64 bit machine.
 *
 * OUT:
 * @rectime	Structure where timestamp (expressed in local time or in UTC)
 *		has been saved.
 *
 * RETURNS:
 * 1 if the record has been successfully displayed, and 0 otherwise.
 ***************************************************************************
 */
int print_special_record(struct record_header *record_hdr, uint64_t l_flags,
			 struct tstamp_ext *tm_start, struct tstamp_ext *tm_end, int rtype,
			 int ifd, struct tstamp_ext *rectime, char *file, int tab, char *my_tz,
			 struct file_magic *file_magic, struct file_header *file_hdr,
			 struct activity *act[], struct report_format *ofmt,
			 int endian_mismatch, int arch_64)
{
	char cur_date[TIMESTAMP_LEN], cur_time[TIMESTAMP_LEN];
	int dp = 1;

	/* Fill timestamp structure (rectime) for current record */
	if (sa_get_record_timestamp_struct(l_flags, record_hdr, rectime))
		return 0;

	/* The record must be in the interval specified by -s/-e options */
	if ((datecmp(rectime, tm_start, FALSE) < 0) ||
	    (datecmp(rectime, tm_end, FALSE) > 0)) {
		/* Will not display the special record */
		dp = 0;
	}
	else {
		/* Set date and time strings to be displayed for current record */
		set_record_timestamp_string(l_flags, cur_date, cur_time, TIMESTAMP_LEN,
					    rectime);
	}

	if (rtype == R_RESTART) {
		int p;

		/* Read new cpu number following RESTART record */
		file_hdr->sa_cpu_nr = read_nr_value(ifd, file, file_magic,
						    endian_mismatch, arch_64, TRUE, NR_CPUS + 1);

		/*
		 * We don't know if CPU related activities will be displayed or not.
		 * But if it is the case, @nr_ini will be used in the loop
		 * to process all CPUs. So update their value here and
		 * reallocate buffers if needed.
		 * NB: We may have nr_allocated=0 here if the activity has
		 * not been collected in file (or if it has an unknown format).
		 */
		for (p = 0; p < NR_ACT; p++) {
			if (HAS_PERSISTENT_VALUES(act[p]->options) && (act[p]->nr_ini > 0)) {
				act[p]->nr_ini = file_hdr->sa_cpu_nr;
				if (act[p]->nr_ini > act[p]->nr_allocated) {
					reallocate_buffers(act[p], act[p]->nr_ini, l_flags);
				}
			}
		}

		/* Ignore unknown extra structures if present */
		if (record_hdr->extra_next && (skip_extra_struct(ifd, endian_mismatch, arch_64) < 0))
			return 0;

		if (!dp)
			return 0;

		if (*ofmt->f_restart) {
			(*ofmt->f_restart)(&tab, F_MAIN, cur_date, cur_time, my_tz, file_hdr, record_hdr);
		}
	}
	else if (rtype == R_COMMENT) {
		char file_comment[MAX_COMMENT_LEN];

		/* Read and replace non printable chars in comment */
		replace_nonprintable_char(ifd, file_comment);

		/* Ignore unknown extra structures if present */
		if (record_hdr->extra_next && (skip_extra_struct(ifd, endian_mismatch, arch_64) < 0))
			return 0;

		if (!dp || !DISPLAY_COMMENT(l_flags))
			return 0;

		if (*ofmt->f_comment) {
			(*ofmt->f_comment)(&tab, F_MAIN, cur_date, cur_time, my_tz,
					   file_comment, file_hdr, record_hdr);
		}
	}

	return 1;
}

/*
 ***************************************************************************
 * Compute global CPU statistics as the sum of individual CPU ones, and
 * calculate interval for global CPU.
 * Also identify offline CPU.
 *
 * IN:
 * @a		Activity structure with statistics.
 * @prev	Index in array where stats used as reference are.
 * @curr	Index in array for current sample statistics.
 * @flags	Flags for common options and system state.
 * @offline_cpu_bitmap
 *		CPU bitmap for offline CPU.
 *
 * OUT:
 * @a		Activity structure with updated statistics (those for global
 *		CPU, and also those for offline CPU).
 * @offline_cpu_bitmap
 *		CPU bitmap with offline CPU.
 *
 * RETURNS:
 * Interval for global CPU.
 ***************************************************************************
 */
unsigned long long get_global_cpu_statistics(struct activity *a, int prev, int curr,
					     uint64_t flags, unsigned char offline_cpu_bitmap[])
{
	int i;
	unsigned long long tot_jiffies_c, tot_jiffies_p;
	unsigned long long deltot_jiffies = 0;
	struct stats_cpu *scc, *scp;
	struct stats_cpu *scc_all = (struct stats_cpu *) ((char *) a->buf[curr]);
	struct stats_cpu *scp_all = (struct stats_cpu *) ((char *) a->buf[prev]);

	/*
	 * Initial processing.
	 * Compute CPU "all" as sum of all individual CPU. Done only on SMP machines (a->nr_ini > 1).
	 * For UP machines we keep the values read from global CPU line in /proc/stat.
	 * Also look for offline CPU: They won't be displayed, and some of their values may
	 * have to be modified.
	 */
	if (a->nr_ini > 1) {
		memset(scc_all, 0, sizeof(struct stats_cpu));
		memset(scp_all, 0, sizeof(struct stats_cpu));
	}

	for (i = 1; (i < a->nr_ini) && (i < a->bitmap->b_size + 1); i++) {

		/*
		 * The size of a->buf[...] CPU structure may be different from the default
		 * sizeof(struct stats_cpu) value if data have been read from a file!
		 * That's why we don't use a syntax like:
		 * scc = (struct stats_cpu *) a->buf[...] + i;
		 */
		scc = (struct stats_cpu *) ((char *) a->buf[curr] + i * a->msize);
		scp = (struct stats_cpu *) ((char *) a->buf[prev] + i * a->msize);

		/*
		 * Compute the total number of jiffies spent by current processor.
		 * NB: Don't add cpu_guest/cpu_guest_nice because cpu_user/cpu_nice
		 * already include them.
		 */
		tot_jiffies_c = scc->cpu_user + scc->cpu_nice +
				scc->cpu_sys + scc->cpu_idle +
				scc->cpu_iowait + scc->cpu_hardirq +
				scc->cpu_steal + scc->cpu_softirq;
		tot_jiffies_p = scp->cpu_user + scp->cpu_nice +
				scp->cpu_sys + scp->cpu_idle +
				scp->cpu_iowait + scp->cpu_hardirq +
				scp->cpu_steal + scp->cpu_softirq;

		/*
		 * If the CPU is offline then it is omited from /proc/stat:
		 * All the fields couldn't have been read and the sum of them is zero.
		 */
		if (tot_jiffies_c == 0) {
			/*
			 * CPU is currently offline.
			 * Set current struct fields (which have been set to zero)
			 * to values from previous iteration. Hence their values won't
			 * jump from zero when the CPU comes back online.
			 * Note that this workaround no longer fully applies with recent kernels,
			 * as I have noticed that when a CPU comes back online, some fields
			 * restart from their previous value (e.g. user, nice, system)
			 * whereas others restart from zero (idle, iowait)! To deal with this,
			 * the get_per_cpu_interval() function will set these previous values
			 * to zero if necessary.
			 */
			*scc = *scp;

			/*
			 * Mark CPU as offline to not display it
			 * (and thus it will not be confused with a tickless CPU).
			 */
			MARK_CPU_OFFLINE(offline_cpu_bitmap, i);
		}

		if ((tot_jiffies_p == 0) && !WANT_SINCE_BOOT(flags)) {
			/*
			 * CPU has just come back online.
			 * Unfortunately, no reference values are available
			 * from a previous iteration, probably because it was
			 * already offline when the first sample has been taken.
			 * So don't display that CPU to prevent "jump-from-zero"
			 * output syndrome, and don't take it into account for CPU "all".
			 */
			MARK_CPU_OFFLINE(offline_cpu_bitmap, i);
			continue;
		}

		/*
		 * Get interval for current CPU and add it to global CPU.
		 * Note: Previous idle and iowait values (saved in scp) may be modified here.
		 */
		deltot_jiffies += get_per_cpu_interval(scc, scp);

		scc_all->cpu_user += scc->cpu_user;
		scp_all->cpu_user += scp->cpu_user;

		scc_all->cpu_nice += scc->cpu_nice;
		scp_all->cpu_nice += scp->cpu_nice;

		scc_all->cpu_sys += scc->cpu_sys;
		scp_all->cpu_sys += scp->cpu_sys;

		scc_all->cpu_idle += scc->cpu_idle;
		scp_all->cpu_idle += scp->cpu_idle;

		scc_all->cpu_iowait += scc->cpu_iowait;
		scp_all->cpu_iowait += scp->cpu_iowait;

		scc_all->cpu_hardirq += scc->cpu_hardirq;
		scp_all->cpu_hardirq += scp->cpu_hardirq;

		scc_all->cpu_steal += scc->cpu_steal;
		scp_all->cpu_steal += scp->cpu_steal;

		scc_all->cpu_softirq += scc->cpu_softirq;
		scp_all->cpu_softirq += scp->cpu_softirq;

		scc_all->cpu_guest += scc->cpu_guest;
		scp_all->cpu_guest += scp->cpu_guest;

		scc_all->cpu_guest_nice += scc->cpu_guest_nice;
		scp_all->cpu_guest_nice += scp->cpu_guest_nice;
	}

	return deltot_jiffies;
}

/*
 ***************************************************************************
 * Compute softnet statistics for CPU "all" as the sum of individual CPU
 * ones.
 * Also identify offline CPU.
 *
 * IN:
 * @a		Activity structure with statistics.
 * @prev	Index in array where stats used as reference are.
 * @curr	Index in array for current sample statistics.
 * @flags	Flags for common options and system state.
 * @offline_cpu_bitmap
 *		CPU bitmap for offline CPU.
 *
 * OUT:
 * @a		Activity structure with updated statistics (those for global
 *		CPU, and also those for offline CPU).
 * @offline_cpu_bitmap
 *		CPU bitmap with offline CPU.
 ***************************************************************************
 */
void get_global_soft_statistics(struct activity *a, int prev, int curr,
				uint64_t flags, unsigned char offline_cpu_bitmap[])
{
	int i;
	struct stats_softnet *ssnc, *ssnp;
	struct stats_softnet *ssnc_all = (struct stats_softnet *) ((char *) a->buf[curr]);
	struct stats_softnet *ssnp_all = (struct stats_softnet *) ((char *) a->buf[prev]);

	/*
	 * Init structures that will contain values for CPU "all".
	 * CPU "all" doesn't exist in /proc/net/softnet_stat file, so
	 * we compute its values as the sum of the values of each CPU.
	 */
	memset(ssnc_all, 0, sizeof(struct stats_softnet));
	memset(ssnp_all, 0, sizeof(struct stats_softnet));

	for (i = 1; (i < a->nr_ini) && (i < a->bitmap->b_size + 1); i++) {

		/*
		 * The size of a->buf[...] CPU structure may be different from the default
		 * sizeof(struct stats_softnet) value if data have been read from a file!
		 * That's why we don't use a syntax like:
		 * ssnc = (struct stats_softnet *) a->buf[...] + i;
                 */
                ssnc = (struct stats_softnet *) ((char *) a->buf[curr] + i * a->msize);
                ssnp = (struct stats_softnet *) ((char *) a->buf[prev] + i * a->msize);

		if ((ssnp->processed + ssnp->dropped + ssnp->time_squeeze +
		    ssnp->received_rps + ssnp->flow_limit + ssnp->backlog_len == 0) &&
		    !WANT_SINCE_BOOT(flags)) {
			/*
			 * No previous sample for current CPU: Don't display it unless
			 * we want stats since last boot time.
			 * (CPU may be online but we don't display it because all
			 * its counters would appear to jump from zero...)
			 */
			MARK_CPU_OFFLINE(offline_cpu_bitmap, i);
			continue;
		}

		if (ssnc->processed + ssnc->dropped + ssnc->time_squeeze +
		    ssnc->received_rps + ssnc->flow_limit + ssnc->backlog_len == 0) {
			/* Assume current CPU is offline */
			*ssnc = *ssnp;
			MARK_CPU_OFFLINE(offline_cpu_bitmap, i);
		}

		ssnc_all->processed += ssnc->processed;
		ssnc_all->dropped += ssnc->dropped;
		ssnc_all->time_squeeze += ssnc->time_squeeze;
		ssnc_all->received_rps += ssnc->received_rps;
		ssnc_all->flow_limit += ssnc->flow_limit;
		ssnc_all->backlog_len += ssnc->backlog_len;

		ssnp_all->processed += ssnp->processed;
		ssnp_all->dropped += ssnp->dropped;
		ssnp_all->time_squeeze += ssnp->time_squeeze;
		ssnp_all->received_rps += ssnp->received_rps;
		ssnp_all->flow_limit += ssnp->flow_limit;
		ssnp_all->backlog_len += ssnp->backlog_len;
	}
}

/*
 ***************************************************************************
 * Identify offline CPU (those for which all interrupts are 0) and keep
 * interrupts statistics (their values are persistent). Include also CPU
 * which have not been selected (this is necessary so that the header of the
 * interrupts statistics report can be displayed).
 *
 * IN:
 * @a		Activity structure with statistics.
 * @prev	Index in array where stats used as reference are.
 * @curr	Index in array for current sample statistics.
 * @flags	Flags for common options and system state.
 * @masked_cpu_bitmap
 *		CPU bitmap for offline and unselected CPU.
 *
 * OUT:
 * @a		Activity structure with updated statistics (those for global
 *		CPU, and also those for offline CPU).
 * @masked_cpu_bitmap
 *		CPU bitmap with offline and unselected CPU.
 ***************************************************************************
 */
void get_global_int_statistics(struct activity *a, int prev, int curr,
			       uint64_t flags, unsigned char masked_cpu_bitmap[])
{
	int i;
	struct stats_irq *stc_cpu_sum, *stp_cpu_sum;

	for (i = 0; (i < a->nr_ini) && (i < a->bitmap->b_size + 1); i++) {

		/*
		 * The size of a->buf[...] CPU structure may be different from the default
		 * sizeof(struct stats_irq) value if data have been read from a file!
		 * That's why we don't use a syntax like:
		 * stc_cpu_sum = (struct stats_irq *) a->buf[...] + i;
                 */
		stc_cpu_sum = (struct stats_irq *) ((char *) a->buf[curr] + i * a->msize * a->nr2);
		stp_cpu_sum = (struct stats_irq *) ((char *) a->buf[prev] + i * a->msize * a->nr2);

		/*
		 * Check if current CPU is back online but with no previous sample for it,
		 * or if it has not been selected.
		 */
		if (((stp_cpu_sum->irq_nr == 0) && !WANT_SINCE_BOOT(flags)) ||
		    !IS_CPU_SELECTED(a->bitmap->b_array, i)) {
			/* CPU should not be displayed */
			SET_CPU_BITMAP(masked_cpu_bitmap, i);
			continue;
		}

		if (stc_cpu_sum->irq_nr == 0) {
			/* Assume current CPU is offline */
			SET_CPU_BITMAP(masked_cpu_bitmap, i);
			memcpy(stc_cpu_sum, stp_cpu_sum, (size_t) a->msize * a->nr2);
		}

	}
}

/*
 ***************************************************************************
 * Get filesystem name to display. This may be either the persistent name
 * if requested by the user, the standard filesystem name (e.g. /dev/sda1,
 * /dev/sdb3, etc.) or the mount point. This is used when displaying
 * filesystem statistics: sar -F or sadf -- -F).
 *
 * IN:
 * @a		Activity structure.
 * @flags	Flags for common options and system state.
 * @st_fs	Statistics for current filesystem.
 *
 * RETURNS:
 * Filesystem name to display.
 ***************************************************************************
 */
char *get_fs_name_to_display(struct activity *a, uint64_t flags, struct stats_filesystem *st_fs)
{
	char *pname = NULL, *persist_dev_name;

	if (DISPLAY_PERSIST_NAME_S(flags) && !DISPLAY_MOUNT(a->opt_flags)) {
		char fname[MAX_FS_LEN];

		strncpy(fname, st_fs->fs_name, sizeof(fname));
		fname[sizeof(fname) - 1] = '\0';
		if ((persist_dev_name = get_persistent_name_from_pretty(basename(fname))) != NULL) {
			pname = persist_dev_name;
		}
	}
	if (!pname) {
		pname = DISPLAY_MOUNT(a->opt_flags) ? st_fs->mountp : st_fs->fs_name;
	}
	return pname;
}

/*
 * **************************************************************************
 * Make a few checks on timestamps entered with options -s/-e.
 *
 * IN:
 * @tm_start	Timestamp entered with option -s.
 * @tm_end	Timestamp entered with option -e.
 *
 * RETURNS:
 * 1 if an error has been detected.
 ***************************************************************************
 */
int check_time_limits(struct tstamp_ext *tm_start, struct tstamp_ext *tm_end)
{
	if ((tm_start->use == USE_HHMMSS_T) && (tm_end->use == USE_HHMMSS_T) &&
	    (tm_end->tm_time.tm_hour < tm_start->tm_time.tm_hour)) {
		tm_end->tm_time.tm_hour += 24;
	}

	if ((tm_start->use == USE_EPOCH_T) && (tm_end->use == USE_EPOCH_T) &&
	    (tm_end->epoch_time < tm_start->epoch_time))
		return 1;

	return 0;
}

/*
 * **************************************************************************
 * Check for min and max values.
 *
 * IN:
 * @a		Activity structure with statistics.
 * @idx		Index in min/max buffers.
 * @val		Value to check.
 ***************************************************************************
 */
void save_minmax(struct activity *a, int idx, double val)
{
	if (val < *(a->spmin + idx)) {
		*(a->spmin + idx) = val;
	}
	if (val > *(a->spmax + idx)) {
		*(a->spmax + idx) = val;
	}
}

/*
 * **************************************************************************
 * Compare the values of a statistics sample with the max and min values
 * already found in previous samples for this same activity. If some new
 * min or max values are found, then save them.
 * Assume values cannot be negative.
 * The structure containing the statistics sample is composed of @llu_nr
 * unsigned long long fields, followed by @lu_nr unsigned long fields, then
 * followed by @u_nr unsigned int fields.
 *
 * IN:
 * @types_nr	Number of fields whose type is "long long", "long" and "int"
 * 		composing the structure.
 * @cs		Pointer on current sample statistics structure.
 * @ps		Pointer on previous sample statistics structure (may be NULL).
 * @itv		Interval of time in 1/100th of a second.
 * @spmin	Array containing min values already found for this activity.
 * @spmax	Array containing max values already found for this activity.
 * @g_fields	Index in spmin/spmax arrays where extrema values for each
 *		activity metric will be saved. As a consequence spmin/spmax
 *		arrays may contain values in a different order than that of
 *		the fields in the statistics structure.
 *
 * OUT:
 * @spmin	Array containing the possible new min values for current activity.
 * @spmax	Array containing the possible new max values for current activity.
 ***************************************************************************
 */
void save_extrema(const unsigned int types_nr[], void *cs, void *ps, unsigned long long itv,
		  double *spmin, double *spmax, int g_fields[])
{
	unsigned long long *lluc, *llup;
	unsigned long *luc, *lup;
	unsigned int *uc, *up;
	double val;
	int i, m = 0;

	/* Compare unsigned long long fields */
	lluc = (unsigned long long *) cs;
	llup = (unsigned long long *) ps;
	for (i = 0; i < types_nr[0]; i++, m++) {
		if (g_fields[m] >= 0) {
			if (ps) {
				val = *lluc < *llup ? 0.0 : S_VALUE(*llup, *lluc, itv);
			}
			else {
				/*
				 * If no pointer on previous sample has been given
				 * then the value is not a per-second one.
				 */
				val = (double) *lluc;
			}
			if (val < *(spmin + g_fields[m])) {
				*(spmin + g_fields[m]) = val;
			}
			if (val > *(spmax + g_fields[m])) {
				*(spmax + g_fields[m]) = val;
			}
		}
		lluc = (unsigned long long *) ((char *) lluc + ULL_ALIGNMENT_WIDTH);
		if (ps) {
			llup = (unsigned long long *) ((char *) llup + ULL_ALIGNMENT_WIDTH);
		}
	}

	/* Compare unsigned long fields */
	luc = (unsigned long *) lluc;
	lup = (unsigned long *) llup;
	for (i = 0; i < types_nr[1]; i++, m++) {
		if (g_fields[m] >= 0) {
			if (ps) {
				val = *luc < *lup ? 0.0 : S_VALUE(*lup, *luc, itv);
			}
			else {
				val = (double) *luc;
			}
			if (val < *(spmin + g_fields[m])) {
				*(spmin + g_fields[m]) = val;
			}
			if (val > *(spmax + g_fields[m])) {
				*(spmax + g_fields[m]) = val;
			}
		}
		luc = (unsigned long *) ((char *) luc + UL_ALIGNMENT_WIDTH);
		if (ps) {
			lup = (unsigned long *) ((char *) lup + UL_ALIGNMENT_WIDTH);
		}
	}

	/* Compare unsigned int fields */
	uc = (unsigned int *) luc;
	up = (unsigned int *) lup;
	for (i = 0; i < types_nr[2]; i++, m++) {
		if (g_fields[m] >= 0) {
			if (ps) {
				val = *uc < *up ? 0.0 : S_VALUE(*up, *uc, itv);
			}
			else {
				val = (double) *uc;
			}
			if (val < *(spmin + g_fields[m])) {
				*(spmin + g_fields[m]) = val;
			}
			if (val > *(spmax + g_fields[m])) {
				*(spmax + g_fields[m]) = val;
			}
		}
		uc = (unsigned int *) ((char *) uc + U_ALIGNMENT_WIDTH);
		if (ps) {
			up = (unsigned int *) ((char *) up + U_ALIGNMENT_WIDTH);
		}
	}
}

/*
 * **************************************************************************
 * Init min and max values. Also free linked list with device names.
 *
 * IN:
 * @a	Activity structure.
 * @nr	Number of slots for min and max values that shall be initialized.
 *
 * OUT:
 * @a	Activity structure with min/max values initialized.
 ***************************************************************************
 */
void init_extrema_values(struct activity *a, int nr)
{
	int i;

	for (i = 0; i < nr; i++) {
		*(a->spmin + i) = DBL_MAX;
		*(a->spmax + i) = -DBL_MAX;
	}

	free_item_list(&(a->xdev_list));
}

/*
 * **************************************************************************
 * Print min and max header.
 *
 * IN:
 * @ismax	TRUE: Display max header - FALSE: Display min header.
 ***************************************************************************
 */
void print_minmax(int ismax)
{
	printf("%-11s", ismax ? _("Maximum:")
			      : _("Minimum:"));
}

#endif /* SOURCE_SADC undefined */
