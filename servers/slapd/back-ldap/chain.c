/* chain.c - chain LDAP operations */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2005 The OpenLDAP Foundation.
 * Portions Copyright 2003 Howard Chu.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "back-ldap.h"

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
#define SLAP_CH_RESOLVE_SHIFT				SLAP_CONTROL_SHIFT
#define SLAP_CH_RESOLVE_MASK				(0x3 << SLAP_CH_RESOLVE_SHIFT)
#define SLAP_CH_RESOLVE_CHAINING_PREFERRED		(LDAP_CHAINING_PREFERRED << SLAP_CH_RESOLVE_SHIFT)
#define SLAP_CH_RESOLVE_CHAINING_REQUIRED		(LDAP_CHAINING_REQUIRED << SLAP_CH_RESOLVE_SHIFT)
#define SLAP_CH_RESOLVE_REFERRALS_PREFERRED		(LDAP_REFERRALS_PREFERRED << SLAP_CH_RESOLVE_SHIFT)
#define SLAP_CH_RESOLVE_REFERRALS_REQUIRED		(LDAP_REFERRALS_REQUIRED << SLAP_CH_RESOLVE_SHIFT)
#define SLAP_CH_RESOLVE_DEFAULT				SLAP_CH_RESOLVE_CHAINING_PREFERRED
#define	SLAP_CH_CONTINUATION_SHIFT			(SLAP_CH_RESOLVE_SHIFT + 2)
#define SLAP_CH_CONTINUATION_MASK			(0x3 << SLAP_CH_CONTINUATION_SHIFT)
#define SLAP_CH_CONTINUATION_CHAINING_PREFERRED		(LDAP_CHAINING_PREFERRED << SLAP_CH_CONTINUATION_SHIFT)
#define SLAP_CH_CONTINUATION_CHAINING_REQUIRED		(LDAP_CHAINING_REQUIRED << SLAP_CH_CONTINUATION_SHIFT)
#define SLAP_CH_CONTINUATION_REFERRALS_PREFERRED	(LDAP_REFERRALS_PREFERRED << SLAP_CH_CONTINUATION_SHIFT)
#define SLAP_CH_CONTINUATION_REFERRALS_REQUIRED		(LDAP_REFERRALS_REQUIRED << SLAP_CH_CONTINUATION_SHIFT)
#define SLAP_CH_CONTINUATION_DEFAULT			SLAP_CH_CONTINUATION_CHAINING_PREFERRED

#define o_chaining			o_ctrlflag[sc_chainingBehavior]
#define get_chaining(op)		((op)->o_chaining & SLAP_CONTROL_MASK)
#define get_chainingBehavior(op)	((op)->o_chaining & (SLAP_CH_RESOLVE_MASK|SLAP_CH_CONTINUATION_MASK))
#define get_resolveBehavior(op)		((op)->o_chaining & SLAP_CH_RESOLVE_MASK)
#define get_continuationBehavior(op)	((op)->o_chaining & SLAP_CH_CONTINUATION_MASK)
#endif /*  LDAP_CONTROL_X_CHAINING_BEHAVIOR */

#define	LDAP_CH_NONE			((void *)(0))
#define	LDAP_CH_RES			((void *)(1))
#define LDAP_CH_ERR			((void *)(2))

static int		sc_chainingBehavior;
static BackendInfo	*lback;

static int
ldap_chain_operational( Operation *op, SlapReply *rs )
{
	/* Trap entries generated by back-ldap.
	 * 
	 * FIXME: we need a better way to recognize them; a cleaner
	 * solution would be to be able to intercept the response
	 * of be_operational(), so that we can divert only those
	 * calls that fail because operational attributes were
	 * requested for entries that do not belong to the underlying
	 * database.  This fix is likely to intercept also entries
	 * generated by back-perl and so. */
	if ( rs->sr_entry->e_private == NULL ) {
		return 0;
	}

	return SLAP_CB_CONTINUE;
}

/*
 * Search specific response that strips entryDN from entries
 */
