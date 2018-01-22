/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Configuration file parser/reader. Place into the dynamic
 *              data structure representation the conf file representing
 *              the loadbalanced server pool.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <glob.h>
#include <unistd.h>
#include <libgen.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdbool.h>
#include <linux/version.h>
#include <pwd.h>
#include <ctype.h>

#include "parser.h"
#include "memory.h"
#include "logger.h"
#include "rttables.h"
#include "scheduler.h"
#include "list.h"

#define DUMP_KEYWORDS	0

typedef struct _defs {
	char *name;//变量名
	size_t name_len;//变量名称长度
	char *value;
	size_t value_len;
	bool multiline;
} def_t;

#define DEF_LINE_END	'\n'

/* global vars */
vector_t *keywords;//存放全局的关键字
bool reload = 0;
char *config_id;

/* local vars */
static vector_t *current_keywords;
static FILE *current_stream;
static int sublevel = 0;
static int skip_sublevel = 0;

/* Parameter definitions */
static list defs;

/* Forward declarations for recursion */
static bool read_line(char *, size_t);

static char *
null_strvec(const vector_t *strvec, size_t index)
{
	if (index - 1 < vector_size(strvec) && index > 0 && vector_slot(strvec, index - 1))
		log_message(LOG_INFO, "*** Configuration line starting `%s` is missing a parameter after keyword `%s` at word position %zu", vector_slot(strvec, 0) ? (char *)vector_slot(strvec, 0) : "***MISSING ***", (char *)vector_slot(strvec, index - 1), index + 1);
	else
		log_message(LOG_INFO, "*** Configuration line starting `%s` is missing a parameter at word position %zu", vector_slot(strvec, 0) ? (char *)vector_slot(strvec, 0) : "***MISSING ***", index + 1);

	exit(KEEPALIVED_EXIT_CONFIG);

	return NULL;
}

//存入keyword
static void
keyword_alloc(vector_t *keywords_vec, const char *string, void (*handler) (vector_t *), bool active)
{
	keyword_t *keyword;

	//将keywords_vec增加1个空间
	vector_alloc_slot(keywords_vec);

	//构造keyword
	keyword = (keyword_t *) MALLOC(sizeof(keyword_t));
	keyword->string = string;
	keyword->handler = (active) ? handler : NULL;
	keyword->active = active;

	vector_set_slot(keywords_vec, keyword);
}

//将string存为keywords_vec中第sublevel层的最后一个
static void
keyword_alloc_sub(vector_t *keywords_vec, const char *string, void (*handler) (vector_t *))
{
	int i = 0;
	keyword_t *keyword;

	/* fetch last keyword */
	//取出上一个关键字
	keyword = vector_slot(keywords_vec, vector_size(keywords_vec) - 1);

	/* Don't install subordinate keywords if configuration block inactive */
	if (!keyword->active)
		return;//无法将此关键字加入，上层关键字不活跃（被置为inactive)

	/* position to last sub level */
	//开始找次顶层，。。。一直找sublevel层（每次均找最后一个）
	for (i = 0; i < sublevel; i++)
		keyword = vector_slot(keyword->sub, vector_size(keyword->sub) - 1);

	/* First sub level allocation */
	//找到最后一次，如果其没有设置sub,则创建sub
	if (!keyword->sub)
		keyword->sub = vector_alloc();

	/* add new sub keyword */
	//将关键字存入sub中
	keyword_alloc(keyword->sub, string, handler, true);
}

/* Exported helpers */
//增加层数
void
install_sublevel(void)
{
	sublevel++;
}

//减少层数
void
install_sublevel_end(void)
{
	sublevel--;
}

//安装顶级keyword
void
install_keyword_root(const char *string, void (*handler) (vector_t *), bool active)
{
	keyword_alloc(keywords, string, handler, active);
}

