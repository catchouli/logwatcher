#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <stdarg.h>
#include <time.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <microhttpd.h>
#include <sqlite3.h>

#include "errors.h"
#include "queries.h"
#include "stringstream.h"

#define PORT     9002
#define LOGFILE  "/home/rena/irclogs/renaporn/#talkhaus.log"

#define LOAD_LOG_INITIAL 1
#define MAX_LOG_MESSAGES -1

// Parse lines of log
void parse_lines(char* lines);

// Parse line of log
void parse_line(const char* line);

// Generate statistics page in response to http request
int generate_statistics(void *cls, struct MHD_Connection *connection,
                          const char *url,
                          const char *method, const char *version,
                          const char *upload_data,
                          size_t *upload_data_size, void **con_cls);

// Execute multi statement SQL
int execute_sql(const char* sql);

// Get count of rows in table
int count_sql(const char* table);

sqlite3* db;                  // Sqlite database
char* sqlite_error = NULL;    // Sqlite error

int sqlite_messages = 0;      // Messages in message table

// Entry point
int main(int argc, char** argv)
{
	int inotify_fd;                 // File descriptor for inotify
	int inotify_wd;                 // Watch descriptor for logfile
	struct inotify_event event;     // inotify event struct

	FILE* logfile_fd;               // File descriptor for logfile
	long logfile_len;               // Logfile length
	char* logfile_new_text = NULL;  // New logfile data

	struct MHD_Daemon* daemon;      // microhttpd daemon

	int rc;                         // Return code

	// Initialise inotify
	printf("Initialising inotify...\n");
	inotify_fd = inotify_init();

	if (inotify_fd < 0)
	{
		fprintf(stderr, INOTIFY_INIT_FAILURE);
		return INOTIFY_INIT_FAILURE_ID;
	}

	// Add a watch to logfile
	printf("Watching logfile...\n");
	inotify_wd = inotify_add_watch(inotify_fd, LOGFILE, IN_MODIFY);

	if (inotify_wd < 0)
	{
		fprintf(stderr, INOTIFY_WATCH_FAILURE);
		return INOTIFY_WATCH_FAILURE_ID;
	}

	// Get logfile length
	logfile_fd = fopen(LOGFILE, "r");
	fseek(logfile_fd, 0, SEEK_END);
	logfile_len = ftell(logfile_fd);

	// Read logfile
	printf("Reading logfile...\n");
	fseek(logfile_fd, 0, SEEK_SET);
	logfile_new_text = malloc(logfile_len);

	rc = fread(logfile_new_text, 1, logfile_len, logfile_fd);
	if (rc <= 0)
	{
		printf("%s %d\n", "No data read from logfile", errno);
	}

	// Initialise httpd
	printf("Initialising httpd...\n");
	daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
					&generate_statistics, NULL, MHD_OPTION_END);
	if (daemon == NULL)
	{
		fprintf(stderr, MHD_INIT_FAILURE);
		return MHD_INIT_FAILURE_ID;
	}

	// Create sqlite database
	printf("Creating database...\n");
	rc = sqlite3_open(":memory:", &db);
	if (rc)
	{
		fprintf(stderr, SQLITE_DATABASE_CREATION_FAILURE, sqlite3_errmsg(db));
		return SQLITE_DATABASE_CREATION_FAILURE_ID;
	}

	// Initialise database
	printf("Creating database tables...\n");
	execute_sql(TABLE_CREATION);

	// Iterate through lines
	printf("Parsing logfile...\n");
	if (LOAD_LOG_INITIAL)
		parse_lines(logfile_new_text);
	printf("Finished parsing logfile.\n");

	// Deallocate memory
	free(logfile_new_text);
	logfile_new_text = NULL;

	// Wait for changes
	printf("Waiting for new messages...\n");
	while (read(inotify_fd, &event, sizeof(struct inotify_event)))
	{
		char* new_text;
		size_t new_text_len;
		long new_logfile_len;

		// Get new length
		fseek(logfile_fd, 0, SEEK_END);
		new_logfile_len = ftell(logfile_fd);
		new_text_len = (size_t)(new_logfile_len - logfile_len);
		new_text = malloc(new_text_len);

		// Seek to new messages
		fseek(logfile_fd, logfile_len, SEEK_SET);

		// Read new text
		fread(new_text, new_text_len, 1, logfile_fd);

		// Handle new text
		parse_lines(new_text);

		// Update logfile length
		logfile_len = new_logfile_len;

		// Deallocate memory
		free(new_text);
	}

	// Clean up descriptors
	inotify_rm_watch(inotify_fd, inotify_wd);
	fclose(logfile_fd);

	return 0;
}

