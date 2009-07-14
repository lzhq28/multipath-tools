/*
 * Copyright (c) 2004, 2005 Christophe Varoqui
 * Copyright (c) 2005 Stefan Bader, IBM
 * Copyright (c) 2005 Edward Goggin, EMC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkers.h"
#include "vector.h"
#include "memory.h"
#include "structs.h"
#include "util.h"
#include "debug.h"
#include "config.h"

#define WORD_SIZE 64

static int
merge_words (char ** dst, char * word, int space)
{
	char * p;
	int len;

	len = strlen(*dst) + strlen(word) + space;
	*dst = REALLOC(*dst, len + 1);

	if (!*dst)
		return 1;

	p = *dst;

	while (*p != '\0')
		p++;

	while (space) {
		*p = ' ';
		p++;
		space--;
	}
	strncpy(p, word, strlen(word) + 1);

	return 0;
}

static int
add_feature (char **f, char *n)
{
	int c = 0, d, l;
	char *e, *p, *t;

	if (!f)
		return 1;

	/* Nothing to do */
	if (!n || *n == '0')
		return 0;

	/* Check if feature is already present */
	if (strstr(*f, n))
		return 0;

	/* Get feature count */
	c = strtoul(*f, &e, 10);
	if (*f == e)
		/* parse error */
		return 1;

	/* Check if we need to increase feature count space */
	l = strlen(*f) + strlen(n) + 1;

	/* Count new features */
	if ((c % 10) == 9)
		l++;
	c++;
	p = n;
	while (*p != '\0') {
		if (*p == ' ' && p[1] != '\0' && p[1] != ' ') {
			if ((c % 10) == 9)
				l++;
			c++;
		}
		p++;
	}

	t = MALLOC(l + 1);
	if (!t)
		return 1;

	memset(t, 0, l + 1);

	/* Update feature count */
	d = c;
	l = 1;
	while (d > 9) {
		d /= 10;
		l++;
	}
	p = t;
	snprintf(p, l + 2, "%0d ", c);

	/* Copy the feature string */
	p = strchr(*f, ' ');
	if (p) {
		while (*p == ' ')
			p++;
		strcat(t, p);
		strcat(t, " ");
	} else {
		p = t + strlen(t);
	}
	strcat(t, n);

	FREE(*f);
	*f = t;

	return 0;
}

static int remove_feature(char **f, char *o)
{
	int c = 0, d, l;
	char *e, *p, *n;

	if (!f || !*f)
		return 1;

	/* Nothing to do */
	if (!o || *o == '\0')
		return 0;

	/* Check if not present */
	if (!strstr(*f, o))
		return 0;

	/* Get feature count */
	c = strtoul(*f, &e, 10);
	if (*f == e)
		/* parse error */
		return 1;

	/* Normalize features */
	while (*o == ' ') {
		o++;
	}
	/* Just spaces, return */
	if (*o == '\0')
		return 0;
	e = o + strlen(o);
	while (*e == ' ')
		e--;
	d = (int)(e - o);

	/* Update feature count */
	c--;
	p = o;
	while (p[0] != '\0') {
		if (p[0] == ' ' && p[1] != ' ' && p[1] != '\0')
			c--;
		p++;
	}

	/* Quick exit if all features have been removed */
	if (c == 0) {
		n = MALLOC(2);
		if (!n)
			return 1;
		strcpy(n, "0");
		goto out;
	}

	/* Search feature to be removed */
	e = strstr(*f, o);
	if (!e)
		/* Not found, return */
		return 0;

	/* Update feature count space */
	l = strlen(*f) - d;
	n =  MALLOC(l + 1);
	if (!n)
		return 1;

	/* Copy the feature count */
	sprintf(n, "%0d", c);
	/*
	 * Copy existing features up to the feature
	 * about to be removed
	 */
	p = strchr(*f, ' ');
	while (*p == ' ')
		p++;
	p--;
	if (e != p) {
		do {
			e--;
			d++;
		} while (*e == ' ');
		e++; d--;
		strncat(n, p, (size_t)(e - p));
		p += (size_t)(e - p);
	}
	/* Skip feature to be removed */
	p += d;

	/* Copy remaining features */
	if (strlen(p)) {
		while (*p == ' ')
			p++;
		if (strlen(p)) {
			p--;
			strcat(n, p);
		}
	}

out:
	FREE(*f);
	*f = n;

	return 0;
}

