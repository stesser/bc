/*
 * *****************************************************************************
 *
 * Copyright 2018 Gavin D. Howard
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * *****************************************************************************
 *
 * Code common to all of bc and dc.
 *
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

#include <status.h>
#include <vm.h>
#include <io.h>

void bc_vm_sig(int sig) {
	if (sig == SIGINT) {
		if (write(2, bc_sig_msg, sizeof(bc_sig_msg) - 1) >= 0)
			bcg.sig += (bcg.signe = bcg.sig == bcg.sigc);
	}
	else bcg.sig_other = 1;
}

BcStatus bc_vm_error(BcStatus s, const char *file, size_t line) {

	assert(file);

	if (!s || s >= BC_STATUS_POSIX_NAME_LEN) return s;

	fprintf(stderr, bc_err_fmt, bc_errs[bc_err_indices[s]], bc_err_descs[s]);
	fprintf(stderr, "    %s", file);
	fprintf(stderr, bc_err_line + 3 * !line, line);

	return s * (!bcg.tty || !!strcmp(file, bc_program_stdin_name));
}

BcStatus bc_vm_posix_error(BcStatus s, const char *file,
                           size_t line, const char *msg)
{
	int p = (int) bcg.posix, w = (int) bcg.warn;

	if (!(p || w) || s < BC_STATUS_POSIX_NAME_LEN) return BC_STATUS_SUCCESS;

	fprintf(stderr, "\n%s %s: %s\n", bc_errs[bc_err_indices[s]],
	        p ? "error" : "warning", bc_err_descs[s]);

	if (msg) fprintf(stderr, "    %s\n", msg);

	if (file) {
		fprintf(stderr, "    %s", file);
		fprintf(stderr, bc_err_line + 3 * !line, line);
	}

	return s * (!bcg.tty && !!p);
}

BcStatus bc_vm_process(BcVm *vm, const char *text) {

	BcStatus s = bc_lex_text(&vm->parse.lex, text);

	if ((s = bc_vm_error(s, vm->parse.lex.file, vm->parse.lex.line))) return s;

	while (vm->parse.lex.t.t != BC_LEX_EOF) {

		if ((s = vm->parse.parse(&vm->parse)) == BC_STATUS_LIMITS) {

			s = BC_STATUS_IO_ERR;

			if (putchar('\n') == EOF) return s;

			if (printf("BC_BASE_MAX     = %lu\n", BC_MAX_OBASE) < 0 ||
			    printf("BC_DIM_MAX      = %lu\n", BC_MAX_DIM) < 0 ||
			    printf("BC_SCALE_MAX    = %lu\n", BC_MAX_SCALE) < 0 ||
			    printf("BC_STRING_MAX   = %lu\n", BC_MAX_STRING) < 0 ||
			    printf("BC_NAME_MAX     = %lu\n", BC_MAX_NAME) < 0 ||
			    printf("BC_NUM_MAX      = %lu\n", BC_MAX_NUM) < 0 ||
			    printf("Max Exponent    = %lu\n", BC_MAX_EXP) < 0 ||
			    printf("Number of Vars  = %lu\n", BC_MAX_VARS) < 0)
			{
				return s;
			}

			if (putchar('\n') == EOF) return s;

			s = BC_STATUS_SUCCESS;
		}
		else if (s == BC_STATUS_QUIT || bcg.sig_other ||
		         (s = bc_vm_error(s, vm->parse.lex.file, vm->parse.lex.line)))
		{
			return s;
		}
	}

	if (BC_PARSE_CAN_EXEC(&vm->parse)) {
		s = bc_program_exec(&vm->prog);
		if (bcg.tty) fflush(stdout);
		if (s && s != BC_STATUS_QUIT) s = bc_vm_error(s, vm->parse.lex.file, 0);
	}

	return s;
}

BcStatus bc_vm_file(BcVm *bc, const char *file) {

	BcStatus s;
	char *data;
	BcFunc *main_func;
	BcInstPtr *ip;

	bc->prog.file = file;
	if ((s = bc_io_fread(file, &data))) return bc_vm_error(s, file, 0);

	bc_lex_file(&bc->parse.lex, file);
	if ((s = bc_vm_process(bc, data))) goto err;

	main_func = bc_vec_item(&bc->prog.fns, BC_PROG_MAIN);
	ip = bc_vec_item(&bc->prog.stack, 0);

	if (main_func->code.len < ip->idx) s = BC_STATUS_EXEC_FILE_NOT_EXECUTABLE;

err:
	free(data);
	return s;
}

BcStatus bc_vm_stdin(BcVm *bc) {

	BcStatus s;
	BcVec buf, buffer;
	char c;
	size_t len, i;
	bool string, comment, notend;

	bc->prog.file = bc_program_stdin_name;
	bc_lex_file(&bc->parse.lex, bc_program_stdin_name);

	if ((s = bc_vec_init(&buffer, sizeof(char), NULL))) return s;
	if ((s = bc_vec_init(&buf, sizeof(char), NULL))) goto buf_err;
	if ((s = bc_vec_pushByte(&buffer, '\0'))) goto err;

	string = comment = false;

	// This loop is complex because the vm tries not to send any lines that end
	// with a backslash to the parser. The reason for that is because the parser
	// treats a backslash+newline combo as whitespace, per the bc spec. In that
	// case, and for strings and comments, the parser will expect more stuff.
	while (!s && !(s = bc_io_getline(&buf, ">>> "))) {

		char *str = buf.vec;

		len = buf.len - 1;

		if (len == 1 && buf.vec[0] == '"') string = !string;
		else if (len > 1 || comment) {

			for (i = 0; i < len; ++i) {

				notend = len > i + 1;

				if ((c = str[i]) == '"') string = !string;
				else if (c == '/' && notend && !comment && str[i + 1] == '*') {
					comment = true;
					break;
				}
				else if (c == '*' && notend && comment && str[i + 1] == '/')
					comment = false;
			}

			if (string || comment || str[len - 2] == '\\') {
				if ((s = bc_vec_concat(&buffer, buf.vec))) goto err;
				continue;
			}
		}

		if ((s = bc_vec_concat(&buffer, buf.vec))) goto err;
		if ((s = bc_vm_process(bc, buffer.vec))) goto err;

		bc_vec_npop(&buffer, buffer.len);
	}

	if (s == BC_STATUS_BIN_FILE)
		s = bc_vm_error(s, bc->parse.lex.file, bc->parse.lex.line);

	// I/O error will always happen when stdin is
	// closed. It's not a problem in that case.
	s = s == BC_STATUS_IO_ERR ? BC_STATUS_SUCCESS : s;

	if (string) s = bc_vm_error(BC_STATUS_LEX_NO_STRING_END,
	                            bc->parse.lex.file, bc->parse.lex.line);
	else if (comment) s = bc_vm_error(BC_STATUS_LEX_NO_COMMENT_END,
	                                  bc->parse.lex.file, bc->parse.lex.line);

err:
	bc_vec_free(&buf);
buf_err:
	bc_vec_free(&buffer);
	return s;
}

BcStatus bc_vm_exec(unsigned int flags, BcVec *exprs, BcVec *files,
                    BcParseInit parse_init, BcParseExpr parse_expr)
{
	BcStatus s;
	BcVm vm;
	size_t i, len;
	char *lenv;
	struct sigaction sa;
	int num, ttyout = isatty(1);

	bcg.ttyin = isatty(0);
	bcg.tty = (flags & BC_FLAG_I) || bcg.ttyin || ttyout;
	bcg.posix = flags & BC_FLAG_S;
	bcg.warn = flags & BC_FLAG_W;

	if (!strcmp(bcg.name, bc_name) && (lenv = getenv("BC_LINE_LENGTH"))) {
		len = strlen(lenv);
		for (num = 1, i = 0; num && i < len; ++i) num = isdigit(lenv[i]);
		if (!num || ((len = (size_t) atoi(lenv) - 1) < 2 && len >= INT32_MAX))
			len = BC_NUM_PRINT_WIDTH;
	}
	else len = BC_NUM_PRINT_WIDTH;

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = bc_vm_sig;
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGPIPE, &sa, NULL) < 0 ||
	    sigaction(SIGHUP, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
	{
		return BC_STATUS_EXEC_SIGACTION_FAIL;
	}

	if ((s = bc_program_init(&vm.prog, len, parse_init, parse_expr))) return s;
	if ((s = parse_init(&vm.parse, &vm.prog))) goto parse_err;

	if (bcg.tty && ttyout && !(flags & BC_FLAG_Q) && puts(bc_header) == EOF) {
		s = BC_STATUS_IO_ERR;
		goto err;
	}

#ifdef BC_CONFIG
	if (flags & BC_FLAG_L) {

		bc_lex_file(&vm.parse.lex, bc_lib_name);
		if ((s = bc_lex_text(&vm.parse.lex, bc_lib))) goto err;

		while (!s && vm.parse.lex.t.t != BC_LEX_EOF)
			s = vm.parse.parse(&vm.parse);

		if (s || (s = bc_program_exec(&vm.prog))) goto err;
	}
#endif // BC_CONFIG

	if (exprs->len > 1 && (s = bc_vm_process(&vm, exprs->vec))) goto err;

	for (i = 0; !bcg.sig_other && !s && i < files->len; ++i)
		s = bc_vm_file(&vm, *((char**) bc_vec_item(files, i)));
	if (s || bcg.sig_other) goto err;

	if (exprs->len <= 1) s = bc_vm_stdin(&vm);

err:
	bc_parse_free(&vm.parse);
parse_err:
	bc_program_free(&vm.prog);
	return s == BC_STATUS_QUIT ? BC_STATUS_SUCCESS : s;
}