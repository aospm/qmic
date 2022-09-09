#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qmic.h"

static const char *sz_native_types[] = {
	[TYPE_U8] = "uint8_t",
	[TYPE_U16] = "uint16_t",
	[TYPE_U32] = "uint32_t",
	[TYPE_U64] = "uint64_t",
};

static const char *sz_data_types[] = {
	[TYPE_U8] = "QMI_UNSIGNED_1_BYTE",
	[TYPE_U16] = "QMI_UNSIGNED_2_BYTE",
	[TYPE_U32] = "QMI_UNSIGNED_4_BYTE",
	[TYPE_U64] = "QMI_UNSIGNED_8_BYTE"
};

static void emit_struct_definition(FILE *fp, const char *package,
				   struct qmi_struct *qs)
{
	struct qmi_struct_member *qsm;

	fprintf(fp, "struct %s_%s {\n", package, qs->name);

	list_for_each_entry(qsm, &qs->members, node) {
		if (qsm->is_ptr) {
			fprintf(fp, "\t%s %s_len;\n",
				sz_native_types[qsm->array_len_type],
				qsm->name);
		}
		switch (qsm->type) {
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64:
			fprintf(fp, "\t%s %s;\n", sz_native_types[qsm->type], qsm->name);
			break;
		case TYPE_STRING:
			fprintf(fp, "\tuint32_t %s_len;\n", qsm->name);
			fprintf(fp, "\tchar %s[256];\n", qsm->name);
			break;
		case TYPE_STRUCT:
			fprintf(fp, "\tstruct %s_%s %s%s;\n", package, qsm->qmi_struct->name,
				qsm->is_ptr ? "*" : "", qsm->name);
			break;
		}
	}

	fprintf(fp, "};\n");
	fprintf(fp, "\n");
}

static void emit_struct_native_ei(FILE *fp, const char *package,
				 struct qmi_struct *qs,
				 struct qmi_struct_member *qsm)
{
	if (qsm->array_fixed)
		fprintf(fp, "\t{\n"
			    "\t\t.data_type = %4$s,\n"
			    "\t\t.elem_len = %6$d,\n"
			    "\t\t.array_type = STATIC_ARRAY,\n"
			    "\t\t.elem_size = sizeof(%5$s),\n"
			    "\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
			    "\t},\n",
			    package, qs->name, qsm->name,
			    sz_data_types[qsm->type], sz_native_types[qsm->type],
			    qsm->array_size);
	else
		fprintf(fp, "\t{\n"
			    "\t\t.data_type = %4$s,\n"
			    "\t\t.elem_len = 1,\n"
			    "\t\t.elem_size = sizeof(%5$s),\n"
			    "\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
			    "\t},\n",
			    package, qs->name, qsm->name,
			    sz_data_types[qsm->type], sz_native_types[qsm->type]);
}

static void emit_struct_nested_ei(FILE *fp, const char *package,
				 struct qmi_struct *qs,
				 struct qmi_struct_member *qsm)
{
	if (qsm->is_ptr) {
		fprintf(fp, "\t{\n"
			"\t\t.data_type = QMI_STRUCT,\n"
			"\t\t.elem_len = 255,\n"
			"\t\t.array_type = VAR_LEN_ARRAY,\n"
			"\t\t.elem_size = sizeof(struct %1$s_%2$s),\n"
			"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
			"\t\t.ei_array = %1$s_%4$s_ei,\n"
			"\t},\n",
			package, qs->name, qsm->name, qsm->qmi_struct->name);
	} else {
		fprintf(fp, "\t{\n"
			"\t\t.data_type = QMI_STRUCT,\n"
			"\t\t.elem_len = 1,\n"
			"\t\t.elem_size = sizeof(struct %1$s_%2$s),\n"
			"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
			"\t\t.ei_array = %1$s_%4$s_ei,\n"
			"\t},\n",
			package, qs->name, qsm->name, qsm->qmi_struct->name);
	}
}

