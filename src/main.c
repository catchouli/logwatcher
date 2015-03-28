// TODOs:
// Cleanup at exit and sigint to help with debugging
// Look into thread safety and performance benefits of enabling multiple threads in mhttpd

#define _XOPEN_SOURCE 700

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <microhttpd.h>
#include <sqlite3.h>
#include <libconfig.h>

#include "structures.h"
#include "errors.h"
#include "queries.h"
#include "stringstream.h"

#define CONFIG_FILE_DEFAULT "logwatcher.conf"

// Parse line of log
void parse_line(const char* line);

// Generate statistics page in response to http request
int generate_statistics(void *cls, struct MHD_Connection *connection,
                          const char *url,
                          const char *method, const char *version,
                          const char *upload_data,
                          size_t *upload_data_size, void **con_cls);

// Get top users with all data
// Returns the count of users actually retrieved if < requested
int stats_get_top_users_full(struct stats_user* users, int count);

// Get top users with message count and nick only
// Returns the count of messages actually retrieved if < requested
int stats_get_top_users_min(struct stats_user* users, int count, int offset);

// Get random messages from log
// Returns the count of messages actually retrieved if < requested
int stats_get_random_messages(struct stats_message* messages, int count);

// Get last topics from log
// Returns the count of topics actually retrieved if < requested
int stats_get_last_topics(struct stats_message* topics, int count);

// Execute multi statement SQL
int execute_sql(const char* sql);

// Convert unix time to string
int convert_time_to_string(time_t time, char* buffer, size_t buffer_len, const char* format);

// Timer thing
void timer_start();
void timer_end();

// Globals
sqlite3* db;                    // Sqlite database
char* sqlite_error = NULL;      // Sqlite error

int sqlite_messages = 0;        // Messages in message table

time_t current_day = 0;         // Current day (last encountered in log)
time_t latest_time_at_load = 0; // Latest time encountered
int messages_skipped = 0;       // Messages skipped at this time
int messages_to_skip = 0;       // Messages to skip at this time

