# define INCLUDE_TELNET
# include "dgd.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "interpret.h"
# include "data.h"
# include "comm.h"

# ifndef TELOPT_LINEMODE
# define TELOPT_LINEMODE	34	/* linemode option */
# define LM_MODE		1
# define MODE_EDIT		0x01
# endif

# define MAXIACSEQLEN		7	/* longest IAC sequence sent */

typedef struct _user_ {
    uindex oindex;		/* associated object index */
    struct _user_ *prev;	/* preceding user */
    struct _user_ *next;	/* next user */
    struct _user_ *flush;	/* next in flush list */
    char flags;			/* connection flags */
    char state;			/* telnet state */
    short newlines;		/* # of newlines in input buffer */
    connection *conn;		/* connection */
    char *inbuf;		/* input buffer */
    array *extra;		/* object's extra value */
    string *outbuf;		/* output buffer string */
    ssizet inbufsz;		/* bytes in input buffer */
    ssizet osdone;		/* bytes of output string done */
} user;

/* flags */
# define CF_BINARY	0x00	/* binary connection */
# define  CF_UDP	0x02	/* receive UDP datagrams */
# define  CF_UDPDATA	0x04	/* UDP data received */
# define CF_TELNET	0x01	/* telnet connection */
# define  CF_ECHO	0x02	/* client echoes input */
# define  CF_GA		0x04	/* send GA after prompt */
# define  CF_SEENCR	0x08	/* just seen a CR */
# define  CF_PROMPT	0x10	/* prompt in telnet output */
# define CF_BLOCKED	0x20	/* input blocked */
# define CF_FLUSH	0x40	/* in flush list */
# define CF_ODONE	0x80	/* output done */

/* state */
# define TS_DATA	0
# define TS_IAC		1
# define TS_DO		2
# define TS_DONT	3
# define TS_WILL	4
# define TS_WONT	5
# define TS_SB		6
# define TS_SE		7

static user *users;		/* array of users */
static user *lastuser;		/* last user checked */
static user *freeuser;		/* linked list of free users */
static user *flush;		/* flush list */
static int maxusers;		/* max # of users */
static int nusers;		/* # of users */
static int odone;		/* # of users with output done */
static long newlines;		/* # of newlines in all input buffers */
static uindex this_user;	/* current user */

/*
 * NAME:	comm->init()
 * DESCRIPTION:	initialize communications
 */
bool comm_init(n, telnet_port, binary_port)
int n;
unsigned int telnet_port, binary_port;
{
    register int i;
    register user *usr;

    users = ALLOC(user, maxusers = n);
    for (i = n, usr = users + i; i > 0; --i) {
	--usr;
	usr->oindex = OBJ_NONE;
	usr->next = usr + 1;
    }
    users[n - 1].next = (user *) NULL;
    freeuser = usr;
    lastuser = (user *) NULL;
    flush = (user *) NULL;
    nusers = odone = newlines = 0;
    this_user = OBJ_NONE;

    return conn_init(n, telnet_port, binary_port);
}

/*
 * NAME:	comm->finish()
 * DESCRIPTION:	terminate connections
 */
void comm_finish()
{
    conn_finish();
}

/*
 * NAME:	comm->listen()
 * DESCRIPTION:	start listening on telnet port and binary port
 */
void comm_listen()
{
    conn_listen();
}

/*
 * NAME:	addtoflush()
 * DESCRIPTION:	add a user to the flush list
 */
static void addtoflush(usr, arr)
register user *usr;
register array *arr;
{
    usr->flags |= CF_FLUSH;
    usr->flush = flush;
    flush = usr;
    arr_ref(usr->extra = arr);

    /* remember initial buffer */
    if (d_get_elts(arr)[1].type == T_STRING) {
	str_ref(usr->outbuf = arr->elts[1].u.string);
    }
}

/*
 * NAME:	comm->new()
 * DESCRIPTION:	accept a new connection
 */
