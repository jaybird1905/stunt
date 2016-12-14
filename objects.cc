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

#include "collection.h"
#include "db.h"
#include "db_io.h"
#include "execute.h"
#include "functions.h"
#include "list.h"
#include "numbers.h"
#include "quota.h"
#include "server.h"
#include "storage.h"
#include "structures.h"
#include "utils.h"

static int
controls(Objid who, Objid what)
{
    return is_wizard(who) || who == db_object_owner(what);
}

static int
controls2(Objid who, const Var& what)
{
    return is_wizard(who) || who == db_object_owner2(what);
}

static Var
make_arglist(Objid what)
{
    List r = new_list(1);
    r.v.list[1] = Var::new_obj(what);
    return r;
}

static bool
all_valid(const Var& vars)
{
    Var var;
    int i, c;
    FOR_EACH(var, vars, i, c)
	if (!valid(var.v.obj))
	    return false;
    return true;
}

static bool
all_allowed(const Var& vars, Objid progr, db_object_flag f)
{
    Var var;
    int i, c;
    FOR_EACH(var, vars, i, c)
	if (!db_object_allows(var, progr, f))
	    return false;
    return true;
}

/*
 * Returns true if `_this' is a descendant of `obj'.
 */
static bool
is_a_descendant(const Var& _this, const Var& obj)
{
    Var descendants = db_descendants(obj, true);
    int ret = ismember(_this, descendants, 1);
    free_var(descendants);
    return ret ? true : false;
}

/*
 * Returns true if any of `these' are descendants of `obj'.
 */
static bool
any_are_descendants(const Var& these, const Var& obj)
{
    Var _this, descendants = db_descendants(obj, true);
    int i, c;
    FOR_EACH(_this, these, i, c) {
	if (is_a_descendant(_this, obj)) {
	    free_var(descendants);
	    return true;
	}
    }
    free_var(descendants);
    return false;
}

struct bf_move_data {
    Objid what, where;
};

static package
do_move(const Var& value, Objid progr, Byte next, struct bf_move_data *data)
{
    Objid what = data->what, where = data->where;
    Objid oid, oldloc;
    int accepts;
    Var args;
    enum error e;

    switch (next) {
    case 1:			/* Check validity and decide `accepts' */
	if (!valid(what) || (!valid(where) && where != NOTHING))
	    return make_error_pack(E_INVARG);
	else if (!controls(progr, what))
	    return make_error_pack(E_PERM);
	else if (where == NOTHING)
	    accepts = 1;
	else {
	    args = make_arglist(what);
	    e = call_verb(where, "accept", Var::new_obj(where), args, 0);
	    /* e will not be E_INVIND */

	    if (e == E_NONE)
		return make_call_pack(2, data);
	    else {
		free_var(args);
		if (e == E_VERBNF)
		    accepts = 0;
		else		/* (e == E_MAXREC) */
		    return make_error_pack(e);
	    }
	}
	goto accepts_decided;

    case 2:			/* Returned from `accepts' call */
	accepts = is_true(value);

      accepts_decided:
	if (!is_wizard(progr) && accepts == 0)
	    return make_error_pack(E_NACC);

	if (!valid(what)
	    || (where != NOTHING && !valid(where))
	    || db_object_location(what) == where)
	    return no_var_pack();

	/* Check to see that we're not trying to violate the hierarchy */
	for (oid = where; oid != NOTHING; oid = db_object_location(oid))
	    if (oid == what)
		return make_error_pack(E_RECMOVE);

	oldloc = db_object_location(what);
	db_change_location(what, where);

	args = make_arglist(what);
	e = call_verb(oldloc, "exitfunc", Var::new_obj(oldloc), args, 0);

	if (e == E_NONE)
	    return make_call_pack(3, data);
	else {
	    free_var(args);
	    if (e == E_MAXREC)
		return make_error_pack(e);
	}
	/* e == E_INVIND or E_VERBNF, in both cases fall through */

    case 3:			/* Returned from exitfunc call */
	if (valid(where) && valid(what)
	    && db_object_location(what) == where) {
	    args = make_arglist(what);
	    e = call_verb(where, "enterfunc", Var::new_obj(where), args, 0);
	    /* e != E_INVIND */

	    if (e == E_NONE)
		return make_call_pack(4, data);
	    else {
		free_var(args);
		if (e == E_MAXREC)
		    return make_error_pack(e);
		/* else e == E_VERBNF, fall through */
	    }
	}
    case 4:			/* Returned from enterfunc call */
	return no_var_pack();

    default:
	panic("Unknown PC in DO_MOVE");
	return no_var_pack();	/* Dead code to eliminate compiler warning */
    }
}

