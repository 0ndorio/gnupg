/* audit.h - Definitions for the audit subsystem
 *	Copyright (C) 2007 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GNUPG_COMMON_AUDIT_H
#define GNUPG_COMMON_AUDIT_H

#include <ksba.h>

#include "estream.h"

struct audit_ctx_s;
typedef struct audit_ctx_s *audit_ctx_t;

/* Constants for the audit type.  */
typedef enum
  {
    AUDIT_TYPE_NONE  = 0,  /* No type set.  */
    AUDIT_TYPE_VERIFY      /* Signature verification.  */
  }
audit_type_t;

/* The events we support.  */
typedef enum
  {
    AUDIT_NULL_EVENT = 0,
    /* No such event.  Its value shall be 0 and no other values shall
       be assigned to the other enum symbols.  This is required so
       that the exaudit.awk script comes up with correct values
       without running cc.  */

    AUDIT_SETUP_READY,
    /* All preparations done so that the actual processing can start
       now.  This indicates that all parameters are okay and we can
       start to process the actual data.  */

    AUDIT_GOT_DATA,
    /* Data to be processed has been seen.  */

    AUDIT_DETACHED_SIGNATURE,
    /* The signature is a detached one. */

    AUDIT_CERT_ONLY_SIG,
    /* A certifciate only signature has been detected.  */

    AUDIT_DATA_HASH_ALGO,  /* int */
    /* The hash algo given as argument is used for this signature.
       This event will be repeated for all hash algorithms used with
       the data.  */

    AUDIT_BAD_DATA_HASH_ALGO,  /* string */
    /* The hash algo as specified by the signature can't be used.
       STRING is the description of this algorithm which usually is an
       OID string.  STRING may be NULL. */

    AUDIT_DATA_HASHING,    /* ok_err */
    /* Logs the result of the data hashing. */

    AUDIT_READ_ERROR,     /* ok_err */
    /* A generic read error occurred.  */

    AUDIT_WRITE_ERROR,     /* ok_err */
    /* A generic write error occurred.  */

    AUDIT_USAGE_ERROR,
    /* The program was used in an inappropriate way; For example by
       passing a data object while the signature does not expect one
       or vice versa.  */
    
    AUDIT_SAVE_CERT,       /* cert, ok_err */
    /* Save the certificate received in a message. */

    AUDIT_NEW_SIG,         /* int */
    /* Start the verification of a new signature for the last data
       object.  The argument is the signature number as used
       internally by the program.  */
    
    AUDIT_SIG_NAME,        /* string */
    /* The name of a signer.  This is the name or other identification
       data as known from the signature and not the name from the
       certificate used for verification.  An example for STRING when
       using CMS is: "#1234/CN=Prostetnic Vogon Jeltz".  */

    AUDIT_SIG_STATUS,     /* string */
    /* The signature status of the current signer.  This is the last
       audit information for one signature.  STRING gives the status:

         "error"       - there was a problem checking this or any signature.
         "unsupported" - the signature type is not supported. 
         "no-cert"     - The certificate of the signer was not found (the
                         S/N+issuer of the signer is already in the log).
         "bad"         - bad signature
         "good"        - good signature
    */

    AUDIT_VALIDATE_CHAIN,
    /* Start the validation of a certificate chain. */

    AUDIT_CHAIN_BEGIN,
    AUDIT_CHAIN_CERT,    /* cert */
    AUDIT_CHAIN_ROOTCERT,/* cert */
    AUDIT_CHAIN_END,
    /* These 4 events are used to log the certificates making up a
       certificate chain.  ROOTCERT is used for the trustanchor and
       CERT for all other certificates.  */ 

    AUDIT_CHAIN_STATUS,  /* err */
    /* Tells the final status of the chain validation.  */


    AUDIT_LAST_EVENT  /* Marker for parsing this list.  */
  }
audit_event_t;


audit_ctx_t audit_new (void);
void audit_release (audit_ctx_t ctx);
void audit_set_type (audit_ctx_t ctx, audit_type_t type);
void audit_log (audit_ctx_t ctx, audit_event_t event);
void audit_log_ok (audit_ctx_t ctx, audit_event_t event, gpg_error_t err);
void audit_log_i (audit_ctx_t ctx, audit_event_t event, int value);
void audit_log_s (audit_ctx_t ctx, audit_event_t event, const char *value);
void audit_log_cert (audit_ctx_t ctx, audit_event_t event, 
                     ksba_cert_t cert, gpg_error_t err);

void audit_print_result (audit_ctx_t ctx, estream_t stream, int use_html);



#endif /*GNUPG_COMMON_AUDIT_H*/
