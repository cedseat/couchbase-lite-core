//
// c4Certificate.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Internal.hh"
#include "c4ExceptionUtils.hh"
#include "c4Database.h"
#include "c4Certificate.h"
#include "Certificate.hh"
#include "PublicKey.hh"
#include "c4.hh"
#include <vector>

#undef ENABLE_SENDING_CERT_REQUESTS
#ifdef ENABLE_SENDING_CERT_REQUESTS
#include "CertRequest.hh"
#endif


#ifdef COUCHBASE_ENTERPRISE

using namespace fleece;
using namespace litecore::crypto;
using namespace c4Internal;


static inline CertBase* internal(C4Cert *cert)    {return (CertBase*)cert;}
static inline C4Cert* external(CertBase *cert)    {return (C4Cert*)cert;}

static C4Cert* retainedExternal(CertBase *cert) {
    return external(retain(cert));
}

static CertSigningRequest* asUnsignedCert(C4Cert *cert NONNULL, C4Error *outError =nullptr) {
    if (internal(cert)->isSigned()) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Cert already signed"_sl, outError);
        return nullptr;
    }
    return (CertSigningRequest*)cert;
}

static Cert* asSignedCert(C4Cert *cert NONNULL, C4Error *outError =nullptr) {
    if (!internal(cert)->isSigned()) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Cert not signed"_sl, outError);
        return nullptr;
    }
    return (Cert*)cert;
}


static inline Key* internal(C4KeyPair *key)    {return (Key*)key;}
static inline C4KeyPair* external(Key *key)    {return (C4KeyPair*)key;}

static C4KeyPair* retainedExternal(Key *key) {
    return external(retain(key));
}

LITECORE_UNUSED static PublicKey* publicKey(C4KeyPair *c4key NONNULL) {
    auto key = internal(c4key);
    return key->isPrivate() ? ((PrivateKey*)key)->publicKey().get() : (PublicKey*)key;
}

static PrivateKey* privateKey(C4KeyPair *c4key NONNULL) {
    auto key = internal(c4key);
    return key->isPrivate() ? (PrivateKey*)key : nullptr;
}

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
static PersistentPrivateKey* persistentPrivateKey(C4KeyPair *c4key NONNULL) {
    if (PrivateKey *priv = privateKey(c4key); priv)
        return priv->asPersistent();
    return nullptr;
}
#endif


const C4CertIssuerParameters kDefaultCertIssuerParameters = {
    CertSigningRequest::kOneYear,
    C4STR("1"),
    -1,
    false,
    true,
    true,
    true
};


#pragma mark - C4CERT:


C4Cert* c4cert_createRequest(const C4CertNameComponent *nameComponents,
                             size_t nameCount,
                             C4CertUsage certUsages,
                             C4KeyPair *subjectKey C4NONNULL,
                             C4Error *outError) C4API
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        vector<DistinguishedName::Entry> name;
        SubjectAltNames altNames;
        for (size_t i = 0; i < nameCount; ++i) {
            auto attributeID = nameComponents[i].attributeID;
            if (auto tag = SubjectAltNames::tagNamed(attributeID); tag)
                altNames.emplace_back(*tag, nameComponents[i].value);
            else
                name.push_back({attributeID, nameComponents[i].value});
        }
        Cert::SubjectParameters params(name);
        params.subjectAltNames = move(altNames);
        params.nsCertType = NSCertType(certUsages);
        return retainedExternal(new CertSigningRequest(params, privateKey(subjectKey)));
    });
}


C4Cert* c4cert_fromData(C4Slice certData, C4Error *outError) C4API {
    return tryCatch<C4Cert*>(outError, [&]() {
        return retainedExternal(new Cert(certData));
    });
}


C4Cert* c4cert_requestFromData(C4Slice certRequestData, C4Error *outError) C4API {
#ifdef ENABLE_CERT_REQUEST
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        return retainedExternal(new CertSigningRequest(certRequestData));
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented,
                   "Certificate requests are disabled"_sl, outError);
    return nullptr;
#endif
}


C4SliceResult c4cert_copyData(C4Cert* cert, bool pemEncoded) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(internal(cert)->data(pemEncoded ? KeyFormat::PEM : KeyFormat::DER));
    });
}


C4StringResult c4cert_subjectName(C4Cert* cert) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        return C4StringResult(internal(cert)->subjectName());
    });
}


C4StringResult c4cert_subjectNameComponent(C4Cert* cert, C4CertNameAttributeID attrID) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        if (auto tag = SubjectAltNames::tagNamed(attrID); tag)
            return C4StringResult(internal(cert)->subjectAltNames()[*tag]);
        else
            return C4StringResult(internal(cert)->subjectName()[attrID]);
    });
}


