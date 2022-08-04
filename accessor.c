#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qmic.h"

/**
 * @brief: Checks to see if builder helpers (_alloc(), _encode(), _set()) should be
 * emitted for this qmi message
 * 
 * @pkg_type: The type of package this IDL file is for (server/client/agnostic)
 * @qm: QMI message to check
 * 
 * If a client package, then emit builders for requests, if server then emit builders
 * for responses.
 */
static inline bool should_emit_builder(enum package_type pkg_type, const struct qmi_message *qm)
{
	if (pkg_type == PKG_TYPE_AGNOSTIC || qm->type == MESSAGE_INDICATION)
		return true;
	if (pkg_type == PKG_TYPE_CLIENT && qm->type == MESSAGE_REQUEST)
		return true;
	if (pkg_type == PKG_TYPE_SERVER && qm->type == MESSAGE_RESPONSE)
		return true;

	return false;
}

// For agnostic packages or indication messages emit everything
static inline bool should_emit_parser(enum package_type pkg_type, const struct qmi_message *qm)
{
	if (pkg_type == PKG_TYPE_AGNOSTIC || qm->type == MESSAGE_INDICATION)
		return true;

	return !should_emit_builder(pkg_type, qm);
}

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
		} else if (qsm->struct_type) {
			LOGD("struct pointer to %s\n", qsm->struct_type);
			fprintf(fp, "%s\tstruct %s_%s %s%s;\n", indent,
				package, qsm->struct_type,
				qsm->is_ptr ? "*" : "", qsm->name);
			continue;
		}
		fprintf(fp, "%s\t%s %s%s", indent,
				sz_simple_types[qsm->type].name,
				qsm->is_ptr ? "*" : "", qsm->name);

		if (qsm->array_size) {
			fprintf(fp, "[%u]", qsm->array_size);
		}

		fprintf(fp, ";\n");
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
			       const struct qmi_package package,
			       const struct qmi_message *qm,
			       const char *member,
			       unsigned array_size,
			       struct qmi_struct *qs)
{
	if (array_size) {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val, size_t count);\n",
				    package.name, qm->name, member, qs->type);
		if (should_emit_parser(package.type, qm))
			fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count);\n\n",
				    package.name, qm->name, member, qs->type);
	} else {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val);\n",
				    package.name, qm->name, member, qs->type);
		if (should_emit_parser(package.type, qm)) {
			fprintf(fp, "struct %1$s_%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s);\n",
				    package.name, qm->name, member, qs->type);
			fprintf(fp, "void %1$s_%2$s_free(struct %1$s_%2$s *val);\n",
				package.name, qs->type);
		}
	}

	fprintf(fp, "\n");
}

static bool qmi_struct_has_ptr_members(struct qmi_struct *qs)
{
	struct qmi_struct_member *qsm;
	if (qs->has_ptr_members)
		return true;
	list_for_each_entry(qsm, &qs->members, node) {
		if (qsm->struct_ch && qmi_struct_has_ptr_members(qsm->struct_ch))
			goto out_true;
		if (qsm->is_ptr)
			goto out_true;
		if (qsm->type == TYPE_STRING)
			goto out_true;
	}

	return false;

out_true:
	if (qsm && qsm->struct_ch)
		qsm->struct_ch->has_ptr_members = true;
	qs->has_ptr_members = true;
	return qs->has_ptr_members;
}

