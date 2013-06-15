#define _XOPEN_SOURCE 600

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
#include <libconfig.h>

#include "errors.h"
#include "queries.h"
#include "stringstream.h"

#define PORT     9003
#define LOGFILE  "/home/rena/irclogs/renaporn/#talkhaus.log"
//#define LOGFILE  "/home/rena/irclogs/1272/#srs_bsns.log"
#define DATABASE ":memory:"
#define CONFIG_FILE "logwatcher.conf"

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

// Convert unix time to string
int convert_time_to_string(time_t time, char* buffer, size_t buffer_len, const char* format);

// Timer thing
void timer_start();
void timer_end();

// Globals
sqlite3* db;                  // Sqlite database
char* sqlite_error = NULL;    // Sqlite error

int sqlite_messages = 0;      // Messages in message table

time_t current_day = 0;       // Current day (last encountered in log)

// Entry point
int main(int argc, char** argv)
{
	config_t config;                 // Config structure
	const config_setting_t* setting; // Config setting
	int config_array_len;            // Config array length

	int inotify_fd;                  // File descriptor for inotify
	int inotify_wd;                  // Watch descriptor for logfile
	struct inotify_event event;      // inotify event struct

	FILE* logfile_fd;                // File descriptor for logfile
	long logfile_len;                // Logfile length
	char* logfile_new_text = NULL;   // New logfile data

	struct MHD_Daemon* daemon;       // microhttpd daemon

	int rc;                          // Return code

	// Initialise config struct
	config_init(&config);

	// Read config file
	rc = config_read_file(&config, CONFIG_FILE);
	if (rc != CONFIG_TRUE)
	{
		fprintf(stderr, CONFIG_LOAD_FAILURE, config_error_text(&config), config_error_line(&config));
		return CONFIG_LOAD_FAILURE_ID;
	}

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
	rc = sqlite3_open(DATABASE, &db);
	if (rc)
	{
		fprintf(stderr, SQLITE_DATABASE_CREATION_FAILURE, sqlite3_errmsg(db));
		return SQLITE_DATABASE_CREATION_FAILURE_ID;
	}

	// Load extensions
	printf("Loading sqlite extensions...\n");

	// Enable extension loading
	sqlite3_enable_load_extension(db, 1);

	// Load extension functions
	rc = sqlite3_load_extension(db, "extension-functions.so", 0, &sqlite_error);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_EXTENSION_LOADING_FAILURE, sqlite_error);
	}

	// Disable extension loading
	sqlite3_enable_load_extension(db, 1);

	// Initialise database
	printf("Creating database tables...\n");
	execute_sql(TABLE_CREATION);

	// Read aliases from config file
	setting = config_lookup(&config, "logwatcher.aliases");
	if (setting == 0)
	{
		fprintf(stderr, "Failed to load aliases from config file\n");
		return -1;
	}

	// Get number of aliases
	config_array_len = config_setting_length(setting);
	if (config_array_len > 0)
	{
		int i, j, k;

		k = 0;

		printf("Loading %d sets aliases...\n", config_array_len);
		for (i = 0; i < config_array_len; ++i)
		{
			const config_setting_t* inner_array;

			inner_array = config_setting_get_elem(setting, i);

			if (inner_array != NULL)
			{
				int inner_array_len = config_setting_length(inner_array);

				if (inner_array_len < 2)
				{
					fprintf(stderr, "Warning: <2 elements in aliases array, format: aliases ( [alias, nick, ...], [alias, nick, ...], ... )\n");
				}
                                else if (inner_array_len >= 2)
                                {
					sqlite3_stmt* statement;

                                        const char* alias;
                                        const char* nick;

					alias = config_setting_get_string_elem(inner_array, 0);

					for (j = 1; j < inner_array_len; ++j)
					{
						nick = config_setting_get_string_elem(inner_array, j);

						printf("Adding alias %s => %s\n", nick, alias);

						// Add to database
						rc = sqlite3_prepare_v2(db, INSERT_ALIAS, -1, &statement, NULL);
						if (rc != SQLITE_OK)
						{
							fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
							fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_TOPIC);
						}

						// Bind values
						sqlite3_bind_text(statement, 1, nick, -1, SQLITE_STATIC);
						sqlite3_bind_text(statement, 2, alias, -1, SQLITE_STATIC);

						// Insert
						rc = sqlite3_step(statement);
						if (rc != SQLITE_DONE)
						{
							fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
							fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_TOPIC);
						}

						// Finalise query
						sqlite3_finalize(statement);

						k++;
					}
				}
			}
		}

		printf("Successfully loaded %d aliases\n", k);
	}

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

	// Clean up
	config_destroy(&config);
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
	int year;
	char month[32];
	int date;
	int hour;
	int minute;

	int rc;                // Return code
	int rc2;               // Return code

	time_t time;

	char* nick = NULL;     // Nickname
	char* message = NULL;  // Message

	struct tm time_struct;

	int line_len = strlen(line);

	hour = 0;
	minute = 0;

	// Allocate sscanf buffers
	nick = malloc(line_len);
	message = malloc(line_len);

	// Parse log open and day change message
	rc = sscanf(line, "--- Log opened %*s %s %d %*d:%*d:%*d %d", month, &date, &year);
	rc2 = sscanf(line, "--- Day changed %*s %s %d %d", month, &date, &year);
	if (rc == 3 || rc2 == 3)
	{
		const int buffer_len = 1024;
		char buffer[buffer_len];

		// Create date string
		snprintf(buffer, buffer_len, "%d %s %d", date, month, year);

		// Parse date string
		if (strptime(buffer, "%d %b %Y", &time_struct) == NULL)
		{
			fprintf(stderr, "Failed to parse date format %s\n", buffer);

			return;
		}

		// Convert day to unix time
		current_day = mktime(&time_struct);

		goto cleanup;
	}

	// Parse topic
	rc = sscanf(line, "%d:%d -!- %s changed the topic of %*s to: %[^\n]", &hour, &minute, nick, message);

	if (rc == 4)
	{
		time_t time;
		sqlite3_stmt* statement;

		// Add topic to database
		rc = sqlite3_prepare_v2(db, INSERT_TOPIC, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_TOPIC);
			goto cleanup;
		}

		// Work out time
		time = current_day + hour * 3600 + minute * 60;

		// Bind values
		sqlite3_bind_int(statement, 1, (int)time);
		sqlite3_bind_text(statement, 2, nick, -1, SQLITE_STATIC);
		sqlite3_bind_text(statement, 3, message, -1, SQLITE_STATIC);

		// Insert
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_TOPIC);
		}

		// Finalise query
		sqlite3_finalize(statement);

		goto cleanup;
	}

	// Parse message string
	rc = sscanf(line, "%d:%d <%[^>]> %[^\n]", &hour, &minute, nick, message);

	if (rc == 4)
	{
		sqlite3_stmt* statement;

		// Remove first character from nick (op char)
		strcpy(nick, nick+1);

		// Add message to database
		rc = sqlite3_prepare_v2(db, INSERT_MESSAGE, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_MESSAGE);
			goto cleanup;
		}

		// Calculate time
		time = current_day + hour * 3600 + minute * 60;

		// Bind values
		sqlite3_bind_text(statement, 1, nick, -1, SQLITE_STATIC);
		sqlite3_bind_text(statement, 2, message, -1, SQLITE_STATIC);
		sqlite3_bind_int(statement, 3, time);
		//sqlite3_bind_text(statement, 4, nick, -1, SQLITE_STATIC);

		// Run statement
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_MESSAGE);
		}

		// Delete statement
		sqlite3_finalize(statement);

		// Increment message count for user
		rc = sqlite3_prepare_v2(db, INCREMENT_MESSAGE_COUNT, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, INCREMENT_MESSAGE_COUNT);
			goto cleanup;
		}

		// Bind values
		sqlite3_bind_int(statement, 1, time);
		sqlite3_bind_text(statement, 2, nick, -1, SQLITE_STATIC);

		// Run statement
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, INCREMENT_MESSAGE_COUNT);
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
				fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_MESSAGE_COUNT);
				goto cleanup;
			}

			// Bind values
			sqlite3_bind_text(statement, 1, nick, -1, SQLITE_STATIC);
			sqlite3_bind_int(statement, 2, time);

			// Run statement
			rc = sqlite3_step(statement);
			if (rc != SQLITE_DONE)
			{
				fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
				fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_MESSAGE_COUNT);
			}

			// Delete statement
			sqlite3_finalize(statement);
		}

		// Increment total message count
		sqlite_messages++;

		goto cleanup;
	}

