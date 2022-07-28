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

static struct qmi_package qmi_package;

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
	TOK_PACKAGE_TYPE,
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
			printf("Have symbol: %s\n", sym->name);
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
	// printf("want: %c\n", token_id < 128 ? token_id : token_name(token_id)[0]);
	// printf("have: %c %s\n",
	// 	curr_token.id < 128 ? curr_token.id : token_name(curr_token.id)[0],
	// 	curr_token.str ?: "");

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
	if (want) {
		yyerror("expected %s", want);
	} else {
		yyerror("expected '%c' not '%c'", token_id, curr_token.id);
	}
}

/**
 * @brief: packages of type "server" need to call the _alloc(),
 * _set()  * and _encode() functions on QMI response messages, but
 * usually  * don't need to _parse() them. The opposite is true for
 * requests.
 * 
 * This is all inverted for "clients" who don't need to _alloc()
 * or _encode() responses, only _parse(). And who need to _set() on
 * requests but not on responses.
 * 
 * Specifying a package type will cause only the necessary tlv
 * helpers to be generated and avoid extra bloat.
 * 
 */
static void qmi_package_type_parse(void)
{
	struct token tok;
	static bool parsed = false;

	if (parsed)
		yyerror("type may only be specified once");

	token_expect(TOK_ID, &tok);

	if (!strcmp(tok.str, "server"))
		qmi_package.type = PKG_TYPE_SERVER;
	else if (!strcmp(tok.str, "client"))
		qmi_package.type = PKG_TYPE_CLIENT;
	else if (!strcmp(tok.str, "agnostic"))
		qmi_package.type = PKG_TYPE_AGNOSTIC;

	token_expect(';', NULL);
	parsed = true;
}

