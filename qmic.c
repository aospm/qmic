#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "list.h"
#include "qmic.h"

FILE *sourcefile;

const struct symbol_type_table
/* +1 because
 * "warning: array subscript 5 is above array bounds of ‘const struct symbol_type_table[5]’
 * [-Warray-bounds]"
 * suspect GCC static analysis issue?
 */
sz_simple_types[SYMBOL_TYPE_MAX + 1] = {
	[TYPE_U8] = {"uint8_t", 1},
	[TYPE_U16] = {"uint16_t", 2},
	[TYPE_U32] = {"uint32_t", 4},
	[TYPE_U64] = {"uint64_t", 8},
	[TYPE_I8] = {"int8_t", 1},
	[TYPE_I16] = {"int16_t", 2},
	[TYPE_I32] = {"int32_t", 4},
	[TYPE_I64] = {"int64_t", 8},
	[TYPE_STRING] = {"char *", -1},
	[TYPE_STRUCT] = {"struct", -1},
};

void qmi_const_header(FILE *fp)
{
	struct qmi_const *qc;

	if (list_empty(&qmi_consts))
		return;

	list_for_each_entry(qc, &qmi_consts, node)
		fprintf(fp, "#define %s %llu\n", qc->name, qc->value);

	fprintf(fp, "\n");
}

void emit_source_includes(FILE *fp, const char *package)
{
	fprintf(fp, "#include <errno.h>\n"
		    "#include <string.h>\n"
		    "#include \"qmi_%1$s.h\"\n\n",
		    package);
}

void guard_header(FILE *fp, const char *package)
{
	char *upper;
	char *p;

	upper = p = strdup(package);
	while (*p) {
		*p = toupper(*p);
		p++;
	}

	fprintf(fp, "#ifndef __QMI_%s_H__\n", upper);
	fprintf(fp, "#define __QMI_%s_H__\n", upper);
	fprintf(fp, "\n");
}

void guard_footer(FILE *fp)
{
	fprintf(fp, "#ifdef __cplusplus\n"
		    "}\n"
		    "#endif\n\n");	
	fprintf(fp, "#endif\n");
}

static void usage(void)
{
	extern const char *__progname;

	fprintf(stderr, "Usage: %s [-ak] [-f FILE] [-o dir]\n", __progname);
	fprintf(stderr, "    -a        Emit accessor style sources for use with qmi_tlv\n");
	fprintf(stderr, "    -k        Emit kernel style sources\n");
	fprintf(stderr, "    -f FILE   Read from file (defaults to stdin)\n");
	fprintf(stderr, "    -o DIR    Output directory to write to\n");
	exit(1);
}

int main(int argc, char **argv)
{
	char fname[256];
	const char* source = NULL;
	const char* outdir = NULL;
	struct stat sb;
	FILE *hfp;
	FILE *sfp;
	int method = 0;
	int opt;
	struct qmi_package package;

	while ((opt = getopt(argc, argv, "akf:o:")) != -1) {
		switch (opt) {
		case 'a':
			method = 0;
			break;
		case 'k':
			method = 1;
			break;
		case 'f':
			source = optarg;
			break;
		case 'o':
			outdir = optarg;
			break;
		default:
			usage();
		}
	}

	if (source) {
		sourcefile = fopen(source, "r");
		if (!sourcefile) {
			fprintf(stderr, "Failed to open '%s' (%d: %s)\n", source,
				errno, strerror(errno));
			return EXIT_FAILURE;
		}
	} else {
		sourcefile = stdin;
	}

	if (outdir && !(stat(outdir, &sb) == 0 && S_ISDIR(sb.st_mode))) {
		fprintf(stderr, "Specified output directory '%s' either doesn't"
			" exist or isn't a directory\n", outdir);
		return EXIT_FAILURE;
	}

	if (!outdir)
		outdir = ".";

	qmi_parse(&package);

	snprintf(fname, sizeof(fname), "%s/qmi_%s.c", outdir, package.name);
	sfp = fopen(fname, "w");
	if (!sfp)
		err(1, "failed to open %s", fname);

	snprintf(fname, sizeof(fname), "%s/qmi_%s.h", outdir, package.name);
	hfp = fopen(fname, "w");
	if (!hfp)
		err(1, "failed to open %s", fname);

	switch (method) {
	case 0:
		accessor_emit_h(hfp, package);
		accessor_emit_c(sfp, package);
		break;
	case 1:
		kernel_emit_c(sfp, package.name);
		kernel_emit_h(hfp, package.name);
		break;
	}

	fclose(hfp);
	fclose(sfp);

	return 0;
}
