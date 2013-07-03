#ifndef __STRUCTURES_H__
#define __STRUCTURES_H__

#define STATS_MAX(x, y)    (x > y ? x : y)

#define STATS_NICK_LEN     32
#define STATS_MESSAGE_LEN  1024

struct stats_user
{
	char nick[STATS_NICK_LEN];
	char message[STATS_MESSAGE_LEN];
	int lines;
	time_t lastseen;
};

struct stats_message
{
	time_t time;
	char nick[STATS_NICK_LEN];
	char message[STATS_MESSAGE_LEN];
};


#endif /* __STRUCTURES_H__ */

