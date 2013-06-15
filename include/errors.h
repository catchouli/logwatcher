#ifndef __ERRORS_H__
#define __ERRORS_H__

#define INOTIFY_INIT_FAILURE	                "Failed to initialise inotify\n"
#define INOTIFY_INIT_FAILURE_ID                 1

#define INOTIFY_WATCH_FAILURE	                "Failed to add watch for logfile\n"
#define INOTIFY_WATCH_FAILURE_ID                2

#define MHD_INIT_FAILURE                        "Failed to initialise httpd\n"
#define MHD_INIT_FAILURE_ID                     3

#define SQLITE_DATABASE_CREATION_FAILURE        "Failed to create database: %s\n"
#define SQLITE_DATABASE_CREATION_FAILURE_ID     4

#define SQLITE_TABLE_CREATION_FAILURE           "Failed to create table: %s\n"
#define SQLITE_TABLE_CREATION_FAILURE_ID        5

#define SQLITE_QUERY_FAILURE                    "Sqlite query failed: %s\n"
#define SQLITE_QUERY_FAILURE_ID                 6

#define SQLITE_STATEMENT_PREPERATION_FAILURE    "Failed to create prepared statement: %s\n"
#define SQLITE_STATEMENT_PREPERATION_FAILURE_ID 7

#define SQLITE_EXTENSION_LOADING_FAILURE        "Failed to load extensions: %s\n"
#define SQLITE_EXTENSION_LOADING_FAILURE_ID     8

#define SQLITE_PROBLEM_QUERY                    "Problem query: %s\n"
#define SQLITE_PROBLEM_QUERY_ID                 9

#define CONFIG_LOAD_FAILURE                     "Failed to load config file: %s at line %d\n"
#define CONFIG_LOAD_FAILURE_ID                  10

#endif /* __ERRORS_H__ */
