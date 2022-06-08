#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qmic.h"

struct symbol_type_table {
	const char* type_name;
	int type_sz;
};

static const struct symbol_type_table
/* +1 because
 * "warning: array subscript 5 is above array bounds of ‘const struct symbol_type_table[5]’
 * [-Warray-bounds]"
 * suspect GCC static analysis issue?
 */
symbol_table_lookup[SYMBOL_TYPE_MAX + 1] = {
	[TYPE_U8] = {"uint8_t", 1},
	[TYPE_U16] = {"uint16_t", 2},
	[TYPE_U32] = {"uint32_t", 4},
	[TYPE_U64] = {"uint64_t", 8},
	[TYPE_STRING] = {"char *", -1},
	[TYPE_STRUCT] = {"struct", -1},
};

static void qmi_struct_members_header(FILE *fp,
				const char *package,
				struct qmi_struct *qs,
				char *indent)
{
	struct qmi_struct_member *qsm;

	LOGD("struct %s, indent %ld\n", qs->type, strlen(indent));

	if (indent[0] != '\0')
		fprintf(fp, "%sstruct %s {\n", indent, qs->type);
	else
		fprintf(fp, "struct %s_%s {\n", package, qs->type);

	list_for_each_entry(qsm, &qs->members, node) {
		LOGD("member %s\n", qsm->name);
		if (qsm->struct_ch) {
			LOGD("nested struct %s\n", qsm->struct_ch->name);
			indent[strlen(indent)] = '\t';

			LOGD("%s: type is struct %s\n", qsm->struct_ch->name, qsm->struct_ch->type);

			qmi_struct_members_header(fp, package,
					qsm->struct_ch, indent);
			continue;
		}
		fprintf(fp, "%s\t%s %s%s;\n", indent,
				sz_simple_types[qsm->type],
				qsm->is_ptr ? "*" : "", qsm->name);
	}
	
	if (indent[0] != '\0') {
		fprintf(fp, "%s} %s%s;\n", indent, qs->is_ptr ? "*" : "", qs->name ?: qs->type);
		indent[strlen(indent)-1] = '\0';
	} else {
		fprintf(fp, "};\n\n");
	}
}

static void qmi_struct_header(FILE *fp, const char *package)
{
	struct qmi_struct *qs;
	char* indent = memalloc(QMI_STRUCT_NEST_MAX + 2);

	list_for_each_entry(qs, &qmi_structs, node) {
		qmi_struct_members_header(fp, package, qs, indent);
	}
}

static void qmi_struct_emit_prototype(FILE *fp,
			       const char *package,
			       const char *message,
			       const char *member,
			       unsigned array_size,
			       struct qmi_struct *qs)
{
	if (array_size) {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val, size_t count);\n",
			    package, message, member, qs->type);

		fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count);\n\n",
			    package, message, member, qs->type);
	} else {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val);\n",
			    package, message, member, qs->type);

		fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s);\n\n",
			    package, message, member, qs->type);
	}
}

static bool qmi_struct_has_ptr_members(struct qmi_struct *qs)
{
	struct qmi_struct_member *qsm;
	list_for_each_entry(qsm, &qs->members, node) {
		if (qsm->struct_ch && qmi_struct_has_ptr_members(qsm->struct_ch))
			return true;
		if (qsm->is_ptr)
			return true;
	}

	return false;
}

/* Ensures that member m1 is either
 * <m2_name>_n or <m2_name>_num, fails otherwise
 */
static bool qmi_struct_assert_member_is_len(struct qmi_struct_member *m1, struct qmi_struct_member *m2)
{
	if (!m1) {
		fprintf(stderr, "ERROR: dynamic array not preceded by length member.\n"
				"missing 'u8 %1$s_n;' before member '%1$s'\n",
				m2->name);
		exit(1);
	}
	size_t n = strlen(m1->name);
	size_t n2 = strlen(m2->name);
	if(n - n2 != 2 || strncmp(m1->name, m2->name, n2) || strncmp(m1->name + n2, "_n", 2)) {
		fprintf(stderr, "ERROR: Member before '%1$s' should be '%1$s_n', "
				"got '%2$s'\n", m2->name, m1->name);
		exit(1);
	}

	return true;
}

#define TARGET_VAR_MAX_LEN 512