/*
 * Transforms the path group vector into a proper device map string
 */
int
assemble_map (struct multipath * mp, char * params, int len)
{
	int i, j;
	int shift, freechar;
	int minio;
	char * p, * f;
	char no_path_retry[] = "queue_if_no_path";
	struct pathgroup * pgp;
	struct path * pp;

	minio = mp->minio;
	p = params;
	freechar = len;

	f = STRDUP(mp->features);

	if (mp->no_path_retry == NO_PATH_RETRY_UNDEF ||
	    mp->no_path_retry == NO_PATH_RETRY_FAIL) {
		/* remove queue_if_no_path settings */
		condlog(3, "%s: remove queue_if_no_path from '%s'",
			mp->alias, mp->features);
		remove_feature(&f, no_path_retry);
	} else {
		add_feature(&f, no_path_retry);
	}

	shift = snprintf(p, freechar, "%s %s %i %i",
			 f, mp->hwhandler,
			 VECTOR_SIZE(mp->pg), mp->bestpg);

	FREE(f);

	if (shift >= freechar) {
		condlog(0, "%s: params too small\n", mp->alias);
		return 1;
	}
	p += shift;
	freechar -= shift;

	vector_foreach_slot (mp->pg, pgp, i) {
		pgp = VECTOR_SLOT(mp->pg, i);
		shift = snprintf(p, freechar, " %s %i 1", mp->selector,
				 VECTOR_SIZE(pgp->paths));
		if (shift >= freechar) {
			condlog(0, "%s: params too small\n", mp->alias);
			return 1;
		}
		p += shift;
		freechar -= shift;

		vector_foreach_slot (pgp->paths, pp, j) {
			int tmp_minio = minio;

			if (mp->rr_weight == RR_WEIGHT_PRIO
			    && pp->priority > 0)
				tmp_minio = minio * pp->priority;
			if (!strlen(pp->dev_t) ) {
				condlog(0, "dev_t not set for '%s'\n", pp->dev);
				return 1;
			}
			shift = snprintf(p, freechar, " %s %d",
					 pp->dev_t, tmp_minio);
			if (shift >= freechar) {
				condlog(0, "%s: params too small\n", mp->alias);
				return 1;
			}
			p += shift;
			freechar -= shift;
		}
	}
	if (freechar < 1) {
		condlog(0, "%s: params too small\n", mp->alias);
		return 1;
	}
	snprintf(p, 1, "\n");

	condlog(3, "%s: assembled map [%s]\n", mp->alias, params);

	return 0;
}

