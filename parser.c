#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "list.h"
#include "qmic.h"

#define TOKEN_BUF_SIZE		128	/* TOKEN_BUF_MIN or more */
#define TOKEN_BUF_MIN		24	/* Enough for a 64-bit octal number */

const char *qmi_package;

struct list_head qmi_consts = LIST_INIT(qmi_consts);
struct list_head qmi_messages = LIST_INIT(qmi_messages);
struct list_head qmi_structs = LIST_INIT(qmi_structs);

enum token_id {
	/* Also any non-NUL (7-bit) ASCII character */
	TOK_CONST = CHAR_MAX + 1,
	TOK_ID,
	TOK_MESSAGE,
	TOK_NUM,
	TOK_VALUE,
	TOK_PACKAGE,
	TOK_STRUCT,
	TOK_TYPE,
	TOK_REQUIRED,
	TOK_OPTIONAL,
	TOK_EOF,
};

struct token {
	enum token_id id;
	char *str;
	unsigned long long num;
	struct qmi_struct *qmi_struct;
};

static int yyline = 1;

static void yyerror(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	fprintf(stderr, "%s: parse error on line %u:\n\t",
		program_invocation_short_name, yyline);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");

	va_end(ap);

	exit(1);
}

static char input()
{
	int ch;

	ch = fgetc(sourcefile);
	if (ch < 0)
		return 0;	/* End of input */

	if (ch == '\n')
		yyline++;
	else if (!isascii(ch))
		yyerror("invalid non-ASCII character");
	else if (!ch)
		yyerror("invalid NUL character");

	return (char)ch;
}

static void unput(int ch)
{
	if (ch == '\n')
		yyline--;
	if (ungetc(ch, sourcefile) != ch)
		yyerror("ungetc error");
}

struct symbol {
	enum token_id token_id;
	const char *name;

	union {
		enum message_type message_type;		/* TOK_MESSAGE */
		struct {				/* TOK_TYPE */
			enum symbol_type symbol_type;
			/* TYPE_STRUCT also has a struct pointer */
			struct qmi_struct *qmi_struct;
		};
		unsigned long long value;		/* TOK_VALUE */
	};

	struct list_head node;
};

static struct list_head symbols = LIST_INIT(symbols);

static struct symbol *symbol_find(const char *name)
{
	struct symbol *sym;

	list_for_each_entry(sym, &symbols, node)
		if (!strcmp(name, sym->name))
			return sym;
	return NULL;
}

static const char *token_name(enum token_id token_id)
{
	struct symbol *sym;

	switch (token_id) {
	case TOK_ID:
		return "identifier";
	case TOK_MESSAGE:
		return "(message)";
	case TOK_NUM:
		return "(number)";
	case TOK_EOF:
		return "(EOF)";
	default:
		break;
	}

	list_for_each_entry(sym, &symbols, node)
		if (token_id == sym->token_id)
			return sym->name;

	return NULL;
}

static bool symbol_valid(const char *name)
{
	const char *p = name;
	char ch;

	/* Symbol name must start with an alphabetic character */
	if (!p || !isalpha(*p++))
		return false;

	/* Remainder of the name is alphanumeric or underscore */
	while ((ch = *p++))
		if (!(isalnum(ch) || ch == '_'))
			return false;

	/* Symbol name must fit in the token buffer */
	if (p - name > TOKEN_BUF_SIZE)
		return 0;

	/* Finally, symbol names must be unique */
	if (symbol_find(name))
		return false;

	return true;
}

static void symbol_add(const char *name, enum token_id token_id, ...)
{
	struct symbol *sym;
	va_list ap;

	//printf("Adding symbol: %s\n", name);

	assert(symbol_valid(name));

	va_start(ap, token_id);

	sym = memalloc(sizeof(struct symbol));
	sym->token_id = token_id;
	sym->name = name;

	switch (token_id) {
	case TOK_MESSAGE:
		sym->message_type = va_arg(ap, enum message_type);
		break;
	case TOK_TYPE:
		sym->symbol_type = va_arg(ap, enum symbol_type);
		if (sym->symbol_type == TYPE_STRUCT)
			sym->qmi_struct = va_arg(ap, struct qmi_struct *);
		break;
	case TOK_VALUE:
		sym->value = va_arg(ap, unsigned long long);
		break;
	default:
		break;	/* Most tokens are standalone */
	}

	list_add(&symbols, &sym->node);

	va_end(ap);
}