/* Ensures that member m1 is either
 * <m2_name>_n, fails otherwise
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

		sym = &sz_simple_types[curr->type];
		PLOGD(indent, "member '%s': %s\n", curr->name, sym->name);

		if (curr->is_ptr && curr->type != TYPE_STRING && qmi_struct_assert_member_is_len(prev, curr)) {
			strcpy(target + strlen(target), curr->name);

			PLOGD(indent, "	new target: '%s'\n", target);

			fprintf(fp, "%1$ssize_t %2$s_sz = ",
				    indent, curr->name);
			if (curr->type == TYPE_STRUCT && curr->struct_ch) {
				fprintf(fp, "sizeof(struct %1$s);\n",
					curr->struct_ch->type);
			} else {
				fprintf(fp, "%1$d;\n", sym->size);
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
					indent, target, sym->name, sym->size);
				PLOGD(indent, "%s = get_next(%s, %d);\n",
					target, sym->name, sym->size);
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
			} else if (curr->type == TYPE_STRING) {
				// FIXME: yeah this is pretty janky...
				// Also I think not all QMI messages bother with null terminated strings
				// Maybe that's only when the string IS a TLV
				fprintf(fp, "%1$s%2$s%3$s = malloc(strlen(ptr + len));\n",
					indent, target, curr->name);
				fprintf(fp, "%1$sstrcpy(%2$s%3$s, ptr + len); len += strlen(ptr + len);\n",
					indent, target, curr->name);
			} else {
				fprintf(fp, "%1$s%2$s%3$s = get_next(%4$s, %5$d);\n",
					indent, target, curr->name, sym->name, sym->size);
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

		sym = &sz_simple_types[curr->type];
		PLOGD(indent, "member '%s': %s\n", curr->name, sym->name);

		if (curr->is_ptr && curr->type != TYPE_STRING && qmi_struct_assert_member_is_len(prev, curr)) {
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
					indent, sym->name, target);
				fprintf(fp, "%1$slen += %2$d;\n", indent, sym->size);
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
			} else if (curr->type == TYPE_STRING) {
				fprintf(fp, "%1$sstrcpy(ptr + len, %2$s%3$s);\n",
					indent, target, curr->name);
				fprintf(fp, "%1$slen += strlen(%2$s%3$s);\n", indent, target, curr->name);
			} else {
				fprintf(fp, "%1$s*((%2$s*)(ptr + len)) = %3$s%4$s;\n",
					indent, sym->name, target, curr->name);
				fprintf(fp, "%1$slen += %2$d;\n", indent, sym->size);
			}
		}

		prev = curr;
	}

	LOGD("Leaving %s, target: %s\n", qs->name ? qs->name : qs->type, target);
}

/**
 * qmi_struct_emit_free() - recursively
 * 	emit code to free nested structs
 * 
 * @fp: output file
 * @package: name of QMI package
 * @target: current object we're setting properties on
 * @qs: the struct we're freeing
 */
static void qmi_struct_emit_free_recurse(FILE *fp,
			       const char *package,
			       char *indent,
			       char *target,
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

		if (!curr->is_ptr && curr->type != TYPE_STRUCT && curr->type != TYPE_STRING)
			continue;

		sym = &sz_simple_types[curr->type];
		PLOGD(indent, "member '%s': %s\n", curr->name, sym->name);

		// Pointer to a previously defined struct
		if (curr->struct_type && curr->is_ptr) {
			fprintf(stderr, "pointers to other structs unsupported\n");
			exit(1);
			fprintf(fp, "%1$s_%2$s_free(%3$s);\n",
				package, curr->struct_type, curr->name);
		} else if (curr->type == TYPE_STRUCT
				&& qmi_struct_has_ptr_members(curr->struct_ch)) {
			strcpy(target + strlen(target), curr->name);

			PLOGD(indent, "	new target: '%s'\n", target);

			// target_len = strlen(target);
			// temp = target[target_len - 2 -i];
			// target[target_len - 2 -i] = '\0';

			fprintf(fp, "%1$sfor(size_t %2$s = 0; %2$s < %3$s_n; %2$s++) {\n",
				    indent, iter, target);

			//target[target_len - 2 -i] = temp;

			target[strlen(target)] = '[';
			strncpy(target + strlen(target), iter, i);
			strncpy(target + strlen(target), "]", 2);

			indent[strlen(indent)] = '\t';

			target[strlen(target)] = '.';
			qmi_struct_emit_free_recurse(fp, package,
				indent, target, curr->struct_ch);

			indent[strlen(indent)-1] = '\0';
			fprintf(fp, "%1$s}\n", indent);

			memset(target + old_target_len, '\0', TARGET_VAR_MAX_LEN - old_target_len);
		}

		if (curr->is_ptr) {
			fprintf(fp, "%1$sfree(%2$s%3$s);\n",
				    indent, target, curr->name);
		}

		prev = curr;
	}

	// if (strlen(indent) > 1)
	// 	fprintf(fp, "%1$sfree(%2$s%3$s);\n",
	// 			    indent, target, qs->name);


	LOGD("Leaving %s, target: %s\n", qs->name ? qs->name : qs->type, target);
}