extern int
disassemble_map (vector pathvec, char * params, struct multipath * mpp)
{
	char * word;
	char * p;
	int i, j, k;
	int num_features = 0;
	int num_hwhandler = 0;
	int num_pg = 0;
	int num_pg_args = 0;
	int num_paths = 0;
	int num_paths_args = 0;
	int def_minio = 0;
	struct path * pp;
	struct pathgroup * pgp;

	p = params;

	condlog(3, "%s: disassemble map [%s]\n", mpp->alias, params);

	/*
	 * features
	 */
	p += get_word(p, &mpp->features);

	if (!mpp->features)
		return 1;

	num_features = atoi(mpp->features);

	for (i = 0; i < num_features; i++) {
		p += get_word(p, &word);

		if (!word)
			return 1;

		if (merge_words(&mpp->features, word, 1)) {
			FREE(word);
			return 1;
		}
		if ((mpp->no_path_retry == NO_PATH_RETRY_UNDEF) ||
			(mpp->no_path_retry == NO_PATH_RETRY_FAIL) ||
			(mpp->no_path_retry == NO_PATH_RETRY_QUEUE))
			setup_feature(mpp, word);
		FREE(word);
	}

	/*
	 * hwhandler
	 */
	p += get_word(p, &mpp->hwhandler);

	if (!mpp->hwhandler)
		return 1;

	num_hwhandler = atoi(mpp->hwhandler);

	for (i = 0; i < num_hwhandler; i++) {
		p += get_word(p, &word);

		if (!word)
			return 1;

		if (merge_words(&mpp->hwhandler, word, 1)) {
			FREE(word);
			return 1;
		}
		FREE(word);
	}

	/*
	 * nb of path groups
	 */
	p += get_word(p, &word);

	if (!word)
		return 1;

	num_pg = atoi(word);
	FREE(word);

	if (num_pg > 0 && !mpp->pg) {
		mpp->pg = vector_alloc();

		if (!mpp->pg)
			return 1;
	} else
		mpp->pg = NULL;

	/*
	 * first pg to try
	 */
	p += get_word(p, &word);

	if (!word)
		goto out;

	mpp->nextpg = atoi(word);
	FREE(word);

	for (i = 0; i < num_pg; i++) {
		/*
		 * selector
		 */

		if (!mpp->selector) {
			p += get_word(p, &mpp->selector);

			if (!mpp->selector)
				goto out;

			/*
			 * selector args
			 */
			p += get_word(p, &word);

			if (!word)
				goto out;

			num_pg_args = atoi(word);

			if (merge_words(&mpp->selector, word, 1)) {
				FREE(word);
				goto out1;
			}
			FREE(word);
		} else {
			p += get_word(p, NULL);
			p += get_word(p, NULL);
		}

		for (j = 0; j < num_pg_args; j++)
			p += get_word(p, NULL);

		/*
		 * paths
		 */
		pgp = alloc_pathgroup();

		if (!pgp)
			goto out;

		if (store_pathgroup(mpp->pg, pgp))
			goto out;

		p += get_word(p, &word);

		if (!word)
			goto out;

		num_paths = atoi(word);
		FREE(word);

		p += get_word(p, &word);

		if (!word)
			goto out;

		num_paths_args = atoi(word);
		FREE(word);

		for (j = 0; j < num_paths; j++) {
			pp = NULL;
			p += get_word(p, &word);

			if (!word)
				goto out;

			if (pathvec)
				pp = find_path_by_devt(pathvec, word);

			if (!pp) {
				pp = alloc_path();

				if (!pp)
					goto out1;

				strncpy(pp->dev_t, word, BLK_DEV_SIZE);

				/* Only call this in multipath client mode */
				if (!conf->daemon && store_path(pathvec, pp))
					goto out1;
			}
			FREE(word);

			if (store_path(pgp->paths, pp))
				goto out;

			/*
			 * Update wwid for multipaths which are not setup
			 * in the get_dm_mpvec() code path
			 */
			if (!strlen(mpp->wwid))
				strncpy(mpp->wwid, pp->wwid, WWID_SIZE);

			/*
			 * Update wwid for paths which may not have been
			 * active at the time the getuid callout was run
			 */
			else if (!strlen(pp->wwid))
				strncpy(pp->wwid, mpp->wwid, WWID_SIZE);

			pgp->id ^= (long)pp;
			pp->pgindex = i + 1;

			for (k = 0; k < num_paths_args; k++)
				if (k == 0) {
					if (!strncmp(mpp->selector,
						     "round-robin", 11)) {
						p += get_word(p, &word);
						def_minio = atoi(word);

						if (mpp->rr_weight == RR_WEIGHT_PRIO
						    && pp->priority > 0)
							def_minio /= pp->priority;

						FREE(word);
					} else
						def_minio = 0;

					if (def_minio != mpp->minio)
						mpp->minio = def_minio;
				}
				else
					p += get_word(p, NULL);

		}
	}
	return 0;
out1:
	FREE(word);
out:
	free_pgvec(mpp->pg, KEEP_PATHS);
	mpp->pg = NULL;
	return 1;
}