/**
 * qmi_struct_emit_deserialise() - recursively
 * 	emit code to deserialise nested structs
 * 
 * @fp: output file
 * @package: name of QMI package
 * @target: current object we're setting properties on
 * @indent: nested indent amount
 * @qs: the struct we're deserialising
 */
static void qmi_struct_emit_deserialise(FILE *fp,
			       const char *package,
			       char *target,
			       char *indent,
			       struct qmi_struct *qs)
{
	struct qmi_struct_member *curr, *prev = NULL;
	const struct symbol_type_table *sym;
	int i, old_target_len = strlen(target);
	char iter[QMI_STRUCT_NEST_MAX] = {0};

	PLOGD(strlen(indent) > 1 ? indent : "", "struct %s (%s)\n", qs->type, qs->name);

	for(i = 0; i < strlen(indent); i++)
		iter[i] = 'i';

	list_for_each_entry(curr, &qs->members, node) {
		if (curr->type > SYMBOL_TYPE_MAX) {
			fprintf(stderr, "ERROR: member '%s' is of unsupported type %d\n",
					curr->name, curr->type);
			exit(1);
		}

		sym = &symbol_table_lookup[curr->type];
		PLOGD(indent, "member '%s': %s\n", curr->name, sym->type_name);

		if (curr->is_ptr && qmi_struct_assert_member_is_len(prev, curr)) {
			strcpy(target + strlen(target), curr->name);

			PLOGD(indent, "	new target: '%s'\n", target);

			fprintf(fp, "%1$ssize_t %2$s_sz = ",
				    indent, curr->name);
			if (curr->type == TYPE_STRUCT && curr->struct_ch) {
				fprintf(fp, "sizeof(struct %1$s);\n",
					curr->struct_ch->type);
			} else {
				fprintf(fp, "%1$d;\n", sym->type_sz);
			}
			fprintf(fp, "%1$s%2$s = malloc(%3$s_sz *",
				    indent, target, curr->name);
			// target_len = strlen(target);
			// temp = target[target_len - 2 -i];
			// target[target_len - 2 -i] = '\0';

			fprintf(fp, " %1$s_n);\n", target);
			fprintf(fp, "%1$sfor(size_t %2$s = 0; %2$s < %3$s_n; %2$s++) {\n",
				    indent, iter, target);

			//target[target_len - 2 -i] = temp;

			target[strlen(target)] = '[';
			strncpy(target + strlen(target), iter, i);
			strncpy(target + strlen(target), "]", 2);

			indent[strlen(indent)] = '\t';

			if (curr->type == TYPE_STRUCT && curr->struct_ch) {
				target[strlen(target)] = '.';
				qmi_struct_emit_deserialise(fp, package, target,
					indent, curr->struct_ch);
			} else {
				fprintf(fp, "%1$s%2$s = get_next(%3$s, %4$d);\n",
					indent, target, sym->type_name, sym->type_sz);
				PLOGD(indent, "%s = get_next(%s, %d);\n",
					target, sym->type_name, sym->type_sz);
			}

			indent[strlen(indent)-1] = '\0';
			fprintf(fp, "%1$s}\n", indent);

			memset(target + old_target_len, '\0', TARGET_VAR_MAX_LEN - old_target_len);
		} else {
			/* target is something like "out->cards[0].applications[1]."
			* or just "out->"
			*/
			if (curr->type == TYPE_STRUCT && curr->struct_ch) {
				strcpy(target + strlen(target), curr->name);
				target[strlen(target)] = '.';
				qmi_struct_emit_deserialise(fp, package, target,
					indent, curr->struct_ch);
				memset(target + old_target_len, '\0', TARGET_VAR_MAX_LEN - old_target_len);
			} else {
				fprintf(fp, "%1$s%2$s%3$s = get_next(%4$s, %5$d);\n",
					indent, target, curr->name, sym->type_name, sym->type_sz);
			}
		}

		prev = curr;
	}

	LOGD("Leaving %s, target: %s\n", qs->name ? qs->name : qs->type, target);
}

/**
 * qmi_struct_emit_serialise() - recursively
 * 	emit code to serialise nested structs
 * 
 * @fp: output file
 * @package: name of QMI package
 * @target: current object we're setting properties on
 * @indent: nested indent amount
 * @qs: the struct we're serialising
 */
static void qmi_struct_emit_serialise(FILE *fp,
			       const char *package,
			       char *target,
			       char *indent,
			       struct qmi_struct *qs)
{
	struct qmi_struct_member *curr, *prev = NULL;
	const struct symbol_type_table *sym;
	int i, old_target_len = strlen(target);
	char iter[QMI_STRUCT_NEST_MAX] = {0};