/* Skip over white space and comments (which start with '#', end with '\n') */
static bool skip(char ch)
{
	static bool in_comment = false;

	if (in_comment) {
		if (ch == '\n')
			in_comment = false;
		return true;
	}

	if (isspace(ch))
		return true;

	if (ch == '#')
		in_comment = true;

	return in_comment;
}

/* Extract an identifier from input into the given buffer */
static struct symbol *qmi_identifier_parse(char *buf, size_t size, char ch)
{
	char *p = buf;

	/* First character is known to be alphabetic */
	*p++ = ch;
	while ((ch = input()) && (isalnum(ch) || ch == '_')) {
		if (p - buf == size) {
			buf[TOKEN_BUF_MIN] = '\0';
			yyerror("token too long: \"%s...\"", buf);
		}
		*p++ = ch;
	}
	unput(ch);
	*p = '\0';

	return symbol_find(buf);
}

/* Used for parsing octal numbers */
static int isodigit(int c)
{
	return isdigit(c) && c < '8';
}

/* Extract a number from input into the given buffer; return base */
static unsigned qmi_number_parse(char *buf, size_t size, char ch)
{
	int (*isvalid)(int) = isdigit;
	char *p = buf;
	unsigned base = 10;

	/* First character is known to be a digit 0-9 */
	*p++ = ch;

	/* Determine base and valid character set */
	if (ch == '0') {
		ch = input();
		if (ch == 'x' || ch == 'X') {
			*p++ = ch;
			ch = input();
			isvalid = isxdigit;
			base = 16;
		} else if (isodigit(ch)) {
			isvalid = isodigit;
			base = 8;
		}
		unput(ch);
	}

	while ((ch = input()) && isvalid(ch)) {
		if (p - buf == size) {
			buf[TOKEN_BUF_MIN] = '\0';
			yyerror("number too long: \"%s...\"", buf);
		}
		*p++ = ch;
	}
	unput(ch);
	*p = '\0';

	return base;
}

static struct token yylex()
{
	struct symbol *sym;
	struct token token = {};
	unsigned long long num;
	char buf[TOKEN_BUF_SIZE];
	int base;
	char ch;

	while ((ch = input()) && skip(ch))
		;

	if (isalpha(ch)) {
		sym = qmi_identifier_parse(buf, sizeof(buf), ch);

		token.str = strdup(buf);
		if (!token.str)
			yyerror("strdup() failed in %s(), line %d\n",
				__func__, __LINE__);
		if (sym) {
			token.id = sym->token_id;
			switch (token.id) {
			case TOK_MESSAGE:
				token.num = sym->message_type;
				break;
			case TOK_TYPE:
				token.num = sym->symbol_type;
				token.qmi_struct = sym->qmi_struct;
				break;
			case TOK_VALUE:
				/* Override token id; use numeric value */
				token.id = TOK_NUM;
				token.num = sym->value;
				break;
			default:
				break;	/* Others just have id and string */
			}
		} else {
			token.id = TOK_ID;	/* Just an identifier */
		}

		return token;
	} else if (isdigit(ch)) {
		base = qmi_number_parse(buf, sizeof(buf), ch);

		errno = 0;
		num = strtoull(buf, NULL, base);
		if (errno)
			yyerror("number %s out of range", buf);

		token.num = num;
		token.id = TOK_NUM;

		return token;
	} else if (!ch) {
		token.id = TOK_EOF;

		return token;
	}

	token.id = ch;
	return token;
}

static struct token curr_token;

static void token_init(void)
{
	curr_token = yylex();
}

static bool token_accept(enum token_id token_id, struct token *tok)
{
	// printf("have: %s / %d:%c / %d, want: %d:%c\n",
	// 	curr_token.str, curr_token.id, curr_token.id,
	// 	curr_token.num, token_id, token_id);
	if (curr_token.id != token_id)
		return false;

	/* Be sure to free the token string if caller won't be doing it */
	if (tok)
		*tok = curr_token;
	else if (curr_token.str)
		free(curr_token.str);

	curr_token = yylex();

	return true;
}

static void token_expect(enum token_id token_id, struct token *tok)
{
	const char *want;

	if (token_accept(token_id, tok))
		return;

	want = token_name(token_id);
	if (want)
		yyerror("expected %s", want);
	else
		yyerror("expected '%c'", token_id);
}