extern int
disassemble_status (char * params, struct multipath * mpp)
{
	char * word;
	char * p;
	int i, j, k;
	int num_feature_args;
	int num_hwhandler_args;
	int num_pg;
	int num_pg_args;
	int num_paths;
	int def_minio = 0;
	struct path * pp;
	struct pathgroup * pgp;

	p = params;

	condlog(3, "%s: disassemble status [%s]\n", mpp->alias, params);

	/*
	 * features
	 */
	p += get_word(p, &word);

	if (!word)
		return 1;

	num_feature_args = atoi(word);
	FREE(word);

	for (i = 0; i < num_feature_args; i++) {
		if (i == 1) {
			p += get_word(p, &word);

			if (!word)
				return 1;

			mpp->queuedio = atoi(word);
			FREE(word);
			continue;
		}
		/* unknown */
		p += get_word(p, NULL);
	}
	/*
	 * hwhandler
	 */
	p += get_word(p, &word);

	if (!word)
		return 1;

	num_hwhandler_args = atoi(word);
	FREE(word);

	for (i = 0; i < num_hwhandler_args; i++)
		p += get_word(p, NULL);

	/*
	 * nb of path groups
	 */
	p += get_word(p, &word);

	if (!word)
		return 1;

	num_pg = atoi(word);
	FREE(word);

	if (num_pg == 0)
		return 0;

	/*
	 * next pg to try
	 */
	p += get_word(p, NULL);

	if (VECTOR_SIZE(mpp->pg) < num_pg)
		return 1;

	for (i = 0; i < num_pg; i++) {
		pgp = VECTOR_SLOT(mpp->pg, i);
		/*
		 * PG status
		 */
		p += get_word(p, &word);

		if (!word)
			return 1;

		switch (*word) {
		case 'D':
			pgp->status = PGSTATE_DISABLED;
			break;
		case 'A':
			pgp->status = PGSTATE_ACTIVE;
			break;
		case 'E':
			pgp->status = PGSTATE_ENABLED;
			break;
		default:
			pgp->status = PGSTATE_UNDEF;
			break;
		}
		FREE(word);

		/*
		 * PG Status (discarded, would be '0' anyway)
		 */
		p += get_word(p, NULL);

		p += get_word(p, &word);

		if (!word)
			return 1;

		num_paths = atoi(word);
		FREE(word);

		p += get_word(p, &word);

		if (!word)
			return 1;

		num_pg_args = atoi(word);
		FREE(word);

		if (VECTOR_SIZE(pgp->paths) < num_paths)
			return 1;

		for (j = 0; j < num_paths; j++) {
			pp = VECTOR_SLOT(pgp->paths, j);
			/*
			 * path
			 */
			p += get_word(p, NULL);

			/*
			 * path status
			 */
			p += get_word(p, &word);

			if (!word)
				return 1;

			switch (*word) {
			case 'F':
				pp->dmstate = PSTATE_FAILED;
				break;
			case 'A':
				pp->dmstate = PSTATE_ACTIVE;
				break;
			default:
				break;
			}
			FREE(word);
			/*
			 * fail count
			 */
			p += get_word(p, &word);

			if (!word)
				return 1;

			pp->failcount = atoi(word);
			FREE(word);

			/*
			 * selector args
			 */
			for (k = 0; k < num_pg_args; k++) {
				if (!strncmp(mpp->selector,
					     "least-pending", 13)) {
					p += get_word(p, &word);
					if (sscanf(word,"%d:*d",
						   &def_minio) == 1 &&
					    def_minio != mpp->minio)
							mpp->minio = def_minio;
				} else
					p += get_word(p, NULL);
			}
		}
	}
	return 0;
}