static void qmi_package_parse(void)
{
	struct token tok;

	if (qmi_package.name)
		yyerror("package may only be specified once");

	token_expect(TOK_ID, &tok);
	qmi_package.name = tok.str;
	token_expect(';', NULL);
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
	bool array_fixed;
	bool required;

	token_expect(TOK_ID, &msg_id_tok);
	token_expect('{', NULL);

	qm = memalloc(sizeof(struct qmi_message));
	qm->name = msg_id_tok.str;
	qm->type = message_type;
	list_init(&qm->members);

	while (!token_accept('}', NULL)) {
		if (token_accept(TOK_REQUIRED, NULL))
			required = true;
		else if (token_accept(TOK_OPTIONAL, NULL))
			required = false;
		else
			yyerror("expected required, optional or '}'");

		token_expect(TOK_TYPE, &type_tok);
		token_expect(TOK_ID, &id_tok);

		LOGD("Parsing property %s %s\n", sz_simple_types[type_tok.num].name, id_tok.str);

		if (token_accept('[', NULL)) {
			token_expect(TOK_NUM, &num_tok);
			array_size = num_tok.num;
			token_expect(']', NULL);
			array_fixed = true;
		} else if (token_accept('(', NULL)) {
			token_expect(TOK_NUM, &num_tok);
			array_size = num_tok.num;
			token_expect(')', NULL);
			array_fixed = false;
		} else {
			array_size = 0;
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

void qmi_struct_parse_assert_member_unique(struct qmi_struct *qs,
				    const char* member_id)
{
	struct qmi_struct_member *qsm;
	list_for_each_entry(qsm, &qs->members, node)
			if (!strcmp(qsm->name, member_id))
				yyerror("duplicate struct member \"%s\"",
					qsm->name);
}

/**
 * @brief add a struct as a member of a parent struct
 *
 * @qs: struct this @qsm is a member of
 * @qsc: child struct represented by @qsm
 * @id: member name
 */
void qmi_struct_parse_add_struct_member(struct qmi_struct *qs,
					struct qmi_struct *qsc,
					const char* member_id, bool is_ptr)
{
	struct qmi_struct_member *qsm;
	const char *id = member_id ?: qsc->name;
	if (!id) {
		LOGD("struct %s: member struct has no name\n", qs->type ?: qs->name);
		exit(1);
	}

	qmi_struct_parse_assert_member_unique(qs, id);

	qsm = memalloc(sizeof(struct qmi_struct_member));
	qsm->name = id;
	LOGD("member %s\n", qsm->name);
	qsm->type = TYPE_STRUCT;
	qsm->is_ptr = qsc->is_ptr = qsc->is_ptr || is_ptr;

	qsc->name = id;
	qsm->struct_ch = qsc;

	LOGD("adding member struct %s\n", qsc->name);
	list_add(&qs->members, &qsm->node);
}

void qmi_struct_populate_member_struct_types(struct qmi_struct *qs, char *type)
{
	struct qmi_struct_member *qsm;
	char *qs_type;
	int type_len = strlen(type);
	int name_len = strlen(qs->name);
	/* We don't get the name of the current struct if it's nested until now
	 * so we have to iterate over members again to set their types
	 */
	if (strlen(type) + strlen(qs->name) > QMI_STRUCT_TYPE_NAME_MAX) {
		yyerror("autogenerated struct type %s_%s longer than %d", type, qs->type, QMI_STRUCT_TYPE_NAME_MAX);
	}
	type[type_len] = '_';
	type_len++;
	strcpy(type + type_len, qs->name);
	type[type_len + name_len] = '\0'; // gotta terminate those strings!

	qs_type = memalloc(type_len + name_len + 1);
	strcpy(qs_type, type);
	qs->type = qs_type;
	printf("Set type of '%s' to '%s'\n", qs->name, qs->type);

	list_for_each_entry(qsm, &qs->members, node)
			if (qsm->type == TYPE_STRUCT)
				qmi_struct_populate_member_struct_types(qsm->struct_ch, type);
}

/*
 * Some not so obvious rules:
 * The top level struct "name" property is unset.
 * The nested struct "type" property is unset, it will be set
 * later on in the qmi_struct_members_header() function.
 * The type is the type of the parent struct + the name of the struct
 * 
 * The is_ptr property is set on the qmi_struct_member and
 * associated qmi_struct if applicable. This makes accessing easier
 * but isn't reaally ideal.
 * 
 */
static struct qmi_struct *qmi_struct_parse(int nested)
{
	struct qmi_struct_member *qsm;
	char *qsm_struct_type = NULL;
	struct qmi_struct *qsc = NULL;
	struct token struct_id_tok;
	struct qmi_struct *qs;
	struct token type_tok;
	struct token id_tok;
	bool struct_last_member = false;

	qs = memalloc(sizeof(struct qmi_struct));

	if (!nested) {
		token_expect(TOK_ID, &struct_id_tok);
		qs->type = struct_id_tok.str;
		LOGD("Start %s\n", qs->type);
	}

	token_expect('{', NULL);

	list_init(&qs->members);

	while (token_accept(TOK_TYPE, &type_tok) || token_accept(TOK_STRUCT, &type_tok)) {
		bool is_ptr = false;
		qsc = NULL;
		// If this member is referencing a previously defined struct
		// (known because the token_accept will succeed) then don't
		// nest, struct_id_tok will be the type of the struct
		if (!strcmp(type_tok.str, "struct")) {
			if (token_accept(TOK_TYPE, &struct_id_tok)) {
				qsm_struct_type = memalloc(strlen(struct_id_tok.str));
				strcpy(qsm_struct_type, struct_id_tok.str);
				LOGD("%s is struct ptr!\n", qsm_struct_type);
			} else if (token_accept(TOK_ID, &struct_id_tok)) {
				yyerror("struct %s referenced but not defined!", struct_id_tok.str);
			} else {
				if (nested == QMI_STRUCT_NEST_MAX)
					yyerror("Can't nest more than %d structs", QMI_STRUCT_NEST_MAX);
				qsc = qmi_struct_parse(nested + 1);
				//printf("exit nested %s\n", qsc->type);
				qmi_struct_parse_add_struct_member(qs, qsc, NULL, false);
				if (token_accept('}', NULL)) {
					struct_last_member = true;
					break;
				}
				/* we just parsed a member struct, the next
					* token is another type
					*/
				continue;
			}
		}

		if (token_accept('*', NULL))
			is_ptr = true;

		token_expect(TOK_ID, &id_tok);
		token_expect(';', NULL);

		if (qsc) {
			qmi_struct_parse_add_struct_member(qs, qsc, id_tok.str, is_ptr);
			continue;
		}

		qmi_struct_parse_assert_member_unique(qs, id_tok.str);

		qsm = memalloc(sizeof(struct qmi_struct_member));
		qsm->name = id_tok.str;
		LOGD("member %s\n", qsm->name);
		if (qsm_struct_type) {
			qsm->struct_type = qsm_struct_type;
			qsm_struct_type = NULL;
		}
		qsm->type = type_tok.num;
		qsm->is_ptr = is_ptr;
		if (type_tok.str)
			free(type_tok.str);

		list_add(&qs->members, &qsm->node);
	}

	if (!struct_last_member)
		token_expect('}', NULL);

	if (nested) {
		if (token_accept('*', NULL))
			qs->is_ptr = true;
		token_expect(TOK_ID, &struct_id_tok);
		qs->name = struct_id_tok.str;
	}
	token_expect(';', NULL);

	if (!nested) {
		list_add(&qmi_structs, &qs->node);
		symbol_add(qs->type, TOK_TYPE, TYPE_STRUCT, qs);

		// Now we need to recurse a second time to set
		// the nested struct types
		char* type = memalloc(QMI_STRUCT_TYPE_NAME_MAX);
		strcpy(type, qs->type);

		list_for_each_entry(qsm, &qs->members, node)
			if (qsm->type == TYPE_STRUCT)
				qmi_struct_populate_member_struct_types(qsm->struct_ch, type);
	}

	LOGD("Finished (%d) %s\n", nested, qs->type ?: qs->name);
	if (nested)
		return qs;
	
	return NULL;
}

struct qmi_struct qmi_response_type_v01 = {
	.type = "qmi_response_type_v01",
	.members = LIST_INIT(qmi_response_type_v01.members),
};

void qmi_parse(struct qmi_package *out_pkg)
{
	struct token tok;

	/* PACKAGE ID<string>';' */
	/* [PACKAGE_TYPE (SERVER | CLIENT | AGNOSTIC)';'] */
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
	symbol_add("package_type", TOK_PACKAGE_TYPE);
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

	symbol_add("qmi_response_type_v01", TOK_TYPE, TYPE_STRUCT, &qmi_response_type_v01);

	qmi_package.type = PKG_TYPE_AGNOSTIC;

	token_init();
	while (!token_accept(TOK_EOF, NULL)) {
		if (token_accept(TOK_PACKAGE, NULL)) {
			qmi_package_parse();
		} else if (token_accept(TOK_PACKAGE_TYPE, NULL)) {
			qmi_package_type_parse();
		} else if (token_accept(TOK_CONST, NULL)) {
			qmi_const_parse();
		} else if (token_accept(TOK_STRUCT, NULL)) {
			qmi_struct_parse(0);
		} else if (token_accept(TOK_MESSAGE, &tok)) {
			qmi_message_parse(tok.num);
			free(tok.str);
		} else {
			yyerror("unexpected symbol");
			break;
		}
	}

	LOGD("\n\t==\nFinished parsing\n\t==\n\n");

	/* The package name must have been specified */
	if (!qmi_package.name)
		yyerror("package not specified");
	
	out_pkg->name = qmi_package.name;
	out_pkg->type = qmi_package.type;
}