static void qmi_package_parse(void)
{
	struct token tok;

	token_expect(TOK_ID, &tok);
	token_expect(';', NULL);

	if (qmi_package)
		yyerror("package may only be specified once");
	qmi_package = tok.str;
}

static void qmi_const_parse()
{
	struct qmi_const *qcm;
	struct qmi_const *qc;
	struct token num_tok;
	struct token id_tok;

	token_expect(TOK_ID, &id_tok);
	token_expect('=', NULL);
	token_expect(TOK_NUM, &num_tok);
	token_expect(';', NULL);

	list_for_each_entry(qcm, &qmi_consts, node)
		if (!strcmp(qcm->name, id_tok.str))
			yyerror("duplicate constant \"%s\"", qcm->name);

	qc = memalloc(sizeof(struct qmi_const));
	qc->name = id_tok.str;
	qc->value = num_tok.num;

	list_add(&qmi_consts, &qc->node);

	symbol_add(qc->name, TOK_VALUE, qc->value);
}

static void qmi_message_parse(enum message_type message_type)
{
	struct qmi_message_member *qmm;
	struct qmi_message *qm;
	struct token msg_id_tok;
	struct token type_tok;
	struct token num_tok;
	struct token id_tok;
	unsigned array_size;
	int array_len_type;
	bool array_fixed;
	bool required;

	token_expect(TOK_ID, &msg_id_tok);
	token_expect('{', NULL);

	qm = memalloc(sizeof(struct qmi_message));
	qm->name = msg_id_tok.str;
	qm->type = message_type;
	list_init(&qm->members);

	while (!token_accept('}', NULL)) {
		array_len_type = -1;
		array_size = 0;
		array_fixed = false;
		if (token_accept(TOK_REQUIRED, NULL))
			required = true;
		else if (token_accept(TOK_OPTIONAL, NULL))
			required = false;
		else
			yyerror("expected required, optional or '}'");

		token_expect(TOK_TYPE, &type_tok);
		token_expect(TOK_ID, &id_tok);

		if (token_accept('[', NULL)) {
			token_expect(TOK_NUM, &num_tok);
			array_size = num_tok.num;
			token_expect(']', NULL);
			array_fixed = true;
		} else if (token_accept('(', NULL)) {
			if (token_accept(TOK_NUM, &num_tok)) {
				array_size = num_tok.num;
			} else if (token_accept(TOK_TYPE, &num_tok)) {
				if (num_tok.num > TYPE_CHAR)
					yyerror("Array size type must be a basic type");
				array_len_type = num_tok.num;
				array_size = 1;
			}
			token_expect(')', NULL);
			array_fixed = false;
		}

		token_expect('=', NULL);
		token_expect(TOK_NUM, &num_tok);
		token_expect(';', NULL);

		list_for_each_entry(qmm, &qm->members, node) {
			if (!strcmp(qmm->name, id_tok.str))
				yyerror("duplicate message member \"%s\"",
					qmm->name);
			if (qmm->id == num_tok.num)
				yyerror("duplicate message member number %u",
					qmm->id);
		}

		qmm = memalloc(sizeof(struct qmi_message_member));
		qmm->name = id_tok.str;
		qmm->type = type_tok.num;
		if (type_tok.str)
			free(type_tok.str);
		qmm->qmi_struct = type_tok.qmi_struct;
		qmm->id = num_tok.num;
		qmm->required = required;
		qmm->array_size = array_size;
		qmm->array_len_type = array_len_type;
		qmm->array_fixed = array_fixed;

		list_add(&qm->members, &qmm->node);
	}

	if (token_accept('=', NULL)) {
		token_expect(TOK_NUM, &num_tok);

		qm->msg_id = num_tok.num;
	}

	token_expect(';', NULL);

	list_add(&qmi_messages, &qm->node);
}

