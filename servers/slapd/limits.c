/* limits.c - routines to handle regex-based size and time limits */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2004 The OpenLDAP Foundation.
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

#include "portable.h"

#include <stdio.h>

#include <ac/regex.h>
#include <ac/string.h>

#include "slap.h"

int
limits_get( 
	Operation		*op,
	struct berval		*ndn, 
	struct slap_limits_set 	**limit
)
{
	struct slap_limits **lm;

	assert( op );
	assert( limit );

	/*
	 * default values
	 */
	*limit = &op->o_bd->be_def_limit;

	if ( op->o_bd->be_limits == NULL ) {
		return( 0 );
	}

	for ( lm = op->o_bd->be_limits; lm[0] != NULL; lm++ ) {
		unsigned	style = lm[0]->lm_flags & SLAP_LIMITS_MASK;
		unsigned	type = lm[0]->lm_flags & SLAP_LIMITS_TYPE_MASK;

		switch ( style ) {
		case SLAP_LIMITS_EXACT:
			if ( ndn->bv_len == 0 ) {
				break;
			}

			if ( type == SLAP_LIMITS_TYPE_GROUP ) {
				int	rc;

				rc = backend_group( op, NULL,
						&lm[0]->lm_pat, ndn,
						lm[0]->lm_group_oc,
						lm[0]->lm_group_ad );
				if ( rc == 0 ) {
					*limit = &lm[0]->lm_limits;
					return( 0 );
				}
			}
			
			if ( dn_match( &lm[0]->lm_pat, ndn ) ) {
				*limit = &lm[0]->lm_limits;
				return( 0 );
			}
			break;

		case SLAP_LIMITS_ONE:
		case SLAP_LIMITS_SUBTREE:
		case SLAP_LIMITS_CHILDREN: {
			size_t d;
			
			if ( ndn->bv_len == 0 ) {
				break;
			}

			/* ndn shorter than dn_pat */
			if ( ndn->bv_len < lm[0]->lm_pat.bv_len ) {
				break;
			}
			d = ndn->bv_len - lm[0]->lm_pat.bv_len;

			/* allow exact match for SUBTREE only */
			if ( d == 0 ) {
				if ( style != SLAP_LIMITS_SUBTREE ) {
					break;
				}
			} else {
				/* check for unescaped rdn separator */
				if ( !DN_SEPARATOR( ndn->bv_val[d-1] ) ) {
					break;
				}
			}

			/* in case of (sub)match ... */
			if ( lm[0]->lm_pat.bv_len == ( ndn->bv_len - d )
					&& strcmp( lm[0]->lm_pat.bv_val,
						&ndn->bv_val[d] ) == 0 )
			{
				/* check for exactly one rdn in case of ONE */
				if ( style == SLAP_LIMITS_ONE ) {
					/*
					 * if ndn is more that one rdn
					 * below dn_pat, continue
					 */
					if ( (size_t) dn_rdnlen( NULL, ndn )
							!= d - 1 )
					{
						break;
					}
				}

				*limit = &lm[0]->lm_limits;
				return( 0 );
			}

			break;
		}

		case SLAP_LIMITS_REGEX:
			if ( ndn->bv_len == 0 ) {
				break;
			}
			if ( regexec( &lm[0]->lm_regex, ndn->bv_val,
						0, NULL, 0 ) == 0 )
			{
				*limit = &lm[0]->lm_limits;
				return( 0 );
			}
			break;

		case SLAP_LIMITS_ANONYMOUS:
			if ( ndn->bv_len == 0 ) {
				*limit = &lm[0]->lm_limits;
				return( 0 );
			}
			break;

		case SLAP_LIMITS_USERS:
			if ( ndn->bv_len != 0 ) {
				*limit = &lm[0]->lm_limits;
				return( 0 );
			}
			break;

		case SLAP_LIMITS_ANY:
			*limit = &lm[0]->lm_limits;
			return( 0 );

		default:
			assert( 0 );	/* unreachable */
			return( -1 );
		}
	}

	return( 0 );
}