static int
ldap_chain_cb_search_response( Operation *op, SlapReply *rs )
{
	assert( op->o_tag == LDAP_REQ_SEARCH );

	/* if in error, don't proceed any further */
	if ( op->o_callback->sc_private == LDAP_CH_ERR ) {
		return 0;
	}

	if ( rs->sr_type == REP_SEARCH ) {
		Attribute	**ap = &rs->sr_entry->e_attrs;

		for ( ; *ap != NULL; ap = &(*ap)->a_next ) {
			/* will be generated later by frontend
			 * (a cleaner solution would be that
			 * the frontend checks if it already exists */
			if ( ad_cmp( (*ap)->a_desc, slap_schema.si_ad_entryDN ) == 0 )
			{
				Attribute *a = *ap;

				*ap = (*ap)->a_next;
				attr_free( a );

				/* there SHOULD be one only! */
				break;
			}
		}
		
		return SLAP_CB_CONTINUE;

	} else if ( rs->sr_type == REP_SEARCHREF ) {
		/* if we get it here, it means the library was unable
		 * to chase the referral... */

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
		if ( get_chaining( op ) > SLAP_CONTROL_IGNORED ) {
			switch ( get_continuationBehavior( op ) ) {
			case SLAP_CH_RESOLVE_CHAINING_REQUIRED:
				op->o_callback->sc_private = LDAP_CH_ERR;
				return -1;

			default:
				break;
			}
		}
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */
		return SLAP_CB_CONTINUE;

	} else if ( rs->sr_type == REP_RESULT ) {
		/* back-ldap tried to send result */
		op->o_callback->sc_private = LDAP_CH_RES;
	}

	return 0;
}

/*
 * Dummy response that simply traces if back-ldap tried to send 
 * anything to the client
 */
static int
ldap_chain_cb_response( Operation *op, SlapReply *rs )
{
	/* if in error, don't proceed any further */
	if ( op->o_callback->sc_private == LDAP_CH_ERR ) {
		return 0;
	}

	if ( rs->sr_type == REP_RESULT ) {
		op->o_callback->sc_private = LDAP_CH_RES;

	} else if ( op->o_tag == LDAP_REQ_SEARCH && rs->sr_type == REP_SEARCH )
	{
		/* strip the entryDN attribute, but keep returning results */
		(void)ldap_chain_cb_search_response( op, rs );
	}

	return SLAP_CB_CONTINUE;
}

static int
ldap_chain_op(
	Operation	*op,
	SlapReply	*rs,
	int		( *op_f )( Operation *op, SlapReply *rs ), 
	BerVarray	ref )
{
	slap_overinst	*on = (slap_overinst *) op->o_bd->bd_info;
	struct ldapinfo	li, *lip = (struct ldapinfo *)on->on_bi.bi_private;

	/* NOTE: returned if ref is empty... */
	int		rc = LDAP_OTHER;

	if ( lip->url != NULL ) {
		op->o_bd->be_private = on->on_bi.bi_private;
		return ( *op_f )( op, rs );
	}

	li = *lip;
	op->o_bd->be_private = &li;

	/* if we parse the URI then by no means 
	 * we can cache stuff or reuse connections, 
	 * because in back-ldap there's no caching
	 * based on the URI value, which is supposed
	 * to be set once for all (correct?) */
	op->o_do_not_cache = 1;

	for ( ; !BER_BVISNULL( ref ); ref++ ) {
		LDAPURLDesc	*srv;
		char		*save_dn;
			
		/* We're setting the URI of the first referral;
		 * what if there are more?

Document: draft-ietf-ldapbis-protocol-27.txt

4.1.10. Referral 
   ...
   If the client wishes to progress the operation, it MUST follow the 
   referral by contacting one of the supported services. If multiple 
   URIs are present, the client assumes that any supported URI may be 
   used to progress the operation. 

		 * so we actually need to follow exactly one,
		 * and we can assume any is fine.
		 */
	
		/* parse reference and use 
		 * proto://[host][:port]/ only */
		rc = ldap_url_parse_ext( ref->bv_val, &srv );
		if ( rc != LDAP_URL_SUCCESS ) {
			/* try next */
			rc = LDAP_OTHER;
			continue;
		}

		/* remove DN essentially because later on 
		 * ldap_initialize() will parse the URL 
		 * as a comma-separated URL list */
		save_dn = srv->lud_dn;
		srv->lud_dn = "";
		srv->lud_scope = LDAP_SCOPE_DEFAULT;
		li.url = ldap_url_desc2str( srv );
		srv->lud_dn = save_dn;
		ldap_free_urldesc( srv );

		if ( li.url == NULL ) {
			/* try next */
			rc = LDAP_OTHER;
			continue;
		}

		rc = ( *op_f )( op, rs );

		ldap_memfree( li.url );
		li.url = NULL;
		
		if ( rc == LDAP_SUCCESS && rs->sr_err == LDAP_SUCCESS ) {
			break;
		}
	}

	return rc;
}