bool c4cert_subjectNameAtIndex(C4Cert* cert,
                               unsigned index,
                               C4CertNameInfo *outInfo C4NONNULL) C4API
{
    outInfo->id = nullslice;
    outInfo->value = {};

    // First go through the DistinguishedNames:
    if (auto dn = internal(cert)->subjectName().asVector(); index < dn.size()) {
        outInfo->id = dn[index].first;
        outInfo->value = C4StringResult(dn[index].second);
        return true;
    } else {
        index -= dn.size();
    }

    // Then look in SubjectAlternativeName:
    if (auto san = internal(cert)->subjectAltNames(); index < san.size()) {
        outInfo->id = SubjectAltNames::nameOfTag(san[index].first);
        outInfo->value = C4StringResult(alloc_slice(san[index].second));
        return true;
    }

    return false;
}


C4CertUsage c4cert_usages(C4Cert* cert) C4API {
    return internal(cert)->nsCertType();
}


C4StringResult c4cert_summary(C4Cert* cert) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4StringResult(internal(cert)->summary());
    });
}


void c4cert_getValidTimespan(C4Cert* cert C4NONNULL,
                             C4Timestamp *outCreated,
                             C4Timestamp *outExpires)
{
    try {
        if (Cert *signedCert = asSignedCert(cert); signedCert) {
            time_t tCreated, tExpires;
            tie(tCreated, tExpires) = signedCert->validTimespan();
            *outCreated = C4Timestamp(difftime(tCreated, 0) * 1000.0);
            *outExpires = C4Timestamp(difftime(tExpires, 0) * 1000.0);
            return;
        }
    } catch (...) { }
    *outCreated = *outExpires = 0;
}


bool c4cert_isSigned(C4Cert* cert) C4API {
    return internal(cert)->isSigned();
}


C4Cert* c4cert_signRequest(C4Cert *c4Cert,
                           const C4CertIssuerParameters *c4Params,
                           C4KeyPair *issuerPrivateKey,
                           C4Cert *issuerC4Cert,
                           C4Error *outError) C4API
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        auto csr = asUnsignedCert(c4Cert, outError);
        if (!csr)
            return nullptr;
        if (!privateKey(issuerPrivateKey)) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "No private key"_sl, outError);
            return nullptr;
        }

        // Construct the issuer parameters:
        if (!c4Params)
            c4Params = &kDefaultCertIssuerParameters;
        CertSigningRequest::IssuerParameters params;
        params.validity_secs = c4Params->validityInSeconds;
        params.serial = c4Params->serialNumber;
        params.max_pathlen = c4Params->maxPathLen;
        params.is_ca = c4Params->isCA;
        params.add_authority_identifier = c4Params->addAuthorityIdentifier;
        params.add_subject_identifier = c4Params->addSubjectIdentifier;
        params.add_basic_constraints = c4Params->addBasicConstraints;

        // Get the issuer cert:
        Cert *issuerCert = nullptr;
        if (issuerC4Cert) {
            issuerCert = asSignedCert(issuerC4Cert);
            if (!issuerCert) {
                c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter,
                               "issuerCert is not signed"_sl, outError);
                return nullptr;
            }
        }

        // Sign!
        Retained<Cert> cert = csr->sign(params, privateKey(issuerPrivateKey), issuerCert);
        return retainedExternal(cert.get());
    });
}


bool c4cert_sendSigningRequest(C4Cert *c4Cert C4NONNULL,
                               C4Address address,
                               C4Slice optionsDictFleece,
                               C4CertSigningCallback callback C4NONNULL,
                               void *context,
                               C4Error *outError) C4API
{
#ifdef ENABLE_SENDING_CERT_REQUESTS
    auto csr = asUnsignedCert(c4Cert, outError);
    if (!csr)
        return false;
    return tryCatch(outError, [&] {
        auto request = retained(new litecore::REST::CertRequest());
        request->start(csr,
                       net::Address(address),
                       AllocedDict(optionsDictFleece),
                       [=](crypto::Cert *cert, C4Error error) {
                           callback(context, external(cert), error);
                       });
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "Sending CSRs is disabled"_sl, outError);
    return false;
#endif
}


C4KeyPair* c4cert_getPublicKey(C4Cert* cert) C4API {
    return tryCatch<C4KeyPair*>(nullptr, [&]() -> C4KeyPair* {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return retainedExternal(signedCert->subjectPublicKey());
        return nullptr;
    });
}


C4KeyPair* c4cert_loadPersistentPrivateKey(C4Cert* cert, C4Error *outError) C4API {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (auto signedCert = asSignedCert(cert, outError); signedCert) {
            if (auto key = signedCert->loadPrivateKey(); key)
                return retainedExternal(key);
        }
        return nullptr;
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
    return nullptr;
#endif
}


C4Cert* c4cert_nextInChain(C4Cert* cert) C4API {
    return tryCatch<C4Cert*>(nullptr, [&]() -> C4Cert* {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return retainedExternal(signedCert->next());
        return nullptr;
    });
}

C4SliceResult c4cert_copyChainData(C4Cert* cert) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return C4SliceResult(signedCert->dataOfChain());
        else
            return c4cert_copyData(cert, true);
    });
}



