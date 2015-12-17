#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include "lock.h"
#include "log.h"

#define LEVEL_NAME_LEN	8
#define LOG_BUF_LEN		4096
#define PATH_MAX        256

struct taglogger {
    char name[PATH_MAX];
    int  level;
    FILE *fp;
	unsigned long long wcurr;//当前写的位置
	unsigned long long wtotal;//总共写入数
	unsigned long long rotatesize;//日志切割大小
    LockerObj *lockerobj;
};

inline static void setlevel(Logger *logger,int level)
{
	if (NULL == logger)
		return;

	logger->level = level;
}

inline static char* getLevelName(int level){

   	switch(level){
		case LEVEL_NONE:
			return "[NONE] ";
		case LEVEL_ERROR:
			return "[ERROR] ";
		case LEVEL_WARN:
			return "[WARN] ";
		case LEVEL_INFO:
			return "[INFO] ";
		case LEVEL_DEBUG:
			return "[DEBUG] ";
	}
	return "";
}

static void rotate(Logger *logger)
{
	char newpath[PATH_MAX];
	time_t time;
	struct timeval tv;
	struct tm *tm;

	if (NULL == logger)
		return;

	fclose(logger->fp);
	logger->fp = NULL;
	gettimeofday(&tv, NULL);

	time = tv.tv_sec;
	tm   = localtime(&time);

	sprintf(newpath, "%s.%04d%02d%02d-%02d%02d%02d",
		logger->name,
		tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);

	int ret = rename(logger->name,newpath);
	if(ret == -1){
		return;
	}

	logger->fp = fopen(logger->name, "a");

	if(logger->fp == NULL){
		return;
	}
	logger->wcurr = 0;
}

static int logv(Logger *logger, const char *fmt, va_list ap)
 {
	if (NULL == logger)
		return -1;
	char buf[LOG_BUF_LEN];
	int len;
	char *ptr = buf;
	time_t time;
	struct timeval tv;
	struct tm *tm;
	int space = 0;
	gettimeofday(&tv, NULL);
	time = tv.tv_sec;
	tm = localtime(&time);
	/* %3ld 在数值位数超过3位的时候不起作用, 所以这里转成int */
	len = sprintf(ptr, "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
			tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
			tm->tm_min, tm->tm_sec, (int) (tv.tv_usec / 1000));
	if (len < 0) {
		return -1;
	}
	ptr += len;

	memcpy(ptr, getLevelName(logger->level), LEVEL_NAME_LEN);
	ptr += LEVEL_NAME_LEN;

	space = sizeof(buf) - (ptr - buf) - 10;
	len = vsnprintf(ptr, space, fmt, ap);
	if (len < 0) {
		return -1;
	}
	ptr += len > space ? space : len;
	*ptr++ = '\n';
	*ptr = '\0';

	len = ptr - buf;

	logger->lockerobj->Lock(logger->lockerobj->locker);

    fwrite(buf, len, 1, logger->fp);
	fflush(logger->fp);

	logger->wcurr  += len;
	logger->wtotal += len;

	if (logger->rotatesize > 0 && logger->wcurr > logger->rotatesize) {
		rotate(logger);
	}
	logger->lockerobj->Unlock(logger->lockerobj->locker);
	return 0;
}

static void logclose(Logger *logger)
{
	if (NULL != logger) {
		if (logger->fp != stdin && logger->fp != stdout) {
			fclose(logger->fp);
		}
	}
}

Logger *Logger_Create(int level,int rotate_size,char *name)
{
	Logger *logger = NULL;

	logger = (Logger *)malloc(sizeof(Logger));

	if (NULL != logger) {

		logger->fp          = NULL;

		bzero(logger->name,sizeof(logger->name));
		memcpy(logger->name,name,strlen(name));

		logger->level       = level;
		logger->wcurr      = 0;
		logger->wtotal     = 0;
		logger->rotatesize = rotate_size * 1024 * 1024;//日志切割默认都是MB;

		logger->lockerobj = LockerObj_Create();

		if(strcmp(name, "stdout") == 0){
			logger->fp = stdout;
		}else if(strcmp(name, "stderr") == 0){
			logger->fp = stderr;
		} else {
			logger->fp = fopen(name, "a");
			if(logger->fp == NULL){
			   printf("open log file faile:file:%s,err:%s\n",name,strerror(errno));
			   return NULL;
		    }
		}
	}

	return logger;
}

void Logger_Destory(Logger *logger)
{
   if (logger != NULL){
	   Locker_Free(logger->lockerobj->locker);
	   Locker_Clear(logger->lockerobj->locker);
	   logger->lockerobj->locker = NULL;
	   logclose(logger);
	   free(logger);
	   logger = NULL;
   }
}

int logerror(Logger *logger,int level,const char *fmt, ...)
{
	int ret = 0;
	if (NULL == logger)
		return -1;

	va_list ap;
	va_start(ap, fmt);
	ret = logv(logger, fmt, ap);
	va_end(ap);
	return ret;
}

int logdebug(Logger *logger,int level,const char *fmt, ...){

	int ret = 0;
	if (NULL == logger)
	   return -1;

	setlevel(logger,level);
	va_list ap;
	va_start(ap, fmt);
	ret = logv(logger, fmt, ap);
	va_end(ap);

	return ret;
}

int logwarn(Logger *logger,int level,const char *fmt, ...)
{
	int ret = 0;
	if (NULL == logger)
	   return -1;

	setlevel(logger,level);
	va_list ap;
	va_start(ap, fmt);
	ret = logv(logger, fmt, ap);
	va_end(ap);
	return ret;
}

int loginfo(Logger *logger,int level,const char *fmt, ...)
{
	int ret = 0;
	if (NULL == logger)
		return -1;

	setlevel(logger,level);
	va_list ap;
	va_start(ap, fmt);
	ret = logv(logger, fmt, ap);
	va_end(ap);

	return ret;
}
