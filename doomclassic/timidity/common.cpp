/*

TiMidity -- Experimental MIDI to WAVE converter
Copyright (C) 1995 Tuukka Toivonen <toivonen@clinet.fi>

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

common.c

*/

#include "../../neo/idlib/precompiled.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include "config.h"
#include "common.h"
#include "output.h"
#include "controls.h"

/* I guess "rb" should be right for any libc */
#define OPEN_MODE "rb"

char current_filename[1024];

#ifdef DEFAULT_PATH
/* The paths in this list will be tried whenever we're reading a file */
static PathList defaultpathlist={DEFAULT_PATH,0};
static PathList *pathlist=&defaultpathlist; /* This is a linked list */
#else
static PathList *pathlist=0;
#endif

/* Try to open a file for reading. If the filename ends in one of the 
defined compressor extensions, pipe the file through the decompressor */
static idFile * try_to_open(char *name, int decompress, int noise_mode)
{	
	idFile * fp;

	fp = fileSystem->OpenFileRead( name );

	if (!fp)
		return 0;

	return fp;
}

/* This is meant to find and open files for reading, possibly piping
them through a decompressor. */
static bool path_fits(size_t prefix_length, size_t separator_length, size_t name_length)
{
	size_t remaining=sizeof(current_filename)-1; /* Reserve the NUL first. */
	if (prefix_length>remaining)
		return false;
	remaining-=prefix_length;
	if (separator_length>remaining)
		return false;
	remaining-=separator_length;
	return name_length<=remaining;
}

idFile * open_file(const char *name, int decompress, int noise_mode)
{
	idFile * fp;
	PathList *plp=pathlist;

	if (!name || !(*name))
	{
		ctl->cmsg(CMSG_ERROR, VERB_NORMAL, "Attempted to open nameless file.");
		return 0;
	}

	const size_t name_length=strlen(name);
	if (!path_fits(0, 0, name_length))
	{
		/* Never open a truncated name that could identify another file. */
		*current_filename=0;
		errno=ENAMETOOLONG;
		if (noise_mode>=2)
			ctl->cmsg(CMSG_ERROR, VERB_NORMAL, "%s: %s", name, strerror(errno));
		return 0;
	}

	/* First try the given name */
	memcpy(current_filename, name, name_length+1);

	ctl->cmsg(CMSG_INFO, VERB_DEBUG, "Trying to open %s", current_filename);
	if ((fp=try_to_open(current_filename, decompress, noise_mode)))
		return fp;

	if (name[0] != PATH_SEP)
		while (plp)  /* Try along the path then */
		{
			const size_t l=strlen(plp->path);
			const size_t separator_length=(l && plp->path[l-1]!=PATH_SEP) ? sizeof(PATH_STRING)-1 : 0;
			if (!path_fits(l, separator_length, name_length))
			{
				/* One oversized directory must not hide later valid paths. */
				errno=ENAMETOOLONG;
				plp=(PathList*)plp->next;
				continue;
			}
			memcpy(current_filename, plp->path, l);
			if (separator_length)
				memcpy(current_filename+l, PATH_STRING, separator_length);
			memcpy(current_filename+l+separator_length, name, name_length+1);
			ctl->cmsg(CMSG_INFO, VERB_DEBUG, "Trying to open %s", current_filename);
			if ((fp=try_to_open(current_filename, decompress, noise_mode)))
				return fp;

			plp=(PathList*)plp->next;
		}

		/* Nothing could be opened. */

		*current_filename=0;

		if (noise_mode>=2)
			ctl->cmsg(CMSG_ERROR, VERB_NORMAL, "%s: %s", name, strerror(errno));

		return 0;
}

/* This closes files opened with open_file */
void close_file(idFile * fp)
{
	delete fp;
}

/* This is meant for skipping a few bytes in a file or fifo. */
void skip(idFile * fp, size_t len)
{
	size_t c;
	char tmp[1024];
	while (len>0)
	{
		c=len;
		if (c>1024) c=1024;
		len-=c;
		if (c!=fp->Read(tmp, c ))
			ctl->cmsg(CMSG_ERROR, VERB_NORMAL, "%s: skip: %s",
			current_filename, strerror(errno));
	}
}

//extern void *Real_Tim_Malloc( size_t );
/* This'll allocate memory or die. */
void *safe_malloc(size_t count)
{
	void *p;
	if (count > (1<<21))
	{
		ctl->cmsg(CMSG_FATAL, VERB_NORMAL, 
			"Strange, I feel like allocating %d bytes. This must be a bug.",
			count);
	}
	else if ((p=Real_Tim_Malloc(count)))
		return p;
	else
		ctl->cmsg(CMSG_FATAL, VERB_NORMAL, "Sorry. Couldn't malloc %d bytes.", count);

	ctl->close();
	//exit(10);
	return(NULL);
}

/* This adds a directory to the path list */
void add_to_pathlist(char *s)
{
	PathList *plp=(PathList*)safe_malloc(sizeof(PathList));
	strcpy((plp->path=(char *)safe_malloc(strlen(s)+1)),s);
	plp->next=pathlist;
	pathlist=plp;
}

/* Required memory management functions */
void *Real_Tim_Malloc( int sz ) {
	return malloc( sz );
}

void Real_Tim_Free( void *pt ) {
	free( pt );
}

void* Real_Malloc( unsigned int sz ) {
	return malloc( sz );
}
