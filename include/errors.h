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

#endif /* __ERRORS_H__ */