void parse_lines(char* lines)
{
	int messages = 0;

	char* position;

	// Iterate through lines
	position = strtok(lines, "\n");
	while (position != NULL)
	{
		if (MAX_LOG_MESSAGES >= 0)
		{
			messages++;
			if (messages > MAX_LOG_MESSAGES)
				break;
		}

		// Parse line from log
		parse_line(position);

		// Next line
		position = strtok(NULL, "\n");
	}
}

void parse_line(const char* line)
{
	int hour;              // Hour
	int minute;            // Minute
	char nick[8192];       // Nickname
	char message[8192];    // Message text

	int rc;                // Return code

	hour = 0;
	minute = 0;
	strcpy(nick, "nick");
	strcpy(message, "message");

	// Parse message string
	rc = sscanf(line, "%d:%d <%[^>]> %[^\n]", &hour, &minute, nick, message);

	// If this is a message string
	if (rc == 4)
	{
		sqlite3_stmt* statement;

		// Remove first character from nick (op char)
		strcpy(nick, nick+1);

		// Add message to database
		rc = sqlite3_prepare_v2(db, ADD_MESSAGE, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		}

		// Bind values
		sqlite3_bind_text(statement, 1, nick, -1, SQLITE_STATIC);
		sqlite3_bind_text(statement, 2, message, -1, SQLITE_STATIC);

		// Run statement
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		}

		// Delete statement
		sqlite3_finalize(statement);

		// Increment message count for user
		rc = sqlite3_prepare_v2(db, INCREMENT_MESSAGE_COUNT, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		}

		// Bind values
		sqlite3_bind_text(statement, 1, nick, -1, SQLITE_STATIC);

		// Run statement
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		}

		// Delete statement
		sqlite3_finalize(statement);

		// Check that a row was modified
		if (sqlite3_changes(db) == 0)
		{
			// If not, insert initial row
			rc = sqlite3_prepare_v2(db, INSERT_MESSAGE_COUNT, -1, &statement, NULL);
			if (rc != SQLITE_OK)
			{
				fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
			}

			// Bind values
			sqlite3_bind_text(statement, 1, nick, -1, SQLITE_STATIC);

			// Run statement
			rc = sqlite3_step(statement);
			if (rc != SQLITE_DONE)
			{
				fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			}

			// Delete statement
			sqlite3_finalize(statement);
		}

		// Increment total message count
		sqlite_messages++;
	}
}