static package
bf_move(const Var& value, Objid progr, Byte next, void *vdata)
{
    struct bf_move_data *data = (bf_move_data *)vdata;
    package p;

    if (next == 1) {
	data = (bf_move_data *)alloc_data(sizeof(*data));
	data->what = value.v.list[1].v.obj;
	data->where = value.v.list[2].v.obj;
    }
    p = do_move(value, progr, next, data);
    free_var(value);

    if (p.kind != package::BI_CALL)
	free_data(data);

    return p;
}

static void
bf_move_write(void *vdata)
{
    struct bf_move_data *data = (bf_move_data *)vdata;

    dbio_printf("bf_move data: what = %d, where = %d\n",
		data->what, data->where);
}

static void *
bf_move_read()
{
    struct bf_move_data *data = (bf_move_data *)alloc_data(sizeof(*data));

    if (dbio_scanf("bf_move data: what = %d, where = %d\n",
		   &data->what, &data->where) == 2)
	return data;
    else
	return 0;
}

static package
bf_toobj(const List& arglist, Objid progr)
{
    Var r;
    int i;
    enum error e;

    e = become_integer(arglist[1], &i, 0);
    r = Var::new_obj(i);

    free_var(arglist);

    if (e != E_NONE)
	return make_error_pack(e);
    else
	return make_var_pack(r);
}

