/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

#include "ast.h"
#include "list.h"
#include "parser.h"
#include "program.h"
#include "server.h"
#include "storage.h"
#include "structures.h"
#include "utils.h"

Program *
new_program(void)
{
    Program *p = (Program *)malloc(sizeof(Program));

    p->ref_count = 1;
    p->first_lineno = 1;
    p->cached_lineno = 1;
    p->cached_lineno_pc = 0;
    p->cached_lineno_vec = MAIN_VECTOR;
    return p;
}

Program *
null_program(void)
{
    static Program *p = 0;
    List code;
    Var errors;

    if (!p) {
	code = new_list(0);
	p = parse_list_as_program(code, &errors);
	if (!p)
	    panic("Can't create the null program!");
	free_var(code);
	free_var(errors);
    }
    return p;
}

Program *
program_ref(Program * p)
{
    p->ref_count++;

    return p;
}

int
program_bytes(Program * p)
{
    unsigned int i, count;

    count = sizeof(Program);
    count += p->main_vector.size;

    for (i = 0; i < p->num_literals; i++)
	count += value_bytes(p->literals[i]);

    count += sizeof(Bytecodes) * p->fork_vectors_size;
    for (i = 0; i < p->fork_vectors_size; i++)
	count += p->fork_vectors[i].size;

    count += sizeof(const char *) * p->num_var_names;
    for (i = 0; i < p->num_var_names; i++)
	count += memo_strlen(p->var_names[i]) + 1;

    return count;
}

void
free_program(Program * p)
{
    unsigned i;

    p->ref_count--;
    if (p->ref_count == 0) {

	for (i = 0; i < p->num_literals; i++)
	    /* can't be a list--strings and floats need to be freed, though. */
	    free_var(p->literals[i]);
	if (p->literals)
	    free(p->literals);

	for (i = 0; i < p->fork_vectors_size; i++)
	    free(p->fork_vectors[i].vector);
	if (p->fork_vectors_size)
	    free(p->fork_vectors);

	for (i = 0; i < p->num_var_names; i++)
	    free_str(p->var_names[i]);
	free(p->var_names);

	free(p->main_vector.vector);

	free(p);
    }
}