static user *comm_new(obj, conn, telnet)
object *obj;
connection *conn;
bool telnet;
{
    static char init[] = { (char) IAC, (char) WONT, (char) TELOPT_ECHO,
			   (char) IAC, (char) DO,   (char) TELOPT_LINEMODE };
    register user *usr;
    dataspace *data;
    array *arr;
    value val;

    if (obj->flags & O_SPECIAL) {
	error("User object is already special purpose");
    }

    usr = freeuser;
    freeuser = usr->next;
    if (lastuser != (user *) NULL) {
	usr->prev = lastuser->prev;
	usr->prev->next = usr;
	usr->next = lastuser;
	lastuser->prev = usr;
    } else {
	usr->prev = usr;
	usr->next = usr;
	lastuser = usr;
    }

    d_wipe_extravar(data = o_dataspace(obj));
    arr = arr_new(data, 3L);
    arr->elts[0] = zero_int;
    arr->elts[1] = arr->elts[2] = nil_value;
    PUT_ARRVAL_NOREF(&val, arr);
    d_set_extravar(data, &val);

    usr->oindex = obj->index;
    obj->flags |= O_USER;
    obj->etabi = usr - users;
    usr->conn = conn;
    usr->outbuf = (string *) NULL;
    usr->osdone = 0;
    if (telnet) {
	/* initialize connection */
	usr->flags = CF_TELNET | CF_ECHO;
	usr->state = TS_DATA;
	usr->newlines = 0;
	usr->inbufsz = 0;
	m_static();
	usr->inbuf = ALLOC(char, INBUF_SIZE + 1);
	*usr->inbuf++ = LF;	/* sentinel */
	m_dynamic();
	addtoflush(usr, arr);

	arr->elts[0].u.number = CF_ECHO;
	PUT_STRVAL_NOREF(&val, str_new(init, (long) sizeof(init)));
	d_assign_elt(data, arr, &arr->elts[1], &val);
	obj->flags |= O_PENDIO;
    } else {
	usr->flags = 0;
    }
    nusers++;

    return usr;
}

/*
 * NAME:	comm->del()
 * DESCRIPTION:	delete a connection
 */
static void comm_del(f, usr, obj, force)
register frame *f;
register user *usr;
object *obj;
bool force;
{
    dataspace *data;
    uindex olduser;

    data = o_dataspace(obj);
    if (!(usr->flags & CF_FLUSH)) {
	addtoflush(usr, d_get_extravar(data)->u.array);
    }

    obj->flags &= ~O_USER;
    olduser = this_user;
    if (ec_push((ec_ftn) NULL)) {
	this_user = olduser;
	error((char *) NULL);
    } else {
	this_user = obj->index;
	PUSH_INTVAL(f, force);
	if (i_call(f, obj, (array *) NULL, "close", 5, TRUE, 1)) {
	    i_del_value(f->sp++);
	}
	this_user = olduser;
	ec_pop();
    }
}

/*
 * NAME:	comm->write()
 * DESCRIPTION:	add bytes to output buffer
 */
static int comm_write(usr, obj, str, text, len)
register user *usr;
object *obj;
register string *str;
char *text;
unsigned int len;
{
    dataspace *data;
    array *arr;
    register value *v;
    register ssizet osdone, olen;
    value val;

    arr = d_get_extravar(data = o_dataspace(obj))->u.array;
    if (!(usr->flags & CF_FLUSH)) {
	addtoflush(usr, arr);
    }

    v = arr->elts + 1;
    if (v->type == T_STRING) {
	/* append to existing buffer */
	osdone = (usr->outbuf == v->u.string) ? usr->osdone : 0;
	olen = v->u.string->len - osdone;
	if (olen + len > MAX_STRLEN) {
	    len = MAX_STRLEN - olen;
	    if (len == 0 ||
		((usr->flags & CF_TELNET) && text[0] == (char) IAC &&
		 len < MAXIACSEQLEN)) {
		return 0;
	    }
	}
	str = str_new((char *) NULL, (long) olen + len);
	memcpy(str->text, v->u.string->text + osdone, olen);
	memcpy(str->text + olen, text, len);
    } else {
	/* create new buffer */
	if (usr->flags & CF_ODONE) {
	    usr->flags &= ~CF_ODONE;
	    --odone;
	}
	obj->flags |= O_PENDIO;
	if (str == (string *) NULL) {
	    str = str_new(text, (long) len);
	}
    }

    PUT_STRVAL_NOREF(&val, str);
    d_assign_elt(data, arr, v, &val);
    return len;
}

/*
 * NAME:	comm->send()
 * DESCRIPTION:	send a message to a user
 */
