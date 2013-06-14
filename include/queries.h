#ifndef __QUERIES_H__
#define __QUERIES_H__

#define TABLE_CREATION                   "CREATE TABLE messages(id INTEGER PRIMARY KEY, userid INTEGER, nick text collate nocase, message text, time DATE);" \
                                         "CREATE TABLE users(id INTEGER PRIMARY KEY, nick text collate nocase, messages int, lastseen DATE);" \
                                         "CREATE INDEX messages_index ON messages (userid);" \
                                         "CREATE INDEX users_index ON users (messages);"
#define ADD_MESSAGE                      "INSERT INTO messages (userid, nick, message, time) SELECT messages, ?, ?, ? FROM users WHERE nick=?;"
#define SELECT_MESSAGE_AT_POSITION       "SELECT time, nick, message FROM messages LIMIT 1 OFFSET ?;"
#define SELECT_USER_MESSAGE_AT_POSITION  "SELECT message FROM messages WHERE nick=? LIMIT 1 OFFSET ?;"
#define INSERT_MESSAGE_COUNT             "INSERT INTO users (nick, messages, lastseen) VALUES (?, 1, ?)"
#define INCREMENT_MESSAGE_COUNT          "UPDATE users SET messages=messages+1, lastseen=? WHERE nick=?;"
#define SELECT_TOP_USERS                 "SELECT nick, messages, lastseen FROM users ORDER BY messages DESC LIMIT ?;"
#define SELECT_TOP_USERS_EXT             "SELECT nick, messages FROM users ORDER BY messages DESC LIMIT ? OFFSET ?;"
#define SELECT_RANDOM_MESSAGES           "SELECT time, nick, message FROM messages WHERE id IN (SELECT ABS(random() % (SELECT max(id) FROM messages)) FROM messages LIMIT ?);"

#endif /* __QUERIES_H__ */