static int
ldap_chain_response( Operation *op, SlapReply *rs )
{
	slap_overinst	*on = (slap_overinst *) op->o_bd->bd_info;
	void		*private = op->o_bd->be_private;
	slap_callback	*sc = op->o_callback,
			sc2 = { 0 };
	int		rc = 0;
	int		cache = op->o_do_not_cache;
	BerVarray	ref;
	struct berval	ndn = op->o_ndn;

	struct ldapinfo	li, *lip = (struct ldapinfo *)on->on_bi.bi_private;

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
	int		sr_err = rs->sr_err;
	slap_reply_t	sr_type = rs->sr_type;
	slap_mask_t	chain_mask = 0;
	ber_len_t	chain_shift = 0;
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */

	if ( rs->sr_err != LDAP_REFERRAL && rs->sr_type != REP_SEARCHREF ) {
		return SLAP_CB_CONTINUE;
	}

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
	if ( rs->sr_err == LDAP_REFERRAL && get_chaining( op ) > SLAP_CONTROL_IGNORED ) {
		switch ( get_resolveBehavior( op ) ) {
		case SLAP_CH_RESOLVE_REFERRALS_PREFERRED:
		case SLAP_CH_RESOLVE_REFERRALS_REQUIRED:
			return SLAP_CB_CONTINUE;

		default:
			chain_mask = SLAP_CH_RESOLVE_MASK;
			chain_shift = SLAP_CH_RESOLVE_SHIFT;
			break;
		}

	} else if ( rs->sr_type == REP_SEARCHREF && get_chaining( op ) > SLAP_CONTROL_IGNORED ) {
		switch ( get_continuationBehavior( op ) ) {
		case SLAP_CH_CONTINUATION_REFERRALS_PREFERRED:
		case SLAP_CH_CONTINUATION_REFERRALS_REQUIRED:
			return SLAP_CB_CONTINUE;

		default:
			chain_mask = SLAP_CH_CONTINUATION_MASK;
			chain_shift = SLAP_CH_CONTINUATION_SHIFT;
			break;
		}
	}
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */

	/*
	 * TODO: add checks on who/when chain operations; e.g.:
	 *   a) what identities are authorized
	 *   b) what request DN (e.g. only chain requests rooted at <DN>)
	 *   c) what referral URIs
	 *   d) what protocol scheme (e.g. only ldaps://)
	 *   e) what ssf
	 */

	ref = rs->sr_ref;
	rs->sr_ref = NULL;

	/* we need this to know if back-ldap returned any result */
	sc2.sc_response = ldap_chain_cb_response;
	op->o_callback = &sc2;

	/* Chaining can be performed by a privileged user on behalf
	 * of normal users, using the ProxyAuthz control, by exploiting
	 * the identity assertion feature of back-ldap; see idassert-*
	 * directives in slapd-ldap(5).
	 *
	 * FIXME: the idassert-authcDN is one, will it be fine regardless
	 * of the URI we obtain from the referral?
	 */

	switch ( op->o_tag ) {
	case LDAP_REQ_BIND: {
		struct berval	rndn = op->o_req_ndn;
		Connection	*conn = op->o_conn;

		/* FIXME: can we really get a referral for binds? */
		op->o_req_ndn = slap_empty_bv;
		op->o_conn = NULL;
		rc = ldap_chain_op( op, rs, lback->bi_op_bind, ref );
		op->o_req_ndn = rndn;
		op->o_conn = conn;
		}
		break;
	case LDAP_REQ_ADD:
		{
		int		cleanup_attrs = 0;

		if ( op->ora_e->e_attrs == NULL ) {
			char		textbuf[ SLAP_TEXT_BUFLEN ];
			size_t		textlen = sizeof( textbuf );

#if 0
			/* FIXME: op->o_bd is still set to the BackendDB 
			 * structure of the database that tried to handle
			 * the operation and actually returned a referral
			 * ... */
			assert( SLAP_DBFLAGS( op->o_bd ) & SLAP_DBFLAG_GLOBAL_OVERLAY );
#endif

			/* global overlay: create entry */
			/* NOTE: this is a hack to use the chain overlay
			 * as global.  I expect to be able to remove this
			 * soon by using slap_mods2entry() earlier in
			 * do_add(), adding the operational attrs later
			 * if required. */
			rs->sr_err = slap_mods2entry( op->ora_modlist,
					&op->ora_e, 0, 1,
					&rs->sr_text, textbuf, textlen );
			if ( rs->sr_err != LDAP_SUCCESS ) {
				send_ldap_result( op, rs );
				rc = 1;
				break;
			}
		}
		rc = ldap_chain_op( op, rs, lback->bi_op_add, ref );
		if ( cleanup_attrs ) {
			attrs_free( op->ora_e->e_attrs );
			op->ora_e->e_attrs = NULL;
		}
		break;
		}
	case LDAP_REQ_DELETE:
		rc = ldap_chain_op( op, rs, lback->bi_op_delete, ref );
		break;
	case LDAP_REQ_MODRDN:
		rc = ldap_chain_op( op, rs, lback->bi_op_modrdn, ref );
	    	break;
	case LDAP_REQ_MODIFY:
		rc = ldap_chain_op( op, rs, lback->bi_op_modify, ref );
		break;
	case LDAP_REQ_COMPARE:
		rc = ldap_chain_op( op, rs, lback->bi_op_compare, ref );
		break;
	case LDAP_REQ_SEARCH:
		if ( rs->sr_type == REP_SEARCHREF ) {
			struct berval	*curr = ref,
					odn = op->o_req_dn,
					ondn = op->o_req_ndn;

			rs->sr_type = REP_SEARCH;

			sc2.sc_response = ldap_chain_cb_search_response;

			li = *lip;
			li.url = NULL;
			op->o_bd->be_private = &li;
			
			/* if we parse the URI then by no means 
			 * we can cache stuff or reuse connections, 
			 * because in back-ldap there's no caching
			 * based on the URI value, which is supposed
			 * to be set once for all (correct?) */
			op->o_do_not_cache = 1;

			/* copy the private info because we need to modify it */
			for ( ; !BER_BVISNULL( &curr[0] ); curr++ ) {
				LDAPURLDesc	*srv;
				char		*save_dn;

				/* parse reference and use
				 * proto://[host][:port]/ only */
				rc = ldap_url_parse_ext( curr[0].bv_val, &srv );
				if ( rc != LDAP_URL_SUCCESS ) {
					/* try next */
					rs->sr_err = LDAP_OTHER;
					continue;
				}

				/* remove DN essentially because later on 
				 * ldap_initialize() will parse the URL 
				 * as a comma-separated URL list */
				save_dn = srv->lud_dn;
				srv->lud_dn = "";
				srv->lud_scope = LDAP_SCOPE_DEFAULT;
				li.url = ldap_url_desc2str( srv );
				if ( li.url != NULL ) {
					ber_str2bv_x( save_dn, 0, 1, &op->o_req_dn,
							op->o_tmpmemctx );
					ber_dupbv_x( &op->o_req_ndn, &op->o_req_dn,
							op->o_tmpmemctx );
				}

				srv->lud_dn = save_dn;
				ldap_free_urldesc( srv );

				if ( li.url == NULL ) {
					/* try next */
					rs->sr_err = LDAP_OTHER;
					continue;
				}


				/* FIXME: should we also copy filter and scope?
				 * according to RFC3296, no */
				rc = lback->bi_op_search( op, rs );

				ldap_memfree( li.url );
				li.url = NULL;

				op->o_tmpfree( op->o_req_dn.bv_val,
						op->o_tmpmemctx );
				op->o_tmpfree( op->o_req_ndn.bv_val,
						op->o_tmpmemctx );

				if ( rc == LDAP_SUCCESS && rs->sr_err == LDAP_SUCCESS ) {
					break;
				}

				rc = rs->sr_err;
			}

			op->o_req_dn = odn;
			op->o_req_ndn = ondn;
			rs->sr_type = REP_SEARCHREF;
			rs->sr_entry = NULL;

			if ( rc != LDAP_SUCCESS ) {
				/* couldn't chase any of the referrals */
				rc = SLAP_CB_CONTINUE;
			}
			
		} else {
			rc = ldap_chain_op( op, rs, lback->bi_op_search, ref );
		}
	    	break;
	case LDAP_REQ_EXTENDED:
		rc = ldap_chain_op( op, rs, lback->bi_extended, ref );
		/* FIXME: ldap_back_extended() by design 
		 * doesn't send result; frontend is expected
		 * to send it... */
		if ( rc != SLAPD_ABANDON ) {
			send_ldap_extended( op, rs );
			rc = LDAP_SUCCESS;
		}
		break;
	default:
		rc = SLAP_CB_CONTINUE;
		break;
	}

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
	if ( rc != LDAP_SUCCESS || sc2.sc_private == LDAP_CH_ERR ) {
		if ( rs->sr_err == LDAP_CANNOT_CHAIN ) {
			goto cannot_chain;
		}

		switch ( ( get_chainingBehavior( op ) & chain_mask ) >> chain_shift ) {
		case LDAP_CHAINING_REQUIRED:
cannot_chain:;
			op->o_callback = NULL;
			send_ldap_error( op, rs, LDAP_CANNOT_CHAIN, "operation cannot be completed without chaining" );
			break;

		default:
			rc = SLAP_CB_CONTINUE;
			rs->sr_err = sr_err;
			rs->sr_type = sr_type;
			break;
		}
		goto dont_chain;
	}
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */

	if ( sc2.sc_private == LDAP_CH_NONE ) {
		op->o_callback = NULL;
		rc = rs->sr_err = slap_map_api2result( rs );
		send_ldap_result( op, rs );
	}

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
dont_chain:;
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */
	op->o_do_not_cache = cache;
	op->o_bd->be_private = private;
	op->o_callback = sc;
	op->o_ndn = ndn;
	rs->sr_ref = ref;

	return rc;
}

