/* Ubuntu's static libssh.a (see native.yml's linux-x86_64 job) was built
 * with GSSAPI auth support compiled in, but Ubuntu ships no static
 * Kerberos library (libkrb5-dev has headers only, no .a) - so the static
 * link fails on undefined gss_* references unless something provides
 * them. This project never authenticates via GSSAPI/Kerberos (only
 * password/publickey - see ssh_auth_password/ssh_auth_key in
 * larzscript.c), so every stub here just reports the real, correct
 * RFC 2744 answer for "this facility isn't available" rather than being
 * an arbitrary placeholder - functionally identical to building libssh
 * with -DWITH_GSSAPI=OFF, achieved without needing to rebuild libssh
 * itself from source.
 *
 * Types are minimal, ABI-compatible approximations of the real GSSAPI
 * ones (opaque pointers/uint32), not a copy of <gssapi/gssapi.h> - C
 * linking resolves by symbol name only, and nothing here is ever
 * actually invoked with real credentials for a signature mismatch to
 * matter in practice. */

typedef unsigned int gss_OM_uint32;
typedef struct { gss_OM_uint32 length; void *value; } gss_buffer_desc, *gss_buffer_t;
typedef struct gss_OID_desc_struct { gss_OM_uint32 length; void *elements; } gss_OID_desc, *gss_OID;
typedef struct gss_OID_set_desc_struct { unsigned long count; gss_OID elements; } gss_OID_set_desc, *gss_OID_set;
typedef void *gss_name_t;
typedef void *gss_cred_id_t;
typedef void *gss_ctx_id_t;
typedef void *gss_channel_bindings_t;
typedef gss_OM_uint32 gss_qop_t;
typedef int gss_cred_usage_t;

/* GSS_S_CALL_INACCESSIBLE_READ (bit set in the routine-error field) -
 * any nonzero value with the high bits set reads as an error to
 * GSSAPI's own GSS_ERROR() convention, which is all that matters here. */
#define LZ_GSS_UNAVAILABLE ((gss_OM_uint32)(9u << 24))

static unsigned char oid_hostbased_service[] = {0x2a,0x86,0x48,0x86,0xf7,0x12,0x01,0x02,0x01,0x04};
static gss_OID_desc oid_hostbased_service_desc = {10, oid_hostbased_service};
gss_OID GSS_C_NT_HOSTBASED_SERVICE = &oid_hostbased_service_desc;

static unsigned char oid_user_name[] = {0x2a,0x86,0x48,0x86,0xf7,0x12,0x01,0x02,0x01,0x02};
static gss_OID_desc oid_user_name_desc = {10, oid_user_name};
gss_OID GSS_C_NT_USER_NAME = &oid_user_name_desc;

