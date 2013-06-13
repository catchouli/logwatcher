#ifndef __QUERIES_H__
#define __QUERIES_H__

#define TABLE_CREATION                   "CREATE TABLE messages(id INTEGER PRIMARY KEY, nick text, message text, time DATE);" \
                                         "CREATE TABLE users(id INTEGER PRIMARY KEY, nick text, messages int);"
#define ADD_MESSAGE                      "INSERT INTO messages (nick, message) VALUES (?, ?);"
#define SELECT_MESSAGE_AT_POSITION       "SELECT nick,message FROM messages LIMIT 1 OFFSET ?;"
#define INCREMENT_MESSAGE_COUNT          "UPDATE users SET messages=messages+1 WHERE nick=?;"
#define SELECT_TOP_USERS                 "SELECT nick,messages FROM users;"

#endif /* __QUERIES_H__ */