static int
ldap_chain_db_config(
	BackendDB	*be,
	const char	*fname,
	int		lineno,
	int		argc,
	char	**argv
)
{
	slap_overinst	*on = (slap_overinst *) be->bd_info;
	void		*private = be->be_private;
	char		*argv0 = NULL;
	int		rc;

	be->be_private = on->on_bi.bi_private;
	if ( strncasecmp( argv[ 0 ], "chain-", STRLENOF( "chain-" ) ) == 0 ) {
		argv0 = argv[ 0 ];
		argv[ 0 ] = &argv[ 0 ][ STRLENOF( "chain-" ) ];
	}
	rc = lback->bi_db_config( be, fname, lineno, argc, argv );
	if ( argv0 ) {
		argv[ 0 ] = argv0;
	}
	
	be->be_private = private;
	return rc;
}

static int
ldap_chain_db_init(
	BackendDB *be
)
{
	slap_overinst	*on = (slap_overinst *)be->bd_info;
	int		rc;
	BackendDB	bd = *be;

	if ( lback == NULL ) {
		lback = backend_info( "ldap" );

		if ( lback == NULL ) {
			return -1;
		}
	}

	bd.be_private = NULL;
	rc = lback->bi_db_init( &bd );
	on->on_bi.bi_private = bd.be_private;

	return rc;
}

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
static int
ldap_chain_db_open(
	BackendDB *be
)
{
	return overlay_register_control( be, LDAP_CONTROL_X_CHAINING_BEHAVIOR );
}
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */

static int
ldap_chain_db_destroy(
	BackendDB *be
)
{
	slap_overinst *on = (slap_overinst *) be->bd_info;
	void *private = be->be_private;
	int rc;

	be->be_private = on->on_bi.bi_private;
	rc = lback->bi_db_destroy( be );
	on->on_bi.bi_private = be->be_private;
	be->be_private = private;
	return rc;
}

static int
ldap_chain_connection_destroy(
	BackendDB *be,
	Connection *conn
)
{
	slap_overinst *on = (slap_overinst *) be->bd_info;
	void *private = be->be_private;
	int rc;

	be->be_private = on->on_bi.bi_private;
	rc = lback->bi_connection_destroy( be, conn );
	on->on_bi.bi_private = be->be_private;
	be->be_private = private;
	return rc;
}

#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
static int
ldap_chain_parse_ctrl(
	Operation	*op,
	SlapReply	*rs,
	LDAPControl	*ctrl )
{
	ber_tag_t	tag;
	BerElement	*ber;
	ber_int_t	mode,
			behavior;

	if ( get_chaining( op ) != SLAP_CONTROL_NONE ) {
		rs->sr_text = "Chaining behavior control specified multiple times";
		return LDAP_PROTOCOL_ERROR;
	}

	if ( op->o_pagedresults != SLAP_CONTROL_NONE ) {
		rs->sr_text = "Chaining behavior control specified with pagedResults control";
		return LDAP_PROTOCOL_ERROR;
	}

	if ( BER_BVISEMPTY( &ctrl->ldctl_value ) ) {
		mode = (SLAP_CH_RESOLVE_DEFAULT|SLAP_CH_CONTINUATION_DEFAULT);

	} else {
		ber_len_t	len;

		/* Parse the control value
		 *      ChainingBehavior ::= SEQUENCE { 
		 *           resolveBehavior         Behavior OPTIONAL, 
		 *           continuationBehavior    Behavior OPTIONAL } 
		 *                             
		 *      Behavior :: = ENUMERATED { 
		 *           chainingPreferred       (0), 
		 *           chainingRequired        (1), 
		 *           referralsPreferred      (2), 
		 *           referralsRequired       (3) } 
		 */

		ber = ber_init( &ctrl->ldctl_value );
		if( ber == NULL ) {
			rs->sr_text = "internal error";
			return LDAP_OTHER;
		}

		tag = ber_scanf( ber, "{e" /* } */, &behavior );
		/* FIXME: since the whole SEQUENCE is optional,
		 * should we accept no enumerations at all? */
		if ( tag != LBER_ENUMERATED ) {
			rs->sr_text = "Chaining behavior control: resolveBehavior decoding error";
			return LDAP_PROTOCOL_ERROR;
		}

		switch ( behavior ) {
		case LDAP_CHAINING_PREFERRED:
			mode = SLAP_CH_RESOLVE_CHAINING_PREFERRED;
			break;

		case LDAP_CHAINING_REQUIRED:
			mode = SLAP_CH_RESOLVE_CHAINING_REQUIRED;
			break;

		case LDAP_REFERRALS_PREFERRED:
			mode = SLAP_CH_RESOLVE_REFERRALS_PREFERRED;
			break;

		case LDAP_REFERRALS_REQUIRED:
			mode = SLAP_CH_RESOLVE_REFERRALS_REQUIRED;
			break;

		default:
			rs->sr_text = "Chaining behavior control: unknown resolveBehavior";
			return LDAP_PROTOCOL_ERROR;
		}

		tag = ber_peek_tag( ber, &len );
		if ( tag == LBER_ENUMERATED ) {
			tag = ber_scanf( ber, "e", &behavior );
			if ( tag == LBER_ERROR ) {
				rs->sr_text = "Chaining behavior control: continuationBehavior decoding error";
				return LDAP_PROTOCOL_ERROR;
			}
		}

		if ( tag == LBER_DEFAULT ) {
			mode |= SLAP_CH_CONTINUATION_DEFAULT;

		} else {
			switch ( behavior ) {
			case LDAP_CHAINING_PREFERRED:
				mode |= SLAP_CH_CONTINUATION_CHAINING_PREFERRED;
				break;

			case LDAP_CHAINING_REQUIRED:
				mode |= SLAP_CH_CONTINUATION_CHAINING_REQUIRED;
				break;

			case LDAP_REFERRALS_PREFERRED:
				mode |= SLAP_CH_CONTINUATION_REFERRALS_PREFERRED;
				break;

			case LDAP_REFERRALS_REQUIRED:
				mode |= SLAP_CH_CONTINUATION_REFERRALS_REQUIRED;
				break;

			default:
				rs->sr_text = "Chaining behavior control: unknown continuationBehavior";
				return LDAP_PROTOCOL_ERROR;
			}
		}

		if ( ( ber_scanf( ber, /* { */ "}") ) == LBER_ERROR ) {
			rs->sr_text = "Chaining behavior control: decoding error";
			return LDAP_PROTOCOL_ERROR;
		}

		(void) ber_free( ber, 1 );
	}

	op->o_chaining = mode | ( ctrl->ldctl_iscritical
			? SLAP_CONTROL_CRITICAL
			: SLAP_CONTROL_NONCRITICAL );

	return LDAP_SUCCESS;
}
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */

static slap_overinst ldapchain;

int
chain_init( void )
{
#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
	int	rc;

	rc = register_supported_control( LDAP_CONTROL_X_CHAINING_BEHAVIOR,
			/* SLAP_CTRL_GLOBAL| */ SLAP_CTRL_ACCESS|SLAP_CTRL_HIDE, NULL,
			ldap_chain_parse_ctrl, &sc_chainingBehavior );
	if ( rc != LDAP_SUCCESS ) {
		fprintf( stderr, "Failed to register chaining behavior control: %d\n", rc );
		return rc;
	}
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */

	ldapchain.on_bi.bi_type = "chain";
	ldapchain.on_bi.bi_db_init = ldap_chain_db_init;
#ifdef LDAP_CONTROL_X_CHAINING_BEHAVIOR
	ldapchain.on_bi.bi_db_open = ldap_chain_db_open;
#endif /* LDAP_CONTROL_X_CHAINING_BEHAVIOR */
	ldapchain.on_bi.bi_db_config = ldap_chain_db_config;
	ldapchain.on_bi.bi_db_destroy = ldap_chain_db_destroy;

	/* ... otherwise the underlying backend's function would be called,
	 * likely passing an invalid entry; on the contrary, the requested
	 * operational attributes should have been returned while chasing
	 * the referrals.  This all in all is a bit messy, because part
	 * of the operational attributes are generated by the backend;
	 * part by the frontend; back-ldap should receive all the available
	 * ones from the remote server, but then, on its own, it strips those
	 * it assumes will be (re)generated by the frontend (e.g.
	 * subschemaSubentry.) */
	ldapchain.on_bi.bi_operational = ldap_chain_operational;
	
	ldapchain.on_bi.bi_connection_destroy = ldap_chain_connection_destroy;

	ldapchain.on_response = ldap_chain_response;

	return overlay_register( &ldapchain );
}