void
install_root_end_handler(void (*handler) (void))
{
	keyword_t *keyword;

	/* fetch last keyword */
	keyword = vector_slot(keywords, vector_size(keywords) - 1);

	if (!keyword->active)
		return;

	keyword->sub_close_handler = handler;
}

//安装非顶级sub(实际上如果所有的keyword均存入到sub中，这样就不需要install_keyword_root函数了）
void
install_keyword(const char *string, void (*handler) (vector_t *))
{
	keyword_alloc_sub(keywords, string, handler);
}

void
install_sublevel_end_handler(void (*handler) (void))
{
	int i = 0;
	keyword_t *keyword;

	/* fetch last keyword */
	keyword = vector_slot(keywords, vector_size(keywords) - 1);

	if (!keyword->active)
		return;

	/* position to last sub level */
	for (i = 0; i < sublevel; i++)
		keyword = vector_slot(keyword->sub, vector_size(keyword->sub) - 1);
	keyword->sub_close_handler = handler;
}

#if DUMP_KEYWORDS
static void
dump_keywords(vector_t *keydump, int level, FILE *fp)
{
	unsigned int i;
	keyword_t *keyword_vec;
	char file_name[21];

	if (!level) {
		snprintf(file_name, sizeof(file_name), "/tmp/keywords.%d", getpid());
		fp = fopen(file_name, "w");
		if (!fp)
			return;
	}

	for (i = 0; i < vector_size(keydump); i++) {
		keyword_vec = vector_slot(keydump, i);
		fprintf(fp, "%*sKeyword : %s (%s)\n", level * 2, "", keyword_vec->string, keyword_vec->active ? "active": "disabled");
		if (keyword_vec->sub)
			dump_keywords(keyword_vec->sub, level + 1, fp);
	}

	if (!level)
		fclose(fp);
}
#endif

static void
free_keywords(vector_t *keywords_vec)
{
	keyword_t *keyword_vec;
	unsigned int i;

	for (i = 0; i < vector_size(keywords_vec); i++) {
		keyword_vec = vector_slot(keywords_vec, i);
		if (keyword_vec->sub)
			free_keywords(keyword_vec->sub);
		FREE(keyword_vec);
	}
	vector_free(keywords_vec);
}

//传入的字符串是一组token流，解析这组token,记录在vector中
vector_t *
alloc_strvec(char *string)
{
	char *cp, *start, *token;
	size_t str_len;
	vector_t *strvec;

	if (!string)
		return NULL;

	cp = string;

	/* Skip white spaces */
	//跳过前面的空格
	while (isspace((int) *cp) && *cp != '\0')
		cp++;

	/* Return if there is only white spaces */
	//空白行
	if (*cp == '\0')
		return NULL;

	/* Return if string begin with a comment */
	//注释行
	if (*cp == '!' || *cp == '#')
		return NULL;

	/* Create a vector and alloc each command piece */
	strvec = vector_alloc();

	while (1) {
		start = cp;

		/* Save a quoted string without the "s as a single string */
		if (*cp == '"') {
			//遇到字符串
			start++;
			if (!(cp = strchr(start, '"'))) {
				log_message(LOG_INFO, "Unmatched quote: '%s'", string);
				return strvec;
			}
			str_len = (size_t)(cp - start);//记录字符串长度
			cp++;
		} else {
			//遇到一个token
			while (!isspace((int) *cp) && *cp != '\0' && *cp != '"'
						   && *cp != '!' && *cp != '#')
				cp++;
			str_len = (size_t)(cp - start);//记录token的长度
		}
		//保存token
		token = MALLOC(str_len + 1);
		memcpy(token, start, str_len);
		token[str_len] = '\0';

		/* Alloc & set the slot */
		//将token存入到strvec中
		vector_alloc_slot(strvec);
		vector_set_slot(strvec, token);

		//跳过下一个token的前导空格
		while (isspace((int) *cp) && *cp != '\0')
			cp++;
		//跳过注释的后半载行，或者到达行尾
		if (*cp == '\0' || *cp == '!' || *cp == '#')
			return strvec;
	}
}