static void emit_struct_ei(FILE *fp, const char *package, struct qmi_struct *qs)
{
	struct qmi_struct_member *qsm;

	fprintf(fp, "struct qmi_elem_info %s_%s_ei[] = {\n", package, qs->name);

	list_for_each_entry(qsm, &qs->members, node) {
		if (qsm->is_ptr)
			fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_DATA_LEN,\n"
				"\t\t.elem_len = 1,\n"
				"\t\t.elem_size = sizeof(%4$s),\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s_len),\n"
				"\t},\n",
				package, qs->name, qsm->name,
				sz_native_types[qsm->array_len_type]);
		switch (qsm->type) {
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64:
			emit_struct_native_ei(fp, package, qs, qsm);
			break;
		case TYPE_STRING:
			fprintf(fp, "\t{\n"
				    "\t\t.data_type = QMI_STRING,\n"
				    "\t\t.elem_len = 256,\n"
				    "\t\t.elem_size = sizeof(char),\n"
				    "\t\t.offset = offsetof(struct %1$s_%2$s, %3$s)\n"
				    "\t},\n",
				package, qs->name, qsm->name);
			break;
		case TYPE_STRUCT:
			emit_struct_nested_ei(fp, package, qs, qsm);
			break;
		}
	}

	fprintf(fp, "\t{}\n");
	fprintf(fp, "};\n");
	fprintf(fp, "\n");
}

static void emit_native_type(FILE *fp, const char *package, struct qmi_message *qm,
			    struct qmi_message_member *qmm)
{
	static const char *sz_types[] = {
		[TYPE_U8] = "uint8_t",
		[TYPE_U16] = "uint16_t",
		[TYPE_U32] = "uint32_t",
		[TYPE_U64] = "uint64_t",
	};

	if (!qmm->required)
		fprintf(fp, "\tbool %s_valid;\n", qmm->name);

	if (qmm->array_size) {
		fprintf(fp, "\tuint32_t %s_len;\n", qmm->name);
		fprintf(fp, "\t%s %s[%d];\n", sz_types[qmm->type],
			qmm->name, qmm->array_size);
	} else {
		fprintf(fp, "\t%s %s;\n", sz_types[qmm->type],
			qmm->name);
	}
}

static void emit_struct_type(FILE *fp, const char *package, struct qmi_message *qm,
			     struct qmi_message_member *qmm)
{
	struct qmi_struct *qs = qmm->qmi_struct;
	if (!qmm->required)
		fprintf(fp, "\tbool %s_valid;\n", qmm->name);

	if (qmm->array_size) {
		fprintf(fp, "\tuint32_t %s_len;\n", qmm->name);
		fprintf(fp, "\tstruct %s_%s %s[%d];\n", package, qs->name,
			qmm->name, qmm->array_size);
	} else {
		fprintf(fp, "\tstruct %s_%s %s;\n", package, qs->name, qmm->name);
	}
}

static void emit_msg_struct(FILE *fp, const char *package, struct qmi_message *qm)
{
	struct qmi_message_member *qmm;

	fprintf(fp, "struct %1$s_%2$s {\n", package, qm->name);
	fprintf(fp, "\tstruct qmi_header qmi_header;\n");
	fprintf(fp, "\tstruct qmi_elem_info **ei;\n");

	list_for_each_entry(qmm, &qm->members, node) {
		switch (qmm->type) {
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64:
			emit_native_type(fp, package, qm, qmm);
			break;
		case TYPE_STRING:
			fprintf(fp, "\tuint32_t %s_len;\n", qmm->name);
			fprintf(fp, "\tchar %s[256];\n", qmm->name);
			break;
		case TYPE_STRUCT:
			emit_struct_type(fp, package, qm, qmm);
			break;
		}
	}

	fprintf(fp, "};\n");
	fprintf(fp, "\n");
}

static void emit_msg_initialiser(FILE *fp, const char *package,
				    struct qmi_message *qm)
{
	struct qmi_message_member *qmm;
	int initialiser_len = strlen(package) + strlen(qm->name) + 15; // "_, _INITIALIZER"
	char *upper;
	char *p = upper = memalloc(initialiser_len+1);

