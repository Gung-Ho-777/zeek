// See the file "COPYING" in the main distribution directory for copyright.

#include <string>

#include "X509.h"
#include "Event.h"

#include "events.bif.h"
#include "types.bif.h"

#include "file_analysis/File.h"
#include "file_analysis/Manager.h"

#include <broker/error.hh>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>
#include <openssl/opensslconf.h>
#include <openssl/err.h>

using namespace file_analysis;

file_analysis::X509::X509(RecordVal* args, file_analysis::File* file)
	: file_analysis::X509Common::X509Common(file_mgr->GetComponentTag("X509"), args, file)
	{
	cert_data.clear();
	}

bool file_analysis::X509::DeliverStream(const u_char* data, uint64_t len)
	{
	// just add it to the data we have so far, since we cannot do anything else anyways...
	cert_data.append(reinterpret_cast<const char*>(data), len);
	return true;
	}

bool file_analysis::X509::Undelivered(uint64_t offset, uint64_t len)
	{
	return false;
	}

bool file_analysis::X509::EndOfFile()
	{
	const unsigned char* cert_char = reinterpret_cast<const unsigned char*>(cert_data.data());
	if ( certificate_cache )
		{
		// first step - let's see if the certificate has been cached.
		unsigned char buf[SHA256_DIGEST_LENGTH];
		auto ctx = hash_init(Hash_SHA256);
		hash_update(ctx, cert_char, cert_data.size());
		hash_final(ctx, buf);
		std::string cert_sha256 = sha256_digest_print(buf);
		auto index = make_intrusive<StringVal>(cert_sha256);
		auto entry = certificate_cache->Lookup(index.get(), false);
		if ( entry )
			// in this case, the certificate is in the cache and we do not
			// do any further processing here. However, if there is a callback, we execute it.
			{
			if ( ! cache_hit_callback )
				return false;
			// yup, let's call the callback.

			cache_hit_callback->Call(IntrusivePtr{NewRef{}, GetFile()->GetVal()},
			                         std::move(entry),
			                         make_intrusive<StringVal>(cert_sha256));
			return false;
			}
		}

	// ok, now we can try to parse the certificate with openssl. Should
	// be rather straightforward...
	::X509* ssl_cert = d2i_X509(NULL, &cert_char, cert_data.size());
	if ( ! ssl_cert )
		{
		reporter->Weird(GetFile(), "x509_cert_parse_error");
		return false;
		}

	X509Val* cert_val = new X509Val(ssl_cert); // cert_val takes ownership of ssl_cert

	// parse basic information into record.
	RecordVal* cert_record = ParseCertificate(cert_val, GetFile());

	// and send the record on to scriptland
	if ( x509_certificate )
		mgr.Enqueue(x509_certificate,
		            IntrusivePtr{NewRef{}, GetFile()->GetVal()},
		            IntrusivePtr{NewRef{}, cert_val},
		            IntrusivePtr{NewRef{}, cert_record});

	// after parsing the certificate - parse the extensions...

	int num_ext = X509_get_ext_count(ssl_cert);
	for ( int k = 0; k < num_ext; ++k )
		{
		X509_EXTENSION* ex = X509_get_ext(ssl_cert, k);
		if ( ! ex )
			continue;

		ParseExtension(ex, x509_extension, false);
		}

	// X509_free(ssl_cert); We do _not_ free the certificate here. It is refcounted
	// inside the X509Val that is sent on in the cert record to scriptland.
	//
	// The certificate will be freed when the last X509Val is Unref'd.

	Unref(cert_record); // Unref the RecordVal that we kept around from ParseCertificate
	Unref(cert_val); // Same for cert_val

	return false;
	}