/* recursive configuration stream handler */
static int kw_level = 0;
static void
process_stream(vector_t *keywords_vec, int need_bob)
{
	unsigned int i;
	keyword_t *keyword_vec;
	char *str;
	char *buf;
	vector_t *strvec;
	vector_t *prev_keywords = current_keywords;
	current_keywords = keywords_vec;
	int bob_needed = 0;

	buf = MALLOC(MAXBUF);
	//读一行数据，展开$指明的变量，处理include指令
	while (read_line(buf, MAXBUF)) {
		strvec = alloc_strvec(buf);

		if (!strvec)
			continue;

		str = vector_slot(strvec, 0);//取解析的第一个token

		if (skip_sublevel == -1) {
			/* There wasn't a '{' on the keyword line */
			if (!strcmp(str, BOB)) {
				/* We've got the opening '{' now */
				skip_sublevel = 1;
				free_strvec(strvec);
				continue;
			}
			else {
				/* The skipped keyword doesn't have a {} block, so we no longer want to skip */
				skip_sublevel = 0;
			}
		}
		if (skip_sublevel) {
			for (i = 0; i < vector_size(strvec); i++) {
				str = vector_slot(strvec,i);
				if (!strcmp(str,BOB))
					skip_sublevel++;
				else if (!strcmp(str,EOB)) {
					if (--skip_sublevel == 0)
						break;
				}
			}

			free_strvec(strvec);
			continue;
		}

		if (need_bob) {
			need_bob = 0;
			if (!strcmp(str, BOB) && kw_level > 0) {
				free_strvec(strvec);
				continue;
			}
			else
				log_message(LOG_INFO, "Missing '{' at beginning of configuration block");
		}
		else if (!strcmp(str, BOB)) {
			log_message(LOG_INFO, "Unexpected '{' - ignoring");
			free_strvec(strvec);
			continue;
		}

		if (!strcmp(str, EOB) && kw_level > 0) {
			free_strvec(strvec);
			break;
		}

		//将str与keywords_vec中的值进行匹配，匹配命令
		for (i = 0; i < vector_size(keywords_vec); i++) {
			keyword_vec = vector_slot(keywords_vec, i);

			if (!strcmp(keyword_vec->string, str)) {
				//str与keyword_vec匹配成功
				if (!keyword_vec->active) {
					if (!strcmp(vector_slot(strvec, vector_size(strvec)-1), BOB))
						skip_sublevel = 1;
					else
						skip_sublevel = -1;
				}

				/* There is an inconsistency here. 'static_ipaddress' for example
				 * does not have sub levels, but needs a '{' */
				if (keyword_vec->sub) {
					/* Remove a trailing '{' */
					char *bob = vector_slot(strvec, vector_size(strvec)-1) ;
					if (!strcmp(bob, BOB)) {
						vector_unset(strvec, vector_size(strvec)-1);
						FREE(bob);
						bob_needed = 0;
					}
					else
						bob_needed = 1;
				}

				//与keyword_vec匹配，调用回调，传入本行解析的strvec
				if (keyword_vec->handler)
					(*keyword_vec->handler) (strvec);

				//如果此keyword_vec有内部命令，则递归处理
				if (keyword_vec->sub) {
					kw_level++;
					process_stream(keyword_vec->sub, bob_needed);
					kw_level--;
					if (keyword_vec->active && keyword_vec->sub_close_handler)
						(*keyword_vec->sub_close_handler) ();
				}
				break;
			}
		}

		if (i >= vector_size(keywords_vec))
			log_message(LOG_INFO, "Unknown keyword '%s'", str );

		free_strvec(strvec);
	}

	current_keywords = prev_keywords;
	FREE(buf);
	return;
}

