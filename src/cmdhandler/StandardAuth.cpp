#include "StandardAuth.hpp"

#include <fstream>
#include <map>

#include <DateTime.hpp>
#include <ErrCode.hpp>
#include <Log.hpp>
#include <SafePtr.hpp>

extern "C" {
#define new fix_cpp_keyword_new
#include <crystal.h>
#include <ela_did.h>
#include <ela_jwt.h>
#include "../did.h"
#undef new
//} // ela_jwt error

namespace trinity {

#define CHECK_DIDSDK(expr, errCode, errDesp) \
    if(!(expr)) { \
        Log::E(Log::TAG, errDesp); \
        Log::D(Log::TAG, "Did sdk errCode:0x%x, errDesc:%s", \
               DIDError_GetCode(), DIDError_GetMessage()); \
        CHECK_ERROR(errCode); \
    }

/* =========================================== */
/* === static variables initialize =========== */
/* =========================================== */

/* =========================================== */
/* === static function implement ============= */
/* =========================================== */
std::filesystem::path StandardAuth::GetLocalDocDir()
{
    auto localDocDir = GetDataDir() / LocalDocDirName;
    auto dirExists = std::filesystem::exists(localDocDir);
    if(dirExists == false) {
        dirExists = std::filesystem::create_directories(localDocDir);
    }
    if(dirExists == false) {
        Log::E(Log::TAG, "No such directory: %s", localDocDir.c_str());
        localDocDir.clear();
    }

    return localDocDir;
}

int StandardAuth::SaveLocalDIDDocument(DID* did, DIDDocument* doc)
{
    CHECK_ASSERT(did && doc, ErrCode::InvalidArgument);

    auto localDocDir = GetLocalDocDir();
    CHECK_ASSERT(localDocDir.empty() == false, ErrCode::DirectoryNotExistsError);

    auto creater = [](DIDDocument* doc) -> const char* {
        return DIDDocument_ToJson(doc, false);
    };
    auto deleter = [](const char* ptr) -> void {
        if(ptr != nullptr) {
            free(const_cast<char*>(ptr));
        }
    };
    auto docStr = std::shared_ptr<const char>(creater(doc), deleter);
    CHECK_DIDSDK(docStr != nullptr, ErrCode::AuthBadDidDoc, "Failed to format did document to json.");

    auto docFilePath = localDocDir / DID_GetMethodSpecificId(did);
    Log::D(Log::TAG, "Save did document to local: %s", docFilePath.c_str());
    std::fstream docStream;
    docStream.open(docFilePath, std::ios::binary | std::ios::out);
    docStream.seekg(0);
    docStream.write(docStr.get(), std::strlen(docStr.get()) + 1);
    docStream.flush();
    docStream.close();

    return 0;
}

DIDDocument* StandardAuth::LoadLocalDIDDocument(DID* did)
{
    if(did == nullptr) {
        return nullptr;
    }

    {
        // adapt old process from local_resolver()
        auto oldResolverDoc = (DID_Equals(did, feeds_did) ? DIDStore_LoadDID(feeds_didstore, feeds_did) : NULL);
        if(oldResolverDoc != nullptr) {
            return oldResolverDoc; 
        }
    }

    auto localDocDir = GetLocalDocDir();
    if(localDocDir.empty() == true) {
        Log::E(Log::TAG, "Local did document directory is not set.");
        return nullptr;
    };

    auto docFilePath = localDocDir / DID_GetMethodSpecificId(did);
    auto fileExists = std::filesystem::exists(docFilePath);
    if(fileExists == false) {
        return nullptr;
    }
    Log::D(Log::TAG, "Load did document from local: %s", docFilePath.c_str());

    auto docSize = std::filesystem::file_size(docFilePath);
    char docStr[docSize];

    std::fstream docStream;
    docStream.open(docFilePath, std::ios::binary | std::ios::in);
    docStream.seekg(0);
    docStream.read(docStr, docSize);
    docStream.close();

    return DIDDocument_FromJson(docStr);
}

/* =========================================== */
/* === class public function implement  ====== */
/* =========================================== */
StandardAuth::StandardAuth()
{
    using namespace std::placeholders;
    std::map<const char*, Handler> cmdHandleMap {
        {Method::SignIn,  {std::bind(&StandardAuth::onSignIn, this, _1, _2), Accessible::Anyone}},
        {Method::DidAuth, {std::bind(&StandardAuth::onDidAuth, this, _1, _2), Accessible::Anyone}},
    };

    setHandleMap(cmdHandleMap);
}

StandardAuth::~StandardAuth()
{
}

/* =========================================== */
/* === class protected function implement  === */
/* =========================================== */


/* =========================================== */
/* === class private function implement  ===== */
/* =========================================== */
int StandardAuth::onSignIn(std::shared_ptr<Req> req,
                           std::shared_ptr<Resp> &resp)
{
    auto signInReq = std::reinterpret_pointer_cast<StandardSignInReq>(req);
    Log::D(Log::TAG, "Request params:");
    Log::D(Log::TAG, "    document: %s", signInReq->params.doc);

    auto docCreater = [](const char* json) -> DIDDocument* {
        return DIDDocument_FromJson(json);
    };
    auto docDeleter = [](DIDDocument* ptr) -> void {
        DIDDocument_Destroy(ptr);
    };
    auto didDoc = std::shared_ptr<DIDDocument>(docCreater(signInReq->params.doc), docDeleter);
    CHECK_DIDSDK(didDoc != nullptr, ErrCode::AuthBadDidDoc, "Failed to get did document from json.");

    bool valid = DIDDocument_IsValid(didDoc.get());
    CHECK_DIDSDK(valid, ErrCode::AuthDidDocInvlid, "Did document is invalid.");

    auto did = DIDDocument_GetSubject(didDoc.get());
    CHECK_DIDSDK(did, ErrCode::AuthBadDid, "Failed to get did from document.");

    char didStrBuf[ELA_MAX_DID_LEN];
    auto didStr = DID_ToString(did, didStrBuf, sizeof(didStrBuf));
    CHECK_DIDSDK(didStr, ErrCode::AuthBadDidString, "Failed to get did string.");
    Log::D(Log::TAG, "Sign in Did: %s", didStr);

    int ret = SaveLocalDIDDocument(did, didDoc.get());
    CHECK_DIDSDK(ret >= 0, ErrCode::AuthSaveDocFailed, "Failed to save did document to local.");

    auto expiration = DateTime::Current() + JWT_EXPIRATION;

    uint8_t nonce[NONCE_BYTES];
    char nonceStr[NONCE_BYTES << 1];
    crypto_random_nonce(nonce);
    crypto_nonce_to_str(nonce, nonceStr, sizeof(nonceStr));

    std::string challenge;
    ret = makeJwt(expiration, didStr, "DIDAuthChallenge",
                  {{"nonce", std::string(nonceStr)}},
                  challenge);
    CHECK_ERROR(ret);

    authSecretMap[nonceStr] = std::move(AuthSecret{didStr, expiration});

    auto signInResp = std::make_shared<StandardSignInResp>();
    signInResp->tsx_id = signInReq->tsx_id;
    auto challengeSize = challenge.length() + 1;
    signInResp->result.challenge = (char*)rc_zalloc(challengeSize, nullptr);
    strncpy(signInResp->result.challenge, challenge.data(), challengeSize);
    Log::D(Log::TAG, "Response result:");
    Log::D(Log::TAG, "    challenge: %s", signInResp->result.challenge);

    resp = std::reinterpret_pointer_cast<Resp>(signInResp);

    return 0;
}

int StandardAuth::onDidAuth(std::shared_ptr<Req> req,
                            std::shared_ptr<Resp> &resp)
{
    auto didAuthReq = std::reinterpret_pointer_cast<StandardDidAuthReq>(req);
    Log::D(Log::TAG, "Request params:");
    Log::D(Log::TAG, "    vp: %s", didAuthReq->params.vp);

    int ret;

    json credentialSubject;
    ret = checkAuthToken(didAuthReq->params.vp, credentialSubject);
    CHECK_ERROR(ret);

    std::string accessToken;
    ret = createAccessToken(credentialSubject, accessToken);
    CHECK_ERROR(ret);

    auto didAuthResp = std::make_shared<StandardDidAuthResp>();
    didAuthResp->tsx_id = didAuthReq->tsx_id;
    auto tokenSize = accessToken.length() + 1;
    didAuthResp->result.access_token = (char*)rc_zalloc(tokenSize, nullptr);
    strncpy(didAuthResp->result.access_token, accessToken.data(), tokenSize);
    Log::D(Log::TAG, "Response result:");
    Log::D(Log::TAG, "    access_token: %s", didAuthResp->result.access_token);

    resp = std::reinterpret_pointer_cast<Resp>(didAuthResp);

    return 0;
}

std::string StandardAuth::getServiceDid()
{
    char didStrBuf[ELA_MAX_DID_LEN] = {0};

    DID_ToString(DIDURL_GetDid(feeeds_auth_key_url), didStrBuf, sizeof(didStrBuf));

    return std::string(didStrBuf);
}

int StandardAuth::makeJwt(time_t expiration,
                          const std::string& audience,
                          const std::string& subject,
                          const std::map<const char*, std::string>& claimMap,
                          std::string& jwt)
{
    auto jwtCreater = [](DIDDocument* didDoc) -> JWTBuilder* {
        return DIDDocument_GetJwtBuilder(didDoc);
    };
    auto jwtDeleter = [](JWTBuilder* ptr) -> void {
        JWTBuilder_Destroy(ptr);
    };
    auto jwtBuilder = std::shared_ptr<JWTBuilder>(jwtCreater(feeds_doc), jwtDeleter);
    CHECK_DIDSDK(jwtBuilder != nullptr, ErrCode::AuthBadJwtBuilder, "Failed to get jwt builder from service did");

    int ret = JWTBuilder_SetHeader(jwtBuilder.get(), "typ", "JWT");
    CHECK_DIDSDK(ret, ErrCode::AuthBadJwtHeader, "Failed to set jwt header.");

    ret = JWTBuilder_SetHeader(jwtBuilder.get(), "version", "1.0");
    CHECK_DIDSDK(ret, ErrCode::AuthBadJwtHeader, "Failed to set jwt header.");

    ret = JWTBuilder_SetExpiration(jwtBuilder.get(), expiration);
    CHECK_DIDSDK(ret, ErrCode::AuthBadJwtExpiration, "Failed to set jwt expiration.");

    ret = JWTBuilder_SetAudience(jwtBuilder.get(), audience.c_str());
    CHECK_DIDSDK(ret, ErrCode::AuthBadJwtAudience, "Failed to set jwt audience.");

    ret = JWTBuilder_SetSubject(jwtBuilder.get(), subject.c_str());
    CHECK_DIDSDK(ret, ErrCode::AuthBadJwtSubject, "Failed to set jwt subject.");

    for(const auto& it: claimMap) {
        ret = JWTBuilder_SetClaim(jwtBuilder.get(), it.first, it.second.c_str());
        CHECK_DIDSDK(ret, ErrCode::AuthBadJwtClaim, "Failed to set jwt claim.");
    }

    int jwtSigned = JWTBuilder_Sign(jwtBuilder.get(), feeeds_auth_key_url, feeds_storepass);
    CHECK_DIDSDK(jwtSigned == 0, ErrCode::AuthJwtSignFailed, "Failed to sign jwt.");

    auto tokenCreater = [](JWTBuilder* jwtBuilder) -> const char* {
        return JWTBuilder_Compact(jwtBuilder);
    };
    auto tokenDeleter = [](const char* ptr) -> void {
        if(ptr != nullptr) {
            free(const_cast<char*>(ptr));
        }
    };
    auto token = std::shared_ptr<const char>(tokenCreater(jwtBuilder.get()), tokenDeleter);
    CHECK_DIDSDK(token != nullptr, ErrCode::AuthJwtCompactFailed, "Failed to compact jwt.");

    jwt = token.get();

    return 0;
}

int StandardAuth::checkAuthToken(const char* jwt, json& credentialSubject)
{
    CHECK_ASSERT(jwt != nullptr, ErrCode::InvalidArgument);

    /** check jwt token **/
    DIDBackend_SetLocalResolveHandle(StandardAuth::LoadLocalDIDDocument);
    auto jwsCreater = [](const char* jwt) -> JWT* {
        return DefaultJWSParser_Parse(jwt);
    };
    auto jwsDeleter = [](JWT* ptr) -> void {
        JWT_Destroy(ptr);
    };
    auto jws = std::shared_ptr<JWT>(jwsCreater(jwt), jwsDeleter);
    CHECK_DIDSDK(jws != nullptr, ErrCode::AuthBadJwtChallenge, "Failed to parse jws from jwt.");

    auto vpStrCreater = [](JWT *jws) -> const char* {
        return JWT_GetClaimAsJson(jws, "presentation");
    };
    auto vpStrDeleter = [](const char* ptr) -> void {
        if(ptr != nullptr) {
            free(reinterpret_cast<void*>(const_cast<char*>(ptr)));
        }
    };
    auto vpStr = std::shared_ptr<const char>(vpStrCreater(jws.get()), vpStrDeleter);
    CHECK_DIDSDK(vpStr != nullptr, ErrCode::AuthGetJwsClaimFailed, "Failed to get claim from jws.");

    auto vpCreater = [](const char *vpStr) -> Presentation* {
        return Presentation_FromJson(vpStr);
    };
    auto vpDeleter = [](Presentation *ptr) -> void {
        Presentation_Destroy(ptr);
    };
    auto vp = std::shared_ptr<Presentation>(vpCreater(vpStr.get()), vpDeleter);
    CHECK_DIDSDK(vp != nullptr, ErrCode::AuthGetPresentationFailed, "Failed to get presentation from json.");

    auto vpJson = json::parse(vpStr.get());

    /** check vp **/
    bool valid = Presentation_IsValid(vp.get());
    CHECK_DIDSDK(valid, ErrCode::AuthInvalidPresentation, "Failed to check presentation.");

    /** check nonce **/
    auto nonce = Presentation_GetNonce(vp.get());
    CHECK_DIDSDK(nonce != nullptr, ErrCode::AuthPresentationEmptyNonce, "Failed to get presentation nonce, return null.");

    auto authSecretIt = authSecretMap.find(nonce); // TODO: change to remove
    CHECK_DIDSDK(authSecretIt != authSecretMap.end(), ErrCode::AuthPresentationBadNonce, "Bad presentation nonce.");
    auto authSecret = authSecretIt->second;

    /** check realm **/
    auto realm = Presentation_GetRealm(vp.get());
    CHECK_DIDSDK(realm != nullptr, ErrCode::AuthPresentationEmptyRealm, "Failed to get presentation realm, return null.");
    CHECK_DIDSDK(getServiceDid() == realm, ErrCode::AuthPresentationBadRealm, "Bad presentation realm.");

    /** check vc **/
    auto count = Presentation_GetCredentialCount(vp.get());
    CHECK_DIDSDK(count >= 1, ErrCode::AuthVerifiableCredentialBadCount, "The credential count is error.");

    CHECK_DIDSDK(vpJson.contains("verifiableCredential"), ErrCode::AuthVerifiableCredentialNotExists, "The verifiable credential isn't exist.");
    auto vcsJson = vpJson["verifiableCredential"];
    CHECK_DIDSDK(vcsJson.is_array(), ErrCode::AuthVerifiableCredentialInvalid, "The verifiable credential isn't valid.");

    auto vcJson = vcsJson[0];
    CHECK_DIDSDK(vcJson.is_null() == false, ErrCode::AuthCredentialNotExists, "The credential isn't exist.");

    auto vcStr = vcJson.dump();
    CHECK_DIDSDK(vcStr.empty() == false, ErrCode::AuthCredentialSerialFailed, "Failed to serialize credential.");

    auto vcCreater = [](const char *vcStr) -> Credential* {
        return Credential_FromJson(vcStr, nullptr);
    };
    auto vcDeleter = [](Credential *ptr) -> void {
        Credential_Destroy(ptr);
    };
    auto vc = std::shared_ptr<Credential>(vcCreater(vcStr.c_str()), vcDeleter);
    CHECK_DIDSDK(vc != nullptr, ErrCode::AuthCredentialParseFailed, "The credential string is error, unable to rebuild to a credential object.");

    valid = Credential_IsValid(vc.get());
    CHECK_DIDSDK(valid, ErrCode::AuthCredentialInvalid, "The credential isn't valid.");

    CHECK_DIDSDK(vcJson.contains("credentialSubject"), ErrCode::AuthCredentialSubjectNotExists, "The credential subject isn't exist.");
    credentialSubject = vcJson["credentialSubject"];

    CHECK_DIDSDK(credentialSubject.contains("id"), ErrCode::AuthCredentialSubjectIdNotExists, "The credential subject's id isn't exist.");
    auto instanceDid = credentialSubject["id"];
    CHECK_ASSERT(instanceDid == authSecret.did, ErrCode::AuthCredentialSubjectBadInstanceId);

    CHECK_DIDSDK(credentialSubject.contains("appDid"), ErrCode::AuthCredentialSubjectAppIdNotExists, "The credential subject's id isn't exist.");

    bool expired = (authSecret.expiration < DateTime::Current());
    CHECK_ASSERT(expired == false, ErrCode::AuthNonceExpiredError);

    CHECK_DIDSDK(vcJson.contains("issuer"), ErrCode::AuthCredentialIssuerNotExists, "The credential issuer isn't exist.");
    credentialSubject["userDid"] = vcJson["issuer"];
    credentialSubject["nonce"] = nonce;

    auto expirationDate = Credential_GetExpirationDate(vc.get());
    CHECK_DIDSDK(expirationDate > 0, ErrCode::AuthCredentialExpirationError, "Faile to get credential expiration date.");
    credentialSubject["expTime"] = expirationDate;

    return 0;
}

int StandardAuth::createAccessToken(json& credentialSubject, std::string& accessToken)
{
    int ret;
    std::string userDid = credentialSubject["userDid"];
    std::string appId = credentialSubject["appDid"];
    std::string appInstanceDid = credentialSubject["id"];

    auto expTime = credentialSubject["expTime"];
    auto expiration = DateTime::Current() + ACCESS_EXPIRATION;
    if(expiration > expTime) {
        expiration = expTime;
    }

    ret = makeJwt(expiration, appInstanceDid, "AccessToken",
                  {{"userDid", userDid},
                   {"appId", appId},
                   {"appInstanceDid", appInstanceDid}},
                  accessToken);
    CHECK_ERROR(ret);

    return 0;
}

} // namespace trinity