int comm_send(obj, str)
object *obj;
string *str;
{
    register user *usr;

    usr = &users[EINDEX(obj->etabi)];
    if (usr->flags & CF_TELNET) {
	char outbuf[OUTBUF_SIZE];
	register char *p, *q;
	register unsigned int len, size, n, length;

	/*
	 * telnet connection
	 */
	p = str->text;
	len = str->len;
	q = outbuf;
	size = 0;
	length = 0;
	for (;;) {
	    if (len == 0 || size >= OUTBUF_SIZE - 1 || UCHAR(*p) == IAC) {
		n = comm_write(usr, obj, (string *) NULL, outbuf, size);
		if (n != size) {
		    /*
		     * count how many bytes of original string were written
		     */
		    for (n = size - n; n != 0; --n) {
			if (*--p == *--q) {
			    len++;
			    if (UCHAR(*p) == IAC) {
				break;
			    }
			} else if (*q == CR) {
			    /* inserted CR */
			    p++;
			} else {
			    /* skipped char */
			    q++;
			    len++;
			}
		    }
		    return str->len - len;
		}
		if (len == 0) {
		    return str->len;
		}
		size = 0;
		q = outbuf;
	    }
	    if (UCHAR(*p) == IAC) {
		/*
		 * double the telnet IAC character
		 */
		*q++ = (char) IAC;
		size++;
	    } else if (*p == LF) {
		/*
		 * insert CR before LF
		 */
		*q++ = CR;
		size++;
	    } else if ((*p & 0x7f) < ' ' && *p != HT && *p != BEL && *p != BS) {
		/*
		 * illegal character
		 */
		p++;
		--len;
		continue;
	    }
	    *q++ = *p++;
	    --len;
	    size++;
	}
    } else {
	/*
	 * binary connection
	 */
	return comm_write(usr, obj, str, str->text, str->len);
    }
}

/*
 * NAME:	comm->udpsend()
 * DESCRIPTION:	send a message on the UDP channel of a binary connection
 */
int comm_udpsend(obj, str)
object *obj;
string *str;
{
    register user *usr;
    dataspace *data;
    array *arr;
    register value *v;
    value val;

    usr = &users[EINDEX(obj->etabi)];
    if ((usr->flags & (CF_TELNET | CF_UDP)) != CF_UDP) {
	error("Object has no UDP channel");
    }
    if (!(usr->flags & CF_UDPDATA)) {
	error("No datagrams received yet");
    }

    arr = d_get_extravar(data = obj->data)->u.array;
    if (!(usr->flags & CF_FLUSH)) {
	addtoflush(usr, arr);
    }

    v = arr->elts + 2;
    if (v->type == T_STRING) {
	return 0;	/* datagram queued already */
    }
    obj->flags |= O_PENDIO;
    PUT_STRVAL_NOREF(&val, str);
    d_assign_elt(data, arr, v, &val);

    return str->len;
}

/*
 * NAME:	comm->echo()
 * DESCRIPTION:	turn on/off input echoing for a user
 */
bool comm_echo(obj, echo)
object *obj;
int echo;
{
    register user *usr;
    register dataspace *data;
    array *arr;
    register value *v;

    usr = &users[EINDEX(obj->etabi)];
    if (usr->flags & CF_TELNET) {
	arr = d_get_extravar(data = obj->data)->u.array;
	v = d_get_elts(arr);
	if (echo != (v->u.number & CF_ECHO) >> 1) {
	    value val;

	    if (!(usr->flags & CF_FLUSH)) {
		addtoflush(usr, arr);
	    }
	    val = *v;
	    val.u.number ^= CF_ECHO;
	    d_assign_elt(data, arr, v, &val);
	}
	return TRUE;
    }
    return FALSE;
}

/*
 * NAME:	comm->block()
 * DESCRIPTION:	suspend or release input from a user
 */
void comm_block(obj, block)
object *obj;
int block;
{
    register user *usr;
    register dataspace *data;
    array *arr;
    register value *v;

    usr = &users[EINDEX(obj->etabi)];
    if (usr->flags & CF_TELNET) {
	arr = d_get_extravar(data = obj->data)->u.array;
	v = d_get_elts(arr);
	if (block != (v->u.number & CF_BLOCKED) >> 5) {
	    value val;

	    if (!(usr->flags & CF_FLUSH)) {
		addtoflush(usr, arr);
	    }
	    val = *v;
	    val.u.number ^= CF_BLOCKED;
	    d_assign_elt(data, arr, v, &val);
	}
    }
}