static bool
read_conf_file(const char *conf_file)
{
	FILE *stream;
	glob_t globbuf;
	size_t i;
	int	res;
	struct stat stb;
	unsigned num_matches = 0;

	globbuf.gl_offs = 0;
	res = glob(conf_file, GLOB_MARK
#if HAVE_DECL_GLOB_BRACE
					| GLOB_BRACE
#endif
						    , NULL, &globbuf);

	if (res) {
		if (res == GLOB_NOMATCH)
			log_message(LOG_INFO, "No config files matched '%s'.", conf_file);
		else
			log_message(LOG_INFO, "Error reading config file(s): glob(\"%s\") returned %d, skipping.", conf_file, res);
		return true;
	}

	//遍历通配的文件
	for (i = 0; i < globbuf.gl_pathc; i++) {
		//跳过目录
		if (globbuf.gl_pathv[i][strlen(globbuf.gl_pathv[i])-1] == '/') {
			/* This is a directory - so skip */
			continue;
		}

		log_message(LOG_INFO, "Opening file '%s'.", globbuf.gl_pathv[i]);
		stream = fopen(globbuf.gl_pathv[i], "r");
		//跳过打不开的文件
		if (!stream) {
			log_message(LOG_INFO, "Configuration file '%s' open problem (%s) - skipping"
				       , globbuf.gl_pathv[i], strerror(errno));
			continue;
		}

		/* Make sure what we have opened is a regular file, and not for example a directory or executable */
		//跳过非规则文件，跳过有执行权限的文件
		if (fstat(fileno(stream), &stb) ||
		    !S_ISREG(stb.st_mode) ||
		    (stb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
			log_message(LOG_INFO, "Configuration file '%s' is not a regular non-executable file - skipping", globbuf.gl_pathv[i]);
			fclose(stream);
			continue;
		}

		//匹配数加1
		num_matches++;

		current_stream = stream;

		int curdir_fd = -1;
		if (strchr(globbuf.gl_pathv[i], '/')) {
			/* If the filename contains a directory element, change to that directory.
			   The man page open(2) states that fchdir() didn't support O_PATH until Linux 3.5,
			   even though testing on Linux 3.1 shows it appears to work. To be safe, don't
			   use it until Linux 3.5. */
			curdir_fd = open(".", O_RDONLY | O_DIRECTORY
#if HAVE_DECL_O_PATH && LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
								     | O_PATH
#endif
									     );

			char *confpath = strdup(globbuf.gl_pathv[i]);
			dirname(confpath);
			if (chdir(confpath) < 0)//改变工作目录到配置文件所在的目录
				log_message(LOG_INFO, "chdir(%s) error (%s)", confpath, strerror(errno));
			free(confpath);
		}

		process_stream(current_keywords, 0);
		fclose(stream);

		/* If we changed directory, restore the previous directory */
		if (curdir_fd != -1) {
			if ((res = fchdir(curdir_fd)))
				log_message(LOG_INFO, "Failed to restore previous directory after include");
			close(curdir_fd);
			if (res)
				return true;
		}
	}

	globfree(&globbuf);
	if (LIST_EXISTS(defs))
		free_list(&defs);

	if (!num_matches)
		log_message(LOG_INFO, "No config files matched '%s'.", conf_file);

	return false;
}

bool check_conf_file(const char *conf_file)
{
	glob_t globbuf;
	size_t i;
	bool ret = true;
	int res;
	struct stat stb;
	unsigned num_matches = 0;

	globbuf.gl_offs = 0;
	res = glob(conf_file, GLOB_MARK
#if HAVE_DECL_GLOB_BRACE
					| GLOB_BRACE
#endif
						    , NULL, &globbuf);
	if (res) {
		log_message(LOG_INFO, "Unable to find configuration file %s (glob returned %d)", conf_file, res);
		return false;
	}

	for (i = 0; i < globbuf.gl_pathc; i++) {
		if (globbuf.gl_pathv[i][strlen(globbuf.gl_pathv[i])-1] == '/') {
			/* This is a directory - so skip */
			continue;
		}

		if (access(globbuf.gl_pathv[i], R_OK)) {
			log_message(LOG_INFO, "Unable to read configuration file %s", globbuf.gl_pathv[i]);
			ret = false;
			break;
		}

		/* Make sure that the file is a regular file, and not for example a directory or executable */
		if (stat(globbuf.gl_pathv[i], &stb) ||
		    !S_ISREG(stb.st_mode) ||
		     (stb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
			log_message(LOG_INFO, "Configuration file '%s' is not a regular non-executable file", globbuf.gl_pathv[i]);
			ret = false;
			break;
		}

		num_matches++;
	}

	if (ret) {
		if (num_matches > 1)
			log_message(LOG_INFO, "WARNING, more than one file matches configuration file %s, using %s", conf_file, globbuf.gl_pathv[0]);
		else if (num_matches == 0) {
			log_message(LOG_INFO, "Unable to find configuration file %s", conf_file);
			ret = false;
		}
	}

	globfree(&globbuf);

	return ret;
}

static bool
check_include(char *buf)
{
	vector_t *strvec;
	bool ret = false;
	FILE *prev_stream;

	strvec = alloc_strvec(buf);

	if (!strvec)
		return false;

	if(!strcmp("include", vector_slot(strvec, 0)) && vector_size(strvec) == 2) {
		prev_stream = current_stream;

		read_conf_file(vector_slot(strvec, 1));

		current_stream = prev_stream;
		ret = true;
	}

	free_strvec(strvec);
	return ret;
}

static def_t *
find_definition(const char *name, size_t len, bool definition)
{
	element e;
	def_t *def;
	const char *p;
	bool using_braces = false;
	bool allow_multiline;

	if (LIST_ISEMPTY(defs))
		return NULL;

	if (!definition && *name == '{') {
		using_braces = true;
		name++;
	}

	//必须有合法的变量起始符
	if (!isalpha(*name) && *name != '_')
		return NULL;

	if (!len) {
		//如果未指定长度，则计算长度
		for (len = 1, p = name + 1; *p != '\0' && (isalnum(*p) || *p == '_'); len++, p++);

		/* Check we have a suitable end character */
		//排除掉不合法的输入（遇到非合法变量起始符或者缺少'}'符
		if (using_braces && *p != '}')
			return NULL;

		//
		if (!using_braces && !definition &&
		     *p != ' ' && *p != '\t' && *p != '\0')
			return NULL;
	}

	if (definition ||
	    (!using_braces && name[len] == '\0') ||
	    (using_braces && name[len+1] == '\0'))
		allow_multiline = true;
	else
		allow_multiline = false;

	//查找是否有名称为name的def
	for (e = LIST_HEAD(defs); e; ELEMENT_NEXT(e)) {
		def = ELEMENT_DATA(e);
		if (def->name_len == len &&
		    (allow_multiline || !def->multiline) &&
		    !strncmp(def->name, name, len))
			return def;
	}

	//未找到
	return NULL;
}

static char *
replace_param(char *buf, size_t max_len, bool in_multiline)
{
	char *cur_pos = buf;
	size_t len_used = strlen(buf);
	def_t *def;
	char *s, *d, *e;
	ssize_t i;
	size_t extra_braces;
	size_t replacing_len;
	char *next_ptr = NULL;

	while ((cur_pos = strchr(cur_pos, '$')) && cur_pos[1] != '\0') {
		if ((def = find_definition(cur_pos + 1, 0, false))) {
			extra_braces = cur_pos[1] == '{' ? 2 : 0;

			/* We can't handle nest multiline definitions */
			if (def->multiline && in_multiline) {
				log_message(LOG_INFO, "Expansion of multiline definition within multiline definitions not supported");
				cur_pos += def->name_len + 1 + extra_braces;
				continue;
			}

			/* Ensure there is enough room to replace $PARAM or ${PARAM} with value */
			if (def->multiline) {
				replacing_len = strchr(def->value, DEF_LINE_END) - def->value;
				in_multiline = true;
				next_ptr = def->value + replacing_len + 1;
			}
			else
				replacing_len = def->value_len;

			if (len_used + replacing_len - (def->name_len + 1 + extra_braces) >= max_len) {
				log_message(LOG_INFO, "Parameter substitution on line '%s' would exceed maximum line length", buf);
				return NULL;
			}

			if (def->name_len + 1 + extra_braces != replacing_len) {
				/* We need to move the existing text */
				if (def->name_len + 1 + extra_braces < replacing_len) {
					/* We are lengthening the buf text */
					s = cur_pos + strlen(cur_pos);
					d = s - (def->name_len + 1 + extra_braces) + replacing_len;
					e = cur_pos;
					i = -1;
				} else {
					/* We are shortening the buf text */
					s = cur_pos + (def->name_len + 1 + extra_braces) - replacing_len;
					d = cur_pos;
					e = cur_pos + strlen(cur_pos);
					i = 1;
				}

				do {
					*d = *s;
					if (s == e)
						break;
					d += i;
					s += i;
				} while (true);

				len_used = len_used + replacing_len - (def->name_len + 1 + extra_braces);
			}

			/* Now copy the replacement text */
			strncpy(cur_pos, def->value, replacing_len);
		}
		else
			cur_pos++;
	}

	return next_ptr;
}

static void
free_definition(void *d)
{
	def_t *def = d;

	FREE(def->name);
	FREE(def->value);
	FREE(def);
}

/* A definition is of the form $NAME=TEXT */
static def_t*
check_definition(const char *buf)
{
	const char *p;
	def_t* def;
	char *str;

	if (buf[0] != '$')
		return false;

	//不是有效的变量名开始符，返回false
	if (!isalpha(buf[1]) && buf[1] != '_')
		return false;

	//检查是否合法的变量字符，如果遇到'='跳出，如果不合法返回false
	for (p = &buf[2]; *p; p++) {
		if (*p == '=')
			break;
		if (!isalnum(*p) &&
		    !isdigit(*p) &&
		    *p != '_')
			return false;
	}

	//如果退出时未遇到等号，表明缺少'='号，返回false
	if (*p != '=')
		return false;

	//检查def是否存在
	if ((def = find_definition(&buf[1], p - &buf[1], true)))
		FREE(def->value);
	else {
		//def不存在，创建相应的def并加入到defs链表中
		def = MALLOC(sizeof(*def));
		def->name_len = p - &buf[1];
		str = MALLOC(def->name_len + 1);
		strncpy(str, &buf[1], def->name_len);
		str[def->name_len] = '\0';
		def->name = str;

		//如果defs链表不存在，则创建defs链表
		if (!LIST_EXISTS(defs))
			defs = alloc_list(free_definition, NULL);
		list_add(defs, def);
	}

	p++;//跳过‘＝’号
	def->value_len = strlen(p);
	if (p[def->value_len - 1] == '\\') {
		/* Remove leading and trailing whitespace */
		//跳过前导的空格
		while (isblank(*p))
			p++, def->value_len--;
		//跳过结尾的空格
		while (def->value_len >= 2) {
			if (isblank(p[def->value_len - 2]))
				def->value_len--;
		}
		if (def->value_len >= 2)
			def->value[def->value_len - 1] = DEF_LINE_END;
		else {
			p += def->value_len;
			def->value_len = 0;
		}
		//标记为多行
		def->multiline = true;
	} else
		def->multiline = false;
	str = MALLOC(def->value_len + 1);
	strcpy(str, p);
	//设置str
	def->value = str;

	return def;
}

static bool
read_line(char *buf, size_t size)
{
	size_t len ;
	bool eof = false;
	size_t config_id_len;
	char *buf_start;
	bool rev_cmp;
	size_t ofs;
	char *text_start;
	bool recheck;
	static def_t *def = NULL;
	static char *next_ptr = NULL;
	bool multiline_param_def = false;
	char *new_str;
	char *end;
	char *next_ptr1;

	config_id_len = config_id ? strlen(config_id) : 0;
	do {
		if (next_ptr) {
			/* We are expanding a multiline parameter, so copy next line */
			end = strchr(next_ptr, DEF_LINE_END);
			if (!end) {
				strcpy(buf, next_ptr);
				next_ptr = NULL;
			} else {
				strncpy(buf, next_ptr, end - next_ptr);
				buf[end - next_ptr] = '\0';
				next_ptr = end + 1;
			}
		}
		//读取一行数据
		else if (!fgets(buf, (int)size, current_stream))
		{
			//文件到达结尾，退出循环
			eof = true;
			buf[0] = '\0';
			break;
		}

		/* Remove trailing <CR>/<LF> */
		//移除\n或者\r
		len = strlen(buf);
		while (len && (buf[len-1] == '\n' || buf[len-1] == '\r'))
			buf[--len] = '\0';

		/* Handle multi-line definitions */
		if (multiline_param_def) {
			//多行处理
			/* Remove leading and trailing spaces and tabs */
			text_start = buf + strspn(buf, " \t");
			len -= text_start - buf;
			if (len && text_start[len-1] == '\\') {
				while (len >= 2 && isblank(text_start[len - 2]))
					len--;
				text_start[len-1] = DEF_LINE_END;
			} else {
				while (len >= 1 && isblank(text_start[len - 1]))
					len--;
				multiline_param_def = false;
			}

			/* Skip blank lines */
			if (!len ||
			    (len == 1 && multiline_param_def)) {
				buf[0] = '\0';
				continue;
			}

			/* Add the line to the definition */
			new_str = MALLOC(def->value_len + len + 1);
			strcpy(new_str, def->value);
			strncpy(new_str + def->value_len, text_start, len);
			new_str[def->value_len + len] = '\0';
			FREE(def->value);
			def->value = new_str;
			def->value_len += len;

			buf[0] = '\0';
			continue;
		}

		//跳过空行
		if (len == 0)
			continue;

		//跳过起始的空格及tab符，跳过trim后的空行
		text_start = buf + strspn(buf, " \t");
		if (text_start[0] == '\0') {
			buf[0] = '\0';
			continue;
		}

		recheck = false;
		do {
			//处理以@符开头的行
			if (text_start[0] == '@') {
				/* If the line starts '@', check the following word matches the system id.
				   @^ reverses the sense of the match */
				if (text_start[1] == '^') {
					rev_cmp = true;
					ofs = 2;
				} else {
					rev_cmp = false;
					ofs = 1;
				}

				/* We need something after the system_id */
				if (!(buf_start = strpbrk(text_start + ofs, " \t"))) {
					buf[0] = '\0';
					break;
				}

				/* Check if config_id matches/doesn't match as appropriate */
				if ((!config_id ||
				     (size_t)(buf_start - (text_start + ofs)) != config_id_len ||
				     strncmp(text_start + ofs, config_id, config_id_len)) != rev_cmp) {
					buf[0] = '\0';
					break;
				}

				/* Remove the @config_id from start of line */
				memset(text_start, ' ', (size_t)(buf_start - text_start));

				text_start += strspn(text_start, " \t");
			}

			//遇着‘$'按变量处理
			if (text_start[0] == '$' && (def = check_definition(text_start))) {
				/* check_definition() saves the definition */
				if (def->multiline)
					multiline_param_def = true;
				buf[0] = '\0';
				break;
			}

			//变量展开
			if (!LIST_ISEMPTY(defs) && strchr(text_start, '$')) {
				next_ptr1 = replace_param(buf, size, !!next_ptr);
				if (!next_ptr)
					next_ptr = next_ptr1;
				text_start += strspn(text_start, " \t");
				if (text_start[0] == '@')
					recheck = true;
			}
		} while (recheck);
	} while (buf[0] == '\0' || check_include(buf));//include指令处理

	return !eof;
}

vector_t *
read_value_block(vector_t *strvec)
{
	char *buf;
	unsigned int word;
	char *str = NULL;
	char *dup;
	vector_t *vec = NULL;
	vector_t *elements = vector_alloc();
	int first = 1;
	int need_bob = 1;
	int got_eob = 0;

	buf = (char *) MALLOC(MAXBUF);
	while (first || read_line(buf, MAXBUF)) {
		if (first && vector_size(strvec) > 1) {
			vec = strvec;
			word = 1;
		}
		else {
			vec = alloc_strvec(buf);
			word = 0;
		}
		if (vec) {
			str = vector_slot(vec, word);
			if (need_bob) {
				if (!strcmp(str, BOB))
					word++;
				else
					log_message(LOG_INFO, "'{' missing at beginning of block %s", FMT_STR_VSLOT(strvec,0));
				need_bob = 0;
			}

			for (; word < vector_size(vec); word++) {
				str = vector_slot(vec, word);
				if (!strcmp(str, EOB)) {
					if (word != vector_size(vec) - 1)
						log_message(LOG_INFO, "Extra characters after '}' - \"%s\"", buf);
					got_eob = 1;
					break;
				}
				dup = (char *) MALLOC(strlen(str) + 1);
				memcpy(dup, str, strlen(str));
				vector_alloc_slot(elements);
				vector_set_slot(elements, dup);
			}
			if (vec != strvec)
				free_strvec(vec);
			if (got_eob)
				break;
		}
		memset(buf, 0, MAXBUF);
		first = 0;
	}

	FREE(buf);
	return elements;
}

void
alloc_value_block(void (*alloc_func) (vector_t *))
{
	char *buf;
	char *str = NULL;
	vector_t *vec = NULL;

	buf = (char *) MALLOC(MAXBUF);
	while (read_line(buf, MAXBUF)) {
		vec = alloc_strvec(buf);
		if (vec) {
			str = vector_slot(vec, 0);
			if (!strcmp(str, EOB)) {
				free_strvec(vec);
				break;
			}

			if (vector_size(vec))
				(*alloc_func) (vec);

			free_strvec(vec);
		}
	}
	FREE(buf);
}

void *
set_value(vector_t *strvec)
{
	char *str;
	size_t size;
	char *alloc;

	if (vector_size(strvec) < 2)
		return NULL;

	str = vector_slot(strvec, 1);
	size = strlen(str);

	alloc = (char *) MALLOC(size + 1);
	if (!alloc)
		return NULL;

	memcpy(alloc, str, size);

	return alloc;
}

unsigned long
read_timer(vector_t *strvec)
{
	unsigned long timer;

	timer = strtoul(strvec_slot(strvec, 1), NULL, 10);
	if (timer >= ULONG_MAX / TIMER_HZ)
		return ULONG_MAX;

	return timer * TIMER_HZ;
}

/* Checks for on/true/yes or off/false/no */
int
check_true_false(char *str)
{
	if (!strcmp(str, "true") || !strcmp(str, "on") || !strcmp(str, "yes"))
		return true;
	if (!strcmp(str, "false") || !strcmp(str, "off") || !strcmp(str, "no"))
		return false;

	return -1;	/* error */
}

void skip_block(void)
{
	/* Don't process the rest of the configuration block */
	skip_sublevel = 1;
}

/* Data initialization */
void
init_data(const char *conf_file, vector_t * (*init_keywords) (void))
{
	/* Init Keywords structure */
	keywords = vector_alloc();

	(*init_keywords) ();

#if DUMP_KEYWORDS
	/* Dump configuration */
	dump_keywords(keywords, 0, NULL);
#endif

	/* Stream handling */
	current_keywords = keywords;

	register_null_strvec_handler(null_strvec);
	read_conf_file(conf_file);//读取配置文件
	unregister_null_strvec_handler();

	/* Close the password database if it was opened */
	endpwent();

	free_keywords(keywords);
	clear_rt_names();
}
