#ifndef __QMIC_H__
#define __QMIC_H__

#include <err.h>
#include <stdbool.h>

#include "list.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))

#define STRUCT_NEST_MAX 32

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
};

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
	/* Number of bytes used to encode array length.
	 * only valid for variable arrays.
	 */
	int array_len_type;

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
	/* Valid only if type is TYPE_STRUCT */
	struct qmi_struct *qmi_struct;
	bool is_ptr;
	bool is_struct_ref;
	/* Number of bytes used to encode array length.
	 * only valid if is_ptr is true
	 */
	int array_len_type;

	unsigned array_size;
	bool array_fixed;

	struct list_head node;
};

struct qmi_struct {
	char *name;
	/*
	 * Valid if this struct is defined
	 * as a member of another struct
	 */
	struct qmi_struct_member *member;

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

#endif