RecordVal* file_analysis::X509::ParseCertificate(X509Val* cert_val, File* f)
	{
	::X509* ssl_cert = cert_val->GetCertificate();

	char buf[2048]; // we need a buffer for some of the openssl functions
	memset(buf, 0, sizeof(buf));

	RecordVal* pX509Cert = new RecordVal(BifType::Record::X509::Certificate);
	BIO *bio = BIO_new(BIO_s_mem());

	pX509Cert->Assign(0, val_mgr->GetCount((uint64_t) X509_get_version(ssl_cert) + 1));
	i2a_ASN1_INTEGER(bio, X509_get_serialNumber(ssl_cert));
	int len = BIO_read(bio, buf, sizeof(buf));
	pX509Cert->Assign(1, make_intrusive<StringVal>(len, buf));
	BIO_reset(bio);

	X509_NAME_print_ex(bio, X509_get_subject_name(ssl_cert), 0, XN_FLAG_RFC2253);
	len = BIO_gets(bio, buf, sizeof(buf));
	pX509Cert->Assign(2, make_intrusive<StringVal>(len, buf));
	BIO_reset(bio);

	X509_NAME *subject_name = X509_get_subject_name(ssl_cert);
	// extract the most specific (last) common name from the subject
	int namepos = -1;
	for ( ;; )
		{
		int j = X509_NAME_get_index_by_NID(subject_name, NID_commonName, namepos);
		if ( j == -1 )
			break;

		namepos = j;
		}

	if ( namepos != -1 )
		{
		// we found a common name
		ASN1_STRING_print(bio, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject_name, namepos)));
		len = BIO_gets(bio, buf, sizeof(buf));
		pX509Cert->Assign(4, make_intrusive<StringVal>(len, buf));
		BIO_reset(bio);
		}

	X509_NAME_print_ex(bio, X509_get_issuer_name(ssl_cert), 0, XN_FLAG_RFC2253);
	len = BIO_gets(bio, buf, sizeof(buf));
	pX509Cert->Assign(3, make_intrusive<StringVal>(len, buf));
	BIO_free(bio);

	pX509Cert->Assign(5, make_intrusive<Val>(GetTimeFromAsn1(X509_get_notBefore(ssl_cert), f, reporter), TYPE_TIME));
	pX509Cert->Assign(6, make_intrusive<Val>(GetTimeFromAsn1(X509_get_notAfter(ssl_cert), f, reporter), TYPE_TIME));

	// we only read 255 bytes because byte 256 is always 0.
	// if the string is longer than 255, that will be our null-termination,
	// otherwhise i2t does null-terminate.
	ASN1_OBJECT *algorithm;
	X509_PUBKEY_get0_param(&algorithm, NULL, NULL, NULL, X509_get_X509_PUBKEY(ssl_cert));
	if ( ! i2t_ASN1_OBJECT(buf, 255, algorithm) )
		buf[0] = 0;

	pX509Cert->Assign(7, make_intrusive<StringVal>(buf));

	// Special case for RDP server certificates. For some reason some (all?) RDP server
	// certificates like to specify their key algorithm as md5WithRSAEncryption, which
	// is wrong on so many levels. We catch this special case here and set it to what is
	// actually should be (namely - rsaEncryption), so that OpenSSL will parse out the
	// key later. Otherwise it will just fail to parse the certificate key.

	if ( OBJ_obj2nid(algorithm) == NID_md5WithRSAEncryption )
		{
		ASN1_OBJECT *copy = OBJ_dup(algorithm); // the next line will destroy the original algorithm.
		X509_PUBKEY_set0_param(X509_get_X509_PUBKEY(ssl_cert), OBJ_nid2obj(NID_rsaEncryption), 0, NULL, NULL, 0);
		algorithm = copy;
		// we do not have to worry about freeing algorithm in that case - since it will be re-assigned using
		// set0_param and the cert will take ownership.
		}
	else
		algorithm = 0;

	if ( ! i2t_ASN1_OBJECT(buf, 255, OBJ_nid2obj(X509_get_signature_nid(ssl_cert))) )
		buf[0] = 0;

	pX509Cert->Assign(8, make_intrusive<StringVal>(buf));

	// Things we can do when we have the key...
	EVP_PKEY *pkey = X509_extract_key(ssl_cert);
	if ( pkey != NULL )
		{
		if ( EVP_PKEY_base_id(pkey) == EVP_PKEY_DSA )
			pX509Cert->Assign(9, make_intrusive<StringVal>("dsa"));

		else if ( EVP_PKEY_base_id(pkey) == EVP_PKEY_RSA )
			{
			pX509Cert->Assign(9, make_intrusive<StringVal>("rsa"));

			const BIGNUM *e;
			RSA_get0_key(EVP_PKEY_get0_RSA(pkey), NULL, &e, NULL);
			char *exponent = BN_bn2dec(e);
			if ( exponent != NULL )
				{
				pX509Cert->Assign(11, make_intrusive<StringVal>(exponent));
				OPENSSL_free(exponent);
				exponent = NULL;
				}
			}
#ifndef OPENSSL_NO_EC
		else if ( EVP_PKEY_base_id(pkey) == EVP_PKEY_EC )
			{
			pX509Cert->Assign(9, make_intrusive<StringVal>("ecdsa"));
			pX509Cert->Assign(12, KeyCurve(pkey));
			}
#endif

		// set key algorithm back. We do not have to free the value that we created because (I think) it
		// comes out of a static array from OpenSSL memory.
		if ( algorithm )
			X509_PUBKEY_set0_param(X509_get_X509_PUBKEY(ssl_cert), algorithm, 0, NULL, NULL, 0);

		unsigned int length = KeyLength(pkey);
		if ( length > 0 )
			pX509Cert->Assign(10, val_mgr->GetCount(length));

		EVP_PKEY_free(pkey);
		}


	return pX509Cert;
	}