static void qmi_struct_gen_names(struct qmi_struct *qs, char *_namebuf)
{
	struct qmi_struct_member *qsm;
	char *namebuf = memalloc(1024);

	if (qs->name) {
		// In case this is a reference to a previously defined struct
		if (symbol_find(qs->name))
			return;
		strcpy(namebuf, qs->name);
		free(qs->name);
	} else {
		strcpy(namebuf, _namebuf);
		strcat(namebuf, "_");
		strcat(namebuf, qs->member->name);
	}

	qs->name = strdup(namebuf);
	list_for_each_entry(qsm, &qs->members, node)
		if (qsm->type == TYPE_STRUCT && !qsm->is_struct_ref) {
			qmi_struct_gen_names(qsm->qmi_struct, qs->name);
		}

	symbol_add(qs->name, TOK_TYPE, TYPE_STRUCT, qs);
}

/*
 * @brief: parses the type to use for variable array lengh, or the
 * length of a fixed array.
 * 
 * structs containing variable length arrays must be in
 * length-value format, the number of bytes used to store the length
 * is specific to the particular QMI message, this function is used to
 * parse that format, example:
 * 
 * struct file_attrs_t {
 * 	u16 file_size;
 * 	u16 file_id;
 * 	...
 * 	u8 *raw_data(u16); # u16 is the type used to encode the size
 * };
 * 
 * The packed QMI message will look something like this (this isn't a valid C struct
 * as the variable length data might not be at the end):
 * 
 * struct file_attrs_msg {
 * 	u16 file_size;
 * 	u16 file_id;
 * 	...
 * 	u16 raw_data_len;
 * 	u8 raw_data[];
 * };
 */
static inline void qmi_struct_parse_array_len_size(struct qmi_struct_member *qsm)
{
	struct token array_len_size;

	if (qsm->is_ptr) {
		if (!token_accept('(', NULL))
			yyerror("Variable length arrays must define the length type, "
				"e.g. u8 *my_data(u16);");
		token_expect(TOK_TYPE, &array_len_size);
		if (array_len_size.num >= TYPE_STRING)
			yyerror("Array size type must be a basic type");
		token_expect(')', NULL);
		qsm->array_len_type = array_len_size.num;
	} else if (token_accept('[', NULL)) {
		if (qsm->is_ptr)
			yyerror("Fixed arrays can't be pointers");

		token_expect(TOK_NUM, &array_len_size);
		token_expect(']', NULL);
		qsm->array_size = array_len_size.num;
		qsm->array_fixed = !!qsm->array_size;
	}

	return;
}

static void qmi_struct_parse(void)
{
	struct qmi_struct_member *qsm, *qsm_temp;
	struct token struct_id_tok;
	struct qmi_struct **structs;
	struct qmi_struct *qs;
	int nest = 0;
	struct token type_tok;
	struct token id_tok;

	token_expect(TOK_ID, &struct_id_tok);
	token_expect('{', NULL);

	structs = memalloc(sizeof(struct qmi_struct*) * STRUCT_NEST_MAX);
	qs = structs[0] = memalloc(sizeof(struct qmi_struct));

	qs->name = struct_id_tok.str;
	list_init(&qs->members);

	while (true) {
		char *struct_type = NULL;
		qsm = memalloc(sizeof(struct qmi_struct_member));
		/*
		 * If we find a nested struct definition, "push" it to the structs stack
		 * and parse the struct header. Also parse a member which references a previously
		 * defined struct.
		 *	struct [TYPE] [REFERENCE|DEFINITION]
		 *	REFERENCE: (required TYPE)[*]ID<string>';'
		 *	DEFINITION: '{' MEMBERS '}' ';'
		 */
		if (token_accept(TOK_STRUCT, NULL)) {
			/*
			 * If we accept TOK_TYPE, that means the struct was already defined
			 * if instead we accept TOK_ID then it's a custom type for a new struct
			 */
			if (token_accept(TOK_TYPE, &type_tok)) {
				qsm->qmi_struct = type_tok.qmi_struct;
				qsm->is_struct_ref = true;
				if (!qsm->qmi_struct) // SHOULD NOT HAPPEN
					yyerror("Can't find qmi_struct pointer for \"%s\"",
						type_tok.str);
			} else {
				// Optionally define a custom type for the nested struct
				if (token_accept(TOK_ID, &id_tok))
					struct_type = id_tok.str;

				// Add the struct member associated with this nested
				// struct
				list_add(&qs->members, &qsm->node);

				nest++;
				if (nest == STRUCT_NEST_MAX) {
					yyerror("Can't have nested structs more than %d levels "
						"deep!", STRUCT_NEST_MAX);
				}
				qs = structs[nest] = memalloc(sizeof(struct qmi_struct));
				list_init(&qs->members);

				// Save this for later
				qs->member = qsm;
				if (struct_type)
					qs->name = struct_type;

				token_expect('{', NULL);
				continue;
			}
		} else if (!token_accept(TOK_TYPE, &type_tok)) {
			token_expect('}', NULL);

			if (nest) {
				qsm = qs->member;
				if (token_accept('*', NULL))
					qsm->is_ptr = true;
				token_expect(TOK_ID, &id_tok);
				qsm->name = id_tok.str;
				qsm->type = TYPE_STRUCT;
				qsm->qmi_struct = qs;

				qmi_struct_parse_array_len_size(qsm);
			}

			token_expect(';', NULL);

			list_add(&qmi_structs, &qs->node);

			if (!nest)
				break;

			nest--;
			qs = structs[nest];
			continue;
		}

		if (token_accept('*', NULL))
			qsm->is_ptr = true;
		token_expect(TOK_ID, &id_tok);
		qmi_struct_parse_array_len_size(qsm);
		token_expect(';', NULL);

		list_for_each_entry(qsm_temp, &qs->members, node)
			if (!strcmp(qsm_temp->name, id_tok.str))
				yyerror("duplicate struct member \"%s\"",
					qsm_temp->name);
		

		qsm->name = id_tok.str;
		qsm->type = type_tok.num;
		if (type_tok.str)
			free(type_tok.str);

		list_add(&qs->members, &qsm->node);
	}

	assert(nest == 0);
	free(structs);

	qmi_struct_gen_names(qs, memalloc(1024));
}