static void qmi_struct_emit_free(FILE *fp,
			       const struct qmi_package package,
			       struct qmi_struct *qs)
{
	char *indent, *target;

	indent = memalloc(QMI_STRUCT_NEST_MAX + 2);
	indent[0] = '\t';
	target = memalloc(TARGET_VAR_MAX_LEN);

	printf("\n\nEmitting struct free: %s\n", qs->type);
	fprintf(fp, "void %1$s_%2$s_free(struct %1$s_%2$s *val)\n"
		    "{\n",
		package.name, qs->type);

	strncpy(target, "val->", 6);

	printf("\n\nEMIT FREE FOR STRUCT: %s\n", qs->type);
	qmi_struct_emit_free_recurse(fp, package.name, indent, target, qs);

	fprintf(fp, "\n"
		    "}\n\n");

	free(indent);
	free(target);
}

static void qmi_struct_emit_accessors(FILE *fp,
			       const struct qmi_package package,
			       const struct qmi_message *qm,
			       const char *member,
			       int member_id,
			       unsigned array_size,
			       struct qmi_struct *qs)
{
	char *indent, *target;
	if (array_size) {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val, size_t count)\n"
				    "{\n"
				    "	return qmi_tlv_set_array((struct qmi_tlv*)%2$s, %5$d, %6$d, val, count, sizeof(struct %1$s_%4$s));\n"
				    "}\n\n",
				    package.name, qm->name, member, qs->type, member_id, array_size);
		if (should_emit_parser(package.type, qm))
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
				    package.name, qm->name, member, qs->type, member_id, array_size);
	} else if (qmi_struct_has_ptr_members(qs)) {
		indent = memalloc(QMI_STRUCT_NEST_MAX + 2);
		indent[0] = '\t';
		target = memalloc(TARGET_VAR_MAX_LEN);

		if (should_emit_builder(package.type, qm)) {
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val)\n"
				    "{\n"
				    "	size_t len = 0;\n"
				    "	int rc;\n"
				    "	// FIXME: use realloc dynamically instead\n"
				    "	void *ptr = malloc(1024);\n"
				    "	memset(ptr, 0, 1024);\n",
				    package.name, qm->name, member, qs->type);
			strncpy(target, "val->", 6);

			qmi_struct_emit_serialise(fp, package.name, target, indent, qs);

			fprintf(fp, "	rc = qmi_tlv_set((struct qmi_tlv*)%1$s, %2$d, ptr, len);\n"
				"	free(ptr);\n"
				"	return rc;\n"
				"}\n\n",
				qm->name, member_id);
		}
		if (should_emit_parser(package.type, qm)) {
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
			package.name, qm->name, member, qs->type, member_id);

			memset(indent, '\0', QMI_STRUCT_NEST_MAX + 2);
			indent[0] = '\t';
			memset(target, '\0', TARGET_VAR_MAX_LEN);
			strncpy(target, "out->", 6);

			qmi_struct_emit_deserialise(fp, package.name, target, indent, qs);

			fprintf(fp, "\n"
				"	return out;\n\n"
				"err_wrong_len:\n"
				"	printf(\"%%s: expected at least %%zu bytes but got %%zu\\n\", __func__, len, buf_sz);\n"
				"	free(out);\n"
				"	return NULL;\n"
				"}\n\n");
		}

		free(indent);
		free(target);
	} else {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, struct %1$s_%4$s *val)\n"
				"{\n"
				"	return qmi_tlv_set((struct qmi_tlv*)%2$s, %5$d, val, sizeof(struct %1$s_%4$s));\n"
				"}\n\n",
				package.name, qm->name, member, qs->type, member_id);

		if (should_emit_parser(package.type, qm))
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
				package.name, qm->name, member, qs->type, member_id);
	}
}