X509_STORE* file_analysis::X509::GetRootStore(TableVal* root_certs)
	{
	// If this certificate store was built previously, just reuse the old one.
	if ( x509_stores.count(root_certs) > 0 )
		return x509_stores[root_certs];

	X509_STORE* ctx = X509_STORE_new();
	ListVal* idxs = root_certs->ConvertToPureList();

	// Build the validation store
	for ( int i = 0; i < idxs->Length(); ++i )
		{
		Val* key = idxs->Index(i);
		StringVal *sv = root_certs->Lookup(key)->AsStringVal();
		assert(sv);
		const uint8_t* data = sv->Bytes();
		::X509* x = d2i_X509(NULL, &data, sv->Len());
		if ( ! x )
			{
			builtin_error(fmt("Root CA error: %s", ERR_error_string(ERR_get_error(), NULL)));
			return nullptr;
			}

		X509_STORE_add_cert(ctx, x);
		X509_free(x);
		}

	delete idxs;

	// Save the newly constructed certificate store into the cacheing map.
	x509_stores[root_certs] = ctx;

	return ctx;
	}

void file_analysis::X509::FreeRootStore()
	{
	for ( const auto& e : x509_stores )
		X509_STORE_free(e.second);
	}

void file_analysis::X509::ParseBasicConstraints(X509_EXTENSION* ex)
	{
	assert(OBJ_obj2nid(X509_EXTENSION_get_object(ex)) == NID_basic_constraints);

	BASIC_CONSTRAINTS *constr = (BASIC_CONSTRAINTS *) X509V3_EXT_d2i(ex);

	if ( constr )
		{
		if ( x509_ext_basic_constraints )
			{
			auto pBasicConstraint = make_intrusive<RecordVal>(BifType::Record::X509::BasicConstraints);
			pBasicConstraint->Assign(0, val_mgr->GetBool(constr->ca));

			if ( constr->pathlen )
				pBasicConstraint->Assign(1, val_mgr->GetCount((int32_t) ASN1_INTEGER_get(constr->pathlen)));

			mgr.Enqueue(x509_ext_basic_constraints,
				IntrusivePtr{NewRef{}, GetFile()->GetVal()},
				std::move(pBasicConstraint)
			);
			}

		BASIC_CONSTRAINTS_free(constr);
		}

	else
		reporter->Weird(GetFile(), "x509_invalid_basic_constraint");
	}

