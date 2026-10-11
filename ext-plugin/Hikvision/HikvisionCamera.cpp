#include "HikvisionCamera.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "Control/PsiaDeviceControl.h"
#include "Onvif/pugixml.hpp"

using namespace std;

namespace managerkit {

static const char kIntegratePath[] = "/ISAPI/System/Network/Integrate";
static const char kOnvifUsersPath[] = "/ISAPI/Security/ONVIF/users";
static const char kXml[] = "application/xml";

bool HikvisionCamera::parseIsapiDeviceInfo(const std::string &body, IDeviceInfo &info) {
    OnvifDeviceInfo parsed;
    // deviceName is whatever the user named the camera: never a manufacturer.
    if (!psia_detail::parseDeviceInfo(body, parsed, "Hikvision")) {
        return false;
    }
    info.manufacturer    = parsed.manufacturer;
    info.model           = parsed.model;
    info.firmwareVersion = parsed.firmwareVersion;
    info.serialNumber    = parsed.serialNumber;
    info.hardwareId      = parsed.hardwareId;
    info.macAddress      = parsed.macAddress;
    return true;
}

// Name of `node` without its namespace prefix.
static const char *localName(const pugi::xml_node &node) {
    const char *name = node.name();
    const char *colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static pugi::xml_node child(const pugi::xml_node &node, const char *name) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_element && strcmp(localName(c), name) == 0) {
            return c;
        }
    }
    return pugi::xml_node();
}

static string text(const pugi::xml_node &node, const char *name) {
    string value = child(node, name).text().get();
    value.erase(0, value.find_first_not_of(" \t\r\n"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);
    return value;
}

// <enable> of the <ONVIF> node of an Integrate document.
static pugi::xml_node onvifEnable(const pugi::xml_document &doc) {
    pugi::xml_node root = doc.document_element();
    if (!root || strcmp(localName(root), "Integrate") != 0) {
        return pugi::xml_node();
    }
    return child(child(root, "ONVIF"), "enable");
}

bool HikvisionCamera::parseIntegrateOnvif(const std::string &xml, bool &enabled) {
    pugi::xml_document doc;
    if (xml.empty() || !doc.load_buffer(xml.data(), xml.size())) {
        return false;
    }
    pugi::xml_node enable = onvifEnable(doc);
    if (!enable) {
        return false;
    }
    enabled = strcmp(enable.text().get(), "true") == 0;
    return true;
}

bool HikvisionCamera::enableIntegrateOnvif(const std::string &xml, std::string &out) {
    pugi::xml_document doc;
    if (xml.empty() || !doc.load_buffer(xml.data(), xml.size())) {
        return false;
    }
    pugi::xml_node enable = onvifEnable(doc);
    if (!enable) {
        return false;
    }
    enable.text().set("true");
    ostringstream oss;
    doc.save(oss, "", pugi::format_raw);
    out = oss.str();
    return true;
}

bool HikvisionCamera::parseOnvifUsers(const std::string &xml, std::vector<OnvifUser> &users) {
    pugi::xml_document doc;
    if (xml.empty() || !doc.load_buffer(xml.data(), xml.size())) {
        return false;
    }
    pugi::xml_node root = doc.document_element();
    if (!root || strcmp(localName(root), "UserList") != 0) {
        return false;
    }
    users.clear();
    for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_element && strcmp(localName(c), "User") == 0) {
            OnvifUser user;
            user.id = text(c, "id");
            user.name = text(c, "userName");
            users.push_back(user);
        }
    }
    return true;
}

std::string HikvisionCamera::onvifUserXml(const std::string &id, const std::string &name, const std::string &password) {
    pugi::xml_document doc;
    pugi::xml_node user = doc.append_child("User");
    user.append_attribute("version") = "2.0";
    user.append_attribute("xmlns") = "http://www.hikvision.com/ver20/XMLSchema";
    user.append_child("id").text().set(id.c_str());
    user.append_child("userName").text().set(name.c_str());
    user.append_child("password").text().set(password.c_str());
    // PTZ, encoder and clock writes need an administrator.
    user.append_child("userType").text().set("administrator");
    ostringstream oss;
    doc.save(oss, "", pugi::format_raw);
    return oss.str();
}

// A write answered 200 with a ResponseStatus that says OK (or no body).
static bool writeOk(const VendorHttpAnswer &answer) {
    if (answer.status != 200) {
        return false;
    }
    psia_detail::ResponseStatus status;
    return answer.body.empty() || !psia_detail::parseResponseStatus(answer.body, status) ||
           psia_detail::isResponseStatusOk(status);
}

void HikvisionCamera::prepareOnvif(const std::string &username, const std::string &password, OnDeviceResult cb) {
    // The pending request holds `cb`, which the caller makes hold this plugin.
    std::weak_ptr<HikvisionCamera> weak_self = std::static_pointer_cast<HikvisionCamera>(shared_from_this());
    request("GET", kIntegratePath, "", "", [weak_self, username, password, cb](const VendorHttpAnswer &answer) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        bool enabled = false;
        if (answer.status != 200 || !parseIntegrateOnvif(answer.body, enabled)) {
            self->failPrepare(answer, true, "read the ONVIF switch", cb);
            return;
        }
        if (enabled) {
            self->ensureOnvifUser(username, password, cb);
            return;
        }
        string integrate;
        enableIntegrateOnvif(answer.body, integrate);
        self->request("PUT", kIntegratePath, integrate, kXml, [weak_self, username, password, cb](const VendorHttpAnswer &answer) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (!writeOk(answer)) {
                self->failPrepare(answer, false, "turn ONVIF on", cb);
                return;
            }
            self->ensureOnvifUser(username, password, cb);
        });
    });
}

void HikvisionCamera::ensureOnvifUser(const std::string &username, const std::string &password, const OnDeviceResult &cb) {
    if (username.empty()) {
        setLastFailure(DeviceFailure::None);
        cb(true, "ONVIF on");
        return;
    }
    std::weak_ptr<HikvisionCamera> weak_self = std::static_pointer_cast<HikvisionCamera>(shared_from_this());
    request("GET", kOnvifUsersPath, "", "", [weak_self, username, password, cb](const VendorHttpAnswer &answer) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        vector<OnvifUser> users;
        if (answer.status != 200 || !parseOnvifUsers(answer.body, users)) {
            self->failPrepare(answer, false, "read the ONVIF users", cb);
            return;
        }
        string id;
        int max_id = 0;
        for (const auto &user : users) {
            max_id = std::max(max_id, atoi(user.id.c_str()));
            if (user.name == username) {
                id = user.id;
            }
        }
        // The password cannot be read back: an existing user gets it written again.
        const bool exists = !id.empty();
        if (!exists) {
            id = to_string(max_id + 1);
        }
        const string path = exists ? string(kOnvifUsersPath) + "/" + id : string(kOnvifUsersPath);
        self->request(exists ? "PUT" : "POST", path, onvifUserXml(id, username, password), kXml,
                      [weak_self, exists, cb](const VendorHttpAnswer &answer) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (!writeOk(answer)) {
                self->failPrepare(answer, false, exists ? "set the ONVIF user password" : "create the ONVIF user", cb);
                return;
            }
            self->setLastFailure(DeviceFailure::None);
            cb(true, exists ? "ONVIF user updated" : "ONVIF user created");
        });
    });
}

} // namespace managerkit