static package
bf_typeof(const List& arglist, Objid progr)
{
    Var r = Var::new_int((int)arglist[1].type & TYPE_DB_MASK);
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_valid(const List& arglist, Objid progr)
{				/* (object) */
    Var r;

    if (arglist[1].is_object()) {
	r = Var::new_int(is_valid(arglist[1]));
    }
    else {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    }

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_max_object(const List& arglist, Objid progr)
{				/* () */
    Var r = Var::new_obj(db_last_used_objid());
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_create(const Var& value, Objid progr, Byte next, void *vdata)
{			/* (OBJ|LIST parent(s) [, OBJ owner] [, INT anonymous] [, LIST args]) */
    Var *data = (Var *)vdata;
    Var r;

    if (next == 1) {
	// there must be at least one argument, and
	// it must be an object or list of objects
	if (!is_obj_or_list_of_objs(value.v.list[1])) {
	    free_var(value);
	    return make_error_pack(E_TYPE);
	}

	int nargs = value.v.list[0].v.num;
	Objid owner = progr;
	int anon = 0; // position of the anonymous flag argument
	int init = 0; // position of the initializer argument

	if (1 < nargs && value.v.list[2].is_obj())
	    owner = value.v.list[2].v.obj;
	else if (1 < nargs && value.v.list[2].is_int())
	    anon = 2;
	else if (1 < nargs && value.v.list[2].is_list())
	    init = 2;
	else if (1 < nargs) {
	    free_var(value);
	    return make_error_pack(E_TYPE);
	}

	if (2 < nargs && value.v.list[3].is_int() && !anon)
	    anon = 3;
	else if (2 < nargs && value.v.list[3].is_list() && !init)
	    init = 3;
	else if (2 < nargs) {
	    free_var(value);
	    return make_error_pack(E_TYPE);
	}

	if (3 < nargs && value.v.list[4].is_int() && !anon)
	    anon = 4;
	else if (3 < nargs && value.v.list[4].is_list() && !init)
	    init = 4;
	else if (3 < nargs) {
	    free_var(value);
	    return make_error_pack(E_TYPE);
	}

	bool anonymous = anon > 0 ? value.v.list[anon].v.num : false;

	if ((anonymous && owner == NOTHING)
	    || (!valid(owner) && owner != NOTHING)
	    || (value.v.list[1].is_obj()
		&& !valid(value.v.list[1].v.obj)
		&& value.v.list[1].v.obj != NOTHING)
	    || (value.v.list[1].is_list()
		&& !all_valid(value.v.list[1]))) {
	    free_var(value);
	    return make_error_pack(E_INVARG);
	}
	else if ((progr != owner && !is_wizard(progr))
		 || (value.v.list[1].is_obj()
		     && valid(value.v.list[1].v.obj)
		     && !db_object_allows(value.v.list[1], progr,
		                          anonymous ? FLAG_ANONYMOUS : FLAG_FERTILE))
		 || (value.v.list[1].is_list()
		     && !all_allowed(value.v.list[1], progr,
		                     anonymous ? FLAG_ANONYMOUS : FLAG_FERTILE))) {
	    free_var(value);
	    return make_error_pack(E_PERM);
	}

	if (valid(owner) && !decr_quota(owner)) {
	    free_var(value);
	    return make_error_pack(E_QUOTA);
	}
	else {
	    enum error e;
	    Objid last = db_last_used_objid();
	    Objid oid = db_create_object();
	    Var args;

	    db_set_object_owner(oid, !valid(owner) ? oid : owner);

	    if (!db_change_parents(Var::new_obj(oid), value.v.list[1])) {
		db_destroy_object(oid);
		db_set_last_used_objid(last);
		free_var(value);
		return make_error_pack(E_INVARG);
	    }

	    /*
	     * If anonymous, clean up the object used to create the
	     * anonymous object; `oid' is invalid after that.
	     */
	    if (anonymous) {
		r.type = TYPE_ANON;
		r.v.anon = db_make_anonymous(oid, last);
	    } else {
		r.type = TYPE_OBJ;
		r.v.obj = oid;
	    }

	    data = (Var *)alloc_data(sizeof(Var));
	    *data = var_ref(r);

	    /* pass in initializer args, if present */
	    args = init > 0 ? var_ref(value.v.list[init]) : new_list(0);

	    free_var(value);

	    e = call_verb(oid, "initialize", r, args, 0);
	    /* e will not be E_INVIND */

	    if (e == E_NONE) {
		free_var(r);
		return make_call_pack(2, data);
	    }

	    free_var(*data);
	    free_data(data);
	    free_var(args);

	    if (e == E_MAXREC) {
		free_var(r);
		return make_error_pack(e);
	    } else		/* (e == E_VERBNF) do nothing */
		return make_var_pack(r);
	}
    } else {			/* next == 2, returns from initialize verb_call */
	r = var_ref(*data);
	free_var(*data);
	free_data(data);
	return make_var_pack(r);
    }
}

static void
bf_create_write(void *vdata)
{
    dbio_printf("bf_create data: oid = %d\n", *((Objid *) vdata));
}

static void *
bf_create_read(void)
{
    Objid *data = (Objid *)alloc_data(sizeof(Objid));

    if (dbio_scanf("bf_create data: oid = %d\n", data) == 1)
	return data;
    else
	return 0;
}

static package
bf_chparent_chparents(const List& arglist, Objid progr)
{				/* (OBJ obj, OBJ|LIST what, LIST anon) */
    Var obj = arglist[1];
    Var what = arglist[2];
    int n = arglist.length();
    List anon_kids;

    if (!obj.is_object() || !is_obj_or_list_of_objs(what)) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    }

    if (n > 2 && !is_wizard(progr)) {
	free_var(arglist);
	return make_error_pack(E_PERM);
    }
    else if (n > 2 && arglist[3].is_list()) {
	anon_kids = static_cast<const List&>(arglist[3]);
    }
    else {
	anon_kids = new_list(0);
    }

    if (!is_valid(obj)
	|| (what.is_obj() && !valid(what.v.obj) && what.v.obj != NOTHING)
	|| (what.is_list() && !all_valid(what))) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    }
    else if (!controls2(progr, obj)
	     || (what.is_obj() && valid(what.v.obj) && !db_object_allows(what, progr, FLAG_FERTILE))
	     || (what.is_list() && !all_allowed(what, progr, FLAG_FERTILE))) {
	free_var(arglist);
	return make_error_pack(E_PERM);
    }
    else if ((what.is_obj() && is_a_descendant(what, obj))
	     || (what.is_list() && any_are_descendants(what, obj))) {
	free_var(arglist);
	return make_error_pack(E_RECMOVE);
    }
    else {
	if (!db_change_parents(obj, what, anon_kids)) {
	    free_var(arglist);
	    return make_error_pack(E_INVARG);
	}
	else {
	    free_var(arglist);
	    return no_var_pack();
	}
    }
}

/* bf_parent is DEPRECATED!  It returns only the first parent in the
 * set of parents.  Use bf_parents!
 */
static package
bf_parent(const List& arglist, Objid progr)
{				/* (OBJ object) */
    Var r;

    if (!arglist[1].is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if (!is_valid(arglist[1])) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    } else {
	r = var_ref(db_object_parents2(arglist[1]));
	free_var(arglist);
    }

    if (r.is_obj())
	return make_var_pack(r);

    if (0 == listlength(r)) {
	free_var(r);
	return make_var_pack(Var::new_obj(NOTHING));
    } else {
	Var t = var_ref(r.v.list[1]);
	free_var(r);
	return make_var_pack(t);
    }
}

static package
bf_parents(const List& arglist, Objid progr)
{				/* (OBJ object) */
    Var r;

    if (!arglist[1].is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    }  else if (!is_valid(arglist[1])) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    } else {
	r = var_ref(db_object_parents2(arglist[1]));
	free_var(arglist);
    }

    if (r.is_list())
	return make_var_pack(r);

    if (NOTHING == r.v.obj) {
	free_var(r);
	return make_var_pack(new_list(0));
    } else {
	List t = new_list(1);
	t.v.list[1] = r;
	return make_var_pack(t);
    }
}