static void qmi_message_emit_message_data_struct(FILE *fp,
					       const struct qmi_package package,
					       struct qmi_message *qm)
{
	struct qmi_message_member *qmm;
	const struct symbol_type_table *sym;

	fprintf(fp, "\n"
		    "struct %1$s_%2$s_data {\n",
		    package.name, qm->name);

	list_for_each_entry(qmm, &qm->members, node) {
		if (qmm->type > SYMBOL_TYPE_MAX) {
			fprintf(stderr, "ERROR: member '%s' is of unsupported type %d\n",
					qmm->name, qmm->type);
			exit(1);
		}

		sym = &sz_simple_types[qmm->type];
		if (!qmm->required)
			fprintf(fp, "\tbool %1$s_valid;\n",
				qmm->name);
		if (qmm->array_size)
			fprintf(fp, "\tsize_t %1$s_n;\n",
				qmm->name);

		// result structs are special
		if (qmm->type == TYPE_STRUCT && qmm->id == 0x2)
			fprintf(fp, "\t%1$s %2$s *%3$s;\n",
				sym->name, qmm->qmi_struct->type,
				qmm->name);
		else if (qmm->type == TYPE_STRUCT)
			fprintf(fp, "\t%2$s %1$s_%3$s *%4$s;\n",
				package.name, sym->name, qmm->qmi_struct->type,
				qmm->name);
		else
			fprintf(fp, "\t%1$s %2$s%3$s;\n",
				sym->name, ((qmm->array_size || qmm->type == TYPE_STRING) ? "*" : ""), qmm->name);
	}

	fprintf(fp, "};\n\n");
}

static void qmi_message_emit_message_data_getall(FILE *fp,
				     const struct qmi_package package,
				     struct qmi_message *qm)
{
	struct qmi_message_member *qmm;

	fprintf(fp, "void %1$s_%2$s_getall(struct %1$s_%2$s *%2$s, struct %1$s_%2$s_data *data)\n"
		    "{\n"
		    "	int rc;\n"
		    "	(void)rc;\n\n",
		package.name, qm->name);

	list_for_each_entry(qmm, &qm->members, node) {
		if (qmm->array_size)
			fprintf(fp, "\tdata->%3$s = %1$s_%2$s_get_%3$s(%2$s, &data->%3$s_n);\n",
				package.name, qm->name, qmm->name);
		else if (qmm->type == TYPE_STRING)
			fprintf(fp, "\tdata->%3$s = %1$s_%2$s_get_%3$s(%2$s);\n",
				package.name, qm->name, qmm->name);
		else if (qmm->type == TYPE_STRUCT && qmm->id == 0x2) {
			fprintf(fp, "\tdata->%1$s = malloc(sizeof(struct qmi_response_type_v01));\n",
				qmm->name);
			fprintf(fp, "\tmemcpy(data->%2$s, qmi_tlv_get((struct qmi_tlv*)%1$s, %3$d, NULL), sizeof(struct qmi_response_type_v01));\n",
				qm->name, qmm->name, qmm->id);
		} else if (qmm->type == TYPE_STRUCT)
			fprintf(fp, "\tdata->%3$s = %1$s_%2$s_get_%3$s(%2$s);\n",
				package.name, qm->name, qmm->name);
		else
			fprintf(fp, "\trc = %1$s_%2$s_get_%3$s(%2$s, &data->%3$s);\n",
				package.name, qm->name, qmm->name);

		if (!qmm->required && (qmm->type == TYPE_STRING
				    || qmm->type == TYPE_STRUCT)) {
			fprintf(fp, "\tdata->%1$s_valid = !!data->%1$s;\n",
				qmm->name);
		} else if (!qmm->required && qmm->array_size) {
			fprintf(fp, "\tdata->%1$s_valid = !!data->%1$s_n;\n",
				qmm->name);
		} else if (!qmm->required) {
			fprintf(fp, "\tdata->%1$s_valid = rc >= 0;\n",
				qmm->name);
		}
	}

	fprintf(fp, "}\n\n");
}

