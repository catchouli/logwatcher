#ifndef __QUERIES_H__
#define __QUERIES_H__

#define TABLE_CREATION                   "CREATE TABLE messages(id INTEGER PRIMARY KEY, userid INTEGER, nick text collate nocase, message text, time DATE);" \
                                         "CREATE TABLE users(id INTEGER PRIMARY KEY, nick text collate nocase, messages int, lastseen DATE);" \
                                         "CREATE TABLE topics(id INTEGER PRIMARY KEY, time DATE, nick text, topic text);" \
                                         "CREATE TABLE aliases(id INTEGER PRIMARY KEY, nick text collate nocase, alias text);" \
                                         "CREATE TEMPORARY TABLE top_users(id INTEGER PRIMARY KEY, userid INTEGER, nick text collate nocase, messages INTEGER, lastseen DATE);" \
                                         "CREATE INDEX messages_index ON messages (userid);" \
                                         "CREATE INDEX users_index ON users (messages);" \
                                         "CREATE INDEX aliases_index ON aliases (alias);"
#define INSERT_MESSAGE                   "INSERT INTO messages (userid, nick, message, time) " \
                                         "SELECT (SELECT messages FROM users WHERE nick=(IFNULL((SELECT alias FROM aliases WHERE nick=$nick), $nick))), " \
                                         "(IFNULL((SELECT alias FROM aliases WHERE nick=$nick), $nick)), $message, #time;"
#define INSERT_TOPIC                     "INSERT INTO topics (time, nick, topic) SELECT #time, IFNULL((SELECT alias FROM aliases WHERE nick=$nick), $nick), $message;"
#define INSERT_MESSAGE_COUNT             "INSERT INTO users (nick, messages, lastseen) SELECT IFNULL((SELECT alias FROM aliases WHERE nick=$nick), $nick), 1, #lastseen;"
#define INCREMENT_MESSAGE_COUNT          "UPDATE users SET messages=messages+1, lastseen=#lastseen WHERE nick=IFNULL((SELECT alias FROM aliases WHERE nick=$nick), $nick);"
#define INSERT_ALIAS                     "INSERT INTO aliases (nick, alias) VALUES ($nick, $alias);"
#define SELECT_TOP_USERS                 "SELECT nick, messages, lastseen FROM users ORDER BY messages DESC LIMIT ? OFFSET ?;"
#define SELECT_RANDOM_MESSAGES           "SELECT nick, message FROM messages WHERE id IN (SELECT ABS(random() % (SELECT max(id) FROM messages)) FROM messages LIMIT ?);"
#define SELECT_RANDOM_MESSAGES_USER      "SELECT message FROM messages WHERE nick=$nick AND userid IN (SELECT ABS(random() % (SELECT max(userid) FROM messages WHERE nick=$nick)) FROM messages LIMIT ?);"
#define UPDATE_NEW_ALIASES               "UPDATE messages SET nick=(SELECT nick FROM aliases WHERE alias=messages.nick);"
#define CLEAR_TOP_USERS_TABLE            "DELETE FROM top_users;"
#define PREPARE_TOP_USERS_TABLE          "INSERT INTO top_users (userid, nick, messages, lastseen) SELECT abs(random() % users.messages), nick, messages, lastseen FROM users ORDER BY messages DESC LIMIT ?;"
#define SELECT_TOP_USERS_TABLE           "SELECT top_users.nick, top_users.messages, messages.message, top_users.lastseen FROM top_users LEFT JOIN messages ON top_users.userid = messages.userid AND top_users.nick == messages.nick ORDER BY messages DESC;"
#define SELECT_LATEST_TOPICS             "SELECT time, nick, topic FROM topics ORDER BY time DESC LIMIT ?;"

#endif /* __QUERIES_H__ */