	PLOGD(strlen(indent) > 1 ? indent : "", "struct %s (%s)\n", qs->type, qs->name);

	for(i = 0; i < strlen(indent); i++)
		iter[i] = 'i';

	list_for_each_entry(curr, &qs->members, node) {
		if (curr->type > SYMBOL_TYPE_MAX) {
			fprintf(stderr, "ERROR: member '%s' is of unsupported type %d\n",
					curr->name, curr->type);
			exit(1);
		}

		sym = &symbol_table_lookup[curr->type];
		PLOGD(indent, "member '%s': %s\n", curr->name, sym->type_name);

		if (curr->is_ptr && qmi_struct_assert_member_is_len(prev, curr)) {
			strcpy(target + strlen(target), curr->name);

			PLOGD(indent, "	new target: '%s'\n", target);

			fprintf(fp, "%1$sfor(size_t %2$s = 0; %2$s < %3$s_n; %2$s++) {\n",
				    indent, iter, target);

			//target[target_len - 2 -i] = temp;

			target[strlen(target)] = '[';
			strncpy(target + strlen(target), iter, i);
			strncpy(target + strlen(target), "]", 2);

			indent[strlen(indent)] = '\t';

			if (curr->type == TYPE_STRUCT && curr->struct_ch) {
				target[strlen(target)] = '.';
				qmi_struct_emit_serialise(fp, package, target,
					indent, curr->struct_ch);
			} else {
				fprintf(fp, "%1$s*((%2$s*)(ptr + len)) = %3$s;\n",
					indent, sym->type_name, target);
				fprintf(fp, "%1$slen += %2$d;\n", indent, sym->type_sz);
			}

			indent[strlen(indent)-1] = '\0';
			fprintf(fp, "%1$s}\n", indent);

			memset(target + old_target_len, '\0', TARGET_VAR_MAX_LEN - old_target_len);
		} else {
			/* target is something like "out->cards[0].applications[1]."
			* or just "out->"
			*/
			if (curr->type == TYPE_STRUCT && curr->struct_ch) {
				strcpy(target + strlen(target), curr->name);
				target[strlen(target)] = '.';
				qmi_struct_emit_serialise(fp, package, target,
					indent, curr->struct_ch);
				memset(target + old_target_len, '\0', TARGET_VAR_MAX_LEN - old_target_len);
			} else {
				fprintf(fp, "%1$s*((%2$s*)(ptr + len)) = %3$s%4$s;\n",
					indent, sym->type_name, target, curr->name);
				fprintf(fp, "%1$slen += %2$d;\n", indent, sym->type_sz);
			}
		}

		prev = curr;
	}

	LOGD("Leaving %s, target: %s\n", qs->name ? qs->name : qs->type, target);
}