static void qmi_message_emit_message_data_free(FILE *fp,
				     const struct qmi_package package,
				     struct qmi_message *qm)
{
	struct qmi_message_member *qmm;

	fprintf(fp, "void %1$s_%2$s_data_free(struct %1$s_%2$s_data *data)\n"
		    "{\n\n",
		package.name, qm->name);

	list_for_each_entry(qmm, &qm->members, node) {
		fprintf(fp, "\tif(data->%1$s_valid) {\n",
			qmm->name);
		if (qmm->type == TYPE_STRUCT) {
			if (qmi_struct_has_ptr_members(qmm->qmi_struct)) {
				fprintf(fp, "\t\t%1$s_%2$s_free(data->%3$s);\n",
				package.name, qmm->qmi_struct->type, qmm->name);
			}
			fprintf(fp, "\t\tfree(data->%1$s);\n", qmm->name);
		} else if (qmm->array_size || qmm->type == TYPE_STRING) {
			fprintf(fp, "\t\tfree(data->%1$s);\n", qmm->name);
		}
		fprintf(fp, "\t}\n");
	}

	fprintf(fp, "}\n\n");
}

static void qmi_message_emit_message_prototype(FILE *fp,
					       const struct qmi_package package,
					       struct qmi_message *qm)
{
	fprintf(fp, "/*\n"
		    " * %1$s_%2$s message\n"
		    " */\n",
		    package.name, qm->name);

	// NOTE: conditional inverted to emit components in a nicer order
	if (should_emit_parser(package.type, qm)) {
		qmi_message_emit_message_data_struct(fp, package, qm);
		fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_parse(void *buf, size_t len);\n",
			    package.name, qm->name);
		fprintf(fp, "void %1$s_%2$s_getall(struct %1$s_%2$s *%2$s, struct %1$s_%2$s_data *data);\n",
			    package.name, qm->name);
		fprintf(fp, "void %1$s_%2$s_data_free(struct %1$s_%2$s_data *data);\n",
			    package.name, qm->name);
	}
	if (should_emit_builder(package.type, qm)) {
		fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_alloc(unsigned txn);\n",
			package.name, qm->name);
		fprintf(fp, "void *%1$s_%2$s_encode(struct %1$s_%2$s *%2$s, size_t *len);\n",
			    package.name, qm->name);
	}

	fprintf(fp, "void %1$s_%2$s_free(struct %1$s_%2$s *%2$s);\n\n",
		    package.name, qm->name);
}

static void qmi_message_emit_message(FILE *fp,
				     const struct qmi_package package,
				     struct qmi_message *qm)
{
	if (should_emit_builder(package.type, qm)) {
		fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_alloc(unsigned txn)\n"
			"{\n"
			"	return (struct %1$s_%2$s*)qmi_tlv_init(txn, %3$d, %4$d);\n"
			"}\n\n",
			package.name, qm->name, qm->msg_id, qm->type);

		fprintf(fp, "void *%1$s_%2$s_encode(struct %1$s_%2$s *%2$s, size_t *len)\n"
			"{\n"
			"	return qmi_tlv_encode((struct qmi_tlv*)%2$s, len);\n"
			"}\n\n",
			package.name, qm->name);
	}
	if (should_emit_parser(package.type, qm)) {
		fprintf(fp, "struct %1$s_%2$s *%1$s_%2$s_parse(void *buf, size_t len)\n"
			"{\n"
			"	return (struct %1$s_%2$s*)qmi_tlv_decode(buf, len);\n"
			"}\n\n",
			package.name, qm->name);
		qmi_message_emit_message_data_getall(fp, package, qm);
		qmi_message_emit_message_data_free(fp, package, qm);
	}

	fprintf(fp, "void %1$s_%2$s_free(struct %1$s_%2$s *%2$s)\n"
		    "{\n"
		    "	qmi_tlv_free((struct qmi_tlv*)%2$s);\n"
		    "}\n\n",
		    package.name, qm->name);
}

