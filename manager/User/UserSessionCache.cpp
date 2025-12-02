#include "UserSessionCache.h"
#include "server/Manager.h"
#include "Common/config.h"
#include "Storage/UserSession.h"
#include "Util/base64.h"
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>

using namespace std;
using namespace toolkit;
using namespace mediakit;
using traits = jwt::traits::open_source_parsers_jsoncpp;

namespace managerkit {

bool UserSessionHelper::verifyJwtToken(const string &jwt_token) {
    try {
        // step 1. parsing token
        std::string token = jwt_token;
        auto decoded = jwt::decode<traits>(token);

        // step 2. get encoded-base64 public key and decode 
        GET_CONFIG(string, public_key, Manager::kJwtPublicKey)
        if (public_key.empty()) {
            WarnL << "Public key empty. Ignore verify jwt token.";
            return false;
        }
        auto decoded_public_key = decodeBase64(public_key);

        // step 3. verify jwt token
        jwt::verify<traits>()
            .allow_algorithm(jwt::algorithm::rs256(decoded_public_key)) // allow RS256 algorithm
            .leeway(300) // 5 minutes leeway for nbf, iat, exp
            .verify(decoded); // verify token

    } catch (exception &ex) {
        WarnL << "Verify jwt token failed: " << ex.what();
        return false;
    }

    return true;
}

bool UserSessionHelper::decodeJwtToken(Json::Value &decoded_payload, const string &jwt_token) {
    try {
        // step 1. parsing token
        std::string token = jwt_token;
        auto decoded = jwt::decode<traits>(token);

        // step 2. get decoded token, include user_id, sub
        auto payload = decoded.get_payload_json();
        decoded_payload = payload;
    } catch (exception &ex) {
        WarnL << "Decode jwt token failed: " << ex.what();
        return false;
    }

    return true;
}

UserSessionCache::UserSessionCache(const string &token) : _token(token) {
    if (!UserSessionHelper::verifyJwtToken(token)) {
        WarnL << "Invalid token: " << token;
        _has_access = false;
        _created_at = time(nullptr);
        _expired_at = _created_at + 600;
        return;
    }

    Json::Value decoded_payload;
    UserSessionHelper::decodeJwtToken(decoded_payload, token);
    _user_id = decoded_payload["user_id"].asString();
    _user_name = decoded_payload["sub"].asString();
    _created_at = time(nullptr);
    _expired_at = decoded_payload["exp"].asUInt64();
    _project_id = decoded_payload["project_id"].asString();
    _level = decoded_payload["level"].asInt();
    _session_id = decoded_payload["session_id"].asString();
    auto permission_str = decoded_payload["permissions"].asString();
    _permissions = split(permission_str, ",");

    GET_CONFIG(string, mediaServerProjectId, Manager::kMediaServerProjectId)
    if (mediaServerProjectId != _project_id && _level != 0) {
        WarnL << "No permission to access project: " << _project_id;
        _has_access = false;
        return;
    }
    _has_access = true;

    saveUserSession();
}

void UserSessionCache::saveUserSession() {
    UserSession session;
    session.token = _token;
    session.userId = _user_id;
    session.creationTimeS = _created_at;
    auto imp_session = std::make_shared<UserSessionImp>();
    imp_session->add(session, _user_name);
}

} // namespace managerkit