int generate_statistics(void *cls, struct MHD_Connection *connection,
                          const char *url,
                          const char *method, const char *version,
                          const char *upload_data,
                          size_t *upload_data_size, void **con_cls)
{
	int i;                                 // Counter
	char buffer[8192];                     // String buffer

	int rc;                                // Return code
	sqlite3_stmt* statement;               // Sqlite prepared statement

	struct stringstream ss;                // Stringstream for output

	struct MHD_Response* response;         // HTTP response

	int random_id;                         // Random id for selection

	clock_t start, finish;                 // Structures for timing
	double time_taken;                     // Time taken for execution

	// Constants (which should probably be moved into a config file at some point in the future)
	const int max_highscore_users = 10;    // Number of users to show in the message count highscores
	const int random_message_count = 10;   // Number of random messages wanted

	// Initialise start time
	start = clock();

	// Create stringstream
	ss = ss_create();

	// Write start of page
	ss_add(&ss, "<html><head><title>Renaporn stats for #talkhaus</title></head><body>");
	ss_add(&ss, "<h1>Renaporn stats for #talkhaus</h1>");

	// Print out top max_highscore_users users with most messages
	ss_add(&ss, "<h2>Users with the most messages</h2><table>");

	// Create statement
	rc = sqlite3_prepare_v2(db, SELECT_TOP_USERS, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, max_highscore_users);

	// Iterate through results
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		const unsigned char* nick;
		int messages;

		// Get values
		nick = sqlite3_column_text(statement, 0);
		messages = sqlite3_column_int(statement, 1);

		// Output data
		sprintf(buffer, "<tr><td>%s</td><td>%d</td></tr>", nick, messages);
		ss_add(&ss, buffer);

		// Get next row
		rc = sqlite3_step(statement);
	}

	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
	}

	// Finalise query
	sqlite3_finalize(statement);

	// End table
	ss_add(&ss, "</table>");

	// Print out random_message_count random rows
	ss_add(&ss, "<h2>10 random messages from log</h2><table>");
	if (sqlite_messages > 0)
	{
		// Create prepared statement
		rc = sqlite3_prepare_v2(db, SELECT_MESSAGE_AT_POSITION, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		}

		// Iterate random_message_count times, selecting a row each time
		for (i = 0; i < random_message_count; ++i)
		{
			// Select random id
			random_id = rand() % sqlite_messages;

			// Bind parameters
			sqlite3_bind_int(statement, 1, random_id);

			// Run statement
			rc = sqlite3_step(statement);
			while (rc == SQLITE_ROW)
			{
				const unsigned char* nick;
				const unsigned char* message;

				nick = sqlite3_column_text(statement, 0);
				message = sqlite3_column_text(statement, 1);

				sprintf(buffer, "<tr><td>&lt;%s&gt; %s</td></tr>", nick, message);

				ss_add(&ss, buffer);

				rc = sqlite3_step(statement);
			}

			// Print error if statement not complete
			if (rc != SQLITE_DONE)
			{
				fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			}

			// reset statement for next request
			sqlite3_reset(statement);
		}
	}

	// End table
	ss_add(&ss, "</table>");

	// Delete statement
	sqlite3_finalize(statement);

	// Get finish time
	finish = clock();
	time_taken = ((double)(finish - start))/CLOCKS_PER_SEC;

	// Generate footer
	sprintf(buffer, "<p>Row count: %d<br>Time taken to generate: %g seconds</p>", sqlite_messages, time_taken);

	// Write footer
	ss_add(&ss, "<br>");
	ss_add(&ss, buffer);
	ss_add(&ss, "</body></html>");

	// Create response
	response = MHD_create_response_from_buffer(strlen(ss.buffer), (void*)ss.buffer, MHD_RESPMEM_PERSISTENT);

	rc = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return rc;
}

int execute_sql(const char* sql)
{
	int rc;                          // Return code
	sqlite3_stmt* statement = NULL;  // Pepared statement
	char* pos = (char*)sql;          // Current location in string

	while (pos != NULL && pos[0] != 0)
	{
		// Prepare statement
		rc = sqlite3_prepare_v2(db, pos, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_TABLE_CREATION_FAILURE, sqlite3_errmsg(db));
			return SQLITE_TABLE_CREATION_FAILURE_ID;
		}

		// Execute statement
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_TABLE_CREATION_FAILURE, sqlite3_errmsg(db));
			return SQLITE_TABLE_CREATION_FAILURE_ID;
		}

		// Reset statement
		sqlite3_reset(statement);

		// Find next statement
		pos = strstr(pos, ";") + 1;
	}

	// Delete statement
	sqlite3_finalize(statement);

	return 0;
}

int count_sql(const char* table)
{
	int count = 0;                   // Row count
	int rc;                          // Return code
	sqlite3_stmt* statement = NULL;  // Pepared statement

	// Prepare statement
	rc = sqlite3_prepare_v2(db, "Select Count(*) FROM users;", -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
	}

	// Bind table name
	sqlite3_bind_text(statement, 1, table, -1, NULL);

	// Execute statement
	rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW)
	{
		count = sqlite3_column_int(statement, 0);

		// This should set rc to SQLITE_DONE (count should only return one row)
		rc = sqlite3_step(statement);
	}

	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, "Count returned more than one row?\n");
	}

	// Get row count
	count = sqlite3_column_int(statement, 0);

	// Reset statement
	sqlite3_reset(statement);

	// Delete statement
	sqlite3_finalize(statement);

	return count;
}