static void qmi_struct_emit_accessors(FILE *fp,
			       const char *package,
			       const char *message,
			       const char *member,
			       int member_id,
			       unsigned array_size,
			       struct qmi_struct *qs)
{
	char *indent, *target;
	if (array_size) {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val, size_t count)\n"
			    "{\n"
			    "	return qmi_tlv_set_array((struct qmi_tlv*)%2$s, %5$d, %6$d, val, count, sizeof(struct %1$s_%4$s));\n"
			    "}\n\n",
			    package, message, member, qs->type, member_id, array_size);

		fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count)\n"
			    "{\n"
			    "	size_t size;\n"
			    "	size_t len;\n"
			    "	void *ptr;\n"
			    "\n"
			    "	ptr = qmi_tlv_get_array((struct qmi_tlv*)%2$s, %5$d, %6$d, &len, &size);\n"
			    "	if (!ptr)\n"
			    "		return NULL;\n"
			    "\n"
			    "	if (size != sizeof(struct %1$s_%4$s))\n"
			    "		return NULL;\n"
			    "\n"
			    "	*count = len;\n"
			    "	return ptr;\n"
			    "}\n\n",
			    package, message, member, qs->type, member_id, array_size);
	} else if (qmi_struct_has_ptr_members(qs)) {
		indent = memalloc(QMI_STRUCT_NEST_MAX + 2);
		indent[0] = '\t';
		target = memalloc(TARGET_VAR_MAX_LEN);

		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val)\n"
			    "{\n"
			    "	size_t len = 0;\n"
			    "	// FIXME: use realloc dynamically instead\n"
			    "	void *ptr = malloc(1024);\n"
			    "	memset(ptr, 0, 1014);\n",
			    package, message, member, qs->type);
		strncpy(target, "val->", 6);

		qmi_struct_emit_serialise(fp, package, target, indent, qs);

		fprintf(fp, "	return qmi_tlv_set((struct qmi_tlv*)%1$s, %2$d, ptr, len);\n"
			    "}\n\n",
			    message, member_id);

		fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s)\n"
		    "{\n"
		    "	size_t len = 0, buf_sz;\n"
		    "	uint8_t *ptr;\n"
		    "	struct %1$s_%4$s *out;\n"
		    "\n"
		    "	ptr = qmi_tlv_get((struct qmi_tlv*)%2$s, %5$d, &buf_sz);\n"
		    "	if (!ptr)\n"
		    "		return NULL;\n"
		    "\n"
		    "	out = malloc(sizeof(struct %1$s_%4$s));\n",
		package, message, member, qs->type, member_id);

		memset(indent, '\0', QMI_STRUCT_NEST_MAX + 2);
		indent[0] = '\t';
		memset(target, '\0', TARGET_VAR_MAX_LEN);
		strncpy(target, "out->", 6);

		qmi_struct_emit_deserialise(fp, package, target, indent, qs);

		fprintf(fp, "\n"
			    "	return out;\n\n"
			    "err_wrong_len:\n"
			    "	printf(\"%%s: expected at least %%zu bytes but got %%zu\\n\", __func__, len, buf_sz);\n"
			    "	free(out);\n"
			    "	return NULL;\n"
			    "}\n\n");
	} else {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val)\n"
			    "{\n"
			    "	return qmi_tlv_set((struct qmi_tlv*)%2$s, %5$d, val, sizeof(struct %1$s_%4$s));\n"
			    "}\n\n",
			    package, message, member, qs->type, member_id);

		fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s)\n"
			    "{\n"
			    "	size_t len;\n"
			    "	void *ptr;\n"
			    "\n"
			    "	ptr = qmi_tlv_get((struct qmi_tlv*)%2$s, %5$d, &len);\n"
			    "	if (!ptr)\n"
			    "		return NULL;\n"
			    "\n"
			    "	if (len != sizeof(struct %1$s_%4$s))\n"
			    "		return NULL;\n"
			    "\n"
			    "	return ptr;\n"
			    "}\n\n",
			    package, message, member, qs->type, member_id);
	}
}

static void qmi_message_emit_message_type(FILE *fp,
					  const char *package,
					  const char *message)
{
	fprintf(fp, "struct %s_%s;\n", package, message);
}

static void qmi_message_emit_message_prototype(FILE *fp,
					       const char *package,
					       const char *message)
{
	fprintf(fp, "/*\n"
		    " * %1$s_%2$s message\n"
		    " */\n",
		    package, message);

	fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_alloc(unsigned txn);\n",
		    package, message);

	fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_parse(void *buf, size_t len, unsigned *txn);\n",
		    package, message);

	fprintf(fp, "void *%1$s_%2$s_encode(struct %1$s_%2$s *%2$s, size_t *len);\n",
		    package, message);

	fprintf(fp, "void %1$s_%2$s_free(struct %1$s_%2$s *%2$s);\n\n",
		    package, message);
}

static void qmi_message_emit_message(FILE *fp,
				     const char *package,
				     struct qmi_message *qm)
{
	fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_alloc(unsigned txn)\n"
		    "{\n"
		    "	return (struct %1$s_%2$s*)qmi_tlv_init(txn, %3$d, %4$d);\n"
		    "}\n\n",
		    package, qm->name, qm->msg_id, qm->type);

	fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_parse(void *buf, size_t len, unsigned *txn)\n"
		    "{\n"
		    "	return (struct %1$s_%2$s*)qmi_tlv_decode(buf, len, txn, %3$d);\n"
		    "}\n\n",
		    package, qm->name, qm->type);

	fprintf(fp, "void *%1$s_%2$s_encode(struct %1$s_%2$s *%2$s, size_t *len)\n"
		    "{\n"
		    "	return qmi_tlv_encode((struct qmi_tlv*)%2$s, len);\n"
		    "}\n\n",
		    package, qm->name);

	fprintf(fp, "void %1$s_%2$s_free(struct %1$s_%2$s *%2$s)\n"
		    "{\n"
		    "	qmi_tlv_free((struct qmi_tlv*)%2$s);\n"
		    "}\n\n",
		    package, qm->name);
}