/*
 * NAME:	comm->uflush()
 * DESCRIPTION:	flush output buffers for a single user only
 */
static void comm_uflush(usr, obj, data, arr)
register user *usr;
object *obj;
register dataspace *data;
array *arr;
{
    register value *v;
    register int n;

    v = d_get_elts(arr);

    if (v[1].type == T_STRING) {
	if (conn_wrdone(usr->conn)) {
	    n = conn_write(usr->conn, v[1].u.string->text + usr->osdone,
			   v[1].u.string->len - usr->osdone);
	    if (n >= 0) {
		n += usr->osdone;
		if (n == v[1].u.string->len) {
		    /* buffer fully drained */
		    n = 0;
		    usr->flags |= CF_ODONE;
		    odone++;
		    obj->flags &= ~O_PENDIO;
		    d_assign_elt(data, arr, &v[1], &nil_value);
		}
		usr->osdone = n;
	    } else {
		/* wait for conn_read() to discover the problem */
		obj->flags &= ~O_PENDIO;
	    }
	}
    } else {
	/* just a datagram */
	obj->flags &= ~O_PENDIO;
    }

    if (v[2].type == T_STRING) {
	conn_udpwrite(usr->conn, v[2].u.string->text, v[2].u.string->len);
	d_assign_elt(data, arr, &v[2], &nil_value);
    }
}

/*
 * NAME:	comm->flush()
 * DESCRIPTION:	flush state, output and connections
 */
void comm_flush()
{
    register user *usr;
    object *obj;
    array *arr;
    register value *v;

    while (flush != (user *) NULL) {
	usr = flush;
	flush = usr->flush;

	/*
	 * status change
	 */
	obj = OBJ(usr->oindex);
	arr = usr->extra;
	v = arr->elts;
	if (usr->flags & CF_TELNET) {
	    if ((v->u.number ^ usr->flags) & CF_ECHO) {
		char buf[3];

		/* change echo */
		buf[0] = (char) IAC;
		buf[1] = (v->u.number & CF_ECHO) ? WONT : WILL;
		buf[2] = TELOPT_ECHO;
		if (comm_write(usr, obj, (string *) NULL, buf, 3) != 0) {
		    usr->flags ^= CF_ECHO;
		}
	    }
	    if (usr->flags & CF_PROMPT) {
		usr->flags &= ~CF_PROMPT;
		if ((usr->flags & CF_GA) && v[1].type == T_STRING &&
		    usr->outbuf != v[1].u.string) {
		    static char ga[] = { (char) IAC, (char) GA };

		    /* append go-ahead */
		    comm_write(usr, obj, (string *) NULL, ga, 2);
		}
	    }
	}
	if ((v->u.number ^ usr->flags) & CF_BLOCKED) {
	    usr->flags ^= CF_BLOCKED;
	    conn_block(usr->conn, ((usr->flags & CF_BLOCKED) != 0));
	}

	/*
	 * write
	 */
	if (usr->outbuf != (string *) NULL) {
	    if (usr->outbuf != v[1].u.string) {
		usr->osdone = 0;	/* new mesg before buffer drained */
	    }
	    str_del(usr->outbuf);
	    usr->outbuf = (string *) NULL;
	}
	if (obj->flags & O_PENDIO) {
	    comm_uflush(usr, obj, obj->data, arr);
	}

	/*
	 * disconnect
	 */
	if ((obj->flags & O_SPECIAL) != O_USER) {
	    d_wipe_extravar(obj->data);
	    conn_del(usr->conn);
	    if (usr->flags & CF_TELNET) {
		newlines -= usr->newlines;
		FREE(usr->inbuf - 1);
	    }
	    if (usr->flags & CF_ODONE) {
		--odone;
	    }

	    usr->oindex = OBJ_NONE;
	    if (usr->next == usr) {
		lastuser = (user *) NULL;
	    } else {
		usr->next->prev = usr->prev;
		usr->prev->next = usr->next;
		if (usr == lastuser) {
		    lastuser = usr->next;
		}
	    }
	    usr->next = freeuser;
	    freeuser = usr;
	    --nusers;
	}

	arr_del(arr);
	usr->flags &= ~CF_FLUSH;
    }
}

/*
 * NAME:	comm->receive()
 * DESCRIPTION:	receive a message from a user
 */
