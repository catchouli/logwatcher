#ifndef __QUERIES_H__
#define __QUERIES_H__

#define TABLE_CREATION                   "CREATE TABLE messages(id INTEGER PRIMARY KEY, userid INTEGER, nick text collate nocase, message text, time DATE);" \
                                         "CREATE TABLE users(id INTEGER PRIMARY KEY, nick text collate nocase, messages int, lastseen DATE);" \
                                         "CREATE TABLE top_users(userid INTEGER PRIMARY KEY, nick text collate nocase, messages INTEGER, lastseen DATE);" \
                                         "CREATE INDEX messages_index ON messages (userid);" \
                                         "CREATE INDEX users_index ON users (messages);"
#define ADD_MESSAGE                      "INSERT INTO messages (userid, nick, message, time) SELECT messages, ?, ?, ? FROM users WHERE nick=?;"
#define INSERT_MESSAGE_COUNT             "INSERT INTO users (nick, messages, lastseen) VALUES (?, 1, ?)"
#define INCREMENT_MESSAGE_COUNT          "UPDATE users SET messages=messages+1, lastseen=? WHERE nick=?;"
#define SELECT_TOP_USERS                 "SELECT nick, messages, lastseen FROM users ORDER BY messages DESC LIMIT ? OFFSET ?;"
#define SELECT_RANDOM_MESSAGES           "SELECT time, nick, message FROM messages WHERE id IN (SELECT ABS(random() % (SELECT max(id) FROM messages)) FROM messages LIMIT ?);"
#define SELECT_RANDOM_MESSAGES_USER      "SELECT message FROM messages WHERE nick=$nick AND userid IN (SELECT ABS(random() % (SELECT max(userid) FROM messages WHERE nick=$nick)) FROM messages LIMIT ?);"
#define PREPARE_TOP_USERS_TABLE          "DELETE FROM top_users; INSERT INTO top_users SELECT abs(random() % users.messages), nick, messages, lastseen FROM users ORDER BY messages DESC LIMIT 25;"
#define SELECT_TOP_USERS_TABLE           "SELECT messages.nick, top_users.messages, messages.message, top_users.lastseen FROM top_users LEFT JOIN messages ON top_users.userid = messages.userid AND top_users.nick == messages.nick ORDER BY messages DESC;"

#endif /* __QUERIES_H__ */