static package
bf_children(const List& arglist, Objid progr)
{				/* (object) */
    Var obj = arglist[1];

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    } else {
	Var r = var_ref(db_object_children2(obj));
	free_var(arglist);
	return make_var_pack(r);
    }
}

static package
bf_ancestors(const List& arglist, Objid progr)
{				/* (OBJ object) */
    Var obj = arglist[1];
    bool full = (arglist.length() > 1 && is_true(arglist[2])) ? true : false;

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    } else {
	Var r = db_ancestors(obj, full);
	free_var(arglist);
	return make_var_pack(r);
    }
}

static package
bf_descendants(const List& arglist, Objid progr)
{				/* (OBJ object) */
    Var obj = arglist[1];
    bool full = (arglist.length() > 1 && is_true(arglist[2])) ? true : false;

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    } else {
	Var r = db_descendants(obj, full);
	free_var(arglist);
	return make_var_pack(r);
    }
}

static int
move_to_nothing(Objid oid)
{
    /* All we need to do is change the location and run the exitfunc. */
    Objid oldloc = db_object_location(oid);
    Var args;
    enum error e;

    db_change_location(oid, NOTHING);

    args = make_arglist(oid);
    e = call_verb(oldloc, "exitfunc", Var::new_obj(oldloc), args, 0);

    if (e == E_NONE)
	return 1;

    free_var(args);
    return 0;
}

static int
first_proc(void *data, Objid oid)
{
    Objid *oidp = (Objid *)data;

    *oidp = oid;
    return 1;
}

static Objid
get_first(Objid oid, int (*for_all) (Objid, int (*)(void *, Objid), void *))
{
    Objid result = NOTHING;

    for_all(oid, first_proc, &result);

    return result;
}

