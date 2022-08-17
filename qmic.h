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

struct symbol_type_table {
	const char* name;
	int size;
};

enum symbol_type {
	TYPE_U8,
	TYPE_U16,
	TYPE_U32,
	TYPE_U64,
	TYPE_I8,
	TYPE_I16,
	TYPE_I32,
	TYPE_I64,
	TYPE_CHAR,
	TYPE_STRING,
	TYPE_STRUCT,

	TYPE_MAX
#define SYMBOL_TYPE_MAX (TYPE_MAX - 1)
};

enum message_type {
	MESSAGE_REQUEST = 0,
	MESSAGE_RESPONSE = 2,
	MESSAGE_INDICATION = 4,
};

extern const struct symbol_type_table sz_simple_types[SYMBOL_TYPE_MAX + 1];

enum package_type {
	PKG_TYPE_SERVER = (1 << 0),
	PKG_TYPE_CLIENT = (1 << 1),
	PKG_TYPE_AGNOSTIC = (PKG_TYPE_CLIENT | PKG_TYPE_SERVER),
};

struct qmi_package {
	char *name;
	enum package_type type;
};

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
	/* Set if this message is an alias to another message */
	/* e.g. an indication with the same TLVs as a response */
	struct qmi_message *sibling;

	struct list_head node;

	struct list_head members;
};

struct qmi_struct;

struct qmi_struct_member {
	const char *name;
	// If set, this member is a pointer
	// to an existing struct
	const char *struct_type;
	int type;
	bool is_ptr;
	unsigned array_size;
	/* This member might be a struct */
	struct qmi_struct* struct_ch;

	struct list_head node;
};

struct qmi_struct {
	const char *type;
	const char *name;
	bool is_ptr;
	bool has_ptr_members;

	struct list_head node;

	struct list_head members;

	bool emitted;
};

extern struct list_head qmi_consts;
extern struct list_head qmi_messages;
extern struct list_head qmi_structs;
extern struct qmi_struct qmi_response_type_v01;
extern FILE *sourcefile;

void qmi_parse(struct qmi_package *out_pkg);

void emit_source_includes(FILE *fp, const char *package);
void guard_header(FILE *fp, const char *package);
void guard_footer(FILE *fp);
void qmi_const_header(FILE *fp);

void accessor_emit_c(FILE *fp, struct qmi_package package);
void accessor_emit_h(FILE *fp, struct qmi_package package);

void kernel_emit_c(FILE *fp, const char *package);
void kernel_emit_h(FILE *fp, const char *package);

#endif
