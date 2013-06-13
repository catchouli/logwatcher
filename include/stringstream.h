#ifndef __STRINGSTREAM_H__
#define __STRINGSTREAM_H__

#include <stddef.h>

#define SS_DEFAULT_LENGTH   1024
#define SS_GROWTH_CONSTANT  2

struct stringstream
{
	char* buffer;
	size_t len, max_len;
};

struct stringstream ss_create();
void ss_destroy(struct stringstream* ss);
void ss_add(struct stringstream* ss, const char* string);
void ss_clear(struct stringstream* ss);

#endif /* __STRINGSTREAM_H__ */