void file_analysis::X509::ParseExtensionsSpecific(X509_EXTENSION* ex, bool global, ASN1_OBJECT* ext_asn, const char* oid)
	{
	// look if we have a specialized handler for this event...
	if ( OBJ_obj2nid(ext_asn) == NID_basic_constraints )
		ParseBasicConstraints(ex);

	else if ( OBJ_obj2nid(ext_asn) == NID_subject_alt_name )
		ParseSAN(ex);

	// In OpenSSL 1.0.2+, we can get the extension by using NID_ct_precert_scts.
	// In OpenSSL <= 1.0.1, this is not yet defined yet, so we have to manually
	// look it up by performing a string comparison on the oid.
#ifdef NID_ct_precert_scts
	else if ( OBJ_obj2nid(ext_asn) == NID_ct_precert_scts )
#else
	else if ( strcmp(oid, "1.3.6.1.4.1.11129.2.4.2") == 0 )
#endif
		ParseSignedCertificateTimestamps(ex);
	}

void file_analysis::X509::ParseSAN(X509_EXTENSION* ext)
	{
	assert(OBJ_obj2nid(X509_EXTENSION_get_object(ext)) == NID_subject_alt_name);

	GENERAL_NAMES *altname = (GENERAL_NAMES*)X509V3_EXT_d2i(ext);
	if ( ! altname )
		{
		reporter->Weird(GetFile(), "x509_san_parse_error");
		return;
		}

	VectorVal* names = nullptr;
	VectorVal* emails = nullptr;
	VectorVal* uris = nullptr;
	VectorVal* ips = nullptr;

	bool otherfields = false;

	for ( int i = 0; i < sk_GENERAL_NAME_num(altname); i++ )
		{
		GENERAL_NAME *gen = sk_GENERAL_NAME_value(altname, i);
		assert(gen);

		if ( gen->type == GEN_DNS || gen->type == GEN_URI || gen->type == GEN_EMAIL )
			{
			if ( ASN1_STRING_type(gen->d.ia5) != V_ASN1_IA5STRING )
				{
				reporter->Weird(GetFile(), "x509_san_non_string");
				continue;
				}

#if ( OPENSSL_VERSION_NUMBER < 0x10100000L ) || defined(LIBRESSL_VERSION_NUMBER)
			const char* name = (const char*) ASN1_STRING_data(gen->d.ia5);
#else
			const char* name = (const char*) ASN1_STRING_get0_data(gen->d.ia5);
#endif
			StringVal* bs = new StringVal(name);

			switch ( gen->type )
				{
				case GEN_DNS:
					if ( names == nullptr )
						names = new VectorVal(internal_type("string_vec")->AsVectorType());

					names->Assign(names->Size(), bs);
					break;

				case GEN_URI:
					if ( uris == nullptr )
						uris = new VectorVal(internal_type("string_vec")->AsVectorType());

					uris->Assign(uris->Size(), bs);
					break;

				case GEN_EMAIL:
					if ( emails == nullptr )
						emails = new VectorVal(internal_type("string_vec")->AsVectorType());

					emails->Assign(emails->Size(), bs);
					break;
				}
			}

		else if ( gen->type == GEN_IPADD )
			{
				if ( ips == nullptr )
					ips = new VectorVal(internal_type("addr_vec")->AsVectorType());

				uint32_t* addr = (uint32_t*) gen->d.ip->data;

				if( gen->d.ip->length == 4 )
					ips->Assign(ips->Size(), make_intrusive<AddrVal>(*addr));

				else if ( gen->d.ip->length == 16 )
					ips->Assign(ips->Size(), make_intrusive<AddrVal>(addr));

				else
					{
					reporter->Weird(GetFile(), "x509_san_ip_length", fmt("%d", gen->d.ip->length));
					continue;
					}
			}

		else
			{
			// reporter->Error("Subject alternative name contained unsupported fields. fuid %s", GetFile()->GetID().c_str());
			// This happens quite often - just mark it
			otherfields = true;
			continue;
			}
		}

		auto sanExt = make_intrusive<RecordVal>(BifType::Record::X509::SubjectAlternativeName);

		if ( names != nullptr )
			sanExt->Assign(0, names);

		if ( uris != nullptr )
			sanExt->Assign(1, uris);

		if ( emails != nullptr )
			sanExt->Assign(2, emails);

		if ( ips != nullptr )
			sanExt->Assign(3, ips);

		sanExt->Assign(4, val_mgr->GetBool(otherfields));

		mgr.Enqueue(x509_ext_subject_alternative_name,
		            IntrusivePtr{NewRef{}, GetFile()->GetVal()},
		            std::move(sanExt));
	GENERAL_NAMES_free(altname);
	}