void comm_receive(f, timeout, mtime)
register frame *f;
Uint timeout;
unsigned int mtime;
{
    static char intr[] =	{ '\177' };
    static char brk[] =		{ '\034' };
    static char ayt[] =		{ CR, LF, '[', 'Y', 'e', 's', ']', CR, LF };
    static char tm[] =		{ (char) IAC, (char) WONT, (char) TELOPT_TM };
    static char will_sga[] =	{ (char) IAC, (char) WILL, (char) TELOPT_SGA };
    static char wont_sga[] =	{ (char) IAC, (char) WONT, (char) TELOPT_SGA };
    static char mode_edit[] =	{ (char) IAC, (char) SB,
				  (char) TELOPT_LINEMODE, (char) LM_MODE,
				  (char) MODE_EDIT, (char) IAC, (char) SE };
    char buffer[BINBUF_SIZE];
    connection *conn;
    object *obj;
    register user *usr;
    register int n, i, state, nls;
    register char *p, *q;

    if (newlines != 0 || odone != 0) {
	timeout = mtime = 0;
    }
    n = conn_select(timeout, mtime);
    if (n <= 0 && newlines == 0 && odone == 0) {
	/*
	 * call_out to do, or timeout
	 */
	return;
    }

    if (ec_push(errhandler)) {
	endthread();
	this_user = OBJ_NONE;
	return;
    }

    if (nusers < maxusers) {
	/*
	 * accept new telnet connection
	 */
	conn = conn_tnew();
	if (conn != (connection *) NULL) {
	    if (ec_push((ec_ftn) NULL)) {
		conn_del(conn);		/* delete connection */
		error((char *) NULL);	/* pass on error */
	    }
	    call_driver_object(f, "telnet_connect", 0);
	    if (f->sp->type != T_OBJECT) {
		fatal("driver->telnet_connect() did not return a persistent object");
	    }
	    obj = OBJ(f->sp->oindex);
	    f->sp++;
	    usr = comm_new(obj, conn, TRUE);
	    ec_pop();
	    endthread();

	    usr->flags |= CF_PROMPT;
	    addtoflush(usr, d_get_extravar(o_dataspace(obj))->u.array);
	    this_user = obj->index;
	    if (i_call(f, obj, (array *) NULL, "open", 4, TRUE, 0)) {
		i_del_value(f->sp++);
		endthread();
	    }
	    this_user = OBJ_NONE;
	}
    }

    while (nusers < maxusers) {
	/*
	 * accept new binary connection
	 */
	conn = conn_bnew();
	if (conn == (connection *) NULL) {
	    break;
	}

	if (ec_push((ec_ftn) NULL)) {
	    conn_del(conn);		/* delete connection */
	    error((char *) NULL);	/* pass on error */
	}
	call_driver_object(f, "binary_connect", 0);
	if (f->sp->type != T_OBJECT) {
	    fatal("driver->binary_connect() did not return a persistent object");
	}
	obj = OBJ(f->sp->oindex);
	f->sp++;
	usr = comm_new(obj, conn, FALSE);
	ec_pop();
	endthread();

	this_user = obj->index;
	if (i_call(f, obj, (array *) NULL, "open", 4, TRUE, 0)) {
	    if (VAL_TRUE(f->sp)) {
		i_del_value(f->sp);
		if ((obj->flags & O_SPECIAL) == O_USER) {
		    /* open UDP channel */
		    usr->flags |= CF_UDP;
		    conn_udp(conn);
		}
	    }
	    f->sp++;
	    endthread();
	}
	this_user = OBJ_NONE;
    }

    for (i = nusers; i > 0; --i) {
	usr = lastuser;
	lastuser = usr->next;

	obj = OBJ(usr->oindex);
	if (obj->flags & O_PENDIO) {
	    dataspace *data;

	    data = o_dataspace(obj);
	    comm_uflush(usr, obj, data, d_get_extravar(data)->u.array);
	}
	if (usr->flags & CF_ODONE) {
	    /* callback */
	    usr->flags &= ~CF_ODONE;
	    --odone;
	    this_user = obj->index;
	    if (i_call(f, obj, (array *) NULL, "message_done", 12, TRUE, 0)) {
		i_del_value(f->sp++);
		endthread();
	    }
	    this_user = OBJ_NONE;
	    if (obj->count == 0) {
		break;	/* continue, unless the connection was closed */
	    }
	}

	if (usr->flags & CF_BLOCKED) {
	    continue;	/* no input on this connection */
	}

	if (usr->flags & CF_TELNET) {
	    /*
	     * telnet connection
	     */
	    if (usr->inbufsz != INBUF_SIZE) {
		p = usr->inbuf + usr->inbufsz;
		n = conn_read(usr->conn, p, INBUF_SIZE - usr->inbufsz);
		if (n < 0) {
		    if (usr->inbufsz != 0) {
			if (p[-1] != LF) {
			    /*
			     * add a newline at the end
			     */
			    *p = LF;
			    n = 1;
			}
		    } else if (!(obj->flags & O_PENDIO)) {
			/*
			 * empty buffer, no more input, no pending output
			 */
			comm_del(f, usr, obj, FALSE);
			endthread();	/* this cannot be in comm_del() */
			break;
		    }
		}

		state = usr->state;
		nls = usr->newlines;
		q = p;
		while (n > 0) {
		    switch (state) {
		    case TS_DATA:
			switch (UCHAR(*p)) {
			case '\0':
			    usr->flags &= ~CF_SEENCR;
			    break;

			case IAC:
			    state = TS_IAC;
			    break;

			case BS:
			case 0x7f:
			    if (q[-1] != LF) {
				--q;
			    }
			    usr->flags &= ~CF_SEENCR;
			    break;

			case CR:
			    nls++;
			    newlines++;
			    *q++ = LF;
			    usr->flags |= CF_SEENCR;
			    break;

			case LF:
			    if ((usr->flags & CF_SEENCR) != 0) {
				usr->flags &= ~CF_SEENCR;
				break;
			    }
			    nls++;
			    newlines++;
			    /* fall through */
			default:
			    *q++ = *p;
			    usr->flags &= ~CF_SEENCR;
			    break;
			}
			break;

		    case TS_IAC:
			switch (UCHAR(*p)) {
			case IAC:
			    *q++ = *p;
			    state = TS_DATA;
			    break;

			case DO:
			    state = TS_DO;
			    break;

			case DONT:
			    state = TS_DONT;
			    break;

			case WILL:
			    state = TS_WILL;
			    break;

			case WONT:
			    state = TS_WONT;
			    break;

			case SB:
			    state = TS_SB;
			    break;

			case IP:
			    comm_write(usr, obj, (string *) NULL, intr,
				       sizeof(intr));
			    state = TS_DATA;
			    break;

			case BREAK:
			    comm_write(usr, obj, (string *) NULL, brk,
				       sizeof(brk));
			    state = TS_DATA;
			    break;

			case AYT:
			    comm_write(usr, obj, (string *) NULL, ayt,
				       sizeof(ayt));
			    state = TS_DATA;
			    break;

			default:
			    /* let's hope it wasn't important */
			    state = TS_DATA;
			    break;
			}
			break;

		    case TS_DO:
			if (UCHAR(*p) == TELOPT_TM) {
			    comm_write(usr, obj, (string *) NULL, tm,
				       sizeof(tm));
			} else if (UCHAR(*p) == TELOPT_SGA) {
			    usr->flags &= ~CF_GA;
			    comm_write(usr, obj, (string *) NULL, will_sga,
				       sizeof(will_sga));
			}
			state = TS_DATA;
			break;

		    case TS_DONT:
			if (UCHAR(*p) == TELOPT_SGA) {
			    usr->flags |= CF_GA;
			    comm_write(usr, obj, (string *) NULL, wont_sga,
				       sizeof(wont_sga));
			}
			state = TS_DATA;
			break;

		    case TS_WILL:
			if (UCHAR(*p) == TELOPT_LINEMODE) {
			    /* linemode confirmed; now request editing */
			    comm_write(usr, obj, (string *) NULL, mode_edit,
				       sizeof(mode_edit));
			}
			/* fall through */
		    case TS_WONT:
			state = TS_DATA;
			break;

		    case TS_SB:
			/* skip to the end */
			if (UCHAR(*p) == IAC) {
			    state = TS_SE;
			}
			break;

		    case TS_SE:
			if (UCHAR(*p) == SE) {
			    /* end of subnegotiation */
			    state = TS_DATA;
			} else {
			    state = TS_SB;
			}
			break;
		    }
		    p++;
		    --n;
		}
		usr->state = state;
		usr->newlines = nls;
		usr->inbufsz = q - usr->inbuf;
		if (nls == 0) {
		    continue;
		}

		/*
		 * input terminated by \n
		 */
		p = (char *) memchr(q = usr->inbuf, LF, usr->inbufsz);
		usr->newlines--;
		--newlines;
		n = p - usr->inbuf;
		p++;			/* skip \n */
		usr->inbufsz -= n + 1;

		PUSH_STRVAL(f, str_new(usr->inbuf, (long) n));
		for (n = usr->inbufsz; n != 0; --n) {
		    *q++ = *p++;
		}
	    } else {
		/*
		 * input buffer full
		 */
		n = usr->inbufsz;
		usr->inbufsz = 0;
		PUSH_STRVAL(f, str_new(usr->inbuf, (long) n));
	    }
	    usr->flags |= CF_PROMPT;
	    if (!(usr->flags & CF_FLUSH)) {
		addtoflush(usr, d_get_extravar(o_dataspace(obj))->u.array);
	    }
	} else {
	    /*
	     * binary connection
	     */
	    if (usr->flags & CF_UDP) {
		n = conn_udpread(usr->conn, buffer, BINBUF_SIZE);
		if (n >= 0) {
		    /*
		     * received datagram
		     */
		    usr->flags |= CF_UDPDATA;
		    PUSH_STRVAL(f, str_new(buffer, (long) n));
		    this_user = obj->index;
		    if (i_call(f, obj, (array *) NULL, "receive_datagram", 16,
			       TRUE, 1)) {
			i_del_value(f->sp++);
			endthread();
		    }
		    this_user = OBJ_NONE;
		}
	    }

	    n = conn_read(usr->conn, p = buffer, BINBUF_SIZE);
	    if (n <= 0) {
		if (n < 0 && !(obj->flags & O_PENDIO)) {
		    /*
		     * no more input and no pending output
		     */
		    comm_del(f, usr, obj, FALSE);
		    endthread();	/* this cannot be in comm_del() */
		    break;
		}
		continue;
	    }

	    PUSH_STRVAL(f, str_new(buffer, (long) n));
	}

	this_user = obj->index;
	if (i_call(f, obj, (array *) NULL, "receive_message", 15, TRUE, 1)) {
	    i_del_value(f->sp++);
	    endthread();
	}
	this_user = OBJ_NONE;
	break;
    }

    ec_pop();
    comm_flush();
}