cleanup:
	free(nick);
	free(message);
}

int generate_statistics(void *cls, struct MHD_Connection *connection,
                          const char *url,
                          const char *method, const char *version,
                          const char *upload_data,
                          size_t *upload_data_size, void **con_cls)
{
	int i, j;                              // Counters
	const int buffer_len = 8192;           // String buffer length
	char buffer[buffer_len];               // String buffer

	int rc;                                // Return code
	sqlite3_stmt* statement;               // Sqlite prepared statement

	struct stringstream ss;                // Stringstream for output

	struct MHD_Response* response;         // HTTP response

	clock_t start, finish;                 // Structures for timing
	double time_taken;                     // Time taken for execution

	// Constants (which should probably be moved into a config file at some point in the future)
	const int max_highscore_users = 20;    // Number of users to show in the message count highscores
	const int max_extended_hs = 20;        // Number of extended highscores to show
	const int random_message_count = 10;   // Number of random messages wanted
	const int latest_topic_count = 3;

	// Begin transaction
	execute_sql("BEGIN TRANSACTION;");

	// Initialise start time
	start = clock();

	// Create stringstream
	ss = ss_create();

	// Write start of page
	ss_add(&ss, "<html><head><link rel=\"stylesheet\" href=\"http://www.renaporn.com/~rena/stats.css\">");
	ss_add(&ss, "<title>Renaporn stats for #talkhaus</title>");
	ss_add(&ss, "</head><body>");
	ss_add(&ss, "<h1>Renaporn stats for #talkhaus</h1>");
	ss_add(&ss, "<table><tr><td></td><td style=\"width: 110px\">Nickname</td><td style=\"width: 50px;\">Lines</td><td style=\"width: 90px;\">Last seen</td><td style=\"width: 500px;\">Random message</td></tr>");

	// Clear top users
	execute_sql(CLEAR_TOP_USERS_TABLE);

	// Prepare top users
	rc = sqlite3_prepare_v2(db, PREPARE_TOP_USERS_TABLE, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, PREPARE_TOP_USERS_TABLE);
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, max_highscore_users);

	// Run query
	rc = sqlite3_step(statement);

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, PREPARE_TOP_USERS_TABLE);
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);
	// Create statement
	rc = sqlite3_prepare_v2(db, SELECT_TOP_USERS_TABLE, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS_TABLE);
	}

	// Run query
	i = 0;
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		time_t lastseen;
		int messages;
		const char* nick;
		const char* message;

		const int timebuf_len = 1024;
		char timebuf[timebuf_len];

		// Increment row number
		i++;

		// Get values
		nick = (const char*)sqlite3_column_text(statement, 0);
		messages = sqlite3_column_int(statement, 1);
		message = (const char*)sqlite3_column_text(statement, 2);
		lastseen = (time_t)sqlite3_column_int(statement, 3);

		// Get time string
		convert_time_to_string(lastseen, timebuf, timebuf_len, "%d %b %Y");

		// Generate html
		snprintf(buffer, buffer_len, "<tr><td>%d</td><td>%s</td><td>%d</td><td>%s</td><td>%s</td></tr>", i, nick, messages, timebuf, message);

		// Write to page buffer
		ss_add(&ss, buffer);

		// Get next row
		rc = sqlite3_step(statement);
	}

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS_TABLE);
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);

	// End table
	ss_add(&ss, "</table><h3>Users who didn't quite make it</h3>");

	// Extended highscores table
	rc = sqlite3_prepare_v2(db, SELECT_TOP_USERS, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS);
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, max_extended_hs);
	sqlite3_bind_int(statement, 2, max_highscore_users);

	// Execute query
	rc = sqlite3_step(statement);

	// Write out in 5 column table
	ss_add(&ss, "<table>");
	for (i = 0; i < (int)(max_extended_hs / 5 + 0.5); ++i)
	{
		ss_add(&ss, "<tr>");
		for (j = 0; j < 5; ++j)
		{
			const int buffer_len = 2048;
			char buffer[buffer_len];

			const char* nick;
			int messages;

			// Blank string
			buffer[0] = 0;

			if (rc == SQLITE_ROW)
			{
				// Get nick and messages
				nick = (const char*)sqlite3_column_text(statement, 0);
				messages = sqlite3_column_int(statement, 1);

				// Write nick (messages) to buffer
				snprintf(buffer, buffer_len, "%s (%d)", nick, messages);

				// Fetch next row
				rc = sqlite3_step(statement);
			}

			// Write cell
			ss_add(&ss, "<td style=\"width: 150px;\">");
			ss_add(&ss, buffer);
			ss_add(&ss, "</td>");
		}
		ss_add(&ss, "</tr>");
	}

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS);
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);

	// Line break
	ss_add(&ss, "</table><br>");

	// Print out random_message_count random rows
	ss_add(&ss, "<h2>10 random messages from log</h2><table>");

	// Create prepared statement
	rc = sqlite3_prepare_v2(db, SELECT_RANDOM_MESSAGES, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_RANDOM_MESSAGES);
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, random_message_count);

	// Execute statement
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		const char* nick;
		const char* message;

		nick = (const char*)sqlite3_column_text(statement, 0);
		message = (const char*)sqlite3_column_text(statement, 1);

		snprintf(buffer, buffer_len, "<tr><td>&lt;%s&gt; %s</td></tr>", nick, message);

		ss_add(&ss, buffer);

		rc = sqlite3_step(statement);
	}

	// Check if query done
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_RANDOM_MESSAGES);
	}

	// Finalise statement
	sqlite3_finalize(statement);

	// End table
	ss_add(&ss, "</table>");

	// Show latest topics
	ss_add(&ss, "<h2>Latest topics</h2><table>");

	// Prepare statement
	rc = sqlite3_prepare_v2(db, SELECT_LATEST_TOPICS, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_LATEST_TOPICS);
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, latest_topic_count);

	// Iterate through results
	i = 0;
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		const int datebuf_len = 1024;
		char datebuf[datebuf_len];

		time_t time;
		const char* nick;
		const char* message;

		i++;

		// Get values
		time = (time_t)sqlite3_column_int(statement, 0);
		nick = (const char*)sqlite3_column_text(statement, 1);
		message = (const char*)sqlite3_column_text(statement, 2);

		// Convert time to string
		convert_time_to_string(time, datebuf, datebuf_len, "%d %b %Y %k:%M");

		// Format output
		snprintf(buffer, buffer_len, "<tr><td style=\"width: 450px;\">%s</td><td>Set by %s at %s</td></tr>", message, nick, datebuf);

		// Add to page buffer
		ss_add(&ss, buffer);

		// Get next row
		rc = sqlite3_step(statement);
	}

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_LATEST_TOPICS);
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);

	// End table
	ss_add(&ss, "</table>");

	// Get finish time
	finish = clock();
	time_taken = ((double)(finish - start))/CLOCKS_PER_SEC;

	// Generate footer
	snprintf(buffer, buffer_len, "<p>Row count: %d<br>Time taken to generate: %g seconds</p>", sqlite_messages, time_taken);

	// Write footer
	ss_add(&ss, "<br>");
	ss_add(&ss, buffer);
	ss_add(&ss, "</body></html>");

	execute_sql("END TRANSACTION;");

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
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, pos);
			return SQLITE_QUERY_FAILURE_ID;
		}

		// Execute statement
		rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
			fprintf(stderr, SQLITE_PROBLEM_QUERY, pos);
			return SQLITE_QUERY_FAILURE_ID;
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

int convert_time_to_string(time_t time, char* buffer, size_t buffer_len, const char* format)
{
	struct tm* time_struct;

	// Convert epoch to time struct
	time_struct = gmtime(&time);

	// Convert time to string
	return strftime(buffer, buffer_len, format, time_struct);
}

clock_t start, end;

void timer_start()
{
	start = clock();
}

void timer_end()
{
	double time_taken;

	end = clock();
	time_taken = ((double)(end - start))/(double)CLOCKS_PER_SEC;

	printf("Time taken: %f\n", time_taken);

}