// Configuration variables
const char* config_file = CONFIG_FILE_DEFAULT;
const char* database_filename;
const char* network;
const char* channel;
const char* logfile;

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
	size_t data_read;                // Amount of data read by getline
	char* line;                      // Pointer to getline buffer
	size_t size;                     // Size of getline buffer
	size_t old_position;             // Old position in buffer

	struct MHD_Daemon* daemon;       // microhttpd daemon
	int port = 0;                    // httpd port

	int rc;                          // Return code
	sqlite3_stmt* statement;         // Sqlite statement

	// Check args for config file
	if (argc >= 2)
	{
		config_file = argv[1];
	}

	// Print config filename
	printf("Using config file: %s\n", config_file);

	// Initialise config struct
	config_init(&config);

	// Read config file
	rc = config_read_file(&config, config_file);
	if (rc != CONFIG_TRUE)
	{
		fprintf(stderr, CONFIG_LOAD_FAILURE, config_error_text(&config), config_error_line(&config));
		return CONFIG_LOAD_FAILURE_ID;
	}

	// Load database filename
	setting = config_lookup(&config, "logwatcher.database_filename");
	database_filename = config_setting_get_string(setting);

	// Load network name
	setting = config_lookup(&config, "logwatcher.network");
	network = config_setting_get_string(setting);

	// Load channel name
	setting = config_lookup(&config, "logwatcher.channel");
	channel = config_setting_get_string(setting);

	// Load logfile name
	setting = config_lookup(&config, "logwatcher.logfile");
	logfile = config_setting_get_string(setting);

	// Load port
	setting = config_lookup(&config, "logwatcher.port");
	port = config_setting_get_int(setting);

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
	inotify_wd = inotify_add_watch(inotify_fd, logfile, IN_MODIFY);

	if (inotify_wd < 0)
	{
		fprintf(stderr, INOTIFY_WATCH_FAILURE);
		return INOTIFY_WATCH_FAILURE_ID;
	}

	// Get logfile length
	logfile_fd = fopen(logfile, "r");
	//fseek(logfile_fd, 0, SEEK_END);
	logfile_len = ftell(logfile_fd);

	// Read logfile
	printf("Reading logfile...\n");
	//fseek(logfile_fd, 0, SEEK_SET);

	// Initialise httpd
	printf("Initialising httpd...\n");
	daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
					&generate_statistics, NULL,
					MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)16,
					MHD_OPTION_END);
	if (daemon == NULL)
	{
		fprintf(stderr, MHD_INIT_FAILURE);
		return MHD_INIT_FAILURE_ID;
	}

	// Enable sqlite serialized threads mode
	rc = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
	if (rc == SQLITE_ERROR)
	{
		fprintf(stderr, "Failed to set sqlite to serialized threads mode, is "
				"sqlite compiled without thread support?");
		return -1;
	}

	// Create or open sqlite database
	printf("Opening database...\n");
	rc = sqlite3_open(database_filename, &db);
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
	printf("Creating database tables if non existent...\n");
	rc = execute_sql(TABLE_CREATION);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "Failed to initialise database, terminating.\n");
		return -1;
	}

	// Get latest message time from database
	latest_time_at_load = 0;

	// Prepare statement
	rc = sqlite3_prepare_v2(db, SELECT_LATEST_MESSAGES, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		return SQLITE_STATEMENT_PREPERATION_FAILURE_ID;
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, 1);       // Number of messages to retrieve (1)

	// Run statement
	rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW)
	{
		latest_time_at_load = (time_t)sqlite3_column_int(statement, 0);

		printf("Got latest time from database: %d\n", (int)latest_time_at_load);

		rc = sqlite3_finalize(statement);

		// Get number of messages to skip at this time
		// (so we don't double up messages at the same time)
		rc = sqlite3_prepare_v2(db, SELECT_MESSAGE_COUNT_AT_TIME, -1, &statement, NULL);
		if (rc != SQLITE_OK)
		{
			fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
			return SQLITE_STATEMENT_PREPERATION_FAILURE_ID;
		}

		// Bind parameters
		sqlite3_bind_int(statement, 1, latest_time_at_load);

		// Run statement
		rc = sqlite3_step(statement);
		if (rc == SQLITE_ROW)
		{
			messages_to_skip = sqlite3_column_int(statement, 0);

			rc = sqlite3_finalize(statement);
		}
	}
	else
	{
		printf("Creating new db\n");
		sqlite3_finalize(statement);
	}

        // Get total message count
	sqlite_messages = 0;

        // Prepare statement
        rc = sqlite3_prepare_v2(db, SELECT_MESSAGE_COUNT, -1, &statement, NULL);
        if (rc != SQLITE_OK)
        {
                fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
                return SQLITE_STATEMENT_PREPERATION_FAILURE_ID;
        }

        // Run statement
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW)
        {
                sqlite_messages = sqlite3_column_int(statement, 0);

                printf("Got message count: %d\n", sqlite_messages);

                rc = sqlite3_step(statement);
        }
        else
        {
                printf("Creating new db");
        }

        // Clean up statement
        sqlite3_finalize(statement);

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

		printf("Loading aliases...\n");
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

	if (latest_time_at_load > 0)
	{
		printf("Skipping to new messages...\n");
	}

	// Get starting position in buffer
	old_position = ftell(logfile_fd);

	// Read lines
	line = NULL;
	while ((data_read = getline(&line, &size, logfile_fd)) != -1)
	{
		if (line != NULL)
		{
			// If last character isn't a linebreak,
			// Break and let monitoring loop get it instead
			// (this is the last line of the file and is not yet complete)
			if (line[data_read-1] != '\n')
			{
				fseek(logfile_fd, old_position, SEEK_SET);
				break;
			}

			// Update position in buffer
			old_position = ftell(logfile_fd);

			// Parse line
			parse_line(line);

			// Free memory allocated by getline
			free(line);
			line = NULL;
		}
	}
	logfile_len = ftell(logfile_fd);
	printf("Finished parsing logfile.\n");

	// Wait for changes
	printf("Waiting for new messages...\n");
	while (read(inotify_fd, &event, sizeof(struct inotify_event)))
	{
		// Get current position
		old_position = ftell(logfile_fd);

		// Read new text
		//fread(new_text, new_text_len, 1, logfile_fd);
	        line = NULL;
	        while ((data_read = getline(&line, &size, logfile_fd)) != -1)
	        {
	                if (line != NULL)
	                {
				// If last character isn't a linebreak,
				// Seek back and continue monitoring file
				// (this is the end of the file and this line is not yet complete)
				if (line[data_read-1] != '\n')
				{
					fseek(logfile_fd, old_position, SEEK_SET);
					break;
				}

				// Update old position
				old_position = ftell(logfile_fd);

	                        // Parse line
	                        parse_line(line);

	                        // Free memory allocated by getline
	                        free(line);
	                        line = NULL;
	                }
	        }
	}

	return 0;
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
		snprintf(buffer, buffer_len, "%d %s %d 00:00:00", date, month, year);

		// Parse date string %b
		if (strptime(buffer, "%d %b %Y %H:%M:%S", &time_struct) == NULL)
		{
			fprintf(stderr, "Failed to parse date format %s\n", buffer);

			return;
		}

		// Convert day to unix time
		current_day = mktime(&time_struct);

		goto parse_line_cleanup;
	}

	// Parse topic
	rc = sscanf(line, "%d:%d -!- %s changed the topic of %*s to: %[^\n]", &hour, &minute, nick, message);

	if (rc == 4)
	{
		time_t time;
		sqlite3_stmt* statement;

		// Work out time
		time = current_day + hour * 3600 + minute * 60;

		// Skip if from the past
		if (time > latest_time_at_load)
		{
			// Add topic to database
			rc = sqlite3_prepare_v2(db, INSERT_TOPIC, -1, &statement, NULL);
			if (rc != SQLITE_OK)
			{
				fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
				fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_TOPIC);
				goto parse_line_cleanup;
			}

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
		}

		goto parse_line_cleanup;
	}

	// Parse message string
	rc = sscanf(line, "%d:%d <%[^>]> %[^\n]", &hour, &minute, nick, message);

	if (rc == 4)
	{
		sqlite3_stmt* statement;

		// Remove first character from nick (op char)
		char* nick_only = nick+1;

		// Calculate time
		time = current_day + hour * 3600 + minute * 60;

		if (time >= latest_time_at_load)
		{
			// Skip messages already in the db at load
			if (time == latest_time_at_load && messages_skipped < messages_to_skip)
			{
				messages_skipped++;

				goto parse_line_cleanup;
			}

			// Add message to database
			rc = sqlite3_prepare_v2(db, INSERT_MESSAGE, -1, &statement, NULL);
			if (rc != SQLITE_OK)
			{
				fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
				fprintf(stderr, SQLITE_PROBLEM_QUERY, INSERT_MESSAGE);
				goto parse_line_cleanup;
			}

			// Bind values
			sqlite3_bind_text(statement, 1, nick_only, -1, SQLITE_STATIC);
			sqlite3_bind_text(statement, 2, message, -1, SQLITE_STATIC);
			sqlite3_bind_int(statement, 3, time);

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
				goto parse_line_cleanup;
			}

			// Bind values
			sqlite3_bind_int(statement, 1, time);
			sqlite3_bind_text(statement, 2, nick_only, -1, SQLITE_STATIC);

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
					goto parse_line_cleanup;
				}

				// Bind values
				sqlite3_bind_text(statement, 1, nick_only, -1, SQLITE_STATIC);
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
		}

		goto parse_line_cleanup;
	}