	snprintf(upper, initialiser_len, "%s_%s_NEW", package, qm->name);
	while (*p) {
		*p = toupper(*p);
		p++;
	}

	fprintf(fp, "#define %1$s ({ \\\n"
		    "	struct %2$s_%3$s *ptr = malloc(sizeof(struct %2$s_%3$s)); \\\n"
		    "	ptr->qmi_header->type = %4$d; ptr->qmi_header->msg_id = 0x%5$04x; \\\n"
		    "	ptr->ei = &%2$s_%3$s_ei; ptr })\n",
		upper, package, qm->name, qm->type, qm->msg_id);

	snprintf(upper, initialiser_len, "%s_%s_INITIALIZER", package, qm->name);
	p = upper;
	while (*p) {
		*p = toupper(*p);
		p++;
	}

	fprintf(fp, "#define %s { { %d, 0, 0x%04x, 0 }, &%s_%s_ei", upper,
		qm->type, qm->msg_id, package, qm->name);

	list_for_each_entry(qmm, &qm->members, node) {
		switch (qmm->type) {
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64:
			fprintf(fp, ", 0");
			break;
		case TYPE_STRING:
			fprintf(fp, ", 0, NULL");
			break;
		case TYPE_STRUCT:
			fprintf(fp, ", {}");
			break;
		}
	}

	fprintf(fp, " }\n");
}

static void emit_native_ei(FILE *fp, const char *package, struct qmi_message *qm,
			   struct qmi_message_member *qmm)
{
	if (!qmm->required) {
		fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_OPT_FLAG,\n"
				"\t\t.elem_len = 1,\n"
				"\t\t.elem_size = sizeof(bool),\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s_valid),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id);
	}

	if (qmm->array_fixed) {
		fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_UNSIGNED_1_BYTE,\n"
				"\t\t.elem_len = %5$d,\n"
				"\t\t.elem_size = sizeof(%6$s),\n"
				"\t\t.array_type = STATIC_ARRAY,\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id, qmm->array_size,
				sz_native_types[qmm->type]);
	} else if (qmm->array_size) {
		fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_DATA_LEN,\n"
				"\t\t.elem_len = 1,\n"
				"\t\t.elem_size = sizeof(%5$s),\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s_len),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id,
				qmm->array_size >= 256 ? "uint16_t" : "uint8_t");
		fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_UNSIGNED_1_BYTE,\n"
				"\t\t.elem_len = %5$d,\n"
				"\t\t.elem_size = sizeof(%6$s),\n"
				"\t\t.array_type = VAR_LEN_ARRAY,\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id, qmm->array_size,
				sz_native_types[qmm->type]);
	} else {
		fprintf(fp, "\t{\n"
				"\t\t.data_type = %5$s,\n"
				"\t\t.elem_len = 1,\n"
				"\t\t.elem_size = sizeof(%6$s),\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id,
				sz_data_types[qmm->type],
				sz_native_types[qmm->type]);
	}
}

static void emit_struct_ref_ei(FILE *fp, const char *package, struct qmi_message *qm,
			   struct qmi_message_member *qmm)
{
	struct qmi_struct *qs = qmm->qmi_struct;

	if (!qmm->required) {
		fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_OPT_FLAG,\n"
				"\t\t.elem_len = 1,\n"
				"\t\t.elem_size = sizeof(bool),\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s_valid),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id);
	}

