#ifndef __QMIC_H__
#define __QMIC_H__

#include <stdbool.h>
#include <err.h>

#include "list.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))

#define QMI_STRUCT_NEST_MAX 32
#define QMI_STRUCT_TYPE_NAME_MAX (QMI_STRUCT_NEST_MAX * 24)

#define DEBUG 1

#if DEBUG == 1
#define LOGD(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOGD(fmtn, ...)
#endif

#define PLOGD(prefix, fmt, ...) LOGD("%s" fmt, prefix, ##__VA_ARGS__)

/* Allocate and zero a block of memory; and exit if it fails */
#define memalloc(size) ({						\
		void *__p = malloc(size);				\
									\
		if (!__p)						\
			errx(1, "malloc() failed in %s(), line %d\n",	\
				__func__, __LINE__);			\
		memset(__p, 0, size);					\
		__p;							\
	 })

enum symbol_type {
	TYPE_U8,
	TYPE_U16,
	TYPE_U32,
	TYPE_U64,
	TYPE_STRING,
	TYPE_STRUCT
};

#define SYMBOL_TYPE_MAX TYPE_STRUCT

enum message_type {
	MESSAGE_REQUEST = 0,
	MESSAGE_RESPONSE = 2,
	MESSAGE_INDICATION = 4,
};

extern const char *sz_simple_types[];

extern const char *qmi_package;

struct qmi_const {
	const char *name;
	unsigned long long value;

	struct list_head node;
};

struct qmi_message_member {
	const char *name;
	int type;
	struct qmi_struct *qmi_struct;
	int id;
	bool required;
	unsigned array_size;
	bool array_fixed;

	struct list_head node;
};

struct qmi_message {
	enum message_type type;
	const char *name;
	unsigned msg_id;

	struct list_head node;

	struct list_head members;
};

struct qmi_struct;

struct qmi_struct_member {
	const char *name;
	int type;
	bool is_ptr;
	/* This member might be a struct */
	struct qmi_struct* struct_ch;

	struct list_head node;
};

struct qmi_struct {
	const char *type;
	const char *name;
	bool is_ptr;

	struct list_head node;

	struct list_head members;
};

extern struct list_head qmi_consts;
extern struct list_head qmi_messages;
extern struct list_head qmi_structs;
extern FILE *sourcefile;

void qmi_parse(void);

void emit_source_includes(FILE *fp, const char *package);
void guard_header(FILE *fp, const char *package);
void guard_footer(FILE *fp);
void qmi_const_header(FILE *fp);

void accessor_emit_c(FILE *fp, const char *package);
void accessor_emit_h(FILE *fp, const char *package);

void kernel_emit_c(FILE *fp, const char *package);
void kernel_emit_h(FILE *fp, const char *package);

#endif