static int
limits_add(
	Backend 	        *be,
	unsigned		flags,
	const char		*pattern,
	ObjectClass		*group_oc,
	AttributeDescription	*group_ad,
	struct slap_limits_set	*limit
)
{
	int 			i;
	struct slap_limits	*lm;
	unsigned		type, style;
	
	assert( be );
	assert( limit );

	type = flags & SLAP_LIMITS_TYPE_MASK;
	style = flags & SLAP_LIMITS_MASK;

	switch ( style ) {
	case SLAP_LIMITS_ANONYMOUS:
	case SLAP_LIMITS_USERS:
	case SLAP_LIMITS_ANY:
		for ( i = 0; be->be_limits && be->be_limits[ i ]; i++ ) {
			if ( be->be_limits[ i ]->lm_flags == style ) {
				return( -1 );
			}
		}
		break;
	}


	lm = ( struct slap_limits * )ch_calloc( sizeof( struct slap_limits ), 1 );

	switch ( style ) {
	case SLAP_LIMITS_UNDEFINED:
		style = SLAP_LIMITS_EXACT;
		/* continue to next cases */
	case SLAP_LIMITS_EXACT:
	case SLAP_LIMITS_ONE:
	case SLAP_LIMITS_SUBTREE:
	case SLAP_LIMITS_CHILDREN:
		lm->lm_flags = style | type;
		{
			int rc;
			struct berval bv;
			bv.bv_val = (char *) pattern;
			bv.bv_len = strlen( pattern );

			rc = dnNormalize( 0, NULL, NULL, &bv, &lm->lm_pat, NULL );
			if ( rc != LDAP_SUCCESS ) {
				ch_free( lm );
				return( -1 );
			}
		}
		break;
		
	case SLAP_LIMITS_REGEX:
		lm->lm_flags = style | type;
		ber_str2bv( pattern, 0, 1, &lm->lm_pat );
		if ( regcomp( &lm->lm_regex, lm->lm_pat.bv_val, 
					REG_EXTENDED | REG_ICASE ) ) {
			free( lm->lm_pat.bv_val );
			ch_free( lm );
			return( -1 );
		}
		break;

	case SLAP_LIMITS_ANONYMOUS:
	case SLAP_LIMITS_USERS:
	case SLAP_LIMITS_ANY:
		lm->lm_flags = style | type;
		lm->lm_pat.bv_val = NULL;
		lm->lm_pat.bv_len = 0;
		break;
	}

	switch ( type ) {
	case SLAP_LIMITS_TYPE_GROUP:
		assert( group_oc );
		assert( group_ad );
		lm->lm_group_oc = group_oc;
		lm->lm_group_ad = group_ad;
		break;
	}

	lm->lm_limits = *limit;

	i = 0;
	if ( be->be_limits != NULL ) {
		for ( ; be->be_limits[i]; i++ );
	}

	be->be_limits = ( struct slap_limits ** )ch_realloc( be->be_limits,
			sizeof( struct slap_limits * ) * ( i + 2 ) );
	be->be_limits[i] = lm;
	be->be_limits[i+1] = NULL;
	
	return( 0 );
}