static void qmi_message_emit_simple_prototype(FILE *fp,
					      const char *package,
					      const char *message,
					      struct qmi_message_member *qmm)
{
	if (qmm->array_size) {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s *val, size_t count);\n",
			    package, message, qmm->name, sz_simple_types[qmm->type]);

		fprintf(fp, "%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count);\n\n",
			    package, message, qmm->name, sz_simple_types[qmm->type]);
	} else {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s val);\n",
			    package, message, qmm->name, sz_simple_types[qmm->type]);

		fprintf(fp, "int %1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, %4$s *val);\n\n",
			    package, message, qmm->name, sz_simple_types[qmm->type]);
	}
}


static void qmi_message_emit_simple_accessors(FILE *fp,
					      const char *package,
					      const char *message,
					      struct qmi_message_member *qmm)
{
	if (qmm->array_size) {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s *val, size_t count)\n"
			    "{\n"
			    "	return qmi_tlv_set_array((struct qmi_tlv*)%2$s, %5$d, %6$d, val, count, sizeof(%4$s));\n"
			    "}\n\n",
			    package, message, qmm->name, sz_simple_types[qmm->type], qmm->id, qmm->array_size);

		fprintf(fp, "%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count)\n"
			    "{\n"
			    "	%4$s *ptr;\n"
			    "	size_t size;\n"
			    "	size_t len;\n"
			    "\n"
			    "	ptr = qmi_tlv_get_array((struct qmi_tlv*)%2$s, %5$d, %6$d, &len, &size);\n"
			    "	if (!ptr)\n"
			    "		return NULL;\n"
			    "\n"
			    "	if (size != sizeof(%4$s))\n"
			    "		return NULL;\n"
			    "\n"
			    "	*count = len;\n"
			    "	return ptr;\n"
			    "}\n\n",
			    package, message, qmm->name, sz_simple_types[qmm->type], qmm->id, qmm->array_size);
	} else {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s val)\n"
			    "{\n"
			    "	return qmi_tlv_set((struct qmi_tlv*)%2$s, %5$d, &val, sizeof(%4$s));\n"
			    "}\n\n",
			    package, message, qmm->name, sz_simple_types[qmm->type], qmm->id);

		fprintf(fp, "int %1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, %4$s *val)\n"
			    "{\n"
			    "	%4$s *ptr;\n"
			    "	size_t len;\n"
			    "\n"
			    "	ptr = qmi_tlv_get((struct qmi_tlv*)%2$s, %5$d, &len);\n"
			    "	if (!ptr)\n"
			    "		return -ENOENT;\n"
			    "\n"
			    "	if (len != sizeof(%4$s))\n"
			    "		return -EINVAL;\n"
			    "\n"
			    "	*val = *(%4$s*)ptr;\n"
			    "	return 0;\n"
			    "}\n\n",
			    package, message, qmm->name, sz_simple_types[qmm->type], qmm->id);
	}
}

static void qmi_message_emit_string_prototype(FILE *fp,
					      const char *package,
					      const char *message,
					      struct qmi_message_member *qmm)
{
	if (qmm->array_size) {
		fprintf(stderr, "Dont' know how to encode string arrays yet");
		exit(1);
	} else {
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, char *buf, size_t len);\n",
			    package, message, qmm->name);

		fprintf(fp, "int %1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, char *buf, size_t buflen);\n\n",
			    package, message, qmm->name);
	}
}

static void qmi_message_emit_string_accessors(FILE *fp,
					      const char *package,
					      const char *message,
					      struct qmi_message_member *qmm)
{
	fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, char *buf, size_t len)\n"
		    "{\n"
		    "	return qmi_tlv_set((struct qmi_tlv*)%2$s, %4$d, buf, len);\n"
		    "}\n\n",
		    package, message, qmm->name, qmm->id);

	fprintf(fp, "int %1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, char *buf, size_t buflen)\n"
		    "{\n"
		    "	size_t len;\n"
		    "	char *ptr;\n"
		    "\n"
		    "	ptr = qmi_tlv_get((struct qmi_tlv*)%2$s, %4$d, &len);\n"
		    "	if (!ptr)\n"
		    "		return -ENOENT;\n"
		    "\n"
		    "	if (len >= buflen)\n"
		    "		return -ENOMEM;\n"
		    "\n"
		    "	memcpy(buf, ptr, len);\n"
		    "	buf[len] = '\\0';\n"
		    "	return len;\n"
		    "}\n\n",
		    package, message, qmm->name, qmm->id);

}