static void qmi_message_emit_simple_prototype(FILE *fp,
					      const struct qmi_package package,
					      const struct qmi_message *qm,
					      struct qmi_message_member *qmm)
{
	if (qmm->array_size) {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s *val, size_t count);\n",
				package.name, qm->name, qmm->name, sz_simple_types[qmm->type].name);
		if (should_emit_parser(package.type, qm))
			fprintf(fp, "%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count);\n\n",
				    package.name, qm->name, qmm->name, sz_simple_types[qmm->type].name);
	} else {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s val);\n",
					package.name, qm->name, qmm->name, sz_simple_types[qmm->type].name);
		if (should_emit_parser(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, %4$s *val);\n\n",
					package.name, qm->name, qmm->name, sz_simple_types[qmm->type].name);
	}
}


static void qmi_message_emit_simple_accessors(FILE *fp,
					      const struct qmi_package package,
					      const struct qmi_message *qm,
					      struct qmi_message_member *qmm)
{
	const struct symbol_type_table *type = &sz_simple_types[qmm->type];
	if (qmm->array_size) {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s *val, size_t count)\n"
				    "{\n"
				    "	return qmi_tlv_set_array((struct qmi_tlv*)%2$s, %5$d, %6$d, val, count, sizeof(%4$s));\n"
				    "}\n\n",
				    package.name, qm->name, qmm->name, type->name, qmm->id, type->size);
		if (should_emit_parser(package.type, qm))
			fprintf(fp, "%4$s *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s, size_t *count)\n"
				    "{\n"
				    "	%4$s *ptr, *out;\n"
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
				    "	out = malloc(len);\n"
				    "	memcpy(out, ptr, len);\n"
				    "\n"
				    "	*count = len;\n"
				    "	return out;\n"
				    "}\n\n",
				    package.name, qm->name, qmm->name, type->name, qmm->id, type->size);
	} else {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, %4$s val)\n"
				"{\n"
				"	return qmi_tlv_set((struct qmi_tlv*)%2$s, %5$d, &val, sizeof(%4$s));\n"
				"}\n\n",
				package.name, qm->name, qmm->name, type->name, qmm->id);
		if (should_emit_parser(package.type, qm))
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
				package.name, qm->name, qmm->name, type->name, qmm->id);
	}
}

static void qmi_message_emit_string_prototype(FILE *fp,
					      const struct qmi_package package,
					      const struct qmi_message *qm,
					      struct qmi_message_member *qmm)
{
	if (qmm->array_size) {
		fprintf(stderr, "Dont' know how to encode string arrays yet");
		exit(1);
	} else {
		if (should_emit_builder(package.type, qm))
			fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, char *buf, size_t len);\n",
				    package.name, qm->name, qmm->name);
		else
			fprintf(fp, "char *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s);\n\n",
				    package.name, qm->name, qmm->name);
	}
}

static void qmi_message_emit_string_accessors(FILE *fp,
					      const struct qmi_package package,
					      const struct qmi_message *qm,
					      struct qmi_message_member *qmm)
{
	if (should_emit_builder(package.type, qm))
		fprintf(fp, "int %1$s_%2$s_set_%3$s(struct %1$s_%2$s *%2$s, char *buf, size_t len)\n"
			"{\n"
			"	return qmi_tlv_set((struct qmi_tlv*)%2$s, %4$d, buf, len);\n"
			"}\n\n",
			package.name, qm->name, qmm->name, qmm->id);
	if (should_emit_parser(package.type, qm))
		fprintf(fp, "char *%1$s_%2$s_get_%3$s(struct %1$s_%2$s *%2$s)\n"
			"{\n"
			"	char *ptr = NULL, *out;\n"
			"	size_t len;\n"
			"\n"
			"	ptr = qmi_tlv_get((struct qmi_tlv*)%2$s, %4$d, &len);\n"
			"	if (!ptr)\n"
			"		return NULL;\n"
			"\n"
			"	if (!ptr[len-1]) {\n"
			"		out = malloc(len);\n"
			"		memcpy(out, ptr, len);\n"
			"	} else {\n"
			"		out = malloc(len + 1);\n"
			"		memcpy(out, ptr, len);\n"
			"		out[len] = '\\0';\n"
			"	}\n"
			"\n"
			"	return out;\n"
			"}\n\n",
			package.name, qm->name, qmm->name, qmm->id);
}