parse_line_cleanup:
	free(nick);
	free(message);
}

int generate_statistics(void* cls, struct MHD_Connection* connection,
                          const char* url,
                          const char* method, const char* version,
                          const char* upload_data,
                          size_t* upload_data_size, void** con_cls)
{
	// Constants (which should probably be moved into a config file at some point in the future)
	const int max_highscore_users = 20;    // Number of users to show in the message count highscores
	const int max_extended_hs = 20;        // Number of extended highscores to show
	const int random_message_count = 10;   // Number of random messages wanted
	const int latest_topic_count = 3;      // Number of latest topics to show

	int i, j;                              // Counters
	const int buffer_len = 8192;           // String buffer length
	char buffer[buffer_len];               // String buffer

	const int timebuf_len = 1024;          // Time string buffer length
	char timebuf[timebuf_len];             // Time string buffer

	int rc;                                // Return code

	struct stringstream ss;                // Stringstream for output

	struct MHD_Response* response;         // HTTP response

	struct timespec start, finish;         // Time structures for measuring execution time
	float time_taken;                      // Time taken in ms

	const char* default_mode = "html";     // The default mode for the page
	const char* mode = default_mode;       // The mode from GET("mode") or default_mode if unavailable

	struct stats_user* users = NULL;        // Users array for stats_* calls
	struct stats_message* messages = NULL;  // Messages array for stats* calls

	// Allocate memory for arrays
	users = malloc(sizeof(struct stats_user) * STATS_MAX(max_highscore_users, max_extended_hs));
	messages = malloc(sizeof(struct stats_message) * STATS_MAX(random_message_count, latest_topic_count));

	// Create stringstream
	ss = ss_create();

	// Initialise start time
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Get page mode
	mode = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "mode");

	// Set default mode if GET("mode") is null
	if (mode == NULL)
		mode = default_mode;

	if (strcmp(mode, "html") == 0)
	{
		// Write start of page
		ss_add(&ss, "<html><head><link rel=\"stylesheet\" href=\"http://www.renaporn.com/~rena/stats.css\">");

		// Format channel name and network for title
		snprintf(buffer, buffer_len, "<title>Stats for %s at %s</title>", channel, network);
		ss_add(&ss, buffer);

		// Format channel name and network for page
		snprintf(buffer, buffer_len, "<h1>Stats for %s at %s</h1>", channel, network);
		ss_add(&ss, "</head><body>");
		ss_add(&ss, buffer);
		ss_add(&ss, "<table><tr><td></td><td style=\"width: 110px\">Nickname</td><td style=\"width: 50px;\">Lines</td><td style=\"width: 90px;\">Last seen</td><td style=\"width: 500px;\">Random message</td></tr>");

		// Get top users
		rc = stats_get_top_users_full(users, max_highscore_users);

		// Iterate through top users
		for (i = 0; i < rc; ++i)
		{
			// Get time string
			convert_time_to_string(users[i].lastseen, timebuf, timebuf_len, "%d %b %Y");

			// Generate html
			snprintf(buffer, buffer_len, "<tr><td>%d</td><td>%s</td><td>%d</td><td>%s</td><td>%s</td></tr>", i+1, users[i].nick, users[i].lines, timebuf, users[i].message);

			// Write to page buffer
			ss_add(&ss, buffer);
		}

		// End table
		ss_add(&ss, "</table><h3>Users who didn't quite make it</h3>");

		// Get extended highscore users
		rc = stats_get_top_users_min(users, max_extended_hs, max_highscore_users);

		// Generate html
		ss_add(&ss, "<table>");
		for (i = 0; i < (int)(rc / 5 + 0.5); ++i)
		{
			ss_add(&ss, "<tr>");
			for (j = 0; j < 5; ++j)
			{
				snprintf(buffer, buffer_len, "<td style=\"width: 150px;\">%s (%d)</td>", users[i*5 + j].nick, users[i*5 + j].lines);
				ss_add(&ss, buffer);
			}
			ss_add(&ss, "</tr>");
		}

		// Line break
		ss_add(&ss, "</table><br>");

		// Get random_message_count random rows
		rc = stats_get_random_messages(messages, random_message_count);

		// Generate HTML
		ss_add(&ss, "<h2>10 random messages from log</h2><table>");

		for (i = 0; i < rc; ++i)
		{
			// Format HTML
			snprintf(buffer, buffer_len, "<tr><td>&lt;%s&gt; %s</td></tr>", messages[i].nick, messages[i].message);

			// Add to page
			ss_add(&ss, buffer);
		}

		// End table
		ss_add(&ss, "</table>");

		// Get latest topics
		rc = stats_get_last_topics(messages, latest_topic_count);

		// Generate HTML
		ss_add(&ss, "<h2>Latest topics</h2><table>");

		for (i = 0; i < rc; ++i)
		{
			// Get time string
			convert_time_to_string(messages[i].time, timebuf, timebuf_len, "%d %b %Y");

			// Format output
			snprintf(buffer, buffer_len, "<tr><td style=\"width: 450px;\">%s</td><td>Set by %s at %s</td></tr>", messages[i].message, messages[i].nick, timebuf);

			// Add to page
			ss_add(&ss, buffer);
		}

		// End table
		ss_add(&ss, "</table>");

		// Get finish time in ms
		clock_gettime(CLOCK_MONOTONIC, &finish);
		time_taken = (finish.tv_sec - start.tv_sec) * 1000.0f +
				 (finish.tv_nsec - start.tv_nsec) / 1000000.0f;

		// Generate footer
		snprintf(buffer, buffer_len, "<p>Total messages: %d<br>Mode: %s<br>Time taken to generate: %gms</p>", sqlite_messages, mode, time_taken);

		// Write footer
		ss_add(&ss, "<br>");
		ss_add(&ss, buffer);
		ss_add(&ss, "</body></html>");
	}
	else if (strcmp(mode, "json") == 0)
	{
		// Get top users
		rc = stats_get_top_users_full(users, max_highscore_users);

		// Top of json
		ss_add(&ss, "{ \"users\": [");

		// Iterate through them and write json
		for (i = 0; i < rc; ++i)
		{
			const char* top_user_format = "{ \"id\": %d, \"nick\": \"%s\", \"lines\": %d, \"message\": \"%s\" }";

			snprintf(buffer, buffer_len, top_user_format, i+1, users[i].nick, users[i].lines, users[i].message);

			if (i > 0)
				ss_add(&ss, ",");

			ss_add(&ss, buffer);
		}

		// Bottom of json
		ss_add(&ss, "] }");
	}

	// Create response
	response = MHD_create_response_from_buffer(strlen(ss.buffer), (void*)ss.buffer, MHD_RESPMEM_PERSISTENT);

	MHD_add_response_header(response, "Access-Control-Allow-Origin", "http://www.renaporn.com");

	rc = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	// Clean up memory
	ss_destroy(&ss);
	free(users);
	free(messages);

	return rc;
}