	if (qmm->array_size) {
		fprintf(fp, "\t{\n"
				"\t\t.data_type = QMI_DATA_LEN,\n"
				"\t\t.elem_len = 1,\n"
				"\t\t.elem_size = sizeof(%5$s),\n"
				"\t\t.tlv_type = %4$d,\n"
				"\t\t.offset = offsetof(struct %1$s_%2$s, %3$s_len),\n"
				"\t},\n",
				package, qm->name, qmm->name, qmm->id,
				qmm->array_size >= 256 ? "uint16_t" : "uint8_t");

		fprintf(fp, "\t{\n"
			    "\t\t.data_type = QMI_STRUCT,\n"
			    "\t\t.elem_len = %6$d,\n"
			    "\t\t.elem_size = sizeof(struct %1$s_%5$s),\n"
			    "\t\t.array_type = VAR_LEN_ARRAY,\n"
			    "\t\t.tlv_type = %4$d,\n"
			    "\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
			    "\t\t.ei_array = %1$s_%5$s_ei,\n"
			    "\t},\n",
			    package, qm->name, qmm->name, qmm->id, qs->name, qmm->array_size);
	} else {
		fprintf(fp, "\t{\n"
			    "\t\t.data_type = QMI_STRUCT,\n"
			    "\t\t.elem_len = 1,\n"
			    "\t\t.elem_size = sizeof(struct %1$s_%5$s),\n"
			    "\t\t.tlv_type = %4$d,\n"
			    "\t\t.offset = offsetof(struct %1$s_%2$s, %3$s),\n"
			    "\t\t.ei_array = %1$s_%5$s_ei,\n"
			    "\t},\n",
			    package, qm->name, qmm->name, qmm->id, qs->name);
	}
}

static void emit_elem_info_array_decl(FILE *fp, const char *package, struct qmi_message *qm)
{
	fprintf(fp, "extern struct qmi_elem_info %1$s_%2$s_ei[];\n",
		package, qm->name);
}

static void emit_elem_info_array(FILE *fp, const char *package, struct qmi_message *qm)
{
	struct qmi_message_member *qmm;

	fprintf(fp, "struct qmi_elem_info %1$s_%2$s_ei[] = {\n",
		package, qm->name);

	list_for_each_entry(qmm, &qm->members, node) {
		switch (qmm->type) {
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64:
			emit_native_ei(fp, package, qm, qmm);
			break;
		case TYPE_STRUCT:
			emit_struct_ref_ei(fp, package, qm, qmm);
			break;
		case TYPE_STRING:
			fprintf(fp, "\t{\n"
				    "\t\t.data_type = QMI_STRING,\n"
				    "\t\t.elem_len = 256,\n"
				    "\t\t.elem_size = sizeof(char),\n"
				    "\t\t.array_type = VAR_LEN_ARRAY,\n"
				    "\t\t.tlv_type = %4$d,\n"
				    "\t\t.offset = offsetof(struct %1$s_%2$s, %3$s)\n"
				    "\t},\n",
				package, qm->name, qmm->name, qmm->id);
			break;
		}
	}
	fprintf(fp, "\t{}\n");
	fprintf(fp, "};\n");
	fprintf(fp, "\n");
}

static void emit_h_file_header(FILE *fp)
{
	fprintf(fp, "#include <stdint.h>\n"
		    "#include <stdbool.h>\n"
		    "\n"
		    "#include \"libqrtr.h\"\n"
		    "\n");
};

void kernel_emit_c(FILE *fp, const char *package)
{
	struct qmi_message *qm;
	struct qmi_struct *qs;

	emit_source_includes(fp, package);
	
	list_for_each_entry(qs, &qmi_structs, node)
		emit_struct_ei(fp, package, qs);
	
	list_for_each_entry(qm, &qmi_messages, node)
		emit_elem_info_array(fp, package, qm);
}
	
void kernel_emit_h(FILE *fp, const char *package)
{
	struct qmi_message *qm;
	struct qmi_struct *qs;

	guard_header(fp, package);
	emit_h_file_header(fp);

	list_for_each_entry(qm, &qmi_messages, node)
		emit_elem_info_array_decl(fp, package, qm);
	fprintf(fp, "\n");

	qmi_const_header(fp);

	list_for_each_entry(qs, &qmi_structs, node)
		emit_struct_definition(fp, package, qs);

	list_for_each_entry(qm, &qmi_messages, node)
		emit_msg_struct(fp, package, qm);

	list_for_each_entry(qm, &qmi_messages, node)
		emit_msg_initialiser(fp, package, qm);
	fprintf(fp, "\n");

	guard_footer(fp);
}