/*
 * NAME:	comm->ip_number()
 * DESCRIPTION:	return the ip number of a user (as a string)
 */
string *comm_ip_number(obj)
object *obj;
{
    char *ipnum;

    ipnum = conn_ipnum(users[EINDEX(obj->etabi)].conn);
    return str_new(ipnum, (long) strlen(ipnum));
}

/*
 * NAME:	comm->ip_name()
 * DESCRIPTION:	return the ip name of a user
 */
string *comm_ip_name(obj)
object *obj;
{
    char *ipname;

    ipname = conn_ipname(users[EINDEX(obj->etabi)].conn);
    return str_new(ipname, (long) strlen(ipname));
}

/*
 * NAME:	comm->close()
 * DESCRIPTION:	remove a user
 */
void comm_close(f, obj)
frame *f;
object *obj;
{
    comm_del(f, &users[EINDEX(obj->etabi)], obj, TRUE);
}

/*
 * NAME:	comm->user()
 * DESCRIPTION:	return the current user
 */
object *comm_user()
{
    object *obj;

    return (this_user != OBJ_NONE && (obj=OBJR(this_user))->count != 0) ?
	    obj : (object *) NULL;
}

/*
 * NAME:	comm->users()
 * DESCRIPTION:	return an array with all user objects
 */
array *comm_users(data)
dataspace *data;
{
    array *a;
    register int i, n;
    register user *usr;
    register value *v;
    register object *obj;

    n = 0;
    for (i = nusers, usr = users; i > 0; usr++) {
	if (usr->oindex != OBJ_NONE) {
	    --i;
	    if (OBJR(usr->oindex)->count != 0) {
		n++;
	    }
	}
    }

    a = arr_new(data, (long) n);
    v = a->elts;
    for (usr = users; n > 0; usr++) {
	if (usr->oindex != OBJ_NONE && (obj=OBJR(usr->oindex))->count != 0) {
	    PUT_OBJVAL(v, obj);
	    v++;
	    --n;
	}
    }
    return a;
}
