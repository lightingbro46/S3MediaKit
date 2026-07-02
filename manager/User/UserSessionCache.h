#ifndef USER_USERSESSION_H
#define USER_USERSESSION_H

#include <string>
#include <json/json.h>

namespace managerkit {

#define SYSTEM_ADMIN_LEVEL 0
#define OWNER_PROJECT_LEVEL 1
#define NORMAL_USER_LEVEL 3

#define PTZ_CONTROL_PERMISSION_CODE "1001"
#define LIVE_VIEW_PERMISSION_CODE "1002"
#define PLAYBACK_PERMISSION_CODE "1003"
#define EXTRACT_PERMISSION_CODE "1004"
#define READ_BOOKMARK_PERMISSION_CODE "200101"
#define MODIFY_BOOKMARK_PERMISSION_CODE "200102"
#define READ_MSERVER_PERMISSION_CODE "150301"
#define MODIFY_MSERVER_PERMISSION_CODE "150302"
#define ADD_CAMERA_PERMISSION_CODE "40102"
#define VIDEOWALL_CONTROL_PERMISSION_CODE "1901"
#define VIDEOWALL_ATTACH_PERMISSION_CODE "1902"

class UserSessionHelper {
public:
    /**
     * Verify a JWT for syntactic correctness, signature validity and policy compliance.
    */
    static bool verifyJwtToken(const std::string &jwt_token);

    /**
     * Decode and validate a JSON Web Token (JWT) and update the user session cache.
    */
    static bool decodeJwtToken(Json::Value &decoded_payload, const std::string &jwt_token);
};

struct ClientOSInfo {
    std::string os;
    std::string browser;
    std::string browserVersion;
    std::string device;
};

/**
 * Authorization cache of user with device, default max elapsed is 60 seconds
 */
class UserSessionCache {
public:
    using Ptr = std::shared_ptr<UserSessionCache>;

    UserSessionCache(const std::string &token, const std::string &user_agent = "", const std::string &client_ip = "");

    /**
     * get created at timestamp, unit: second
     */
    uint64_t getCreatedAt() { return _created_at; }

    /**
     * get expired at timestamp, unit: second
     */
    uint64_t getExpiredAt() { return _expired_at; }

    /**
     * get user id in cache
     */
    std::string getUid() { return _user_id; }

    /**
     *  get user name in cache
     */
    std::string getUserName() { return _user_name; }

    /**
     * get project id in cache
     */
    std::string getProjectId() { return _project_id; }

    /**
     * return permisstion to access this project
     */
    bool hasProjectAccess() { return _has_access; }

    /**
     * return permisstion to access feature
     */
    bool hasPermissionCode(std::string code);

    /**
     * get client os info
     */
    ClientOSInfo getClientOSInfo() { return _client_os_info; }

    /**
     * get client ip
     */
    std::string getClientIp() { return _client_ip; }

private:
    /**
     *  save user session into database
     */
    void saveUserSession();

private:
    std::string _user_id;
    std::string _user_name;
    uint64_t _created_at;
    uint64_t _expired_at;
    std::string _token;
    std::vector<std::string> _permissions;
    std::string _project_id;
    int _level;
    std::string _session_id;
    bool _has_access = false;
    ClientOSInfo _client_os_info;
    std::string _client_ip;
};

} // namespace managerkit

#endif // USER_USERSESSION_H