int stats_get_top_users_full(struct stats_user* users, int count)
{
	int i;                          // Counter
	int rc;                         // Return code
	sqlite3_stmt* statement;        // Sqlite statement

	// Clear top users
	execute_sql(CLEAR_TOP_USERS_TABLE);

	// Prepare top users
	rc = sqlite3_prepare_v2(db, PREPARE_TOP_USERS_TABLE, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, PREPARE_TOP_USERS_TABLE);

		return 0;
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, count);

	// Run query
	rc = sqlite3_step(statement);

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, PREPARE_TOP_USERS_TABLE);

		return 0;
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);

	// Select generated table
	rc = sqlite3_prepare_v2(db, SELECT_TOP_USERS_TABLE, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS_TABLE);

		return 0;
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

		// Get values
		nick = (const char*)sqlite3_column_text(statement, 0);
		messages = sqlite3_column_int(statement, 1);
		message = (const char*)sqlite3_column_text(statement, 2);
		lastseen = (time_t)sqlite3_column_int(statement, 3);

		// If a blank string is in the db sqlite will return NULL
		if (nick == NULL)
			nick = "";
		if (message == NULL)
			message = "";

		// Add to array
		strncpy(users[i].nick, nick, STATS_NICK_LEN);
		strncpy(users[i].message, message, STATS_MESSAGE_LEN);
		users[i].lines = messages;
		users[i].lastseen = lastseen;

		// Get next row
		rc = sqlite3_step(statement);

		// Increment row number
		i++;
	}

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, "test\n");
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS_TABLE);
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);

	return i;
}