struct qmi_struct qmi_response_type_v01 = {
	.name = "qmi_response_type_v01",
	.members = LIST_INIT(qmi_response_type_v01.members),
};

void qmi_parse(void)
{
	struct token tok;

	/* PACKAGE ID<string> ';' */
	/* CONST ID<string> '=' NUM<num> ';' */
	/* STRUCT ID<string> '{' ... '}' ';' */
		/* TYPE<type*> ID<string> ';' */
	/* MESSAGE ID<string> '{' ... '}' ';' */
		/* (REQUIRED | OPTIONAL) TYPE<type*> ID<string> '=' NUM<num> ';' */

	symbol_add("const", TOK_CONST);
	symbol_add("optional", TOK_OPTIONAL);
	symbol_add("message", TOK_MESSAGE, MESSAGE_RESPONSE); /* backward compatible with early hacking */
	symbol_add("request", TOK_MESSAGE, MESSAGE_REQUEST);
	symbol_add("response", TOK_MESSAGE, MESSAGE_RESPONSE);
	symbol_add("indication", TOK_MESSAGE, MESSAGE_INDICATION);
	symbol_add("package", TOK_PACKAGE);
	symbol_add("required", TOK_REQUIRED);
	symbol_add("struct", TOK_STRUCT);
	symbol_add("string", TOK_TYPE, TYPE_STRING);
	symbol_add("u8", TOK_TYPE, TYPE_U8);
	symbol_add("u16", TOK_TYPE, TYPE_U16);
	symbol_add("u32", TOK_TYPE, TYPE_U32);
	symbol_add("u64", TOK_TYPE, TYPE_U64);
	symbol_add("i8", TOK_TYPE, TYPE_I8);
	symbol_add("i16", TOK_TYPE, TYPE_I16);
	symbol_add("i32", TOK_TYPE, TYPE_I32);
	symbol_add("i64", TOK_TYPE, TYPE_I64);
	symbol_add("char", TOK_TYPE, TYPE_CHAR);

	symbol_add(qmi_response_type_v01.name, TOK_TYPE, TYPE_STRUCT, &qmi_response_type_v01);

	token_init();
	while (!token_accept(TOK_EOF, NULL)) {
		if (token_accept(TOK_PACKAGE, NULL)) {
			qmi_package_parse();
		} else if (token_accept(TOK_CONST, NULL)) {
			qmi_const_parse();
		} else if (token_accept(TOK_STRUCT, NULL)) {
			qmi_struct_parse();
		} else if (token_accept(TOK_MESSAGE, &tok)) {
			qmi_message_parse(tok.num);
			free(tok.str);
		} else {
			yyerror("unexpected symbol");
			break;
		}
	}

	/* The package name must have been specified */
	if (!qmi_package)
		yyerror("package not specified");
}