static void qmi_message_source(FILE *fp, const struct qmi_package package)
{
	struct qmi_message_member *qmm;
	struct qmi_message *qm;
	struct qmi_struct *qs;

	list_for_each_entry(qm, &qmi_messages, node) {
		qmi_message_emit_message(fp, package, qm);

		list_for_each_entry(qmm, &qm->members, node) {
			if (qmm->type > SYMBOL_TYPE_MAX) {
				fprintf(stderr, "Invalid type for member '%s'!\n", qmm->name);
				exit(1);
			}
			switch (qmm->type) {
			case TYPE_STRING:
				qmi_message_emit_string_accessors(fp, package, qm, qmm);
				break;
			case TYPE_STRUCT:
				if (!strcmp(qmm->qmi_struct->type, qmi_response_type_v01.type))
					continue;
				qmi_struct_emit_accessors(fp, package, qm, qmm->name, qmm->id, qmm->array_size, qmm->qmi_struct);
				break;
			default:
				qmi_message_emit_simple_accessors(fp, package, qm, qmm);
				break;
			};
		}
	}

	list_for_each_entry(qs, &qmi_structs, node) {
		if (!strcmp(qs->type, qmi_response_type_v01.type))
			continue;
		if (!qmi_struct_has_ptr_members(qs))
			continue;
		qmi_struct_emit_free(fp, package, qs);
	}
}

static void qmi_message_header(FILE *fp, const struct qmi_package package)
{
	struct qmi_message_member *qmm;
	struct qmi_message *qm;

	list_for_each_entry(qm, &qmi_messages, node)
		fprintf(fp, "struct %s_%s;\n", package.name, qm->name);

	fprintf(fp, "\n");

	list_for_each_entry(qm, &qmi_messages, node) {
		qmi_message_emit_message_prototype(fp, package, qm);
		list_for_each_entry(qmm, &qm->members, node) {
			if (qmm->type > SYMBOL_TYPE_MAX) {
				fprintf(stderr, "Invalid type for member '%s'!\n", qmm->name);
				exit(1);
			}
			switch (qmm->type) {
			case TYPE_STRING:
				qmi_message_emit_string_prototype(fp, package, qm, qmm);
				break;
			case TYPE_STRUCT:
				if (!strcmp(qmm->qmi_struct->type, qmi_response_type_v01.type))
					continue;
				qmi_struct_emit_prototype(fp, package, qm, qmm->name, qmm->array_size, qmm->qmi_struct);
				break;
			default:
				qmi_message_emit_simple_prototype(fp, package, qm, qmm);
				break;
			};
		}
	}
}

static void emit_header_file_header(FILE *fp)
{
	fprintf(fp, "#include <stdint.h>\n"
		    "#include <stdbool.h>\n"
		    "#include <stddef.h>\n"
		    "#include <stdio.h>\n"
		    "#include <stdlib.h>\n\n"
		    "#include <libqrtr.h>\n\n");
	fprintf(fp, "#ifdef __cplusplus\n"
		    "extern \"C\" {\n"
		    "#endif\n\n");	
	fprintf(fp, "#define get_next(_type, _sz) ({ \\\n"
		    "	void* buf = ptr + len; \\\n"
		    "	len += _sz; \\\n"
		    "	if (len > buf_sz) goto err_wrong_len; \\\n"
		    "	*(_type*)buf; \\\n"
		    "})\n\n");
}

void accessor_emit_c(FILE *fp, const struct qmi_package package)
{
	emit_source_includes(fp, package.name);
	qmi_message_source(fp, package);
}
	
void accessor_emit_h(FILE *fp, const struct qmi_package package)
{
	guard_header(fp, package.name);
	emit_header_file_header(fp);
	qmi_const_header(fp);
	qmi_struct_header(fp, package.name);
	qmi_message_header(fp, package);
	guard_footer(fp);
	LOGD("\n\t==\nEmitted headers\n\t==\n\n");
}