StringVal* file_analysis::X509::KeyCurve(EVP_PKEY *key)
	{
	assert(key != NULL);

#ifdef OPENSSL_NO_EC
	// well, we do not have EC-Support...
	return NULL;
#else
	if ( EVP_PKEY_base_id(key) != EVP_PKEY_EC )
		{
		// no EC-key - no curve name
		return nullptr;
		}

	const EC_GROUP *group;
	int nid;
	if ( (group = EC_KEY_get0_group(EVP_PKEY_get0_EC_KEY(key))) == NULL )
		// I guess we could not parse this
		return nullptr;

	nid = EC_GROUP_get_curve_name(group);
	if ( nid == 0 )
		// and an invalid nid...
		return nullptr;

	const char * curve_name = OBJ_nid2sn(nid);
	if ( curve_name == nullptr )
		return nullptr;

	return new StringVal(curve_name);
#endif
	}

unsigned int file_analysis::X509::KeyLength(EVP_PKEY *key)
	{
	assert(key != NULL);

	switch(EVP_PKEY_base_id(key)) {
	case EVP_PKEY_RSA:
		const BIGNUM *n;
		RSA_get0_key(EVP_PKEY_get0_RSA(key), &n, NULL, NULL);
		return BN_num_bits(n);

	case EVP_PKEY_DSA:
		const BIGNUM *p;
		DSA_get0_pqg(EVP_PKEY_get0_DSA(key), &p, NULL, NULL);
		return BN_num_bits(p);

#ifndef OPENSSL_NO_EC
	case EVP_PKEY_EC:
		{
		BIGNUM* ec_order = BN_new();
		if ( ! ec_order )
			// could not malloc bignum?
			return 0;

		const EC_GROUP *group = EC_KEY_get0_group(EVP_PKEY_get0_EC_KEY(key));

		if ( ! group )
			{
			// unknown ex-group
			BN_free(ec_order);
			return 0;
			}

		if ( ! EC_GROUP_get_order(group, ec_order, NULL) )
			{
			// could not get ec-group-order
			BN_free(ec_order);
			return 0;
			}

		unsigned int length = BN_num_bits(ec_order);
		BN_free(ec_order);
		return length;
		}
#endif
	default:
		return 0; // unknown public key type
	}

	reporter->InternalError("cannot be reached");
	}

X509Val::X509Val(::X509* arg_certificate) : OpaqueVal(x509_opaque_type)
	{
	certificate = arg_certificate;
	}

X509Val::X509Val() : OpaqueVal(x509_opaque_type)
	{
	certificate = 0;
	}

X509Val::~X509Val()
	{
	if ( certificate )
		X509_free(certificate);
	}

IntrusivePtr<Val> X509Val::DoClone(CloneState* state)
	{
	auto copy = make_intrusive<X509Val>();
	if ( certificate )
		copy->certificate = X509_dup(certificate);

	return state->NewClone(this, copy);
	}

::X509* X509Val::GetCertificate() const
	{
	return certificate;
	}

IMPLEMENT_OPAQUE_VALUE(X509Val)

broker::expected<broker::data> X509Val::DoSerialize() const
	{
	unsigned char *buf = nullptr;
	int length = i2d_X509(certificate, &buf);

	if ( length < 0 )
		return broker::ec::invalid_data;

	auto d = std::string(reinterpret_cast<const char*>(buf), length);
	OPENSSL_free(buf);

	return {std::move(d)};
	}

bool X509Val::DoUnserialize(const broker::data& data)
	{
	auto s = caf::get_if<std::string>(&data);
	if ( ! s )
		return false;

	auto opensslbuf = reinterpret_cast<const unsigned char*>(s->data());
	certificate = d2i_X509(NULL, &opensslbuf, s->size());
	return (certificate != nullptr);
	}