int stats_get_top_users_min(struct stats_user* users, int count, int offset)
{
	int i;                          // Counter
	int rc;                         // Return code
	sqlite3_stmt* statement;        // Sqlite statement

	// Select top users
	rc = sqlite3_prepare_v2(db, SELECT_TOP_USERS, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS_TABLE);

		return 0;
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, count);
	sqlite3_bind_int(statement, 2, offset);

	// Run query
	i = 0;
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		time_t lastseen;
		int messages;
		const char* nick;
		const char* message;

		// Get values
		nick = (const char*)sqlite3_column_text(statement, 0);
		messages = sqlite3_column_int(statement, 1);
		message = (const char*)sqlite3_column_text(statement, 2);
		lastseen = (time_t)sqlite3_column_int(statement, 3);

		// If a blank string is in the db sqlite will return NULL
		if (nick == NULL)
			nick = "";
		if (message == NULL)
			message = "";

		// Add to array
		strncpy(users[i].nick, nick, STATS_NICK_LEN);
		strncpy(users[i].message, message, STATS_MESSAGE_LEN);
		users[i].lines = messages;
		users[i].lastseen = lastseen;

		// Get next row
		rc = sqlite3_step(statement);

		// Increment row number
		i++;
	}

	// Make sure query completed successfully
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_TOP_USERS_TABLE);
	}

	// Finalise statement
	rc = sqlite3_finalize(statement);

	return i;
}

