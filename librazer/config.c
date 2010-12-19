/*
 *   Copyright (C) 2010 Michael Buesch <mb@bu3sch.de>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include "config.h"
#include "razer_private.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>


static inline int strcmp_case(const char *a, const char *b, bool ignorecase)
{
	if (ignorecase)
		return strcasecmp(a, b);
	return strcmp(a, b);
}

static char * string_strip(char *str)
{
	char *start = str;
	size_t len;

	if (!str)
		return NULL;
	while (*start != '\0' && isspace(*start))
		start++;
	len = strlen(start);
	while (len && isspace(start[len - 1])) {
		start[len - 1] = '\0';
		len--;
	}

	return start;
}

static void free_item(struct config_item *i)
{
	if (i) {
		free(i->name);
		free(i->value);
		free(i);
	}
}

static void free_items(struct config_item *i)
{
	struct config_item *next;

	for ( ; i; i = next) {
		next = i->next;
		free_item(i);
	}
}

static void free_section(struct config_section *s)
{
	if (s) {
		free_items(s->items);
		free(s->name);
		free(s);
	}
}

static void free_sections(struct config_section *s)
{
	struct config_section *next;

	for ( ; s; s = next) {
		next = s->next;
		free_section(s);
	}
}

const char * config_get(struct config_file *f,
			const char *section,
			const char *item,
			const char *_default,
			unsigned int flags)
{
	struct config_section *s;
	struct config_item *i;
	const char *retval = _default;

	if (!f || !section || !item)
		return _default;
	for (s = f->sections; s; s = s->next) {
		if (strcmp_case(s->name, section, !!(flags & CONF_SECT_NOCASE)) == 0) {
			for (i = s->items; i; i = i->next) {
				if (strcmp_case(i->name, item, !!(flags & CONF_ITEM_NOCASE)) == 0) {
					retval = i->value;
					break;
				}
			}
			break;
		}
	}

	return retval;
}

static int string_to_int(const char *string, int *i)
{
	char *tailptr;
	long retval;

	retval = strtol(string, &tailptr, 0);
	if (tailptr == string || tailptr[0] != '\0')
		return -EINVAL;
	*i = retval;

	return 0;
}

int config_get_int(struct config_file *f,
		   const char *section,
		   const char *item,
		   int _default,
		   unsigned int flags)
{
	const char *value;
	int i;

	value = config_get(f, section, item, NULL, flags);
	if (!value)
		return _default;
	if (string_to_int(value, &i))
		return _default;

	return i;
}

int config_get_bool(struct config_file *f,
		    const char *section,
		    const char *item,
		    int _default,
		    unsigned int flags)
{
	const char *value;
	int i;

	value = config_get(f, section, item, NULL, flags);
	if (!value)
		return _default;
	if (strcmp_case(value, "yes", !!(flags & CONF_VALUE_NOCASE)) == 0 ||
	    strcmp_case(value, "true", !!(flags & CONF_VALUE_NOCASE)) == 0 ||
	    strcmp_case(value, "on", !!(flags & CONF_VALUE_NOCASE)) == 0)
		return 1;
	if (strcmp_case(value, "no", !!(flags & CONF_VALUE_NOCASE)) == 0 ||
	    strcmp_case(value, "false", !!(flags & CONF_VALUE_NOCASE)) == 0 ||
	    strcmp_case(value, "off", !!(flags & CONF_VALUE_NOCASE)) == 0)
		return 0;
	if (string_to_int(value, &i))
		return _default;

	return !!i;
}

#define list_append(container, baseptr, item) do {	\
		__typeof__(item) last;			\
		item->next = NULL;			\
		if (!(container->baseptr)) {		\
			container->baseptr = item;	\
			return;				\
		}					\
		for (last = container->baseptr;		\
		     last->next;			\
		     last = last->next)			\
			;				\
		last->next = item;			\
	} while (0)

static void append_section(struct config_file *f, struct config_section *s)
{
	list_append(f, sections, s);
}

static void append_item(struct config_section *s, struct config_item *i)
{
	list_append(s, items, i);
}

struct config_file * config_file_parse(const char *path)
{
	struct config_file *f;
	struct config_section *s = NULL;
	struct config_item *i;
	FILE *fd;
	char *name, *value;
	size_t len;
	unsigned int lineno = 0;
	ssize_t count;
	size_t linebuf_size = 0;
	char *linebuf = NULL, *line;

	f = zalloc(sizeof(*f));
	if (!f)
		goto error;
	f->path = strdup(path);
	if (!f->path)
		goto err_free_f;
	fd = fopen(path, "rb");
	if (!fd) {
		fprintf(stderr, "Failed to open config file %s: %s\n",
			path, strerror(errno));
		goto err_free_path;
	}

	while (1) {
		count = getline(&linebuf, &linebuf_size, fd);
		if (count <= 0)
			break;
		line = string_strip(linebuf);

		lineno++;
		len = strlen(line);
		if (!len)
			continue;
		if (line[0] == '#') /* comment */
			continue;
		if (len >= 3 && line[0] == '[' && line[len - 1] == ']') {
			/* New section */
			s = zalloc(sizeof(*s));
			if (!s)
				goto error_unwind;
			s->file = f;
			line[len - 1] = '\0'; /* strip ] */
			s->name = strdup(line + 1); /* strip [ */
			if (!s->name) {
				free(s);
				goto error_unwind;
			}
			append_section(f, s);
			continue;
		}
		if (!s) {
			fprintf(stderr, "%s:%u: Stray characters\n", path, lineno);
			goto error_unwind;
		}
		/* Config item in section */
		value = strchr(line, '=');
		if (!value) {
			fprintf(stderr, "%s:%u: Invalid config item\n", path, lineno);
			goto error_unwind;
		}
		value[0] = '\0';
		value++;
		name = line;
		i = zalloc(sizeof(*i));
		if (!i)
			goto error_unwind;
		i->section = s;
		i->name = strdup(name);
		if (!i->name) {
			free(i);
			goto error_unwind;
		}
		i->value = strdup(value);
		if (!i->value) {
			free(i->name);
			free(i);
			goto error_unwind;
		}
		append_item(s, i);
	}
	free(linebuf);
	fclose(fd);

	return f;

error_unwind:
	free_sections(f->sections);
	free(linebuf);
	fclose(fd);
err_free_path:
	free(f->path);
err_free_f:
	free(f);
error:
	return NULL;
}

void config_file_free(struct config_file *f)
{
	if (f) {
		free_sections(f->sections);
		free(f->path);
		free(f);
	}
}