static package
bf_recycle(const Var& value, Objid progr, Byte func_pc, void *vdata)
{				/* (OBJ|ANON object) */
    Var *data = (Var *)vdata;
    enum error e;
    Var obj;
    Var args;

    switch (func_pc) {
    case 1:
	obj = var_ref(value.v.list[1]);
	free_var(value);

	if (!obj.is_object()) {
	    free_var(obj);
	    return make_error_pack(E_TYPE);
	} else if (!is_valid(obj) || db_object_has_flag2(obj, FLAG_RECYCLED)) {
	    free_var(obj);
	    return make_error_pack(E_INVARG);
	} else if (!controls2(progr, obj)) {
	    free_var(obj);
	    return make_error_pack(E_PERM);
	}

	db_set_object_flag2(obj, FLAG_RECYCLED);

	/* Recycle permanent and anonymous objects.
	 *
	 * At this point in time, an anonymous object may be in the
	 * root buffer and may be any color (purple, if the last
	 * operation was a decrement, black, if the last operation was
	 * an increment) (it _will_ have a reference, however -- a
	 * reference to itself, at least).
	 */

	data = (Var *)alloc_data(sizeof(Var));
	*data = var_ref(obj);
	args = new_list(0);
	e = call_verb(obj.is_obj() ? obj.v.obj : NOTHING, "recycle", obj, args, 0);
	/* e != E_INVIND */

	if (e == E_NONE) {
	    free_var(obj);
	    return make_call_pack(2, data);
	}
	/* else e == E_VERBNF or E_MAXREC; fall through */

	free_var(args);

	goto moving_contents;

    case 2:
	obj = var_ref(*data);
	free_var(value);

      moving_contents:

	if (!is_valid(obj)) {
	    free_var(obj);
	    free_var(*data);
	    free_data(data);
	    return no_var_pack();
	}

	if (obj.is_obj()) {
	    Objid c, oid = obj.v.obj;

	    while ((c = get_first(oid, db_for_all_contents)) != NOTHING)
		if (move_to_nothing(c))
		    return make_call_pack(2, data);

	    if (db_object_location(oid) != NOTHING && move_to_nothing(oid))
		/* Return to same case because this :exitfunc might add new */
		/* contents to OID or even move OID right back in. */
		return make_call_pack(2, data);

	    /* we can now be confident that OID has no contents and no location */

	    /* do the same thing for the inheritance hierarchy */
	    while ((c = get_first(oid, db_for_all_children)) != NOTHING) {
		Var cp = db_object_parents(c);
		Var op = db_object_parents(oid);
		if (cp.is_obj()) {
		    db_change_parents(Var::new_obj(c), op);
		}
		else {
		    int i = 1;
		    int j = 1;
		    List _new = new_list(0);
		    while (i <= cp.v.list[0].v.num && cp.v.list[i].v.obj != oid) {
			_new = setadd(_new, var_ref(cp.v.list[i]));
			i++;
		    }
		    if (op.is_obj()) {
			if (valid(op.v.obj))
			    _new = setadd(_new, var_ref(op));
		    }
		    else {
			while (j <= op.v.list[0].v.num) {
			    _new = setadd(_new, var_ref(op.v.list[j]));
			    j++;
			}
		    }
		    i++;
		    while (i <= cp.v.list[0].v.num) {
			_new = setadd(_new, var_ref(cp.v.list[i]));
			i++;
		    }
		    db_change_parents(Var::new_obj(c), _new);
		    free_var(_new);
		}
	    }

	    db_change_parents(obj, nothing);

	    incr_quota(db_object_owner(oid));

	    db_destroy_object(oid);

	    free_var(obj);
	    free_var(*data);
	    free_data(data);
	    return no_var_pack();
	}
	else if (obj.is_anon()) {
	    /* We'd like to run `db_change_parents()' to be consistent
	     * with the pattern laid out for permanent objects, but we
	     * can't because the object can be invalid at this point
	     * due to changes in parentage.
	     */
	    /*db_change_parents(obj, nothing, none);*/

	    incr_quota(db_object_owner2(obj));

	    db_destroy_anonymous_object(obj.v.anon);

	    free_var(obj);
	    free_var(*data);
	    free_data(data);
	    return no_var_pack();
	}
    }

    panic("Can't happen in BF_RECYCLE");
    return no_var_pack();
}

static void
bf_recycle_write(void *vdata)
{
    Objid *data = (Objid *)vdata;

    dbio_printf("bf_recycle data: oid = %d, cont = 0\n", *data);
}

static void *
bf_recycle_read(void)
{
    Objid *data = (Objid *)alloc_data(sizeof(*data));
    int dummy;

    /* I use a `dummy' variable here and elsewhere instead of the `*'
     * assignment-suppression syntax of `scanf' because it allows more
     * straightforward error checking; unfortunately, the standard says that
     * suppressed assignments are not counted in determining the returned value
     * of `scanf'...
     */
    if (dbio_scanf("bf_recycle data: oid = %d, cont = %d\n",
		   data, &dummy) == 2)
	return data;
    else
	return 0;
}

static package
bf_players(const List& arglist, Objid progr)
{				/* () */
    free_var(arglist);
    return make_var_pack(var_ref(db_all_users()));
}