int stats_get_random_messages(struct stats_message* messages, int count)
{
	int i;                          // Counter
	int rc;                         // Return code
	sqlite3_stmt* statement;        // Sqlite statement

	// Create prepared statement
	rc = sqlite3_prepare_v2(db, SELECT_RANDOM_MESSAGES, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_RANDOM_MESSAGES);

		return 0;
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, count);

	// Execute statement
	i = 0;
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		const char* nick;
		const char* message;

		nick = (const char*)sqlite3_column_text(statement, 0);
		message = (const char*)sqlite3_column_text(statement, 1);

		// If a blank string is in the db sqlite will return NULL
		if (nick == NULL)
			nick = "";
		if (message == NULL)
			message = "";

		strncpy(messages[i].nick, nick, STATS_NICK_LEN);
		strncpy(messages[i].message, message, STATS_MESSAGE_LEN);

		i++;
		rc = sqlite3_step(statement);
	}

	// Check if query done
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_RANDOM_MESSAGES);

		return 0;
	}

	// Finalise statement
	sqlite3_finalize(statement);

	return i;

}

int stats_get_last_topics(struct stats_message* topics, int count)
{
	int i;                          // Counter
	int rc;                         // Return code
	sqlite3_stmt* statement;        // Sqlite statement

	// Create prepared statement
	rc = sqlite3_prepare_v2(db, SELECT_LATEST_TOPICS, -1, &statement, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, SQLITE_STATEMENT_PREPERATION_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_RANDOM_MESSAGES);

		return 0;
	}

	// Bind parameters
	sqlite3_bind_int(statement, 1, count);

	// Execute statement
	i = 0;
	rc = sqlite3_step(statement);
	while (rc == SQLITE_ROW)
	{
		time_t time;
		const char* nick;
		const char* message;

		time = (time_t)sqlite3_column_int(statement, 0);
		nick = (const char*)sqlite3_column_text(statement, 1);
		message = (const char*)sqlite3_column_text(statement, 2);

		// If a blank string is in the db sqlite will return NULL
		if (nick == NULL)
			nick = "";
		if (message == NULL)
			message = "";

		topics[i].time = time;
		strncpy(topics[i].nick, nick, STATS_NICK_LEN);
		strncpy(topics[i].message, message, STATS_MESSAGE_LEN);

		i++;
		rc = sqlite3_step(statement);
	}

	// Check if query done
	if (rc != SQLITE_DONE)
	{
		fprintf(stderr, SQLITE_QUERY_FAILURE, sqlite3_errmsg(db));
		fprintf(stderr, SQLITE_PROBLEM_QUERY, SELECT_RANDOM_MESSAGES);

		return 0;
	}

	// Finalise statement
	sqlite3_finalize(statement);

	return i;
}

int execute_sql(const char* sql)
{
	int rc;
	char* sqlite_error = NULL;

	// Execute SQL
	rc = sqlite3_exec(db, sql, NULL, NULL, &sqlite_error);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "sqlite3_exec error: %s\n", sqlite_error);
	}

	// Free memory
	sqlite3_free(sqlite_error);

	return rc;
}

int convert_time_to_string(time_t time, char* buffer, size_t buffer_len, const char* format)
{
	struct tm* time_struct;

	// Convert epoch to time struct
	time_struct = gmtime(&time);

	// Convert time to string
	return strftime(buffer, buffer_len, format, time_struct);
}