int
limits_parse(
	Backend     *be,
	const char  *fname,
	int         lineno,
	int         argc,
	char        **argv
)
{
	int			flags = SLAP_LIMITS_UNDEFINED;
	char			*pattern;
	struct slap_limits_set 	limit;
	int 			i, rc = 0;
	ObjectClass		*group_oc = NULL;
	AttributeDescription	*group_ad = NULL;

	assert( be );

	if ( argc < 3 ) {
#ifdef NEW_LOGGING
		LDAP_LOG( CONFIG, CRIT, 
			"%s : line %d: missing arg(s) in "
			"\"limits <pattern> <limits>\" line.\n", fname, lineno, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"%s : line %d: missing arg(s) in "
			"\"limits <pattern> <limits>\" line.\n%s",
			fname, lineno, "" );
#endif
		return( -1 );
	}

	limit = be->be_def_limit;

	/*
	 * syntax:
	 *
	 * "limits" <pattern> <limit> [ ... ]
	 * 
	 * 
	 * <pattern>:
	 * 
	 * "anonymous"
	 * "users"
	 * [ "dn" [ "." { "exact" | "base" | "onelevel" | "subtree" | children"
	 *	| "regex" | "anonymous" } ] "=" ] <dn pattern>
	 *
	 * Note:
	 *	"exact" and "base" are the same (exact match);
	 *	"onelevel" means exactly one rdn below, NOT including pattern
	 *	"subtree" means any rdn below, including pattern
	 *	"children" means any rdn below, NOT including pattern
	 *	
	 *	"anonymous" may be deprecated in favour 
	 *	of the pattern = "anonymous" form
	 *
	 * "group[/objectClass[/attributeType]]" "=" "<dn pattern>"
	 *
	 * <limit>:
	 *
	 * "time" [ "." { "soft" | "hard" } ] "=" <integer>
	 *
	 * "size" [ "." { "soft" | "hard" | "unchecked" } ] "=" <integer>
	 */
	
	pattern = argv[1];
	if ( strcmp( pattern, "*" ) == 0) {
		flags = SLAP_LIMITS_ANY;

	} else if ( strcasecmp( pattern, "anonymous" ) == 0 ) {
		flags = SLAP_LIMITS_ANONYMOUS;

	} else if ( strcasecmp( pattern, "users" ) == 0 ) {
		flags = SLAP_LIMITS_USERS;
		
	} else if ( strncasecmp( pattern, "dn", sizeof( "dn" ) - 1 ) == 0 ) {
		pattern += sizeof( "dn" ) - 1;
		if ( pattern[0] == '.' ) {
			pattern++;
			if ( strncasecmp( pattern, "exact", sizeof( "exact" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_EXACT;
				pattern += sizeof( "exact" ) - 1;

			} else if ( strncasecmp( pattern, "base", sizeof( "base" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_BASE;
				pattern += sizeof( "base" ) - 1;

			} else if ( strncasecmp( pattern, "one", sizeof( "one" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_ONE;
				pattern += sizeof( "one" ) - 1;
				if ( strncasecmp( pattern, "level", sizeof( "level" ) - 1 ) == 0 ) {
					pattern += sizeof( "level" ) - 1;

				} else {
#ifdef NEW_LOGGING
					LDAP_LOG( CONFIG, WARNING , 
						"%s : line %d: deprecated \"one\" style "
						"\"limits <pattern> <limits>\" line; "
						"use \"onelevel\" instead.\n", fname, lineno, 0 );
#else
					Debug( LDAP_DEBUG_ANY,
						"%s : line %d: deprecated \"one\" style "
						"\"limits <pattern> <limits>\" line; "
						"use \"onelevel\" instead.\n", fname, lineno, 0 );
#endif
				}

			} else if ( strncasecmp( pattern, "sub", sizeof( "sub" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_SUBTREE;
				pattern += sizeof( "sub" ) - 1;
				if ( strncasecmp( pattern, "tree", sizeof( "tree" ) - 1 ) == 0 ) {
					pattern += sizeof( "tree" ) - 1;

				} else {
#ifdef NEW_LOGGING
					LDAP_LOG( CONFIG, WARNING , 
						"%s : line %d: deprecated \"sub\" style "
						"\"limits <pattern> <limits>\" line; "
						"use \"subtree\" instead.\n", fname, lineno, 0 );
#else
					Debug( LDAP_DEBUG_ANY,
						"%s : line %d: deprecated \"sub\" style "
						"\"limits <pattern> <limits>\" line; "
						"use \"subtree\" instead.\n", fname, lineno, 0 );
#endif
				}

			} else if ( strncasecmp( pattern, "children", sizeof( "children" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_CHILDREN;
				pattern += sizeof( "children" ) - 1;

			} else if ( strncasecmp( pattern, "regex", sizeof( "regex" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_REGEX;
				pattern += sizeof( "regex" ) - 1;

			/* 
			 * this could be deprecated in favour
			 * of the pattern = "anonymous" form
			 */
			} else if ( strncasecmp( pattern, "anonymous", sizeof( "anonymous" ) - 1 ) == 0 ) {
				flags = SLAP_LIMITS_ANONYMOUS;
				pattern = NULL;
			}
		}

		/* pre-check the data */
		switch ( flags ) {
		case SLAP_LIMITS_ANONYMOUS:
		case SLAP_LIMITS_USERS:

			/* no need for pattern */
			pattern = NULL;
			break;

		default:
			if ( pattern[0] != '=' ) {
#ifdef NEW_LOGGING
				LDAP_LOG( CONFIG, CRIT, 
					"%s : line %d: missing '=' in "
					"\"dn[.{exact|base|onelevel|subtree"
					"|children|regex|anonymous}]" "=<pattern>\" in "
					"\"limits <pattern> <limits>\" line.\n", fname, lineno, 0 );
#else
				Debug( LDAP_DEBUG_ANY,
					"%s : line %d: missing '=' in "
					"\"dn[.{exact|base|onelevel|subtree"
					"|children|regex|anonymous}]"
					"=<pattern>\" in "
					"\"limits <pattern> <limits>\" "
					"line.\n%s",
					fname, lineno, "" );
#endif
				return( -1 );
			}

			/* skip '=' (required) */
			pattern++;

			/* trim obvious cases */
			if ( strcmp( pattern, "*" ) == 0 ) {
				flags = SLAP_LIMITS_ANY;
				pattern = NULL;

			} else if ( flags == SLAP_LIMITS_REGEX
					&& strcmp( pattern, ".*" ) == 0 ) {
				flags = SLAP_LIMITS_ANY;
				pattern = NULL;
			}
		}

	} else if (strncasecmp( pattern, "group", sizeof( "group" ) - 1 ) == 0 ) {
		pattern += sizeof( "group" ) - 1;

		if ( pattern[0] == '/' ) {
			struct berval	oc, ad;

			oc.bv_val = pattern + 1;

			ad.bv_val = strchr(pattern, '/');
			if ( ad.bv_val != NULL ) {
				const char	*text = NULL;
				int		rc;

				oc.bv_len = ad.bv_val - oc.bv_val;

				ad.bv_val++;
				ad.bv_len = strlen( ad.bv_val );
				rc = slap_bv2ad( &ad, &group_ad, &text );
				if ( rc != LDAP_SUCCESS ) {
					goto no_ad;
				}

				pattern = ad.bv_val + ad.bv_len;

			} else {
				oc.bv_len = strlen( oc.bv_val );

				pattern = oc.bv_val + oc.bv_len;
			}

			group_oc = oc_bvfind( &oc );
			if ( group_oc == NULL ) {
				goto no_oc;
			}
		}

		if ( group_oc == NULL ) {
			group_oc = oc_find( SLAPD_GROUP_CLASS );
			if ( group_oc == NULL ) {
no_oc:;
				return( -1 );
			}
		}

		if ( group_ad == NULL ) {
			const char	*text = NULL;
			int		rc;
			
			rc = slap_str2ad( SLAPD_GROUP_ATTR, &group_ad, &text );

			if ( rc != LDAP_SUCCESS ) {
no_ad:;
				return( -1 );
			}
		}

		flags = SLAP_LIMITS_TYPE_GROUP | SLAP_LIMITS_EXACT;

		if ( pattern[0] != '=' ) {
#ifdef NEW_LOGGING
			LDAP_LOG( CONFIG, CRIT, 
				"%s : line %d: missing '=' in "
				"\"group[/objectClass[/attributeType]]"
				"=<pattern>\" in "
				"\"limits <pattern> <limits>\" line.\n",
				fname, lineno, 0 );
#else
			Debug( LDAP_DEBUG_ANY,
				"%s : line %d: missing '=' in "
				"\"group[/objectClass[/attributeType]]"
				"=<pattern>\" in "
				"\"limits <pattern> <limits>\" line.\n",
				fname, lineno, 0 );
#endif
			return( -1 );
		}

		/* skip '=' (required) */
		pattern++;
	}

	/* get the limits */
	for ( i = 2; i < argc; i++ ) {
		if ( limits_parse_one( argv[i], &limit ) ) {

#ifdef NEW_LOGGING
			LDAP_LOG( CONFIG, CRIT, 
				"%s : line %d: unknown limit values \"%s\" in "
				"\"limits <pattern> <limits>\" line.\n",
				fname, lineno, argv[i] );
#else
			Debug( LDAP_DEBUG_ANY,
				"%s : line %d: unknown limit values \"%s\" in "
				"\"limits <pattern> <limits>\" line.\n",
			fname, lineno, argv[i] );
#endif

			return( 1 );
		}
	}

	/*
	 * sanity checks ...
	 */
	if ( limit.lms_t_hard > 0 && 
			( limit.lms_t_hard < limit.lms_t_soft 
			  || limit.lms_t_soft == -1 ) ) {
		limit.lms_t_hard = limit.lms_t_soft;
	}
	
	if ( limit.lms_s_hard > 0 && 
			( limit.lms_s_hard < limit.lms_s_soft 
			  || limit.lms_s_soft == -1 ) ) {
		limit.lms_s_hard = limit.lms_s_soft;
	}
	
	rc = limits_add( be, flags, pattern, group_oc, group_ad, &limit );
	if ( rc ) {

#ifdef NEW_LOGGING
		LDAP_LOG( CONFIG, CRIT, 
			"%s : line %d: unable to add limit in "
			"\"limits <pattern> <limits>\" line.\n",
			fname, lineno, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"%s : line %d: unable to add limit in "
			"\"limits <pattern> <limits>\" line.\n",
		fname, lineno, 0 );
#endif
	}

	return( rc );
}

int
limits_parse_one(
	const char 		*arg,
	struct slap_limits_set 	*limit
)
{
	assert( arg );
	assert( limit );

	if ( strncasecmp( arg, "time", sizeof( "time" ) - 1 ) == 0 ) {
		arg += 4;

		if ( arg[0] == '.' ) {
			arg++;
			if ( strncasecmp( arg, "soft", sizeof( "soft" ) - 1 ) == 0 ) {
				arg += 4;
				if ( arg[0] != '=' ) {
					return( 1 );
				}
				arg++;
				if ( strcasecmp( arg, "none" ) == 0 ) {
					limit->lms_t_soft = -1;
				} else {
					char	*next = NULL;

					limit->lms_t_soft = 
						strtol( arg, &next, 10 );
					if ( next == arg || limit->lms_t_soft < -1 ) {
						return( 1 );
					}
				}
				
			} else if ( strncasecmp( arg, "hard", sizeof( "hard" ) - 1 ) == 0 ) {
				arg += 4;
				if ( arg[0] != '=' ) {
					return( 1 );
				}
				arg++;
				if ( strcasecmp( arg, "soft" ) == 0 ) {
					limit->lms_t_hard = 0;
				} else if ( strcasecmp( arg, "none" ) == 0 ) {
					limit->lms_t_hard = -1;
				} else {
					char	*next = NULL;

					limit->lms_t_hard = 
						strtol( arg, &next, 10 );
					if ( next == arg || limit->lms_t_hard < -1 ) {
						return( 1 );
					}
				}
				
			} else {
				return( 1 );
			}
			
		} else if ( arg[0] == '=' ) {
			arg++;
			if ( strcasecmp( arg, "none" ) == 0 ) {
				limit->lms_t_soft = -1;
			} else {
				char	*next = NULL;

				limit->lms_t_soft = strtol( arg, &next, 10 );
				if ( next == arg || limit->lms_t_soft < -1 ) {
					return( 1 );
				}
			}
			limit->lms_t_hard = 0;
			
		} else {
			return( 1 );
		}

	} else if ( strncasecmp( arg, "size", sizeof( "size" ) - 1 ) == 0 ) {
		arg += 4;
		
		if ( arg[0] == '.' ) {
			arg++;
			if ( strncasecmp( arg, "soft", sizeof( "soft" ) - 1 ) == 0 ) {
				arg += 4;
				if ( arg[0] != '=' ) {
					return( 1 );
				}
				arg++;
				if ( strcasecmp( arg, "none" ) == 0 ) {
					limit->lms_s_soft = -1;
				} else {
					char	*next = NULL;

					limit->lms_s_soft = 
						strtol( arg, &next, 10 );
					if ( next == arg || limit->lms_s_soft < -1 ) {
						return( 1 );
					}
				}
				
			} else if ( strncasecmp( arg, "hard", sizeof( "hard" ) - 1 ) == 0 ) {
				arg += 4;
				if ( arg[0] != '=' ) {
					return( 1 );
				}
				arg++;
				if ( strcasecmp( arg, "soft" ) == 0 ) {
					limit->lms_s_hard = 0;
				} else if ( strcasecmp( arg, "none" ) == 0 ) {
					limit->lms_s_hard = -1;
				} else {
					char	*next = NULL;

					limit->lms_s_hard = 
						strtol( arg, &next, 10 );
					if ( next == arg || limit->lms_s_hard < -1 ) {
						return( 1 );
					}
				}
				
			} else if ( strncasecmp( arg, "unchecked", sizeof( "unchecked" ) - 1 ) == 0 ) {
				arg += 9;
				if ( arg[0] != '=' ) {
					return( 1 );
				}
				arg++;
				if ( strcasecmp( arg, "none" ) == 0 ) {
					limit->lms_s_unchecked = -1;
				} else {
					char	*next = NULL;

					limit->lms_s_unchecked = 
						strtol( arg, &next, 10 );
					if ( next == arg || limit->lms_s_unchecked < -1 ) {
						return( 1 );
					}
				}

			} else if ( strncasecmp( arg, "pr", sizeof( "pr" ) - 1 ) == 0 ) {
				arg += sizeof( "pr" ) - 1;
				if ( arg[0] != '=' ) {
					return( 1 );
				}
				arg++;
				if ( strcasecmp( arg, "noEstimate" ) == 0 ) {
					limit->lms_s_pr_hide = 1;
				} else {
					char	*next = NULL;

					limit->lms_s_pr = 
						strtol( arg, &next, 10 );
					if ( next == arg || limit->lms_s_pr < -1 ) {
						return( 1 );
					}
				}
				
			} else {
				return( 1 );
			}
			
		} else if ( arg[0] == '=' ) {
			arg++;
			if ( strcasecmp( arg, "none" ) == 0 ) {
				limit->lms_s_soft = -1;
			} else {
				char	*next = NULL;

				limit->lms_s_soft = strtol( arg, &next, 10 );
				if ( next == arg || limit->lms_s_soft < -1 ) {
					return( 1 );
				}
			}
			limit->lms_s_hard = 0;
			
		} else {
			return( 1 );
		}
	}

	return 0;
}


int
limits_check( Operation *op, SlapReply *rs )
{
	assert( op );
	assert( rs );
	/* FIXME: should this be always true? */
	assert( op->o_tag == LDAP_REQ_SEARCH);
	
	/* allow root to set no limit */
	if ( be_isroot( op->o_bd, &op->o_ndn ) ) {
		op->ors_limit = NULL;

		if ( op->ors_tlimit == 0 ) {
			op->ors_tlimit = -1;
		}

		if ( op->ors_slimit == 0 ) {
			op->ors_slimit = -1;
		}

	/* if not root, get appropriate limits */
	} else {
		( void ) limits_get( op, &op->o_ndn, &op->ors_limit );

		assert( op->ors_limit != NULL );

		/* if no limit is required, use soft limit */
		if ( op->ors_tlimit <= 0 ) {
			op->ors_tlimit = op->ors_limit->lms_t_soft;

		/* if requested limit higher than hard limit, abort */
		} else if ( op->ors_tlimit > op->ors_limit->lms_t_hard ) {
			/* no hard limit means use soft instead */
			if ( op->ors_limit->lms_t_hard == 0
					&& op->ors_limit->lms_t_soft > -1
					&& op->ors_tlimit > op->ors_limit->lms_t_soft ) {
				op->ors_tlimit = op->ors_limit->lms_t_soft;

			/* positive hard limit means abort */
			} else if ( op->ors_limit->lms_t_hard > 0 ) {
				rs->sr_err = LDAP_ADMINLIMIT_EXCEEDED;
				send_ldap_result( op, rs );
				rs->sr_err = LDAP_SUCCESS;
				return -1;
			}
	
			/* negative hard limit means no limit */
		}
	
		/* if no limit is required, use soft limit */
		if ( op->ors_slimit <= 0 ) {
			if ( get_pagedresults( op ) && op->ors_limit->lms_s_pr != 0 ) {
				op->ors_slimit = op->ors_limit->lms_s_pr;
			} else {
				op->ors_slimit = op->ors_limit->lms_s_soft;
			}

		/* if requested limit higher than hard limit, abort */
		} else if ( op->ors_slimit > op->ors_limit->lms_s_hard ) {
			/* no hard limit means use soft instead */
			if ( op->ors_limit->lms_s_hard == 0
					&& op->ors_limit->lms_s_soft > -1
					&& op->ors_slimit > op->ors_limit->lms_s_soft ) {
				op->ors_slimit = op->ors_limit->lms_s_soft;

			/* positive hard limit means abort */
			} else if ( op->ors_limit->lms_s_hard > 0 ) {
				rs->sr_err = LDAP_ADMINLIMIT_EXCEEDED;
				send_ldap_result( op, rs );
				rs->sr_err = LDAP_SUCCESS;	
				return -1;
			}
		
			/* negative hard limit means no limit */
		}
	}

	return 0;
}