gss_OM_uint32 gss_create_empty_oid_set(gss_OM_uint32 *minor, gss_OID_set *set){ if(minor)*minor=0; if(set)*set=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_indicate_mechs(gss_OM_uint32 *minor, gss_OID_set *set){ if(minor)*minor=0; if(set)*set=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_test_oid_set_member(gss_OM_uint32 *minor, gss_OID member, gss_OID_set set, int *present){ (void)member;(void)set; if(minor)*minor=0; if(present)*present=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_add_oid_set_member(gss_OM_uint32 *minor, gss_OID member, gss_OID_set *set){ (void)member;(void)set; if(minor)*minor=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_release_oid_set(gss_OM_uint32 *minor, gss_OID_set *set){ (void)set; if(minor)*minor=0; return 0; }
gss_OM_uint32 gss_import_name(gss_OM_uint32 *minor, gss_buffer_t input, gss_OID type, gss_name_t *output){ (void)input;(void)type; if(minor)*minor=0; if(output)*output=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_acquire_cred(gss_OM_uint32 *minor, gss_name_t name, gss_OM_uint32 lifetime, gss_OID_set mechs, gss_cred_usage_t usage, gss_cred_id_t *cred, gss_OID_set *actual, gss_OM_uint32 *actual_lifetime){ (void)name;(void)lifetime;(void)mechs;(void)usage; if(minor)*minor=0; if(cred)*cred=0; if(actual)*actual=0; if(actual_lifetime)*actual_lifetime=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_release_name(gss_OM_uint32 *minor, gss_name_t *name){ (void)name; if(minor)*minor=0; return 0; }
gss_OM_uint32 gss_accept_sec_context(gss_OM_uint32 *minor, gss_ctx_id_t *ctx, gss_cred_id_t cred, gss_buffer_t input_token, gss_channel_bindings_t bindings, gss_name_t *src_name, gss_OID *mech, gss_buffer_t output_token, gss_OM_uint32 *ret_flags, gss_OM_uint32 *time_rec, gss_cred_id_t *deleg_cred){ (void)cred;(void)input_token;(void)bindings; if(minor)*minor=0; if(ctx)*ctx=0; if(src_name)*src_name=0; if(mech)*mech=0; if(output_token){output_token->length=0;output_token->value=0;} if(ret_flags)*ret_flags=0; if(time_rec)*time_rec=0; if(deleg_cred)*deleg_cred=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_display_name(gss_OM_uint32 *minor, gss_name_t name, gss_buffer_t output, gss_OID *type){ (void)name; if(minor)*minor=0; if(output){output->length=0;output->value=0;} if(type)*type=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_release_buffer(gss_OM_uint32 *minor, gss_buffer_t buf){ (void)buf; if(minor)*minor=0; return 0; }
gss_OM_uint32 gss_verify_mic(gss_OM_uint32 *minor, gss_ctx_id_t ctx, gss_buffer_t msg, gss_buffer_t token, gss_qop_t *qop){ (void)ctx;(void)msg;(void)token; if(minor)*minor=0; if(qop)*qop=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_inquire_cred(gss_OM_uint32 *minor, gss_cred_id_t cred, gss_name_t *name, gss_OM_uint32 *lifetime, gss_cred_usage_t *usage, gss_OID_set *mechs){ (void)cred; if(minor)*minor=0; if(name)*name=0; if(lifetime)*lifetime=0; if(usage)*usage=0; if(mechs)*mechs=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_inquire_cred_by_mech(gss_OM_uint32 *minor, gss_cred_id_t cred, gss_OID mech, gss_name_t *name, gss_OM_uint32 *initiator_lifetime, gss_OM_uint32 *acceptor_lifetime, gss_cred_usage_t *usage){ (void)cred;(void)mech; if(minor)*minor=0; if(name)*name=0; if(initiator_lifetime)*initiator_lifetime=0; if(acceptor_lifetime)*acceptor_lifetime=0; if(usage)*usage=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_init_sec_context(gss_OM_uint32 *minor, gss_cred_id_t cred, gss_ctx_id_t *ctx, gss_name_t name, gss_OID mech, gss_OM_uint32 req_flags, gss_OM_uint32 lifetime, gss_channel_bindings_t bindings, gss_buffer_t input_token, gss_OID *actual_mech, gss_buffer_t output_token, gss_OM_uint32 *ret_flags, gss_OM_uint32 *time_rec){ (void)cred;(void)name;(void)mech;(void)req_flags;(void)lifetime;(void)bindings;(void)input_token; if(minor)*minor=0; if(ctx)*ctx=0; if(actual_mech)*actual_mech=0; if(output_token){output_token->length=0;output_token->value=0;} if(ret_flags)*ret_flags=0; if(time_rec)*time_rec=0; return LZ_GSS_UNAVAILABLE; }
gss_OM_uint32 gss_get_mic(gss_OM_uint32 *minor, gss_ctx_id_t ctx, gss_qop_t qop, gss_buffer_t msg, gss_buffer_t token){ (void)ctx;(void)qop;(void)msg; if(minor)*minor=0; if(token){token->length=0;token->value=0;} return LZ_GSS_UNAVAILABLE; }
