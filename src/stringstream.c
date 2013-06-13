#include <stringstream.h>

#include <malloc.h>
#include <string.h>

struct stringstream ss_create()
{
	struct stringstream ss;

	// Create default sized stringstream
	ss.max_len = SS_DEFAULT_LENGTH;
	ss.buffer = malloc(SS_DEFAULT_LENGTH);
	ss.buffer[0] = 0;
	ss.len = 0;

	return ss;
}

void ss_destroy(struct stringstream *ss)
{
	// If this stringstream hasn't already been destroyed
	if (ss->buffer != NULL)
	{
		// Deallocate the memory and 0 all values
		free(ss->buffer);
		ss->max_len = 0;
		ss->buffer = NULL;
		ss->len = 0;
	}
}

void ss_add(struct stringstream* ss, const char* string)
{
	// If this stringstream hasn't already been destroyed
	if (ss->buffer != NULL)
	{
		size_t str_len = strlen(string);
		size_t space_remaining = ss->max_len - ss->len;

		while (str_len > space_remaining)
		{
			ss->max_len *= SS_GROWTH_CONSTANT;
			ss->buffer = realloc(ss->buffer, ss->max_len);

			space_remaining = ss->max_len - ss->len;
		}

		strcpy(ss->buffer + ss->len, string);
		ss->len += str_len;
	}
}

void ss_clear(struct stringstream* ss)
{
	// If this stringstream hasn't already been destroyed
	if (ss->buffer != NULL)
	{
		// If the maximum length is greater than 0
		if (ss->max_len > 0)
		{
			// Set the first character to 0, effectively clearing the string
			ss->buffer[0] = 0;
		}
	}
}