static constexpr slice kCertStoreName = "certs"_sl;


bool c4cert_save(C4Cert *cert,
                 bool entireChain,
                 C4Database *db C4NONNULL,
                 C4String name,
                 C4Error *outError)
{
    C4SliceResult data = {};
    if (cert) {
        if (entireChain)
            data = c4cert_copyChainData(cert);
        else
            data = c4cert_copyData(cert, false);
    }
    return c4raw_put(db, kCertStoreName, name, nullslice, alloc_slice(data), outError);
}


/** Loads a certificate from a database given the name it was saved under. */
C4Cert* c4cert_load(C4Database *db C4NONNULL,
                    C4String name,
                    C4Error *outError)
{
    c4::ref<C4RawDocument> doc = c4raw_get(db, kCertStoreName, name, outError);
    if (!doc)
        return nullptr;
    return c4cert_fromData(doc->body, outError);
}


#pragma mark - C4KEY:


C4KeyPair* c4keypair_generate(C4KeyPairAlgorithm algorithm,
                              unsigned sizeInBits,
                              bool persistent,
                              C4Error *outError) C4API
{
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (algorithm != kC4RSA) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Invalid algorithm"_sl, outError);
            return nullptr;
        }
        Retained<PrivateKey> privateKey;
        if (persistent) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
            privateKey = PersistentPrivateKey::generateRSA(sizeInBits);
#else
            c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
            return nullptr;
#endif
        } else {
            privateKey = PrivateKey::generateTemporaryRSA(sizeInBits);
        }
        return retainedExternal(privateKey);
    });
}


C4KeyPair* c4keypair_fromPublicKeyData(C4Slice publicKeyData, C4Error *outError) C4API {
    return tryCatch<C4KeyPair*>(outError, [&]() {
        return retainedExternal(new PublicKey(publicKeyData));
    });
}


C4KeyPair* c4keypair_fromPrivateKeyData(C4Slice data, C4Slice password, C4Error *outError) C4API
{
    return tryCatch<C4KeyPair*>(outError, [&]() {
        return retainedExternal(new PrivateKey(data, password));
    });
}


C4KeyPair* c4keypair_persistentWithPublicKey(C4KeyPair* key, C4Error *outError) C4API {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (auto persistent = persistentPrivateKey(key); persistent != nullptr)
            return retainedExternal(persistent);
        auto privKey = PersistentPrivateKey::withPublicKey(publicKey(key));
        if (!privKey) {
            clearError(outError);
            return nullptr;
        }
        return retainedExternal(privKey);
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
    return nullptr;
#endif
}


bool c4keypair_hasPrivateKey(C4KeyPair* key) C4API {
    return privateKey(key) != nullptr;
}


bool c4keypair_isPersistent(C4KeyPair* key) C4API {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return persistentPrivateKey(key) != nullptr;
#else
    return false;
#endif
}


C4SliceResult c4keypair_publicKeyDigest(C4KeyPair* key) C4API {
    return sliceResult(internal(key)->digestString());
}


C4SliceResult c4keypair_publicKeyData(C4KeyPair* key) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(internal(key)->publicKeyData());
    });
}


C4SliceResult c4keypair_privateKeyData(C4KeyPair* key) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        if (auto priv = privateKey(key); priv && priv->isPrivateKeyDataAvailable())
            return C4SliceResult(priv->privateKeyData());
        return C4SliceResult{};
    });
}


bool c4keypair_removePersistent(C4KeyPair* key, C4Error *outError) C4API {
    if (!privateKey(key)) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "No private key"_sl, outError);
        return false;
    }
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch(outError, [&]() {
        if (auto persistentKey = persistentPrivateKey(key); persistentKey)
            persistentKey->remove();
    });
#else
    return true;
#endif
}


#endif // COUCHBASE_ENTERPRISE