static package
bf_is_player(const List& arglist, Objid progr)
{				/* (object) */
    Var r;
    Objid oid = arglist[1].v.obj;

    free_var(arglist);

    if (!valid(oid))
	return make_error_pack(E_INVARG);

    r = Var::new_int(is_user(oid));
    return make_var_pack(r);
}

static package
bf_set_player_flag(const List& arglist, Objid progr)
{				/* (object, yes/no) */
    Var obj;
    char flag;

    obj = arglist[1];
    flag = is_true(arglist[2]);

    free_var(arglist);

    if (!valid(obj.v.obj))
	return make_error_pack(E_INVARG);
    else if (!is_wizard(progr))
	return make_error_pack(E_PERM);

    if (flag) {
	db_set_object_flag(obj.v.obj, FLAG_USER);
    } else {
	boot_player(obj.v.obj);
	db_clear_object_flag(obj.v.obj, FLAG_USER);
    }
    return no_var_pack();
}

static package
bf_object_bytes(const List& arglist, Objid progr)
{
    Var obj = arglist[1];

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
	free_var(arglist);
	return make_error_pack(E_INVIND);
    } else if (!is_wizard(progr)) {
	free_var(arglist);
	return make_error_pack(E_PERM);
    } else {
	Var v = Var::new_int(db_object_bytes(obj));

	free_var(arglist);

	return make_var_pack(v);
    }
}

static package
bf_isa(const List& arglist, Objid progr)
{				/* (object, parent) */
    Var object = arglist[1];
    Var parent = arglist[2];

    if (!object.is_object() || !parent.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    }
    else if (!is_valid(object)) {
	free_var(arglist);
	return make_var_pack(Var::new_int(0));
    }
    else if (db_object_isa(object, parent)) {
	free_var(arglist);
	return make_var_pack(Var::new_int(1));
    }
    else {
	free_var(arglist);
	return make_var_pack(Var::new_int(0));
    }
}

Var nothing;			/* useful constant */
Var clear;			/* useful constant */
Var none;			/* useful constant */

void
register_objects(void)
{
    nothing = Var::new_obj(NOTHING);
    clear.type = TYPE_CLEAR;
    none.type = TYPE_NONE;

    register_function("toobj", 1, 1, bf_toobj, TYPE_ANY);
    register_function("typeof", 1, 1, bf_typeof, TYPE_ANY);
    register_function_with_read_write("create", 1, 4, bf_create,
				      bf_create_read, bf_create_write,
				      TYPE_ANY, TYPE_ANY, TYPE_ANY, TYPE_ANY);
    register_function_with_read_write("recycle", 1, 1, bf_recycle,
				      bf_recycle_read, bf_recycle_write,
				      TYPE_ANY);
    register_function("object_bytes", 1, 1, bf_object_bytes, TYPE_ANY);
    register_function("valid", 1, 1, bf_valid, TYPE_ANY);
    register_function("chparents", 2, 3, bf_chparent_chparents,
		      TYPE_ANY, TYPE_LIST, TYPE_LIST);
    register_function("chparent", 2, 3, bf_chparent_chparents,
		      TYPE_ANY, TYPE_OBJ, TYPE_LIST);
    register_function("parents", 1, 1, bf_parents, TYPE_ANY);
    register_function("parent", 1, 1, bf_parent, TYPE_ANY);
    register_function("children", 1, 1, bf_children, TYPE_ANY);
    register_function("ancestors", 1, 2, bf_ancestors,
		      TYPE_ANY, TYPE_ANY);
    register_function("descendants", 1, 2, bf_descendants,
		      TYPE_ANY, TYPE_ANY);
    register_function("max_object", 0, 0, bf_max_object);
    register_function("players", 0, 0, bf_players);
    register_function("is_player", 1, 1, bf_is_player, TYPE_OBJ);
    register_function("set_player_flag", 2, 2, bf_set_player_flag,
		      TYPE_OBJ, TYPE_ANY);
    register_function_with_read_write("move", 2, 2, bf_move,
				      bf_move_read, bf_move_write,
				      TYPE_OBJ, TYPE_OBJ);
    register_function("isa", 2, 2, bf_isa, TYPE_ANY, TYPE_ANY);
}