static void qmi_message_source(FILE *fp, const char *package)
{
	struct qmi_message_member *qmm;
	struct qmi_message *qm;

	list_for_each_entry(qm, &qmi_messages, node) {
		qmi_message_emit_message(fp, package, qm);

		list_for_each_entry(qmm, &qm->members, node) {
			switch (qmm->type) {
			case TYPE_U8:
			case TYPE_U16:
			case TYPE_U32:
			case TYPE_U64:
				qmi_message_emit_simple_accessors(fp, package, qm->name, qmm);
				break;
			case TYPE_STRING:
				qmi_message_emit_string_accessors(fp, package, qm->name, qmm);
				break;
			case TYPE_STRUCT:
				qmi_struct_emit_accessors(fp, package, qm->name, qmm->name, qmm->id, qmm->array_size, qmm->qmi_struct);
				break;
			};
		}
	}
}

static void qmi_message_header(FILE *fp, const char *package)
{
	struct qmi_message_member *qmm;
	struct qmi_message *qm;

	list_for_each_entry(qm, &qmi_messages, node)
		qmi_message_emit_message_type(fp, package, qm->name);

	fprintf(fp, "\n");

	list_for_each_entry(qm, &qmi_messages, node) {
		qmi_message_emit_message_prototype(fp, package, qm->name);

		list_for_each_entry(qmm, &qm->members, node) {
			switch (qmm->type) {
			case TYPE_U8:
			case TYPE_U16:
			case TYPE_U32:
			case TYPE_U64:
				qmi_message_emit_simple_prototype(fp, package, qm->name, qmm);
				break;
			case TYPE_STRING:
				qmi_message_emit_string_prototype(fp, package, qm->name, qmm);
				break;
			case TYPE_STRUCT:
				qmi_struct_emit_prototype(fp, package, qm->name, qmm->name, qmm->array_size, qmm->qmi_struct);
				break;
			};
		}
	}
}

static void emit_header_file_header(FILE *fp)
{
	fprintf(fp, "#include <stdint.h>\n"
		    "#include <stddef.h>\n"
		    "#include <stdio.h>\n"
		    "#include <stdlib.h>\n\n");
	fprintf(fp, "#define get_next(_type, _sz) ({ \\\n"
		    "	void* buf = ptr + len; \\\n"
		    "	len += _sz; \\\n"
		    "	if (len > buf_sz) goto err_wrong_len; \\\n"
		    "	*(_type*)buf; \\\n"
		    "})\n\n");
	fprintf(fp, "struct qmi_tlv;\n"
		    "\n"
		    "struct qmi_tlv *qmi_tlv_init(unsigned txn, unsigned msg_id, unsigned type);\n"
		    "struct qmi_tlv *qmi_tlv_decode(void *buf, size_t len, unsigned *txn, unsigned type);\n"
		    "void *qmi_tlv_encode(struct qmi_tlv *tlv, size_t *len);\n"
		    "void qmi_tlv_free(struct qmi_tlv *tlv);\n"
		    "\n"
		    "void *qmi_tlv_get(struct qmi_tlv *tlv, unsigned id, size_t *len);\n"
		    "void *qmi_tlv_get_array(struct qmi_tlv *tlv, unsigned id, unsigned len_size, size_t *len, size_t *size);\n"
		    "int qmi_tlv_set(struct qmi_tlv *tlv, unsigned id, void *buf, size_t len);\n"
		    "int qmi_tlv_set_array(struct qmi_tlv *tlv, unsigned id, unsigned len_size, void *buf, size_t len, size_t size);\n"
		    "\n");
}

void accessor_emit_c(FILE *fp, const char *package)
{
	emit_source_includes(fp, package);
	qmi_message_source(fp, package);
}
	
void accessor_emit_h(FILE *fp, const char *package)
{
	guard_header(fp, qmi_package);
	emit_header_file_header(fp);
	qmi_const_header(fp);
	qmi_struct_header(fp, qmi_package);
	qmi_message_header(fp, qmi_package);
	guard_footer(fp);
	LOGD("\n\t==\nEmitted headers\n\t==\n\n");
}
